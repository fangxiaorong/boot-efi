#pragma once
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

#define C_ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define _c_cleanup_(_x) __attribute__((__cleanup__(_x)))

#define C_DEFINE_CLEANUP(_type, _func)                  \
        static inline void _func ## p(_type *p) {       \
                if (*p)                                 \
                        _func(*p);                      \
} struct c_internal_trailing_semicolon

static inline VOID CFreePoolP(VOID *p) {
        FreePool(*(VOID **)p);
}

static inline VOID CCloseP(EFI_FILE_HANDLE *handle) {
        if (*handle)
                uefi_call_wrapper((*handle)->Close, 1, *handle);
}

static inline const CHAR16 *yes_no(BOOLEAN b) {
        return b ? L"yes" : L"no";
}

EFI_STATUS efivar_set(const EFI_GUID *vendor, CHAR16 *name, CHAR8 *buf, UINTN size, BOOLEAN persistent);
EFI_STATUS efivar_get(const EFI_GUID *vendor, CHAR16 *name, CHAR8 **buffer, UINTN *size);

INTN StrniCmp(const CHAR16 *s1, const CHAR16 *s2, UINTN n);

EFI_STATUS loader_filename_parse(EFI_FILE_HANDLE f, const CHAR16 *release, UINTN release_len, INTN *boot_countp);
INTN file_read_str(EFI_FILE_HANDLE dir, CHAR16 *name, UINTN off, UINTN size, CHAR16 **str);
