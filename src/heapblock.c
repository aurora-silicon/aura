/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "heapblock.h"
#include "assert.h"
#include "sptm_virt.h"
#include "types.h"
#include "utils.h"
#include "xnuboot.h"

/*
 * This is a non-freeing allocator, used as a backend for malloc and for uncompressing data.
 *
 * Allocating 0 bytes is allowed, and guarantees "infinite" (until the end of RAM) space is
 * available at the returned pointer as long as no other malloc/heapblock calls occur, which is
 * useful as a buffer for unknown-length uncompressed data. A subsequent call with a size will then
 * actually reserve the block.
 */

static void *heap_base;
static void *heap_limit;

void heapblock_init(void)
{
    u64 carveout_base, carveout_limit;

    /* Under the virtualized SPTM, SPTM owns low RAM: aura's heap lives in its carve-out. */
    if (sptm_virt_heap_window(&carveout_base, &carveout_limit)) {
        heap_base = (void *)carveout_base;
        heapblock_alloc(0); // align base
        printf("Heap base (SPTM-virt carve-out): %p\n", heap_base);
        heapblock_set_limit((void *)carveout_limit);
        return;
    }

    heap_base = (void *)cur_boot_args.top_of_kernel_data;
    heapblock_alloc(0); // align base

    printf("Heap base: %p\n", heap_base);
}

void heapblock_set_limit(void *limit)
{
    heap_limit = limit;
    printf("Heap limit: %p (%ld MiB)\n", limit, ((u8 *)limit - (u8 *)heap_base) >> 20);
}

uintptr_t heapblock_limit(void)
{
    return (uintptr_t)heap_limit;
}

uintptr_t heapblock_high_water(void)
{
    return (uintptr_t)heap_base;
}

void *heapblock_alloc(size_t size)
{
    return heapblock_alloc_aligned(size, 64);
}

void *heapblock_alloc_aligned(size_t size, size_t align)
{
    assert((align & (align - 1)) == 0);
    assert(heap_base);

    uintptr_t block = (((uintptr_t)heap_base) + align - 1) & ~(align - 1);

    if (heap_limit && block + size > (uintptr_t)heap_limit)
        panic("heapblock: out of memory (%zu bytes at 0x%lx, limit %p)\n", size, block, heap_limit);

    heap_base = (void *)(block + size);

    return (void *)block;
}
