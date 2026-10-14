// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Windows.h>

inline bool IsReadablePage(DWORD state, DWORD protect) {
    // Write-combined pages with read access use the ordinary sampling cadence.
    if (state != MEM_COMMIT || (protect & (PAGE_GUARD | PAGE_NOCACHE))) {
        return false;
    }
    switch (protect & 0xFF) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}
