#ifdef clion

#include "external/kernel.h"
#include "external/klib-macros.h"
#include "external/klib.h"

#else
#include <kernel.h>
#include <klib-macros.h>
#include <klib.h>
#endif

extern const size_t PAGE_SIZE;
extern const size_t MAX_REQUEST_MEM;
extern const int MEM_METADATA_MAGIC;
extern const int SLAB_METADATA_MAGIC;

#define SLAB_TYPES 5
// todo explain why remove 256
//  hint: 4096 / 256 = 16 = sizeof(bitmap)
// todo explain why slab has to be these sizes
// hint: for alignment
extern const int SLAB_CATEGORY[SLAB_TYPES];
extern const int SLAB_INIT_PAGES_PER_TURN[SLAB_TYPES];
// todo explain why
extern const int SLAB_INIT_TURNS[SLAB_TYPES];
//int SLAB_TOTAL_PAGES[] = {5, 8, 15, 12, 12};
// SLAB_TOTAL_PAGES[i] = SLAB_INIT_PAGES_PER_TURN[i] * SLAB_INIT_TURNS[i];

typedef int SpinLock;

inline void lock_init(SpinLock *lock) {
    atomic_xchg(lock, 0);
}

inline void lock_acquire(SpinLock *lock) {
    while (atomic_xchg(lock, 1));
}

inline void lock_release(SpinLock *lock) {
    atomic_xchg(lock, 0);
}

/***** BUDDY ALLOCATION ***********/
/**
 * physical memory partition model.
 * this kind of model can cause internal fraction.
 *     addr     space              beginning    addr
 *     ***************************************************
 *     *  meta 1*          |offset| space 1     *  meta 2*
 *     *  data  *          |      |             *  data  *
 *     ***************************************************
 *     +----------------------------------------+
 *
 *  1. +-----+  represents power of 2 partition beginning of current page
 *  2. offset means the distance between beginning and space:
 *          offset = beginning  - space;                   not take offset(size_t) into account
 *     rather than:
 *          offset = beginning - sizeof(size_t) - space;   take offset(size_t) into account
 *
 */
/***** memory metadata ************/
typedef struct mem_metadata {
    int MAGIC;
    struct mem_metadata *next;
} MemMetaData;

/***** memory allocator ************/
/**
 * @note all sizes relating to memory_allocator are gross sizes rather than net
 * sizes, which means that the size of metadata should be accounted for.
 */
struct memory_allocator {
    SpinLock lock;
    int base_order; // the order of 'page size'
    int max_order;
    /*  index <- order of size - base_order. (all sizes are power of two).
        free_list[index] -> address */
    MemMetaData *free_list[1 + 32 - 12]; // an array of pointer to MemMetaData.

    // index <- (page's address >> base_order)
    // mp[index] -> actual order and actual order is valid if `actual order` >= `base_order`
    int registry[1 << 20]; // registry
};

/***** SLAB ALLOCATION *************/

// one bitmap keeps track of a single group, a group contains (sizeof(bitmap) * 8) members.
typedef int16_t bitmap; // 2B or 16 bits for a single bitmap

/**
 * SENTINEL is designed for slab_manager exclusively;
 * INITIAL means the initial slab that can't be reused;
 * while REUSABLE means it can be returned to the memory.
 */
typedef enum status {
    SENTINEL, INITIAL, REUSABLE
} Status;

typedef struct slab_metadata {
    // circular doubly linked list, acting as a deque
    struct slab_metadata *next, *prev;
    int MAGIC;
    Status status;
    int typeSize; // such as 8,16...

    // below are unnecessary for sentinel
    int remaining; // how many cells are left
    int groups;
    bitmap *p_bitmap; // point to the start of bitmap;
    /* the distance between the beginning of slab_metadata and actual storage.
     * offset = actual storage address - slab_metadata; */
    size_t offset;
} SlabMetaData;

/***** slab manager ****************/
// every cpu has a single slab_manager
struct slab_manager {
    SpinLock lock;
    SlabMetaData sentinels[SLAB_TYPES]; // regard slab as node in singly linked list,
    // this line of code servers as an array of sentinel node for each slab type.
};
