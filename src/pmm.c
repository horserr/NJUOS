#ifdef clion

#include "../include/common.h"

#else
#include <common.h>
#endif
// the type of address is uintptr_t
// the type of size of space is size_t
// both of them are unsigned long int

// todo pointer type
// slab should ask for space in a row
// free_list's order should minus base order
// check magic
// should initial metadata
// todo clarify naming convention
static struct memory_allocator MemAllocator;

const int SLAB_CATEGORY[] = {1, 2, 4, 8, 16, 32, 64, 128, 4096};

static inline int get_order(size_t size);

static inline size_t align_size(size_t size);

static void util_list_addFirst(int index, MemMetaData *target);

static MemMetaData *util_list_removeFirst(int index);

/**
 * give the start of a space in memory, return the address of memory metadata
 */
static inline MemMetaData *private__get_mem_metadata(intptr_t addr) {
    return (MemMetaData *) (addr - (intptr_t) sizeof(MemMetaData));
}

/**
 * give the address of memory metadata, return the address of space
 */
static inline intptr_t private__get_mem_space_with_metadata(intptr_t metadata) {
    return metadata + (intptr_t) sizeof(MemMetaData);
}

/**
 * interpret this address as 'memory metadata' and initialize it
 */
MemMetaData *private__init_mem_metadata(const intptr_t addr) {
    MemMetaData *meta = (MemMetaData *) addr;
    meta->MAGIC = MEM_METADATA_MAGIC;
    meta->next = NULL;

    meta->size = 0;
    meta->offset = 0;
    return meta;
}

/**
 * @brief initialize the global memory allocator, aka. `MemAllocator`.
 * @note parameters of this function may not be aligned
 */
static void private__init_mem_allocator(intptr_t startAddr, intptr_t endAddr) {
    MemAllocator.base_order = 12;

    // truncate or align address to 'page size'
    endAddr = ROUNDDOWN(endAddr, PAGE_SIZE);
    // startAddr is also the first-come metadata address
    startAddr = ROUNDUP(startAddr, PAGE_SIZE);
    MemMetaData *meta = private__init_mem_metadata(startAddr);

    // the margin between startAddr and endAddr may not be 'power of two'
    int order = get_order((size_t) (endAddr - startAddr));
    MemAllocator.max_order = order;

    for (int i = 0; i < LENGTH(MemAllocator.free_list); i++) {
        MemAllocator.free_list[i] = NULL;
    }
    for (int i = 0; i < LENGTH(MemAllocator.mp); i++) {
        MemAllocator.mp[i] = 0;
    }

    MemAllocator.free_list[order - MemAllocator.base_order] = meta;
}

/**
 * @brief **private** function call of memory allocate in aid of MemAllocator
 * @note
 * 1. The parameter should be greater than the maximum size of slab which
 *    is 4096, in other words, the requested size should be greater than a page.
 * 2. This function shouldn't be invoked directly.
 * @param size the gross size that may including the metadata which controls the
 * following space.
 * @return the address of space truly using for containing;
 * @return return NULL, if there isn't available space anymore
 */
static intptr_t private__mem_allocate(size_t size) {
    size = align_size(size);
    const int order = get_order(size);
    // fitted space is available
    if (MemAllocator.free_list[order - MemAllocator.base_order]) {
        MemMetaData *meta = util_list_removeFirst(order - MemAllocator.base_order);
        // assert(meta % PAGE_SIZE == 0);
        intptr_t addr = (intptr_t) meta;
        MemAllocator.mp[addr >> MemAllocator.base_order] = order; // register
        return private__get_mem_space_with_metadata(addr);
    }
    // fitted space isn't available
    int available_order = -1;
    for (int i = order + 1; i <= MemAllocator.max_order; i++) {
        if (MemAllocator.free_list[i - MemAllocator.base_order]) {
            available_order = i;
            break;
        }
    }
    if (available_order == -1) { // there is absolutely no space
        return (intptr_t) NULL;
    }
    for (int i = available_order; i > order; i--) {
        MemMetaData *meta = util_list_removeFirst(i - MemAllocator.base_order);
        intptr_t addr = (intptr_t) meta;
        intptr_t newAddr = addr + (1 << (i - 1));
        MemMetaData *newMeta = private__init_mem_metadata(newAddr);
        util_list_addFirst(i - 1 - MemAllocator.base_order, newMeta);
        util_list_addFirst(i - 1 - MemAllocator.base_order, meta);
    }
    MemMetaData *meta = util_list_removeFirst(order - MemAllocator.base_order);
    intptr_t addr = (intptr_t) meta;
    MemAllocator.mp[addr >> MemAllocator.base_order] = order;
    return private__get_mem_space_with_metadata(addr);
}

/**
 * @brief **public** function call of memory allocate in aid of MemAllocator.
 * middle layer between slab and actual 'memory allocator'
 * @note  the parameter should be greater than the maximum size of slab which is
 * 4096, in other words, the requested size should be greater than a page size.
 * @param size the net size, not including the metadata that controls the
 * following space.
 * @return the address of requested space;
 * @return return NULL, if there isn't available space anymore
 */
intptr_t mem_allocate(size_t size) {
    size = align_size(size);
    // todo this can cause internal fraction
    intptr_t space = private__mem_allocate(size + sizeof(MemMetaData));
    if (!space) return (intptr_t) NULL;

    intptr_t beginning = ROUNDUP(space, size);
    MemMetaData *meta = private__get_mem_metadata(space);
    meta->offset = beginning - space;
    meta->size = size;
    return beginning;
}

/**
 * @brief **private** function call of memory deallocate in aid of MemAllocator.
 * @note addr may not have been registered before, in this case, it is illegal.
 * Therefore, MemAllocator.mp as well as MAGIC should always be checked.
 * @param addr according to `private__mem_allocate`, this parameter should be the
 * beginning of space rather than metadata.
 * @return 0 if success; 1 if failed
 */
int private__mem_deallocate(intptr_t addr) {
    MemMetaData *meta = private__get_mem_metadata(addr);
    if (meta->MAGIC != MEM_METADATA_MAGIC) {
        return 1;
    }
    int order = MemAllocator.mp[meta >> MemAllocator.base_order];
    if (order < MemAllocator.base_order) {
        return 1;
    }
    MemAllocator.mp[meta >> MemAllocator.base_order] = 0; // register off

    for (int i = order; i < MemAllocator.max_order; i++) {
        if (MemAllocator.free_list[i - MemAllocator.base_order]) {
            // coalesce
        }
    }
}

void mem_deallocate() {}

// todo max request memory is not allowed
static void *kalloc(size_t size) {
    // TODO
    // You can add more .c files to the repo.

    return NULL;
}

static void kfree(void *ptr) {
    // TODO
    // You can add more .c files to the repo.
}

static void pmm_init() {
    uintptr_t pmsize = ((uintptr_t) heap.end - (uintptr_t) heap.start);

    printf("Got %d MiB heap: [%p, %p)\n", pmsize >> 20, heap.start, heap.end);
}

MODULE_DEF(pmm) = {
        .init = pmm_init,
        .alloc = kalloc,
        .free = kfree,
};

/***** utility function ************/
/**
 * @brief this function is designed for 'memory allocator', which calcualtes the
 * order of power of 2 size. And 2^order is less than or equal to the given
 * size.
 */
static inline int get_order(size_t size) {
    // counting_leading_zeros();
    return ((int) sizeof(size_t) * 8 - 1) - __builtin_clz(size);
}

/**
 * @brief Calculate the nearest power of 2 that is greater than or equal to the
 * given size.
 * @param size The size for which the nearest power of 2 is to be found.
 *
 * @return size_t The nearest power of 2 that is greater than or equal to the
 * given size.
 */
static inline size_t align_size(size_t size) {
    if (size == 0)
        return 1;
    if ((size & (size - 1)) == 0) // power of 2
        return size;
    size--;
    size |= size >> 1;
    size |= size >> 2;
    size |= size >> 4;
    size |= size >> 8;
    size |= size >> 16;
#if (ULONG_MAX == 0xffffffffffffffff)
    size |= size >> 32;
#endif
    size++;
    return size;
}

static inline int get_buddyNum(intptr_t addr, int order) {
    return (int) (addr >> order) % 2;
}

/**
 * @brief designed for adding metadata to "MemAllocator's" free_list
 * @param index the target index of free_list
 * @warning index is different from order for MemAllocator.
 */
static void util_list_addFirst(int index, MemMetaData *target) {
    MemMetaData *nextMeta = MemAllocator.free_list[index]->next;
    MemAllocator.free_list[index] = target;
    target->next = nextMeta;
}

/**
 * @brief designed for removing metadata from "MemAllocator's" free_list
 * @param index the target index of free_list
 * @warning index is different from order for MemAllocator.
 * @note to use this function, first check whether free_list[index] == NULL or
 * not
 * @return address of first element
 */
static MemMetaData *util_list_removeFirst(int index) {
    // assert(MemAllocator.free_list[index]);
    MemMetaData *meta = MemAllocator.free_list[index]->next;
    MemMetaData *nextMeta = meta->next;
    MemAllocator.free_list[index] = nextMeta;
    meta->next = NULL;
    return meta;
}
