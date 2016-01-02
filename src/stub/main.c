/***
  This file is part of bus1. See COPYING for details.

  bus1 is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  bus1 is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with bus1; If not, see <http://www.gnu.org/licenses/>.
***/

#include <efi.h>
#include <efilib.h>

#include "util.h"
#include "disk.h"
#include "pefile.h"
#include "graphics.h"
#include "splash.h"
#include "linux.h"

static const EFI_GUID global_guid = EFI_GLOBAL_VARIABLE;

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *sys_table) {
        EFI_LOADED_IMAGE *loaded_image;
        EFI_FILE *root_dir;
        CHAR16 *loaded_image_path;
        CHAR16 uuid[37] = {};
        CHAR8 *b;
        UINTN size;
        BOOLEAN secure = FALSE;
        CHAR8 *sections[] = {
                (UINT8 *)".options",
                (UINT8 *)".linux",
                (UINT8 *)".initrd",
                (UINT8 *)".splash",
                NULL
        };
        UINTN addrs[C_ARRAY_SIZE(sections)-1] = {};
        UINTN offs[C_ARRAY_SIZE(sections)-1] = {};
        UINTN szs[C_ARRAY_SIZE(sections)-1] = {};
        CHAR16 *options = NULL;
        UINTN options_len = 0;
        UINTN i;
        CHAR8 *cmdline;
        UINTN cmdline_len;
        EFI_STATUS err;

        InitializeLib(image, sys_table);

        err = uefi_call_wrapper(BS->OpenProtocol, 6, image, &LoadedImageProtocol, (VOID **)&loaded_image,
                                image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(err))
                return err;

        root_dir = LibOpenRoot(loaded_image->DeviceHandle);
        if (!root_dir)
                return EFI_LOAD_ERROR;

        if (efivar_get(&global_guid, L"SecureBoot", &b, &size) == EFI_SUCCESS) {
                if (*b > 0)
                        secure = TRUE;
                FreePool(b);
        }

        loaded_image_path = DevicePathToStr(loaded_image->FilePath);
        if (!loaded_image_path)
                return EFI_LOAD_ERROR;

        err = pefile_locate_sections(root_dir, loaded_image_path, sections, addrs, offs, szs);
        if (EFI_ERROR(err)) {
                Print(L"Unable to locate embedded .linux section: %r\n", err);
                uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
                return err;
        }

        FreePool(loaded_image_path);

        if (secure && loaded_image->LoadOptionsSize > 0) {
                Print(L"Secure Boot active, ignoring custom kernel command line.\n");
                uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
        }

        if (!secure && loaded_image->LoadOptionsSize > 0) {
                options = (CHAR16 *)loaded_image->LoadOptions;
                options_len = (loaded_image->LoadOptionsSize / sizeof(CHAR16));
        } else if (szs[0] > 0) {
                options = (CHAR16 *)(loaded_image->ImageBase + addrs[0]);
                options_len = szs[0] / sizeof(CHAR16);
        }

        err = disk_get_disk_uuid(loaded_image->DeviceHandle, uuid);
        if (EFI_ERROR(err))
                return err;

        cmdline_len = StrLen(L"disk=f7e35557-c7b0-4788-a083-5eb131da85c5");
        cmdline = AllocatePool(cmdline_len + 1 + options_len);
        CopyMem(cmdline, "disk=", 5);
        for (i = 0; i < cmdline_len - 5 ; i++) {
                /* some firmwares do not implement StrLwr() */
                if (uuid[i] >= 'A' && uuid[i] <= 'Z')
                        uuid[i] |= 0x20;
                cmdline[5 + i] = uuid[i];
        }

        if (options_len) {
                cmdline[cmdline_len++] = ' ';
                for (i = 0; i < options_len; i++)
                        cmdline[cmdline_len++] = options[i];
        }

        if (szs[3] > 0)
                graphics_splash((UINT8 *)((UINTN)loaded_image->ImageBase + addrs[3]), szs[3], NULL);

        err = linux_exec(image, cmdline, cmdline_len,
                         (UINTN)loaded_image->ImageBase + addrs[1],
                         (UINTN)loaded_image->ImageBase + addrs[2], szs[2]);

        graphics_mode(FALSE);
        Print(L"Execution of embedded linux image failed: %r\n", err);
        uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
        return err;
}