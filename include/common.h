#ifdef clion

#include "external/kernel.h"
#include "external/klib-macros.h"
#include "external/klib.h"

#else
#include <kernel.h>
#include <klib-macros.h>
#include <klib.h>
#endif

#define PAGE_SIZE (4 * (1 << 10))        // 4 KB     2^12
#define MAX_REQUEST_MEM (16 * (1 << 20)) // 16 MB   2^24
#define SLAB_TYPES 9
#define MEM_METADATA_MAGIC 1

typedef struct util_list {
    struct util_list *next, *prev;
} UtilList;

typedef int SpinLock;
// typedef union page {
//     struct {           // anonymous struct
//         UtilList list; // link the page of the same slab_size together
//         SpinLock lock;
//         int cpu;
//         int slab_size; // 如果是0，则表示它不在缓存而在大内存中
//         int count;     // actual number of objects in this page
//         int capacity;  // the maximum number of objects

//         unsigned int bitmap[31];
//         void *base; // the address of first object which means the beginning
//         of
//     };
//     uint8_t data[PAGE_SIZE];
// } Page;

typedef struct buffer {
    int cpu;
    SpinLock lock;
    int free_num[SLAB_TYPES];
    UtilList slab_list[SLAB_TYPES]; // the genuine space allocated
    UtilList *freepage[SLAB_TYPES]; // the shortcut for find available space
    // in slab_list
} Buffer;

/***** memory metadata ************/
typedef struct mem_metadata {
    // TODO
    size_t size;   // the actual size of storage
    size_t offset; // relative to the beginning of page, represents the actual
    // address for storing. And offset is always positive
    int MAGIC;
    struct mem_metadata *next;
} MemMetaData;

/***** memory allocator ************/
/**
 * @ref
 * https://www.geeksforgeeks.org/buddy-memory-allocation-program-set-2-deallocation/
 *     addr     space   beginning
 *     ********************************************
 *     *  meta 1*   space 1              *  meta 2*
 *     *  data  *                        *  data  *
 *     ********************************************
 *     +---------------------------------+            +-----+  represents power
 * of 2 partition beginning of current page
 */
/**
 * @brief all size relating to memory_allocator is gross size rather than net
 * size, which means the size of metadata should be acconted for.
 */
struct memory_allocator {      // memory allocation is based on page
    int base_order; // the order of 'page size'
    int max_order;
    //  index -> order of size. (all sizes are power of two).
    //  free_list[actual_order - base_order] -> address
    MemMetaData *free_list[1 + 32 - 12];

    // index -> page's address >> base_order, mp[index] -> actual order
    // and actual order is valid if `actual order` >= `base_order`
    int registry[1 << 20]; // registry
};

/***** buffer manager **************/
