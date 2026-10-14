// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2019 SNMetamorph; modified for the standalone MemGlow viewer.
#include "memglow/full_memory_view.h"
#include <cstdio>
#include <cstring>
#include <exception>

int main(int argc, char* argv[]) {
    if (argc == 1 ||
        (argc == 2 && (!std::strcmp(argv[1], "--help") || !std::strcmp(argv[1], "-h")))) {
        std::puts("Usage: MemGlow.exe -p PID [options]\n"
                  "  --types         Color image, mapped, heap, stack and other private memory\n"
                  "  --fullscreen    Show only the artwork\n"
                  "  --no-glow       Disable byte-change glow\n"
                  "  --all           Start with all committed memory in view\n"
                  "  --focus ADDRESS Focus on a virtual address (decimal or 0x hexadecimal)\n"
                  "  --view-address ADDRESS Start at this virtual address\n"
                  "  --view-bin BYTES Set bytes per pixel for the starting view\n"
                  "  --report PATH   Write runtime status as JSON\n"
                  "  --probe         Read one frame without opening a window\n"
                  "  --snapshot PATH Save the probe frame as BMP\n"
                  "  --self-test     Run the built-in regression checks\n"
                  "Keys: A all memory, T colors, F glow, F11 fullscreen.\n"
                  "Esc: leave fullscreen/close.\n"
                  "Wheel: zoom about 10%; Shift+wheel about 2% in all three views.\n"
                  "Drag with the left mouse button: pan; canvas motion stays within the selected "
                  "context.");
        return 0;
    }
    try {
        return RunVirtualMemoryMap(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "MemGlow: %s\n", error.what());
        return 2;
    }
}
