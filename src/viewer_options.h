// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

struct ViewerOptions {
    uint32_t pid = 0;
    uint64_t focus = 0;
    uint64_t viewBase = 0;
    uint64_t viewAddress = 0;
    uint64_t viewBin = 0;
    uint64_t contextBase = 0;
    uint64_t contextBytes = 0;
    std::string report;
    std::string snapshot;
    bool selfTest = false;
    bool probe = false;
    bool types = false;
    bool glow = true;
    bool showAll = false;
    bool fullscreen = false;
};

ViewerOptions ParseViewerOptions(int argc, char* argv[]);
