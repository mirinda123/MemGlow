// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef WIN32
#include "memory_types.h"
#include "memory_access.h"
#include "segment_heap_layout.h"
#include <TlHelp32.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace {
using Clock = std::chrono::steady_clock;
using NtQueryInformation = LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
struct ProcessBasic64 {
    LONG exitCode;
    void* peb;
    uintptr_t affinity;
    LONG priority;
    uintptr_t processId;
    uintptr_t parentId;
};

struct ThreadBasic64 {
    LONG exitCode;
    void* teb;
    uintptr_t processId;
    uintptr_t threadId;
    uintptr_t affinity;
    LONG priority;
    LONG basePriority;
};
// The prefix follows phnt/ntpebteb.h. Every remote value is treated as
// untrusted and checked against the queried private memory ranges.
struct PebPrefix64 {
    BYTE flags[4];
    void* mutant;
    void* image;
    void* loader;
    void* parameters;
    void* subsystem;
    void* defaultHeap;
    void* lock;
    void* atl;
    void* ifeo;
    ULONG crossFlags;
    void* callback;
    ULONG reserved;
    ULONG atl32;
    void* apiSet;
    ULONG tlsCount;
    void* tlsBitmap;
    ULONG tlsBits[2];
    void* readOnly;
    void* shared;
    void* staticServer;
    void* ansi;
    void* oem;
    void* unicode;
    ULONG processors;
    ULONG globalFlags;
    LONGLONG timeout;
    SIZE_T reserve;
    SIZE_T commit;
    SIZE_T freeTotal;
    SIZE_T freeBlocks;
    ULONG heapCount;
    ULONG heapMax;
    void** heaps;
};
#ifdef _WIN64
static_assert(
    sizeof(ProcessBasic64) == 48 && sizeof(ThreadBasic64) == 48, "Unexpected basic info layout");
static_assert(offsetof(PebPrefix64, heapCount) == 0xE8 && offsetof(PebPrefix64, heaps) == 0xF0,
    "Unexpected PEB prefix layout");
#endif
constexpr size_t RoleCount = static_cast<size_t>(MemoryRole::Count);
using MetadataRead = std::function<bool(uint64_t, void*, size_t)>;
using PrivateRegionAt = std::function<const PrivateMemoryRange*(uint64_t)>;

uint64_t U64(const uint8_t* bytes, size_t offset) {
    uint64_t value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

bool MatchingCodeView(const std::array<uint8_t, 24>& record) {
    return std::memcmp(record.data(), "RSDS", 4) == 0 &&
           std::equal(SegmentHeapLayout::Identity.begin(),
               SegmentHeapLayout::Identity.end(),
               record.begin() + 4);
}

bool VerifySegmentHeapLayout(DWORD pid, const MetadataRead& read) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    uint64_t base = 0, size = 0;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL okay = Module32FirstW(snapshot, &entry); okay;
         okay = Module32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szModule, L"ntdll.dll") == 0) {
            base = reinterpret_cast<uint64_t>(entry.modBaseAddr);
            size = entry.modBaseSize;
            break;
        }
    }
    CloseHandle(snapshot);
    if (!base || size < sizeof(IMAGE_DOS_HEADER) + sizeof(IMAGE_NT_HEADERS64))
        return false;
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 headers{};
    if (!read(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < sizeof(dos) ||
        static_cast<uint64_t>(dos.e_lfanew) > size - sizeof(headers) ||
        !read(base + dos.e_lfanew, &headers, sizeof(headers)) ||
        headers.Signature != IMAGE_NT_SIGNATURE ||
        headers.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        headers.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        headers.OptionalHeader.SizeOfImage != size ||
        headers.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG)
        return false;
    const auto& directory = headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!directory.VirtualAddress || directory.Size < sizeof(IMAGE_DEBUG_DIRECTORY) ||
        directory.Size > 4096 || directory.VirtualAddress >= size ||
        directory.Size > size - directory.VirtualAddress)
        return false;
    std::vector<IMAGE_DEBUG_DIRECTORY> entries(directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY));
    if (!read(base + directory.VirtualAddress,
            entries.data(),
            entries.size() * sizeof(IMAGE_DEBUG_DIRECTORY)))
        return false;
    for (const auto& debug : entries) {
        if (debug.Type != IMAGE_DEBUG_TYPE_CODEVIEW || debug.SizeOfData < 24 ||
            debug.AddressOfRawData >= size || 24 > size - debug.AddressOfRawData)
            continue;
        std::array<uint8_t, 24> record{};
        if (read(base + debug.AddressOfRawData, record.data(), record.size()) &&
            MatchingCodeView(record))
            return true;
    }
    return false;
}

bool CollectSegmentContext(const HeapTypeTag& tag,
    size_t index,
    const MetadataRead& read,
    const PrivateRegionAt& at,
    std::vector<HeapMemoryRange>& ranges,
    size_t& segmentCount) {
    using namespace SegmentHeapLayout;
    uint64_t address = tag.heap + Contexts + index * ContextSize;
    std::array<uint8_t, ContextSize> context{};
    if (!read(address, context.data(), context.size()) || U64(context.data(), Heap) != tag.heap)
        return false;
    uint8_t shift = context[UnitShift], firstDescriptor = context[FirstDescriptorIndex];
    if (shift < 12 || shift > 20)
        return false;
    uint64_t segmentBytes = 256ull << shift;
    if (U64(context.data(), SegmentMask) != ~(segmentBytes - 1) ||
        (static_cast<uint64_t>(firstDescriptor) << shift) < DescriptorBytes)
        return false;
    uint64_t count = U64(context.data(), SegmentCount);
    if (count > 4096)
        return false;
    uint64_t head = address + SegmentListHead, node = U64(context.data(), SegmentListHead),
             previous = head;
    uint64_t last = U64(context.data(), SegmentListHead + 8);
    std::vector<HeapMemoryRange> pending;
    std::unordered_set<uint64_t> seen;
    for (uint64_t n = 0; n < count; ++n) {
        if (!node || node == head || (node & (segmentBytes - 1)) ||
            node >= 0x0000800000000000ull - segmentBytes || !seen.insert(node).second)
            return false;
        const auto* region = at(node);
        if (!region || !region->readable)
            return false;
        std::array<uint64_t, 3> links{};
        if (!read(node, links.data(), sizeof(links)) || links[1] != previous)
            return false;
        pending.push_back({node, segmentBytes, region->allocation, tag});
        previous = node;
        node = links[0];
    }
    if (node != head || previous != last)
        return false;
    // The process keeps running. Reject a changed list instead of publishing a partial traversal.
    std::array<uint8_t, ContextSize> after{};
    if (!read(address, after.data(), after.size()) || U64(after.data(), Heap) != tag.heap ||
        U64(after.data(), SegmentCount) != count ||
        std::memcmp(after.data() + SegmentListHead, context.data() + SegmentListHead, 16) != 0)
        return false;
    ranges.insert(ranges.end(), pending.begin(), pending.end());
    segmentCount += pending.size();
    return true;
}

void NormalizeSegmentRanges(std::vector<HeapMemoryRange>& ranges) {
    std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) {
        return a.base < b.base;
    });
    std::vector<HeapMemoryRange> merged;
    for (auto range : ranges) {
        if (!merged.empty()) {
            auto& last = merged.back();
            uint64_t end = last.base + last.size;
            bool sameOwner = last.allocation == range.allocation &&
                             last.tag.heap == range.tag.heap && last.tag.index == range.tag.index;
            if (range.base < end || (range.base == end && sameOwner)) {
                if (!sameOwner)
                    last.tag.index = 0; // Conflicting ownership remains unclassified.
                last.size = std::max(end, range.base + range.size) - last.base;
                continue;
            }
        }
        merged.push_back(range);
    }
    ranges.swap(merged);
}

constexpr std::array<uint32_t, RoleCount> Palette = {
    0xFF24FF88u, // other private: phosphor green
    0xFF9EFF32u, // confirmed heap: fluorescent lime
    0xFF38FFFFu, // thread stack: electric cyan
    0xFFFFCF44u, // EXE/DLL image, code and data: bright amber
    0xFF6EA0FFu  // mapped section: luminous blue
};

const std::array<std::array<uint32_t, 256>, RoleCount + 1>& BytePalette() {
    static const auto table = [] {
        std::array<std::array<uint32_t, 256>, RoleCount + 1> values{};
        for (uint32_t byte = 0; byte < 256; ++byte) {
            // Both modes use linear brightness: 0 is black, 255 is the palette color.
            for (size_t role = 0; role <= RoleCount; ++role) {
                uint32_t color = role < RoleCount ? Palette[role] : 0xFF00FF00u;
                values[role][byte] = 0xFF000000u | (((color >> 16 & 255) * byte / 255) << 16) |
                                     (((color >> 8 & 255) * byte / 255) << 8) |
                                     ((color & 255) * byte / 255);
            }
        }
        return values;
    }();
    return table;
}
} // namespace

const char* MemoryRoleName(MemoryRole role) {
    static const char* names[] = {"OTHER PRIVATE", "HEAP", "STACK", "IMAGE", "MAPPED"};
    static_assert(sizeof(names) / sizeof(*names) == RoleCount, "Each category needs a label");
    return names[static_cast<size_t>(role)];
}

uint32_t MemoryRoleColor(MemoryRole role) {
    return Palette[static_cast<size_t>(role)];
}

const std::array<uint32_t, 256>& MemoryBytePalette(MemoryRole role, bool types) {
    return BytePalette()[types ? static_cast<size_t>(role) : RoleCount];
}

uint32_t MemoryByteColor(MemoryRole role, uint8_t byte, bool types) {
    return MemoryBytePalette(role, types)[byte];
}

const HeapTypeTag* SegmentHeapForRange(
    const MemoryTypeAtlas& atlas, uint64_t base, uint64_t size, uint64_t allocation) {
    auto it = std::upper_bound(atlas.segmentRanges.begin(),
        atlas.segmentRanges.end(),
        base,
        [](uint64_t value, const HeapMemoryRange& range) {
            return value < range.base;
        });
    if (it == atlas.segmentRanges.begin())
        return nullptr;
    --it;
    return it->tag.index && it->allocation == allocation && base >= it->base &&
                   base - it->base < it->size && size <= it->size - (base - it->base)
               ? &it->tag
               : nullptr;
}

bool TestSegmentHeapRecognition() {
    using namespace SegmentHeapLayout;
    constexpr uint64_t MiB = 1024 * 1024, base = 0x10000000, one = base + MiB, two = base + 2 * MiB;
    std::vector<uint8_t> bytes(3 * MiB);
    std::vector<PrivateMemoryRange> regions{
        {base, MiB, base, true}, {one, MiB, one, true}, {two, MiB, one, true}};
    uint64_t context = base + Contexts, head = context + SegmentListHead;
    auto put = [&](uint64_t address, uint64_t value) {
        std::memcpy(bytes.data() + address - base, &value, 8);
    };
    auto reset = [&] {
        std::fill(bytes.begin(), bytes.end(), 0);
        put(context + Heap, base);
        put(context + SegmentMask, ~(MiB - 1));
        bytes[context - base + UnitShift] = 12;
        bytes[context - base + FirstDescriptorIndex] = 2;
        put(context + SegmentCount, 2);
        put(head, one);
        put(head + 8, two);
        put(one, two);
        put(one + 8, head);
        put(two, head);
        put(two + 8, one);
        regions[2].readable = true;
    };
    PrivateRegionAt at = [&](uint64_t address) -> const PrivateMemoryRange* {
        for (const auto& region : regions)
            if (address >= region.base && address - region.base < region.size)
                return &region;
        return nullptr;
    };
    bool mutate = false;
    size_t contextReads = 0, guardReads = 0;
    MetadataRead read = [&](uint64_t address, void* destination, size_t size) {
        if (address < base || address - base >= bytes.size() ||
            size > bytes.size() - (address - base))
            return false;
        if (address == two && !regions[2].readable) {
            ++guardReads;
            return false;
        }
        std::memcpy(destination, bytes.data() + address - base, size);
        if (address == context && ++contextReads == 2 && mutate) {
            uint64_t changed = 3;
            std::memcpy(static_cast<uint8_t*>(destination) + SegmentCount, &changed, 8);
        }
        return true;
    };
    auto collect = [&](std::vector<HeapMemoryRange>& ranges) {
        size_t count = 0;
        contextReads = 0;
        return CollectSegmentContext({base, 1, true}, 0, read, at, ranges, count);
    };
    reset();
    std::vector<HeapMemoryRange> valid;
    bool okay = collect(valid) && valid.size() == 2;
    NormalizeSegmentRanges(valid);
    MemoryTypeAtlas atlas;
    atlas.segmentRanges = valid;
    okay = okay && valid.size() == 1 && SegmentHeapForRange(atlas, one + MiB / 2, MiB, one) &&
           !SegmentHeapForRange(atlas, one - 4096, 4096, base) &&
           !SegmentHeapForRange(atlas, two, MiB + 1, one) &&
           !SegmentHeapForRange(atlas, one, 4096, base);
    auto rejects = [&] {
        std::vector<HeapMemoryRange> rejected;
        return !collect(rejected) && rejected.empty();
    };
    put(context + Heap, base + 4096);
    okay = rejects() && okay;
    reset();
    put(two, one);
    okay = rejects() && okay;
    reset();
    put(two + 8, head);
    okay = rejects() && okay;
    reset();
    put(context + SegmentCount, 4097);
    okay = rejects() && okay;
    reset();
    put(context + SegmentMask, 0);
    okay = rejects() && okay;
    reset();
    regions[2].readable = false;
    okay = rejects() && guardReads == 0 && okay;
    reset();
    mutate = true;
    okay = rejects() && okay;
    mutate = false;
    reset();
    put(head, head);
    put(head + 8, head);
    put(context + SegmentCount, 0);
    std::vector<HeapMemoryRange> empty;
    okay = collect(empty) && empty.empty() && okay;
    std::array<uint8_t, 24> identity{};
    std::memcpy(identity.data(), "RSDS", 4);
    std::copy(Identity.begin(), Identity.end(), identity.begin() + 4);
    okay = MatchingCodeView(identity) && okay;
    identity[20] ^= 1;
    okay = !MatchingCodeView(identity) && okay;
    atlas.segmentRanges = {
        {one, MiB, one, {base, 1, true}}, {one + 4096, 4096, one, {base + 4096, 2, true}}};
    NormalizeSegmentRanges(atlas.segmentRanges);
    okay = !SegmentHeapForRange(atlas, one, 4096, one) && okay;
    std::printf("Self-test Segment Heap: linked ownership, merged ranges, bounds, wrong owner, "
                "cycles, broken links, limits, guard skip, concurrent change, empty list, version "
                "mismatch, conflicts=%d\n",
        okay);
    return okay;
}

MemoryTypeAtlas QueryMemoryTypes(
    HANDLE process, DWORD pid, const std::vector<PrivateMemoryRange>& regions) {
    auto started = Clock::now();
    MemoryTypeAtlas result;
#ifdef _WIN64
    auto finish = [&] {
        result.queryMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        return result;
    };
    BOOL wow64 = FALSE;
    if (!IsWow64Process(process, &wow64) || wow64)
        return finish();
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return finish();
    auto queryProcess =
        reinterpret_cast<NtQueryInformation>(GetProcAddress(ntdll, "NtQueryInformationProcess"));
    auto queryThread =
        reinterpret_cast<NtQueryInformation>(GetProcAddress(ntdll, "NtQueryInformationThread"));
    if (!queryProcess || !queryThread)
        return finish();
    result.x64Supported = true;
    uint64_t metadataBudget = 4 * 1024 * 1024;
    auto read = [&](uint64_t address, void* destination, size_t size) {
        if (!address || size > metadataBudget)
            return false;
        metadataBudget -= size;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQueryEx(process, reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) ||
            !IsReadablePage(mbi.State, mbi.Protect) || (mbi.Protect & PAGE_WRITECOMBINE))
            return false;
        uint64_t start = reinterpret_cast<uint64_t>(mbi.BaseAddress);
        if (size > mbi.RegionSize - (address - start))
            return false;
        SIZE_T moved = 0;
        bool okay =
            ReadProcessMemory(
                process, reinterpret_cast<void*>(address), destination, size, &moved) != FALSE;
        result.bytesRead += moved;
        return okay && moved == size;
    };
    auto at = [&](uint64_t address) -> const PrivateMemoryRange* {
        auto it = std::upper_bound(regions.begin(),
            regions.end(),
            address,
            [](uint64_t value, const PrivateMemoryRange& r) {
                return value < r.base;
            });
        if (it == regions.begin())
            return nullptr;
        --it;
        return address >= it->base && address - it->base < it->size ? &*it : nullptr;
    };
    std::unordered_map<uint64_t, HeapTypeTag> heapRoots;
    ProcessBasic64 basic{};
    if (queryProcess(process, 0, &basic, sizeof(basic), nullptr) >= 0) {
        PebPrefix64 peb{};
        uint64_t pebAddress = reinterpret_cast<uint64_t>(basic.peb);
        if (read(pebAddress, &peb, sizeof(peb)) && peb.heapCount > 0 &&
            peb.heapCount <= peb.heapMax && peb.heapMax < 65536) {
            std::vector<uint64_t> heaps(peb.heapCount);
            uint64_t arrayAddress = reinterpret_cast<uint64_t>(peb.heaps);
            if (read(arrayAddress, heaps.data(), heaps.size() * sizeof(uint64_t)) &&
                std::find(heaps.begin(),
                    heaps.end(),
                    reinterpret_cast<uint64_t>(peb.defaultHeap)) != heaps.end()) {
                for (size_t index = 0; index < heaps.size(); ++index) {
                    uint64_t heap = heaps[index];
                    const auto* region = at(heap);
                    std::array<uint8_t, 48> header{};
                    if (!region || !read(heap, header.data(), header.size()))
                        continue;
                    uint32_t signature = 0;
                    std::memcpy(&signature, header.data() + 16, sizeof(signature));
                    if (signature != 0xFFEEFFEEu && signature != 0xDDEEDDEEu)
                        continue;
                    HeapTypeTag tag{
                        heap, static_cast<uint32_t>(index + 1), signature == 0xDDEEDDEEu};
                    heapRoots[heap] = tag;
                    result.heaps[region->allocation] = tag;
                    ++result.heapRoots;
                }
            }
        }
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL okay = Thread32First(snapshot, &entry); okay;
             entry.dwSize = sizeof(entry), okay = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != pid)
                continue;
            ++result.threads;
            HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
            if (!thread) {
                ++result.failedThreads;
                continue;
            }
            ThreadBasic64 threadInfo{};
            LONG status = queryThread(thread, 0, &threadInfo, sizeof(threadInfo), nullptr);
            CloseHandle(thread);
            std::array<uint64_t, 7> tib{};
            uint64_t teb = reinterpret_cast<uint64_t>(threadInfo.teb);
            if (status < 0 || threadInfo.processId != pid || !read(teb, tib.data(), sizeof(tib))) {
                ++result.failedThreads;
                continue;
            }
            uint64_t high = tib[1], low = tib[2];
            const auto* region = at(low);
            const auto* stackEnd = at(high - 1);
            if (!region || tib[6] != teb || low >= high || high > 0x0000800000000000ull ||
                !stackEnd || stackEnd->allocation != region->allocation) {
                ++result.failedThreads;
                continue;
            }
            result.stacks[region->allocation] = {low, high, entry.th32ThreadID};
        }
        CloseHandle(snapshot);
    }
    // A matching segment header must point back to a validated NT heap root.
    for (const auto& region : regions) {
        if (!region.readable || region.size < 48 || result.heaps.count(region.allocation) ||
            result.stacks.count(region.allocation))
            continue;
        std::array<uint8_t, 48> header{};
        if (!read(region.base, header.data(), header.size()))
            continue;
        uint32_t signature = 0;
        uint64_t owner = 0;
        std::memcpy(&signature, header.data() + 16, sizeof(signature));
        std::memcpy(&owner, header.data() + 40, sizeof(owner));
        auto root = heapRoots.find(owner);
        if (signature == 0xFFEEFFEEu && root == heapRoots.end()) {
            // Some Windows heap registrations are absent from the PEB array.
            // Accept a separately discovered NT root only when its segment
            // self pointer, base pointer and allocator signature agree.
            const auto* rootRegion = at(owner);
            std::array<uint8_t, 0x10C> rootHeader{};
            if (rootRegion && owner == rootRegion->allocation &&
                read(owner, rootHeader.data(), rootHeader.size())) {
                uint32_t rootSegment = 0, signature8 = 0, signature10 = 0;
                uint64_t rootOwner = 0, rootBase = 0;
                std::memcpy(&rootSegment, rootHeader.data() + 16, 4);
                std::memcpy(&rootOwner, rootHeader.data() + 40, 8);
                std::memcpy(&rootBase, rootHeader.data() + 48, 8);
                std::memcpy(&signature8, rootHeader.data() + 0x98, 4);
                std::memcpy(&signature10, rootHeader.data() + 0x108, 4);
                if (rootSegment == 0xFFEEFFEEu && rootOwner == owner && rootBase == owner &&
                    (signature8 == 0xEEFFEEFFu || signature10 == 0xEEFFEEFFu)) {
                    HeapTypeTag tag{owner, static_cast<uint32_t>(++result.heapRoots), false};
                    heapRoots[owner] = tag;
                    result.heaps[rootRegion->allocation] = tag;
                    root = heapRoots.find(owner);
                }
            }
        }
        if (signature == 0xFFEEFFEEu && root != heapRoots.end() && !root->second.segmentHeap) {
            result.heaps[region.allocation] = root->second;
            ++result.ntHeapSegments;
        }
    }
    result.segmentLayoutVerified = VerifySegmentHeapLayout(pid, read);
    if (result.segmentLayoutVerified) {
        for (const auto& root : heapRoots)
            if (root.second.segmentHeap) {
                for (size_t index = 0; index < 2; ++index) {
                    if (CollectSegmentContext(root.second,
                            index,
                            read,
                            at,
                            result.segmentRanges,
                            result.segmentHeapSegments))
                        ++result.segmentHeapContexts;
                    else
                        ++result.rejectedSegmentContexts;
                }
            }
        NormalizeSegmentRanges(result.segmentRanges);
    }
    return finish();
#else
    result.queryMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    return result;
#endif
}
#endif
