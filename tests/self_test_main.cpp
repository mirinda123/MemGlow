// SPDX-License-Identifier: GPL-3.0-or-later
#include "memglow/full_memory_view.h"
#include <cstdio>
#include <exception>
int main() {
    try {
        return RunMemorySelfTests() ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Self-test failed: %s\n", error.what());
        return 1;
    }
}
