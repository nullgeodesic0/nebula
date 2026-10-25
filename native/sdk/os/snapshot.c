/* Snapshots of the whole game for netplay's rollback and live join (deterministic mode only; os_native.c has the
   scheme). A snapshot is taken at a retrace boundary inside the module (det_step): the code on the stack there is
   the module's own, at its fixed address, and every game thread's native stack lives in the game's memory. So the
   game is entirely:
   - the game's memory (MEM1 and MEM2, at their fixed address; thread stacks included),
   - the module's writable data, but for what the host's threads share with it (NEBULA_HOST_STATE: the event
     handoffs, the GPU command ring's writer, the staged remote input, this engine), and
   - the registers of the save point, kept on its stack by nebula_switch_stack.

   Saving is incremental: Windows' write watch on the game's memory tells which pages changed since the last save.
   A shadow copy holds the memory as of the latest save; each save records the pages it changed as they were before
   it (its undo record) and brings the shadow up to date. Restoring to an earlier save copies the shadow over the
   pages changed since the latest save, then applies the undo records back to that save, and resumes the save point:
   nebula_snapshot_save returns 1 there. Saves are kept for the last kSlots boundaries.

   The work runs on a helper stack outside the game's memory (the memory it rewrites holds the caller's stack). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <revolution/os.h>

#include <nebula_game_api.h>

#include "nebula_sdk.h"

#if defined(_WIN32)
#include <windows.h>

void* nebula_prepare_stack(void* top, size_t size, void (*entry)(void*), void* arg);
void nebula_switch_stack(void** from_sp, void* to_sp);
uint8_t* nebula_arena_base(void);
void nebula_image_range(uintptr_t* base, uintptr_t* size);

enum {
    kSlots = 24,
    kPage = 4096,
    kMem1 = 0x00000000u,
    kMem1Size = 0x04000000u,
    kMem2 = 0x10000000u,
    kMem2Size = 0x10000000u,
    kShadowSize = kMem1Size + kMem2Size,
    kHelperStackSize = 1u << 20,
    kMaxRanges = 8,
};

typedef struct {
    int used;
    uint32_t frame;
    void* sp;               /* the save point's stack pointer (its registers are saved there) */
    uint8_t* module;        /* the module's restorable data */
    uint32_t* undoOffsets;  /* shadow offsets of the pages this save changed, and their contents before it */
    uint8_t* undoPages;
    uint32_t undoCount;
    uint32_t undoCapacity;
    uint8_t* gxPending;     /* the GX commands written but not yet submitted at the save (gx_fifo.c) */
    size_t gxPendingSize;
    size_t gxPendingCapacity;
    uint8_t* gxPtr;
    uint8_t* gxEnd;
} Slot;

typedef struct {
    uint8_t* base;
    size_t size;
} Range;

static NEBULA_HOST_STATE Slot sSlots[kSlots];
static NEBULA_HOST_STATE uint32_t sNewest;   /* the latest save's slot (valid when sHaveSave) */
static NEBULA_HOST_STATE int sHaveSave;
static NEBULA_HOST_STATE uint8_t* sShadow;
static NEBULA_HOST_STATE Range sRanges[kMaxRanges];
static NEBULA_HOST_STATE int sRangeCount;
static NEBULA_HOST_STATE size_t sModuleBytes;
static NEBULA_HOST_STATE uint8_t* sHelperStack;
static NEBULA_HOST_STATE void* sCallerSp;
static NEBULA_HOST_STATE void* sHelperSp;
static NEBULA_HOST_STATE int sOp;            /* 1 save, 2 restore */
static NEBULA_HOST_STATE uint32_t sOpFrame;
static NEBULA_HOST_STATE int sResumed;
static NEBULA_HOST_STATE PVOID sWatched[kMem2Size / kPage];
static NEBULA_HOST_STATE const uint8_t* sImport; /* live join's image (nebula_game_snapshot_import) */
static NEBULA_HOST_STATE size_t sImportSize;
static void* do_load(void);

/* The module's writable sections but the host-shared one. */
static void find_ranges(void) {
    uintptr_t base = 0;
    uintptr_t size = 0;
    nebula_image_range(&base, &size);
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + (uintptr_t)dos->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    sModuleBytes = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections && sRangeCount < kMaxRanges; i++, section++) {
        if ((section->Characteristics & IMAGE_SCN_MEM_WRITE) == 0 || memcmp(section->Name, ".nbhost", 7) == 0) {
            continue;
        }
        sRanges[sRangeCount].base = (uint8_t*)(base + section->VirtualAddress);
        sRanges[sRangeCount].size = section->Misc.VirtualSize;
        sModuleBytes += section->Misc.VirtualSize;
        sRangeCount++;
    }
}

static uint8_t* region_base(int region) {
    return nebula_arena_base() + (region == 0 ? kMem1 : kMem2);
}

static size_t region_size(int region) {
    return region == 0 ? kMem1Size : kMem2Size;
}

static size_t shadow_offset(int region) {
    return region == 0 ? 0 : kMem1Size;
}

static int readable(const void* page) {
    MEMORY_BASIC_INFORMATION info;
    return VirtualQuery(page, &info, sizeof(info)) != 0 && info.State == MEM_COMMIT &&
           (info.Protect & (PAGE_READWRITE | PAGE_READONLY)) != 0 && (info.Protect & PAGE_GUARD) == 0;
}

static void push_undo(Slot* slot, uint32_t offset, const uint8_t* before) {
    if (slot->undoCount == slot->undoCapacity) {
        slot->undoCapacity = slot->undoCapacity == 0 ? 1024 : slot->undoCapacity * 2;
        slot->undoOffsets = (uint32_t*)realloc(slot->undoOffsets, slot->undoCapacity * sizeof(uint32_t));
        slot->undoPages = (uint8_t*)realloc(slot->undoPages, (size_t)slot->undoCapacity * kPage);
        if (slot->undoOffsets == NULL || slot->undoPages == NULL) {
            nebula_fatal("snapshot: out of memory for undo records");
        }
    }
    slot->undoOffsets[slot->undoCount] = offset;
    memcpy(slot->undoPages + (size_t)slot->undoCount * kPage, before, kPage);
    slot->undoCount++;
}

/* The pages written since the last call (write watch, reset). */
static ULONG_PTR changed_pages(int region) {
    ULONG_PTR count = kMem2Size / kPage;
    ULONG granularity = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, region_base(region), region_size(region), sWatched, &count,
                      &granularity) != 0) {
        nebula_fatal("snapshot: the game's memory is not watched for writes");
    }
    return count;
}

static void do_save(void) {
    const uint32_t index = sHaveSave ? (sNewest + 1) % kSlots : 0;
    Slot* slot = &sSlots[index];
    slot->undoCount = 0;
    if (slot->module == NULL) {
        slot->module = (uint8_t*)malloc(sModuleBytes);
    }
    if (!sHaveSave) {
        /* The first save: the whole memory into the shadow (readable pages; guards and the unused stay zero). */
        for (int region = 0; region < 2; region++) {
            (void)changed_pages(region);
            /* By runs of pages alike (a query per page takes seconds over the watched memory). */
            for (size_t at = 0; at < region_size(region);) {
                uint8_t* page = region_base(region) + at;
                MEMORY_BASIC_INFORMATION info;
                if (VirtualQuery(page, &info, sizeof(info)) == 0) {
                    break;
                }
                size_t run = (size_t)((uint8_t*)info.BaseAddress + info.RegionSize - page);
                if (run > region_size(region) - at) {
                    run = region_size(region) - at;
                }
                if (readable(page)) {
                    memcpy(sShadow + shadow_offset(region) + at, page, run);
                }
                at += run;
            }
        }
    } else {
        for (int region = 0; region < 2; region++) {
            const ULONG_PTR count = changed_pages(region);
            for (ULONG_PTR i = 0; i < count; i++) {
                const size_t at = (size_t)((uint8_t*)sWatched[i] - region_base(region));
                const uint32_t offset = (uint32_t)(shadow_offset(region) + at);
                push_undo(slot, offset, sShadow + offset);
                memcpy(sShadow + offset, region_base(region) + at, kPage);
            }
        }
    }
    uint8_t* out = slot->module;
    for (int i = 0; i < sRangeCount; i++) {
        memcpy(out, sRanges[i].base, sRanges[i].size);
        out += sRanges[i].size;
    }
    const uint8_t* pending = NULL;
    slot->gxPendingSize = nebula_gx_pending(&pending, &slot->gxPtr, &slot->gxEnd);
    if (slot->gxPendingSize > slot->gxPendingCapacity) {
        slot->gxPendingCapacity = slot->gxPendingSize;
        slot->gxPending = (uint8_t*)realloc(slot->gxPending, slot->gxPendingCapacity);
        if (slot->gxPending == NULL) {
            nebula_fatal("snapshot: out of memory for the GX commands");
        }
    }
    if (slot->gxPendingSize != 0) {
        memcpy(slot->gxPending, pending, slot->gxPendingSize);
    }
    slot->sp = sCallerSp;
    slot->frame = sOpFrame;
    slot->used = 1;
    sNewest = index;
    sHaveSave = 1;
}

static Slot* find_slot(uint32_t frame) {
    for (int i = 0; i < kSlots; i++) {
        if (sSlots[i].used && sSlots[i].frame == frame) {
            return &sSlots[i];
        }
    }
    return NULL;
}

static void do_restore(Slot* target) {
    /* Back to the latest save: its shadow over the pages changed since. */
    for (int region = 0; region < 2; region++) {
        const ULONG_PTR count = changed_pages(region);
        for (ULONG_PTR i = 0; i < count; i++) {
            const size_t at = (size_t)((uint8_t*)sWatched[i] - region_base(region));
            memcpy(region_base(region) + at, sShadow + shadow_offset(region) + at, kPage);
        }
    }
    /* Then save by save back to the target: each newer save's pages as they were before it. */
    uint32_t index = sNewest;
    while (&sSlots[index] != target) {
        Slot* slot = &sSlots[index];
        for (uint32_t i = slot->undoCount; i-- > 0;) {
            const uint32_t offset = slot->undoOffsets[i];
            const uint8_t* before = slot->undoPages + (size_t)i * kPage;
            memcpy(sShadow + offset, before, kPage);
            uint8_t* page = offset < kMem1Size ? region_base(0) + offset : region_base(1) + (offset - kMem1Size);
            memcpy(page, before, kPage);
        }
        slot->used = 0;
        index = (index + kSlots - 1) % kSlots;
    }
    sNewest = index;
    const uint8_t* in = target->module;
    for (int i = 0; i < sRangeCount; i++) {
        memcpy(sRanges[i].base, in, sRanges[i].size);
        in += sRanges[i].size;
    }
    /* The copies above are not the game's writes. */
    for (int region = 0; region < 2; region++) {
        (void)changed_pages(region);
    }
    nebula_gx_restore_pending(target->gxPending, target->gxPendingSize, target->gxPtr, target->gxEnd);
}

/* On the helper stack: the operation, then back to the save point (the caller, or the restored one). */
static void helper_main(void* arg) {
    (void)arg;
    void* resume = sCallerSp;
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    if (sOp == 1) {
        do_save();
        sResumed = 0;
    } else if (sOp == 3) {
        resume = do_load();
        sResumed = 1;
    } else {
        Slot* target = find_slot(sOpFrame);
        do_restore(target);
        resume = target->sp;
        sResumed = 1;
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    static int timing = -1; /* GALAXY_SNAPSHOT_TIMING=1: each save and restore's time, logged */
    if (timing < 0) {
        timing = getenv("GALAXY_SNAPSHOT_TIMING") != NULL;
    }
    if (timing) {
        nebula_log("snapshot: %s frame %u in %.2f ms\n", sOp == 1 ? "save" : "restore", sOpFrame,
                   (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart);
    }
    void* ignored = NULL;
    nebula_switch_stack(&ignored, resume);
    nebula_fatal("snapshot: the helper stack was resumed");
}

static int run_on_helper(int op, uint32_t frame) {
    if (sHelperStack == NULL) {
        sHelperStack = (uint8_t*)VirtualAlloc(NULL, kHelperStackSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        sShadow = (uint8_t*)VirtualAlloc(NULL, kShadowSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (sHelperStack == NULL || sShadow == NULL) {
            nebula_fatal("snapshot: no memory for the shadow copy");
        }
        find_ranges();
    }
    sOp = op;
    sOpFrame = frame;
    sHelperSp = nebula_prepare_stack(sHelperStack + kHelperStackSize, kHelperStackSize, helper_main, NULL);
    nebula_switch_stack(&sCallerSp, sHelperSp);
    return sResumed;
}

int nebula_snapshot_save(uint32_t frame) {
    return run_on_helper(1, frame);
}

int nebula_snapshot_kept(uint32_t frame) {
    return find_slot(frame) != NULL;
}

/* Live join: the imported image, from its save point on (does not return). */
static void nebula_snapshot_load(void) {
    if (sImport == NULL) {
        nebula_fatal("live join: no game image");
    }
    nebula_det_clear_staged();
    nebula_gx_drop_unsubmitted();
    nebula_gx_wait_idle();
    (void)run_on_helper(3, 0);
    nebula_fatal("snapshot: the load returned");
}

void nebula_snapshot_restore(uint32_t frame) {
    if (find_slot(frame) == NULL) {
        nebula_fatal("snapshot: frame %u is no longer kept", frame);
    }
    nebula_det_clear_staged();
    nebula_gx_drop_unsubmitted();
    nebula_gx_wait_idle();
    (void)run_on_helper(2, frame);
    nebula_fatal("snapshot: the restore returned");
}

/* At each retrace boundary (det_step): what the host wants done there. A restore resumes an earlier boundary's
   save, where the host is asked again. */
/* Live join's image (nebula_game_snapshot_export): a header, the module's restorable data, the GX commands written
   but not submitted, then each page of the game's memory that is not all zero (its shadow offset, its bytes). */
typedef struct {
    uint32_t magic;
    uint32_t frame;
    uint64_t sp;
    uint64_t moduleBytes;
    uint64_t gxPendingSize;
    uint64_t gxPtr;
    uint64_t gxEnd;
    uint32_t pages;
    uint32_t reserved;
} ImageHeader;
enum { kImageMagic = 0x4A4C424Eu }; /* "NBLJ" */


static int page_is_zero(const uint8_t* page) {
    const uint64_t* words = (const uint64_t*)page;
    for (int i = 0; i < kPage / 8; i++) {
        if (words[i] != 0) {
            return 0;
        }
    }
    return 1;
}

NEBULA_GAME_API int nebula_game_snapshot_export(uint32_t frame, void (*write)(void* context, const void* data,
                                                                              size_t size), void* context) {
    if (!sHaveSave || sSlots[sNewest].frame != frame) {
        return 0;
    }
    const Slot* slot = &sSlots[sNewest];
    ImageHeader header = {kImageMagic, frame, (uint64_t)(uintptr_t)slot->sp, sModuleBytes, slot->gxPendingSize,
                          (uint64_t)(uintptr_t)slot->gxPtr, (uint64_t)(uintptr_t)slot->gxEnd, 0, 0};
    for (size_t at = 0; at < kShadowSize; at += kPage) {
        header.pages += !page_is_zero(sShadow + at);
    }
    write(context, &header, sizeof(header));
    write(context, slot->module, sModuleBytes);
    if (slot->gxPendingSize != 0) {
        write(context, slot->gxPending, slot->gxPendingSize);
    }
    for (size_t at = 0; at < kShadowSize; at += kPage) {
        if (!page_is_zero(sShadow + at)) {
            const uint32_t offset = (uint32_t)at;
            write(context, &offset, sizeof(offset));
            write(context, sShadow + at, kPage);
        }
    }
    return 1;
}

NEBULA_GAME_API void nebula_game_snapshot_import(const void* image, size_t size) {
    sImport = (const uint8_t*)image;
    sImportSize = size;
}

/* On the helper stack: the game's memory and data become the image's; then on from the image's save point. */
static void* do_load(void) {
    const ImageHeader* header = (const ImageHeader*)sImport;
    if (sImportSize < sizeof(*header) || header->magic != kImageMagic || header->moduleBytes != sModuleBytes) {
        nebula_fatal("live join: the game image does not fit this build");
    }
    const uint8_t* in = sImport + sizeof(*header);
    for (int i = 0; i < sRangeCount; i++) {
        memcpy(sRanges[i].base, in, sRanges[i].size);
        in += sRanges[i].size;
    }
    const uint8_t* pending = in;
    in += header->gxPendingSize;
    /* Every readable page zero, then the image's pages. */
    for (int region = 0; region < 2; region++) {
        for (size_t at = 0; at < region_size(region);) {
            uint8_t* page = region_base(region) + at;
            MEMORY_BASIC_INFORMATION info;
            if (VirtualQuery(page, &info, sizeof(info)) == 0) {
                break;
            }
            size_t run = (size_t)((uint8_t*)info.BaseAddress + info.RegionSize - page);
            if (run > region_size(region) - at) {
                run = region_size(region) - at;
            }
            if (readable(page)) {
                memset(page, 0, run);
            }
            at += run;
        }
    }
    for (uint32_t i = 0; i < header->pages; i++) {
        uint32_t offset;
        memcpy(&offset, in, sizeof(offset));
        in += sizeof(offset);
        uint8_t* page = offset < kMem1Size ? region_base(0) + offset : region_base(1) + (offset - kMem1Size);
        if (offset < kShadowSize && readable(page)) {
            memcpy(page, in, kPage);
        }
        in += kPage;
    }
    if ((size_t)(in - sImport) > sImportSize) {
        nebula_fatal("live join: the game image is cut short");
    }
    nebula_gx_restore_pending(pending, (size_t)header->gxPendingSize, (uint8_t*)(uintptr_t)header->gxPtr,
                              (uint8_t*)(uintptr_t)header->gxEnd);
    /* Nothing kept yet here: the next save takes the whole memory. */
    for (int i = 0; i < kSlots; i++) {
        sSlots[i].used = 0;
    }
    sHaveSave = 0;
    for (int region = 0; region < 2; region++) {
        (void)changed_pages(region);
    }
    return (void*)(uintptr_t)header->sp;
}

void nebula_snapshot_touch(void* addr, uint32_t size) {
    if (sShadow == NULL || addr == NULL || size == 0) {
        return;
    }
    /* Only bytes the read wrote (the game may be writing the rest of the first page meanwhile). */
    volatile uint8_t* p = (volatile uint8_t*)addr;
    volatile uint8_t* const end = p + size;
    while (p < end) {
        *p = *p;
        p = (volatile uint8_t*)(((uintptr_t)p & ~(uintptr_t)(kPage - 1)) + kPage);
    }
}

static void boundary(uint32_t frame) {
    for (;;) {
        uint32_t arg = 0;
        const uint32_t action = nebula_host()->det_boundary(nebula_host()->user, frame, &arg);
        if (action == NEBULA_DET_SAVE) {
            if (nebula_snapshot_save(frame)) {
                return; /* resumed by a restore: this boundary again, already reported */
            }
            continue;
        }
        if (action == NEBULA_DET_RESTORE) {
            nebula_snapshot_restore(arg);
        }
        if (action == NEBULA_DET_LOAD) {
            nebula_snapshot_load();
        }
        return;
    }
}

void nebula_snapshot_boundary(uint32_t frame) {
    boundary(frame);
    /* The host's work left this PC's own values on the stack; the game goes on with none (online checks compare the
       game's memory, which its uninitialized temporaries would carry them into). */
    nebula_clear_dead_stack();
}
#else
NEBULA_GAME_API int nebula_game_snapshot_export(uint32_t frame, void (*write)(void* context, const void* data,
                                                                              size_t size), void* context) {
    (void)frame;
    (void)write;
    (void)context;
    return 0;
}

NEBULA_GAME_API void nebula_game_snapshot_import(const void* image, size_t size) {
    (void)image;
    (void)size;
}

void nebula_snapshot_touch(void* addr, uint32_t size) {
    (void)addr;
    (void)size;
}

void nebula_snapshot_boundary(uint32_t frame) {
    (void)frame;
}
#endif
