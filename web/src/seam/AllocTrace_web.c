// AllocTrace_web.c -- ROADMAP Phase M / M6 bisect instrument. OFF unless -DEDEN_ALLOC_TRACE=ON.
//
// M6's finding is that every world load grows linear memory by a fixed amount that is never
// reclaimed, at both world heights. The ROADMAP row says explicitly: "don't guess further, bisect
// by instrumenting allocation totals across a quit->reload boundary." This file is that
// instrument.
//
// HOW IT INTERPOSES. emmalloc declares every one of its public entry points
// __attribute__((weak)) (system/lib/emmalloc.c, EMMALLOC_EXPORT), specifically so an application
// can define its own strong malloc/free and forward to the emmalloc_* names, which are strong.
// That is all this file does -- no -Wl,--wrap=, no allocator replacement. Every allocation in the
// program funnels through here, including libc's internal ones (libc is a static archive, so its
// undefined `malloc` binds to ours at link time) and JS-side Module._malloc.
//
// WHAT IT RECORDS.
//   * a live-bytes histogram bucketed by power-of-two size class (emmalloc_usable_size, so the
//     numbers reconcile with eden_debug_alloc()'s dynHeap - freeDyn);
//   * a table of every live allocation at or above EDEN_ALLOC_BIG_BYTES (32 KB), with its size and
//     an allocation sequence number, so "which size grew by N MB between two menu visits" has a
//     concrete answer instead of a size class;
//   * a per-PHASE breakdown: eden_alloc_trace_phase("name") tags subsequent allocations, and the
//     dump reports live bytes per tag. Tagging the world load/unload path turns "22 MB leaks" into
//     "22 MB leaks in <subsystem>".
//
// It is single-threaded-only by construction (plain non-atomic counters). That is fine and
// deliberate: the bisect runs on build-relwdiag (EDEN_THREADED=OFF), and paying for atomics here
// would perturb the thing being measured. Never enable this in a shipped or threaded build.
#include <emscripten/emscripten.h>
#include <emscripten/emmalloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// CMakeLists forces every seam source through the C++ frontend (.mm files are compiled as CXX),
// so this file is compiled as C++ even though it is named .c. malloc/free/... keep C linkage
// anyway (stdlib.h declares them extern "C", and these definitions match), but the exports below
// would be name-mangled without this — and then the probe cannot find them.
#ifdef __cplusplus
extern "C" {
#endif

// Tunables. The pointer table must hold every live allocation in the program (peak live count is
// ~90k in a 64z session), so 1M slots of open addressing is ~8x headroom; overflow is counted and
// reported rather than silently wrong.
#define ALLOC_TABLE_SLOTS   (1u << 20)
#define ALLOC_STACK_SLOTS   192
#define ALLOC_STACK_CHARS   768
#define ALLOC_PHASES        32

typedef struct { uint32_t ptr; uint32_t bytes; uint16_t stack; uint16_t phase; } Ent;
static Ent g_tab[ALLOC_TABLE_SLOTS];
static size_t g_liveBytes, g_liveCount, g_tabOverflow;

typedef struct { size_t count; size_t bytes; } Bucket;
static Bucket g_bucket[40];

// Phase tags: eden_alloc_trace_phase() names the region an allocation was made in.
static const char* g_phaseName[ALLOC_PHASES] = { "(untagged)" };
static int    g_phaseCount = 1, g_phase = 0;
static size_t g_phaseBytes[ALLOC_PHASES], g_phaseCounts[ALLOC_PHASES];

// Call-site capture: for allocations at or above g_stackMin, record the JS-visible wasm callstack
// and aggregate live bytes per distinct stack. This is the whole point of the instrument — a size
// class says "something leaks 48 KB blocks", a stack says which function allocated them. Needs a
// build with the wasm name section (--profiling-funcs, added by the EDEN_ALLOC_TRACE CMake block).
static char   g_stackText[ALLOC_STACK_SLOTS][ALLOC_STACK_CHARS];
static size_t g_stackBytes[ALLOC_STACK_SLOTS], g_stackCount[ALLOC_STACK_SLOTS];
static int    g_stackUsed = 0;
static size_t g_stackMin = 0;          // 0 = capture disabled (the default; capture is slow)
static int    g_inTrace = 0;           // reentrancy guard: emscripten_get_callstack allocates

static int size_class(size_t bytes) {
    int i = 0;
    while ((size_t)1 << (i + 1) <= bytes && i < 39) i++;
    return i;
}

static uint32_t slot_for(uint32_t ptr) {
    // Fibonacci hash of the pointer; allocations are 8-byte aligned so the low bits are dead.
    uint32_t h = (uint32_t)((ptr >> 3) * 2654435761u);
    return h & (ALLOC_TABLE_SLOTS - 1);
}

static int capture_stack(void) {
    static char buf[ALLOC_STACK_CHARS];
    g_inTrace = 1;
    emscripten_get_callstack(EM_LOG_C_STACK | EM_LOG_NO_PATHS, buf, sizeof(buf));
    g_inTrace = 0;
    for (int i = 0; i < g_stackUsed; i++) {
        if (strcmp(g_stackText[i], buf) == 0) return i;
    }
    if (g_stackUsed >= ALLOC_STACK_SLOTS) return -1;
    snprintf(g_stackText[g_stackUsed], ALLOC_STACK_CHARS, "%s", buf);
    return g_stackUsed++;
}

static void note_alloc(void* p) {
    if (!p || g_inTrace) return;
    size_t n = emmalloc_usable_size(p);
    int c = size_class(n);
    g_bucket[c].count++; g_bucket[c].bytes += n;
    g_liveBytes += n; g_liveCount++;
    g_phaseBytes[g_phase] += n; g_phaseCounts[g_phase]++;

    int stack = -1;
    if (g_stackMin && n >= g_stackMin) stack = capture_stack();
    if (stack >= 0) { g_stackBytes[stack] += n; g_stackCount[stack]++; }

    uint32_t ptr = (uint32_t)(uintptr_t)p;
    uint32_t i = slot_for(ptr);
    for (uint32_t probe = 0; probe < ALLOC_TABLE_SLOTS; probe++) {
        if (g_tab[i].ptr == 0 || g_tab[i].ptr == ptr) {
            g_tab[i].ptr = ptr; g_tab[i].bytes = (uint32_t)n;
            g_tab[i].stack = (uint16_t)(stack + 1); g_tab[i].phase = (uint16_t)g_phase;
            return;
        }
        i = (i + 1) & (ALLOC_TABLE_SLOTS - 1);
    }
    g_tabOverflow++;
}

static void note_free(void* p) {
    if (!p || g_inTrace) return;
    uint32_t ptr = (uint32_t)(uintptr_t)p;
    uint32_t i = slot_for(ptr);
    for (uint32_t probe = 0; probe < ALLOC_TABLE_SLOTS; probe++) {
        if (g_tab[i].ptr == ptr) break;
        if (g_tab[i].ptr == 0) return;   // never tracked (allocated before this file was linked in)
        i = (i + 1) & (ALLOC_TABLE_SLOTS - 1);
    }
    if (g_tab[i].ptr != ptr) return;
    size_t n = g_tab[i].bytes;
    int c = size_class(n);
    if (g_bucket[c].count) g_bucket[c].count--;
    g_bucket[c].bytes = g_bucket[c].bytes > n ? g_bucket[c].bytes - n : 0;
    g_liveBytes = g_liveBytes > n ? g_liveBytes - n : 0;
    if (g_liveCount) g_liveCount--;
    int ph = g_tab[i].phase;
    g_phaseBytes[ph] = g_phaseBytes[ph] > n ? g_phaseBytes[ph] - n : 0;
    if (g_phaseCounts[ph]) g_phaseCounts[ph]--;
    if (g_tab[i].stack) {
        int st = g_tab[i].stack - 1;
        g_stackBytes[st] = g_stackBytes[st] > n ? g_stackBytes[st] - n : 0;
        if (g_stackCount[st]) g_stackCount[st]--;
    }
    // Tombstone-free deletion: rehash the cluster following this slot (Knuth 6.4 algorithm R).
    uint32_t hole = i;
    uint32_t j = (i + 1) & (ALLOC_TABLE_SLOTS - 1);
    g_tab[hole].ptr = 0;
    while (g_tab[j].ptr) {
        uint32_t home = slot_for(g_tab[j].ptr);
        uint32_t distJ = (j - home) & (ALLOC_TABLE_SLOTS - 1);
        uint32_t distH = (j - hole) & (ALLOC_TABLE_SLOTS - 1);
        if (distJ >= distH) { g_tab[hole] = g_tab[j]; g_tab[j].ptr = 0; hole = j; }
        j = (j + 1) & (ALLOC_TABLE_SLOTS - 1);
    }
}

void* malloc(size_t size)                { void* p = emmalloc_malloc(size); note_alloc(p); return p; }
void* calloc(size_t n, size_t size)      { void* p = emmalloc_calloc(n, size); note_alloc(p); return p; }
void* memalign(size_t a, size_t size)    { void* p = emmalloc_memalign(a, size); note_alloc(p); return p; }
void* aligned_alloc(size_t a, size_t sz) { void* p = emmalloc_memalign(a, sz); note_alloc(p); return p; }
void  free(void* p)                      { note_free(p); emmalloc_free(p); }

void* realloc(void* p, size_t size) {
    note_free(p);
    void* q = emmalloc_realloc(p, size);
    if (q) note_alloc(q);
    else if (p) note_alloc(p);  // realloc failed: the old block is still live
    return q;
}

int posix_memalign(void** memptr, size_t alignment, size_t size) {
    int r = emmalloc_posix_memalign(memptr, alignment, size);
    if (r == 0 && memptr) note_alloc(*memptr);
    return r;
}

// ---- exports ---------------------------------------------------------------------------------

// Tag every subsequent allocation with `name` (stored by pointer for engine literals, copied into
// a small arena otherwise is NOT done -- pass string literals or JS-allocated stable strings).
// Pass NULL or "" to go back to untagged.
EMSCRIPTEN_KEEPALIVE
void eden_alloc_trace_phase(const char* name) {
    if (!name || !*name) { g_phase = 0; return; }
    for (int i = 1; i < g_phaseCount; i++) {
        if (strcmp(g_phaseName[i], name) == 0) { g_phase = i; return; }
    }
    if (g_phaseCount >= ALLOC_PHASES) { g_phase = 0; return; }
    // Copy: callers from JS pass a temporary buffer.
    static char arena[ALLOC_PHASES][48];
    snprintf(arena[g_phaseCount], sizeof(arena[0]), "%s", name);
    g_phaseName[g_phaseCount] = arena[g_phaseCount];
    g_phase = g_phaseCount++;
}

// Capture callstacks for allocations >= `bytes` (0 disables). Slow -- one JS stack capture per
// qualifying allocation -- so raise the threshold until only the interesting allocations qualify.
EMSCRIPTEN_KEEPALIVE
void eden_alloc_trace_stacks(unsigned bytes) { g_stackMin = bytes; }

EMSCRIPTEN_KEEPALIVE
const char* eden_debug_alloc_histogram(void) {
    static char buf[4096];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n, "{\"liveBytes\":%llu,\"liveCount\":%llu,\"tableOverflow\":%llu,\"buckets\":{",
                  (unsigned long long)g_liveBytes, (unsigned long long)g_liveCount,
                  (unsigned long long)g_tabOverflow);
    int first = 1;
    for (int i = 0; i < 40; i++) {
        if (!g_bucket[i].count && !g_bucket[i].bytes) continue;
        n += snprintf(buf + n, sizeof(buf) - n, "%s\"%d\":{\"count\":%llu,\"bytes\":%llu}",
                      first ? "" : ",", 1 << i,
                      (unsigned long long)g_bucket[i].count, (unsigned long long)g_bucket[i].bytes);
        first = 0;
        if (n >= (int)sizeof(buf) - 64) break;
    }
    snprintf(buf + n, sizeof(buf) - n, "}}");
    return buf;
}

// Live bytes per phase tag.
EMSCRIPTEN_KEEPALIVE
const char* eden_debug_alloc_phases(void) {
    static char buf[4096];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n, "{");
    for (int i = 0; i < g_phaseCount; i++) {
        if (n >= (int)sizeof(buf) - 120) break;
        n += snprintf(buf + n, sizeof(buf) - n, "%s\"%s\":{\"bytes\":%llu,\"count\":%llu}",
                      i ? "," : "", g_phaseName[i],
                      (unsigned long long)g_phaseBytes[i], (unsigned long long)g_phaseCounts[i]);
    }
    snprintf(buf + n, sizeof(buf) - n, "}");
    return buf;
}

// The live-bytes-per-callstack table, biggest first. `index` selects one entry so the caller can
// page through it without needing a megabyte-sized JSON buffer; returns "" past the end.
EMSCRIPTEN_KEEPALIVE
int eden_debug_alloc_stack_count(void) { return g_stackUsed; }

EMSCRIPTEN_KEEPALIVE
unsigned eden_debug_alloc_stack_bytes(int index) {
    return (index >= 0 && index < g_stackUsed) ? (unsigned)g_stackBytes[index] : 0;
}

EMSCRIPTEN_KEEPALIVE
unsigned eden_debug_alloc_stack_live(int index) {
    return (index >= 0 && index < g_stackUsed) ? (unsigned)g_stackCount[index] : 0;
}

EMSCRIPTEN_KEEPALIVE
const char* eden_debug_alloc_stack_text(int index) {
    return (index >= 0 && index < g_stackUsed) ? g_stackText[index] : "";
}

#ifdef __cplusplus
}
#endif
