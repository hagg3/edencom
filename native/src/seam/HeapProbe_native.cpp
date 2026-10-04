// HeapProbe_native.cpp — native twin of web/src/seam/HeapProbe_web.mm (Phase N Stage 1,
// WORKING/native-migration-plan-2026-09-04.md). Pure measurement, no behaviour change.
//
// Stage 1's criterion 3 is "report peak RSS at menu / 64z / 256z, in the same three phases as
// web/tools/headless-heap-ceiling-probe.js, so the figures are directly comparable" — and the
// number that decides the plan's kill criterion (>250 MB at 64z → stop and re-plan) has to come
// from the process itself, because there is no browser here to ask.
//
// WHAT IS AND IS NOT COMPARABLE TO THE WEB NUMBERS, stated up front because getting this wrong
// would make the kill criterion meaningless:
//
//   * `heapSize`/`sbrkTop`/`peakSbrkTop` on web are *linear memory* — the wasm heap, which is the
//     engine's allocations and nothing else. The closest native analogue is the allocator's own
//     in-use figure (`malloc_zone_statistics`), reported here under the same key names so a probe
//     can read either target with one parser. It is NOT the same quantity: wasm linear memory
//     includes ~62 MB of static data that native carries in its own mapped segments instead.
//   * `rss` / `footprint` have NO web equivalent at all — on web the process is the browser, and
//     Phase V6 measured that Chrome spends 747 MB of browser-wide RSS to run a heap of ~130 MB.
//     They are the numbers Stage 1 actually exists to produce: what this engine costs when the
//     browser is not in the picture.
//   * `phys_footprint` is the same accounting iOS kills apps on and the same one
//     tools/browser-memory-probe.js reads for Safari, so it is the honest cross-platform column.
//
// Deliberately NOT gated on EDEN_DIAGNOSTICS, same as the web twin: a memory figure that only
// exists in a debug build is a figure you cannot take when it matters.
//
// PHASE N STAGE 3.4 — THREE IMPLEMENTATIONS, ONE JSON SHAPE. Stage 1's version was Mach-only with
// a `getrusage` stub for "not Apple", and that stub was wrong in a way worth naming: `ru_maxrss`
// is the process PEAK, not the current RSS, so `rss` and `peakRss` would have been the same
// number forever and the phase-by-phase comparison Stage 1's criterion 3 is built on would have
// been meaningless. The three real implementations:
//
//   Apple    task_info(TASK_VM_INFO) + malloc_zone_statistics.
//   Linux    /proc/self/status (VmRSS now, VmHWM peak) + glibc mallinfo2() for live bytes.
//   Windows  GetProcessMemoryInfo (WorkingSetSize / PeakWorkingSetSize / PrivateUsage).
//
// THE ONE HONEST GAP IS WINDOWS' LIVE-ALLOCATION FIGURE. Apple's allocator publishes
// `size_in_use` and glibc publishes `uordblks`; the mingw CRT publishes nothing equivalent, and
// the alternatives (a debug CRT, or interposing malloc) are either MSVC-only or the AllocTrace
// machinery web keeps behind its own build flag. So Windows reports PROCESS COMMIT CHARGE
// (`PrivateUsage`) in the alloc slot instead, and this is a deliberate choice rather than a
// fallback: `headless-alloc-leak-probe`'s actual test is "is this figure FLAT across N world
// loads", and commit charge answers that at least as well as a CRT statistic would — it also
// catches a leak the CRT would not attribute, at the cost of being coarser. Read a Windows
// `live` number as a trend, never as a byte count.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach/vm_statistics.h>
#include <malloc/malloc.h>
#include <algorithm>
#include <map>
#elif defined(_WIN32)
// This file reaches no Foundation and no engine header, so <windows.h> is contained here and
// cannot collide with the ObjC `BOOL` the way it would anywhere the shim is in scope. See
// native/src/shim/gl/framework/GLES/gl.h for the place where that containment actually mattered.
#include <windows.h>
#include <psapi.h>
#else
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#endif

namespace {

uint64_t g_peakRss = 0;
uint64_t g_peakFootprint = 0;
uint64_t g_peakAllocInUse = 0;

struct MemSample {
    uint64_t rss = 0;         // resident_size — pages actually in physical memory
    uint64_t footprint = 0;   // phys_footprint — Apple's "what this process costs" accounting
    uint64_t allocInUse = 0;  // bytes the malloc zones have handed out to the program
    uint64_t allocMax = 0;    // high-water of the above, as the allocator itself tracks it
};

#if !defined(__APPLE__) && !defined(_WIN32)
// One pass over /proc/self/status for both VmRSS (current) and VmHWM (peak). Both are printed in
// kB. Reading the file rather than /proc/self/statm because statm's numbers are in pages and carry
// no high-water mark, and the peak is half of what this probe is for.
void read_proc_status(uint64_t* rssBytes, uint64_t* hwmBytes) {
    *rssBytes = 0;
    *hwmBytes = 0;
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long kb = 0;
        if (sscanf(line, "VmRSS: %llu kB", &kb) == 1) *rssBytes = kb * 1024ull;
        else if (sscanf(line, "VmHWM: %llu kB", &kb) == 1) *hwmBytes = kb * 1024ull;
    }
    fclose(f);
}
#endif

MemSample sample() {
    MemSample s;
#if defined(__APPLE__)
    task_vm_info_data_t vmInfo;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vmInfo, &count) == KERN_SUCCESS) {
        s.footprint = (uint64_t)vmInfo.phys_footprint;
        s.rss = (uint64_t)vmInfo.resident_size;
    }
    malloc_statistics_t ms;
    malloc_zone_statistics(nullptr, &ms);  // nullptr = "all zones"
    s.allocInUse = (uint64_t)ms.size_in_use;
    s.allocMax = (uint64_t)ms.max_size_in_use;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc;
    memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        s.rss = (uint64_t)pmc.WorkingSetSize;
        // PrivateUsage is commit charge: the closest thing Windows has to "what this process
        // costs", and the counter that matters if it is ever competing for memory. It is also the
        // alloc proxy — see this file's header for why that is a choice and not a shortfall.
        s.footprint = (uint64_t)pmc.PrivateUsage;
        s.allocInUse = (uint64_t)pmc.PrivateUsage;
        s.allocMax = (uint64_t)pmc.PeakPagefileUsage;
        if ((uint64_t)pmc.PeakWorkingSetSize > g_peakRss) g_peakRss = (uint64_t)pmc.PeakWorkingSetSize;
    }
#else
    uint64_t hwm = 0;
    read_proc_status(&s.rss, &hwm);
    // No phys_footprint equivalent on Linux. RSS is the honest column here, and saying so is
    // better than inventing a third number that looks like Apple's and is not comparable to it.
    s.footprint = s.rss;
    if (hwm > g_peakRss) g_peakRss = hwm;   // the kernel's high-water beats any sampled one
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    // mallinfo2 is the 64-bit-clean replacement for mallinfo, whose fields are `int` and therefore
    // wrap at 2 GB — which this port would reach on a 256z world. glibc 2.33+, i.e. Ubuntu 22.04+.
    struct mallinfo2 mi = mallinfo2();
    s.allocInUse = (uint64_t)mi.uordblks;               // live bytes: the M6 leak signal
    s.allocMax = (uint64_t)mi.uordblks + (uint64_t)mi.fordblks;
#else
    s.allocInUse = s.rss;
    s.allocMax = s.rss;
#endif
#endif
    if (s.rss > g_peakRss) g_peakRss = s.rss;
    if (s.footprint > g_peakFootprint) g_peakFootprint = s.footprint;
    if (s.allocInUse > g_peakAllocInUse) g_peakAllocInUse = s.allocInUse;
    return s;
}

}  // namespace

extern "C" {

// Same name, same shape (a JSON object as a C string in a static buffer), same caller contract as
// the web twin — so one probe script can read `eden_debug_heap()` on either target. The three
// web-shaped keys come first and mean what the comment at the top of this file says they mean.
const char* eden_debug_heap(void) {
    static char buf[384];
    MemSample s = sample();
    std::snprintf(buf, sizeof(buf),
        "{\"heapSize\":%llu,\"sbrkTop\":%llu,\"peakSbrkTop\":%llu,\"heapMax\":0,\"usedPct\":0.0,"
        "\"rss\":%llu,\"peakRss\":%llu,\"footprint\":%llu,\"peakFootprint\":%llu,"
        "\"allocMax\":%llu}",
        (unsigned long long)s.allocInUse,
        (unsigned long long)s.allocInUse,
        (unsigned long long)g_peakAllocInUse,
        (unsigned long long)s.rss,
        (unsigned long long)g_peakRss,
        (unsigned long long)s.footprint,
        (unsigned long long)g_peakFootprint,
        (unsigned long long)s.allocMax);
    return buf;
}

void eden_debug_heap_reset_peak(void) {
    MemSample s = sample();
    g_peakRss = s.rss;
    g_peakFootprint = s.footprint;
    g_peakAllocInUse = s.allocInUse;
}

// The web twin warns as linear memory approaches -sMAXIMUM_MEMORY, because there the cap is a hard
// abort. Native has no such cap — it has the OS, which is a different failure with a different
// remedy — so this keeps the same name and call site (the frame loop) but does the one thing that
// IS useful here: keep the peak trackers honest between explicit eden_debug_heap() calls, so a
// peak that happens between two probe samples is not missed. Throttled to once every 600 frames,
// same as the web twin, because task_info() is cheap but not free.
void eden_heap_pressure_tick(void) {
    static int frames = 0;
    if (++frames < 600) return;
    frames = 0;
    (void)sample();
}

// Web reports emmalloc's live-bytes statistic here to tell a leak from fragmentation. The macOS
// allocator exposes the same distinction directly (`size_in_use` live, `max_size_in_use` its
// high-water) and so does glibc (`uordblks` live, `fordblks` free-and-held). Windows has no such
// statistic and reports commit charge instead — a trend, not a byte count; see this file's header.
// Reported under the web twin's key names for the same one-parser reason.
const char* eden_debug_alloc(void) {
    static char buf[192];
    MemSample s = sample();
    std::snprintf(buf, sizeof(buf),
        "{\"dynHeap\":%llu,\"freeDyn\":%llu,\"live\":%llu}",
        (unsigned long long)s.allocMax,
        (unsigned long long)(s.allocMax > s.allocInUse ? s.allocMax - s.allocInUse : 0),
        (unsigned long long)s.allocInUse);
    return buf;
}

// ---- N.4.9: where the footprint goes (--mem-trace) -------------------------------------------
//
// WHY THIS EXISTS. On the iPad Air 2 a GL-on `--stage1` reads phys_footprint 356 MB at the menu
// against RSS 89 MB and a malloc heap of 17 MB, while the same run headless reads 16 MB. So ~270 MB
// is CHARGED to the process without being RESIDENT in it — which is how iOS bills memory the GPU
// driver allocates on the process's behalf. Instruments' VM Tracker would split that up; this does
// the same thing from inside the process, so a device run over SSH can answer it.
//
// Two sources, both read-only:
//   1. task_info(TASK_VM_INFO)'s LEDGERS. phys_footprint is a sum the kernel keeps per task:
//      internal (dirty anonymous) + compressed + IOKit-mapped + the tagged ledgers (graphics,
//      media, neural, network) + purgeable-nonvolatile + page tables. The struct exposes most
//      terms; `rest` below is the footprint minus every term it exposes, i.e. what is left for
//      IOKit mappings and page tables.
//   2. A walk of the address space (vm_region_recurse_64), summing dirty+swapped and resident
//      bytes per VM tag. That names WHO owns the mapped part (MALLOC_*, IOKit, IOSurface,
//      IOAccelerator, CoreAnimation, GLSL, ...).
// Off unless `--mem-trace` sets g_eden_mem_trace; each call is a full region walk (a few ms).
int g_eden_mem_trace = 0;
extern "C" const char* eden_debug_gl_buffer_bytes(void);   // gl_fixed_function.cpp
extern "C" const char* eden_debug_gl_tex_bytes(void);

#if defined(__APPLE__)
static const char* eden_vm_tag_name(unsigned tag) {
    switch (tag) {
        case 0: return "untagged";
#ifdef VM_MEMORY_MALLOC
        case VM_MEMORY_MALLOC: return "MALLOC";
#endif
#ifdef VM_MEMORY_MALLOC_SMALL
        case VM_MEMORY_MALLOC_SMALL: return "MALLOC_SMALL";
#endif
#ifdef VM_MEMORY_MALLOC_LARGE
        case VM_MEMORY_MALLOC_LARGE: return "MALLOC_LARGE";
#endif
#ifdef VM_MEMORY_MALLOC_HUGE
        case VM_MEMORY_MALLOC_HUGE: return "MALLOC_HUGE";
#endif
#ifdef VM_MEMORY_MALLOC_TINY
        case VM_MEMORY_MALLOC_TINY: return "MALLOC_TINY";
#endif
#ifdef VM_MEMORY_MALLOC_NANO
        case VM_MEMORY_MALLOC_NANO: return "MALLOC_NANO";
#endif
#ifdef VM_MEMORY_MALLOC_MEDIUM
        case VM_MEMORY_MALLOC_MEDIUM: return "MALLOC_MEDIUM";
#endif
#ifdef VM_MEMORY_IOKIT
        case VM_MEMORY_IOKIT: return "IOKit";
#endif
#ifdef VM_MEMORY_STACK
        case VM_MEMORY_STACK: return "STACK";
#endif
#ifdef VM_MEMORY_GUARD
        case VM_MEMORY_GUARD: return "GUARD";
#endif
#ifdef VM_MEMORY_SHARED_PMAP
        case VM_MEMORY_SHARED_PMAP: return "SHARED_PMAP";
#endif
#ifdef VM_MEMORY_DYLIB
        case VM_MEMORY_DYLIB: return "DYLIB";
#endif
#ifdef VM_MEMORY_OBJC_DISPATCHERS
        case VM_MEMORY_OBJC_DISPATCHERS: return "OBJC_DISPATCHERS";
#endif
#ifdef VM_MEMORY_FOUNDATION
        case VM_MEMORY_FOUNDATION: return "FOUNDATION";
#endif
#ifdef VM_MEMORY_COREGRAPHICS
        case VM_MEMORY_COREGRAPHICS: return "COREGRAPHICS";
#endif
#ifdef VM_MEMORY_CORESERVICES
        case VM_MEMORY_CORESERVICES: return "CORESERVICES";
#endif
#ifdef VM_MEMORY_DYLD
        case VM_MEMORY_DYLD: return "DYLD";
#endif
#ifdef VM_MEMORY_DYLD_MALLOC
        case VM_MEMORY_DYLD_MALLOC: return "DYLD_MALLOC";
#endif
#ifdef VM_MEMORY_SQLITE
        case VM_MEMORY_SQLITE: return "SQLITE";
#endif
#ifdef VM_MEMORY_GLSL
        case VM_MEMORY_GLSL: return "GLSL";
#endif
#ifdef VM_MEMORY_OPENCL
        case VM_MEMORY_OPENCL: return "OPENCL";
#endif
#ifdef VM_MEMORY_COREIMAGE
        case VM_MEMORY_COREIMAGE: return "COREIMAGE";
#endif
#ifdef VM_MEMORY_IMAGEIO
        case VM_MEMORY_IMAGEIO: return "IMAGEIO";
#endif
#ifdef VM_MEMORY_LAYERKIT
        case VM_MEMORY_LAYERKIT: return "CoreAnimation";
#endif
#ifdef VM_MEMORY_CGIMAGE
        case VM_MEMORY_CGIMAGE: return "CGIMAGE";
#endif
#ifdef VM_MEMORY_OS_ALLOC_ONCE
        case VM_MEMORY_OS_ALLOC_ONCE: return "OS_ALLOC_ONCE";
#endif
#ifdef VM_MEMORY_LIBDISPATCH
        case VM_MEMORY_LIBDISPATCH: return "LIBDISPATCH";
#endif
#ifdef VM_MEMORY_ACCELERATE
        case VM_MEMORY_ACCELERATE: return "ACCELERATE";
#endif
#ifdef VM_MEMORY_COREUI
        case VM_MEMORY_COREUI: return "COREUI";
#endif
#ifdef VM_MEMORY_COREAUDIO
        case VM_MEMORY_COREAUDIO: return "COREAUDIO";
#endif
#ifdef VM_MEMORY_IOSURFACE
        case VM_MEMORY_IOSURFACE: return "IOSurface";
#endif
#ifdef VM_MEMORY_IOACCELERATOR
        case VM_MEMORY_IOACCELERATOR: return "IOAccelerator";
#endif
#ifdef VM_MEMORY_SKYWALK
        case VM_MEMORY_SKYWALK: return "SKYWALK";
#endif
#ifdef VM_MEMORY_JAVASCRIPT_CORE
        case VM_MEMORY_JAVASCRIPT_CORE: return "JSC";
#endif
        default: return nullptr;
    }
}
#endif

extern "C" void eden_mem_trace(const char* where) {
    if (!g_eden_mem_trace) return;
#if defined(__APPLE__)
    const double MB = 1048576.0;
    task_vm_info_data_t vi;
    std::memset(&vi, 0, sizeof(vi));
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    const kern_return_t kr = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &count);
    if (kr != KERN_SUCCESS) { std::fprintf(stderr, "[eden-mem] %s: task_info failed\n", where); return; }
    const bool haveLedgers = count >= TASK_VM_INFO_REV3_COUNT;
    const int64_t gfx = vi.ledger_tag_graphics_footprint + vi.ledger_tag_graphics_footprint_compressed;
    const int64_t media = vi.ledger_tag_media_footprint + vi.ledger_tag_media_footprint_compressed;
    const int64_t neural = vi.ledger_tag_neural_footprint + vi.ledger_tag_neural_footprint_compressed;
    const int64_t net = vi.ledger_tag_network_nonvolatile + vi.ledger_tag_network_nonvolatile_compressed;
    const int64_t purg = vi.ledger_purgeable_nonvolatile + vi.ledger_purgeable_novolatile_compressed;
    const int64_t rest = (int64_t)vi.phys_footprint - (int64_t)vi.internal - (int64_t)vi.compressed
                         - gfx - media - neural - net - purg;
    std::printf("[eden-mem] %-22s footprint %7.1f | internal %6.1f compressed %6.1f graphics %6.1f "
                "media %5.1f purgeable %5.1f neural %4.1f net %4.1f rest(iokit+pt) %6.1f | resident %6.1f "
                "device %5.1f external %5.1f | ledgers %s, rev count %u\n",
                where, vi.phys_footprint / MB, vi.internal / MB, vi.compressed / MB, gfx / MB,
                media / MB, purg / MB, neural / MB, net / MB, rest / MB, vi.resident_size / MB,
                vi.device / MB, vi.external / MB, haveLedgers ? "yes" : "NO", (unsigned)count);

    // Region walk: dirty(+swapped) and resident bytes per VM tag, top entries by dirty.
    struct Acc { uint64_t dirty = 0, resident = 0, virt = 0; int regions = 0; };
    std::map<unsigned, Acc> byTag;
    vm_address_t addr = 0;
    natural_t depth = 0;
    const uint64_t page = (uint64_t)vi.page_size ? (uint64_t)vi.page_size : 4096;
    for (;;) {
        vm_size_t size = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t icount = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &addr, &size, &depth,
                                 (vm_region_recurse_info_t)&info, &icount) != KERN_SUCCESS) break;
        if (info.is_submap) { ++depth; continue; }
        Acc& a = byTag[info.user_tag];
        a.dirty += ((uint64_t)info.pages_dirtied + (uint64_t)info.pages_swapped_out) * page;
        a.resident += (uint64_t)info.pages_resident * page;
        a.virt += (uint64_t)size;
        a.regions++;
        addr += size;
    }
    std::vector<std::pair<unsigned, Acc>> v(byTag.begin(), byTag.end());
    std::sort(v.begin(), v.end(), [](const std::pair<unsigned, Acc>& x, const std::pair<unsigned, Acc>& y) {
        return x.second.dirty + x.second.virt / 64 > y.second.dirty + y.second.virt / 64;
    });
    std::printf("[eden-mem] %-22s gl buffers %s textures %s\n", where, eden_debug_gl_buffer_bytes(),
                eden_debug_gl_tex_bytes());
    std::printf("[eden-mem] %-22s tags (dirty/resident/virtual MB, regions):", where);
    int shown = 0;
    for (const auto& e : v) {
        if (shown++ >= 10) break;
        const char* n = eden_vm_tag_name(e.first);
        char tagbuf[24];
        if (!n) { std::snprintf(tagbuf, sizeof(tagbuf), "tag%u", e.first); n = tagbuf; }
        std::printf(" %s %.1f/%.1f/%.1f(%d)", n, e.second.dirty / MB, e.second.resident / MB,
                    e.second.virt / MB, e.second.regions);
    }
    std::printf("\n");
    std::fflush(stdout);
#else
    std::printf("[eden-mem] %s %s\n", where, eden_debug_heap());
#endif
}

}  // extern "C"

