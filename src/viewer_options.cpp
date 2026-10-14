// SPDX-License-Identifier: GPL-3.0-or-later
#include "viewer_options.h"
#include <stdexcept>

ViewerOptions ParseViewerOptions(int argc, char* argv[]) {
    ViewerOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--virtual-map") {
            continue;
        }
        if (option == "--self-test") {
            options.selfTest = true;
        } else if (option == "--probe") {
            options.probe = true;
        } else if (option == "--types") {
            options.types = true;
        } else if (option == "--no-glow") {
            options.glow = false;
        } else if (option == "--all") {
            options.showAll = true;
        } else if (option == "--fullscreen") {
            options.fullscreen = true;
        } else if (option == "-p" && i + 1 < argc) {
            options.pid = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (option == "--focus" && i + 1 < argc) {
            options.focus = std::stoull(argv[++i], nullptr, 0);
        } else if (option == "--report" && i + 1 < argc) {
            options.report = argv[++i];
        } else if (option == "--snapshot" && i + 1 < argc) {
            options.snapshot = argv[++i];
        } else if (option == "--view-base" && i + 1 < argc) {
            options.viewBase = std::stoull(argv[++i], nullptr, 0);
        } else if (option == "--view-address" && i + 1 < argc) {
            options.viewAddress = std::stoull(argv[++i], nullptr, 0);
        } else if (option == "--view-bin" && i + 1 < argc) {
            options.viewBin = std::stoull(argv[++i], nullptr, 0);
        } else if (option == "--context-base" && i + 1 < argc) {
            options.contextBase = std::stoull(argv[++i], nullptr, 0);
        } else if (option == "--context-bytes" && i + 1 < argc) {
            options.contextBytes = std::stoull(argv[++i], nullptr, 0);
        } else {
            throw std::invalid_argument("Unknown or incomplete option: " + option);
        }
    }
    return options;
}
