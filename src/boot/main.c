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

#include "shared/util.h"
#include "shared/graphics.h"
#include "shared/disk.h"
#include "shared/pefile.h"
#include "console.h"

enum {
        ENTRY_EDITOR            = 1ULL <<  0,
        ENTRY_AUTOSELECT        = 1ULL <<  1,
};

typedef struct {
        CHAR16 *release;
        CHAR16 *file_path;
        CHAR16 *options;
        CHAR16 *options_edit;
        CHAR16 key;
        EFI_HANDLE *device;
        EFI_STATUS (*call)(VOID);
        INTN boot_count;
        UINT64 flags;
} ConfigEntry;

typedef struct {
        ConfigEntry **entries;
        UINTN n_entries;
        INTN idx_default;
        EFI_LOADED_IMAGE *loaded_image;
} Config;

static VOID cursor_left(UINTN *cursor, UINTN *first) {
        if ((*cursor) > 0)
                (*cursor)--;
        else if ((*first) > 0)
                (*first)--;
}

static VOID cursor_right(UINTN *cursor, UINTN *first, UINTN x_max, UINTN len) {
        if ((*cursor)+1 < x_max)
                (*cursor)++;
        else if ((*first) + (*cursor) < len)
                (*first)++;
}

static BOOLEAN line_edit(CHAR16 *line_in, CHAR16 **line_out, UINTN x_max, UINTN y_pos) {
        _c_cleanup_(CFreePoolP) CHAR16 *line = NULL;
        _c_cleanup_(CFreePoolP) CHAR16 *print = NULL;
        UINTN size;
        UINTN len;
        UINTN first;
        UINTN cursor;
        UINTN clear;
        BOOLEAN exit;
        BOOLEAN enter;

        if (!line_in)
                line_in = L"";
        size = StrLen(line_in) + 1024;
        line = AllocatePool(size * sizeof(CHAR16));
        StrCpy(line, line_in);
        len = StrLen(line);
        print = AllocatePool((x_max+1) * sizeof(CHAR16));

        uefi_call_wrapper(ST->ConOut->EnableCursor, 2, ST->ConOut, TRUE);

        first = 0;
        cursor = 0;
        clear = 0;
        enter = FALSE;
        exit = FALSE;
        while (!exit) {
                UINT64 key;
                UINTN i;
                EFI_STATUS r;

                i = len - first;
                if (i >= x_max-1)
                        i = x_max-1;

                CopyMem(print, line + first, i * sizeof(CHAR16));

                while (clear > 0 && i < x_max-1) {
                        clear--;
                        print[i++] = ' ';
                }
                print[i] = '\0';

                uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, y_pos);
                uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, print);
                uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, cursor, y_pos);

                r = console_key_read(&key, TRUE);
                if (EFI_ERROR(r))
                        continue;

                switch (key) {
                case KEYPRESS(0, SCAN_ESC, 0):
                case KEYPRESS(EFI_CONTROL_PRESSED, 0, 'c'):
                case KEYPRESS(EFI_CONTROL_PRESSED, 0, 'g'):
                case KEYPRESS(EFI_CONTROL_PRESSED, 0, CHAR_CTRL('c')):
                case KEYPRESS(EFI_CONTROL_PRESSED, 0, CHAR_CTRL('g')):
                        exit = TRUE;
                        break;

                case KEYPRESS(0, SCAN_HOME, 0):
                case KEYPRESS(EFI_CONTROL_PRESSED, 0, 'a'):
                case KEYPRESS(EFI_CONTROL_PRESSED, 0, CHAR_CTRL('a')):
                        /* beginning-of-line */
                        cursor = 0;
                        first = 0;
                        continue;

                case KEYPRESS(0, SCAN_END, 0):
                case KEYPRESS(EFI_CONTROL_PRESSED, 0, 'e'):
                case KEYPRESS(EFI_CONTROL_PRESSED, 0, CHAR_CTRL('e')):
                        /* end-of-line */
                        cursor = len - first;
                        if (cursor+1 >= x_max) {
                                cursor = x_max-1;
                                first = len - (x_max-1);
                        }
                        continue;

                case KEYPRESS(0, SCAN_DOWN, 0):
                case KEYPRESS(EFI_ALT_PRESSED, 0, 'f'):
                case KEYPRESS(EFI_CONTROL_PRESSED, SCAN_RIGHT, 0):
                        /* forward-word */
                        while (line[first + cursor] && line[first + cursor] == ' ')
                                cursor_right(&cursor, &first, x_max, len);
                        while (line[first + cursor] && line[first + cursor] != ' ')
                                cursor_right(&cursor, &first, x_max, len);
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, cursor, y_pos);
                        continue;

                case KEYPRESS(0, SCAN_UP, 0):
                case KEYPRESS(EFI_ALT_PRESSED, 0, 'b'):
                case KEYPRESS(EFI_CONTROL_PRESSED, SCAN_LEFT, 0):
                        /* backward-word */
                        if ((first + cursor) > 0 && line[first + cursor-1] == ' ') {
                                cursor_left(&cursor, &first);
                                while ((first + cursor) > 0 && line[first + cursor] == ' ')
                                        cursor_left(&cursor, &first);
                        }
                        while ((first + cursor) > 0 && line[first + cursor-1] != ' ')
                                cursor_left(&cursor, &first);
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, cursor, y_pos);
                        continue;

                case KEYPRESS(0, SCAN_RIGHT, 0):
                case KEYPRESS(EFI_CONTROL_PRESSED, 0, 'f'):
                case KEYPRESS(EFI_CONTROL_PRESSED, 0, CHAR_CTRL('f')):
                        /* forward-char */
                        if (first + cursor == len)
                                continue;
                        cursor_right(&cursor, &first, x_max, len);
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, cursor, y_pos);
                        continue;

                case KEYPRESS(0, SCAN_LEFT, 0):
                case KEYPRESS(EFI_CONTROL_PRESSED, 0, 'b'):
                case KEYPRESS(EFI_CONTROL_PRESSED, 0, CHAR_CTRL('b')):
                        /* backward-char */
                        cursor_left(&cursor, &first);
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, cursor, y_pos);
                        continue;

                case KEYPRESS(EFI_ALT_PRESSED, 0, 'd'):
                        /* kill-word */
                        clear = 0;
                        for (i = first + cursor; i < len && line[i] == ' '; i++)
                                clear++;

                        for (; i < len && line[i] != ' '; i++)
                                clear++;

                        for (i = first + cursor; i + clear < len; i++)
                                line[i] = line[i + clear];
                        len -= clear;
                        line[len] = '\0';
                        continue;

                case KEYPRESS(EFI_CONTROL_PRESSED, 0, 'w'):
                case KEYPRESS(EFI_CONTROL_PRESSED, 0, CHAR_CTRL('w')):
                case KEYPRESS(EFI_ALT_PRESSED, 0, CHAR_BACKSPACE):
                        /* backward-kill-word */
                        clear = 0;
                        if ((first + cursor) > 0 && line[first + cursor-1] == ' ') {
                                cursor_left(&cursor, &first);
                                clear++;
                                while ((first + cursor) > 0 && line[first + cursor] == ' ') {
                                        cursor_left(&cursor, &first);
                                        clear++;
                                }
                        }
                        while ((first + cursor) > 0 && line[first + cursor-1] != ' ') {
                                cursor_left(&cursor, &first);
                                clear++;
                        }
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, cursor, y_pos);

                        for (i = first + cursor; i + clear < len; i++)
                                line[i] = line[i + clear];
                        len -= clear;
                        line[len] = '\0';
                        continue;

                case KEYPRESS(0, SCAN_DELETE, 0):
                case KEYPRESS(EFI_CONTROL_PRESSED, 0, 'd'):
                case KEYPRESS(EFI_CONTROL_PRESSED, 0, CHAR_CTRL('d')):
                        if (len == 0)
                                continue;
                        if (first + cursor == len)
                                continue;
                        for (i = first + cursor; i < len; i++)
                                line[i] = line[i+1];
                        clear = 1;
                        len--;
                        continue;

                case KEYPRESS(EFI_CONTROL_PRESSED, 0, 'k'):
                case KEYPRESS(EFI_CONTROL_PRESSED, 0, CHAR_CTRL('k')):
                        /* kill-line */
                        line[first + cursor] = '\0';
                        clear = len - (first + cursor);
                        len = first + cursor;
                        continue;

                case KEYPRESS(0, 0, CHAR_LINEFEED):
                case KEYPRESS(0, 0, CHAR_CARRIAGE_RETURN):
                        if (StrCmp(line, line_in) != 0) {
                                *line_out = line;
                                line = NULL;
                        }
                        enter = TRUE;
                        exit = TRUE;
                        break;

                case KEYPRESS(0, 0, CHAR_BACKSPACE):
                        if (len == 0)
                                continue;
                        if (first == 0 && cursor == 0)
                                continue;
                        for (i = first + cursor-1; i < len; i++)
                                line[i] = line[i+1];
                        clear = 1;
                        len--;
                        if (cursor > 0)
                                cursor--;
                        if (cursor > 0 || first == 0)
                                continue;
                        /* show full line if it fits */
                        if (len < x_max) {
                                cursor = first;
                                first = 0;
                                continue;
                        }
                        /* jump left to see what we delete */
                        if (first > 10) {
                                first -= 10;
                                cursor = 10;
                        } else {
                                cursor = first;
                                first = 0;
                        }
                        continue;

                case KEYPRESS(0, 0, ' ') ... KEYPRESS(0, 0, '~'):
                case KEYPRESS(0, 0, 0x80) ... KEYPRESS(0, 0, 0xffff):
                        if (len+1 == size)
                                continue;
                        for (i = len; i > first + cursor; i--)
                                line[i] = line[i-1];
                        line[first + cursor] = KEYCHAR(key);
                        len++;
                        line[len] = '\0';
                        if (cursor+1 < x_max)
                                cursor++;
                        else if (first + cursor < len)
                                first++;
                        continue;
                }
        }

        uefi_call_wrapper(ST->ConOut->EnableCursor, 2, ST->ConOut, FALSE);
        return enter;
}

static UINTN entry_lookup_key(Config *config, UINTN start, CHAR16 key) {
        if (key == 0)
                return -1;

        /* select entry by number key */
        if (key >= '1' && key <= '9') {
                UINTN i;

                i = key - '0';
                if (i > config->n_entries)
                        i = config->n_entries;

                return i - 1;
        }

        /* find matching key in config entries */
        for (UINTN i = start; i < config->n_entries; i++)
                if (config->entries[i]->key == key)
                        return i;

        for (UINTN i = 0; i < start; i++)
                if (config->entries[i]->key == key)
                        return i;

        return -1;
}

static VOID print_status(Config *config) {
        CHAR16 *s;
        CHAR16 uuid[37];
        UINT64 key;
        CHAR8 *b;
        UINTN x;
        UINTN y;
        UINTN size;

        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_LIGHTGRAY|EFI_BACKGROUND_BLACK);
        uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);

        Print(L"boot-efi version:       " VERSION "\n");
        Print(L"architecture:           " EFI_MACHINE_TYPE_NAME "\n");
        Print(L"UEFI specification:     %d.%02d\n", ST->Hdr.Revision >> 16, ST->Hdr.Revision & 0xffff);
        Print(L"firmware vendor:        %s\n", ST->FirmwareVendor);
        Print(L"firmware version:       %d.%02d\n", ST->FirmwareRevision >> 16, ST->FirmwareRevision & 0xffff);

        s = DevicePathToStr(config->loaded_image->FilePath);
        if (s) {
                Print(L"loaded image:           %s\n", s);
                FreePool(s);
        }

        if (disk_get_disk_uuid(config->loaded_image->DeviceHandle, uuid) == EFI_SUCCESS)
                Print(L"Disk UUID:              %s\n", uuid);

        if (uefi_call_wrapper(ST->ConOut->QueryMode, 4, ST->ConOut, ST->ConOut->Mode->Mode, &x, &y) == EFI_SUCCESS)
                Print(L"console size:           %d x %d\n", x, y);

        if (efivar_get(NULL, L"SecureBoot", &b, &size) == EFI_SUCCESS) {
                Print(L"SecureBoot:             %s\n", yes_no(*b > 0));
                FreePool(b);
        }

        if (efivar_get(NULL, L"SetupMode", &b, &size) == EFI_SUCCESS) {
                Print(L"SetupMode:              %s\n", *b > 0 ? L"setup" : L"user");
                FreePool(b);
        }

        if (efivar_get(NULL, L"OsIndicationsSupported", &b, &size) == EFI_SUCCESS) {
                Print(L"OsIndicationsSupported: %d\n", (UINT64)*b);
                FreePool(b);
        }
        Print(L"\n");

        Print(L"config entry count:     %d\n", config->n_entries);
        Print(L"entry selected idx:     %d\n", config->idx_default);
        Print(L"\n");

        Print(L"\n--- press key ---\n\n");
        console_key_read(&key, TRUE);

        for (UINTN i = 0; i < config->n_entries; i++) {
                ConfigEntry *entry;

                if (key == KEYPRESS(0, SCAN_ESC, 0) || key == KEYPRESS(0, 0, 'q'))
                        break;

                entry = config->entries[i];
                Print(L"config entry:           %d/%d\n", i+1, config->n_entries);
                Print(L"release                 '%s'\n", entry->release);
                if (entry->file_path)
                        Print(L"file path               '%s'\n", entry->file_path);
                if (entry->options)
                        Print(L"options                 '%s'\n", entry->options);
                if (entry->device) {
                        EFI_DEVICE_PATH *device_path;
                        device_path = DevicePathFromHandle(entry->device);
                        s = DevicePathToStr(device_path);
                        Print(L"device handle           '%s'\n", s);
                        FreePool(s);
                }
                if (entry->boot_count >= 0)
                        Print(L"boot count:             %d\n", entry->boot_count);
                Print(L"editor:                 %s\n", yes_no(entry->flags & ENTRY_EDITOR));
                Print(L"auto-select             %s\n", yes_no(entry->flags & ENTRY_AUTOSELECT));
                if (entry->call)
                        Print(L"internal call           yes\n");

                Print(L"\n--- press key ---\n\n");
                console_key_read(&key, TRUE);
        }

        uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
}

static BOOLEAN menu_run(Config *config, ConfigEntry **chosen_entry) {
        UINTN watchdog_timeout = 60;
        UINTN visible_max;
        UINTN idx_highlight;
        UINTN idx_highlight_prev;
        UINTN idx_first;
        UINTN idx_last;
        BOOLEAN refresh;
        BOOLEAN highlight;
        UINTN line_width;
        CHAR16 **lines;
        UINTN x_start;
        UINTN y_start;
        UINTN x_max;
        UINTN y_max;
        CHAR16 *status;
        CHAR16 *clearline;
        INT16 idx;
        BOOLEAN exit = FALSE;
        BOOLEAN run = TRUE;
        EFI_STATUS r;

        uefi_call_wrapper(BS->SetWatchdogTimer, 4, watchdog_timeout, 0x10000, 0, NULL);
        graphics_mode(FALSE);
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        uefi_call_wrapper(ST->ConOut->EnableCursor, 2, ST->ConOut, FALSE);
        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_LIGHTGRAY|EFI_BACKGROUND_BLACK);

        /* draw a single character to make ClearScreen work on some firmware */
        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, L" ");
        uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);

        r = uefi_call_wrapper(ST->ConOut->QueryMode, 4, ST->ConOut, ST->ConOut->Mode->Mode, &x_max, &y_max);
        if (EFI_ERROR(r)) {
                x_max = 80;
                y_max = 25;
        }

        idx_highlight = config->idx_default;
        idx_highlight_prev = 0;

        visible_max = y_max - 2;

        if ((UINTN)config->idx_default >= visible_max)
                idx_first = config->idx_default-1;
        else
                idx_first = 0;

        idx_last = idx_first + visible_max-1;

        refresh = TRUE;
        highlight = FALSE;

        /* length of the longest entry */
        line_width = 5;
        for (UINTN i = 0; i < config->n_entries; i++) {
                UINTN entry_len;

                entry_len = StrLen(config->entries[i]->release);
                if (line_width < entry_len)
                        line_width = entry_len;
        }
        if (line_width > x_max-6)
                line_width = x_max-6;

        /* offsets to center the entries on the screen */
        x_start = (x_max - (line_width)) / 2;
        if (config->n_entries < visible_max)
                y_start = ((visible_max - config->n_entries) / 2) + 1;
        else
                y_start = 0;

        /* menu entries title lines */
        lines = AllocatePool(sizeof(CHAR16 *) * config->n_entries);
        for (UINTN i = 0; i < config->n_entries; i++) {
                UINTN j, k;

                lines[i] = AllocatePool(((x_max+1) * sizeof(CHAR16)));

                for (j = 0; j < x_start; j++)
                        lines[i][j] = ' ';

                for (k = 0; config->entries[i]->release[k] != '\0' && j < x_max; j++, k++)
                        lines[i][j] = config->entries[i]->release[k];

                for (; j < x_max; j++)
                        lines[i][j] = ' ';

                lines[i][x_max] = '\0';
        }

        status = NULL;

        clearline = AllocatePool((x_max + 1) * sizeof(CHAR16));
        for (UINTN i = 0; i < x_max; i++)
                clearline[i] = ' ';
        clearline[x_max] = 0;

        while (!exit) {
                UINT64 key;

                if (refresh) {
                        for (UINTN i = 0; i < config->n_entries; i++) {
                                if (i < idx_first || i > idx_last)
                                        continue;
                                uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, y_start + i - idx_first);
                                if (i == idx_highlight)
                                        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut,
                                                          EFI_BLACK|EFI_BACKGROUND_LIGHTGRAY);
                                else
                                        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut,
                                                          EFI_LIGHTGRAY|EFI_BACKGROUND_BLACK);
                                uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, lines[i]);
                        }
                        refresh = FALSE;
                } else if (highlight) {
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, y_start + idx_highlight_prev - idx_first);
                        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_LIGHTGRAY|EFI_BACKGROUND_BLACK);
                        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, lines[idx_highlight_prev]);

                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, y_start + idx_highlight - idx_first);
                        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_BLACK|EFI_BACKGROUND_LIGHTGRAY);
                        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, lines[idx_highlight]);
                        highlight = FALSE;
                }

                /* print status at last line of screen */
                if (status) {
                        UINTN len;
                        UINTN x;

                        /* center line */
                        len = StrLen(status);
                        if (len < x_max)
                                x = (x_max - len) / 2;
                        else
                                x = 0;
                        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_LIGHTGRAY|EFI_BACKGROUND_BLACK);
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, y_max-1);
                        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, clearline + (x_max - x));
                        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, status);
                        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, clearline+1 + x + len);
                }

                r = console_key_read(&key, TRUE);

                /* Disable watchdog on activity. */
                if (watchdog_timeout > 0) {
                        uefi_call_wrapper(BS->SetWatchdogTimer, 4, 0, 0x10000, 0, NULL);
                        watchdog_timeout = 0;
                }

                /* clear status after keystroke */
                if (status) {
                        FreePool(status);
                        status = NULL;
                        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_LIGHTGRAY|EFI_BACKGROUND_BLACK);
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, y_max-1);
                        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, clearline+1);
                }

                idx_highlight_prev = idx_highlight;

                switch (key) {
                case KEYPRESS(0, SCAN_UP, 0):
                case KEYPRESS(0, 0, 'k'):
                        if (idx_highlight > 0)
                                idx_highlight--;
                        break;

                case KEYPRESS(0, SCAN_DOWN, 0):
                case KEYPRESS(0, 0, 'j'):
                        if (idx_highlight < config->n_entries-1)
                                idx_highlight++;
                        break;

                case KEYPRESS(0, SCAN_HOME, 0):
                case KEYPRESS(EFI_ALT_PRESSED, 0, '<'):
                        if (idx_highlight > 0) {
                                refresh = TRUE;
                                idx_highlight = 0;
                        }
                        break;

                case KEYPRESS(0, SCAN_END, 0):
                case KEYPRESS(EFI_ALT_PRESSED, 0, '>'):
                        if (idx_highlight < config->n_entries-1) {
                                refresh = TRUE;
                                idx_highlight = config->n_entries-1;
                        }
                        break;

                case KEYPRESS(0, SCAN_PAGE_UP, 0):
                        if (idx_highlight > visible_max)
                                idx_highlight -= visible_max;
                        else
                                idx_highlight = 0;
                        break;

                case KEYPRESS(0, SCAN_PAGE_DOWN, 0):
                        idx_highlight += visible_max;
                        if (idx_highlight > config->n_entries-1)
                                idx_highlight = config->n_entries-1;
                        break;

                case KEYPRESS(0, 0, CHAR_LINEFEED):
                case KEYPRESS(0, 0, CHAR_CARRIAGE_RETURN):
                        exit = TRUE;
                        break;

                case KEYPRESS(0, SCAN_F1, 0):
                case KEYPRESS(0, 0, 'h'):
                case KEYPRESS(0, 0, '?'):
                        status = StrDuplicate(L"(e)dit, (v)ersion (Q)uit (P)rint (h)elp");
                        break;

                case KEYPRESS(0, 0, 'Q'):
                        exit = TRUE;
                        run = FALSE;
                        break;

                case KEYPRESS(0, 0, 'e'):
                        if (!config->entries[idx_highlight]->flags & ENTRY_EDITOR)
                                break;
                        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_LIGHTGRAY|EFI_BACKGROUND_BLACK);
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, y_max-1);
                        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, clearline+1);
                        if (line_edit(config->entries[idx_highlight]->options, &config->entries[idx_highlight]->options_edit, x_max-1, y_max-1))
                                exit = TRUE;
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, y_max-1);
                        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, clearline+1);
                        break;

                case KEYPRESS(0, 0, 'v'):
                        status = PoolPrint(L"boot-efi " VERSION " (" EFI_MACHINE_TYPE_NAME "), UEFI Specification %d.%02d, Vendor %s %d.%02d",
                                           ST->Hdr.Revision >> 16, ST->Hdr.Revision & 0xffff,
                                           ST->FirmwareVendor, ST->FirmwareRevision >> 16, ST->FirmwareRevision & 0xffff);
                        break;

                case KEYPRESS(0, 0, 'P'):
                        print_status(config);
                        refresh = TRUE;
                        break;

                case KEYPRESS(EFI_CONTROL_PRESSED, 0, 'l'):
                case KEYPRESS(EFI_CONTROL_PRESSED, 0, CHAR_CTRL('l')):
                        refresh = TRUE;
                        break;

                default:
                        /* jump with a hotkey directly to a matching entry */
                        idx = entry_lookup_key(config, idx_highlight+1, KEYCHAR(key));
                        if (idx < 0)
                                break;
                        idx_highlight = idx;
                        refresh = TRUE;
                }

                if (idx_highlight > idx_last) {
                        idx_last = idx_highlight;
                        idx_first = 1 + idx_highlight - visible_max;
                        refresh = TRUE;
                } else if (idx_highlight < idx_first) {
                        idx_first = idx_highlight;
                        idx_last = idx_highlight + visible_max-1;
                        refresh = TRUE;
                }

                if (!refresh && idx_highlight != idx_highlight_prev)
                        highlight = TRUE;
        }

        *chosen_entry = config->entries[idx_highlight];

        for (UINTN i = 0; i < config->n_entries; i++)
                FreePool(lines[i]);
        FreePool(lines);
        FreePool(clearline);

        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_WHITE|EFI_BACKGROUND_BLACK);
        uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
        return run;
}

static VOID config_add_entry(Config *config, ConfigEntry *entry) {
        if ((config->n_entries & 15) == 0) {
                UINTN i;

                i = config->n_entries + 16;
                if (config->n_entries == 0)
                        config->entries = AllocatePool(sizeof(VOID *) * i);
                else
                        config->entries = ReallocatePool(config->entries,
                                                         sizeof(VOID *) * config->n_entries, sizeof(VOID *) * i);
        }
        config->entries[config->n_entries++] = entry;
}

static VOID config_entry_free(ConfigEntry *entry) {
        FreePool(entry->release);
        FreePool(entry->options);
}

static BOOLEAN is_digit(CHAR16 c) {
        return (c >= '0') && (c <= '9');
}

static UINTN c_order(CHAR16 c) {
        if (c == '\0')
                return 0;
        if (is_digit(c))
                return 0;
        else if ((c >= 'a') && (c <= 'z'))
                return c;
        else
                return c + 0x10000;
}

static INTN str_verscmp(CHAR16 *s1, CHAR16 *s2) {
        CHAR16 *os1 = s1;
        CHAR16 *os2 = s2;

        while (*s1 || *s2) {
                INTN first;

                while ((*s1 && !is_digit(*s1)) || (*s2 && !is_digit(*s2))) {
                        INTN order;

                        order = c_order(*s1) - c_order(*s2);
                        if (order)
                                return order;
                        s1++;
                        s2++;
                }

                while (*s1 == '0')
                        s1++;
                while (*s2 == '0')
                        s2++;

                first = 0;
                while (is_digit(*s1) && is_digit(*s2)) {
                        if (first == 0)
                                first = *s1 - *s2;
                        s1++;
                        s2++;
                }

                if (is_digit(*s1))
                        return 1;
                if (is_digit(*s2))
                        return -1;

                if (first)
                        return first;
        }

        return StrCmp(os1, os2);
}

static VOID config_sort_entries(Config *config) {
        for (UINTN i = 1; i < config->n_entries; i++) {
                BOOLEAN more;

                more = FALSE;
                for (UINTN k = 0; k < config->n_entries - i; k++) {
                        ConfigEntry *entry;

                        if (str_verscmp(config->entries[k]->file_path, config->entries[k+1]->file_path) <= 0)
                                continue;

                        entry = config->entries[k];
                        config->entries[k] = config->entries[k+1];
                        config->entries[k+1] = entry;
                        more = TRUE;
                }

                if (!more)
                        break;
        }
}

static VOID config_default_entry_select(Config *config) {
        UINTN i;
        INTN idx_default_fallback = -1;

        if (config->n_entries == 0)
                return;

        i = config->n_entries;
        while (i--) {
                if (!(config->entries[i]->flags & ENTRY_AUTOSELECT))
                        continue;

                /* Remember the first "-boot0" entry, in case we don't find a better one. */
                if (config->entries[i]->boot_count == 0) {
                        if (idx_default_fallback < 0)
                                idx_default_fallback = i;

                        continue;
                }

                config->idx_default = i;
                return;
        }

        if (idx_default_fallback >= 0)
                config->idx_default = idx_default_fallback;
}

static BOOLEAN config_entry_add_call(Config *config, CHAR16 *release, EFI_STATUS (*call)(VOID)) {
        ConfigEntry *entry;

        entry = AllocateZeroPool(sizeof(ConfigEntry));
        entry->boot_count = -1;
        entry->release = StrDuplicate(release);
        entry->call = call;
        config_add_entry(config, entry);

        return TRUE;
}

static EFI_STATUS config_entry_add_file(Config *config, EFI_HANDLE *device, EFI_FILE_HANDLE root_dir,
                                        CHAR16 *release, CHAR16 key, CHAR16 *file_path, CHAR16 *options,
                                        INTN boot_count, UINT64 flags) {
        ConfigEntry *entry;
        _c_cleanup_(CCloseP) EFI_FILE_HANDLE handle = NULL;
        EFI_FILE_INFO *info;
        UINTN size = 0;
        EFI_STATUS r;

        /* check existence */
        r = uefi_call_wrapper(root_dir->Open, 5, root_dir, &handle, file_path, EFI_FILE_MODE_READ, 0ULL);
        if (EFI_ERROR(r))
                return r;

        info = LibFileInfo(handle);
        size = info->FileSize;
        FreePool(info);

        if (size == 0)
                return EFI_LOAD_ERROR;

        entry = AllocateZeroPool(sizeof(ConfigEntry));
        if (release)
                entry->release = StrDuplicate(release);
        entry->key = key;
        entry->file_path = StrDuplicate(file_path);
        if (options)
                entry->options = StrDuplicate(options);
        entry->boot_count = boot_count;
        entry->flags = flags;
        entry->device = device;
        config_add_entry(config, entry);

        return EFI_SUCCESS;
}

static VOID config_entry_add_osx(Config *config) {
        EFI_STATUS r;
        UINTN handle_count = 0;
        EFI_HANDLE *handles = NULL;

        r = LibLocateHandle(ByProtocol, &FileSystemProtocol, NULL, &handle_count, &handles);
        if (!EFI_ERROR(r)) {
                for (UINTN i = 0; i < handle_count; i++) {
                        _c_cleanup_(CCloseP) EFI_FILE_HANDLE root = NULL;

                        root = LibOpenRoot(handles[i]);
                        if (!root)
                                continue;

                        r = config_entry_add_file(config, handles[i], root, L"osx", 'a',
                                                  L"\\System\\Library\\CoreServices\\boot.efi", NULL, -1, 0);
                        if (!EFI_ERROR(r))
                                break;
                }

                FreePool(handles);
        }
}

static EFI_STATUS config_entry_add_linux( Config *config, EFI_FILE_HANDLE root_dir) {
        _c_cleanup_(CCloseP) EFI_FILE_HANDLE bus1_dir = NULL;
        EFI_STATUS r;

        r = uefi_call_wrapper(root_dir->Open, 5, root_dir, &bus1_dir, L"\\EFI\\org.bus1", EFI_FILE_MODE_READ, 0ULL);
        if (EFI_ERROR(r))
                return r;

        for (;;) {
                _c_cleanup_(CCloseP) EFI_FILE_HANDLE f = NULL;
                struct {
                        EFI_FILE_INFO info;
                        CHAR16 buf[256];
                } file_info;
                UINTN file_info_size;
                enum {
                        SECTION_RELEASE,
                        SECTION_OPTIONS,
                };
                CHAR8 *sections[] = {
                        [SECTION_RELEASE] = (UINT8 *)".release",
                        [SECTION_OPTIONS] = (UINT8 *)".options",
                };
                UINTN offs[C_ARRAY_SIZE(sections)] = {};
                UINTN szs[C_ARRAY_SIZE(sections)] = {};
                UINTN addrs[C_ARRAY_SIZE(sections)] = {};
                _c_cleanup_(CFreePoolP) CHAR16 *release = NULL;
                _c_cleanup_(CFreePoolP) CHAR16 *options = NULL;
                _c_cleanup_(CFreePoolP) CHAR16 *file = NULL;
                INTN boot_count;
                INTN n;

                file_info_size = sizeof(file_info);
                r = uefi_call_wrapper(bus1_dir->Read, 3, bus1_dir, &file_info_size, &file_info);
                if (file_info_size == 0 || EFI_ERROR(r))
                        break;

                if (file_info.info.FileName[0] == '.')
                        continue;
                if (file_info.info.Attribute & EFI_FILE_DIRECTORY)
                        continue;

                /* look for .release and .options sections in the .efi binary */
                r = uefi_call_wrapper(bus1_dir->Open, 5, bus1_dir, &f, file_info.info.FileName, EFI_FILE_MODE_READ, 0ULL);
                if (EFI_ERROR(r))
                        continue;

                r = pefile_locate_sections(f, sections, C_ARRAY_SIZE(sections), addrs, offs, szs);
                if (EFI_ERROR(r))
                        continue;

                if (szs[SECTION_RELEASE] == 0)
                        continue;

                n = file_read_str(bus1_dir, file_info.info.FileName, offs[SECTION_RELEASE], szs[SECTION_RELEASE], &release);
                if (n <= 0)
                        continue;

                if (loader_filename_parse(f, release, n, &boot_count) != EFI_SUCCESS)
                        continue;

                if (szs[SECTION_OPTIONS] > 0)
                        file_read_str(bus1_dir, file_info.info.FileName, offs[SECTION_OPTIONS], szs[SECTION_OPTIONS], &options);

                file = PoolPrint(L"\\EFI\\org.bus1\\%s", file_info.info.FileName);
                config_entry_add_file(config, config->loaded_image->DeviceHandle, root_dir,
                                      release, 'l', file, options,
                                      boot_count, ENTRY_EDITOR|ENTRY_AUTOSELECT);
        }

        return EFI_SUCCESS;
}

static EFI_STATUS image_set_boot_count(EFI_FILE_HANDLE root_dir, ConfigEntry *entry, UINTN count) {
        static EFI_GUID EfiFileInfoGuid = EFI_FILE_INFO_ID;
        struct {
                EFI_FILE_INFO info;
                CHAR16 buf[256];
        } file_info;
        UINTN file_info_size = sizeof(file_info);
        _c_cleanup_(CCloseP) EFI_FILE_HANDLE file = NULL;
        CHAR16 *file_path;
        EFI_STATUS r;

        r = uefi_call_wrapper(root_dir->Open, 5, root_dir, &file, entry->file_path, EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE, 0ULL);
        if (EFI_ERROR(r))
                return r;

        r = uefi_call_wrapper(file->GetInfo, 4, file, &EfiFileInfoGuid, &file_info_size, &file_info);
        if (EFI_ERROR(r))
                return r;

        /* Rename the loader file to reflect the new boot count. */
        SPrint(file_info.info.FileName, sizeof(file_info.buf), L"%s-boot%d.efi", entry->release, count);
        r = uefi_call_wrapper(file->SetInfo, 4, file, &EfiFileInfoGuid, file_info_size, &file_info);
        if (EFI_ERROR(r))
                return r;

        /* Update the stored loader path in the entry. */
        file_path = PoolPrint(L"\\EFI\\org.bus1\\%s-boot%d.efi", entry->release, count);
        if (!file_path)
                return EFI_OUT_OF_RESOURCES;

        FreePool(entry->file_path);
        entry->file_path = file_path;

        return 0;
}

static EFI_STATUS image_start(EFI_FILE_HANDLE root_dir, EFI_HANDLE parent_image, ConfigEntry *entry) {
        _c_cleanup_(CFreePoolP) EFI_DEVICE_PATH *path = NULL;
        EFI_HANDLE image;
        EFI_STATUS r;

        if (entry->boot_count > 0) {
                r = image_set_boot_count(root_dir, entry, entry->boot_count - 1);
                if (EFI_ERROR(r)) {
                        Print(L"Error updating boot count of %s: %r", entry->file_path, r);
                        uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
                        goto finish;
                }

        }

        path = FileDevicePath(entry->device, entry->file_path);
        if (!path) {
                Print(L"Error getting device path for %s", entry->file_path);
                uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
                return EFI_INVALID_PARAMETER;
        }

        r = uefi_call_wrapper(BS->LoadImage, 6, FALSE, parent_image, path, NULL, 0, &image);
        if (EFI_ERROR(r)) {
                Print(L"Error loading %s: %r", entry->file_path, r);
                uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
                return r;
        }

        if (entry->options_edit) {
                EFI_LOADED_IMAGE *loaded_image;

                r = uefi_call_wrapper(BS->OpenProtocol, 6, image, &LoadedImageProtocol, (VOID **)&loaded_image,
                                        parent_image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
                if (EFI_ERROR(r)) {
                        Print(L"Error getting LoadedImageProtocol handle: %r", entry->file_path, r);
                        uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
                        goto finish;
                }

                loaded_image->LoadOptions = entry->options_edit;
                loaded_image->LoadOptionsSize = (StrLen(loaded_image->LoadOptions)+1) * sizeof(CHAR16);
        }

        r = uefi_call_wrapper(BS->StartImage, 3, image, NULL, NULL);

finish:
        uefi_call_wrapper(BS->UnloadImage, 1, image);

        return r;
}

static EFI_STATUS reboot_into_firmware(VOID) {
        CHAR8 *b;
        UINTN size;
        UINT64 osind;
        EFI_STATUS r;

        osind = EFI_OS_INDICATIONS_BOOT_TO_FW_UI;

        r = efivar_get(NULL, L"OsIndications", &b, &size);
        if (!EFI_ERROR(r))
                osind |= (UINT64)*b;
        FreePool(b);

        r = efivar_set(NULL, L"OsIndications", (CHAR8 *)&osind, sizeof(UINT64), TRUE);
        if (EFI_ERROR(r))
                return r;

        r = uefi_call_wrapper(RT->ResetSystem, 4, EfiResetCold, EFI_SUCCESS, 0, NULL);
        Print(L"Error calling ResetSystem: %r", r);
        uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
        return r;
}

static VOID config_free(Config *config) {
        for (UINTN i = 0; i < config->n_entries; i++)
                config_entry_free(config->entries[i]);
        FreePool(config->entries);
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *sys_table) {
        CHAR8 *b;
        UINTN size;
        _c_cleanup_(CCloseP) EFI_FILE_HANDLE root_dir = NULL;
        Config config = {
                .idx_default = -1,
        };
        BOOLEAN menu = FALSE;
        UINT64 key;
        EFI_STATUS r;

        InitializeLib(image, sys_table);
        r = uefi_call_wrapper(BS->OpenProtocol, 6, image, &LoadedImageProtocol, (VOID **)&config.loaded_image,
                              image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(r)) {
                Print(L"Error getting a LoadedImageProtocol handle: %r ", r);
                uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
                return r;
        }

        root_dir = LibOpenRoot(config.loaded_image->DeviceHandle);
        if (!root_dir) {
                Print(L"Unable to open root directory: %r ", r);
                uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
                return EFI_LOAD_ERROR;
        }

        /* scan /EFI/org.bus1/ directory */
        config_entry_add_linux(&config, root_dir);

        /* sort entries by release string */
        config_sort_entries(&config);

        /* check for some well-known files, add them to the end of the list */
        config_entry_add_file(&config, config.loaded_image->DeviceHandle, root_dir,
                              L"windows", 'w', L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi", NULL,
                              -1, 0);
        config_entry_add_file(&config, config.loaded_image->DeviceHandle, root_dir,
                              L"shell", 's', L"\\shell" EFI_MACHINE_TYPE_NAME ".efi", NULL,
                              -1, 0);
        config_entry_add_osx(&config);

        if (efivar_get(NULL, L"OsIndicationsSupported", &b, &size) == EFI_SUCCESS) {
                UINT64 osind = (UINT64)*b;

                if (osind & EFI_OS_INDICATIONS_BOOT_TO_FW_UI)
                        config_entry_add_call(&config, L"firmware", reboot_into_firmware);
                FreePool(b);
        }

        if (config.n_entries == 0) {
                Print(L"No loader found on the system. Exiting.");
                uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
                goto finish;
        }

        config_default_entry_select(&config);

        r = console_key_read(&key, FALSE);
        if (!EFI_ERROR(r)) {
                INT16 idx;

                /* find matching key in config entries */
                idx = entry_lookup_key(&config, config.idx_default, KEYCHAR(key));
                if (idx >= 0)
                        config.idx_default = idx;
                else
                        menu = TRUE;
        }

        if (config.idx_default == -1) {
                config.idx_default = 0;
                menu = TRUE;
        }

        for (;;) {
                ConfigEntry *entry;

                entry = config.entries[config.idx_default];
                if (menu) {
                        if (!menu_run(&config, &entry))
                                break;

                        /* run special entry like "reboot" */
                        if (entry->call) {
                                entry->call();
                                continue;
                        }
                }

                uefi_call_wrapper(BS->SetWatchdogTimer, 4, 60, 0x10000, 0, NULL);
                r = image_start(root_dir, image, entry);
                if (EFI_ERROR(r)) {
                        graphics_mode(FALSE);
                        Print(L"\nFailed to execute %s (%s): %r\n", entry->release, entry->file_path, r);
                        uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
                        goto finish;
                }

                menu = TRUE;
        }

        r = EFI_SUCCESS;

finish:
        uefi_call_wrapper(BS->CloseProtocol, 4, image, &LoadedImageProtocol, image, NULL);
        config_free(&config);

        return r;
}
