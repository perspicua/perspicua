/*
 * malloc.c - Userspace heap allocator for Perspicua libc.
 *
 * Design
 * ──────
 * Backing memory is obtained from the kernel via sys_mmap in CHUNK_SIZE
 * page-aligned slabs.  Memory is never returned to the kernel; freed
 * blocks are recycled through an explicit doubly-linked free list.
 *
 * Every allocation (free or in-use) carries a header and a footer so
 * that adjacent free blocks can be coalesced in O(1) without scanning.
 *
 * Block layout (sizes in bytes, AArch64 — 8-byte words)
 * ───────────────────────────────────────────────────────
 *
 *   ┌─────────────────────────────┐  ← block start (header)
 *   │  size | flags   (8 bytes)  │    bit 0 = ALLOC flag
 *   ├─────────────────────────────┤
 *   │  payload ...               │    returned to caller
 *   │  (or free-list pointers    │    when block is free:
 *   │   prev/next  — 8 bytes ea) │      prev (8), next (8)
 *   ├─────────────────────────────┤
 *   │  size | flags   (8 bytes)  │  ← footer (mirrors header)
 *   └─────────────────────────────┘
 *
 * The size field always stores the *total* block size including header
 * and footer.  The footer of block N is immediately before the header
 * of block N+1, enabling O(1) neighbour lookup.
 *
 * Thread safety
 * ─────────────
 * A single global spinlock (AArch64 ldaxr/stxr) serialises all
 * malloc/free calls.  calloc and realloc are built on top of those.
 *
 * mmap flags
 * ──────────
 * We use PROT_READ|PROT_WRITE and MAP_PRIVATE|MAP_ANONYMOUS.
 * Adjust the constants below if your kernel uses different values.
 */

#include "syscall.h"
#include "types.h"

/* ── mmap constants (match your kernel's uapi) ───────────────────── */
#define MMAP_PROT_READ   0x1
#define MMAP_PROT_WRITE  0x2
#define MMAP_MAP_PRIVATE 0x2
#define MMAP_MAP_ANON    0x20

/* ── allocator tunables ──────────────────────────────────────────── */
#define PAGE_SIZE   4096UL
#define CHUNK_PAGES 16UL /* 64 kB per mmap call */
#define CHUNK_SIZE  (CHUNK_PAGES * PAGE_SIZE)
#define ALIGNMENT   16UL                  /* AArch64 ABI */
#define WORD        sizeof(size_t)        /* 8 bytes        */
#define HDR_SIZE    WORD                  /* header = 1 word */
#define FTR_SIZE    WORD                  /* footer = 1 word */
#define OVERHEAD    (HDR_SIZE + FTR_SIZE) /* per block       */
#define MIN_BLOCK   (OVERHEAD + 2 * WORD) /* hdr+prev+next+ftr */

/* ── block flag bits packed into the low bits of size ───────────── */
#define ALLOC_BIT 0x1UL
#define SIZE_MASK (~(ALIGNMENT - 1UL)) /* clears flag bits */

/* ── block accessors ─────────────────────────────────────────────── */
typedef size_t word_t;

/* Header/footer value encoding */
static inline word_t pack(size_t sz, int allocated)
{
    return (word_t)(sz | (size_t)allocated);
}

/* Read/write a word at an arbitrary byte address */
static inline word_t get(void* p)
{
    word_t v;
    __builtin_memcpy(&v, p, WORD);
    return v;
}
static inline void put(void* p, word_t v)
{
    __builtin_memcpy(p, &v, WORD);
}

/* Given a payload pointer, derive block start/end pointers */
#define HDRP(bp) ((char*)(bp) - HDR_SIZE)
#define FTRP(bp) ((char*)(bp) + get_size(bp) - OVERHEAD)

/* Extract size and alloc bit from a block's payload pointer */
static inline size_t get_size(void* bp)
{
    return get(HDRP(bp)) & SIZE_MASK;
}
static inline int get_alloc(void* bp)
{
    return (int)(get(HDRP(bp)) & ALLOC_BIT);
}

/* Neighbour blocks (also via payload pointers) */
#define NEXT_BLKP(bp) ((char*)(bp) + get_size(bp))
#define PREV_BLKP(bp) ((char*)(bp) - (get(((char*)(bp) - OVERHEAD)) & SIZE_MASK))

/* Free-list prev/next pointers stored inside the payload area */
#define PREV_PTR(bp) ((char**)(bp))
#define NEXT_PTR(bp) ((char**)((char*)(bp) + WORD))

/* ── spinlock (AArch64 ldaxr/stxr) ──────────────────────────────── */
typedef volatile int spinlock_t;
static spinlock_t heap_lock = 0;

static void lock_acquire(spinlock_t* lk)
{
    int tmp;
    __asm__ volatile("1: ldaxr  %w0, [%1]       \n" /* load-acquire exclusive  */
                     "   cbnz   %w0, 1b         \n" /* spin if already locked  */
                     "   stxr   %w0, %w2, [%1]  \n" /* try to store 1          */
                     "   cbnz   %w0, 1b         \n" /* retry if store failed   */
                     : "=&r"(tmp)
                     : "r"(lk), "r"(1)
                     : "memory");
}

static void lock_release(spinlock_t* lk)
{
    __asm__ volatile("stlr  wzr, [%0]" /* store-release 0 */
                     :
                     : "r"(lk)
                     : "memory");
}

/* ── free list (explicit, doubly-linked, LIFO insertion) ─────────── */
static char* free_list_head = NULL; /* payload pointer of first free block */

static void fl_insert(char* bp)
{
    *PREV_PTR(bp) = NULL;
    *NEXT_PTR(bp) = free_list_head;
    if (free_list_head)
        *PREV_PTR(free_list_head) = bp;
    free_list_head = bp;
}

static void fl_remove(char* bp)
{
    char* prev = *PREV_PTR(bp);
    char* next = *NEXT_PTR(bp);
    if (prev)
        *NEXT_PTR(prev) = next;
    else
        free_list_head = next;
    if (next)
        *PREV_PTR(next) = prev;
}

/* ── internal helpers ────────────────────────────────────────────── */

/*
 * align_up - Round sz up to the nearest multiple of ALIGNMENT,
 * ensuring at least MIN_BLOCK total size.
 */
static size_t align_up(size_t payload)
{
    size_t total = payload + OVERHEAD;
    if (total < MIN_BLOCK)
        total = MIN_BLOCK;
    return (total + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}

/*
 * set_block - Write header and footer for a block of given total size.
 */
static void set_block(char* bp, size_t total, int allocated)
{
    word_t val = pack(total, allocated);
    put(HDRP(bp), val);
    put(FTRP(bp), val);
}

/*
 * coalesce - Merge bp with any free neighbours.
 * Removes affected blocks from the free list, merges, re-inserts.
 * Returns the payload pointer of the resulting merged block.
 */
static char* coalesce(char* bp)
{
    size_t sz = get_size(bp);
    int prev_alloc = get_alloc(PREV_BLKP(bp));
    int next_alloc = get_alloc(NEXT_BLKP(bp));

    if (prev_alloc && next_alloc)
    {
        /* nothing to merge */
    }
    else if (prev_alloc && !next_alloc)
    {
        /* merge with next */
        fl_remove(NEXT_BLKP(bp));
        sz += get_size(NEXT_BLKP(bp));
        set_block(bp, sz, 0);
    }
    else if (!prev_alloc && next_alloc)
    {
        /* merge with prev — prev is in free list, bp is not yet */
        char* prev_bp = PREV_BLKP(bp);
        fl_remove(prev_bp);
        sz += get_size(prev_bp);
        bp = prev_bp;
        set_block(bp, sz, 0);
    }
    else
    {
        /* merge both — prev and next are in free list, bp is not yet */
        char* prev_bp = PREV_BLKP(bp);
        char* next_bp = NEXT_BLKP(bp);
        fl_remove(prev_bp);
        fl_remove(next_bp);
        sz += get_size(prev_bp) + get_size(next_bp);
        bp = prev_bp;
        set_block(bp, sz, 0);
    }

    fl_insert(bp);
    return bp;
}

/*
 * extend_heap - Ask the kernel for more memory and add it as one large
 * free block.  The new slab gets a prologue footer at the start and an
 * epilogue header at the end so coalesce() never walks off the edge.
 *
 *  [ prologue footer (ALLOC) | free block ... | epilogue header (ALLOC) ]
 *
 * Returns the payload pointer of the new free block, or NULL on failure.
 */
static char* extend_heap(size_t min_bytes)
{
    /* round up to a multiple of CHUNK_SIZE */
    size_t chunk = CHUNK_SIZE;
    while (chunk < min_bytes + OVERHEAD + 2 * WORD)
        chunk += CHUNK_SIZE;

    void* raw = sys_mmap(NULL, chunk, MMAP_PROT_READ | MMAP_PROT_WRITE, MMAP_MAP_PRIVATE | MMAP_MAP_ANON, -1, 0);
    if (!raw || (long)raw < 0)
        return NULL;

    char* mem = (char*)raw;

    /*
     * Layout within the slab:
     *
     *   mem+0:              prologue footer  (size=WORD, alloc=1)
     *   mem+WORD:           first free block header
     *   mem+2*WORD:         first free block payload  ← returned
     *   ...
     *   mem+chunk-WORD:     epilogue header  (size=0, alloc=1)
     */
    put(mem, pack(WORD, 1)); /* prologue footer  */

    char* bp = mem + WORD + HDR_SIZE;             /* payload of free block */
    size_t free_sz = chunk - 2 * WORD - OVERHEAD; /* subtract prologue + epilogue */

    /* align free_sz down so the epilogue lands correctly */
    free_sz &= ~(ALIGNMENT - 1UL);

    set_block(bp, free_sz + OVERHEAD, 0); /* the free block itself */

    put(FTRP(bp) + FTR_SIZE, pack(0, 1)); /* epilogue header  */

    /* don't coalesce on first insertion — no neighbours yet in this slab */
    fl_insert(bp);
    return bp;
}

/*
 * find_fit - First-fit search of the free list for a block of at
 * least `need` total bytes.  Returns payload pointer or NULL.
 */
static char* find_fit(size_t need)
{
    for (char* bp = free_list_head; bp; bp = *NEXT_PTR(bp))
    {
        if (get_size(bp) >= need)
            return bp;
    }
    return NULL;
}

/*
 * place - Mark a free block as allocated.  If the leftover after
 * carving out `need` bytes is large enough for a free block, split.
 */
static void place(char* bp, size_t need)
{
    size_t sz = get_size(bp);
    size_t remainder = sz - need;

    fl_remove(bp);

    if (remainder >= MIN_BLOCK)
    {
        /* split: allocate the front, free the tail */
        set_block(bp, need, 1);
        char* tail = NEXT_BLKP(bp);
        set_block(tail, remainder, 0);
        fl_insert(tail);
    }
    else
    {
        /* use the whole block (internal fragmentation ≤ MIN_BLOCK-1) */
        set_block(bp, sz, 1);
    }
}

/* ── public API ──────────────────────────────────────────────────── */

void* malloc(size_t size)
{
    if (size == 0)
        return NULL;

    size_t need = align_up(size);

    lock_acquire(&heap_lock);

    char* bp = find_fit(need);
    if (!bp)
    {
        bp = extend_heap(need);
        if (!bp)
        {
            lock_release(&heap_lock);
            return NULL;
        }
        /* extend_heap inserted bp into the free list; find_fit again
           so place() can split properly if the slab is larger than need */
        bp = find_fit(need);
        if (!bp)
        {
            lock_release(&heap_lock);
            return NULL;
        }
    }

    place(bp, need);

    lock_release(&heap_lock);
    return bp;
}

void free(void* ptr)
{
    if (!ptr)
        return;

    lock_acquire(&heap_lock);

    char* bp = (char*)ptr;
    size_t sz = get_size(bp);
    set_block(bp, sz, 0); /* mark free */
    coalesce(bp);         /* merge + insert into free list */

    lock_release(&heap_lock);
}

void* calloc(size_t nmemb, size_t size)
{
    /* check for overflow */
    if (nmemb && size > (size_t)-1 / nmemb)
        return NULL;
    size_t total = nmemb * size;

    void* ptr = malloc(total);
    if (!ptr)
        return NULL;

    /* zero the payload */
    char* p = (char*)ptr;
    for (size_t i = 0; i < total; i++)
        p[i] = 0;
    return ptr;
}

void* realloc(void* ptr, size_t size)
{
    if (!ptr)
        return malloc(size);
    if (size == 0)
    {
        free(ptr);
        return NULL;
    }

    lock_acquire(&heap_lock);
    size_t old_payload = get_size(ptr) - OVERHEAD;
    lock_release(&heap_lock);

    void* new_ptr = malloc(size);
    if (!new_ptr)
        return NULL;

    size_t copy = size < old_payload ? size : old_payload;
    char* src = (char*)ptr;
    char* dst = (char*)new_ptr;
    for (size_t i = 0; i < copy; i++)
        dst[i] = src[i];

    free(ptr);
    return new_ptr;
}
