// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2020, Heinrich Schuchardt <xypron.glpk@gmx.de>
 *
 * dtbdump.efi saves the device tree provided as a configuration table
 * to a file.
 */

#include <common.h>
#include <efi_api.h>
#include <efi_dt_fixup.h>

#define BUFFER_SIZE 64
#define ESC 0x17

#define efi_size_in_pages(size) ((size + EFI_PAGE_MASK) >> EFI_PAGE_SHIFT)

static struct efi_simple_text_output_protocol *cerr;
static struct efi_simple_text_output_protocol *cout;
static struct efi_simple_text_input_protocol *cin;
static struct efi_boot_services *bs;
static const efi_guid_t fdt_guid = EFI_FDT_GUID;
static const efi_guid_t loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static const efi_guid_t guid_simple_file_system_protocol =
					EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static efi_handle_t handle;
static struct efi_system_table *systable;
static const efi_guid_t efi_dt_fixup_protocol_guid = EFI_DT_FIXUP_PROTOCOL_GUID;
static const efi_guid_t efi_file_info_guid = EFI_FILE_INFO_GUID;

/**
 * error() - print error string
 *
 * @string:	error text
 */
static void error(u16 *string)
{
	cout->set_attribute(cout, EFI_LIGHTRED | EFI_BACKGROUND_BLACK);
	cout->output_string(cout, string);
	cout->set_attribute(cout, EFI_LIGHTBLUE | EFI_BACKGROUND_BLACK);
}

/**
 * input() - read string from console
 *
 * @buffer:		input buffer
 * @buffer_size:	buffer size
 * Return:		status code
 */
static efi_status_t efi_input(u16 *buffer, efi_uintn_t buffer_size)
{
	struct efi_input_key key = {0};
	efi_uintn_t index;
	efi_uintn_t pos = 0;
	u16 outbuf[2] = L" ";
	efi_status_t ret;

	/* Drain the console input */
	ret = cin->reset(cin, true);
	*buffer = 0;
	for (;;) {
		ret = bs->wait_for_event(1, &cin->wait_for_key, &index);
		if (ret != EFI_SUCCESS)
			continue;
		ret = cin->read_key_stroke(cin, &key);
		if (ret != EFI_SUCCESS)
			continue;
		switch (key.scan_code) {
		case 0x17: /* Escape */
			cout->output_string(cout, L"\nAborted\n");
			return EFI_ABORTED;
		default:
			break;
		}
		switch (key.unicode_char) {
		case 0x08: /* Backspace */
			if (pos) {
				buffer[pos--] = 0;
				cout->output_string(cout, L"\b \b");
			}
			break;
		case 0x0a: /* Linefeed */
		case 0x0d: /* Carriage return */
			cout->output_string(cout, L"\n");
			return EFI_SUCCESS;
		default:
			break;
		}
		/* Ignore surrogate codes */
		if (key.unicode_char >= 0xD800 && key.unicode_char <= 0xDBFF)
			continue;
		if (key.unicode_char >= 0x20 &&
		    pos < buffer_size - 1) {
			*outbuf = key.unicode_char;
			buffer[pos++] = key.unicode_char;
			buffer[pos] = 0;
			cout->output_string(cout, outbuf);
		}
	}
}

/*
 * Convert FDT value to host endianness.
 *
 * @val		FDT value
 * @return	converted value
 */
static u32 f2h(fdt32_t val)
{
	char *buf = (char *)&val;
	char i;

	/* Swap the bytes */
	i = buf[0]; buf[0] = buf[3]; buf[3] = i;
	i = buf[1]; buf[1] = buf[2]; buf[2] = i;
	return *(u32 *)buf;
}

/**
 * get_dtb() - get device tree
 *
 * @systable:	system table
 * Return:	device tree or NULL
 */
void *get_dtb(struct efi_system_table *systable)
{
	void *dtb = NULL;
	efi_uintn_t i;

	for (i = 0; i < systable->nr_tables; ++i) {
		if (!memcmp(&systable->tables[i].guid, &fdt_guid,
			    sizeof(efi_guid_t))) {
			dtb = systable->tables[i].table;
			break;
		}
	}
	return dtb;
}

/**
 * skip_whitespace() - skip over leading whitespace
 *
 * @pos:	UTF-16 string
 * Return:	pointer to first non-whitespace
 */
u16 *skip_whitespace(u16 *pos)
{
	for (; *pos && *pos <= 0x20; ++pos)
		;
	return pos;
}

/**
 * starts_with() - check if @string starts with @keyword
 *
 * @string:	string to search for keyword
 * @keyword:	keyword to be searched
 * Return:	true fi @string starts with the keyword
 */
bool starts_with(u16 *string, u16 *keyword)
{
	for (; *keyword; ++string, ++keyword) {
		if (*string != *keyword)
			return false;
	}
	return true;
}

/**
 * do_help() - print help
 */
void do_help(void)
{
	error(L"load <dtb> - load device-tree from file\n");
	error(L"save <dtb> - save device-tree to file\n");
	error(L"exit       - exit the shell\n");
}

/**
 * do_load() - load and install device-tree
 *
 * @filename:	file name
 * Return:	status code
 */
efi_status_t do_load(u16 *filename)
{
	struct efi_dt_fixup_protocol *dt_fixup_prot;
	struct efi_loaded_image *loaded_image;
	struct efi_simple_file_system_protocol *file_system;
	struct efi_file_handle *root = NULL, *file = NULL;
	u64 addr = 0;
	struct efi_file_info *info;
	struct fdt_header *dtb;
	efi_uintn_t buffer_size;
	efi_uintn_t pages;
	efi_status_t ret, ret2;

	ret = bs->locate_protocol(&efi_dt_fixup_protocol_guid, NULL,
				  (void **)&dt_fixup_prot);
	if (ret != EFI_SUCCESS) {
		error(L"Device-tree fix-up protocol not found\n");
		return ret;
	}

	filename = skip_whitespace(filename);

	ret = bs->open_protocol(handle, &loaded_image_guid,
				(void **)&loaded_image, NULL, NULL,
				EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	if (ret != EFI_SUCCESS) {
		error(L"Loaded image protocol not found\n");
		return ret;
	}
	/* Open the simple file system protocol */
	ret = bs->open_protocol(loaded_image->device_handle,
				&guid_simple_file_system_protocol,
				(void **)&file_system, NULL, NULL,
				EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	if (ret != EFI_SUCCESS) {
		error(L"Failed to open simple file system protocol\n");
		goto out;
	}

	/* Open volume */
	ret = file_system->open_volume(file_system, &root);
	if (ret != EFI_SUCCESS) {
		error(L"Failed to open volume\n");
		goto out;
	}

	/* Open file */
	ret = root->open(root, &file, filename, EFI_FILE_MODE_READ, 0);
	if (ret != EFI_SUCCESS) {
		error(L"File not found\n");
		goto out;
	}
	/* Get file size */
	buffer_size = 0;
	ret = file->getinfo(file, &efi_file_info_guid, &buffer_size, NULL);
	if (ret != EFI_BUFFER_TOO_SMALL) {
		error(L"Can't get file info size\n");
		goto out;
	}
	ret = bs->allocate_pool(EFI_LOADER_DATA, buffer_size, (void **)&info);
	if (ret != EFI_SUCCESS) {
		error(L"Out of memory\n");
		goto out;
	}
	ret = file->getinfo(file, &efi_file_info_guid, &buffer_size, info);
	if (ret != EFI_SUCCESS) {
		error(L"Can't get file info\n");
		goto out;
	}
	buffer_size = info->file_size;
	pages = efi_size_in_pages(buffer_size);
	ret = bs->free_pool(info);
	if (ret != EFI_SUCCESS)
		error(L"Can't free memory pool\n");
	/* Read file */
	ret = bs->allocate_pages(EFI_ALLOCATE_ANY_PAGES,
				 EFI_ACPI_RECLAIM_MEMORY,
				 pages, &addr);
	if (ret != EFI_SUCCESS) {
		error(L"Out of memory\n");
		goto out;
	}
	dtb = (struct fdt_header *)(uintptr_t)addr;
	ret = file->read(file, &buffer_size, dtb);
	if (ret != EFI_SUCCESS) {
		error(L"Can't read file\n");
		goto out;
	}
	/* Fixup file, expecting EFI_BUFFER_TOO_SMALL */
	ret = dt_fixup_prot->fixup(dt_fixup_prot, dtb, &buffer_size,
				   EFI_DT_APPLY_FIXUPS | EFI_DT_RESERVE_MEMORY |
				   EFI_DT_INSTALL_TABLE);
	if (ret == EFI_BUFFER_TOO_SMALL) {
		/* Read file into larger buffer */
		ret = bs->free_pages(addr, pages);
		if (ret != EFI_SUCCESS)
			error(L"Can't free memory pages\n");
		pages = efi_size_in_pages(buffer_size);
		ret = bs->allocate_pages(EFI_ALLOCATE_ANY_PAGES,
					 EFI_ACPI_RECLAIM_MEMORY,
					 pages, &addr);
		if (ret != EFI_SUCCESS) {
			error(L"Out of memory\n");
			goto out;
		}
		dtb = (struct fdt_header *)(uintptr_t)addr;
		ret = file->setpos(file, 0);
		if (ret != EFI_SUCCESS) {
			error(L"Can't position file\n");
			goto out;
		}
		ret = file->read(file, &buffer_size, dtb);
		if (ret != EFI_SUCCESS) {
			error(L"Can't read file\n");
			goto out;
		}
		buffer_size = pages << EFI_PAGE_SHIFT;
		ret = dt_fixup_prot->fixup(
				dt_fixup_prot, dtb, &buffer_size,
				EFI_DT_APPLY_FIXUPS | EFI_DT_RESERVE_MEMORY |
				EFI_DT_INSTALL_TABLE);
	}
	if (ret == EFI_SUCCESS)
		cout->output_string(cout, L"device-tree installed\n");
	else
		error(L"Device-tree fix-up failed\n");
out:
	if (addr) {
		ret2 = bs->free_pages(addr, pages);
		if (ret2 != EFI_SUCCESS)
			error(L"Can't free memory pages\n");
	}
	if (file) {
		ret2 = file->close(file);
		if (ret2 != EFI_SUCCESS)
			error(L"Can't close file\n");
	}
	if (root) {
		ret2 = root->close(root);
		if (ret2 != EFI_SUCCESS)
			error(L"Can't close volume\n");
	}
	return ret;
}

/**
 * do_save() - save current device-tree
 *
 * @filename:	file name
 * Return:	status code
 */
efi_status_t do_save(u16 *filename)
{
	struct efi_loaded_image *loaded_image;
	struct efi_simple_file_system_protocol *file_system;
	efi_uintn_t dtb_size;
	struct efi_file_handle *root, *file;
	struct fdt_header *dtb;
	efi_uintn_t ret;

	dtb = get_dtb(systable);
	if (!dtb) {
		error(L"DTB not found\n");
		return EFI_NOT_FOUND;
	}
	if (f2h(dtb->magic) != FDT_MAGIC) {
		error(L"Wrong device tree magic\n");
		return EFI_NOT_FOUND;
	}
	dtb_size = f2h(dtb->totalsize);

	filename = skip_whitespace(filename);

	ret = bs->open_protocol(handle, &loaded_image_guid,
				(void **)&loaded_image, NULL, NULL,
				EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	if (ret != EFI_SUCCESS) {
		error(L"Loaded image protocol not found\n");
		return ret;
	}

	/* Open the simple file system protocol */
	ret = bs->open_protocol(loaded_image->device_handle,
				&guid_simple_file_system_protocol,
				(void **)&file_system, NULL, NULL,
				EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	if (ret != EFI_SUCCESS) {
		error(L"Failed to open simple file system protocol\n");
		return ret;
	}

	/* Open volume */
	ret = file_system->open_volume(file_system, &root);
	if (ret != EFI_SUCCESS) {
		error(L"Failed to open volume\n");
		return ret;
	}
	/* Create file */
	ret = root->open(root, &file, filename,
			 EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
			 EFI_FILE_MODE_CREATE, EFI_FILE_ARCHIVE);
	if (ret == EFI_SUCCESS) {
		/* Write file */
		ret = file->write(file, &dtb_size, dtb);
		if (ret != EFI_SUCCESS)
			error(L"Failed to write file\n");
		file->close(file);
	} else {
		error(L"Failed to open file\n");
	}
	root->close(root);

	if (ret == EFI_SUCCESS) {
		cout->output_string(cout, filename);
		cout->output_string(cout, L" written\n");
	}

	return ret;
}

/**
 * efi_main() - entry point of the EFI application.
 *
 * @handle:	handle of the loaded image
 * @systab:	system table
 * @return:	status code
 */
efi_status_t EFIAPI efi_main(efi_handle_t image_handle,
			     struct efi_system_table *systab)
{
	handle = image_handle;
	systable = systab;
	cerr = systable->std_err;
	cout = systable->con_out;
	cin = systable->con_in;
	bs = systable->boottime;

	cout->set_attribute(cout, EFI_LIGHTBLUE | EFI_BACKGROUND_BLACK);
	cout->clear_screen(cout);
	cout->set_attribute(cout, EFI_WHITE | EFI_BACKGROUND_BLACK);
	cout->output_string(cout, L"DTB Dump\n========\n\n");
	cout->set_attribute(cout, EFI_LIGHTBLUE | EFI_BACKGROUND_BLACK);

	for (;;) {
		u16 command[BUFFER_SIZE];
		u16 *pos;
		efi_uintn_t ret;

		cout->output_string(cout, L"=> ");
		ret = efi_input(command, sizeof(command));
		if (ret == EFI_ABORTED)
			break;
		pos = skip_whitespace(command);
		if (starts_with(pos, L"exit"))
			break;
		else if (starts_with(pos, L"load "))
			do_load(pos + 5);
		else if (starts_with(pos, L"save "))
			do_save(pos + 5);
		else
			do_help();
	}

	cout->set_attribute(cout, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
	cout->clear_screen(cout);
	return EFI_SUCCESS;
}