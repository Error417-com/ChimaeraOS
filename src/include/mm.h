#ifndef MM_H
#define MM_H

#include "types.h"

void     mm_init(void);
void    *kmalloc(uint32_t size);
void     kfree(void *ptr);
uint32_t mm_heap_used(void);  /* bytes allocated so far (bump-allocator ptr) */

#endif /* MM_H */
