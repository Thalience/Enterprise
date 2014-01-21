/*
 * Tool intended to help facilitate the process of booting Linux on Intel
 * Macintosh computers made by Apple from a USB stick or similar.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * Copyright (C) 2013 SevenBits
 *
 */

#include <efi.h>
#include <efilib.h>

#include "main.h"
#include "menu.h"
#include "utils.h"
#define banner L"Welcome to Enterprise! - Version %d.%d\n"
#define configpath L"\\efi\\boot\\.MLUL-Live-USB"
#define bootpath L"\\efi\\boot\\boot.efi"
#define isopath L"\\efi\\boot\\boot.iso"

static const EFI_GUID enterprise_variable_guid = {0x4a67b082, 0x0a4c, 0x41cf, {0xb6, 0xc7, 0x44, 0x0b, 0x29, 0xbb, 0x8c, 0x4f}};
static const EFI_GUID grub_variable_guid = {0x8BE4DF61, 0x93CA, 0x11d2, {0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B,0x8C}};

#define VERSION_MAJOR 0
#define VERSION_MINOR 1

static LinuxBootOption* ReadConfigurationFile(const CHAR16 *name);
static CHAR8* KernelLocationForDistributionName(CHAR8 *name, OUT CHAR8 **boot_folder);
static CHAR8* InitRDLocationForDistributionName(CHAR8 *name);
static EFI_STATUS console_text_mode(VOID);

static EFI_LOADED_IMAGE *this_image = NULL;
static EFI_FILE *root_dir;

static EFI_HANDLE global_image;

/* entry function for EFI */
EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *systab) {
	EFI_STATUS err; // Define an error variable.
	
	InitializeLib(image_handle, systab); // Initialize EFI.
	console_text_mode(); // Put the console into text mode. If we don't do that, the image of the Apple
						// boot manager will remain on the screen and the user won't see any output
						// from the program.
	global_image = image_handle;
	
	err = uefi_call_wrapper(BS->HandleProtocol, 3, image_handle, &LoadedImageProtocol, (void *)&this_image);
	if (EFI_ERROR(err)) {
		Print(L"Error: could not find loaded image: %d\n", err);
		uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
		return err;
	}
	
	root_dir = LibOpenRoot(this_image->DeviceHandle);
	if (!root_dir) {
		Print(L"Unable to open root directory.\n");
		uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
		return EFI_LOAD_ERROR;
	}
	
	uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_LIGHTGRAY|EFI_BACKGROUND_BLACK); // Set the text color.
	uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut); // Clear the screen.
	Print(banner, VERSION_MAJOR, VERSION_MINOR); // Print the welcome information.
	uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
	uefi_call_wrapper(ST->ConOut->EnableCursor, 2, ST->ConOut, FALSE); // Disable display of the cursor.
	
	BOOLEAN can_continue = TRUE;
	LinuxBootOption *result;
	
	// Check to make sure that we have our configuration file and GRUB bootloader.
	if (!FileExists(root_dir, configpath)) {
		DisplayErrorText(L"Error: can't find configuration file.\n");
	} else {
		result = ReadConfigurationFile(configpath);
		if (!result) {
			can_continue = FALSE;
		}
	}
	
	if (!FileExists(root_dir, bootpath)) {
		DisplayErrorText(L"Error: can't find GRUB bootloader!.\n");
		can_continue = FALSE;
	}
	
	if (!FileExists(root_dir, isopath)) {
		DisplayErrorText(L"Error: can't find ISO file to boot!.\n");
		can_continue = FALSE;
	}
	
	// Check if there is a persistence file present.
	// TODO: Support distributions other than Ubuntu.
	if (FileExists(root_dir, L"\\casper-rw") &&
		strcmpa((CHAR8 *)"Ubuntu", result->distro_family) == 0 &&
		can_continue) {
		DisplayColoredText(L"Found a persistence file! You can enable persistence by " \
							"selecting it in the Modify Boot Settings screen.\n");
	}
	
	// Display the menu where the user can select what they want to do.
	if (can_continue) {
		DisplayMenu();
	} else {
		Print(L"Cannot continue because core files are missing. Restarting...\n");
		uefi_call_wrapper(BS->Stall, 1, 1000 * 1000);
		return EFI_LOAD_ERROR;
	}
	
	return EFI_SUCCESS;
}

EFI_STATUS BootLinuxWithOptions(CHAR16 *params) {
	EFI_STATUS err;
	EFI_HANDLE image;
	EFI_DEVICE_PATH *path;
	
	CHAR8 *sized_str = UTF16toASCII(params, StrLen(params) + 1);
	efi_set_variable(&grub_variable_guid, L"Enterprise_LinuxBootOptions", sized_str,
		sizeof(sized_str[0]) * strlena(sized_str) + 1, FALSE);
	
	LinuxBootOption *boot_params = ReadConfigurationFile(configpath);
	if (!boot_params) {
		DisplayErrorText(L"Error: invalid distribution name specified.\n");
		return EFI_LOAD_ERROR;
	}
	
	CHAR8 *kernel_path = boot_params->kernel_path;
	CHAR8 *initrd_path = boot_params->initrd_path;
	CHAR8 *boot_folder = boot_params->boot_folder;
	efi_set_variable(&grub_variable_guid, L"Enterprise_LinuxKernelPath", kernel_path,
		sizeof(kernel_path[0]) * strlena(kernel_path) + 1, FALSE);
	efi_set_variable(&grub_variable_guid, L"Enterprise_InitRDPath", initrd_path,
		sizeof(initrd_path[0]) * strlena(initrd_path) + 1, FALSE);
	efi_set_variable(&grub_variable_guid, L"Enterprise_BootFolder", boot_folder,
		sizeof(boot_folder[0]) * strlena(boot_folder) + 1, FALSE);
		
	FreePool(boot_params); // Free the now-unneeded memory.
	
	// Load the EFI boot loader image into memory.
	path = FileDevicePath(this_image->DeviceHandle, bootpath);
	err = uefi_call_wrapper(BS->LoadImage, 6, FALSE, global_image, path, NULL, 0, &image);
	if (EFI_ERROR(err)) {
		DisplayErrorText(L"Error loading image: ");
		Print(L"%r\n", err);
		uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
		FreePool(path);
		
		return EFI_LOAD_ERROR;
	}
	
	// Start the EFI boot loader.
	uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut); // Clear the screen.
	err = uefi_call_wrapper(BS->StartImage, 3, image, NULL, NULL);
	if (EFI_ERROR(err)) {
		DisplayErrorText(L"Error starting image: ");
		Print(L"%r\n", err);
		uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
		FreePool(path);
		
		return EFI_LOAD_ERROR;
	}
	
	return EFI_SUCCESS;
}

static LinuxBootOption* ReadConfigurationFile(const CHAR16 *name) {
	LinuxBootOption *boot_options = AllocateZeroPool(sizeof(LinuxBootOption));
	
	CHAR8 *contents;
	UINTN read_bytes = FileRead(root_dir, name, &contents);
	if (read_bytes == 0) {
		DisplayErrorText(L"Error: Couldn't read configuration information.\n");
	}
	
	UINTN position = 0;
	CHAR8 *key, *value, *distribution, *boot_folder;
	while ((GetConfigurationKeyAndValue(contents, &position, &key, &value))) {
		/* 
		 * All that is needed is to specify the distribution that will be loaded.
		 * If it's supported, we'll have its info here.
		 * But you can also manually override the kernel and initrd paths by
		 * specifying them.
		 */
		// The user has given us a distribution family.
		if (strcmpa((CHAR8 *)"family", key) == 0) {
			distribution = value;
			boot_options->distro_family = value;
			boot_options->kernel_path = KernelLocationForDistributionName(distribution, &boot_folder);
			boot_options->initrd_path = InitRDLocationForDistributionName(distribution);
			boot_options->boot_folder = boot_folder;
			//Print(L"Boot folder: %s\n", ASCIItoUTF16(boot_folder, strlena(boot_folder)));
			// If either of the paths are a blank string, then you've got an
			// unsupported distribution or a typo of the distribution name.
			if (strcmpa((CHAR8 *)"", boot_options->kernel_path) == 0 ||
				strcmpa((CHAR8 *)"", boot_options->initrd_path) == 0) {
				Print(L"Distribution family %s is not supported.\n", ASCIItoUTF16(value, strlena(value)));
				
				FreePool(boot_options);
				return NULL;
			}
		// The user is manually specifying information; override any previous values.
		} else if (strcmpa((CHAR8 *)"kernel", key) == 0) {
			boot_options->kernel_path = value;
		} else if (strcmpa((CHAR8 *)"initrd", key) == 0) {
			boot_options->initrd_path = value;
		} else if (strcmpa((CHAR8 *)"root", key) == 0) { 
			boot_options->boot_folder = value;
		} else {
			Print(L"Unrecognized configuration option: %s\n", ASCIItoUTF16(key, strlena(key)));
		}
	}
	
	return boot_options;
}

static CHAR8* KernelLocationForDistributionName(CHAR8 *name, OUT CHAR8 **boot_folder) {
	if (strcmpa((CHAR8 *)"Debian", name) == 0) {
		*boot_folder = (CHAR8 *)"live";
		return (CHAR8 *)"/live/vmlinuz";
	} else if (strcmpa((CHAR8 *)"Ubuntu", name) == 0 || strcmpa((CHAR8 *)"Mint", name) == 0) {
		*boot_folder = (CHAR8 *)"casper";
		return (CHAR8 *)"/casper/vmlinuz";
	} else {
		return (CHAR8 *)"";
	}
}

static CHAR8* InitRDLocationForDistributionName(CHAR8 *name) {
	if (strcmpa((CHAR8 *)"Debian", name) == 0) {
		return (CHAR8 *)"/live/initrd.img";
	} else if (strcmpa((CHAR8 *)"Ubuntu", name) == 0 || strcmpa((CHAR8 *)"Mint", name) == 0) {
		return (CHAR8 *)"/casper/initrd.lz";
	} else {
		return (CHAR8 *)"";
	}
}

static EFI_STATUS console_text_mode(VOID) {
	#define EFI_CONSOLE_CONTROL_PROTOCOL_GUID \
		{ 0xf42f7782, 0x12e, 0x4c12, { 0x99, 0x56, 0x49, 0xf9, 0x43, 0x4, 0xf7, 0x21 } };

	struct _EFI_CONSOLE_CONTROL_PROTOCOL;

	typedef enum {
		EfiConsoleControlScreenText,
		EfiConsoleControlScreenGraphics,
		EfiConsoleControlScreenMaxValue,
	} EFI_CONSOLE_CONTROL_SCREEN_MODE;

	typedef EFI_STATUS (EFIAPI *EFI_CONSOLE_CONTROL_PROTOCOL_GET_MODE)(
		struct _EFI_CONSOLE_CONTROL_PROTOCOL *This,
		EFI_CONSOLE_CONTROL_SCREEN_MODE *Mode,
		BOOLEAN *UgaExists,
		BOOLEAN *StdInLocked
	);

	typedef EFI_STATUS (EFIAPI *EFI_CONSOLE_CONTROL_PROTOCOL_SET_MODE)(
		struct _EFI_CONSOLE_CONTROL_PROTOCOL *This,
		EFI_CONSOLE_CONTROL_SCREEN_MODE Mode
	);

	typedef EFI_STATUS (EFIAPI *EFI_CONSOLE_CONTROL_PROTOCOL_LOCK_STD_IN)(
		struct _EFI_CONSOLE_CONTROL_PROTOCOL *This,
		CHAR16 *Password
	);

	typedef struct _EFI_CONSOLE_CONTROL_PROTOCOL {
		EFI_CONSOLE_CONTROL_PROTOCOL_GET_MODE GetMode;
		EFI_CONSOLE_CONTROL_PROTOCOL_SET_MODE SetMode;
		EFI_CONSOLE_CONTROL_PROTOCOL_LOCK_STD_IN LockStdIn;
	} EFI_CONSOLE_CONTROL_PROTOCOL;

	EFI_GUID ConsoleControlProtocolGuid = EFI_CONSOLE_CONTROL_PROTOCOL_GUID;
	EFI_CONSOLE_CONTROL_PROTOCOL *ConsoleControl = NULL;
	EFI_STATUS err;

	err = LibLocateProtocol(&ConsoleControlProtocolGuid, (VOID **)&ConsoleControl);
	if (EFI_ERROR(err))
		return err;
	return uefi_call_wrapper(ConsoleControl->SetMode, 2, ConsoleControl, EfiConsoleControlScreenText);
}
