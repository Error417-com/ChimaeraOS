/*
 * ChimaeraOS - Simple Bump Allocator
 * mm/mm.c
 *
 * A minimal heap allocator for kernel use.  Allocates from a 2 MB static
 * arena; kfree() is a no-op (acceptable for the short-lived test kernel).
 */
#include "../include/mm.h"
#include "../include/types.h"

#define HEAP_SIZE (2 * 1024 * 1024)  /* 2 MB */

static uint8_t heap[HEAP_SIZE];
static uint32_t heap_ptr = 0;

void mm_init(void)
{
    heap_ptr = 0;
}

void *kmalloc(uint32_t size)
{
    /* Align to 4 bytes */
    size = (size + 3) & ~3u;
    if (heap_ptr + size > HEAP_SIZE) return NULL;
    void *p = &heap[heap_ptr];
    heap_ptr += size;
    /* Zero the allocation */
    uint8_t *b = (uint8_t *)p;
    for (uint32_t i = 0; i < size; i++) b[i] = 0;
    return p;
}

void kfree(void *ptr)
{
    (void)ptr;  /* no-op for bump allocator */
}

uint32_t mm_heap_used(void)
{
    return heap_ptr;
}
