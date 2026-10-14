// SPDX-License-Identifier: GPL-3.0-or-later
// Address-ordered whole-memory view and optional virtual-address-space map.
// Target access is limited to PROCESS_QUERY_INFORMATION | PROCESS_VM_READ.
#ifdef WIN32
#include "memglow/full_memory_view.h"
#include "memory_access.h"
#include "memory_types.h"
#include "viewer_options.h"
#include <Windows.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <SDL_system.h>
#include <emmintrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr int CanvasWidth = 1024;
constexpr int CanvasHeight = 512;
constexpr int PixelCount = CanvasWidth * CanvasHeight;
constexpr size_t ReadChunkBytes = 16 * 1024 * 1024;
constexpr int StripWidth = 112;
constexpr int OverviewX = 8;
constexpr int SelectionX = 136;
constexpr int CanvasX = 264;
constexpr int CanvasY = 88;
constexpr int WindowWidth = CanvasX + CanvasWidth;
constexpr int WindowHeight = CanvasY + CanvasHeight + 90;
enum class ToolbarAction
{
    None,
    Fullscreen,
    All,
    Types,
    Glow
};

constexpr std::array<ToolbarAction, 4> ToolbarActions{
    ToolbarAction::Fullscreen,
    ToolbarAction::All,
    ToolbarAction::Types,
    ToolbarAction::Glow,
};
constexpr int ToolbarSpacing = 96;
constexpr int ToolbarLeft = WindowWidth - static_cast<int>(ToolbarActions.size()) * ToolbarSpacing;
constexpr uint32_t FreeColor = 0xFF030705;
constexpr uint32_t ReserveColor = 0xFF122A45;
constexpr uint32_t ProtectedColor = 0xFF9D58CF;
constexpr uint32_t PaddingColor = 0xFF161A18;
constexpr uint8_t ReadableFlag = 1;
constexpr uint8_t ReservedFlag = 2;
constexpr uint8_t ProtectedFlag = 4;
constexpr double WheelZoomStep = 1.10;
constexpr double FineWheelZoomStep = 1.02;

struct ContentPoint {
    float x;
    float y;
};

SDL_Rect ToolbarRect(size_t index) {
    return {ToolbarLeft + static_cast<int>(index) * ToolbarSpacing, 4, 92, 24};
}

ToolbarAction ToolbarActionAt(ContentPoint point) {
    for (size_t i = 0; i < ToolbarActions.size(); ++i) {
        const auto box = ToolbarRect(i);
        if (point.x >= box.x && point.x < box.x + box.w && point.y >= box.y &&
            point.y < box.y + box.h) {
            return ToolbarActions[i];
        }
    }
    return ToolbarAction::None;
}

bool InCanvas(ContentPoint point) {
    return point.x >= CanvasX && point.x < CanvasX + CanvasWidth && point.y >= CanvasY &&
           point.y < CanvasY + CanvasHeight;
}

ContentPoint FullscreenContentPoint(float x, float y, int windowWidth, int windowHeight) {
    // Fullscreen stretches only the artwork; input still uses the original navigation coordinates.
    return {x * WindowWidth / std::max(1, windowWidth),
        CanvasY + y * CanvasHeight / std::max(1, windowHeight)};
}

struct PresentationKeys {
    void* windowHandle = nullptr;
    uint32_t windowId = 0;
};

void SDLCALL ForwardPresentationKey(
    void* userdata, void* windowHandle, unsigned int message, Uint64 key, Sint64 flags) {
    const auto& target = *static_cast<const PresentationKeys*>(userdata);
    if (windowHandle != target.windowHandle || (message != WM_KEYDOWN && message != WM_KEYUP))
        return;
    // Some input tools omit the scan code. SDL drops these virtual-key messages.
    // Forward only this window's presentation shortcuts, leaving ordinary keyboard events to SDL.
    if ((static_cast<Uint64>(flags) & 0x00FF0000u) || (key != VK_F11 && key != VK_ESCAPE))
        return;
    SDL_Event event{};
    event.type = message == WM_KEYDOWN ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.timestamp = SDL_GetTicks();
    event.key.windowID = target.windowId;
    event.key.state = message == WM_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
    event.key.repeat = message == WM_KEYDOWN && (static_cast<Uint64>(flags) & (1ull << 30)) ? 1 : 0;
    event.key.keysym.sym = key == VK_F11 ? SDLK_F11 : SDLK_ESCAPE;
    event.key.keysym.scancode = key == VK_F11 ? SDL_SCANCODE_F11 : SDL_SCANCODE_ESCAPE;
    SDL_PushEvent(&event);
}

struct Region {
    uint64_t base;
    uint64_t size;
    uint64_t allocation;
    DWORD state;
    DWORD protect;
    DWORD kind;
    bool readable;
    uint64_t position = 0;
    MemoryRole role = MemoryRole::Private;
    uint64_t owner = 0;
};

struct Map {
    uint64_t limit = 0;
    uint64_t committed = 0;
    uint64_t reserved = 0;
    uint64_t readable = 0;
    uint64_t excluded = 0;
    size_t readableRegions = 0;
    std::vector<Region> regions;
    std::array<uint64_t, static_cast<size_t>(MemoryRole::Count)> roleBytes{};
    size_t heapRoots = 0;
    size_t stacks = 0;
    size_t threads = 0;
    size_t failedThreads = 0;
    size_t segmentHeapContexts = 0;
    size_t segmentHeapSegments = 0;
    size_t rejectedSegmentContexts = 0;
    uint64_t typeMetadataBytes = 0;
    double typeQueryMs = 0;
    double typeAgeMs = 0;
    bool detailedTypes = false;
    bool segmentLayoutVerified = false;
};

enum class AnchorStatus
{
    Available,
    Released,
    Remapped,
    RangeUnavailable
};

struct NavigationAnchor {
    uint64_t address = 0;
    uint64_t allocation = 0;
    uint64_t contextFirst = 0;
    uint64_t contextLast = 0;
    DWORD kind = 0;
    MemoryRole role = MemoryRole::Private;
    AnchorStatus status = AnchorStatus::Available;
    bool active = false;
    bool wholeContext = false;
    bool fillsContext = false;
};

struct Request {
    uint64_t base = 0;
    uint64_t bin = 1;
    uint64_t sequence = 0;
    bool packed = true;
    bool overview = true;
    bool types = false;
    bool glow = true;
    uint64_t contextBase = 0;
    uint64_t contextBytes = 0;
    std::shared_ptr<const Map> coordinates;
    NavigationAnchor anchor;
};

struct MiniChanges {
    std::array<uint32_t, CanvasHeight> packed{};
    std::array<uint32_t, CanvasHeight> addresses{};
    std::array<uint32_t, CanvasHeight> context{};
    uint64_t contextBase = 0;
    uint64_t contextBytes = 0;
    bool contextPacked = true;
    uint64_t bytes = 0;
};

struct OverviewActivity {
    std::shared_ptr<const Map> map;
    std::array<uint32_t, CanvasHeight> lastPacked{};
    std::array<uint32_t, CanvasHeight> lastAddresses{};
    std::array<uint32_t, CanvasHeight> lastContext{};
    uint64_t contextBase = 0;
    uint64_t contextBytes = 0;
    bool contextPacked = true;
    uint64_t layout = 0;
    uint64_t bytesRead = 0;
    uint64_t bytesPlanned = 0;
    uint64_t changedBytes = 0;
    uint64_t writeCombineBytesRead = 0;
    uint64_t samples = 0;
    uint64_t changedTotal = 0;
    uint64_t cacheBytes = 0;
    size_t probes = 0;
    double sampleMs = 0;
    double intervalMs = 0;
};

struct StripFrame {
    std::shared_ptr<const Map> map;
    uint64_t base = 0;
    uint64_t bytes = 0;
    uint64_t layout = 0;
    uint64_t bytesRead = 0;
    uint64_t writeCombineBytesRead = 0;
    bool packed = true;
    bool types = false;
    std::vector<uint32_t> pixels;
    std::vector<uint32_t> lastChanged;
};

struct DualStrips {
    std::shared_ptr<const StripFrame> overview;
    std::shared_ptr<const StripFrame> selection;
};

struct Frame {
    std::shared_ptr<const Map> map;
    Request request;
    std::vector<uint32_t> pixels;
    std::vector<uint32_t> changes;
    std::vector<uint32_t> lastChanged;
    uint64_t bytesRead = 0;
    uint64_t bytesWanted = 0;
    uint64_t writeCombineBytesRead = 0;
    uint64_t writeCombineBytesWanted = 0;
    double writeCombineReadMs = 0;
    uint64_t changedBytes = 0;
    uint64_t changedPixels = 0;
    uint64_t changeCacheBytes = 0;
    uint64_t layout = 0;
    uint64_t samples = 0;
    uint64_t changedBytesTotal = 0;
    MiniChanges miniChanges;
    std::array<uint32_t, CanvasHeight> miniLastPacked{};
    std::array<uint32_t, CanvasHeight> miniLastAddresses{};
    std::array<uint32_t, CanvasHeight> contextLast{};
    uint64_t miniLayout = 0;
    double readMs = 0;
    double intervalMs = 0;
    bool byteView = false;
};

struct Shared {
    std::mutex mutex;
    std::condition_variable requestChanged;
    Request request;
    std::shared_ptr<const Frame> frame;
    std::shared_ptr<const OverviewActivity> overviewActivity;
    std::shared_ptr<const DualStrips> strips;
    std::atomic<bool> stop{false};
    std::atomic<bool> targetEnded{false};
    std::atomic<bool> fullscreen{false};
};

bool IsExecutable(DWORD protect) {
    DWORD base = protect & 0xFF;
    return base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE ||
           base == PAGE_EXECUTE_WRITECOPY;
}

uint32_t UnsampledColor(MemoryRole role, bool types, size_t x, size_t y) {
    if (!types)
        return ProtectedColor;
    // A status pattern preserves the category hue without inventing byte data.
    uint32_t color = MemoryRoleColor(role), level = ((x + y) & 7) < 2 ? 112 : 24;
    auto channel = [&](unsigned shift) {
        return (((color >> shift) & 255) * level / 255) << shift;
    };
    return 0xFF000000u | channel(16) | channel(8) | channel(0);
}

std::string PageAttributes(const Region& region) {
    if (region.state != MEM_COMMIT)
        return "";
    std::string result;
    switch (region.protect & 0xFF) {
        case PAGE_NOACCESS:
            result = "NOACCESS";
            break;
        case PAGE_READONLY:
            result = "R";
            break;
        case PAGE_READWRITE:
            result = "RW";
            break;
        case PAGE_WRITECOPY:
            result = "R COPY-ON-WRITE";
            break;
        case PAGE_EXECUTE:
            result = "X";
            break;
        case PAGE_EXECUTE_READ:
            result = "RX";
            break;
        case PAGE_EXECUTE_READWRITE:
            result = "RWX";
            break;
        case PAGE_EXECUTE_WRITECOPY:
            result = "RX COPY-ON-WRITE";
            break;
        default:
            result = "UNKNOWN ACCESS";
            break;
    }
    if (region.protect & PAGE_WRITECOMBINE)
        result += " WC";
    if (region.protect & PAGE_NOCACHE)
        result += " NOCACHE";
    if (region.protect & PAGE_GUARD)
        result += " GUARD";
    if (!region.readable)
        result += " / UNSAMPLED";
    return result;
}

std::shared_ptr<Map> QueryMap(HANDLE process) {
    auto map = std::make_shared<Map>();
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    map->limit = reinterpret_cast<uint64_t>(info.lpMaximumApplicationAddress) + 1;
    uint64_t cursor = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    while (cursor < map->limit &&
           VirtualQueryEx(process, reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi))) {
        uint64_t base = reinterpret_cast<uint64_t>(mbi.BaseAddress);
        uint64_t size = std::min<uint64_t>(mbi.RegionSize, map->limit - base);
        if (!size || base + size <= cursor)
            break;
        Region region{base,
            size,
            reinterpret_cast<uint64_t>(mbi.AllocationBase),
            mbi.State,
            mbi.Protect,
            mbi.Type,
            IsReadablePage(mbi.State, mbi.Protect)};
        if (region.state == MEM_RESERVE)
            map->reserved += size;
        if (region.state == MEM_COMMIT) {
            region.position = map->committed;
            map->committed += size;
            if (region.readable) {
                map->readable += size;
                ++map->readableRegions;
            } else
                map->excluded += size;
        }
        map->regions.push_back(region);
        cursor = base + size;
    }
    return map;
}

std::vector<PrivateMemoryRange> PrivateRanges(const Map& map) {
    std::vector<PrivateMemoryRange> result;
    for (const auto& r : map.regions)
        if (r.state == MEM_COMMIT && r.kind == MEM_PRIVATE)
            result.push_back({r.base, r.size, r.allocation, r.readable});
    return result;
}

void ApplyTypes(Map& map, const MemoryTypeAtlas& atlas, double ageMs = 0) {
    map.roleBytes.fill(0);
    map.heapRoots = atlas.heapRoots;
    map.stacks = atlas.stacks.size();
    map.threads = atlas.threads;
    map.failedThreads = atlas.failedThreads;
    map.typeQueryMs = atlas.queryMs;
    map.typeMetadataBytes = atlas.bytesRead;
    map.typeAgeMs = ageMs;
    map.detailedTypes = atlas.x64Supported;
    map.segmentHeapContexts = atlas.segmentHeapContexts;
    map.segmentHeapSegments = atlas.segmentHeapSegments;
    map.rejectedSegmentContexts = atlas.rejectedSegmentContexts;
    map.segmentLayoutVerified = atlas.segmentLayoutVerified;
    for (auto& r : map.regions) {
        r.owner = 0;
        r.role = r.kind == MEM_IMAGE    ? MemoryRole::Image
                 : r.kind == MEM_MAPPED ? MemoryRole::Mapped
                                        : MemoryRole::Private;
        if (r.kind == MEM_PRIVATE) {
            auto stack = atlas.stacks.find(r.allocation);
            auto heap = atlas.heaps.find(r.allocation);
            auto segmentHeap = SegmentHeapForRange(atlas, r.base, r.size, r.allocation);
            if (stack != atlas.stacks.end() && r.base < stack->second.high) {
                r.role = MemoryRole::Stack;
                r.owner = stack->second.threadId;
            } else if (heap != atlas.heaps.end()) {
                r.role = MemoryRole::Heap;
                r.owner = heap->second.index;
            } else if (segmentHeap) {
                r.role = MemoryRole::Heap;
                r.owner = segmentHeap->index;
            }
        }
        if (r.state == MEM_COMMIT)
            map.roleBytes[static_cast<size_t>(r.role)] += r.size;
    }
}

uint64_t FullBin(uint64_t limit) {
    uint64_t bin = 1;
    while (bin * PixelCount < limit)
        bin *= 2;
    return bin;
}

uint64_t DataBin(const Map& map) {
    return std::max<uint64_t>(1, (map.committed + PixelCount - 1) / PixelCount);
}

uint64_t Limit(const Map& map, bool packed) {
    return packed ? map.committed : map.limit;
}

uint64_t DetailSpan(const Request& request, uint64_t limit) {
    // The final bin may be partial when a selection is not a multiple of N.
    return std::min(
        {limit, request.bin * PixelCount, request.contextBytes ? request.contextBytes : limit});
}

uint64_t DetailEnd(const Request& request, uint64_t limit) {
    return request.base +
           std::min(DetailSpan(request, limit), request.base < limit ? limit - request.base : 0);
}

uint64_t Position(const Region& region, bool packed) {
    return packed ? region.position : region.base;
}

uint64_t PositionOfAddress(const Map& map, uint64_t address) {
    for (const auto& r : map.regions)
        if (r.state == MEM_COMMIT && address >= r.base && address < r.base + r.size)
            return r.position + address - r.base;
    return 0;
}

uint64_t AddressOfPosition(const Map& map, uint64_t position, bool packed) {
    if (!packed)
        return position;
    for (const auto& r : map.regions)
        if (r.state == MEM_COMMIT && position >= r.position && position < r.position + r.size)
            return r.base + position - r.position;
    return 0;
}

Request Center(
    uint64_t address, uint64_t bin, uint64_t limit, uint64_t sequence, bool packed = true) {
    uint64_t span = bin * PixelCount, maxBase = limit > span ? limit - span : 0;
    uint64_t base = address > span / 2 ? address - span / 2 : 0;
    base = std::min(base, maxBase);
    base -= base % bin;
    return {base, bin, sequence, packed, false};
}

Request FitNavigation(Request request, uint64_t limit) {
    if (!limit)
        return request;
    uint64_t span = DetailSpan(request, limit);
    request.base = std::min(request.base, limit - span);
    if (request.overview) {
        request.contextBase = 0;
        request.contextBytes = limit;
        return request;
    }
    if (!request.contextBytes) {
        request.contextBytes = std::min(limit, std::max<uint64_t>(64 * 1024 * 1024, span * 4));
        uint64_t middle = request.base + span / 2;
        request.contextBase =
            middle > request.contextBytes / 2 ? middle - request.contextBytes / 2 : 0;
    }
    request.contextBytes = std::min(
        limit, std::max<uint64_t>(std::min<uint64_t>(PixelCount, limit), request.contextBytes));
    span = DetailSpan(request, limit);
    request.base = std::min(request.base, limit - span);
    request.contextBase = std::min(request.contextBase, limit - request.contextBytes);
    if (request.base < request.contextBase)
        request.contextBase = request.base;
    if (request.base + span > request.contextBase + request.contextBytes)
        request.contextBase = request.base + span - request.contextBytes;
    return request;
}

const Region* CommittedAtPosition(const Map& map, uint64_t position) {
    for (const auto& region : map.regions)
        if (region.state == MEM_COMMIT && position >= region.position &&
            position - region.position < region.size)
            return &region;
    return nullptr;
}

const Region* CommittedAtAddress(const Map& map, uint64_t address) {
    for (const auto& region : map.regions)
        if (region.state == MEM_COMMIT && address >= region.base &&
            address - region.base < region.size)
            return &region;
    return nullptr;
}

uint64_t PackedBoundary(const Map& map, uint64_t address) {
    for (const auto& region : map.regions)
        if (region.state == MEM_COMMIT && address < region.base + region.size)
            return region.position + (address > region.base ? address - region.base : 0);
    return map.committed;
}

Request AnchorNavigation(Request request, std::shared_ptr<const Map> map) {
    request = FitNavigation(request, Limit(*map, request.packed));
    request.coordinates = std::move(map);
    request.anchor = {};
    if (!request.packed || request.overview)
        return request;
    const auto& current = *request.coordinates;
    const auto* region = CommittedAtPosition(current, request.base);
    if (!region)
        return request;
    auto& anchor = request.anchor;
    anchor.address = region->base + request.base - region->position;
    anchor.allocation = region->allocation;
    anchor.kind = region->kind;
    anchor.role = region->role;
    anchor.contextFirst = AddressOfPosition(current, request.contextBase, true);
    anchor.contextLast =
        AddressOfPosition(current, request.contextBase + request.contextBytes - 1, true);
    anchor.wholeContext = request.contextBase == 0 && request.contextBytes == current.committed;
    anchor.fillsContext = request.base == request.contextBase &&
                          DetailSpan(request, current.committed) == request.contextBytes;
    if (anchor.wholeContext && anchor.fillsContext) {
        request.overview = true;
        request.anchor = {};
    } else
        anchor.active = true;
    return request;
}

bool AnchorUnavailable(const Request& request) {
    return request.anchor.active && request.anchor.status != AnchorStatus::Available;
}

const char* AnchorStatusName(const Request& request) {
    if (!request.anchor.active)
        return request.overview ? "whole_process" : "unanchored";
    switch (request.anchor.status) {
        case AnchorStatus::Released:
            return "released";
        case AnchorStatus::Remapped:
            return "remapped";
        case AnchorStatus::RangeUnavailable:
            return "range_unavailable";
        default:
            return "available";
    }
}

const char* AnchorMessage(const Request& request) {
    switch (request.anchor.status) {
        case AnchorStatus::Released:
            return "SELECTED ADDRESS RELEASED";
        case AnchorStatus::Remapped:
            return "SELECTED ADDRESS REMAPPED";
        default:
            return "SELECTED RANGE UNAVAILABLE";
    }
}

Request RebaseNavigation(Request request, std::shared_ptr<const Map> map) {
    if (request.coordinates == map)
        return request;
    if (!request.coordinates)
        return AnchorNavigation(std::move(request), std::move(map));
    request.coordinates = map;
    const auto limit = Limit(*map, request.packed);
    if (request.overview) {
        request.base = 0;
        request.bin = request.packed ? DataBin(*map) : FullBin(map->limit);
        return FitNavigation(request, limit);
    }
    auto& anchor = request.anchor;
    if (!request.packed || !anchor.active)
        return FitNavigation(request, limit);

    request.contextBase = anchor.wholeContext ? 0 : PackedBoundary(*map, anchor.contextFirst);
    const auto contextEnd =
        anchor.wholeContext ? limit : PackedBoundary(*map, anchor.contextLast + 1);
    request.contextBytes = contextEnd - request.contextBase;
    const auto* region = CommittedAtAddress(*map, anchor.address);
    if (anchor.status == AnchorStatus::Available) {
        if (!region)
            anchor.status = AnchorStatus::Released;
        else if (region->allocation != anchor.allocation || region->kind != anchor.kind)
            anchor.status = AnchorStatus::Remapped;
        else if (!request.contextBytes)
            anchor.status = AnchorStatus::RangeUnavailable;
    }
    request.base = PackedBoundary(*map, anchor.address);
    if (anchor.fillsContext && !AnchorUnavailable(request)) {
        request.base = request.contextBase;
        request.bin = std::max<uint64_t>(1, (request.contextBytes + PixelCount - 1) / PixelCount);
    }
    const auto anchoredBase = request.base;
    request = FitNavigation(request, limit);
    // Never silently replace an anchored view with a different address at an edge.
    if (request.base != anchoredBase && !AnchorUnavailable(request))
        request.anchor.status = AnchorStatus::RangeUnavailable;
    return request;
}

Request ChooseContext(Request request, uint64_t base, uint64_t bytes, uint64_t limit) {
    request.overview = false;
    bytes = std::min(limit, std::max<uint64_t>(std::min<uint64_t>(PixelCount, limit), bytes));
    base = std::min(base, limit - bytes);
    request.bin =
        std::max<uint64_t>(1, std::min(request.bin, (bytes + PixelCount - 1) / PixelCount));
    uint64_t span = std::min(bytes, request.bin * PixelCount);
    if (request.base < base || request.base + span > base + bytes)
        request.base = base + (bytes - span) / 2;
    request.contextBase = base;
    request.contextBytes = bytes;
    return FitNavigation(request, limit);
}

Request ChooseDetail(Request request, uint64_t base, uint64_t bytes, uint64_t limit) {
    request.overview = false;
    request.base = base;
    request.bin = std::max<uint64_t>(1, (bytes + PixelCount - 1) / PixelCount);
    return FitNavigation(request, limit);
}

struct WheelScale {
    double desired = 0;
    uint64_t last = 0;
    uint64_t Apply(
        uint64_t current, uint64_t minimum, uint64_t maximum, double notches, bool fine) {
        if (!last || current != last)
            desired = static_cast<double>(current);
        // Retain sub-integer steps so high-resolution input and small byte scales
        // accumulate instead of rounding every event away or forcing a large jump.
        desired = std::clamp(desired * std::pow(fine ? FineWheelZoomStep : WheelZoomStep,
                                           -std::clamp(notches, -12.0, 12.0)),
            static_cast<double>(minimum),
            static_cast<double>(maximum));
        last = std::clamp<uint64_t>(static_cast<uint64_t>(std::llround(desired)), minimum, maximum);
        return last;
    }
};

struct WheelState {
    std::array<WheelScale, 2> scales;
    int target = -1;
};

double WheelNotches(const SDL_MouseWheelEvent& event) {
    double notches = event.y;
#if SDL_VERSION_ATLEAST(2, 0, 18)
    if (event.preciseY != 0)
        notches = event.preciseY;
#endif
    return event.direction == SDL_MOUSEWHEEL_FLIPPED ? -notches : notches;
}

enum class ZoomTarget
{
    Context,
    Detail
};

Request ZoomNavigation(Request request,
    ZoomTarget target,
    uint64_t limit,
    uint64_t maxBin,
    double notches,
    bool fine,
    WheelState& state) {
    if (!limit || !std::isfinite(notches) || notches == 0)
        return request;
    if (target == ZoomTarget::Detail) {
        // A child gesture can repair/clamp its own range, never reframe its parent.
        // If a changing process map invalidated the parent, let the map worker
        // normalize it rather than publishing a parent change from this gesture.
        if (request.contextBytes < std::min<uint64_t>(PixelCount, limit) ||
            request.contextBytes > limit || request.contextBase > limit - request.contextBytes)
            return request;
        uint64_t maximum =
            request.contextBytes == limit
                ? maxBin
                : std::min(maxBin,
                      std::max<uint64_t>(1, (request.contextBytes + PixelCount - 1) / PixelCount));
        request.bin = std::clamp<uint64_t>(request.bin, 1, maximum);
        uint64_t oldSpan = DetailSpan(request, limit);
        request.base = std::clamp(request.base,
            request.contextBase,
            request.contextBase + request.contextBytes - oldSpan);
        uint64_t middle = request.base + oldSpan / 2;
        uint64_t bin = state.scales[1].Apply(request.bin, 1, maximum, notches, fine);
        uint64_t span = std::min(request.contextBytes, bin * PixelCount);
        uint64_t base = middle > span / 2 ? middle - span / 2 : 0;
        request.base = std::clamp(
            base, request.contextBase, request.contextBase + request.contextBytes - span);
        request.bin = bin;
        request.overview = false;
        return request;
    }
    request = FitNavigation(request, limit);
    uint64_t span = state.scales[0].Apply(
        request.contextBytes, std::min<uint64_t>(PixelCount, limit), limit, notches, fine);
    if (span == request.contextBytes)
        return request;
    uint64_t middle = request.contextBase + request.contextBytes / 2;
    uint64_t base = middle > span / 2 ? middle - span / 2 : 0;
    return ChooseContext(request, std::min(base, limit - span), span, limit);
}

enum class NavDragKind
{
    None,
    Move,
    Top,
    Bottom,
    NewRange
};

struct NavigationRange {
    uint64_t domainBase;
    uint64_t domainBytes;
    uint64_t base;
    uint64_t bytes;
};

struct NavigationPixels {
    int top;
    int bottom;
};

NavigationRange NavigationRangeFor(const Request& request, int bar, uint64_t limit) {
    return bar == 0 ? NavigationRange{0, limit, request.contextBase, request.contextBytes}
                    : NavigationRange{request.contextBase,
                          request.contextBytes,
                          request.base,
                          DetailSpan(request, limit)};
}

Request DisplayedNavigationRequest(const Frame& frame, const Request& pending, int bar) {
    if (pending.packed != frame.request.packed) {
        return frame.request;
    }
    const auto displayed = RebaseNavigation(pending, frame.map);
    // The overview domain is fixed. The selection strip can show pending input
    // as soon as its parent domain matches the displayed texture.
    if (bar == 1 && (displayed.contextBase != frame.request.contextBase ||
                        displayed.contextBytes != frame.request.contextBytes)) {
        return frame.request;
    }
    return FitNavigation(displayed, Limit(*frame.map, displayed.packed));
}

NavigationPixels NavigationPixelBounds(const NavigationRange& range) {
    if (!range.domainBytes)
        return {0, CanvasHeight};
    double top =
        (range.base - range.domainBase) * static_cast<double>(CanvasHeight) / range.domainBytes;
    double bottom = (range.base + range.bytes - range.domainBase) *
                    static_cast<double>(CanvasHeight) / range.domainBytes;
    int height =
        std::clamp(static_cast<int>(std::ceil(bottom) - std::floor(top)), 12, CanvasHeight);
    int first = std::clamp(
        static_cast<int>(std::floor((top + bottom - height) / 2)), 0, CanvasHeight - height);
    return {first, first + height};
}

NavDragKind NavigationHit(const NavigationRange& range, double y) {
    auto pixels = NavigationPixelBounds(range);
    double topDistance = std::abs(y - pixels.top), bottomDistance = std::abs(y - pixels.bottom);
    double edge = std::min(6.0, (pixels.bottom - pixels.top) / 3.0);
    if (topDistance <= edge || bottomDistance <= edge)
        return topDistance <= bottomDistance ? NavDragKind::Top : NavDragKind::Bottom;
    return y > pixels.top && y < pixels.bottom ? NavDragKind::Move : NavDragKind::NewRange;
}

int NavigationBarAt(double x, double y) {
    if (y < CanvasY - 7 || y > CanvasY + CanvasHeight + 7)
        return -1;
    if (x >= OverviewX - 4 && x <= OverviewX + StripWidth + 4)
        return 0;
    if (x >= SelectionX - 4 && x <= SelectionX + StripWidth + 4)
        return 1;
    return -1;
}

Request WheelAtPoint(Request request,
    ContentPoint point,
    uint64_t limit,
    uint64_t maxBin,
    double notches,
    bool fine,
    WheelState& state) {
    // Match click/drag hit testing, including the border and resize handles.
    int bar = NavigationBarAt(point.x, point.y);
    bool canvas = InCanvas(point);
    if (bar < 0 && !canvas)
        return request;
    int target = bar < 0 ? 2 : bar;
    if (target != state.target) {
        state = {};
        state.target = target;
    }
    // The canvas and the selection strip both control the same detail range.
    const auto zoomTarget = bar == 0 ? ZoomTarget::Context : ZoomTarget::Detail;
    return ZoomNavigation(request, zoomTarget, limit, maxBin, notches, fine, state);
}

struct CanvasDrag {
    bool active = false;
    float anchorY = 0;
    Request seed;
    uint64_t limit = 0;
};

CanvasDrag BeginCanvasDrag(Request request, ContentPoint point, uint64_t limit) {
    if (!InCanvas(point) || !limit || !request.bin)
        return {};
    uint64_t span = DetailSpan(request, limit);
    if (request.contextBytes < span || request.contextBytes > limit ||
        request.contextBase > limit - request.contextBytes || request.base < request.contextBase ||
        request.base > request.contextBase + request.contextBytes - span)
        return {};
    return {span < request.contextBytes, point.y, request, limit};
}

Request DragCanvas(const CanvasDrag& drag, float mouseY) {
    Request request = drag.seed;
    if (!drag.active || !std::isfinite(mouseY))
        return request;
    uint64_t span = DetailSpan(request, drag.limit);
    uint64_t low = request.contextBase, high = low + request.contextBytes - span;
    // Grab the image: dragging down reveals earlier addresses. Use the press
    // position as the anchor so small motions never accumulate rounding drift.
    double rows = std::round(static_cast<double>(drag.anchorY) - mouseY);
    double base = static_cast<double>(request.base) + rows * CanvasWidth * request.bin;
    request.base = static_cast<uint64_t>(
        std::clamp(base, static_cast<double>(low), static_cast<double>(high)));
    if (request.base != drag.seed.base)
        request.overview = false;
    return request;
}

bool CanvasDragMatches(const CanvasDrag& drag, const Request& request, uint64_t limit) {
    return drag.active && request.bin == drag.seed.bin && request.packed == drag.seed.packed &&
           request.contextBase == drag.seed.contextBase &&
           request.contextBytes == drag.seed.contextBytes && request.contextBytes <= limit &&
           request.contextBase <= limit - request.contextBytes;
}

struct NavigationDrag {
    int bar = -1;
    int anchorY = 0;
    NavDragKind kind = NavDragKind::None;
    Request seed;
    NavigationRange range{};
    uint64_t limit = 0;
};

NavigationDrag BeginNavigationDrag(Request request, int bar, int y, uint64_t limit) {
    request = FitNavigation(request, limit);
    auto range = NavigationRangeFor(request, bar, limit);
    return {bar, std::clamp(y, 0, CanvasHeight), NavigationHit(range, y), request, range, limit};
}

Request DragNavigation(const NavigationDrag& drag, int mouseY) {
    Request request = drag.seed;
    request.overview = false;
    const auto& range = drag.range;
    int y = std::max(0, std::min(CanvasHeight, mouseY));
    int64_t delta = static_cast<int64_t>(
        std::llround((y - drag.anchorY) * static_cast<double>(range.domainBytes) / CanvasHeight));
    uint64_t domainEnd = range.domainBase + range.domainBytes, end = range.base + range.bytes;
    auto clamp = [](int64_t value, uint64_t low, uint64_t high) {
        return static_cast<uint64_t>(
            std::clamp<int64_t>(value, static_cast<int64_t>(low), static_cast<int64_t>(high)));
    };
    uint64_t minimum = std::min<uint64_t>(PixelCount, range.domainBytes);
    if (drag.kind == NavDragKind::Move) {
        uint64_t base = clamp(
            static_cast<int64_t>(range.base) + delta, range.domainBase, domainEnd - range.bytes);
        if (drag.bar == 0) {
            // Translate the nested selection by the actual clamped displacement.
            int64_t shift = static_cast<int64_t>(base) - static_cast<int64_t>(range.base);
            request.contextBase = base;
            request.base = static_cast<uint64_t>(static_cast<int64_t>(request.base) + shift);
        } else
            request.base = base;
        return FitNavigation(request, drag.limit);
    }
    if (drag.kind == NavDragKind::Top || drag.kind == NavDragKind::Bottom) {
        uint64_t base = range.base;
        if (drag.kind == NavDragKind::Top)
            base = y == 0
                       ? range.domainBase
                       : clamp(static_cast<int64_t>(base) + delta, range.domainBase, end - minimum);
        else
            end = y == CanvasHeight
                      ? domainEnd
                      : clamp(static_cast<int64_t>(end) + delta, base + minimum, domainEnd);
        if (drag.bar == 0)
            return ChooseContext(request, base, end - base, drag.limit);
        if (base == range.domainBase && end == domainEnd) {
            request.base = base;
            request.bin = std::max<uint64_t>(1, (range.domainBytes + PixelCount - 1) / PixelCount);
            return FitNavigation(request, drag.limit);
        }
        // The canvas has N pixels and an integer byte scale. Snap its span,
        // holding the opposite edge fixed instead of letting it drift.
        uint64_t available =
            drag.kind == NavDragKind::Top ? end - range.domainBase : domainEnd - base;
        uint64_t maxBin = std::max<uint64_t>(1, available / PixelCount);
        request.bin = std::clamp<uint64_t>((end - base + PixelCount / 2) / PixelCount, 1, maxBin);
        uint64_t span = std::min(request.bin * PixelCount, available);
        request.base = drag.kind == NavDragKind::Top ? end - span : base;
        return FitNavigation(request, drag.limit);
    }
    if (drag.kind == NavDragKind::NewRange) {
        if (std::abs(y - drag.anchorY) < 3) {
            uint64_t anchor = range.domainBase +
                              static_cast<uint64_t>(std::clamp(drag.anchorY, 0, CanvasHeight)) *
                                  range.domainBytes / CanvasHeight;
            uint64_t base = anchor > range.bytes / 2 ? anchor - range.bytes / 2 : 0;
            base = std::max(range.domainBase, std::min(base, domainEnd - range.bytes));
            if (drag.bar == 0)
                return ChooseContext(request, base, range.bytes, drag.limit);
            request.base = base;
            return FitNavigation(request, drag.limit);
        }
        int first = std::min(y, drag.anchorY), last = std::max(y, drag.anchorY);
        uint64_t base = range.domainBase + static_cast<uint64_t>(std::max(0, first)) *
                                               range.domainBytes / CanvasHeight;
        uint64_t bytes = std::max<uint64_t>(
            1, static_cast<uint64_t>(last - first) * range.domainBytes / CanvasHeight);
        return drag.bar == 0 ? ChooseContext(request, base, bytes, drag.limit)
                             : ChooseDetail(request, base, bytes, drag.limit);
    }
    return request;
}

const Region* At(const Map& map, uint64_t address) {
    auto next = std::upper_bound(
        map.regions.begin(), map.regions.end(), address, [](uint64_t value, const Region& r) {
            return value < r.base;
        });
    if (next == map.regions.begin())
        return nullptr;
    --next;
    return address < next->base + next->size ? &*next : nullptr;
}

std::vector<uint8_t> Flags(const Map& map, const Request& request) {
    std::vector<uint8_t> flags(PixelCount, 0);
    uint64_t end = DetailEnd(request, Limit(map, request.packed));
    for (const auto& r : map.regions) {
        if (request.packed && r.state != MEM_COMMIT)
            continue;
        uint64_t position = Position(r, request.packed);
        if (position >= end)
            break;
        if (position + r.size <= request.base)
            continue;
        uint8_t flag = r.readable               ? ReadableFlag
                       : r.state == MEM_RESERVE ? ReservedFlag
                       : r.state == MEM_COMMIT  ? ProtectedFlag
                                                : 0;
        if (!flag)
            continue;
        size_t first =
            static_cast<size_t>((std::max(position, request.base) - request.base) / request.bin);
        size_t last = static_cast<size_t>(
            (std::min(position + r.size, end) - 1 - request.base) / request.bin);
        for (size_t i = first; i <= last; ++i)
            flags[i] |= flag;
    }
    return flags;
}
// At overview resolution a pixel can cross region boundaries. Give its hue to
// the readable region covering the largest portion, or the largest committed
// region when no bytes can be sampled. Byte brightness uses every byte read.
std::vector<MemoryRole> PixelRoles(const Map& map, const Request& request) {
    std::vector<MemoryRole> roles(PixelCount, MemoryRole::Private);
    std::vector<uint64_t> largest(PixelCount, 0);
    std::vector<uint8_t> sampled(PixelCount, 0);
    uint64_t end = DetailEnd(request, Limit(map, request.packed));
    for (const auto& r : map.regions) {
        if (r.state != MEM_COMMIT)
            continue;
        uint64_t position = Position(r, request.packed);
        if (position >= end)
            break;
        if (position + r.size <= request.base)
            continue;
        uint64_t start = std::max(position, request.base), stop = std::min(position + r.size, end);
        while (start < stop) {
            size_t index = static_cast<size_t>((start - request.base) / request.bin);
            uint64_t next = std::min(stop, request.base + (index + 1) * request.bin);
            if ((!sampled[index] || r.readable) &&
                ((r.readable && !sampled[index]) || next - start > largest[index])) {
                largest[index] = next - start;
                roles[index] = r.role;
                sampled[index] = r.readable;
            }
            start = next;
        }
    }
    return roles;
}

uint32_t SumBytes(const uint8_t* bytes, size_t size) {
    __m128i sum = _mm_setzero_si128(), zero = _mm_setzero_si128();
    size_t i = 0;
    for (; i + 16 <= size; i += 16)
        sum = _mm_add_epi64(
            sum, _mm_sad_epu8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes + i)), zero));
    alignas(16) uint64_t lanes[2];
    _mm_store_si128(reinterpret_cast<__m128i*>(lanes), sum);
    uint32_t result = static_cast<uint32_t>(lanes[0] + lanes[1]);
    for (; i < size; ++i)
        result += bytes[i];
    return result;
}

uint32_t NowMs() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
            .count());
}

uint64_t ViewLayout(const Map& map, const Request& request) {
    uint64_t result = 14695981039346656037ull;
    auto mix = [&](uint64_t value) {
        result = (result ^ value) * 1099511628211ull;
    };
    mix(request.base);
    mix(request.bin);
    mix(request.packed);
    uint64_t end = DetailEnd(request, Limit(map, request.packed));
    mix(end);
    for (const auto& r : map.regions) {
        if (request.packed && r.state != MEM_COMMIT)
            continue;
        uint64_t position = Position(r, request.packed);
        if (position >= end)
            break;
        if (position + r.size <= request.base)
            continue;
        mix(position);
        mix(r.base);
        mix(r.size);
        mix(r.allocation);
    }
    return result;
}

unsigned CountBits(unsigned value) {
    value -= (value >> 1) & 0x5555u;
    value = (value & 0x3333u) + ((value >> 2) & 0x3333u);
    value = (value + (value >> 4)) & 0x0F0Fu;
    return (value + (value >> 8)) & 0x1Fu;
}

size_t OverviewRow(uint64_t position, uint64_t limit) {
    return limit ? static_cast<size_t>(
                       std::min<uint64_t>(CanvasHeight - 1, position * CanvasHeight / limit))
                 : 0;
}

uint64_t OverviewLayout(const Map& map) {
    uint64_t result = 14695981039346656037ull;
    for (const auto& r : map.regions)
        if (r.state == MEM_COMMIT) {
            for (auto value : {r.base, r.size, r.allocation})
                result = (result ^ value) * 1099511628211ull;
        }
    return result;
}

void MarkMiniByte(MiniChanges& changes, const Map& map, const Region& region, uint64_t actual) {
    ++changes.packed[OverviewRow(region.position + actual - region.base, map.committed)];
    ++changes.addresses[OverviewRow(actual, map.limit)];
    ++changes.bytes;
    uint64_t position = changes.contextPacked ? region.position + actual - region.base : actual;
    if (changes.contextBytes && position >= changes.contextBase &&
        position - changes.contextBase < changes.contextBytes)
        ++changes.context[OverviewRow(position - changes.contextBase, changes.contextBytes)];
}

void MarkMiniMask(
    MiniChanges& changes, const Map& map, const Region& region, uint64_t actual, unsigned mask) {
    uint64_t packed = region.position + actual - region.base;
    size_t row = OverviewRow(packed, map.committed), addressRow = OverviewRow(actual, map.limit);
    uint64_t position = changes.contextPacked ? packed : actual;
    bool outside = !changes.contextBytes || position + 15 < changes.contextBase ||
                   position >= changes.contextBase + changes.contextBytes;
    bool inside = changes.contextBytes && position >= changes.contextBase &&
                  position + 15 < changes.contextBase + changes.contextBytes;
    bool sameContext =
        outside ||
        (inside && OverviewRow(position - changes.contextBase, changes.contextBytes) ==
                       OverviewRow(position + 15 - changes.contextBase, changes.contextBytes));
    if (row == OverviewRow(packed + 15, map.committed) &&
        addressRow == OverviewRow(actual + 15, map.limit) && sameContext) {
        unsigned count = CountBits(mask);
        changes.packed[row] += count;
        changes.addresses[addressRow] += count;
        changes.bytes += count;
        if (inside)
            changes.context[OverviewRow(position - changes.contextBase, changes.contextBytes)] +=
                count;
    } else
        for (unsigned bit = 0; bit < 16; ++bit)
            if (mask & (1u << bit))
                MarkMiniByte(changes, map, region, actual + bit);
}
// Stable samples in every readable overview band, plus allocator boundaries
// and the upper stack pages. Comparing the same addresses on the next pass
// keeps zooming and shifting region layouts from producing spurious flashes.
class OverviewSampler {
    static constexpr uint64_t Budget = 4 * 1024 * 1024;
    struct Probe {
        Region region;
        uint64_t address;
        size_t size;
    };
    struct CachedSample {
        std::vector<uint8_t> bytes;
        uint64_t allocation = 0, generation = 0;
        size_t valid = 0;
    };
    std::vector<Probe> probes;
    std::unordered_map<uint64_t, CachedSample> cache;
    std::shared_ptr<const Map> prepared;
    uint64_t generation = 0, planned = 0, layout = 0;
    std::array<uint32_t, CanvasHeight> lastPacked{}, lastAddresses{};
    std::array<uint32_t, CanvasHeight> lastContext{};
    uint64_t contextBase = 0, contextBytes = 0;
    bool contextPacked = true;
    uint64_t changedTotal = 0;
    Clock::time_point previous{};
    void Prepare(std::shared_ptr<const Map> map) {
        probes.clear();
        planned = 0;
        auto add = [&](const Region& r, uint64_t address, size_t size) {
            if (!size || planned + size > Budget || probes.size() >= 4096)
                return;
            probes.push_back({r, address, size});
            planned += size;
        };
        std::array<const Region*, CanvasHeight> selected{};
        std::array<uint64_t, CanvasHeight> widths{};
        auto priority = [](MemoryRole role) {
            return role == MemoryRole::Stack     ? 5
                   : role == MemoryRole::Heap    ? 4
                   : role == MemoryRole::Private ? 3
                   : role == MemoryRole::Mapped  ? 2
                                                 : 1;
        };
        for (const auto& r : map->regions)
            if (r.readable && r.state == MEM_COMMIT) {
                size_t first = OverviewRow(r.position, map->committed),
                       last = OverviewRow(r.position + r.size - 1, map->committed);
                for (size_t row = first; row <= last; ++row) {
                    uint64_t start = std::max(
                        r.position, (row * map->committed + CanvasHeight - 1) / CanvasHeight);
                    uint64_t end = std::min(r.position + r.size,
                        ((row + 1) * map->committed + CanvasHeight - 1) / CanvasHeight);
                    if (end <= start)
                        continue;
                    uint64_t width = end - start;
                    if (!selected[row] || priority(r.role) > priority(selected[row]->role) ||
                        (priority(r.role) == priority(selected[row]->role) &&
                            width > widths[row])) {
                        selected[row] = &r;
                        widths[row] = width;
                    }
                }
            }
        for (size_t row = 0; row < CanvasHeight; ++row)
            if (selected[row]) {
                const auto& r = *selected[row];
                uint64_t start =
                    std::max(r.position, (row * map->committed + CanvasHeight - 1) / CanvasHeight);
                uint64_t end = std::min(r.position + r.size,
                    ((row + 1) * map->committed + CanvasHeight - 1) / CanvasHeight);
                size_t size = static_cast<size_t>(std::min<uint64_t>(4096, end - start));
                uint64_t address = r.base + start - r.position + (end - start - size) / 2;
                uint64_t aligned = address & ~uint64_t(4095);
                if (aligned >= r.base + start - r.position)
                    address = aligned;
                add(r, address, size);
            }
        for (const auto& r : map->regions)
            if (r.readable && r.state == MEM_COMMIT) {
                if (r.role == MemoryRole::Stack) {
                    size_t size = static_cast<size_t>(std::min<uint64_t>(32768, r.size));
                    add(r, r.base + r.size - size, size);
                } else if (r.role == MemoryRole::Heap) {
                    size_t size = static_cast<size_t>(std::min<uint64_t>(1024, r.size));
                    add(r, r.base, size);
                    if (r.size > size)
                        add(r, r.base + r.size - size, size);
                }
            }
        // Unconfirmed segment-heap arenas and custom allocators are private.
        // Sample their boundaries as well as the evenly spaced overview bands.
        for (const auto& r : map->regions)
            if (r.readable && r.state == MEM_COMMIT && r.role == MemoryRole::Private) {
                size_t size = static_cast<size_t>(std::min<uint64_t>(256, r.size));
                add(r, r.base, size);
                if (r.size > size)
                    add(r, r.base + r.size - size, size);
            }
        std::sort(probes.begin(), probes.end(), [](const Probe& a, const Probe& b) {
            return a.address < b.address;
        });
        std::vector<Probe> merged;
        for (const auto& probe : probes) {
            if (!merged.empty() && merged.back().region.base == probe.region.base &&
                probe.address <= merged.back().address + merged.back().size) {
                auto& last = merged.back();
                last.size = static_cast<size_t>(
                    std::max(last.address + last.size, probe.address + probe.size) - last.address);
            } else
                merged.push_back(probe);
        }
        probes.swap(merged);
        planned = 0;
        for (const auto& probe : probes)
            planned += probe.size;
        uint64_t newLayout = OverviewLayout(*map);
        if (newLayout != layout) {
            lastPacked.fill(0);
            lastAddresses.fill(0);
            lastContext.fill(0);
            layout = newLayout;
        }
        prepared = map;
    }

  public:
    std::shared_ptr<OverviewActivity> Sample(
        HANDLE process, std::shared_ptr<const Map> map, Request request = {}) {
        auto started = Clock::now();
        if (map != prepared)
            Prepare(map);
        auto activity = std::make_shared<OverviewActivity>();
        activity->map = map;
        activity->layout = layout;
        activity->probes = probes.size();
        activity->bytesPlanned = planned;
        MiniChanges changed;
        ++generation;
        if (contextBase != request.contextBase || contextBytes != request.contextBytes ||
            contextPacked != request.packed)
            lastContext.fill(0);
        contextBase = changed.contextBase = activity->contextBase = request.contextBase;
        contextBytes = changed.contextBytes = activity->contextBytes = request.contextBytes;
        contextPacked = changed.contextPacked = activity->contextPacked = request.packed;
        std::vector<uint8_t> buffer;
        for (const auto& probe : probes) {
            buffer.resize(probe.size);
            SIZE_T moved = 0;
            // Recheck protection: the map may have changed since this worker's
            // snapshot. In particular, never read a newly guarded stack page.
            MEMORY_BASIC_INFORMATION current{};
            if (VirtualQueryEx(
                    process, reinterpret_cast<void*>(probe.address), &current, sizeof(current)) &&
                IsReadablePage(current.State, current.Protect) &&
                reinterpret_cast<uint64_t>(current.AllocationBase) == probe.region.allocation) {
                size_t currentSize = static_cast<size_t>(std::min<uint64_t>(probe.size,
                    reinterpret_cast<uint64_t>(current.BaseAddress) + current.RegionSize -
                        probe.address));
                ReadProcessMemory(process,
                    reinterpret_cast<void*>(probe.address),
                    buffer.data(),
                    currentSize,
                    &moved);
            }
            auto& saved = cache[probe.address];
            size_t known =
                saved.generation + 1 == generation && saved.allocation == probe.region.allocation
                    ? std::min<size_t>(saved.valid, moved)
                    : 0;
            size_t i = 0;
            for (; i + 16 <= known; i += 16) {
                __m128i before =
                    _mm_loadu_si128(reinterpret_cast<const __m128i*>(saved.bytes.data() + i));
                __m128i after =
                    _mm_loadu_si128(reinterpret_cast<const __m128i*>(buffer.data() + i));
                unsigned mask =
                    static_cast<unsigned>(~_mm_movemask_epi8(_mm_cmpeq_epi8(before, after))) &
                    0xFFFFu;
                if (mask)
                    MarkMiniMask(changed, *map, probe.region, probe.address + i, mask);
            }
            for (; i < known; ++i)
                if (saved.bytes[i] != buffer[i])
                    MarkMiniByte(changed, *map, probe.region, probe.address + i);
            saved.bytes.assign(buffer.begin(), buffer.begin() + moved);
            saved.valid = moved;
            saved.allocation = probe.region.allocation;
            saved.generation = generation;
            activity->bytesRead += moved;
            if (probe.region.protect & PAGE_WRITECOMBINE)
                activity->writeCombineBytesRead += moved;
        }
        for (auto i = cache.begin(); i != cache.end();) {
            if (i->second.generation != generation)
                i = cache.erase(i);
            else {
                activity->cacheBytes += i->second.bytes.capacity();
                ++i;
            }
        }
        uint32_t now = NowMs();
        if (!now)
            now = 1;
        for (size_t row = 0; row < CanvasHeight; ++row) {
            if (changed.packed[row])
                lastPacked[row] = now;
            if (changed.addresses[row])
                lastAddresses[row] = now;
            if (changed.context[row])
                lastContext[row] = now;
        }
        activity->lastPacked = lastPacked;
        activity->lastAddresses = lastAddresses;
        activity->lastContext = lastContext;
        activity->samples = generation;
        activity->changedBytes = changed.bytes;
        activity->changedTotal = changedTotal += changed.bytes;
        activity->sampleMs =
            std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        activity->intervalMs =
            previous == Clock::time_point{}
                ? 0
                : std::chrono::duration<double, std::milli>(started - previous).count();
        previous = started;
        return activity;
    }
};

class ByteStripSampler {
    struct Row {
        Region region{};
        uint64_t address = 0;
        size_t size = 0, valid = 0;
        uint32_t background = FreeColor;
        MemoryRole backgroundRole = MemoryRole::Private;
        bool backgroundUnsampled = false;
        std::array<uint8_t, StripWidth> previous{};
        std::array<uint32_t, StripWidth> changed{};
        uint64_t oldAddress = 0, oldAllocation = 0;
    };
    std::vector<Row> rows = std::vector<Row>(CanvasHeight);
    std::shared_ptr<const Map> prepared;
    uint64_t base = 0, bytes = 0, layout = 0;
    bool packed = true;

  public:
    std::shared_ptr<StripFrame> Sample(HANDLE process,
        std::shared_ptr<const Map> map,
        uint64_t start,
        uint64_t size,
        bool dense,
        bool types) {
        uint64_t newLayout = OverviewLayout(*map);
        if (prepared != map || base != start || bytes != size || packed != dense) {
            bool moved = base != start || bytes != size || packed != dense || layout != newLayout;
            base = start;
            bytes = size;
            packed = dense;
            layout = newLayout;
            prepared = map;
            std::array<uint64_t, CanvasHeight> largest{}, largestUnsampled{};
            for (auto& row : rows) {
                row.size = 0;
                row.background = FreeColor;
                row.backgroundUnsampled = false;
                if (moved)
                    row.changed.fill(0);
            }
            for (const auto& r : map->regions) {
                if (packed && r.state != MEM_COMMIT)
                    continue;
                uint64_t position = Position(r, packed);
                uint64_t first = std::max(position, base),
                         last = std::min(position + r.size, base + bytes);
                if (last <= first || !bytes)
                    continue;
                size_t top = OverviewRow(first - base, bytes),
                       bottom = OverviewRow(last - 1 - base, bytes);
                for (size_t y = top; y <= bottom; ++y) {
                    uint64_t begin =
                        std::max(first, base + (y * bytes + CanvasHeight - 1) / CanvasHeight);
                    uint64_t end =
                        std::min(last, base + ((y + 1) * bytes + CanvasHeight - 1) / CanvasHeight);
                    if (end <= begin)
                        continue;
                    auto& row = rows[y];
                    if (!r.readable) {
                        if (r.state == MEM_COMMIT && end - begin > largestUnsampled[y]) {
                            largestUnsampled[y] = end - begin;
                            row.backgroundRole = r.role;
                            row.backgroundUnsampled = true;
                        } else if (r.state == MEM_RESERVE && !row.backgroundUnsampled)
                            row.background = ReserveColor;
                        continue;
                    }
                    if (end - begin > largest[y]) {
                        largest[y] = end - begin;
                        row.region = r;
                        row.size = static_cast<size_t>(std::min<uint64_t>(StripWidth, end - begin));
                        row.address = r.base + begin - position + (end - begin - row.size) / 2;
                    }
                }
            }
        }
        auto strip = std::make_shared<StripFrame>();
        strip->map = map;
        strip->base = base;
        strip->bytes = bytes;
        strip->packed = packed;
        strip->types = types;
        strip->layout = layout;
        strip->pixels.resize(StripWidth * CanvasHeight);
        strip->lastChanged.assign(StripWidth * CanvasHeight, 0);
        uint32_t now = NowMs();
        if (!now)
            now = 1;
        std::array<uint8_t, StripWidth> buffer{};
        for (size_t y = 0; y < CanvasHeight; ++y) {
            auto& row = rows[y];
            SIZE_T moved = 0;
            MEMORY_BASIC_INFORMATION current{};
            if (row.size &&
                VirtualQueryEx(
                    process, reinterpret_cast<void*>(row.address), &current, sizeof(current)) &&
                IsReadablePage(current.State, current.Protect) &&
                reinterpret_cast<uint64_t>(current.AllocationBase) == row.region.allocation) {
                size_t amount = static_cast<size_t>(std::min<uint64_t>(row.size,
                    reinterpret_cast<uint64_t>(current.BaseAddress) + current.RegionSize -
                        row.address));
                ReadProcessMemory(
                    process, reinterpret_cast<void*>(row.address), buffer.data(), amount, &moved);
            }
            bool known =
                row.address == row.oldAddress && row.region.allocation == row.oldAllocation;
            for (size_t x = 0; x < StripWidth; ++x) {
                size_t source = row.size ? x * row.size / StripWidth : 0,
                       index = y * StripWidth + x;
                if (source < moved) {
                    if (known && source < row.valid && buffer[source] != row.previous[source])
                        row.changed[x] = now;
                    else if (!known || source >= row.valid)
                        row.changed[x] = 0;
                    strip->pixels[index] = MemoryByteColor(row.region.role, buffer[source], types);
                    strip->lastChanged[index] = row.changed[x];
                } else {
                    strip->pixels[index] = row.size ? UnsampledColor(row.region.role, types, x, y)
                                           : row.backgroundUnsampled
                                               ? UnsampledColor(row.backgroundRole, types, x, y)
                                               : row.background;
                    row.changed[x] = 0;
                }
            }
            if (moved)
                std::memcpy(row.previous.data(), buffer.data(), moved);
            row.valid = moved;
            row.oldAddress = row.address;
            row.oldAllocation = row.region.allocation;
            strip->bytesRead += moved;
            if (row.region.protect & PAGE_WRITECOMBINE)
                strip->writeCombineBytesRead += moved;
        }
        return strip;
    }
};

bool StripMatchesView(const Frame& frame, const StripFrame& strip, bool fine) {
    return strip.base == (fine ? frame.request.contextBase : 0) &&
           strip.bytes ==
               (fine ? frame.request.contextBytes : Limit(*frame.map, frame.request.packed)) &&
           strip.packed == frame.request.packed && strip.types == frame.request.types &&
           strip.layout == frame.miniLayout;
}

std::shared_ptr<const StripFrame> RenderStripSnapshot(SDL_Renderer* renderer,
    SDL_Texture* texture,
    const SDL_Rect& destination,
    const Request& request,
    std::shared_ptr<const StripFrame> sampled,
    std::shared_ptr<const StripFrame>& displayed) {
    auto compatible = [&](const std::shared_ptr<const StripFrame>& strip) {
        return strip && strip->packed == request.packed && strip->types == request.types;
    };
    // Sampling runs independently of map refreshes and navigation. Keep the latest
    // completed snapshot visible while its replacement is being collected.
    if (compatible(sampled) && sampled != displayed &&
        SDL_UpdateTexture(texture, nullptr, sampled->pixels.data(), StripWidth * 4) == 0)
        displayed = sampled;
    if (!compatible(displayed))
        return nullptr;
    SDL_RenderCopy(renderer, texture, nullptr, &destination);
    return displayed;
}

void OverviewWorker(HANDLE process, Shared& shared) {
    OverviewSampler sampler;
    ByteStripSampler coarse, fine;
    while (!shared.stop.load() && !shared.targetEnded.load()) {
        auto started = Clock::now();
        auto frame = std::atomic_load(&shared.frame);
        if (frame) {
            Request request;
            {
                std::lock_guard<std::mutex> lock(shared.mutex);
                request = shared.request;
            }
            request = RebaseNavigation(request, frame->map);
            auto activity = sampler.Sample(process, frame->map, request);
            std::atomic_store(
                &shared.overviewActivity, std::shared_ptr<const OverviewActivity>(activity));
            auto strips = std::make_shared<DualStrips>();
            strips->overview = coarse.Sample(process,
                frame->map,
                0,
                Limit(*frame->map, request.packed),
                request.packed,
                request.types);
            strips->selection = fine.Sample(process,
                frame->map,
                request.contextBase,
                request.contextBytes,
                request.packed,
                request.types);
            std::atomic_store(&shared.strips, std::shared_ptr<const DualStrips>(strips));
        }
        auto remaining = std::chrono::milliseconds(100) - (Clock::now() - started);
        if (remaining > Clock::duration::zero())
            std::this_thread::sleep_for(remaining);
    }
}
// Keep one previous sample for the actual virtual pages currently in view.
// Unread/new bytes establish a baseline; they never count as changes. The
// comparison uses individual bytes, even when their average is unchanged.
class ChangeTracker {
    struct Page {
        std::array<uint8_t, 4096> bytes{};
        uint64_t allocation = 0, generation = 0;
        size_t start = 4096, end = 0, oldStart = 4096, oldEnd = 0;
    };
    std::unordered_map<uint64_t, Page> pages;
    uint64_t generation = 0;

  public:
    void Begin() {
        ++generation;
    }
    void Observe(uint64_t actual,
        uint64_t display,
        const Region& region,
        const uint8_t* bytes,
        size_t length,
        const Request& request,
        Frame& frame) {
        size_t offset = 0;
        while (offset < length) {
            uint64_t address = actual + offset, pageBase = address & ~uint64_t(4095);
            size_t inPage = static_cast<size_t>(address - pageBase);
            size_t amount = std::min(length - offset, 4096 - inPage);
            auto& page = pages[pageBase];
            if (page.generation != generation) {
                bool known =
                    page.generation + 1 == generation && page.allocation == region.allocation;
                page.oldStart = known ? page.start : 4096;
                page.oldEnd = known ? page.end : 0;
                page.start = 4096;
                page.end = 0;
                page.allocation = region.allocation;
                page.generation = generation;
            }
            size_t first = std::max(inPage, page.oldStart),
                   last = std::min(inPage + amount, page.oldEnd);
            auto mark = [&](size_t index) {
                size_t pixel = static_cast<size_t>(
                    (display + offset + index - inPage - request.base) / request.bin);
                ++frame.changes[pixel];
                ++frame.changedBytes;
                MarkMiniByte(
                    frame.miniChanges, *frame.map, region, actual + offset + index - inPage);
            };
            const bool unchanged =
                first == inPage && last == inPage + amount &&
                std::memcmp(page.bytes.data() + inPage, bytes + offset, amount) == 0;
            if (!unchanged && first < last) {
                size_t i = first;
                for (; i + 16 <= last; i += 16) {
                    const uint8_t* incoming = bytes + offset + i - inPage;
                    __m128i before =
                        _mm_loadu_si128(reinterpret_cast<const __m128i*>(page.bytes.data() + i));
                    __m128i after = _mm_loadu_si128(reinterpret_cast<const __m128i*>(incoming));
                    unsigned mask =
                        static_cast<unsigned>(~_mm_movemask_epi8(_mm_cmpeq_epi8(before, after))) &
                        0xFFFFu;
                    if (!mask)
                        continue;
                    uint64_t relative = display + offset + i - inPage - request.base;
                    size_t pixel = static_cast<size_t>(relative / request.bin);
                    if (relative / request.bin == (relative + 15) / request.bin) {
                        unsigned count = CountBits(mask);
                        frame.changes[pixel] += count;
                        frame.changedBytes += count;
                        MarkMiniMask(frame.miniChanges,
                            *frame.map,
                            region,
                            actual + offset + i - inPage,
                            mask);
                    } else
                        for (unsigned bit = 0; bit < 16; ++bit)
                            if (mask & (1u << bit))
                                mark(i + bit);
                }
                for (; i < last; ++i)
                    if (page.bytes[i] != bytes[offset + i - inPage])
                        mark(i);
            }
            if (!unchanged) {
                std::memcpy(page.bytes.data() + inPage, bytes + offset, amount);
            }
            // Successful reads within a page are normally contiguous. If a
            // transient read failure leaves a hole, retain only the last span.
            if (page.end < inPage) {
                page.start = inPage;
                page.end = inPage + amount;
            } else {
                page.start = std::min(page.start, inPage);
                page.end = std::max(page.end, inPage + amount);
            }
            offset += amount;
        }
    }
    uint64_t Finish() {
        for (auto i = pages.begin(); i != pages.end();) {
            if (i->second.generation != generation)
                i = pages.erase(i);
            else
                ++i;
        }
        return static_cast<uint64_t>(pages.size()) * sizeof(Page);
    }
};

double GlowLevel(uint32_t now, uint32_t changedAt) {
    if (!changedAt)
        return 0;
    uint32_t age = now - changedAt;
    if (age >= 900)
        return 0;
    double fade = 1.0 - age / 900.0;
    double flash = age < 240 ? 0.72 + 0.28 * std::cos(age * 6.283185307179586 / 120.0) : 1.0;
    return fade * fade * flash;
}

struct FrameWorkspace {
    std::vector<uint32_t> sums = std::vector<uint32_t>(PixelCount);
    std::vector<uint32_t> counts = std::vector<uint32_t>(PixelCount);
    std::vector<uint8_t> buffer;
    std::vector<uint8_t> flags;
    std::vector<MemoryRole> roles;
    std::array<const uint32_t*, static_cast<size_t>(MemoryRole::Count)> palettes{};
    std::shared_ptr<const Map> prepared;
    Request previous;
    uint64_t end = 0;

    void Prepare(std::shared_ptr<const Map> map, const Request& request) {
        const auto nextEnd = DetailEnd(request, Limit(*map, request.packed));
        // Published maps are immutable; their identity covers region metadata.
        const bool sameRange = prepared == map && previous.base == request.base &&
                               previous.bin == request.bin && previous.packed == request.packed &&
                               end == nextEnd;
        if (!sameRange) {
            flags = Flags(*map, request);
            roles.clear();
        }
        if (request.types && roles.empty()) {
            roles = PixelRoles(*map, request);
        }
        for (size_t i = 0; i < palettes.size(); ++i) {
            palettes[i] = MemoryBytePalette(static_cast<MemoryRole>(i), request.types).data();
        }
        if (request.bin > 1) {
            std::fill(sums.begin(), sums.end(), 0);
        }
        std::fill(counts.begin(), counts.end(), 0);
        buffer.resize(
            static_cast<size_t>(std::min<uint64_t>(ReadChunkBytes, nextEnd - request.base)));
        prepared = std::move(map);
        previous = request;
        end = nextEnd;
    }
};

std::shared_ptr<Frame> MakeFrame(HANDLE process,
    std::shared_ptr<const Map> map,
    Request request,
    ChangeTracker* tracker = nullptr,
    FrameWorkspace* workspace = nullptr) {
    std::unique_ptr<FrameWorkspace> localWorkspace;
    if (!workspace) {
        localWorkspace = std::make_unique<FrameWorkspace>();
        workspace = localWorkspace.get();
    }
    workspace->Prepare(map, request);
    const auto& flags = workspace->flags;
    const auto& roles = workspace->roles;
    const auto& palettes = workspace->palettes;
    auto colorsForPixel = [&](size_t index) {
        const auto role = request.types ? roles[index] : MemoryRole::Private;
        return palettes[static_cast<size_t>(role)];
    };
    auto frame = std::make_shared<Frame>();
    frame->map = map;
    frame->request = request;
    frame->byteView = request.packed || request.bin <= 256;
    uint64_t end = DetailEnd(request, Limit(*map, request.packed));
    frame->pixels.resize(PixelCount);
    frame->layout = ViewLayout(*map, request);
    frame->miniLayout = OverviewLayout(*map);
    frame->miniChanges.contextBase = request.contextBase;
    frame->miniChanges.contextBytes = request.contextBytes;
    frame->miniChanges.contextPacked = request.packed;
    if (tracker) {
        frame->changes.assign(PixelCount, 0);
        tracker->Begin();
    }
    for (size_t i = 0; i < PixelCount; ++i) {
        uint64_t address = request.base + i * request.bin;
        frame->pixels[i] = address >= end            ? PaddingColor
                           : flags[i] & ReadableFlag ? colorsForPixel(i)[128]
                           : flags[i] & ProtectedFlag
                               ? UnsampledColor(request.types ? roles[i] : MemoryRole::Private,
                                     request.types,
                                     i % CanvasWidth,
                                     i / CanvasWidth)
                           : flags[i] & ReservedFlag ? ReserveColor
                                                     : FreeColor;
    }
    if (AnchorUnavailable(request)) {
        for (size_t i = 0; i < PixelCount; ++i)
            frame->pixels[i] = UnsampledColor(
                request.anchor.role, request.types, i % CanvasWidth, i / CanvasWidth);
    }
    if (!frame->byteView || AnchorUnavailable(request)) {
        if (tracker)
            frame->changeCacheBytes = tracker->Finish();
        return frame;
    }
    auto started = Clock::now();
    auto& sums = workspace->sums;
    auto& counts = workspace->counts;
    auto& buffer = workspace->buffer;
    auto consume = [&](uint64_t address,
                       uint64_t actual,
                       const uint8_t* bytes,
                       size_t length,
                       const Region& region) {
        if (region.protect & PAGE_WRITECOMBINE)
            frame->writeCombineBytesRead += length;
        if (tracker)
            tracker->Observe(actual, address, region, bytes, length, request, *frame);
        if (request.bin == 1) {
            size_t first = static_cast<size_t>(address - request.base);
            const auto* colors = palettes[static_cast<size_t>(region.role)];
            for (size_t i = 0; i < length; ++i) {
                frame->pixels[first + i] = colors[bytes[i]];
                counts[first + i] = 1;
            }
            frame->bytesRead += length;
            return;
        }
        const uint64_t relative = address - request.base;
        size_t index = static_cast<size_t>(relative / request.bin);
        size_t offset = 0;
        auto append = [&](size_t count) {
            sums[index] += SumBytes(bytes + offset, count);
            counts[index] += static_cast<uint32_t>(count);
            offset += count;
            ++index;
        };
        const size_t partial = static_cast<size_t>(relative % request.bin);
        if (partial) {
            append(std::min<size_t>(length, static_cast<size_t>(request.bin) - partial));
        }
        while (length - offset >= request.bin) {
            append(static_cast<size_t>(request.bin));
        }
        if (offset < length) {
            append(length - offset);
        }
        frame->bytesRead += length;
    };
    for (const auto& region : map->regions) {
        if (request.packed && region.state != MEM_COMMIT)
            continue;
        uint64_t position = Position(region, request.packed);
        if (position >= end)
            break;
        if (!region.readable || position + region.size <= request.base)
            continue;
        uint64_t displayStart = std::max(position, request.base);
        uint64_t address = region.base + displayStart - position;
        uint64_t firstAddress = address;
        uint64_t stop = region.base + std::min(position + region.size, end) - position;
        frame->bytesWanted += stop - address;
        bool writeCombined = (region.protect & PAGE_WRITECOMBINE) != 0;
        auto wcStarted = writeCombined ? Clock::now() : Clock::time_point{};
        if (writeCombined)
            frame->writeCombineBytesWanted += stop - address;
        while (address < stop) {
            size_t amount = static_cast<size_t>(std::min<uint64_t>(buffer.size(), stop - address));
            SIZE_T moved = 0;
            // Check live protection before the bulk read as well as retries:
            // a formerly readable page may have become guarded or unmapped.
            MEMORY_BASIC_INFORMATION current{};
            if (!VirtualQueryEx(
                    process, reinterpret_cast<void*>(address), &current, sizeof(current))) {
                break;
            }
            const uint64_t currentEnd =
                reinterpret_cast<uint64_t>(current.BaseAddress) + current.RegionSize;
            amount = static_cast<size_t>(std::min<uint64_t>(amount, currentEnd - address));
            if (!IsReadablePage(current.State, current.Protect) ||
                reinterpret_cast<uint64_t>(current.AllocationBase) != region.allocation) {
                address += amount;
                continue;
            }
            ReadProcessMemory(
                process, reinterpret_cast<void*>(address), buffer.data(), amount, &moved);
            if (moved)
                consume(
                    displayStart + address - firstAddress, address, buffer.data(), moved, region);
            // A region can be decommitted between the query and read. Retry the
            // unread suffix page by page, checking current protection first.
            for (size_t offset = moved; offset < amount;) {
                MEMORY_BASIC_INFORMATION current{};
                SIZE_T valid = 0;
                const uint64_t retryAddress = address + offset;
                if (!VirtualQueryEx(process,
                        reinterpret_cast<void*>(retryAddress),
                        &current,
                        sizeof(current))) {
                    break;
                }
                const uint64_t retryEnd =
                    reinterpret_cast<uint64_t>(current.BaseAddress) + current.RegionSize;
                const size_t available = static_cast<size_t>(
                    std::min<uint64_t>(amount - offset, retryEnd - retryAddress));
                if (!IsReadablePage(current.State, current.Protect) ||
                    reinterpret_cast<uint64_t>(current.AllocationBase) != region.allocation) {
                    offset += available;
                    continue;
                }
                const size_t page = std::min<size_t>(available, 4096 - retryAddress % 4096);
                ReadProcessMemory(
                    process, reinterpret_cast<void*>(retryAddress), buffer.data(), page, &valid);
                if (valid) {
                    consume(displayStart + address - firstAddress + offset,
                        retryAddress,
                        buffer.data(),
                        valid,
                        region);
                }
                offset += page;
            }
            address += amount;
        }
        if (writeCombined)
            frame->writeCombineReadMs +=
                std::chrono::duration<double, std::milli>(Clock::now() - wcStarted).count();
    }
    for (size_t i = 0; i < PixelCount; ++i) {
        if (counts[i]) {
            if (request.bin != 1)
                frame->pixels[i] = colorsForPixel(i)[sums[i] / counts[i]];
        } else if (flags[i] & ReadableFlag)
            frame->pixels[i] = UnsampledColor(request.types ? roles[i] : MemoryRole::Private,
                request.types,
                i % CanvasWidth,
                i / CanvasWidth);
    }
    if (tracker) {
        frame->changeCacheBytes = tracker->Finish();
        for (auto count : frame->changes)
            if (count)
                ++frame->changedPixels;
    }
    frame->readMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    return frame;
}

uint32_t ChangeGlowColor(uint32_t color, double level) {
    unsigned red = (color >> 16) & 255;
    unsigned green = (color >> 8) & 255;
    unsigned blue = color & 255;
    unsigned peak = std::max({red, green, blue});
    if (!peak) {
        red = 36;
        green = 255;
        blue = 136;
        peak = 255;
    }
    // A pale core keeps even newly zeroed bytes visible. The additive texture
    // supplies the surrounding halo in both the canvas and navigation strips.
    auto channel = [&](unsigned value) {
        return static_cast<uint32_t>((140 + value * 115 / peak) * level * 0.9);
    };
    return 0xFF000000u | (channel(red) << 16) | (channel(green) << 8) | channel(blue);
}

bool GlowPixels(const Frame& frame, uint32_t now, std::vector<uint32_t>& pixels) {
    std::fill(pixels.begin(), pixels.end(), 0xFF000000u);
    if (!frame.request.glow || frame.lastChanged.empty())
        return false;
    bool active = false;
    for (size_t i = 0; i < PixelCount; ++i) {
        double level = GlowLevel(now, frame.lastChanged[i]);
        if (level <= 0)
            continue;
        pixels[i] = ChangeGlowColor(frame.pixels[i], level);
        active = true;
    }
    return active;
}

bool StripGlowPixels(const Frame& frame,
    const OverviewActivity* activity,
    const StripFrame& strip,
    bool fine,
    uint32_t now,
    std::vector<uint32_t>& pixels) {
    std::fill(pixels.begin(), pixels.end(), 0xFF000000u);
    if (!frame.request.glow)
        return false;
    bool active = false;
    // Row activity belongs to a particular coordinate system. An older texture
    // can retain its own byte-change glow, but must not inherit a newer map's rows.
    bool aligned = StripMatchesView(frame, strip, fine);
    const auto& exact = fine                   ? frame.contextLast
                        : frame.request.packed ? frame.miniLastPacked
                                               : frame.miniLastAddresses;
    for (size_t y = 0; y < CanvasHeight; ++y) {
        uint32_t rowTime = aligned ? exact[y] : 0;
        if (aligned && activity && activity->layout == frame.miniLayout &&
            (!fine || (activity->contextBase == frame.request.contextBase &&
                          activity->contextBytes == frame.request.contextBytes &&
                          activity->contextPacked == frame.request.packed))) {
            uint32_t sampled = fine                   ? activity->lastContext[y]
                               : frame.request.packed ? activity->lastPacked[y]
                                                      : activity->lastAddresses[y];
            if (sampled && (!rowTime || now - sampled < now - rowTime))
                rowTime = sampled;
        }
        for (size_t x = 0; x < StripWidth; ++x) {
            size_t index = y * StripWidth + x;
            uint32_t time = strip.lastChanged[index];
            if (rowTime && (!time || now - rowTime < now - time))
                time = rowTime;
            double level = GlowLevel(now, time);
            if (level <= 0)
                continue;
            pixels[index] = ChangeGlowColor(strip.pixels[index], level);
            active = true;
        }
    }
    return active;
}

void SaveFrameBitmap(const std::string& path, const Frame& frame) {
    auto pixels = frame.pixels;
    if (frame.request.packed)
        for (const auto& r : frame.map->regions) {
            if (r.state != MEM_COMMIT || r.position < frame.request.base ||
                r.position >= DetailEnd(frame.request, Limit(*frame.map, frame.request.packed)))
                continue;
            size_t index =
                static_cast<size_t>((r.position - frame.request.base) / frame.request.bin);
            uint32_t color = frame.request.types ? MemoryRoleColor(r.role) : 0xFF156F3Du;
            pixels[index] = color;
            if (index + CanvasWidth < PixelCount)
                pixels[index + CanvasWidth] = color;
        }
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
        pixels.data(), CanvasWidth, CanvasHeight, 32, CanvasWidth * 4, SDL_PIXELFORMAT_ARGB8888);
    if (!surface)
        throw std::runtime_error(SDL_GetError());
    int result = SDL_SaveBMP(surface, path.c_str());
    SDL_FreeSurface(surface);
    if (result)
        throw std::runtime_error(SDL_GetError());
}

void WriteReport(const std::string& path,
    DWORD pid,
    const Frame& frame,
    const char* status,
    const OverviewActivity* overview = nullptr,
    const DualStrips* strips = nullptr,
    bool fullscreen = false) {
    if (path.empty())
        return;
    const auto& m = *frame.map;
    uint64_t executable = 0, writeCombine = 0, guard = 0, noAccess = 0, noCache = 0;
    for (const auto& region : m.regions)
        if (region.state == MEM_COMMIT) {
            if (IsExecutable(region.protect))
                executable += region.size;
            if (region.protect & PAGE_WRITECOMBINE)
                writeCombine += region.size;
            if (region.protect & PAGE_GUARD)
                guard += region.size;
            if ((region.protect & 0xFF) == PAGE_NOACCESS)
                noAccess += region.size;
            if (region.protect & PAGE_NOCACHE)
                noCache += region.size;
        }
    size_t sampledActiveRows = 0;
    uint32_t now = NowMs();
    if (overview)
        for (auto time : frame.request.packed ? overview->lastPacked : overview->lastAddresses)
            if (time && now - time < 900)
                ++sampledActiveRows;
    std::ofstream out(path, std::ios::trunc);
    out << "{\n\"target_pid\":" << pid << ",\"status\":\"" << status << "\","
        << "\"address_space_bytes\":" << m.limit << ",\"committed_bytes\":" << m.committed
        << ",\"reserved_bytes\":" << m.reserved << ",\"readable_bytes\":" << m.readable
        << ",\"excluded_bytes\":" << m.excluded << ",\"regions\":" << m.regions.size()
        << ",\"readable_regions\":" << m.readableRegions << ",\"view_base\":" << frame.request.base
        << ",\"bytes_per_pixel\":" << frame.request.bin
        << ",\"selection_tracking\":\"virtual address\""
        << ",\"view_anchor_address\":"
        << (frame.request.anchor.active
                   ? frame.request.anchor.address
                   : AddressOfPosition(m, frame.request.base, frame.request.packed))
        << ",\"view_anchor_status\":\"" << AnchorStatusName(frame.request) << "\""
        << ",\"view_start_address\":"
        << (AnchorUnavailable(frame.request)
                   ? 0
                   : AddressOfPosition(m, frame.request.base, frame.request.packed))
        << ",\"context_start_address\":"
        << AddressOfPosition(m, frame.request.contextBase, frame.request.packed)
        << ",\"context_last_address\":"
        << AddressOfPosition(
               m, frame.request.contextBase + frame.request.contextBytes - 1, frame.request.packed)
        << ",\"byte_view\":" << (frame.byteView ? "true" : "false")
        << ",\"view_overview\":" << (frame.request.overview ? "true" : "false")
        << ",\"fullscreen\":" << (fullscreen ? "true" : "false")
        << ",\"navigation_levels\":3,\"context_base\":" << frame.request.contextBase
        << ",\"context_bytes\":" << frame.request.contextBytes
        << ",\"detail_span_bytes\":" << DetailSpan(frame.request, Limit(m, frame.request.packed))
        << ",\"detail_fills_context\":"
        << (frame.request.contextBytes &&
                       DetailSpan(frame.request, Limit(m, frame.request.packed)) ==
                           frame.request.contextBytes
                   ? "true"
                   : "false")
        << ",\"wheel_zoom_step_percent\":" << std::lround((WheelZoomStep - 1) * 100)
        << ",\"wheel_zoom_fine_step_percent\":" << std::lround((FineWheelZoomStep - 1) * 100)
        << ",\"selection_wheel_scope\":\"fixed parent range\""
        << ",\"canvas_wheel_action\":\"zoom within fixed context\""
        << ",\"canvas_drag_action\":\"pan within fixed context\""
        << ",\"strip_texture_sample_bytes\":"
        << (strips ? strips->overview->bytesRead + strips->selection->bytesRead : 0)
        << ",\"strip_texture_mode\":\"sampled byte values\""
        << ",\"strip_refresh_policy\":\"retain latest completed snapshot\""
        << ",\"write_combine_sampling\":\"normal sampling cadence\""
        << ",\"write_combine_bytes_read\":" << frame.writeCombineBytesRead
        << ",\"write_combine_bytes_requested\":" << frame.writeCombineBytesWanted
        << ",\"write_combine_read_ms\":" << frame.writeCombineReadMs
        << ",\"overview_write_combine_bytes_read\":"
        << (overview ? overview->writeCombineBytesRead : 0)
        << ",\"overview_strip_write_combine_bytes_read\":"
        << (strips ? strips->overview->writeCombineBytesRead : 0)
        << ",\"selection_strip_write_combine_bytes_read\":"
        << (strips ? strips->selection->writeCombineBytesRead : 0)
        << ",\"view_bytes_read\":" << frame.bytesRead << ",\"read_chunk_bytes\":" << ReadChunkBytes
        << ",\"navigation_request_wakeup\":\"immediate\""
        << ",\"view_bytes_requested\":" << frame.bytesWanted << ",\"read_ms\":" << frame.readMs
        << ",\"refresh_interval_ms\":" << frame.intervalMs
        << ",\"change_glow\":" << (frame.request.glow ? "true" : "false")
        << ",\"changed_bytes\":" << frame.changedBytes
        << ",\"changed_pixels\":" << frame.changedPixels << ",\"change_samples\":" << frame.samples
        << ",\"cumulative_changed_bytes\":" << frame.changedBytesTotal
        << ",\"previous_sample_cache_bytes\":" << frame.changeCacheBytes
        << ",\"glow_lifetime_ms\":900"
        << ",\"overview_sampling\":" << (overview ? "true" : "false")
        << ",\"overview_samples\":" << (overview ? overview->samples : 0)
        << ",\"overview_sample_bytes\":" << (overview ? overview->bytesRead : 0)
        << ",\"overview_probe_count\":" << (overview ? overview->probes : 0)
        << ",\"overview_sample_ms\":" << (overview ? overview->sampleMs : 0)
        << ",\"overview_sample_interval_ms\":" << (overview ? overview->intervalMs : 0)
        << ",\"overview_changed_bytes\":" << (overview ? overview->changedBytes : 0)
        << ",\"overview_cumulative_changed_bytes\":" << (overview ? overview->changedTotal : 0)
        << ",\"overview_cache_bytes\":" << (overview ? overview->cacheBytes : 0)
        << ",\"overview_active_rows\":" << sampledActiveRows
        << ",\"overview_coverage\":\"stratified readable bands, stack tops and allocator "
           "boundaries; sampled, not exhaustive\""
        << ",\"type_mode\":" << (frame.request.types ? "true" : "false")
        << ",\"type_query_ms\":" << m.typeQueryMs << ",\"type_cache_age_ms\":" << m.typeAgeMs
        << ",\"type_metadata_bytes_read\":" << m.typeMetadataBytes
        << ",\"identified_heap_roots\":" << m.heapRoots << ",\"identified_stacks\":" << m.stacks
        << ",\"segment_heap_layout_verified\":" << (m.segmentLayoutVerified ? "true" : "false")
        << ",\"identified_segment_heap_contexts\":" << m.segmentHeapContexts
        << ",\"identified_segment_heap_segments\":" << m.segmentHeapSegments
        << ",\"rejected_segment_heap_contexts\":" << m.rejectedSegmentContexts
        << ",\"threads\":" << m.threads << ",\"threads_unidentified\":" << m.failedThreads
        << ",\"detailed_x64_types_supported\":" << (m.detailedTypes ? "true" : "false")
        << ",\"type_categories\":[\"image\",\"mapped\",\"heap\",\"stack\",\"other_private\"]"
        << ",\"unsampled_rendering\":\"category-colored diagonal hatching\""
        << ",\"type_bytes\":{\"other_private\":"
        << m.roleBytes[static_cast<size_t>(MemoryRole::Private)]
        << ",\"heap\":" << m.roleBytes[static_cast<size_t>(MemoryRole::Heap)]
        << ",\"stack\":" << m.roleBytes[static_cast<size_t>(MemoryRole::Stack)]
        << ",\"image\":" << m.roleBytes[static_cast<size_t>(MemoryRole::Image)]
        << ",\"mapped\":" << m.roleBytes[static_cast<size_t>(MemoryRole::Mapped)] << "}"
        << ",\"memory_attributes_bytes\":{\"executable\":" << executable
        << ",\"write_combine\":" << writeCombine << ",\"guard\":" << guard
        << ",\"no_access\":" << noAccess << ",\"no_cache\":" << noCache << "}"
        << ",\"heap_coverage\":\"confirmed NT heaps and version-verified Segment Heap lists; "
           "custom allocators and unverified ranges can remain private\""
        << ",\"unallocated_address_gaps_folded\":" << (frame.request.packed ? "true" : "false")
        << ",\"all_committed_regions_in_address_order\":"
        << (frame.request.packed ? "true" : "false") << ",\"access\":\"query and read only\"\n}\n";
}

void WaitForRequest(Shared& shared, uint64_t sequence, std::chrono::milliseconds delay) {
    std::unique_lock<std::mutex> lock(shared.mutex);
    shared.requestChanged.wait_for(lock, delay, [&] {
        return shared.stop.load() || shared.request.sequence != sequence;
    });
}

void Worker(HANDLE process, DWORD pid, Shared& shared, const std::string& report) {
    std::shared_ptr<const Map> map;
    MemoryTypeAtlas atlas;
    auto nextTypes = Clock::time_point{}, typesUpdated = Clock::time_point{};
    auto nextMap = Clock::time_point{}, lastReport = Clock::time_point{}, previous = Clock::now();
    uint64_t previousSequence = UINT64_MAX;
    ChangeTracker changes;
    FrameWorkspace workspace;
    std::vector<uint32_t> lastChanged(PixelCount, 0);
    uint64_t lastLayout = 0;
    uint64_t lastMiniLayout = 0;
    std::array<uint32_t, CanvasHeight> miniLastPacked{}, miniLastAddresses{};
    std::array<uint32_t, CanvasHeight> contextLast{};
    Request contextHistory;
    uint64_t samples = 0, changedBytesTotal = 0;
    while (!shared.stop.load()) {
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(process, &exitCode) || exitCode != STILL_ACTIVE) {
            shared.targetEnded = true;
            break;
        }
        bool newMap = Clock::now() >= nextMap;
        if (newMap) {
            auto queried = QueryMap(process);
            if (queried->regions.empty()) {
                shared.targetEnded = true;
                break;
            }
            if (Clock::now() >= nextTypes) {
                atlas = QueryMemoryTypes(process, pid, PrivateRanges(*queried));
                typesUpdated = Clock::now();
                nextTypes = typesUpdated + std::chrono::seconds(2);
            }
            ApplyTypes(*queried,
                atlas,
                std::chrono::duration<double, std::milli>(Clock::now() - typesUpdated).count());
            map = queried;
            nextMap = Clock::now() + std::chrono::milliseconds(500);
        }
        Request request;
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            request = shared.request;
            request = RebaseNavigation(request, map);
            shared.request = request;
        }
        if (request.packed || request.bin <= 256 || newMap ||
            request.sequence != previousSequence) {
            auto frame = MakeFrame(process, map, request, &changes, &workspace);
            frame->samples = ++samples;
            frame->changedBytesTotal = changedBytesTotal += frame->changedBytes;
            if (frame->layout != lastLayout) {
                std::fill(lastChanged.begin(), lastChanged.end(), 0);
                lastLayout = frame->layout;
            }
            uint32_t now = NowMs();
            for (size_t i = 0; i < PixelCount; ++i)
                if (frame->changes[i])
                    lastChanged[i] = now ? now : 1;
            frame->lastChanged = lastChanged;
            if (frame->miniLayout != lastMiniLayout) {
                miniLastPacked.fill(0);
                miniLastAddresses.fill(0);
                contextLast.fill(0);
                lastMiniLayout = frame->miniLayout;
            }
            if (contextHistory.contextBase != request.contextBase ||
                contextHistory.contextBytes != request.contextBytes ||
                contextHistory.packed != request.packed)
                contextLast.fill(0);
            contextHistory = request;
            for (size_t row = 0; row < CanvasHeight; ++row) {
                if (frame->miniChanges.packed[row])
                    miniLastPacked[row] = now ? now : 1;
                if (frame->miniChanges.addresses[row])
                    miniLastAddresses[row] = now ? now : 1;
                if (frame->miniChanges.context[row])
                    contextLast[row] = now ? now : 1;
            }
            frame->miniLastPacked = miniLastPacked;
            frame->miniLastAddresses = miniLastAddresses;
            frame->contextLast = contextLast;
            frame->intervalMs =
                std::chrono::duration<double, std::milli>(Clock::now() - previous).count();
            previous = Clock::now();
            previousSequence = request.sequence;
            std::atomic_store(&shared.frame, std::shared_ptr<const Frame>(frame));
            if (Clock::now() - lastReport >= std::chrono::milliseconds(500)) {
                auto activity = std::atomic_load(&shared.overviewActivity);
                auto strips = std::atomic_load(&shared.strips);
                WriteReport(report,
                    pid,
                    *frame,
                    "running",
                    activity.get(),
                    strips.get(),
                    shared.fullscreen.load());
                lastReport = Clock::now();
            }
            auto delay =
                std::chrono::milliseconds(std::max<int>(16, static_cast<int>(frame->readMs * 2)));
            WaitForRequest(shared, request.sequence, delay);
        } else
            WaitForRequest(shared, request.sequence, std::chrono::milliseconds(8));
    }
    auto frame = std::atomic_load(&shared.frame);
    auto activity = std::atomic_load(&shared.overviewActivity);
    auto strips = std::atomic_load(&shared.strips);
    if (frame)
        WriteReport(report,
            pid,
            *frame,
            shared.targetEnded ? "target_closed" : "stopped",
            activity.get(),
            strips.get(),
            shared.fullscreen.load());
}

// Small, locally defined 5x7 font; the atlas avoids per-pixel draw calls.
const std::unordered_map<char, std::array<uint8_t, 7>> Glyphs = {
    {'A', {14, 17, 17, 31, 17, 17, 17}},
    {'B', {30, 17, 17, 30, 17, 17, 30}},
    {'C', {14, 17, 16, 16, 16, 17, 14}},
    {'D', {30, 17, 17, 17, 17, 17, 30}},
    {'E', {31, 16, 16, 30, 16, 16, 31}},
    {'F', {31, 16, 16, 30, 16, 16, 16}},
    {'G', {14, 17, 16, 23, 17, 17, 15}},
    {'H', {17, 17, 17, 31, 17, 17, 17}},
    {'I', {31, 4, 4, 4, 4, 4, 31}},
    {'J', {7, 2, 2, 2, 18, 18, 12}},
    {'K', {17, 18, 20, 24, 20, 18, 17}},
    {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'M', {17, 27, 21, 21, 17, 17, 17}},
    {'N', {17, 25, 25, 21, 19, 19, 17}},
    {'O', {14, 17, 17, 17, 17, 17, 14}},
    {'P', {30, 17, 17, 30, 16, 16, 16}},
    {'Q', {14, 17, 17, 17, 21, 18, 13}},
    {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}},
    {'T', {31, 4, 4, 4, 4, 4, 4}},
    {'U', {17, 17, 17, 17, 17, 17, 14}},
    {'V', {17, 17, 17, 17, 17, 10, 4}},
    {'W', {17, 17, 17, 21, 21, 21, 10}},
    {'X', {17, 17, 10, 4, 10, 17, 17}},
    {'Y', {17, 17, 10, 4, 4, 4, 4}},
    {'Z', {31, 1, 2, 4, 8, 16, 31}},
    {'x', {0, 0, 17, 10, 4, 10, 17}},
    {'0', {14, 17, 19, 21, 25, 17, 14}},
    {'1', {4, 12, 4, 4, 4, 4, 14}},
    {'2', {14, 17, 1, 2, 4, 8, 31}},
    {'3', {30, 1, 1, 14, 1, 1, 30}},
    {'4', {2, 6, 10, 18, 31, 2, 2}},
    {'5', {31, 16, 16, 30, 1, 1, 30}},
    {'6', {14, 16, 16, 30, 17, 17, 14}},
    {'7', {31, 1, 2, 4, 8, 8, 8}},
    {'8', {14, 17, 17, 14, 17, 17, 14}},
    {'9', {14, 17, 17, 15, 1, 1, 14}},
    {'.', {0, 0, 0, 0, 0, 6, 6}},
    {':', {0, 6, 6, 0, 6, 6, 0}},
    {'-', {0, 0, 0, 31, 0, 0, 0}},
    {'/', {1, 1, 2, 4, 8, 16, 16}},
    {'|', {4, 4, 4, 4, 4, 4, 4}},
    {'(', {2, 4, 8, 8, 8, 4, 2}},
    {')', {8, 4, 2, 2, 2, 4, 8}},
    {'+', {0, 4, 4, 31, 4, 4, 0}},
    {'<', {1, 2, 4, 8, 4, 2, 1}},
    {'>', {16, 8, 4, 2, 4, 8, 16}},
    {'=', {0, 31, 0, 31, 0, 0, 0}},
    {'_', {0, 0, 0, 0, 0, 0, 31}},
    {'?', {14, 17, 1, 2, 4, 0, 4}},
    {',', {0, 0, 0, 0, 6, 6, 4}}};
SDL_Texture* FontAtlas(SDL_Renderer* renderer) {
    std::vector<uint32_t> atlas(96 * 48, 0);
    for (const auto& glyph : Glyphs) {
        int index = glyph.first - 32, x = index % 16 * 6, y = index / 16 * 8;
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (glyph.second[row] & (1 << (4 - col)))
                    atlas[(y + row) * 96 + x + col] = 0xFFFFFFFFu;
    }
    SDL_Texture* texture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 96, 48);
    if (!texture)
        throw std::runtime_error(SDL_GetError());
    SDL_UpdateTexture(texture, nullptr, atlas.data(), 96 * 4);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return texture;
}

void Text(SDL_Renderer* renderer,
    SDL_Texture* font,
    int x,
    int y,
    std::string text,
    int scale = 2,
    int maxWidth = 0) {
    if (maxWidth > 0) {
        size_t maxChars = static_cast<size_t>(maxWidth / (6 * scale));
        if (text.size() > maxChars) {
            text.resize(maxChars);
            if (maxChars >= 3)
                text.replace(maxChars - 3, 3, "...");
        }
    }
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(text[i]);
        // Keep the conventional hexadecimal prefix in the otherwise uppercase font.
        if (!(ch == 'x' && i > 0 && text[i - 1] == '0'))
            ch = static_cast<unsigned char>(std::toupper(ch));
        if (ch >= 32 && ch < 128) {
            int index = ch - 32;
            SDL_Rect source{index % 16 * 6, index / 16 * 8, 5, 7},
                target{x, y, 5 * scale, 7 * scale};
            SDL_RenderCopy(renderer, font, &source, &target);
        }
        x += 6 * scale;
    }
}

void DrawToolbar(SDL_Renderer* renderer, SDL_Texture* font, const Request& request) {
    for (size_t i = 0; i < ToolbarActions.size(); ++i) {
        const char* label = "";
        bool active = false;
        switch (ToolbarActions[i]) {
            case ToolbarAction::Fullscreen:
                label = "F11";
                break;
            case ToolbarAction::All:
                label = "A ALL";
                active = request.packed && request.overview;
                break;
            case ToolbarAction::Types:
                label = "T COLOR";
                active = request.types;
                break;
            case ToolbarAction::Glow:
                label = request.glow ? "F GLOW" : "F OFF";
                active = request.glow;
                break;
            case ToolbarAction::None:
                continue;
        }
        const auto box = ToolbarRect(i);
        SDL_SetRenderDrawColor(renderer, 24, 49, 33, 255);
        SDL_RenderFillRect(renderer, &box);
        SDL_SetRenderDrawColor(
            renderer, active ? 123 : 35, active ? 217 : 78, active ? 151 : 49, 255);
        SDL_RenderDrawRect(renderer, &box);
        Text(renderer, font, box.x + 6, box.y + 5, label);
    }
}

int DrawLegendItem(SDL_Renderer* renderer,
    SDL_Texture* font,
    int x,
    const char* label,
    uint32_t color,
    bool hatched = false) {
    const uint8_t red = (color >> 16) & 255;
    const uint8_t green = (color >> 8) & 255;
    const uint8_t blue = color & 255;
    SDL_SetRenderDrawColor(
        renderer, hatched ? 24 : red, hatched ? 49 : green, hatched ? 33 : blue, 255);
    const SDL_Rect chip{x, CanvasY + CanvasHeight + 24, 12, 12};
    SDL_RenderFillRect(renderer, &chip);
    if (hatched) {
        SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
        for (int diagonal = 0; diagonal < chip.w + chip.h - 1; ++diagonal) {
            if (diagonal % 8 >= 2) {
                continue;
            }
            const int start = std::max(0, diagonal - chip.h + 1);
            const int end = std::min(chip.w - 1, diagonal);
            SDL_RenderDrawLine(renderer,
                chip.x + start,
                chip.y + diagonal - start,
                chip.x + end,
                chip.y + diagonal - end);
        }
        SDL_RenderDrawRect(renderer, &chip);
    }
    SDL_SetTextureColorMod(font, red, green, blue);
    Text(renderer, font, x + 16, CanvasY + CanvasHeight + 23, label);
    return x + 16 + static_cast<int>(std::strlen(label)) * 12 + 20;
}

void DrawMemoryLegend(SDL_Renderer* renderer, SDL_Texture* font, bool types) {
    int x = 8;
    if (types) {
        for (auto role : {MemoryRole::Image,
                 MemoryRole::Mapped,
                 MemoryRole::Heap,
                 MemoryRole::Stack,
                 MemoryRole::Private}) {
            x = DrawLegendItem(renderer, font, x, MemoryRoleName(role), MemoryRoleColor(role));
        }
        DrawLegendItem(renderer, font, x, "HATCH: UNSAMPLED", 0xFFB7EBC5u, true);
    } else {
        x = DrawLegendItem(
            renderer, font, x, "BYTE CONTENT", MemoryByteColor(MemoryRole::Private, 255, false));
        DrawLegendItem(renderer, font, x, "UNSAMPLED", ProtectedColor);
        SDL_SetTextureColorMod(font, 183, 235, 197);
    }
}

const char* Kind(const Region* region) {
    if (!region)
        return "OUTSIDE ADDRESS SPACE";
    if (region->state == MEM_FREE)
        return "FREE";
    if (region->state == MEM_RESERVE)
        return "RESERVED";
    return MemoryRoleName(region->role);
}

void UpdateRequest(
    Shared& shared, Request request, bool toggleTypes = false, bool toggleGlow = false) {
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        if (!request.coordinates)
            request.coordinates = shared.request.coordinates;
        const auto previous = request.coordinates
                                  ? RebaseNavigation(shared.request, request.coordinates)
                                  : shared.request;
        request.types = toggleTypes ? !shared.request.types : shared.request.types;
        request.glow = toggleGlow ? !shared.request.glow : shared.request.glow;
        if (!request.contextBytes && !request.overview && request.packed == previous.packed) {
            request.contextBase = previous.contextBase;
            request.contextBytes = previous.contextBytes;
        }
        if (request.coordinates && !toggleTypes && !toggleGlow)
            request = AnchorNavigation(request, request.coordinates);
        request.sequence = shared.request.sequence + 1;
        shared.request = request;
    }
    shared.requestChanged.notify_one();
}

bool TestFiveTypeClassification() {
    constexpr uint64_t base = 0x10000000, page = 4096;
    Map map;
    map.limit = base + 11 * page;
    map.committed = 11 * page;
    auto add = [&](DWORD kind, DWORD protect) {
        uint64_t position = map.regions.size() * page, address = base + position;
        map.regions.push_back({address,
            page,
            address,
            MEM_COMMIT,
            protect,
            kind,
            IsReadablePage(MEM_COMMIT, protect),
            position});
    };
    add(MEM_PRIVATE, PAGE_READWRITE);              // Confirmed heap.
    add(MEM_PRIVATE, PAGE_EXECUTE_READWRITE);      // Executable heap keeps its role.
    add(MEM_PRIVATE, PAGE_READWRITE | PAGE_GUARD); // Guarded stack keeps its role.
    add(MEM_IMAGE, PAGE_EXECUTE_READ);
    add(MEM_IMAGE, PAGE_NOACCESS);
    add(MEM_MAPPED, PAGE_EXECUTE_READ);
    add(MEM_MAPPED, PAGE_NOACCESS);
    add(MEM_PRIVATE, PAGE_EXECUTE_READWRITE); // Other private, not a sixth type.
    add(MEM_PRIVATE, PAGE_READWRITE | PAGE_WRITECOMBINE);
    add(MEM_PRIVATE, PAGE_READWRITE);
    add(MEM_PRIVATE, PAGE_NOACCESS);
    MemoryTypeAtlas atlas;
    atlas.heaps[base] = {base, 1, false};
    atlas.heaps[base + page] = {base + page, 2, false};
    atlas.stacks[base + 2 * page] = {base + 2 * page, base + 3 * page, 7};
    ApplyTypes(map, atlas);
    const std::array<MemoryRole, 11> expected{{MemoryRole::Heap,
        MemoryRole::Heap,
        MemoryRole::Stack,
        MemoryRole::Image,
        MemoryRole::Image,
        MemoryRole::Mapped,
        MemoryRole::Mapped,
        MemoryRole::Private,
        MemoryRole::Private,
        MemoryRole::Private,
        MemoryRole::Private}};
    auto roles = PixelRoles(map, {0, 1, 0, true, false, true});
    bool okay = true;
    for (size_t i = 0; i < expected.size(); ++i)
        okay = okay && map.regions[i].role == expected[i] && roles[i * page] == expected[i];
    uint64_t total = 0;
    for (uint64_t bytes : map.roleBytes)
        total += bytes;
    okay = okay && total == map.committed &&
           map.roleBytes[static_cast<size_t>(MemoryRole::Private)] == 4 * page &&
           PageAttributes(map.regions[3]) == "RX" && PageAttributes(map.regions[8]) == "RW WC" &&
           PageAttributes(map.regions[2]) == "RW GUARD / UNSAMPLED" &&
           Kind(&map.regions[4]) == std::string("IMAGE");
    // Successfully sampled bytes take precedence at a mixed boundary.
    auto mixed = PixelRoles(map, {4 * page, 6144, 0, true, false, true});
    okay = okay && mixed[0] == MemoryRole::Mapped;
    std::printf("Self-test five categories: mapping/roles independent of execution, cache and read "
                "status=%d\n",
        okay);
    return okay;
}

bool TestWriteCombineSampling() {
    constexpr size_t size = 8192;
    auto bytes = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE | PAGE_WRITECOMBINE));
    if (!bytes)
        return false;
    std::memset(bytes, 32, size / 2);
    std::memset(bytes + size / 2, 128, size / 2);
    uint64_t base = reinterpret_cast<uint64_t>(bytes);
    MEMORY_BASIC_INFORMATION state{};
    VirtualQuery(bytes, &state, sizeof(state));
    auto map = std::make_shared<Map>();
    map->limit = base + size;
    map->committed = size;
    map->regions = {{base,
        size,
        reinterpret_cast<uint64_t>(state.AllocationBase),
        state.State,
        state.Protect,
        state.Type,
        IsReadablePage(state.State, state.Protect)}};
    Request request{0, 1, 0, true, false, true};
    ChangeTracker tracker;
    ByteStripSampler stripSampler;
    OverviewSampler overviewSampler;
    auto frame = MakeFrame(GetCurrentProcess(), map, request, &tracker);
    auto strip = stripSampler.Sample(GetCurrentProcess(), map, 0, size, true, true);
    auto overview = overviewSampler.Sample(GetCurrentProcess(), map, request);
    bool okay = (state.Protect & PAGE_WRITECOMBINE) && map->regions[0].readable &&
                frame->bytesRead == size && frame->writeCombineBytesRead == size &&
                frame->writeCombineBytesWanted == size &&
                frame->pixels[0] == MemoryByteColor(MemoryRole::Private, 32, true) &&
                frame->pixels[size / 2] == MemoryByteColor(MemoryRole::Private, 128, true) &&
                frame->changedBytes == 0 && strip->bytesRead > 0 &&
                strip->writeCombineBytesRead == strip->bytesRead &&
                strip->pixels[0] == frame->pixels[0] &&
                overview->writeCombineBytesRead == overview->bytesRead && overview->bytesRead > 0 &&
                overview->changedBytes == 0;
    bytes[0] = 33;
    auto changed = MakeFrame(GetCurrentProcess(), map, request, &tracker);
    auto changedStrip = stripSampler.Sample(GetCurrentProcess(), map, 0, size, true, true);
    auto changedOverview = overviewSampler.Sample(GetCurrentProcess(), map, request);
    okay = okay && changed->changedBytes == 1 && changed->changes[0] == 1 &&
           changedStrip->lastChanged[0] != 0 && changedOverview->changedBytes > 0 &&
           changed->pixels[0] == MemoryByteColor(MemoryRole::Private, 33, true);
    VirtualFree(bytes, 0, MEM_RELEASE);
    std::printf("Self-test write combine: real WC allocation, main/strips/overview reads, byte "
                "colors and change glow=%d\n",
        okay);
    return okay;
}

bool TestStripRefresh() {
    constexpr size_t page = 4096;
    auto bytes =
        static_cast<uint8_t*>(VirtualAlloc(nullptr, 2 * page, MEM_RESERVE, PAGE_READWRITE));
    if (!bytes || !VirtualAlloc(bytes, page, MEM_COMMIT, PAGE_READWRITE))
        return false;
    std::memset(bytes, 96, page);
    uint64_t base = reinterpret_cast<uint64_t>(bytes);
    auto makeMap = [&](size_t size) {
        auto map = std::make_shared<Map>();
        map->limit = base + 2 * page;
        map->committed = size;
        map->regions = {{base, size, base, MEM_COMMIT, PAGE_READWRITE, MEM_PRIVATE, true}};
        return map;
    };
    SDL_Surface* surface =
        SDL_CreateRGBSurfaceWithFormat(0, StripWidth, CanvasHeight, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Renderer* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
    std::array<SDL_Texture*, 2> textures{};
    if (renderer)
        for (auto& texture : textures)
            texture = SDL_CreateTexture(renderer,
                SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_STREAMING,
                StripWidth,
                CanvasHeight);
    bool okay = renderer && textures[0] && textures[1];
    unsigned draws = 0, blankFrames = 0;
    if (okay) {
        SDL_Rect destination{0, 0, StripWidth, CanvasHeight};
        std::vector<uint32_t> rendered(StripWidth * CanvasHeight), glow(StripWidth * CanvasHeight);
        std::array<std::shared_ptr<const StripFrame>, 2> displayed{};
        auto draw = [&](size_t which,
                        const Request& request,
                        std::shared_ptr<const StripFrame> sampled,
                        bool visible = true) {
            SDL_SetRenderDrawColor(renderer, 0, 5, 2, 255);
            SDL_RenderClear(renderer);
            auto chosen = RenderStripSnapshot(
                renderer, textures[which], destination, request, sampled, displayed[which]);
            SDL_RenderPresent(renderer);
            ++draws;
            if (visible && !chosen)
                ++blankFrames;
            if (SDL_RenderReadPixels(
                    renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, rendered.data(), StripWidth * 4))
                return false;
            return visible ? chosen && rendered == chosen->pixels
                           : !chosen &&
                                 std::all_of(rendered.begin(), rendered.end(), [](uint32_t color) {
                                     return color == 0xFF000502u;
                                 });
        };
        ByteStripSampler coarse, fine;
        Request request;
        request.types = true;
        request.contextBytes = page / 2;
        auto map = makeMap(page);
        std::shared_ptr<const StripFrame> overview =
            coarse.Sample(GetCurrentProcess(), map, 0, page, true, true);
        std::shared_ptr<const StripFrame> selection =
            fine.Sample(GetCurrentProcess(), map, 0, page / 2, true, true);
        okay = draw(0, request, overview) && draw(1, request, selection);
        // The main map moves ahead of the 10 Hz sampler while the UI keeps
        // drawing at 60 Hz. Exercise real growth, shrinkage and range changes.
        for (unsigned cycle = 0; cycle < 12 && okay; ++cycle) {
            size_t size = cycle % 2 ? page : 2 * page;
            if (size > page) {
                if (!VirtualAlloc(bytes + page, page, MEM_COMMIT, PAGE_READWRITE)) {
                    okay = false;
                    break;
                }
                std::memset(bytes + page, 160, page);
            } else if (!VirtualFree(bytes + page, page, MEM_DECOMMIT)) {
                okay = false;
                break;
            }
            map = makeMap(size);
            request.contextBase = cycle % 2 ? page / 4 : 0;
            request.contextBytes = size / 2;
            Frame pending;
            pending.map = map;
            pending.request = request;
            pending.miniLayout = OverviewLayout(*map);
            uint32_t now = NowMs();
            if (!now)
                now = 1;
            pending.miniLastPacked.fill(now);
            pending.contextLast.fill(now);
            okay = !StripMatchesView(pending, *overview, false) &&
                   !StripMatchesView(pending, *selection, true) &&
                   !StripGlowPixels(pending, nullptr, *overview, false, now, glow) &&
                   !StripGlowPixels(pending, nullptr, *selection, true, now, glow);
            for (unsigned tick = 0; tick < 6 && okay; ++tick)
                okay = draw(0, request, overview) && draw(1, request, selection);
            overview = coarse.Sample(GetCurrentProcess(), map, 0, size, true, true);
            selection = fine.Sample(
                GetCurrentProcess(), map, request.contextBase, request.contextBytes, true, true);
            okay = okay && draw(0, request, overview) && draw(1, request, selection) &&
                   StripMatchesView(pending, *overview, false) &&
                   StripMatchesView(pending, *selection, true) &&
                   StripGlowPixels(pending, nullptr, *overview, false, now, glow) &&
                   StripGlowPixels(pending, nullptr, *selection, true, now, glow);
        }
        okay = okay && draw(0, request, nullptr) && draw(1, request, nullptr);
        request.types = false;
        okay = okay && draw(0, request, overview, false);
        overview = coarse.Sample(GetCurrentProcess(), map, 0, map->committed, true, false);
        okay = okay && draw(0, request, overview) && overview->pixels[0] == 0xFF006000u;
        request.packed = false;
        okay = okay && draw(0, request, overview, false);
        overview = coarse.Sample(GetCurrentProcess(), map, base, map->committed, false, false);
        okay = okay && draw(0, request, overview) && overview->pixels[0] == 0xFF006000u;
    }
    for (auto texture : textures)
        if (texture)
            SDL_DestroyTexture(texture);
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (surface)
        SDL_FreeSurface(surface);
    VirtualFree(bytes, 0, MEM_RELEASE);
    std::printf("Self-test strip refresh: growth/shrink/drag, rendered frames=%u black gaps=%u, "
                "mode safety and aligned glow=%d\n",
        draws,
        blankFrames,
        okay);
    return okay && blankFrames == 0;
}

bool TestWheelZoom() {
    constexpr uint64_t MiB = 1024 * 1024, limit = 512 * MiB;
    Request seed{192 * MiB, 64, 0, true, false, true, true, 128 * MiB, 256 * MiB};
    WheelState coarse, detail, precise;
    auto coarseIn = ZoomNavigation(seed, ZoomTarget::Context, limit, 1024, 1, false, coarse);
    auto coarseBack = ZoomNavigation(coarseIn, ZoomTarget::Context, limit, 1024, -1, false, coarse);
    auto detailIn = ZoomNavigation(seed, ZoomTarget::Detail, limit, 1024, 1, false, detail);
    auto detailBack = ZoomNavigation(detailIn, ZoomTarget::Detail, limit, 1024, -1, false, detail);
    auto fineIn = ZoomNavigation(seed, ZoomTarget::Detail, limit, 1024, 1, true, precise);
    auto before = NavigationPixelBounds(NavigationRangeFor(seed, 0, limit));
    auto after = NavigationPixelBounds(NavigationRangeFor(coarseIn, 0, limit));
    bool okay =
        coarseIn.contextBytes > seed.contextBytes * 89 / 100 &&
        coarseIn.contextBytes < seed.contextBytes * 93 / 100 &&
        coarseIn.contextBase + coarseIn.contextBytes / 2 ==
            seed.contextBase + seed.contextBytes / 2 &&
        before.bottom - before.top > after.bottom - after.top &&
        (before.bottom - before.top) - (after.bottom - after.top) < 30 &&
        coarseBack.contextBase == seed.contextBase &&
        coarseBack.contextBytes == seed.contextBytes && detailIn.bin < seed.bin &&
        detailIn.bin > seed.bin * 89 / 100 &&
        detailIn.base + detailIn.bin * PixelCount / 2 == seed.base + seed.bin * PixelCount / 2 &&
        detailIn.contextBase == seed.contextBase && detailIn.contextBytes == seed.contextBytes &&
        detailBack.base == seed.base && detailBack.bin == seed.bin && fineIn.bin > detailIn.bin &&
        fineIn.bin < seed.bin;
    WheelState fractional;
    auto partial = seed;
    for (int i = 0; i < 8; ++i)
        partial =
            ZoomNavigation(partial, ZoomTarget::Detail, limit, 1024, 0.125, false, fractional);
    okay = okay && partial.bin == detailIn.bin && partial.base == detailIn.base;
    auto low = seed;
    low.bin = 1;
    WheelState lowScale;
    for (int i = 0; i < 8; ++i)
        low = ZoomNavigation(low, ZoomTarget::Detail, limit, 1024, -1, false, lowScale);
    okay = okay && low.bin > 1 && low.bin < 4;
    auto bounded = seed;
    WheelState bounds;
    for (int i = 0; i < 32; ++i)
        bounded = ZoomNavigation(bounded, ZoomTarget::Context, limit, 1024, 12, false, bounds);
    okay = okay && bounded.contextBytes == PixelCount && bounded.bin == 1 &&
           bounded.base >= bounded.contextBase &&
           bounded.base + PixelCount <= bounded.contextBase + bounded.contextBytes;
    for (int i = 0; i < 32; ++i)
        bounded = ZoomNavigation(bounded, ZoomTarget::Context, limit, 1024, -12, false, bounds);
    okay = okay && bounded.contextBase == 0 && bounded.contextBytes == limit;
    Request tiny{0, 1, 0, true, false, true, true, 0, PixelCount / 3};
    WheelState tinyScale;
    tiny = ZoomNavigation(tiny, ZoomTarget::Detail, PixelCount / 3, 1, -12, false, tinyScale);
    okay = okay && tiny.base == 0 && tiny.bin == 1 && tiny.contextBytes == PixelCount / 3;
    SDL_MouseWheelEvent event{};
    event.y = 2;
    okay = okay && WheelNotches(event) == 2;
#if SDL_VERSION_ATLEAST(2, 0, 18)
    event.preciseY = 0.125f;
    okay = okay && WheelNotches(event) == 0.125;
#endif
    event.direction = SDL_MOUSEWHEEL_FLIPPED;
    okay = okay && WheelNotches(event) < 0;
    std::printf("Self-test wheel zoom: small steps, Shift precision, fractional input, stable "
                "centers and bounds=%d\n",
        okay);
    return okay;
}

bool TestWheelScope() {
    constexpr uint64_t MiB = 1024 * 1024, limit = 512 * MiB;
    Request seed{128 * MiB, 32, 0, true, false, true, true, 128 * MiB, 256 * MiB};
    std::array<Request, 5> ranges{{seed, seed, seed, seed, seed}};
    ranges[1].base = seed.contextBase + seed.contextBytes - seed.bin * PixelCount;
    ranges[2].base = seed.contextBase - 12345; // Stale child: repair it, keep its parent.
    ranges[3].bin = 1024;                      // An oversized child must not enlarge the parent.
    ranges[4] = {71845593, 3, 0, true, false, true, true, 39522333, 102447719};
    bool okay = true;
    unsigned checks = 0;
    for (auto range : ranges)
        for (float x : {SelectionX - 3.0f,
                 SelectionX - 1.0f,
                 SelectionX + StripWidth / 2.0f,
                 SelectionX + StripWidth + 1.0f,
                 SelectionX + StripWidth + 3.0f}) {
            WheelState state;
            auto request = range;
            for (int pass = 0; pass < 48; ++pass) {
                request = WheelAtPoint(request,
                    {x, CanvasY + CanvasHeight / 2.0f},
                    limit,
                    1024,
                    pass < 24 ? -12 : 12,
                    pass % 2 != 0,
                    state);
                auto left = NavigationPixelBounds(NavigationRangeFor(request, 0, limit));
                auto initial = NavigationPixelBounds(NavigationRangeFor(range, 0, limit));
                okay = okay && request.contextBase == range.contextBase &&
                       request.contextBytes == range.contextBytes && left.top == initial.top &&
                       left.bottom == initial.bottom && request.base >= request.contextBase &&
                       request.base + DetailSpan(request, limit) <=
                           request.contextBase + request.contextBytes;
                ++checks;
            }
        }
    for (const auto& size : std::array<SDL_Point, 3>{{{1920, 1080}, {3840, 2160}, {1080, 1920}}}) {
        auto point = FullscreenContentPoint(
            (SelectionX - 2.0f) * size.x / WindowWidth, 0.75f * size.y, size.x, size.y);
        WheelState state;
        auto next = WheelAtPoint(seed, point, limit, 1024, -12, false, state);
        okay = okay && next.contextBase == seed.contextBase &&
               next.contextBytes == seed.contextBytes && next.bin > seed.bin;
    }
    WheelState overview;
    auto changed = WheelAtPoint(seed,
        {OverviewX + StripWidth / 2.0f, CanvasY + CanvasHeight / 2.0f},
        limit,
        1024,
        1,
        false,
        overview);
    okay = okay && changed.contextBytes < seed.contextBytes;
    std::printf("Self-test wheel scope: right strip and handles keep parent fixed, bounds/stale "
                "child/fullscreen, checks=%u result=%d\n",
        checks,
        okay);
    return okay;
}

bool TestCanvasControls() {
    constexpr uint64_t MiB = 1024 * 1024, limit = 512 * MiB;
    Request seed{192 * MiB, 64, 7, true, false, true, true, 128 * MiB, 256 * MiB};
    ContentPoint point{CanvasX + CanvasWidth / 2.0f, CanvasY + CanvasHeight / 2.0f};
    auto fixed = [&](const Request& request) {
        return request.bin == seed.bin && request.contextBase == seed.contextBase &&
               request.contextBytes == seed.contextBytes && request.base >= request.contextBase &&
               request.base + request.bin * PixelCount <=
                   request.contextBase + request.contextBytes &&
               request.packed == seed.packed && request.types == seed.types &&
               request.glow == seed.glow;
    };
    WheelState state, fineState, stripState;
    auto zoomed = WheelAtPoint(seed, point, limit, 1024, 1, false, state);
    auto fineZoom = WheelAtPoint(seed, point, limit, 1024, 1, true, fineState);
    auto stripZoom = WheelAtPoint(seed,
        {SelectionX + StripWidth / 2.0f, CanvasY + CanvasHeight / 2.0f},
        limit,
        1024,
        1,
        false,
        stripState);
    bool okay = zoomed.bin < seed.bin && fineZoom.bin > zoomed.bin && fineZoom.bin < seed.bin &&
                zoomed.bin == stripZoom.bin && zoomed.base == stripZoom.base &&
                zoomed.contextBase == seed.contextBase && zoomed.contextBytes == seed.contextBytes;
    auto fractional = seed;
    WheelState precise;
    for (int i = 0; i < 8; ++i)
        fractional = WheelAtPoint(fractional, point, limit, 1024, 0.125, false, precise);
    okay = okay && fractional.base == zoomed.base && fractional.bin == zoomed.bin;
    auto bounded = seed;
    WheelState bounds;
    for (int i = 0; i < 64; ++i) {
        bounded = WheelAtPoint(bounded, point, limit, 1024, -12, false, bounds);
        okay = okay && bounded.contextBase == seed.contextBase &&
               bounded.contextBytes == seed.contextBytes && bounded.base >= seed.contextBase &&
               bounded.base + bounded.bin * PixelCount <= seed.contextBase + seed.contextBytes;
    }
    okay = okay && bounded.bin == seed.contextBytes / PixelCount;
    auto drag = BeginCanvasDrag(seed, point, limit);
    auto earlier = DragCanvas(drag, point.y + 32), later = DragCanvas(drag, point.y - 32);
    auto atPress = DragCanvas(drag, point.y), low = DragCanvas(drag, 1.0e6f),
         high = DragCanvas(drag, -1.0e6f);
    okay = okay && drag.active && fixed(earlier) && fixed(later) && fixed(atPress) && fixed(low) &&
           fixed(high) && earlier.base < seed.base && later.base > seed.base &&
           atPress.base == seed.base && low.base == seed.contextBase &&
           high.base == seed.contextBase + seed.contextBytes - seed.bin * PixelCount;
    okay = okay && CanvasDragMatches(drag, later, limit);
    auto reframed = seed;
    ++reframed.contextBase;
    okay = okay && !CanvasDragMatches(drag, reframed, limit);
    reframed = seed;
    ++reframed.bin;
    okay = okay && !CanvasDragMatches(drag, reframed, limit) &&
           !CanvasDragMatches(drag, seed, seed.contextBytes);
    for (ContentPoint outside : {ContentPoint{8, 8},
             ContentPoint{CanvasX - 8.0f, CanvasY + CanvasHeight / 2.0f},
             ContentPoint{CanvasX + 8.0f, CanvasY + CanvasHeight + 8.0f}}) {
        auto unchanged = WheelAtPoint(seed, outside, limit, 1024, -1, false, state);
        okay = okay && fixed(unchanged) && unchanged.base == seed.base &&
               !BeginCanvasDrag(seed, outside, limit).active;
    }
    for (const auto& size :
        std::array<SDL_Point, 4>{{{1920, 1080}, {3840, 2160}, {1080, 1920}, {2560, 1440}}}) {
        float screenX = (CanvasX + CanvasWidth / 2.0f) * size.x / WindowWidth;
        auto press = FullscreenContentPoint(screenX, 0.5f * size.y, size.x, size.y);
        auto move =
            FullscreenContentPoint(screenX, (0.5f + 32.0f / CanvasHeight) * size.y, size.x, size.y);
        auto moved = DragCanvas(BeginCanvasDrag(seed, press, limit), move.y);
        WheelState fullscreenState;
        auto fullZoom = WheelAtPoint(seed, press, limit, 1024, 1, false, fullscreenState);
        okay = okay && fixed(moved) && moved.base == earlier.base && fullZoom.bin == zoomed.bin &&
               fullZoom.base == zoomed.base;
    }
    Request all{0, 1, 0, true, true, true, true, 0, PixelCount / 2};
    auto allMoved = DragCanvas(BeginCanvasDrag(all, point, PixelCount / 2), point.y - 100);
    okay = okay && allMoved.base == 0 && allMoved.bin == 1 && allMoved.overview &&
           allMoved.contextBytes == PixelCount / 2;
    Request addresses{0x700000000000ull,
        4096,
        0,
        false,
        false,
        true,
        true,
        0x6FF000000000ull,
        128ull * 1024 * MiB};
    auto addressMoved =
        DragCanvas(BeginCanvasDrag(addresses, point, 0x7FFFFFFF0000ull), point.y - 32);
    okay = okay && addressMoved.base > addresses.base && addressMoved.bin == addresses.bin &&
           addressMoved.contextBase == addresses.contextBase &&
           addressMoved.contextBytes == addresses.contextBytes && !addressMoved.packed;
    // Verify grabbing the image translates its actual byte texture, preserving scale.
    auto bytes = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 3 * PixelCount, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    bool pixelsOkay = false;
    if (bytes) {
        for (int row = 0; row < 3 * CanvasHeight; ++row)
            std::fill(bytes + row * CanvasWidth,
                bytes + (row + 1) * CanvasWidth,
                static_cast<uint8_t>(row % 251));
        auto map = std::make_shared<Map>();
        uint64_t base = reinterpret_cast<uint64_t>(bytes);
        map->limit = base + 3 * PixelCount;
        map->committed = map->readable = 3 * PixelCount;
        map->regions = {
            {base, 3 * PixelCount, base, MEM_COMMIT, PAGE_READWRITE, MEM_PRIVATE, true}};
        Request view{PixelCount, 1, 0, true, false, false, true, 0, 3 * PixelCount};
        auto before = MakeFrame(GetCurrentProcess(), map, view);
        auto moved = DragCanvas(BeginCanvasDrag(view, point, map->committed), point.y - 32);
        auto after = MakeFrame(GetCurrentProcess(), map, moved);
        pixelsOkay = moved.base > view.base && moved.bin == view.bin &&
                     before->bytesRead == PixelCount && after->bytesRead == PixelCount &&
                     std::equal(after->pixels.begin(),
                         after->pixels.end() - CanvasWidth * 32,
                         before->pixels.begin() + CanvasWidth * 32);
        VirtualFree(bytes, 0, MEM_RELEASE);
    }
    std::printf("Self-test canvas controls: wheel zoom/Shift/fractional, grab pan/bounds, fixed "
                "parent, fullscreen, addresses and translated pixels=%d/%d\n",
        okay,
        pixelsOkay);
    return okay && pixelsOkay;
}

bool TestFullSelection() {
    constexpr uint64_t MiB = 1024 * 1024, limit = 512 * MiB, base = 61435122;
    ContentPoint point{CanvasX + CanvasWidth / 2.0f, CanvasY + CanvasHeight / 2.0f};
    bool okay = true;
    for (uint64_t bytes : {uint64_t(PixelCount),
             uint64_t(PixelCount + 1),
             uint64_t(3 * PixelCount - 1),
             uint64_t(42071128),
             uint64_t(81 * PixelCount)}) {
        Request seed{base, 1, 0, true, false, true, true, base, bytes};
        WheelState state;
        auto zoomed = seed;
        for (int i = 0; i < 32; ++i)
            zoomed = WheelAtPoint(zoomed, point, limit, 1024, -12, false, state);
        auto pixels = NavigationPixelBounds(NavigationRangeFor(zoomed, 1, limit));
        auto canvas = BeginCanvasDrag(zoomed, point, limit);
        auto coarse = NavigationRangeFor(zoomed, 0, limit);
        auto strip = BeginNavigationDrag(zoomed, 1, CanvasHeight / 2, limit);
        auto up = DragNavigation(strip, 0), down = DragNavigation(strip, CanvasHeight);
        okay = okay && zoomed.contextBase == base && zoomed.contextBytes == bytes &&
               zoomed.base == base && zoomed.bin == (bytes + PixelCount - 1) / PixelCount &&
               DetailEnd(zoomed, limit) == base + bytes && pixels.top == 0 &&
               pixels.bottom == CanvasHeight && !canvas.active &&
               DragCanvas(canvas, point.y - 300).base == base &&
               DragCanvas(canvas, point.y + 300).base == base && strip.kind == NavDragKind::Move &&
               up.base == base && down.base == base && up.bin == zoomed.bin &&
               down.bin == zoomed.bin && coarse.base == base && coarse.bytes == bytes;
        auto resize = BeginNavigationDrag(
            seed, 1, NavigationPixelBounds(NavigationRangeFor(seed, 1, limit)).bottom, limit);
        auto resized = DragNavigation(resize, CanvasHeight);
        auto selected = ChooseDetail(seed, base, bytes, limit);
        okay = okay && resized.base == base && resized.contextBase == base &&
               resized.contextBytes == bytes && DetailSpan(resized, limit) == bytes &&
               DetailSpan(selected, limit) == bytes && selected.contextBytes == bytes;
        if (zoomed.bin > 1) {
            auto smaller = zoomed;
            WheelState inward;
            for (int i = 0; i < 16 && smaller.bin == zoomed.bin; ++i)
                smaller = WheelAtPoint(smaller, point, limit, 1024, 1, false, inward);
            okay = okay && DetailSpan(smaller, limit) < bytes &&
                   BeginCanvasDrag(smaller, point, limit).active;
        }
    }
    // The last partial bin must contain the final selected byte and no bytes
    // from the adjacent memory. Unused image pixels are padding.
    constexpr uint64_t selectedBase = 4096, selectedBytes = PixelCount + 13,
                       allocation = 3 * PixelCount;
    auto data = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, allocation, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    bool tailOkay = false;
    if (data) {
        std::fill(data, data + allocation, 255);
        std::fill(data + selectedBase, data + selectedBase + selectedBytes, 32);
        data[selectedBase + selectedBytes - 1] = 192;
        auto map = std::make_shared<Map>();
        uint64_t address = reinterpret_cast<uint64_t>(data);
        map->limit = address + allocation;
        map->committed = map->readable = allocation;
        map->regions = {
            {address, allocation, address, MEM_COMMIT, PAGE_READWRITE, MEM_PRIVATE, true}};
        Request view{selectedBase, 2, 0, true, false, true, true, selectedBase, selectedBytes};
        auto frame = MakeFrame(GetCurrentProcess(), map, view);
        size_t last = static_cast<size_t>((selectedBytes - 1) / view.bin);
        tailOkay =
            frame->bytesWanted == selectedBytes && frame->bytesRead == selectedBytes &&
            frame->pixels[0] == MemoryByteColor(MemoryRole::Private, 32, true) &&
            frame->pixels[last] == MemoryByteColor(MemoryRole::Private, 192, true) &&
            std::all_of(frame->pixels.begin() + last + 1, frame->pixels.end(), [](uint32_t color) {
                return color == PaddingColor;
            });
        VirtualFree(data, 0, MEM_RELEASE);
    }
    std::printf("Self-test full selection: exact span, wheel/edge selection, fixed parent, "
                "canvas/strip locked, partial tail and no adjacent reads=%d/%d\n",
        okay,
        tailOkay);
    return okay && tailOkay;
}

bool TestFrameWorkspace() {
    const size_t allocationSize = ReadChunkBytes + 16384;
    auto bytes = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, allocationSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!bytes) {
        return false;
    }
    std::fill(bytes, bytes + 4096, 32);
    std::fill(bytes + 4096, bytes + 8192, 128);
    std::fill(bytes + 8192, bytes + allocationSize, 192);
    const uint64_t address = reinterpret_cast<uint64_t>(bytes);
    auto map = std::make_shared<Map>();
    map->limit = address + allocationSize;
    map->committed = allocationSize;
    map->regions = {
        {address,
            8192,
            address,
            MEM_COMMIT,
            PAGE_READWRITE,
            MEM_PRIVATE,
            true,
            0,
            MemoryRole::Heap},
        {address + 8192,
            allocationSize - 8192,
            address,
            MEM_COMMIT,
            PAGE_READWRITE,
            MEM_PRIVATE,
            true,
            8192,
            MemoryRole::Stack},
    };
    FrameWorkspace workspace;
    ChangeTracker tracker;
    Request request{0, 1, 0, true, false, true, true, 0, allocationSize};
    auto frame = MakeFrame(GetCurrentProcess(), map, request, &tracker, &workspace);
    bool okay = frame->pixels[0] == MemoryByteColor(MemoryRole::Heap, 32, true) &&
                frame->pixels[8192] == MemoryByteColor(MemoryRole::Stack, 192, true);
    request.types = false;
    frame = MakeFrame(GetCurrentProcess(), map, request, &tracker, &workspace);
    okay = okay && frame->changedBytes == 0 &&
           frame->pixels[0] == MemoryByteColor(MemoryRole::Heap, 32, false) &&
           frame->pixels[8192] == MemoryByteColor(MemoryRole::Stack, 192, false);
    request.types = true;
    frame = MakeFrame(GetCurrentProcess(), map, request, &tracker, &workspace);
    okay = okay && frame->pixels[8192] == MemoryByteColor(MemoryRole::Stack, 192, true);

    request.base = 37;
    request.bin = 33;
    request.contextBase = 37;
    request.contextBytes = PixelCount + 13;
    frame = MakeFrame(GetCurrentProcess(), map, request, &tracker, &workspace);
    const size_t last = static_cast<size_t>((request.contextBytes - 1) / request.bin);
    okay = okay && frame->bytesRead == request.contextBytes &&
           frame->pixels[0] == MemoryByteColor(MemoryRole::Heap, 32, true) &&
           frame->pixels[last] == MemoryByteColor(MemoryRole::Stack, 192, true) &&
           frame->pixels[last + 1] == PaddingColor;

    auto updated = std::make_shared<Map>(*map);
    updated->regions[1].role = MemoryRole::Image;
    request.base = 17;
    request.contextBase = 17;
    request.contextBytes = allocationSize - 17;
    frame = MakeFrame(GetCurrentProcess(), updated, request, &tracker, &workspace);
    const size_t endPixel = static_cast<size_t>((request.contextBytes - 1) / request.bin);
    okay = okay && frame->bytesRead == request.contextBytes &&
           frame->pixels[endPixel] == MemoryByteColor(MemoryRole::Image, 192, true) &&
           frame->pixels[endPixel + 1] == PaddingColor;

    // A live protection change must invalidate reads even when map geometry
    // and the cached role/flag buffers have not changed.
    DWORD previousProtect = 0;
    const bool guarded =
        VirtualProtect(bytes + 8192, 4096, PAGE_READWRITE | PAGE_GUARD, &previousProtect) != FALSE;
    frame = MakeFrame(GetCurrentProcess(), updated, request, &tracker, &workspace);
    MEMORY_BASIC_INFORMATION current{};
    VirtualQuery(bytes + 8192, &current, sizeof(current));
    okay = okay && guarded && (current.Protect & PAGE_GUARD) &&
           frame->bytesRead == request.contextBytes - 4096 && frame->changedBytes == 0;
    VirtualFree(bytes, 0, MEM_RELEASE);
    std::printf("Self-test frame workspace: palette/range/map changes, unaligned bins, "
                "partial tails, multi-chunk coverage and live guards=%d\n",
        okay);
    return okay;
}

bool TestPendingNavigation() {
    constexpr uint64_t MiB = 1024 * 1024;
    auto map = std::make_shared<Map>();
    map->committed = 128 * MiB;
    Frame frame;
    frame.map = map;
    frame.request =
        FitNavigation({0, 16, 0, true, false, false, true, 0, 64 * MiB}, map->committed);
    Request pending = frame.request;
    pending.base = 2 * MiB;
    bool okay = DisplayedNavigationRequest(frame, pending, 1).base == pending.base;
    pending = ChooseContext(pending, 8 * MiB, 64 * MiB, map->committed);
    const auto overview = DisplayedNavigationRequest(frame, pending, 0);
    const auto selection = DisplayedNavigationRequest(frame, pending, 1);
    okay = okay && overview.contextBase == pending.contextBase &&
           selection.contextBase == frame.request.contextBase &&
           selection.base == frame.request.base;
    frame.request = pending;
    pending.base += MiB;
    okay = okay && DisplayedNavigationRequest(frame, pending, 1).base == pending.base;
    pending.packed = false;
    okay = okay && DisplayedNavigationRequest(frame, pending, 0).packed;
    std::printf("Self-test pending navigation: immediate feedback, parent-domain consistency "
                "and coordinate-mode changes=%d\n",
        okay);
    return okay;
}

bool TestAddressAnchoring() {
    constexpr uint64_t MiB = 1024 * 1024;
    auto original = std::make_shared<Map>();
    original->limit = 1ull << 47;
    auto add = [](Map& map, uint64_t base, uint64_t size) {
        Region region{base, size, base, MEM_COMMIT, PAGE_READWRITE, MEM_PRIVATE, true};
        region.position = map.committed;
        map.committed += size;
        map.regions.push_back(region);
    };
    add(*original, 0x10000000, 4 * MiB);
    add(*original, 0x30000000, 16 * MiB);
    add(*original, 0x70000000, 32 * MiB);
    auto inserted = std::make_shared<Map>();
    inserted->limit = original->limit;
    add(*inserted, 0x10000000, 4 * MiB);
    add(*inserted, 0x20000000, 14 * MiB);
    add(*inserted, 0x30000000, 16 * MiB);
    add(*inserted, 0x70000000, 32 * MiB);
    const auto seed =
        AnchorNavigation({8 * MiB, 2, 0, true, false, false, true, 6 * MiB, 28 * MiB}, original);
    auto moved = RebaseNavigation(seed, inserted);
    bool stable = moved.base == seed.base + 14 * MiB && moved.bin == seed.bin &&
                  moved.contextBase == seed.contextBase + 14 * MiB &&
                  moved.contextBytes == seed.contextBytes &&
                  AddressOfPosition(*inserted, moved.base, true) == seed.anchor.address &&
                  !AnchorUnavailable(moved);
    for (int i = 0; i < 40; ++i) {
        moved = RebaseNavigation(moved, original);
        stable = stable && moved.base == seed.base && moved.contextBase == seed.contextBase;
        moved = RebaseNavigation(moved, inserted);
    }

    auto expanded = std::make_shared<Map>();
    expanded->limit = original->limit;
    add(*expanded, 0x10000000, 4 * MiB);
    add(*expanded, 0x20000000, 14 * MiB);
    add(*expanded, 0x30000000, 16 * MiB);
    add(*expanded, 0x50000000, 3 * MiB);
    add(*expanded, 0x70000000, 32 * MiB);
    const auto inside = RebaseNavigation(seed, expanded);
    stable = stable && inside.base == seed.base + 14 * MiB && inside.bin == seed.bin &&
             inside.contextBytes == seed.contextBytes + 3 * MiB;
    const auto whole = RebaseNavigation(
        AnchorNavigation({0, DataBin(*original), 0, true, true}, original), inserted);
    bool full = whole.overview && whole.base == 0 && whole.contextBytes == inserted->committed &&
                whole.bin == DataBin(*inserted);
    auto selectedAll = seed;
    selectedAll.base = selectedAll.contextBase;
    selectedAll.bin = selectedAll.contextBytes / PixelCount;
    selectedAll = RebaseNavigation(AnchorNavigation(selectedAll, original), expanded);
    full = full && selectedAll.base == selectedAll.contextBase &&
           DetailSpan(selectedAll, expanded->committed) == selectedAll.contextBytes &&
           !BeginCanvasDrag(selectedAll, {CanvasX + 20.0f, CanvasY + 20.0f}, expanded->committed)
                .active;

    Frame frame;
    frame.map = original;
    frame.request = seed;
    const auto pending = DisplayedNavigationRequest(frame, moved, 1);
    bool input = pending.base == seed.base && pending.contextBase == seed.contextBase;
    Shared shared;
    shared.request = moved;
    auto oldGesture = seed;
    oldGesture.base += 4096;
    UpdateRequest(shared, oldGesture);
    auto latest = RebaseNavigation(shared.request, inserted);
    input = input && latest.base == moved.base + 4096 &&
            latest.anchor.address == seed.anchor.address + 4096;
    auto drag = BeginCanvasDrag(seed, {CanvasX + 20.0f, CanvasY + 100.0f}, original->committed);
    drag.seed = RebaseNavigation(drag.seed, inserted);
    drag.limit = inserted->committed;
    const auto dragged = AnchorNavigation(DragCanvas(drag, drag.anchorY - 4), inserted);
    input = input && dragged.anchor.address == seed.anchor.address + 4 * CanvasWidth * seed.bin;

    auto releasedMap = std::make_shared<Map>();
    releasedMap->limit = original->limit;
    add(*releasedMap, 0x10000000, 4 * MiB);
    add(*releasedMap, 0x70000000, 32 * MiB);
    const auto released = RebaseNavigation(seed, releasedMap);
    const auto returned = RebaseNavigation(released, original);
    const auto unavailable = MakeFrame(GetCurrentProcess(), releasedMap, released);
    bool invalid = released.anchor.status == AnchorStatus::Released &&
                   returned.anchor.status == AnchorStatus::Released &&
                   returned.anchor.address == seed.anchor.address && unavailable->bytesRead == 0 &&
                   unavailable->bytesWanted == 0 &&
                   unavailable->pixels[0] == UnsampledColor(seed.anchor.role, false, 0, 0);
    shared.request = released;
    UpdateRequest(shared, released, true);
    UpdateRequest(shared, shared.request, false, true);
    invalid = invalid && AnchorUnavailable(shared.request);
    UpdateRequest(shared, returned);
    invalid = invalid && !AnchorUnavailable(shared.request);
    auto remapped = std::make_shared<Map>(*original);
    remapped->regions[1].allocation -= MiB;
    invalid = invalid && RebaseNavigation(seed, remapped).anchor.status == AnchorStatus::Remapped;
    auto shortened = std::make_shared<Map>(*original);
    shortened->regions.back().size = 30 * MiB + MiB / 2;
    shortened->committed -= MiB + MiB / 2;
    auto tail = AnchorNavigation(
        {50 * MiB, 2, 0, true, false, false, true, 0, original->committed}, original);
    invalid = invalid &&
              RebaseNavigation(tail, shortened).anchor.status == AnchorStatus::RangeUnavailable;
    std::printf("Self-test address anchoring: layout insertion/removal=%d whole ranges=%d "
                "pending input and dragging=%d released/remapped/clipped views=%d\n",
        stable,
        full,
        input,
        invalid);
    return stable && full && input && invalid;
}

bool TestLiveAddressAnchoring() {
    constexpr uint64_t MiB = 1024 * 1024;
    const auto address = reinterpret_cast<uint64_t>(GetModuleHandleW(L"kernel32.dll")) + 8192;
    const auto original = QueryMap(GetCurrentProcess());
    const auto request = AnchorNavigation({PositionOfAddress(*original, address),
                                              1,
                                              0,
                                              true,
                                              false,
                                              false,
                                              true,
                                              0,
                                              original->committed},
        original);
    const auto before = MakeFrame(GetCurrentProcess(), original, request);
    void* allocation = VirtualAlloc(nullptr, 14 * MiB, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!allocation)
        throw std::runtime_error("Address anchor test allocation failed");
    const auto expanded = QueryMap(GetCurrentProcess());
    const auto rebased = RebaseNavigation(request, expanded);
    const auto after = MakeFrame(GetCurrentProcess(), expanded, rebased);
    bool okay =
        reinterpret_cast<uint64_t>(allocation) < address && rebased.base > request.base &&
        rebased.anchor.address == address && !AnchorUnavailable(rebased) &&
        AddressOfPosition(*expanded, rebased.base, true) == address &&
        before->bytesRead == PixelCount && after->bytesRead == PixelCount &&
        std::equal(before->pixels.begin(), before->pixels.begin() + 64, after->pixels.begin());
    VirtualFree(allocation, 0, MEM_RELEASE);
    const auto shrunk = QueryMap(GetCurrentProcess());
    const auto restored = RebaseNavigation(rebased, shrunk);
    okay = okay && !AnchorUnavailable(restored) &&
           AddressOfPosition(*shrunk, restored.base, true) == address;
    std::printf("Self-test live address anchor: 14 MiB VirtualAlloc/VirtualFree around an "
                "unchanged module address=%d\n",
        okay);
    return okay;
}

bool SelfTest() {
    bool segmentHeapOkay = TestSegmentHeapRecognition();
    bool fiveTypesOkay = TestFiveTypeClassification();
    bool writeCombineOkay = TestWriteCombineSampling();
    bool stripRefreshOkay = TestStripRefresh();
    bool wheelZoomOkay = TestWheelZoom();
    bool wheelScopeOkay = TestWheelScope();
    bool canvasControlsOkay = TestCanvasControls();
    bool fullSelectionOkay = TestFullSelection();
    const bool workspaceOkay = TestFrameWorkspace();
    const bool pendingNavigationOkay = TestPendingNavigation();
    const bool addressAnchoringOkay = TestAddressAnchoring();
    const bool liveAnchoringOkay = TestLiveAddressAnchoring();
    auto bytes = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 3 * 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!bytes)
        throw std::runtime_error("Self-test allocation failed");
    std::fill(bytes, bytes + 4096, 32);
    std::fill(bytes + 8192, bytes + 12288, 192);
    DWORD oldProtect = 0;
    VirtualProtect(bytes + 4096, 4096, PAGE_NOACCESS, &oldProtect);
    auto map = std::make_shared<Map>();
    uint64_t base = reinterpret_cast<uint64_t>(bytes);
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    map->limit = reinterpret_cast<uint64_t>(info.lpMaximumApplicationAddress) + 1;
    map->regions = {{base, 4096, base, MEM_COMMIT, PAGE_READWRITE, MEM_PRIVATE, true},
        {base + 4096, 4096, base, MEM_COMMIT, PAGE_NOACCESS, MEM_PRIVATE, false},
        {base + 8192, 4096, base, MEM_COMMIT, PAGE_READWRITE, MEM_PRIVATE, true}};
    auto frame = MakeFrame(GetCurrentProcess(), map, {base, 1, 1, false, false});
    bool okay = segmentHeapOkay && fiveTypesOkay && writeCombineOkay && stripRefreshOkay &&
                wheelZoomOkay && wheelScopeOkay && canvasControlsOkay && fullSelectionOkay &&
                workspaceOkay && pendingNavigationOkay && addressAnchoringOkay &&
                liveAnchoringOkay && frame->bytesRead == 8192 && frame->bytesWanted == 8192 &&
                frame->pixels[0] == 0xFF002000u && frame->pixels[4095] == 0xFF002000u &&
                frame->pixels[4096] == ProtectedColor && frame->pixels[8191] == ProtectedColor &&
                frame->pixels[8192] == 0xFF00C000u && frame->pixels[12287] == 0xFF00C000u &&
                frame->pixels[12288] == FreeColor &&
                !IsReadablePage(MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD);
    auto average = MakeFrame(GetCurrentProcess(), map, {base, 16, 2, false, false});
    okay = okay && average->pixels[0] == 0xFF002000u && average->pixels[256] == ProtectedColor &&
           average->pixels[512] == 0xFF00C000u && average->bytesRead == 8192;
    auto packedMap = std::make_shared<Map>();
    packedMap->limit = map->limit;
    packedMap->committed = 8192;
    packedMap->regions = {map->regions[0], map->regions[2]};
    packedMap->regions[0].position = 0;
    packedMap->regions[1].position = 4096;
    auto packed = MakeFrame(GetCurrentProcess(), packedMap, {0, 1, 3, true, false});
    okay = okay && packed->bytesRead == 8192 && packed->pixels[0] == 0xFF002000u &&
           packed->pixels[4096] == 0xFF00C000u && packed->pixels[8192] == PaddingColor &&
           AddressOfPosition(*packedMap, 4096, true) == base + 8192;
    packedMap->regions[0].role = MemoryRole::Heap;
    packedMap->regions[1].role = MemoryRole::Stack;
    auto colored = MakeFrame(GetCurrentProcess(), packedMap, {0, 1, 4, true, false, true});
    auto mixed = MakeFrame(GetCurrentProcess(), packedMap, {0, 6144, 5, true, false, true});
    bool mappingOkay = okay;
    okay = okay && colored->bytesRead == packed->bytesRead &&
           colored->pixels[0] == MemoryByteColor(MemoryRole::Heap, 32, true) &&
           colored->pixels[4096] == MemoryByteColor(MemoryRole::Stack, 192, true) &&
           mixed->pixels[0] == MemoryByteColor(MemoryRole::Heap, 85, true) &&
           mixed->pixels[1] == MemoryByteColor(MemoryRole::Stack, 192, true) &&
           mixed->bytesRead == 8192;
    bool colorsOkay = okay;
    // Unread image pages retain the image hue and carry a pattern, not byte data.
    auto blockedMap = std::make_shared<Map>();
    blockedMap->limit = map->limit;
    blockedMap->committed = 4096;
    blockedMap->regions = {map->regions[1]};
    blockedMap->regions[0].kind = MEM_IMAGE;
    blockedMap->regions[0].role = MemoryRole::Image;
    auto blocked = MakeFrame(GetCurrentProcess(), blockedMap, {0, 1, 0, true, false, true});
    ByteStripSampler blockedSampler;
    auto blockedStrip = blockedSampler.Sample(GetCurrentProcess(), blockedMap, 0, 4096, true, true);
    bool statusOkay =
        blocked->bytesRead == 0 && blockedStrip->bytesRead == 0 &&
        blocked->pixels[0] == UnsampledColor(MemoryRole::Image, true, 0, 0) &&
        blocked->pixels[4] == UnsampledColor(MemoryRole::Image, true, 4, 0) &&
        blocked->pixels[0] != blocked->pixels[4] && blockedStrip->pixels[0] == blocked->pixels[0] &&
        blockedStrip->pixels[4] == blocked->pixels[4] &&
        std::all_of(
            blockedStrip->lastChanged.begin(), blockedStrip->lastChanged.end(), [](uint32_t time) {
                return !time;
            });
    std::printf("Self-test unsampled status: category-colored hatching in main/strips without "
                "reads or false glow=%d\n",
        statusOkay);
    okay = okay && statusOkay;
    ChangeTracker tracker;
    Request changeRequest{0, 16, 6, true, false, true};
    auto baseline = MakeFrame(GetCurrentProcess(), packedMap, changeRequest, &tracker);
    auto unchanged = MakeFrame(GetCurrentProcess(), packedMap, changeRequest, &tracker);
    bytes[0] = 31;
    bytes[1] = 33; // Exact same average, two genuinely different bytes.
    auto changed = MakeFrame(GetCurrentProcess(), packedMap, changeRequest, &tracker);
    bool changesOkay = baseline->changedBytes == 0 && unchanged->changedBytes == 0 &&
                       changed->changedBytes == 2 && changed->changedPixels == 1 &&
                       changed->changes[0] == 2 && changed->pixels[0] == unchanged->pixels[0];
    auto zoomed = MakeFrame(GetCurrentProcess(), packedMap, {0, 1, 7, true, false, true}, &tracker);
    bytes[8192 + 4] = 193;
    auto byteChanged =
        MakeFrame(GetCurrentProcess(), packedMap, {0, 1, 8, true, false, true}, &tracker);
    changesOkay = changesOkay && zoomed->changedBytes == 0 && byteChanged->changedBytes == 1 &&
                  byteChanged->changes[4100] == 1;
    std::fill(bytes + 32, bytes + 48, 33);
    auto vectorChanged = MakeFrame(GetCurrentProcess(), packedMap, changeRequest, &tracker);
    changesOkay =
        changesOkay && vectorChanged->changedBytes == 16 && vectorChanged->changes[2] == 16;
    std::fill(bytes + 32, bytes + 48, 32);
    MakeFrame(GetCurrentProcess(), packedMap, changeRequest, &tracker);
    // A newly visible page has no previous sample, and a failed/protected read
    // invalidates its old sample before the page becomes readable again.
    packedMap->regions[1].readable = false;
    auto inaccessible = MakeFrame(GetCurrentProcess(), packedMap, changeRequest, &tracker);
    packedMap->regions[1].readable = true;
    bytes[8192 + 4] = 192;
    auto accessible = MakeFrame(GetCurrentProcess(), packedMap, changeRequest, &tracker);
    changesOkay = changesOkay && inaccessible->changedBytes == 0 && accessible->changedBytes == 0 &&
                  GlowLevel(1000, 1000) > GlowLevel(1300, 1000) && GlowLevel(1900, 1000) == 0 &&
                  GlowLevel(1000, 0) == 0;
    bytes[0] = 32;
    bytes[1] = 32;
    okay = okay && changesOkay;
    OverviewSampler sampler;
    auto miniBaseline = sampler.Sample(GetCurrentProcess(), packedMap);
    ByteStripSampler miniStripSampler;
    auto miniTexture = miniStripSampler.Sample(
        GetCurrentProcess(), packedMap, 0, packedMap->committed, true, true);
    auto local =
        MakeFrame(GetCurrentProcess(), packedMap, {4096, 1, 10, true, false, true}, &tracker);
    bytes[7] = 99; // Outside the main viewport; the overview must still react.
    auto localUnchanged =
        MakeFrame(GetCurrentProcess(), packedMap, {4096, 1, 11, true, false, true}, &tracker);
    auto miniChanged = sampler.Sample(GetCurrentProcess(), packedMap);
    bool miniOkay = miniBaseline->changedBytes == 0 && miniChanged->changedBytes == 1 &&
                    localUnchanged->changedBytes == 0 && miniChanged->lastPacked[0] != 0 &&
                    miniChanged->bytesRead == 8192 && miniChanged->bytesPlanned <= 4 * 1024 * 1024;
    std::vector<uint32_t> miniGlow(StripWidth * CanvasHeight);
    miniOkay = miniOkay &&
               StripGlowPixels(
                   *localUnchanged, miniChanged.get(), *miniTexture, false, NowMs(), miniGlow) &&
               miniGlow[StripWidth / 2] != 0xFF000000u;
    localUnchanged->request.glow = false;
    miniOkay =
        miniOkay && !StripGlowPixels(
                        *localUnchanged, miniChanged.get(), *miniTexture, false, NowMs(), miniGlow);
    localUnchanged->request.glow = true;
    VirtualProtect(bytes + 8192, 4096, PAGE_READWRITE | PAGE_GUARD, &oldProtect);
    auto guardedMini = sampler.Sample(GetCurrentProcess(), packedMap);
    MEMORY_BASIC_INFORMATION guardState{};
    VirtualQuery(bytes + 8192, &guardState, sizeof(guardState));
    miniOkay = miniOkay && guardedMini->bytesRead == 4096 && guardedMini->changedBytes == 0 &&
               (guardState.Protect & PAGE_GUARD);
    VirtualProtect(bytes + 8192, 4096, PAGE_READWRITE, &oldProtect);
    bytes[8192 + 7] = 193;
    auto miniAfterGuard = sampler.Sample(GetCurrentProcess(), packedMap);
    miniOkay = miniOkay && miniAfterGuard->changedBytes == 0;
    auto repacked = std::make_shared<Map>(*map);
    repacked->committed = 12288;
    for (size_t i = 0; i < repacked->regions.size(); ++i)
        repacked->regions[i].position = i * 4096;
    auto miniRepacked = sampler.Sample(GetCurrentProcess(), repacked);
    miniOkay = miniOkay && miniRepacked->changedBytes == 0;
    std::printf("Self-test overview: outside-main-change=%llu main-change=%llu guarded-read=%llu "
                "layout-change=%llu\n",
        miniChanged->changedBytes,
        localUnchanged->changedBytes,
        guardedMini->bytesRead,
        miniRepacked->changedBytes);
    bytes[7] = 32;
    bytes[8192 + 7] = 192;
    okay = okay && miniOkay;
    constexpr uint64_t MiB = 1024 * 1024;
    Request navigation{80 * MiB, 32, 0, true, false, true};
    navigation.contextBase = 0;
    navigation.contextBytes = 64 * MiB;
    navigation = FitNavigation(navigation, 256 * MiB);
    bool navigationOkay = navigation.base == 80 * MiB && navigation.contextBytes == 64 * MiB &&
                          navigation.contextBase <= navigation.base &&
                          navigation.base + navigation.bin * PixelCount <=
                              navigation.contextBase + navigation.contextBytes;
    auto coarseChoice = ChooseContext(navigation, 128 * MiB, 64 * MiB, 256 * MiB);
    auto fineChoice = ChooseDetail(coarseChoice, 140 * MiB, 4 * MiB, 256 * MiB);
    auto resizedContext = ChooseContext(fineChoice, 130 * MiB, 60 * MiB, 256 * MiB);
    auto narrowContext = ChooseContext(resizedContext, 140 * MiB, 2 * MiB, 256 * MiB);
    navigationOkay = navigationOkay && coarseChoice.contextBase == 128 * MiB &&
                     fineChoice.base == 140 * MiB && fineChoice.bin == 8 &&
                     resizedContext.base == fineChoice.base && narrowContext.bin == 4 &&
                     narrowContext.base >= narrowContext.contextBase &&
                     narrowContext.base + narrowContext.bin * PixelCount <=
                         narrowContext.contextBase + narrowContext.contextBytes;
    Request dragRequest{64 * MiB, 32, 0, true, false, true};
    dragRequest.contextBase = 32 * MiB;
    dragRequest.contextBytes = 128 * MiB;
    auto coarseMove = BeginNavigationDrag(dragRequest, 0, 192, 256 * MiB);
    auto coarseMoved = DragNavigation(coarseMove, 224);
    auto coarseClampedLow = DragNavigation(coarseMove, -1000);
    auto coarseClampedHigh = DragNavigation(coarseMove, 1000);
    auto fineMove = BeginNavigationDrag(dragRequest, 1, 160, 256 * MiB);
    auto fineMoved = DragNavigation(fineMove, 192);
    auto fineClampedLow = DragNavigation(fineMove, -1000);
    auto fineClampedHigh = DragNavigation(fineMove, 1000);
    bool dragOkay =
        coarseMove.kind == NavDragKind::Move && coarseMoved.contextBase == 48 * MiB &&
        coarseMoved.contextBytes == dragRequest.contextBytes && coarseMoved.base == 80 * MiB &&
        coarseMoved.bin == dragRequest.bin && coarseClampedLow.contextBase == 0 &&
        coarseClampedHigh.contextBase + coarseClampedHigh.contextBytes == 256 * MiB &&
        fineMove.kind == NavDragKind::Move && fineMoved.base == 72 * MiB &&
        fineMoved.bin == dragRequest.bin && fineMoved.contextBase == dragRequest.contextBase &&
        fineMoved.contextBytes == dragRequest.contextBytes &&
        fineClampedLow.base == dragRequest.contextBase &&
        fineClampedHigh.base + fineClampedHigh.bin * PixelCount ==
            dragRequest.contextBase + dragRequest.contextBytes;
    auto coarseTop = BeginNavigationDrag(dragRequest, 0, 64, 256 * MiB);
    auto coarseTopMoved = DragNavigation(coarseTop, 96);
    auto coarseBottom = BeginNavigationDrag(dragRequest, 0, 320, 256 * MiB);
    auto coarseBottomMoved = DragNavigation(coarseBottom, 352);
    auto fineTop = BeginNavigationDrag(dragRequest, 1, 128, 256 * MiB);
    auto fineTopMoved = DragNavigation(fineTop, 160);
    auto fineBottom = BeginNavigationDrag(dragRequest, 1, 192, 256 * MiB);
    auto fineBottomMoved = DragNavigation(fineBottom, 224);
    dragOkay =
        dragOkay && coarseTop.kind == NavDragKind::Top && coarseTopMoved.contextBase == 48 * MiB &&
        coarseTopMoved.contextBase + coarseTopMoved.contextBytes == 160 * MiB &&
        coarseTopMoved.base == dragRequest.base && coarseBottom.kind == NavDragKind::Bottom &&
        coarseBottomMoved.contextBase == dragRequest.contextBase &&
        coarseBottomMoved.contextBytes == 144 * MiB && fineTop.kind == NavDragKind::Top &&
        fineTopMoved.base == 72 * MiB &&
        fineTopMoved.base + fineTopMoved.bin * PixelCount == 80 * MiB &&
        fineBottom.kind == NavDragKind::Bottom && fineBottomMoved.base == dragRequest.base &&
        fineBottomMoved.base + fineBottomMoved.bin * PixelCount == 88 * MiB;
    auto newRange = BeginNavigationDrag(dragRequest, 1, 256, 256 * MiB);
    auto newSelection = DragNavigation(newRange, 288);
    dragRequest.bin = 1;
    auto tinyPixels = NavigationPixelBounds(NavigationRangeFor(dragRequest, 1, 256 * MiB));
    auto tinyMove =
        BeginNavigationDrag(dragRequest, 1, (tinyPixels.top + tinyPixels.bottom) / 2, 256 * MiB);
    dragOkay = dragOkay && newRange.kind == NavDragKind::NewRange &&
               newSelection.base == 96 * MiB && newSelection.bin == 16 &&
               tinyMove.kind == NavDragKind::Move && tinyPixels.bottom - tinyPixels.top >= 12;
    navigationOkay = navigationOkay && dragOkay;
    std::printf("Self-test drag controls: preserve-span, fixed-opposite-edge, nested-pan, bounds, "
                "new-range, tiny-selection=%d\n",
        dragOkay);
    bool fullscreenOkay = true;
    for (const auto& size :
        std::array<SDL_Point, 4>{{{1920, 1080}, {2560, 1440}, {3840, 2160}, {1080, 1920}}}) {
        auto origin = FullscreenContentPoint(0, 0, size.x, size.y);
        auto end = FullscreenContentPoint(
            static_cast<float>(size.x), static_cast<float>(size.y), size.x, size.y);
        auto fineStart =
            FullscreenContentPoint((SelectionX + StripWidth / 2.0f) * size.x / WindowWidth,
                160.0f * size.y / CanvasHeight,
                size.x,
                size.y);
        auto fineEnd =
            FullscreenContentPoint((SelectionX + StripWidth / 2.0f) * size.x / WindowWidth,
                192.0f * size.y / CanvasHeight,
                size.x,
                size.y);
        auto fullDrag = BeginNavigationDrag(
            Request{64 * MiB, 32, 0, true, false, true, true, 32 * MiB, 128 * MiB},
            NavigationBarAt(fineStart.x, fineStart.y),
            static_cast<int>(std::lround(fineStart.y - CanvasY)),
            256 * MiB);
        auto fullMoved =
            DragNavigation(fullDrag, static_cast<int>(std::lround(fineEnd.y - CanvasY)));
        fullscreenOkay = fullscreenOkay && origin.x == 0 && origin.y == CanvasY &&
                         end.x == WindowWidth && end.y == CanvasY + CanvasHeight &&
                         fullDrag.bar == 1 && fullDrag.kind == NavDragKind::Move &&
                         fullMoved.base == 72 * MiB && fullMoved.bin == 32;
    }
    navigationOkay = navigationOkay && fullscreenOkay;
    std::printf("Self-test fullscreen: landscape/portrait/HiDPI artwork bounds and transformed "
                "selection drag=%d\n",
        fullscreenOkay);
    MiniChanges contextMarks;
    contextMarks.contextBase = 4096;
    contextMarks.contextBytes = 4096;
    MarkMiniMask(contextMarks, *packedMap, packedMap->regions[1], base + 8192, 0xFFFFu);
    navigationOkay = navigationOkay && contextMarks.context[0] == 8 &&
                     contextMarks.context[1] == 8 && contextMarks.bytes == 16;
    ByteStripSampler coarseStrip, fineStrip;
    auto coarseTexture = coarseStrip.Sample(GetCurrentProcess(), packedMap, 0, 8192, true, true);
    auto fineTexture = fineStrip.Sample(GetCurrentProcess(), packedMap, 4096, 4096, true, true);
    bool textureOkay =
        coarseTexture->pixels[0] == MemoryByteColor(MemoryRole::Heap, 32, true) &&
        coarseTexture->pixels[256 * StripWidth] == MemoryByteColor(MemoryRole::Stack, 192, true) &&
        fineTexture->pixels[0] == MemoryByteColor(MemoryRole::Stack, 192, true) &&
        std::all_of(coarseTexture->lastChanged.begin(),
            coarseTexture->lastChanged.end(),
            [](uint32_t time) {
                return time == 0;
            });
    bytes[7] = 99;
    auto changedTexture = coarseStrip.Sample(GetCurrentProcess(), packedMap, 0, 8192, true, true);
    auto unchangedFineTexture =
        fineStrip.Sample(GetCurrentProcess(), packedMap, 4096, 4096, true, true);
    textureOkay = textureOkay &&
                  changedTexture->pixels[7 * StripWidth / 16] ==
                      MemoryByteColor(MemoryRole::Heap, 99, true) &&
                  changedTexture->lastChanged[7 * StripWidth / 16] != 0 &&
                  std::all_of(unchangedFineTexture->lastChanged.begin(),
                      unchangedFineTexture->lastChanged.end(),
                      [](uint32_t time) {
                          return time == 0;
                      });
    auto movedTexture = coarseStrip.Sample(GetCurrentProcess(), packedMap, 4096, 4096, true, true);
    textureOkay = textureOkay && std::all_of(movedTexture->lastChanged.begin(),
                                     movedTexture->lastChanged.end(),
                                     [](uint32_t time) {
                                         return time == 0;
                                     });
    VirtualProtect(bytes + 8192, 4096, PAGE_READWRITE | PAGE_GUARD, &oldProtect);
    auto guardedTexture = fineStrip.Sample(GetCurrentProcess(), packedMap, 4096, 4096, true, true);
    auto guardedMain = MakeFrame(GetCurrentProcess(), packedMap, {0, 1, 0, true, false, true});
    VirtualQuery(bytes + 8192, &guardState, sizeof(guardState));
    textureOkay = textureOkay && guardedTexture->bytesRead == 0 && guardedMain->bytesRead == 4096 &&
                  (guardState.Protect & PAGE_GUARD) &&
                  guardedTexture->pixels[0] == UnsampledColor(MemoryRole::Stack, true, 0, 0) &&
                  guardedTexture->pixels[4] == UnsampledColor(MemoryRole::Stack, true, 4, 0);
    VirtualProtect(bytes + 8192, 4096, PAGE_READWRITE, &oldProtect);
    bytes[8192 + 7] = 193;
    auto recoveredTexture =
        fineStrip.Sample(GetCurrentProcess(), packedMap, 4096, 4096, true, true);
    textureOkay = textureOkay && std::all_of(recoveredTexture->lastChanged.begin(),
                                     recoveredTexture->lastChanged.end(),
                                     [](uint32_t time) {
                                         return time == 0;
                                     });
    bytes[7] = 32;
    bytes[8192 + 7] = 192;
    std::printf(
        "Self-test navigation: nesting=%d sampled-byte-textures=%d\n", navigationOkay, textureOkay);
    okay = okay && navigationOkay && textureOkay;
    // A look-alike segment marker and self pointers are insufficient to label
    // an ordinary VirtualAlloc buffer as a heap without the root signature.
    uint32_t fakeSegment = 0xFFEEFFEEu;
    std::memcpy(bytes + 16, &fakeSegment, 4);
    std::memcpy(bytes + 40, &base, 8);
    std::memcpy(bytes + 48, &base, 8);
    HANDLE heap = HeapCreate(0, 65536, 0);
    void* heapBytes = heap ? HeapAlloc(heap, 0, 1024) : nullptr;
    volatile uint32_t stackSentinel = 7;
    auto ownMap = QueryMap(GetCurrentProcess());
    auto atlas =
        QueryMemoryTypes(GetCurrentProcess(), GetCurrentProcessId(), PrivateRanges(*ownMap));
    ApplyTypes(*ownMap, atlas);
    const Region* ownStack = At(*ownMap, reinterpret_cast<uint64_t>(&stackSentinel));
    const Region* ownHeap = At(*ownMap, reinterpret_cast<uint64_t>(heapBytes));
    const Region* ownPrivate = At(*ownMap, base);
    std::printf(
        "Self-test metadata: supported=%d roots=%zu stacks=%zu threads=%zu failed=%zu read=%llu\n",
        atlas.x64Supported,
        atlas.heapRoots,
        atlas.stacks.size(),
        atlas.threads,
        atlas.failedThreads,
        atlas.bytesRead);
    std::printf(
        "Self-test roles: stack=%s owner=%llu expected-thread=%lu heap=%s direct-allocation=%s\n",
        ownStack ? MemoryRoleName(ownStack->role) : "missing",
        ownStack ? ownStack->owner : 0,
        GetCurrentThreadId(),
        ownHeap ? MemoryRoleName(ownHeap->role) : "missing",
        ownPrivate ? MemoryRoleName(ownPrivate->role) : "missing");
    okay = okay && ownStack && ownStack->role == MemoryRole::Stack &&
           ownStack->owner == GetCurrentThreadId() && heapBytes && ownHeap &&
           ownHeap->role == MemoryRole::Heap && ownPrivate &&
           ownPrivate->role == MemoryRole::Private;
    if (heapBytes)
        HeapFree(heap, 0, heapBytes);
    if (heap)
        HeapDestroy(heap);
    Shared shared;
    UpdateRequest(shared, {0, 1, 0, true, true}, true);
    bool switchedOn = shared.request.types;
    UpdateRequest(shared, {4096, 1, 0, true, false});
    bool preserved = shared.request.types;
    UpdateRequest(shared, shared.request, true);
    okay = okay && switchedOn && preserved && !shared.request.types;
    UpdateRequest(shared, shared.request, false, true);
    bool glowOff = !shared.request.glow;
    UpdateRequest(shared, {4096, 1, 0, true, false});
    bool glowPreserved = !shared.request.glow;
    UpdateRequest(shared, shared.request, false, true);
    okay = okay && glowOff && glowPreserved && shared.request.glow;
    std::printf("Self-test phases: original-mapping=%d type-pixels=%d byte-changes=%d overview=%d "
                "toggle=%d glow-toggle=%d\n",
        mappingOkay,
        colorsOkay,
        changesOkay,
        miniOkay,
        switchedOn && preserved && !shared.request.types,
        glowOff && glowPreserved && shared.request.glow);
    VirtualFree(bytes, 0, MEM_RELEASE);
    std::printf("Address mapping, read coverage, type colors, exact byte differences, independent "
                "overview sampling, guard safety, fade and real heap/stack classification: %s\n",
        okay ? "PASS" : "FAIL");
    return okay;
}
} // namespace

bool RunMemorySelfTests() {
    return SelfTest();
}

int RunVirtualMemoryMap(int argc, char* argv[]) {
    const auto options = ParseViewerOptions(argc, argv);
    if (options.selfTest) {
        return RunMemorySelfTests() ? 0 : 1;
    }
    const DWORD pid = options.pid;
    uint64_t focus = options.focus;
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        std::fprintf(stderr, "Read-only OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }
    auto initial = QueryMap(process);
    if (initial->regions.empty()) {
        CloseHandle(process);
        return 1;
    }
    std::printf("Virtual address space: %llu bytes\nCommitted: %llu | reserved: %llu | readable: "
                "%llu | excluded: %llu\n",
        initial->limit,
        initial->committed,
        initial->reserved,
        initial->readable,
        initial->excluded);
    std::printf("Regions: %zu (readable: %zu). Full region map queried; query/read-only access.\n",
        initial->regions.size(),
        initial->readableRegions);
    if (options.probe) {
        auto atlas = QueryMemoryTypes(process, pid, PrivateRanges(*initial));
        ApplyTypes(*initial, atlas);
        Request request = focus
                              ? Center(PositionOfAddress(*initial, focus), 1, initial->committed, 0)
                              : Request{0, DataBin(*initial), 0, true, true};
        request.types = options.types;
        request.glow = options.glow;
        request = AnchorNavigation(request, initial);
        auto frame = MakeFrame(process, initial, request);
        if (!options.snapshot.empty())
            SaveFrameBitmap(options.snapshot, *frame);
        WriteReport(options.report, pid, *frame, "probe");
        CloseHandle(process);
        return 0;
    }
    char executable[MAX_PATH]{};
    DWORD length = MAX_PATH;
    QueryFullProcessImageNameA(process, 0, executable, &length);
    std::string processName = executable;
    auto slash = processName.find_last_of("\\/");
    if (slash != std::string::npos)
        processName.erase(0, slash + 1);
    if (processName.empty())
        processName = "Process " + std::to_string(pid);
    if (!focus)
        for (const auto& r : initial->regions)
            if (r.readable && r.kind == MEM_PRIVATE) {
                focus = r.base;
                break;
            }
    Shared shared;
    shared.request = Center(PositionOfAddress(*initial, focus),
        std::min<uint64_t>(32, DataBin(*initial)),
        initial->committed,
        0,
        true);
    shared.request.types = options.types;
    if (options.viewBin) {
        const auto viewBin = std::min(options.viewBin, DataBin(*initial));
        shared.request = {options.viewBase, viewBin, 0, true, false, options.types};
    }
    if (options.viewAddress) {
        const auto* region = CommittedAtAddress(*initial, options.viewAddress);
        if (!region) {
            std::fprintf(
                stderr, "Requested view address is not committed: 0x%llX\n", options.viewAddress);
            CloseHandle(process);
            return 1;
        }
        shared.request.base = region->position + options.viewAddress - region->base;
    }
    shared.request.contextBase = options.contextBase;
    shared.request.contextBytes = options.contextBytes;
    if (options.showAll)
        shared.request = {0, DataBin(*initial), 0, true, true, options.types};
    shared.request.glow = options.glow;
    shared.request = AnchorNavigation(shared.request, initial);
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO)) {
        CloseHandle(process);
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("MemGlow | Virtual memory map",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        WindowWidth,
        WindowHeight,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer =
        window
            ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)
            : nullptr;
    if (!renderer && window)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        std::fprintf(stderr, "%s\n", SDL_GetError());
        SDL_Quit();
        CloseHandle(process);
        return 1;
    }
    SDL_RenderSetLogicalSize(renderer, WindowWidth, WindowHeight);
    PresentationKeys presentationKeys;
    SDL_SysWMinfo windowInfo{};
    SDL_VERSION(&windowInfo.version);
    if (SDL_GetWindowWMInfo(window, &windowInfo)) {
        presentationKeys = {windowInfo.info.win.window, SDL_GetWindowID(window)};
        SDL_SetWindowsMessageHook(ForwardPresentationKey, &presentationKeys);
    }
    SDL_Texture* image = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, CanvasWidth, CanvasHeight);
    SDL_Texture* glow = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, CanvasWidth, CanvasHeight);
    SDL_SetTextureBlendMode(glow, SDL_BLENDMODE_ADD);
    std::vector<uint32_t> glowPixels(PixelCount, 0xFF000000u);
    std::array<SDL_Texture*, 2> navTextures{}, navGlows{};
    for (size_t i = 0; i < 2; ++i) {
        navTextures[i] = SDL_CreateTexture(renderer,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING,
            StripWidth,
            CanvasHeight);
        navGlows[i] = SDL_CreateTexture(renderer,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING,
            StripWidth,
            CanvasHeight);
        SDL_SetTextureBlendMode(navGlows[i], SDL_BLENDMODE_ADD);
    }
    std::vector<uint32_t> navGlowPixels(StripWidth * CanvasHeight);
    SDL_Texture* font = FontAtlas(renderer);
    std::array<SDL_Cursor*, 5> cursors{{SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW),
        SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEALL),
        SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENS),
        SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR),
        SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND)}};
    int cursorIndex = -1;
    SDL_StartTextInput();
    std::thread worker(Worker, process, pid, std::ref(shared), options.report);
    std::thread overviewWorker(OverviewWorker, process, std::ref(shared));
    std::shared_ptr<const Frame> shown;
    std::array<std::shared_ptr<const StripFrame>, 2> navShown{};
    NavigationDrag navigationDrag;
    CanvasDrag canvasDrag;
    WheelState wheelState;
    bool navigationMoved = false, canvasMoved = false;
    auto cancelDrag = [&] {
        navigationDrag = {};
        canvasDrag = {};
        navigationMoved = canvasMoved = false;
        SDL_CaptureMouse(SDL_FALSE);
    };
    bool fullscreen = false;
    auto configurePresentation = [&] {
        if (fullscreen) {
            int width = 0, height = 0;
            SDL_GetRendererOutputSize(renderer, &width, &height);
            SDL_RenderSetScale(renderer,
                static_cast<float>(std::max(1, width)) / WindowWidth,
                static_cast<float>(std::max(1, height)) / CanvasHeight);
            SDL_RenderSetViewport(renderer, nullptr);
        } else
            SDL_RenderSetLogicalSize(renderer, WindowWidth, WindowHeight);
    };
    auto setFullscreen = [&](bool enabled) {
        if (enabled == fullscreen)
            return;
        cancelDrag();
        if (SDL_SetWindowFullscreen(window, enabled ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0)) {
            std::fprintf(stderr, "Fullscreen switch failed: %s\n", SDL_GetError());
            return;
        }
        fullscreen = enabled;
        shared.fullscreen = enabled;
        // Disable SDL's logical mouse scaling while the artwork fills the desktop.
        if (fullscreen)
            SDL_RenderSetLogicalSize(renderer, 0, 0);
        configurePresentation();
    };
    auto eventPoint = [&](float x, float y) {
        if (!fullscreen)
            return ContentPoint{x, y}; // SDL already scales windowed mouse events.
        int width = 0, height = 0;
        SDL_GetWindowSize(window, &width, &height);
        return FullscreenContentPoint(x, y, width, height);
    };
    auto windowPoint = [&](int x, int y) {
        if (fullscreen)
            return eventPoint(static_cast<float>(x), static_cast<float>(y));
        ContentPoint point{};
        SDL_RenderWindowToLogical(renderer, x, y, &point.x, &point.y);
        return point;
    };
    if (options.fullscreen)
        setFullscreen(true);
    uint32_t lastTypeKey = 0, lastGlowKey = 0;
    auto lastTitle = Clock::time_point{};
    bool running = true;
    while (running && !shared.targetEnded.load()) {
        auto current = std::atomic_load(&shared.frame);
        auto activity = std::atomic_load(&shared.overviewActivity);
        auto strips = std::atomic_load(&shared.strips);
        if (current && current != shown) {
            shown = current;
            SDL_UpdateTexture(image, nullptr, shown->pixels.data(), CanvasWidth * 4);
        }
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
                break;
            }
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
                if (!event.key.repeat) {
                    if (fullscreen)
                        setFullscreen(false);
                    else
                        running = false;
                }
                continue;
            }
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F11) {
                if (!event.key.repeat)
                    setFullscreen(!fullscreen);
                continue;
            }
            if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                    cancelDrag();
                if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    event.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) {
                    cancelDrag();
                    configurePresentation();
                }
            }
            if (!shown)
                continue;
            if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_KEYDOWN ||
                event.type == SDL_TEXTINPUT)
                wheelState = {};
            Request request;
            {
                std::lock_guard<std::mutex> lock(shared.mutex);
                request = shared.request;
            }
            request = RebaseNavigation(request, shown->map);
            const auto& map = *shown->map;
            uint64_t limit = Limit(map, request.packed);
            if (event.type != SDL_MOUSEWHEEL)
                request = FitNavigation(request, limit);
            if (canvasDrag.active && canvasDrag.seed.coordinates != shown->map) {
                canvasDrag.seed = RebaseNavigation(canvasDrag.seed, shown->map);
                canvasDrag.limit = limit;
                if (AnchorUnavailable(canvasDrag.seed))
                    cancelDrag();
            }
            if (navigationDrag.kind != NavDragKind::None &&
                navigationDrag.seed.coordinates != shown->map) {
                navigationDrag.seed = RebaseNavigation(navigationDrag.seed, shown->map);
                navigationDrag.limit = limit;
                navigationDrag.range =
                    NavigationRangeFor(navigationDrag.seed, navigationDrag.bar, limit);
                if (AnchorUnavailable(navigationDrag.seed))
                    cancelDrag();
            }
            if (canvasDrag.active && (!CanvasDragMatches(canvasDrag, request, limit) ||
                                         event.type == SDL_KEYDOWN || event.type == SDL_TEXTINPUT))
                cancelDrag();
            auto updateRequest = [&](Request next, bool types = false, bool glow = false) {
                next.coordinates = shown->map;
                UpdateRequest(shared, std::move(next), types, glow);
            };
            auto home = [&] {
                updateRequest({0, DataBin(map), 0, true, true});
            };
            auto toggleTypes = [&] {
                updateRequest(request, true);
            };
            auto toggleGlow = [&] {
                updateRequest(request, false, true);
            };
            auto dragCanvasTo = [&](ContentPoint point) {
                if (canvasMoved || std::abs(point.y - canvasDrag.anchorY) >= 2) {
                    canvasMoved = true;
                    auto next = DragCanvas(canvasDrag, point.y);
                    if (next.base != request.base)
                        updateRequest(next);
                }
            };
            if (event.type == SDL_MOUSEBUTTONDOWN) {
                if (event.button.button == SDL_BUTTON_RIGHT) {
                    cancelDrag();
                    home();
                    continue;
                }
                if (event.button.button == SDL_BUTTON_LEFT)
                    cancelDrag();
                auto point = eventPoint(
                    static_cast<float>(event.button.x), static_cast<float>(event.button.y));
                float mx = point.x, my = point.y;
                if (!fullscreen && event.button.button == SDL_BUTTON_LEFT) {
                    const auto action = ToolbarActionAt(point);
                    switch (action) {
                        case ToolbarAction::Fullscreen:
                            setFullscreen(true);
                            break;
                        case ToolbarAction::All:
                            home();
                            break;
                        case ToolbarAction::Types:
                            toggleTypes();
                            break;
                        case ToolbarAction::Glow:
                            toggleGlow();
                            break;
                        case ToolbarAction::None:
                            break;
                    }
                    if (action != ToolbarAction::None) {
                        continue;
                    }
                }
                int bar = NavigationBarAt(mx, my);
                if (event.button.button == SDL_BUTTON_LEFT && bar >= 0) {
                    uint64_t shownLimit = Limit(map, shown->request.packed);
                    navigationDrag =
                        BeginNavigationDrag(DisplayedNavigationRequest(*shown, request, bar),
                            bar,
                            static_cast<int>(my - CanvasY),
                            shownLimit);
                    navigationMoved = false;
                    if (navigationDrag.kind == NavDragKind::NewRange)
                        updateRequest(DragNavigation(navigationDrag, navigationDrag.anchorY));
                    SDL_CaptureMouse(SDL_TRUE);
                    continue;
                }
                if (event.button.button == SDL_BUTTON_LEFT && InCanvas(point)) {
                    canvasDrag = BeginCanvasDrag(request, point, limit);
                    if (canvasDrag.active)
                        SDL_CaptureMouse(SDL_TRUE);
                }
            } else if (event.type == SDL_MOUSEMOTION && canvasDrag.active &&
                       (event.motion.state & SDL_BUTTON_LMASK)) {
                dragCanvasTo(eventPoint(
                    static_cast<float>(event.motion.x), static_cast<float>(event.motion.y)));
            } else if (event.type == SDL_MOUSEMOTION && navigationDrag.kind != NavDragKind::None &&
                       (event.motion.state & SDL_BUTTON_LMASK)) {
                int y = static_cast<int>(eventPoint(static_cast<float>(event.motion.x),
                                             static_cast<float>(event.motion.y))
                                             .y -
                                         CanvasY);
                if (navigationMoved || std::abs(y - navigationDrag.anchorY) >= 2) {
                    navigationMoved = true;
                    updateRequest(DragNavigation(navigationDrag, y));
                }
            } else if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) {
                if (canvasDrag.active)
                    dragCanvasTo(eventPoint(
                        static_cast<float>(event.button.x), static_cast<float>(event.button.y)));
                if (navigationMoved)
                    updateRequest(DragNavigation(navigationDrag,
                        static_cast<int>(eventPoint(static_cast<float>(event.button.x),
                                             static_cast<float>(event.button.y))
                                             .y -
                                         CanvasY)));
                cancelDrag();
            } else if (event.type == SDL_MOUSEWHEEL) {
                if (canvasDrag.active || navigationDrag.kind != NavDragKind::None)
                    continue;
#if SDL_VERSION_ATLEAST(2, 26, 0)
                // SDL already scales event coordinates in windowed mode. Use
                // the position of this wheel event, even if the pointer moved on.
                auto point = eventPoint(
                    static_cast<float>(event.wheel.mouseX), static_cast<float>(event.wheel.mouseY));
#else
                int wx = 0, wy = 0;
                SDL_GetMouseState(&wx, &wy);
                auto point = windowPoint(wx, wy);
#endif
                uint64_t maxBin = request.packed ? DataBin(map) : FullBin(map.limit);
                auto next = WheelAtPoint(request,
                    point,
                    limit,
                    maxBin,
                    WheelNotches(event.wheel),
                    (SDL_GetModState() & KMOD_SHIFT) != 0,
                    wheelState);
                if (next.base != request.base || next.bin != request.bin ||
                    next.contextBase != request.contextBase ||
                    next.contextBytes != request.contextBytes || next.overview != request.overview)
                    updateRequest(next);
            } else if (event.type == SDL_TEXTINPUT) {
                char key =
                    static_cast<char>(std::tolower(static_cast<unsigned char>(event.text.text[0])));
                if (key == 'a')
                    home();
                else if (key == 't' && (lastTypeKey == 0 || SDL_GetTicks() - lastTypeKey > 100))
                    toggleTypes();
                else if (key == 'f' && (lastGlowKey == 0 || SDL_GetTicks() - lastGlowKey > 100))
                    toggleGlow();
            } else if (event.type == SDL_KEYDOWN) {
                SDL_Keycode key = event.key.keysym.sym;
                if (key == SDLK_a || key == SDLK_HOME)
                    home();
                else if (key == SDLK_t) {
                    lastTypeKey = SDL_GetTicks();
                    if (!event.key.repeat)
                        toggleTypes();
                } else if (key == SDLK_f) {
                    lastGlowKey = SDL_GetTicks();
                    if (!event.key.repeat)
                        toggleGlow();
                } else if (key == SDLK_TAB) {
                    uint64_t middle = request.base + request.bin * PixelCount / 2;
                    uint64_t address = request.packed ? PositionOfAddress(map, focus) : focus;
                    for (const auto& r : map.regions)
                        if (r.readable && Position(r, request.packed) > middle) {
                            address = Position(r, request.packed);
                            break;
                        }
                    updateRequest(Center(address, 1, limit, 0, request.packed));
                } else if (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_PAGEUP ||
                           key == SDLK_PAGEDOWN) {
                    uint64_t amount = request.bin * PixelCount /
                                      ((key == SDLK_LEFT || key == SDLK_RIGHT) ? 4 : 1);
                    uint64_t maxBase =
                        limit > request.bin * PixelCount ? limit - request.bin * PixelCount : 0;
                    bool back = key == SDLK_LEFT || key == SDLK_PAGEUP;
                    uint64_t base = back ? request.base > amount ? request.base - amount : 0
                                         : std::min(maxBase, request.base + amount);
                    updateRequest({base, request.bin, 0, request.packed, false});
                }
            }
        }
        if (!running)
            break;
        Request pendingNavigation;
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            pendingNavigation = shared.request;
        }
        if (fullscreen)
            configurePresentation();
        int pointerX = 0, pointerY = 0;
        SDL_GetMouseState(&pointerX, &pointerY);
        auto pointer = windowPoint(pointerX, pointerY);
        float pointerLogicalX = pointer.x, pointerLogicalY = pointer.y;
        int hoveredBar = NavigationBarAt(pointerLogicalX, pointerLogicalY);
        NavDragKind hoverKind = NavDragKind::None;
        if (shown && hoveredBar >= 0) {
            const auto navigation =
                DisplayedNavigationRequest(*shown, pendingNavigation, hoveredBar);
            const auto range = NavigationRangeFor(
                navigation, hoveredBar, Limit(*shown->map, shown->request.packed));
            hoverKind = NavigationHit(range, pointerLogicalY - CanvasY);
        }
        NavDragKind pointerKind =
            navigationDrag.kind != NavDragKind::None ? navigationDrag.kind : hoverKind;
        int nextCursor = canvasDrag.active || pointerKind == NavDragKind::Move ? 1
                         : (pointerKind == NavDragKind::Top || pointerKind == NavDragKind::Bottom)
                             ? 2
                         : pointerKind == NavDragKind::NewRange ? 3
                         : InCanvas(pointer)                    ? 4
                                                                : 0;
        if (nextCursor != cursorIndex && cursors[nextCursor]) {
            SDL_SetCursor(cursors[nextCursor]);
            cursorIndex = nextCursor;
        }
        SDL_SetRenderDrawColor(renderer, 9, 17, 12, 255);
        SDL_RenderClear(renderer);
        SDL_SetTextureColorMod(font, 183, 235, 197);
        char line[256]{};
        if (!fullscreen) {
            std::snprintf(line, sizeof(line), "%s | PID %lu | MEMORY", processName.c_str(), pid);
            Text(renderer, font, 8, 8, line, 2, ToolbarLeft - 16);
            if (!shown)
                Text(renderer, font, 8, 34, "QUERYING THE COMPLETE VIRTUAL ADDRESS SPACE...");
        }
        if (shown) {
            const auto& map = *shown->map;
            uint64_t limit = Limit(map, shown->request.packed);
            if (!fullscreen) {
                std::snprintf(line,
                    sizeof(line),
                    "COMMIT %.1F MIB | READABLE %.1F MIB | RESERVE %.2F GIB | UNSAMPLED %.1F MIB",
                    map.committed / 1048576.0,
                    map.readable / 1048576.0,
                    map.reserved / 1073741824.0,
                    map.excluded / 1048576.0);
                Text(renderer, font, 8, 34, line, 2, WindowWidth - 16);
                uint64_t last = DetailEnd(shown->request, limit) - 1;
                if (shown->request.packed)
                    std::snprintf(line,
                        sizeof(line),
                        "CONTEXT %.1F MIB | DETAIL %.1F MIB | %llu B/PIXEL | UNUSED "
                        "ADDRESS GAPS FOLDED",
                        shown->request.contextBytes / 1048576.0,
                        DetailSpan(shown->request, limit) / 1048576.0,
                        shown->request.bin);
                else
                    std::snprintf(line,
                        sizeof(line),
                        "0x%012llX - 0x%012llX | %llu B/PIXEL | %s",
                        shown->request.base,
                        last,
                        shown->request.bin,
                        shown->byteView ? "BYTE CONTENT" : "REGION STATES");
                Text(renderer, font, CanvasX, CanvasY - 27, line, 2, CanvasWidth);
            }
            int artY = fullscreen ? 0 : CanvasY;
            SDL_Rect canvas{CanvasX, artY, CanvasWidth, CanvasHeight};
            SDL_RenderCopy(renderer, image, nullptr, &canvas);
            if (AnchorUnavailable(shown->request)) {
                SDL_Rect message{CanvasX + 160, artY + CanvasHeight / 2 - 28, 704, 56};
                SDL_SetRenderDrawColor(renderer, 9, 17, 12, 255);
                SDL_RenderFillRect(renderer, &message);
                Text(renderer,
                    font,
                    message.x + 16,
                    message.y + 16,
                    AnchorMessage(shown->request),
                    2);
            }
            if (GlowPixels(*shown, NowMs(), glowPixels)) {
                SDL_UpdateTexture(glow, nullptr, glowPixels.data(), CanvasWidth * 4);
                SDL_RenderSetClipRect(renderer, &canvas);
                SDL_SetTextureAlphaMod(glow, 45);
                for (const auto& offset :
                    std::array<SDL_Point, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
                    SDL_Rect halo{CanvasX + offset.x, artY + offset.y, CanvasWidth, CanvasHeight};
                    SDL_RenderCopy(renderer, glow, nullptr, &halo);
                }
                SDL_SetTextureAlphaMod(glow, 255);
                SDL_RenderCopy(renderer, glow, nullptr, &canvas);
                SDL_RenderSetClipRect(renderer, nullptr);
            }
            auto drawStrip = [&](size_t which) {
                bool fine = which == 1;
                const auto navigation =
                    DisplayedNavigationRequest(*shown, pendingNavigation, static_cast<int>(which));
                int x = fine ? SelectionX : OverviewX;
                uint64_t domainBase = fine ? shown->request.contextBase : 0;
                uint64_t domainBytes = fine ? shown->request.contextBytes : limit;
                uint64_t selectedBase = fine ? navigation.base : navigation.contextBase;
                uint64_t selectedBytes =
                    fine ? DetailSpan(navigation, limit) : navigation.contextBytes;
                auto sampled = strips ? fine ? strips->selection : strips->overview : nullptr;
                SDL_Rect strip{x, artY, StripWidth, CanvasHeight};
                SDL_SetRenderDrawColor(renderer, 0, 5, 2, 255);
                SDL_RenderFillRect(renderer, &strip);
                sampled = RenderStripSnapshot(
                    renderer, navTextures[which], strip, shown->request, sampled, navShown[which]);
                if (sampled) {
                    if (StripGlowPixels(
                            *shown, activity.get(), *sampled, fine, NowMs(), navGlowPixels)) {
                        SDL_UpdateTexture(
                            navGlows[which], nullptr, navGlowPixels.data(), StripWidth * 4);
                        SDL_RenderSetClipRect(renderer, &strip);
                        SDL_SetTextureAlphaMod(navGlows[which], 45);
                        for (int offset : {-1, 1}) {
                            SDL_Rect halo{x, artY + offset, StripWidth, CanvasHeight};
                            SDL_RenderCopy(renderer, navGlows[which], nullptr, &halo);
                        }
                        SDL_SetTextureAlphaMod(navGlows[which], 255);
                        SDL_RenderCopy(renderer, navGlows[which], nullptr, &strip);
                        SDL_RenderSetClipRect(renderer, nullptr);
                    }
                } else if (!fullscreen)
                    Text(renderer, font, x + 6, CanvasY + 8, "SAMPLING", 1);
                if (!fine || !AnchorUnavailable(shown->request)) {
                    auto selectedPixels = NavigationPixelBounds(
                        {domainBase, domainBytes, selectedBase, selectedBytes});
                    int top = selectedPixels.top, bottom = selectedPixels.bottom;
                    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 140);
                    SDL_Rect before{x, artY, StripWidth, top},
                        after{x, artY + bottom, StripWidth, CanvasHeight - bottom};
                    SDL_RenderFillRect(renderer, &before);
                    SDL_RenderFillRect(renderer, &after);
                    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
                    NavDragKind handleKind =
                        navigationDrag.bar == static_cast<int>(which) ? navigationDrag.kind
                        : hoveredBar == static_cast<int>(which)       ? hoverKind
                                                                      : NavDragKind::None;
                    SDL_SetRenderDrawColor(renderer,
                        handleKind == NavDragKind::Move ? 220 : 160,
                        255,
                        handleKind == NavDragKind::Move ? 235 : 193,
                        255);
                    SDL_Rect border{x - 1, artY + top, StripWidth + 2, bottom - top};
                    SDL_RenderDrawRect(renderer, &border);
                    for (bool upper : {true, false}) {
                        bool hot = handleKind == (upper ? NavDragKind::Top : NavDragKind::Bottom);
                        SDL_SetRenderDrawColor(
                            renderer, hot ? 240 : 160, 255, hot ? 245 : 193, 255);
                        int edgeY = upper ? artY + top : artY + bottom - 3;
                        SDL_Rect edge{x - 2, edgeY, StripWidth + 4, 3},
                            grip{x + StripWidth / 2 - 9, edgeY - 1, 18, 5};
                        SDL_RenderFillRect(renderer, &edge);
                        SDL_RenderFillRect(renderer, &grip);
                    }
                }
                if (!fullscreen) {
                    Text(renderer, font, x, CanvasY - 27, fine ? "SELECTION" : "OVERVIEW", 2);
                    std::snprintf(line,
                        sizeof(line),
                        "0x%012llX",
                        AddressOfPosition(map, domainBase, shown->request.packed));
                    Text(renderer, font, x, CanvasY - 11, line, 1);
                    std::snprintf(line,
                        sizeof(line),
                        "0x%012llX",
                        AddressOfPosition(
                            map, domainBase + domainBytes - 1, shown->request.packed));
                    Text(renderer, font, x, CanvasY + CanvasHeight + 5, line, 1);
                }
            };
            drawStrip(0);
            drawStrip(1);
            if (shown->request.packed && !AnchorUnavailable(shown->request)) {
                for (const auto& r : map.regions)
                    if (r.state == MEM_COMMIT && r.position >= shown->request.base &&
                        r.position < DetailEnd(shown->request, limit)) {
                        uint32_t color =
                            shown->request.types ? MemoryRoleColor(r.role) : 0xFF156F3Du;
                        SDL_SetRenderDrawColor(
                            renderer, (color >> 16) & 255, (color >> 8) & 255, color & 255, 255);
                        uint64_t index = (r.position - shown->request.base) / shown->request.bin;
                        SDL_Rect tick{CanvasX + static_cast<int>(index % CanvasWidth),
                            artY + static_cast<int>(index / CanvasWidth),
                            1,
                            2};
                        SDL_RenderFillRect(renderer, &tick);
                    }
            }
            if (!fullscreen) {
                DrawToolbar(renderer, font, shown->request);
                DrawMemoryLegend(renderer, font, shown->request.types);
                int wx = 0, wy = 0;
                SDL_GetMouseState(&wx, &wy);
                float mx = 0, my = 0;
                SDL_RenderWindowToLogical(renderer, wx, wy, &mx, &my);
                if (mx >= CanvasX && mx < CanvasX + CanvasWidth && my >= CanvasY &&
                    my < CanvasY + CanvasHeight) {
                    uint64_t index = static_cast<uint64_t>(my - CanvasY) * CanvasWidth +
                                     static_cast<uint64_t>(mx - CanvasX);
                    uint64_t position = shown->request.base + index * shown->request.bin;
                    bool padding = position >= DetailEnd(shown->request, limit);
                    uint64_t address =
                        padding ? 0 : AddressOfPosition(map, position, shown->request.packed);
                    const auto* region = padding ? nullptr : At(map, address);
                    std::string attributes = region ? PageAttributes(*region) : "";
                    if (AnchorUnavailable(shown->request))
                        std::snprintf(line,
                            sizeof(line),
                            "0x%012llX | %s",
                            shown->request.anchor.address,
                            AnchorMessage(shown->request));
                    else if (region && region->owner &&
                             (region->role == MemoryRole::Stack ||
                                 region->role == MemoryRole::Heap))
                        std::snprintf(line,
                            sizeof(line),
                            "0x%012llX | %s %llu | %s",
                            address,
                            region->role == MemoryRole::Stack ? "THREAD" : "HEAP",
                            region->owner,
                            attributes.c_str());
                    else
                        std::snprintf(line,
                            sizeof(line),
                            "0x%012llX | %s | %s",
                            address,
                            padding ? "PADDING" : Kind(region),
                            attributes.c_str());
                    Text(renderer, font, 8, CanvasY + CanvasHeight + 45, line);
                }
                std::snprintf(line,
                    sizeof(line),
                    "%llu B / %llu PX CHANGED",
                    shown->changedBytes,
                    shown->changedPixels);
                Text(renderer, font, 8, CanvasY + CanvasHeight + 66, line);
            }
            if (Clock::now() - lastTitle >= std::chrono::milliseconds(500)) {
                std::snprintf(line,
                    sizeof(line),
                    "MemGlow | %s | %.1f MiB readable | %s | %s | %llu B/pixel | GLOW %s",
                    processName.c_str(),
                    map.readable / 1048576.0,
                    shown->request.types ? "5 TYPE COLORS" : "GREEN",
                    shown->request.packed ? "all committed memory" : "virtual addresses",
                    shown->request.bin,
                    shown->request.glow ? "ON" : "OFF");
                SDL_SetWindowTitle(window, line);
                lastTitle = Clock::now();
            }
        }
        SDL_RenderPresent(renderer);
        SDL_Delay(1);
    }
    SDL_CaptureMouse(SDL_FALSE);
    SDL_SetWindowsMessageHook(nullptr, nullptr);
    shared.stop = true;
    shared.requestChanged.notify_one();
    worker.join();
    overviewWorker.join();
    SDL_SetCursor(SDL_GetDefaultCursor());
    for (auto cursor : cursors)
        if (cursor)
            SDL_FreeCursor(cursor);
    SDL_StopTextInput();
    SDL_DestroyTexture(font);
    for (auto texture : navGlows)
        SDL_DestroyTexture(texture);
    for (auto texture : navTextures)
        SDL_DestroyTexture(texture);
    SDL_DestroyTexture(glow);
    SDL_DestroyTexture(image);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    CloseHandle(process);
    return 0;
}
#endif
