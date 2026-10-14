// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifdef WIN32
#include <Windows.h>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

// Private is the remaining MEM_PRIVATE memory after recognized heaps/stacks.
// Permissions, cache policy and read availability are separate attributes.
enum class MemoryRole : uint8_t
{
    Private,
    Heap,
    Stack,
    Image,
    Mapped,
    Count
};

struct PrivateMemoryRange {
    uint64_t base;
    uint64_t size;
    uint64_t allocation;
    bool readable;
};

struct HeapTypeTag {
    uint64_t heap;
    uint32_t index;
    bool segmentHeap;
};

struct HeapMemoryRange {
    uint64_t base;
    uint64_t size;
    uint64_t allocation;
    HeapTypeTag tag;
};

struct StackTypeTag {
    uint64_t low;
    uint64_t high;
    DWORD threadId;
};

struct MemoryTypeAtlas {
    std::unordered_map<uint64_t, HeapTypeTag> heaps;
    std::unordered_map<uint64_t, StackTypeTag> stacks;
    std::vector<HeapMemoryRange> segmentRanges;
    size_t heapRoots = 0;
    size_t ntHeapSegments = 0;
    size_t threads = 0;
    size_t failedThreads = 0;
    size_t segmentHeapContexts = 0;
    size_t segmentHeapSegments = 0;
    size_t rejectedSegmentContexts = 0;
    uint64_t bytesRead = 0;
    double queryMs = 0;
    bool x64Supported = false;
    bool segmentLayoutVerified = false;
};

// Only query/read rights are requested. Internal x64 layouts are validated;
// unsupported layouts fall back to the public page types. This does not claim
// complete custom allocator coverage. Segment Heap layouts require an exact
// target ntdll CodeView identity match before any list is followed.
MemoryTypeAtlas QueryMemoryTypes(
    HANDLE process, DWORD pid, const std::vector<PrivateMemoryRange>& regions);
const HeapTypeTag* SegmentHeapForRange(
    const MemoryTypeAtlas& atlas, uint64_t base, uint64_t size, uint64_t allocation);
bool TestSegmentHeapRecognition();
const char* MemoryRoleName(MemoryRole role);
uint32_t MemoryRoleColor(MemoryRole role);
const std::array<uint32_t, 256>& MemoryBytePalette(MemoryRole role, bool types);
uint32_t MemoryByteColor(MemoryRole role, uint8_t byte, bool types);
#endif
