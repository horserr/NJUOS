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
// todo change some @note to @pre
// todo clarify naming conventions
// todo check this(every) function's return cases.
static struct memory_allocator MemAllocator;

struct slab_manager *SlabManagers; // the pointer to an array of slab managers


static int get_order(size_t size);

static size_t align_size(size_t size);

static int calculate_buddyNum(uintptr_t addr, int order);

static void util_list_addFirst(int index, MemMetaData *target);

static MemMetaData *util_list_removeFirst(int index);

static MemMetaData *util_list_retrieve_with_metaAddr(int index, uintptr_t target_metaAddr);

static int util_bitmap_has_space(bitmap b);

static int util_bitmap_get_available_pos(bitmap b);

static void util_bitmap_flip_pos(bitmap *p_bitmap, int pos);


/**
 * give the start of a space in memory, return the address of memory metadata
 */
static MemMetaData *private__mem_get_metadata(const uintptr_t addr) {
    return (MemMetaData *) (addr - (uintptr_t) sizeof(MemMetaData));
}

/**
 * give the address of memory metadata, return the address of space
 */
static uintptr_t private__mem_get_space_with_metaAddr(const uintptr_t metaAddr) {
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
    const int order = get_order((size_t) (endAddr - startAddr));
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
 * @brief **private** function call of memory allocation in aid of MemAllocator
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

    if (MemAllocator.free_list[order - MemAllocator.base_order]) {
        // fitted space is available
        MemMetaData *meta = util_list_removeFirst(order - MemAllocator.base_order);
        const uintptr_t addr = (uintptr_t) meta;
        MemAllocator.registry[addr >> MemAllocator.base_order] = order; // register
        return private__mem_get_space_with_metaAddr(addr);
    }
    // fitted space isn't available
    int available_order = -1;
    for (int i = order + 1; i <= MemAllocator.max_order; i++) {
        if (MemAllocator.free_list[i - MemAllocator.base_order]) {
            available_order = i;
            break;
        }
    }
    if (available_order == -1) {
        // there is absolutely no space
        return (uintptr_t) NULL;
    }
    for (int i = available_order; i > order; i--) {
        MemMetaData *meta = util_list_removeFirst(i - MemAllocator.base_order);
        const uintptr_t addr = (uintptr_t) meta;
        const uintptr_t newAddr = addr + (1 << (i - 1));
        MemMetaData *newMeta = private__init_mem_metadata(newAddr);
        util_list_addFirst(i - 1 - MemAllocator.base_order, newMeta);
        util_list_addFirst(i - 1 - MemAllocator.base_order, meta);
    }
    MemMetaData *meta = util_list_removeFirst(order - MemAllocator.base_order);
    const uintptr_t addr = (uintptr_t) meta;
    MemAllocator.registry[addr >> MemAllocator.base_order] = order;
    return private__mem_get_space_with_metaAddr(addr);
}

/**
 * @brief **public** function call of memory allocation in aid of MemAllocator.
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
    const uintptr_t space = private__mem_allocate(
        size + sizeof(MemMetaData) + sizeof(*p_offset)); // sizeof(*p_offset) == sizeof(size_t)
    if (!space) return (uintptr_t) NULL;

    const uintptr_t beginning = ROUNDUP(space, size);
    p_offset = (size_t *) (beginning - sizeof(*p_offset));
    *p_offset = beginning - space;

    MemMetaData *meta = private__mem_get_metadata(space);
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
int private__mem_deallocate(const uintptr_t space) {
    MemMetaData *meta = private__mem_get_metadata(space);
    if (meta->MAGIC != MEM_METADATA_MAGIC) {
        return 1;
    }
    const uintptr_t addr = (uintptr_t) meta;
    int order = MemAllocator.registry[addr >> MemAllocator.base_order];
    if (order < MemAllocator.base_order) {
        return 1;
    }
    MemAllocator.registry[addr >> MemAllocator.base_order] = 0; // register off

    // coalesce
    while (order < MemAllocator.max_order) {
        const uintptr_t this_buddyAddr = (uintptr_t) meta;
        const int this_buddyNum = calculate_buddyNum(this_buddyAddr, order);
        uintptr_t buddy_buddyAddr;
        if (this_buddyNum) {
            // this is right buddy (higher address) -> 1
            buddy_buddyAddr = this_buddyAddr - (1 << order);
        } else {
            // this is left buddy (lower address) -> 0
            buddy_buddyAddr = this_buddyAddr + (1 << order);
        }

        MemMetaData *buddyMeta = util_list_retrieve_with_metaAddr(order - MemAllocator.base_order, buddy_buddyAddr);
        if (!buddyMeta) break;
        if (this_buddyNum) {
            // right
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
int mem_deallocate(const uintptr_t beginning) {
    const size_t *p_offset = (size_t *) (beginning - sizeof(size_t));
    const uintptr_t space = beginning - *p_offset;
    return private__mem_deallocate(space);
}

/**
 * Each slab is divided into multiple 'cells', where each cell is intended to
 * store a single instance of the object type that the slab manages.
 * The size of each cell is determined by the `typeSize` attribute.
 * <p>
 * Every time, this function is invoked, it first requests memory from the global
 * memory allocator which delivers a fit space. After setting up some attributes
 * of `struct slab_metadata`, it calculates how many bitmaps are suitable.
 * some useful equations are listed below:
 * - groups = number of bitmaps
 * - members per group = sizeof(bitmap) * 8
 * - capacity = (member per group) * groups
 * because bitmaps and cells share common space with each other, they must obey the rule:
 * - capacity <= number of cells.
 * This is because one group representing a bitmap is integrated and non-splittable.
 * This may entail some waste, but for convenience, such approach is tolerable.
 * Besides, in calculation, address alignment should always bear in mind.
 * <p>
 * What's more, if status is `init`, place the new metadata at the front of 'deque';
 * else if status is `reusable`, place it at the rear.
 * The reason for this is that every time search available space from initial pages
 * to reusable pages, rather than randomly pick up one.
 *
 * @param size the total size requesting `MemAllocator`
 * @note size must be multiple times of PAGE_SIZE.
 * @return the pointer to newMeta, if succeed; else, NULL.
 */
SlabMetaData *slab_request_mem(SlabMetaData *sentinel, const Status status, const size_t size) {
    SlabMetaData *newMeta = (SlabMetaData *) mem_allocate(size);
    if (!newMeta) {
        // newMeta is NULL
        return NULL;
    }
    newMeta->status = status;
    newMeta->typeSize = sentinel->typeSize;
    newMeta->MAGIC = SLAB_METADATA_MAGIC;

    uintptr_t start = (uintptr_t) (newMeta + sizeof(SlabMetaData));
    start = ROUNDUP(start, sizeof(bitmap));
    newMeta->p_bitmap = (bitmap *) start;

    const uintptr_t end = (uintptr_t) (newMeta + size);
    const int max_cell_num = (int) ((end - start) / newMeta->typeSize);
    // dynamically partition bitmaps and cells.
    int groups = 1;
    for (int i = max_cell_num; i > 0; --i) {
        const uintptr_t space = end - i * newMeta->typeSize; // the beginning of cell
        groups = (int) ((space - start) / sizeof(bitmap));
        const int capacity = groups * (int) (sizeof(bitmap) * 8);

        if (capacity > i) break;
    }
    // if newMeta->groups == 0, it means this slab is invalid
    newMeta->groups = groups - 1; // guarantee capacity <= number of cells
    newMeta->remaining = (int) (newMeta->groups * (sizeof(bitmap) * 8));
    newMeta->offset = (end - newMeta->remaining * newMeta->typeSize) - (uintptr_t) newMeta;
    // initialize bitmaps
    for (int i = 0; i < newMeta->groups; ++i) {
        newMeta->p_bitmap[i] = 0;
    }

    if (status == INITIAL) {
        newMeta->prev = sentinel;
        newMeta->next = sentinel->next;
        sentinel->next->prev = newMeta;
        sentinel->next = newMeta;
    } else if (status == REUSABLE) {
        newMeta->next = sentinel;
        newMeta->prev = sentinel->prev;
        sentinel->prev->next = newMeta;
        sentinel->prev = newMeta;
    }
    return newMeta;
}

/**
 * @brief Initializes the metadata for a SlabManager.
 *
 * This function sets up the initial state for managing slabs of different sizes
 * by creating sentinel nodes for each slab type. These sentinel nodes serve
 * as the heads of circular doubly linked lists, organizing the metadata for
 * slabs of each type. It then allocates initial memory blocks for these slabs
 * from the global memory allocator, setting the stage for efficient memory management.
 */
void private__init_slab_meta_data(SlabMetaData *metaData, const int typeIndex) {
    // create sentinel first
    metaData->next = metaData->prev = metaData;
    metaData->status = SENTINEL;
    metaData->typeSize = SLAB_CATEGORY[typeIndex];
    metaData->MAGIC = SLAB_METADATA_MAGIC;

    for (int i = 0; i < SLAB_INIT_TURNS[typeIndex]; ++i) {
        slab_request_mem(metaData, INITIAL,
                         SLAB_INIT_PAGES_PER_TURN[typeIndex] * PAGE_SIZE); // mind here
    }
}

/**
 * @brief Initializes a single slab manager.
 *
 * The slab manager is responsible for managing a specific set of slabs,
 * each corresponding to a different object size.
 * @param addr The memory address where the slab manager is to be initialized.
 * @pre This address is expected to be properly aligned and allocated.
 */
void private__init_slab_manager(const uintptr_t addr) {
    struct slab_manager *manager = (struct slab_manager *) addr;
    for (int i = 0; i < SLAB_TYPES; ++i) {
        private__init_slab_meta_data(&manager->slabMetaDatas[i], i);
    }
    // todo init lock
}

/**
 * @brief initialize the array of slab_manager, aka. `SlabManagers`.
 *
 * this function uses physical memory to make room for each SlabManager.
 * @param p_startAddr the pointer to the start address
 * @note the start address of the available space is changed after this function call.
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

/**
 * @brief **private** function call of slab allocation in aid of the dedicated slab manager.
 * @return addr of allocated space either by using the current slab storage or requested
 * from MemAllocator; NULL if not available in current slab storage AND MemAllocator denies
 * the request.
 */
uintptr_t private__slab_allocate(SlabMetaData *sentinel) {
    SlabMetaData *p = sentinel->next;
    while (p != sentinel) {
        if (p->remaining > 0) {
            for (int g = 0; g < p->groups; ++g) {
                if (!util_bitmap_has_space(p->p_bitmap[g]))continue;

                const int pos = util_bitmap_get_available_pos(p->p_bitmap[g]);
                util_bitmap_flip_pos(&p->p_bitmap[g], pos);
                p->remaining--;
                return (uintptr_t) p + p->offset +
                       (g * (sizeof(bitmap) * 8) + pos) * p->typeSize;
            }
        }
        p = p->next;
    }
    // no available space in current list of slabs, request a page once.
    SlabMetaData *newMeta = slab_request_mem(sentinel, REUSABLE, PAGE_SIZE);
    if (!newMeta) {
        // newMeta is NULL
        return (uintptr_t) NULL;
    }
    newMeta->remaining--;
    util_bitmap_flip_pos(newMeta->p_bitmap, 0);
    return (uintptr_t) newMeta + newMeta->offset;
}

/**
 * @brief **public** function call of slab allocation in aid of the dedicated slab manager.
 * @param manager
 * @param typeIndex which type needs.
 * @return same as `__slab_allocate`.
 * @see private__slab_allocate for more details.
 */
uintptr_t slab_allocate(struct slab_manager *manager, const int typeIndex) {
    // todo lock
    return private__slab_allocate(&manager->slabMetaDatas[typeIndex]);
}

/**
 * @return the index of fit(the first greater than or equal) slab size in `SLAB_CATEGORY`,
 * if find; else -1.
 */
int slab_get_typeIndex(const size_t size) {
    for (int i = 0; i < SLAB_TYPES; ++i) {
        if (SLAB_CATEGORY[i] >= size) {
            return i;
        }
    }
    return -1;
}

static void *kalloc(size_t size) {
    if (size > MAX_REQUEST_MEM) {
        return NULL;
    }
    void *ret = NULL;
    const int typeIndex = slab_get_typeIndex(size);
    if (typeIndex) {
        // suitable for slab
        const int cpu = cpu_current();
        ret = (void *) slab_allocate(&SlabManagers[cpu], typeIndex);
    } else {
        // too big for slab
        /* adjust the size to bigger or equal to PAGE_SIZE to fit in with `mem_allocate`.
         Admittedly, this is a kind of waste if SLAB_CATEGORY[-1] < size < PAGE_SIZE */
        size = size >= PAGE_SIZE ? size : PAGE_SIZE;
        ret = (void *) mem_allocate(size);
    }
    return ret;
}

/**
 * work around
 * a little tricky
 * @param addr
 * @return
 */
SlabMetaData *private__slab_get_metaData(const uintptr_t addr) {
    SlabMetaData *meta = (SlabMetaData *) ROUNDDOWN(addr, PAGE_SIZE);
    if (meta->MAGIC == SLAB_METADATA_MAGIC) {
        return meta;
    }
    for (int i = 0; i < SLAB_TYPES; ++i) {
        meta = (SlabMetaData *) ROUNDDOWN(addr, PAGE_SIZE * SLAB_INIT_PAGES_PER_TURN[i]);
        if (meta->MAGIC == SLAB_METADATA_MAGIC) return meta;
    }
    return NULL;
}

static int util_bitmap_test(const bitmap b, const int pos) {
    return b & (1 << pos);
}

static int private__slab_isEmpty(const SlabMetaData *metaData) {
    return metaData->remaining == (metaData->groups * (sizeof(bitmap) * 8));
}

/**
 * @brief return space to the global memory allocator.
 */
void slab_return_mem(SlabMetaData *metaData) {
    if (metaData->status != REUSABLE) return;

    SlabMetaData *p = metaData->prev;
    SlabMetaData *n = metaData->next;
    p->next = n;
    n->prev = p;
    mem_deallocate((uintptr_t) metaData);
}

int slab_deallocate(SlabMetaData *meta, const uintptr_t targetAddr) {
    if (meta->MAGIC != SLAB_METADATA_MAGIC) return 1;

    const int typeIndex = slab_get_typeIndex(meta->typeSize);
    if (typeIndex < 0 || meta->typeSize != SLAB_CATEGORY[typeIndex]) {
        // not the exact size
        return 1;
    }
    if (targetAddr % meta->typeSize) {
        // not aligned
        return 1;
    }
    if (meta->groups <= 0) return 1;

    // todo lock
    const size_t distance = targetAddr - ((uintptr_t) meta + meta->offset);
    const int num =  distance / meta->typeSize;
    const int g =  num / (sizeof(bitmap) * 8);
    if (g < 0 || g >= meta->groups) return 1;

    const int pos =  num % (sizeof(bitmap) * 8);
    if (pos < 0 || pos >= sizeof(bitmap) * 8) return 1;

    if (!util_bitmap_test(meta->p_bitmap[g], pos)) {
        // the target bit is 0
        return 1;
    }
    util_bitmap_flip_pos(&meta->p_bitmap[g], pos);
    meta->remaining++;
    if (private__slab_isEmpty(meta)) {
        slab_return_mem(meta);
    }
    return 0;
}

/**
 * different from allocation, as one cpu may allocate a space and then another cpu frees this.
 * @param ptr
 *
 * 其实还想要加一个iterator 来保证所有的的类型都检查到。
 */
static void kfree(void *ptr) {
    const uintptr_t addr = (uintptr_t) ptr;
    SlabMetaData *possible_slab_meta = private__slab_get_metaData(addr);
    int ret = 1;
    if (possible_slab_meta) {
        // this space is likely within slab
        ret = slab_deallocate(possible_slab_meta, addr);
    }
    if (ret) {
        mem_deallocate(addr);
    }
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
static int get_order(const size_t size) {
    // counting leading zeros;
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
static size_t align_size(size_t size) {
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
static int calculate_buddyNum(const uintptr_t addr, const int order) {
    return (int) (addr >> order) % 2;
}


/**
 * @brief designed for adding metadata to "MemAllocator's" free_list
 * @param index the target index of free_list
 * @param target
 * @warning index is different from order for MemAllocator.
 */
static void util_list_addFirst(const int index, MemMetaData *target) {
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
static MemMetaData *util_list_removeFirst(const int index) {
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
static MemMetaData *util_list_retrieve_with_metaAddr(const int index, const uintptr_t target_metaAddr) {
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

static int util_bitmap_has_space(const bitmap b) {
    return (~b) ? 1 : 0;
}

/**
 * fetch the index of available space(bit 0) in bitmap from lower to higher.
 * @return the index of first zero bit in bitmap.
 * @pre if `_bitmap_has_space(b)` is true, then this function can be called.
 * Otherwise, it is forbidden.
 */
static int util_bitmap_get_available_pos(const bitmap b) {
    // count trailing zeros
    return __builtin_ctz(~b);
}

/**
 * This function toggles the bit at the given position, changing it from 0 to 1
 * or from 1 to 0.
 * @param p_bitmap
 * @param pos the index of to be flipped bit.
 */
static void util_bitmap_flip_pos(bitmap *p_bitmap, const int pos) {
    *p_bitmap ^= (1 << pos);
}
