#ifdef clion

#include "../include/common.h"

#else
#include <common.h>
#endif
// the type of address is uintptr_t
// the type of size of space is size_t
// both of them are unsigned long int

// slab should ask for space in a row
// free_list's order should minus base order
// check magic
// should initial metadata
// the prior class has access to the metadata of inferior class
// todo clarify naming conventions
static struct memory_allocator MemAllocator;

struct slab_manager *SlabManagers;// the pointer to an array of slab managers


static inline int get_order(size_t size);

static inline size_t align_size(size_t size);

static inline int calculate_buddyNum(uintptr_t addr, int order);

static void util_list_addFirst(int index, MemMetaData *target);

static MemMetaData *util_list_removeFirst(int index);

static MemMetaData *util_list_retrieve_with_metaAddr(int index, uintptr_t target_metaAddr);


/**
 * give the start of a space in memory, return the address of memory metadata
 */
static inline MemMetaData *private__get_mem_metadata(uintptr_t addr) {
    return (MemMetaData *) (addr - (uintptr_t) sizeof(MemMetaData));
}

/**
 * give the address of memory metadata, return the address of space
 */
static inline uintptr_t private__get_mem_space_with_metaAddr(uintptr_t metaAddr) {
    return metaAddr + (uintptr_t) sizeof(MemMetaData);
}

/**
 * interpret this address as 'memory metadata' and initialize it
 */
MemMetaData *private__init_mem_metadata(const uintptr_t addr) {
    MemMetaData *meta = (MemMetaData *) addr;
    meta->MAGIC = MEM_METADATA_MAGIC;
    meta->next = NULL;

    meta->size = 0;
    return meta;
}

/**
 * @brief initialize the global memory allocator, aka. `MemAllocator`.
 * @note parameters of this function may not be aligned
 */
static void private__init_mem_allocator(uintptr_t startAddr, uintptr_t endAddr) {
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
    for (int i = 0; i < LENGTH(MemAllocator.registry); i++) {
        MemAllocator.registry[i] = 0;
    }

    MemAllocator.free_list[order - MemAllocator.base_order] = meta;
}

/**
 * @brief **private** function call of memory allocate in aid of MemAllocator
 * @param size the gross size that includes the metadata which controls the
 * following space.
 * @note
 *  <li>  the requested size should be greater than a page.
 *  <li> This function shouldn't be invoked directly.
 * @return the address of space truly used for containing;
 * @return return NULL, if there isn't available space anymore
 * @link https://www.geeksforgeeks.org/buddy-memory-allocation-program-set-1-allocation/ @endlink
 */
static uintptr_t private__mem_allocate(size_t size) {
    size = align_size(size);
    const int order = get_order(size);

    if (MemAllocator.free_list[order - MemAllocator.base_order]) {  // fitted space is available
        MemMetaData *meta = util_list_removeFirst(order - MemAllocator.base_order);
        uintptr_t addr = (uintptr_t) meta;
        MemAllocator.registry[addr >> MemAllocator.base_order] = order; // register
        return private__get_mem_space_with_metaAddr(addr);
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
        return (uintptr_t) NULL;
    }
    for (int i = available_order; i > order; i--) {
        MemMetaData *meta = util_list_removeFirst(i - MemAllocator.base_order);
        uintptr_t addr = (uintptr_t) meta;
        uintptr_t newAddr = addr + (1 << (i - 1));
        MemMetaData *newMeta = private__init_mem_metadata(newAddr);
        util_list_addFirst(i - 1 - MemAllocator.base_order, newMeta);
        util_list_addFirst(i - 1 - MemAllocator.base_order, meta);
    }
    MemMetaData *meta = util_list_removeFirst(order - MemAllocator.base_order);
    uintptr_t addr = (uintptr_t) meta;
    MemAllocator.registry[addr >> MemAllocator.base_order] = order;
    return private__get_mem_space_with_metaAddr(addr);
}

/**
 * @brief **public** function call of memory allocate in aid of MemAllocator.
 * Middle layer between slab and actual 'memory allocator'
 * @param size the net size, not including the metadata that controls the
 * following space.
 * @note  the parameter should be greater than the maximum size of slab which is
 * 4096, in other words, the requested size should be greater than a page size.
 * @return the address of requested space;
 * @return return NULL, if there isn't available space anymore
 * @see the physical storage model in "common.h"
 */
uintptr_t mem_allocate(size_t size) {
    size = align_size(size);
    size_t *p_offset = NULL;
    uintptr_t space = private__mem_allocate(
            size + sizeof(MemMetaData) + sizeof(*p_offset)); // sizeof(*p_offset) == sizeof(size_t)
    if (!space) return (uintptr_t) NULL;

    uintptr_t beginning = ROUNDUP(space, size);
    p_offset = (size_t *) (beginning - sizeof(*p_offset));
    *p_offset = beginning - space;

    MemMetaData *meta = private__get_mem_metadata(space);
    meta->size = size;
    return beginning;
}

/**
 * @brief **private** function call of memory deallocate in aid of MemAllocator.
 * @note address may not have been registered before, in this case, it is illegal.
 * Therefore, MemAllocator.registry as well as MAGIC should always be checked.
 * @param space in accordance with `private__mem_allocate`, this parameter should be the
 * address of space rather than metadata.
 * @return 0 if success; 1 if failed
 * @link https://www.geeksforgeeks.org/buddy-memory-allocation-program-set-2-deallocation/ @endlink
 */
int private__mem_deallocate(uintptr_t space) {
    MemMetaData *meta = private__get_mem_metadata(space);
    if (meta->MAGIC != MEM_METADATA_MAGIC) {
        return 1;
    }
    uintptr_t addr = (uintptr_t) meta;
    int order = MemAllocator.registry[addr >> MemAllocator.base_order];
    if (order < MemAllocator.base_order) {
        return 1;
    }
    MemAllocator.registry[addr >> MemAllocator.base_order] = 0; // register off

    // coalesce
    while (order < MemAllocator.max_order) {
        uintptr_t this_buddyAddr = (uintptr_t) meta;
        int this_buddyNum = calculate_buddyNum(this_buddyAddr, order);
        uintptr_t buddy_buddyAddr;
        if (this_buddyNum) { // this is right buddy (higher address) -> 1
            buddy_buddyAddr = this_buddyAddr - (1 << order);
        } else {// this is left buddy (lower address) -> 0
            buddy_buddyAddr = this_buddyAddr + (1 << order);
        }

        MemMetaData *buddyMeta = util_list_retrieve_with_metaAddr(order - MemAllocator.base_order, buddy_buddyAddr);
        if (!buddyMeta) break;
        if (this_buddyNum) { // right
            meta = buddyMeta;
        }
        order++;
    }
    util_list_addFirst(order - MemAllocator.base_order, meta);
    return 0;
}

/**
 * @brief **public** function call of memory deallocate in aid of MemAllocator.
 * use offset ahead of beginning to calculate address of space and simply pass it to private deallocate function.
 * @param beginning in accordance with `mem_allocate`, this parameter should be the
 * beginning of actual storage rather than metadata or space.
 * @return 0 if success; 1 if failed
 */
int mem_deallocate(uintptr_t beginning) {
    size_t *p_offset = (size_t *) (beginning - sizeof(size_t));
    uintptr_t space = (uintptr_t) (beginning - *p_offset);
    return private__mem_deallocate(space);
}


void private__init_slab_meta_data(SlabMetaData *metaData, int typeIndex) {
    // tackle the first
    mem_allocate(SLAB_INIT_PAGES_PER_TURN[typeIndex] * PAGE_SIZE);

    for (int i = 1; i < SLAB_INIT_TURNS[typeIndex]; ++i) {
    }
}

void private__init_slab_manager(uintptr_t addr) {
    struct slab_manager *manager = (struct slab_manager *) addr;
    for (int i = 0; i < SLAB_TYPES; ++i) {
        private__init_slab_meta_data(&manager->slabMetaDatas[i], i);
    }
}

/**
 * initialize the array of slab_manager, aka. `SlabManagers`.
 * @note
 */
void private__init_slab_managers(uintptr_t *p_startAddr) {
    uintptr_t start = ROUNDUP(*p_startAddr, sizeof(struct slab_manager));
    SlabManagers = (struct slab_manager *) start;
    for (int i = 0; i < cpu_count(); ++i) {
        private__init_slab_manager(start);
        start += sizeof(struct slab_manager);
    }
    *p_startAddr = start;
}


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
    // todo first make room for slab manager and then memory allocator
    uintptr_t start = (uintptr_t) heap.start;
    uintptr_t end = (uintptr_t) heap.end;
    private__init_slab_managers(&start);


    printf("Got %d MiB heap: [%p, %p)\n", pmsize >> 20, heap.start, heap.end);
}

MODULE_DEF(pmm) = {
        .init = pmm_init,
        .alloc = kalloc,
        .free = kfree,
};

/***** utility function ************/
/**
 * this function is designed for 'memory allocator', which calculates the order of power of 2 size.
 * And 2^order is less than or equal to the given size.
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

/**
 * @brief Calculates the buddy number for a given address and order.
 *
 * The buddy system is a memory allocation strategy that divides memory into blocks of size 2^n.
 * In this system, any given block has a "buddy" which is the adjacent block of the same size.
 *
 * @param addr The address for which the buddy number is to be calculated.
 * @param order The order used to calculate the buddy number.
 * @return 0 -> left buddy; 1 -> right buddy.
 */
static inline int calculate_buddyNum(uintptr_t addr, int order) {
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
    MemMetaData *meta = MemAllocator.free_list[index];
    MemMetaData *nextMeta = meta->next;
    MemAllocator.free_list[index] = nextMeta;
    meta->next = NULL;
    return meta;
}

/**
 * @brief designed for retrieving metadata with the given target address from
 * "MemAllocator's" free_list
 * @param index the target index of free_list
 * @param target_metaAddr the target address for **possible** metadata.
 * @note other than giving back the address of target metadata, this function also removes the target
 * metadata from free_list if it exits.
 * @warning index is different from order for MemAllocator.
 * @return NULL, if not found; else the same address as `target_metaAddr`.
 */
static MemMetaData *util_list_retrieve_with_metaAddr(int index, uintptr_t target_metaAddr) {
    if (!MemAllocator.free_list[index])return NULL; // no element
    if ((uintptr_t) MemAllocator.free_list[index] == target_metaAddr) {
        // the first element is target metadata
        return util_list_removeFirst(index);
    }
    // has elements and the first is not the target element.
    // find the predecessor that just before the target element.
    MemMetaData *p = MemAllocator.free_list[index];
    while (p->next && (uintptr_t) p->next != target_metaAddr) p = p->next;
    // reached the end and found nothing
    if (!p->next)return NULL;

    MemMetaData *targetMeta = (MemMetaData *) target_metaAddr;
    p->next = targetMeta->next;
    targetMeta->next = NULL;
    return targetMeta;
}
