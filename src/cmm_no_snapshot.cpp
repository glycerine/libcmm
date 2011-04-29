/*
 * Copyright (c) 2009, Ralf Juengling
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met: 
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef __cplusplus
extern "C" {
#endif 



/*
 * Note: This implementation requires that malloc/realloc/calloc
 * deliver pointers that are 8-byte aligned (i.e., the lowest
 * three bits are clear). If your system does not do this, you
 * may try replacing malloc by posix_memalign and compile with 
 * _XOPEN_SOURCE set to 600.
 *
 * According to http://linux.die.net/man/3/posix_memalign,
 * "GNU libc malloc() always returns 8-byte aligned memory
 *  addresses, ..."
 */


#define _POSIX_SOURCE
#define _POSIX_C_SOURCE  199506L
#define _XOPEN_SOURCE    500

#define CMM_INTERNAL 1
#include "cmm.h"

#include <sys/types.h>
#include <unistd.h>



#ifndef NVALGRIND
#  include <valgrind/memcheck.h>
#else
#  define VALGRIND_CREATE_MEMPOOL(...)
#  define VALGRIND_MEMPOOL_ALLOC(...)
#  define VALGRIND_MEMPOOL_FREE(...)
#  define VALGRIND_CREATE_BLOCK(...)
#  define VALGRIND_MAKE_MEM_NOACCESS(...)
#  define VALGRIND_MAKE_MEM_DEFINED(...)
#  define VALGRIND_MAKE_MEM_UNDEFINED(...)
#  define VALGRIND_CHECK_MEM_IS_DEFINED(...)
#  define VALGRIND_DISCARD(...)
#endif

#include <unistd.h>
#include <stddef.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>

#define max(x,y)        ((x)<(y) ? (y) : (x))
#define min(x,y)        ((x)<(y) ? (x) : (y))

/* jea debug add fprintf(stdout, __VA_ARGS__) to line below */
/*#define cmm_printf(...)  fprintf(stdlog, __VA_ARGS__) */
#define cmm_printf(...) { fprintf(stdlog, __VA_ARGS__); fprintf(stdout, __VA_ARGS__); }

#define debug(...)      cmm_debug_enabled ? (\
        fprintf(stderr, "cmm(%s): ", C99__FUNC__), \
        fprintf(stderr, __VA_ARGS__), \
        fprintf(stdlog, "cmm(%s): ", C99__FUNC__), \
        fprintf(stdlog, __VA_ARGS__), 0) : 1

#define warn(...) do {						\
      fprintf(stderr, "cmm(%s): ", C99__FUNC__);			\
      fprintf(stderr, ##__VA_ARGS__); fflush(stderr); } while(0)



#define PPTR(p)         ((uintptr_t)(p))

#define PAGEBITS        12
#define BLOCKBITS       PAGEBITS
#if defined  PAGESIZE && (PAGESIZE != (1<<PAGEBITS))
#  error Definitions related to PAGESIZE inconsistent
#elif !defined PAGESIZE
#  define PAGESIZE      (1<<PAGEBITS)
#endif
#define BLOCKSIZE       (1<<BLOCKBITS)


#define ALIGN_NUM_BITS  3
#define MIN_HUNKSIZE    (1<<ALIGN_NUM_BITS)

/* jea temp decrease to observe : works with 0x015, but not 0x014
//#define MIN_NUMBLOCKS   0x100
*/
#define MIN_NUMBLOCKS   0x015

#define MIN_TYPES       0x100
#define MIN_MANAGED     0x40000
#define MIN_ROOTS       0x100
#define MIN_STACK       0x1000
#define MAX_VOLUME      (0x800000*sizeof(void *))    /* max volume threshold */
#define MAX_BLOCKS      (150*sizeof(void *))
#define NUM_TRANSFER    (PIPE_BUF/sizeof(void *))
#define NUM_IDLE_CALLS  100

#define HMAP_NUM_BITS   4

#ifndef INT_MAX
#  error INT_MAX not defined.
#elif INT_MAX == 9223372036854775807L
#  define HMAP_EPI       16
#  define HMAP_EPI_BITS   4
#elif INT_MAX == 2147483647
#  define HMAP_EPI        8
#  define HMAP_EPI_BITS   3
#elif INT_MAX == 32767
#  define HMAP_EPI        4
#  define HMAP_EPI_BITS   2
#else
#  error Value of INT_MAX not supported.
#endif


typedef struct hunk {
   struct hunk *next;
} hunk_t;

typedef struct block {
   struct block *next;
} block_t;

typedef struct blockrec {
   mt_t       t;            /* type directory entry    */
   int        in_use;       /* number of object in use */
} blockrec_t;

typedef struct info {
   mt_t       t;            /* type of memory object   */
   uint32_t   nh;           /* size of memory object in multiples of MIN_HUNKSIZE !! */
} info_t;

typedef struct typerec {
   char            *name;
   size_t          size;    /* zero when variable size */
   clear_func_t    *clear; 
   mark_func_t     *mark;
   finalize_func_t *finalize;
   uintptr_t       current_a;     /* current address   */
   uintptr_t       current_amax; 
   int             next_b;        /* next block to try */
} typerec_t;

/* heap management */
static size_t     heapsize;
static size_t     hmapsize;
static size_t     volume_threshold;
static int        block_threshold;
static int        num_blocks;
static int        num_free_blocks;
static blockrec_t *blockrecs = NULL;
static char      *heap = NULL;
static unsigned int *RESTRICTC99 hmap = NULL; /* bits for heap objects */
static bool       heap_exhausted = false;

static void *    * RESTRICTC99 managed;
static int        man_size;
static int        man_last = -1;
static int        man_k = -1;
static int        man_t = 0;
static bool       man_is_compact = true;

static void **   *RESTRICTC99 roots;
static int        roots_last = -1;
static int        roots_size;

/* type registry */
static typerec_t *types;
static mt_t       types_last = -1;
static mt_t       types_size;
static int       *profile = NULL;
static int        num_profiles = 0;

/* the marking stack */
static C99_CONST void *RESTRICTC99 *stack = NULL;
static int        stack_size = MIN_STACK;
static int        stack_last = -1;
static bool       stack_overflowed  = false;
static bool       stack_overflowed2 = false;

/* other state variables */
static int        num_allocs = 0;
static int        num_alloc_blocks = 0;
static int        num_collects = 0;
static size_t     vol_allocs = 0;
static bool       gc_disabled = false;
static int        fetch_backlog = 0;
static bool       collect_in_progress = false;
static bool       mark_in_progress = false;
static bool       collect_requested = false;
static pid_t      collecting_child = 0;
static notify_func_t *client_notify = NULL;
static mt_t       marking_type = mt_undefined;
static C99_CONST void *marking_object = NULL;
static FILE *     stdlog = NULL;
bool              cmm_debug_enabled = false;

/* transient object stack */
typedef C99_CONST void    *stack_elem_t; 
typedef stack_elem_t  *stack_ptr_t;
typedef struct stack cmmstack_t;

 /* dump() calling d and ds(cmmstack_t) debug macros */
#ifndef NDEBUG
#define d()   dump(__FUNCTION__,__LINE__,0)
#define ds(st) dump(__FUNCTION__,__LINE__,st)
#else
#define d() 
#define ds(st)
#endif

static cmmstack_t   *make_stack(void);
static void         stack_push(cmmstack_t *, stack_elem_t);
static stack_elem_t stack_peek(cmmstack_t *);
static stack_elem_t stack_pop(cmmstack_t *);
static bool         stack_empty(cmmstack_t *);
static void         stack_reset(cmmstack_t *, stack_ptr_t);
static stack_elem_t stack_elt(cmmstack_t *, int);

static mt_t       mt_stack;
static mt_t       mt_stack_chunk;
cmmstack_t * C99_CONST _cmm_transients;

/*
 * CMM's little helpers
 */

#define BITL               1  /* LIVE/OBSOLETE address */
#define BITN               2  /* NOTIFY                */
#define BITB               4  /* type blob             */
#define BITM               8

/* Note: The same bit is used to mark an address LIVE or
 * OBSOLETE. The LIVE bit is only needed in the marking
 * phase and we make sure all OBSOLETEs are removd before
 * we start marking.
 */

#define LIVE(p)            (((uintptr_t)(p)) & BITL)
#define NOTIFY(p)          (((uintptr_t)(p)) & BITN)
#define BLOB(p)            (((uintptr_t)(p)) & BITB)
#define OBSOLETE           LIVE

#define MARK_LIVE(p)       { p = (void *)((uintptr_t)(p) | BITL); }
#define MARK_NOTIFY(p)     { p = (void *)((uintptr_t)(p) | BITN); }
#define MARK_BLOB(p)       { p = (void *)((uintptr_t)(p) | BITB); }
#define MARK_OBSOLETE      MARK_LIVE

#define UNMARK_LIVE(p)     { p = (void *)((uintptr_t)(p) & ~BITL); }
#define UNMARK_NOTIFY(p)   { p = (void *)((uintptr_t)(p) & ~BITN); }

#define HMAP(a, op, b) (hmap[(((uintptr_t)(a))>>(ALIGN_NUM_BITS + HMAP_EPI_BITS))] op \
                        ((b) << (((((uintptr_t)(a))>>ALIGN_NUM_BITS) & (HMAP_EPI-1)) * HMAP_NUM_BITS)))
#define HMAP_LIVE(a)       HMAP(a, &, BITL)
#define HMAP_NOTIFY(a)     HMAP(a, &, BITN)
#define HMAP_MANAGED(a)    HMAP(a, &, BITM)

#define HMAP_MARK_LIVE(a)     HMAP(a, |=, BITL)
#define HMAP_MARK_NOTIFY(a)   HMAP(a, |=, BITN)
#define HMAP_MARK_MANAGED(a)  HMAP(a, |=, BITM)

#define HMAP_UNMARK_LIVE(a)     HMAP(a, &=~, BITL)
#define HMAP_UNMARK_NOTIFY(a)   HMAP(a, &=~, BITN)
#define HMAP_UNMARK_MANAGED(a)  HMAP(a, &=~, BITM)

#define LBITS(p)           ((uintptr_t)(p) & (MIN_HUNKSIZE-1))
#define CLRPTR(p)          ((void *)((((uintptr_t)(p)) & ~(MIN_HUNKSIZE-1))))
#define ADDRESS_VALID(p)   (!LBITS(p))
#define TYPE_VALID(t)      ((t)>=0 && (t)<=types_last)
#define INDEX_VALID(i)     ((i)>=0 && (i)<=man_last)


#define INHEAP(p)          (((char *)(p) >= heap) && ((char *)(p) < (heap + heapsize)))
#define BLOCK(p)           (((ptrdiff_t)((char *)(p) - heap))>>BLOCKBITS)
#define BLOCK_ADDR(p)      (heap + BLOCKSIZE*BLOCK(p))
#define BLOCKA(a)          (((uintptr_t)((char *)(a)))>>BLOCKBITS)
#define INFO_S(p)          (BLOB(p) ? 0 : ((info_t *)(unseal((char *)p)))->nh*MIN_HUNKSIZE)
#define INFO_T(p)          (BLOB(p) ? mt_blob : ((info_t *)(unseal((char *)p)))->t)

#define FIX_SIZE(s)        if (s % MIN_HUNKSIZE) \
                              s = ((s>>ALIGN_NUM_BITS)+1)<<ALIGN_NUM_BITS

#define ABORT_WHEN_OOM(p)  if (!(p)) { warn("allocation failed\n"); abort(); }

/* loop over all managed addresses in small object heap */
/* NOTE: the real address is 'heap + a', b is the block */
#define DO_HEAP(a, b) { \
   uintptr_t __a = 0; \
   for (int b = 0; b < num_blocks; b++, __a += BLOCKSIZE) { \
      if (blockrecs[b].in_use > 0) { \
         uintptr_t __a_next = __a + BLOCKSIZE; \
         for (uintptr_t a = __a; a < __a_next; a += MIN_HUNKSIZE) { \
            if (HMAP_MANAGED(a))

#define DO_HEAP_END }}}}

/* loop over all other managed addresses                */
/* NOTE: the real address is 'managed[i]', which is not */
/* a cleared address (use CLRPTR to clear)             */ 
#define DO_MANAGED(i) { \
  int __lasti = collect_in_progress ? man_k : man_last; \
  for (int i = 0; i <= __lasti; i++) { \

#define DO_MANAGED_END }}

#define DISABLE_GC \
   bool __nogc = gc_disabled; \
   gc_disabled = true;

#define ENABLE_GC gc_disabled = __nogc;

#  define cmm_collect()          /* noop */
#  define fetch_unreachables()  /* noop */


//STATICFUNC void *seal(C99_CONST char *p)
STATICFUNC void *seal(C99_CONST void *p)
{
   p = CLRPTR(p);
   VALGRIND_MAKE_MEM_NOACCESS(p, MIN_HUNKSIZE);
   char* pc = (char*) p;
   pc += MIN_HUNKSIZE;
   return (void *)pc;
}

//STATICFUNC void *unseal(C99_CONST char *p)
STATICFUNC void *unseal(C99_CONST void *p)
{
   p = CLRPTR(p);
   char* pc = (char*) p;
   pc -= MIN_HUNKSIZE;
   VALGRIND_MAKE_MEM_DEFINED((void*)pc, MIN_HUNKSIZE);
   return (void *)pc;
}

STATICFUNC void check_num_free_blocks(void)
{
   int n = 0;
   for (int b=0; b < num_blocks; b++)
     if (blockrecs[b].t == mt_undefined)
        n++;
   assert(n==num_free_blocks);
}

STATICFUNC int _find_managed(C99_CONST void *p);
STATICFUNC bool live(C99_CONST void *p)
{
   ptrdiff_t a = ((char *)p) - heap;
   if (a>=0 && (unsigned long)a<heapsize) {
      assert(HMAP_MANAGED(a));
      return HMAP_LIVE(a);
   }
   int i = _find_managed(p);
   assert(i != -1);
   return LIVE(managed[i]);
}

STATICFUNC void mark_live(C99_CONST void *p)
{
   ptrdiff_t a = ((char *)p) - heap;
   if (a>=0 && (unsigned long)a<heapsize) {
      assert(HMAP_MANAGED(a));
      HMAP_MARK_LIVE(a);
      return;
   }
   int i = _find_managed(p);
   assert(i != -1);
   MARK_LIVE(managed[i]);
}

STATICFUNC void maybe_trigger_collect(size_t s)
{
   num_allocs += 1;
   vol_allocs += s;
   if (gc_disabled || collect_in_progress)
      return;

   if (num_alloc_blocks >= block_threshold || 
       vol_allocs >= volume_threshold      ||
       collect_requested) {
      cmm_collect_now();
   }
}

#define AMAX(s) ((BLOCKSIZE/(s) - 1)*(s))
 
/* scan small object heap for next free hunk                 */
/* update current_a field for type t, return true on success */  
STATICFUNC bool _update_current_a(mt_t t, typerec_t *tr, uintptr_t s, uintptr_t a)
{
   if (blockrecs[BLOCKA(a)].t != t)
      goto search_for_block;

search_in_block:
   /* search for next free hunk in block */
   while (a <= tr->current_amax && HMAP_MANAGED(a))
      a += s;
   if (a <= tr->current_amax) {
      tr->current_a = a;
      return true;
   }

search_for_block:
   /* search for another block with free hunks */
   ;
   int b = tr->next_b;
   int orig_b = (b+num_blocks-1) % num_blocks;

   while (b != orig_b) {
      if (blockrecs[b].t == mt_undefined) {
         assert(blockrecs[b].in_use == 0);
         blockrecs[b].t = t;
         VALGRIND_CREATE_BLOCK(heap + a, BLOCKSIZE, types[t].name);
         tr->current_a = a = b*BLOCKSIZE;
         tr->current_amax = a + AMAX(s);
         tr->next_b = (b == num_blocks) ? 1 : b+1;
         num_alloc_blocks++;
         num_free_blocks--;
         return true;

      } else if (blockrecs[b].t==t && blockrecs[b].in_use<(long)(BLOCKSIZE/s)) {
         a = b*BLOCKSIZE;
         tr->current_amax = a + AMAX(s);
         tr->next_b = (b == num_blocks) ? 1 : b+1;
         goto search_in_block;
      }
      b = (b+1) % num_blocks;
   }

   /* no free hunk found */
   assert(b == orig_b);
   heap_exhausted = true;
   tr->current_a = 0;
   tr->current_amax = AMAX(s);
   tr->next_b = 0;
   return false;
}

STATICFUNC bool update_current_a(mt_t t)
{
   typerec_t *tr = &types[t];

   if (blockrecs[BLOCKA(tr->current_a)].t == t) {
      tr->current_a += tr->size;
      if (tr->current_a <= tr->current_amax) {
         if (!HMAP_MANAGED(tr->current_a))
            return true;
      } else
         tr->current_a -= tr->size;
   }   
   return _update_current_a(t, tr, tr->size, tr->current_a);
}

/* allocate from small-object heap if possible */
STATICFUNC void *alloc_fixed_size(mt_t t)
{
   if (collect_in_progress && !collecting_child)
      return NULL;

   if (heap_exhausted || !update_current_a(t))
      return NULL;

   void *p = heap + types[t].current_a;
   VALGRIND_MEMPOOL_ALLOC(heap, p, types[t].size);
   blockrecs[BLOCKA(types[t].current_a)].in_use++;
   
   return p;
}

/* allocate with malloc */
STATICFUNC void *alloc_variable_sized(mt_t t, size_t s)
{
   info_t *info = (info_t*)0;

   /* make sure s is a multiple of MIN_HUNKSIZE */
   assert(s%MIN_HUNKSIZE == 0);


   if (s > CMM_SIZE_MAX) {
      warn("size exceeds CMM_SIZE_MAX\n");
      abort();
   }
  
   void *p = NULL;
   /* don't allocate from heap when t==mt_stack */
   if (t && types[t].size>=s && BLOCKSIZE>=s)
      if ((p = alloc_fixed_size(t)))
         goto fetch_and_return;
   
malloc:
   p = malloc(s + MIN_HUNKSIZE);  // + space for info

   if (!p && collecting_child) {
      if (gc_disabled)
         warn("low memory and GC disabled\n");
      else /* try to recover */
         while (collecting_child)
            fetch_unreachables();
      goto malloc; 
   }
   if (!p)
      return NULL;

   assert(!LBITS(p));

   info = (info_t*)p;
   info->t = t;
   info->nh = s/MIN_HUNKSIZE;
   p = seal(p);

fetch_and_return:
   maybe_trigger_collect(s);
   return p;
}

// printf("address 0x%"PRIxPTR" (in block %d) is marked live\n", PPTR(heap + a), b);
//    translates to
// printf("address 0x%""l" "x"" (in block %d) is marked live\n", ((uintptr_t)(heap + a)), b);

STATICFUNC bool no_marked_live(void)
{
   uintptr_t __a = 0;
   for (int b = 0; b < num_blocks; b++, __a += BLOCKSIZE) {
      uintptr_t __a_next = __a + BLOCKSIZE;
      for (uintptr_t a = __a; a < __a_next; a += MIN_HUNKSIZE) {
         if (HMAP_LIVE(a)) {
            warn("address 0x%lx (in block %d) is marked live\n", PPTR(heap + a), b);
            printf("address 0x%lx (in block %d) is marked live\n", PPTR(heap + a), b);
            printf("address(%p) + (216 decimal, aka 0xd8 hex) bytes --> %p had values: 0x%x hex, or %d decimal. \n", (heap+a), ((heap + a) + 216), *((heap + a) + 216), *((heap + a) + 216) );
            printf("address(%p) + (248 decimal, aka 0xf8 hex) bytes --> %p had values: 0x%x hex, or %d decimal. \n", (heap+a), ((heap + a) + 248), *((heap + a) + 248), *((heap + a) + 248) );
	    assert(0); 
            return false;
         }
      }
   }

   for (int i = 0; i <= man_last; i++) {
      if (LIVE(managed[i])) {
         void *p = CLRPTR(managed[i]);
         warn("address 0x%lx is marked live\n", PPTR(p));
         return false;
      }
   }

   return true;
}


/* 
 * Sort managed array in-place using poplar sort.
 * Information Processing Letters 39, pp 269-276, 1991.

 * Rationale: poplar sort has O(N log N) upper bound on worst case complexity,
    and is an in-situ sort, so it is preferrable to quicksort (worst case N^2 complexity)
    and is acceptable for realtime systems. 

    For arrays that are almost sorted already, poplar sort is much 
    faster than heapsort and competitive with quicksort.

 */

#define MAX_POPLAR 31
static int   poplar_roots[MAX_POPLAR+2] = {-1};
static bool  poplar_sorted[MAX_POPLAR];

#define M(p,q) (((p)+(q))/2)
#define A(q)   (managed[q])

#define SWAP(i,j)             \
{                             \
   void *p = managed[i];      \
   managed[i] = managed[j];   \
   managed[j] = p;            \
}

STATICFUNC void sift(int p, int q)
{
   if (q-p > 1) {
      int x = q;
      int m = M(p,q);
      if (A(q-1) > A(x)) x = q-1;
      if (A(m) > A(x)) x = m;

      if (x != q) {
         SWAP(x,q);
	 if (x == q-1) sift(m, x); else sift(p, x);
      }
   }
}

/* increment man_k to man_last while maintaining the poplar invariant */
STATICFUNC void update_man_k(void)
{
   assert(!collect_in_progress);
#  define r poplar_roots

   while (man_k < man_last) {
      man_k++;
      if ((man_t>=2) && (man_k-1+r[man_t-2] == 2*r[man_t-1])) {
         r[--man_t] = man_k;
         sift(r[man_t-1], man_k);
         poplar_sorted[man_t-1] = false;
      } else {
         r[++man_t] = man_k;
         poplar_sorted[man_t-1] = true;
      }
   }
#  undef r
}

/* sort a single poplar using poplar sort ;-) */
STATICFUNC void sort_poplar(int n)
{
   assert(!collect_in_progress);
   
   if (poplar_sorted[n]) return;

   int r[MAX_POPLAR+1], t = 1;
   r[0] = poplar_roots[n];
   r[1] = poplar_roots[n+1];
   
   /* sort, right to left, by always taking the max root */
   int k = r[1]+1;
   while (--k > r[0]) {

      /* move maximum element to the right */
      int m = t;
      for (int j=1; j<t; j++)
	 if (A(r[m]) < A(r[j])) m = j;
      
      if (m != t) {
	 SWAP(r[m], r[t]);
	 sift(r[m-1], r[m]);
      }
      
      if (r[t-1] == k-1) {
	 t--;
      } else {
	 r[t] = M(r[t-1],k);
	 r[++t] = k-1;
      }
   }
   poplar_sorted[n] = true;
}

#undef A
#undef M
#undef SWAP

/* Remove obsolete entries from managed. */
STATICFUNC void compact_managed(void)
{
   assert(!collect_in_progress);

   if (man_is_compact) return;

   /* every poplar is sorted, so we set man_k to the last element */
   /* of the first poplar and recompute the poplar roots quickly  */
   assert(man_t > 0);
   man_k = poplar_roots[1];

   int i, n = 0;
   for (i = 0; i <= man_k; i++) {
      if (!OBSOLETE(managed[i]))
         managed[n++] = managed[i];
   }
   man_k = n-1;
   for (; i <= man_last; i++) {
      if (!OBSOLETE(managed[i]))
         managed[n++] = managed[i];
   }
   man_last = n-1;

   /* rebuild poplars */
#if 1
   {
      /* this only works when managed was sorted up to man_k */
      n = man_k + 1;
      man_t = 0;
      poplar_roots[man_t] = -1;
      int m = (1<<(MAX_POPLAR-1))-1;
      while (n) {
         if (n >= m) {
            poplar_roots[man_t+1] = poplar_roots[man_t] + m;
            poplar_sorted[man_t] = true;
            man_t++;
            n -= m;
         } else
            m = m>>1;
      }
   }
#else
   man_k = -1;
   man_t = 0;
   update_man_k();
#endif
   
   /* shrink when much bigger than necessary */
   if (man_last*4<man_size && man_size > MIN_MANAGED) {
      man_size /= 2;
      debug("shrinking managed table to %d\n", man_size);
      managed = (void **)realloc(managed, man_size*sizeof(void *));
      assert(managed);
   }
   man_is_compact = true;
}


/* binary search in sorted part of managed */
STATICFUNC int bsearch_managed(C99_CONST void *p, int l, int r)
{
   while (r-l > 1) {
      int n = (r+l)/2;
      if (p >= managed[n])
         l = n;
      else
         r = n;
   }
   if (CLRPTR(managed[l]) == p)
      return l;

   else if (l<man_k && (CLRPTR(managed[l+1]) == p))
      return l+1;

   else 
      return -1;
}

STATICFUNC int _find_managed(C99_CONST void *p)
{
   int i = -1;
   for (int n = 0; n<man_t && i<0; n++) {
      if (!poplar_sorted[n]) sort_poplar(n);
      i = bsearch_managed(p, poplar_roots[n]+1, poplar_roots[n+1]+1);
   }

   if (i==-1 || collect_in_progress) {
      /* do linear search on excess part */
      for (int n = man_last; n > man_k; n--)
         if (CLRPTR(managed[n]) == p) {
            i = n;
            break;
         }
   }
   return i;
}

/* use this one only when not marking */
STATICFUNC int find_managed(C99_CONST void *p)
{
   assert(!mark_in_progress);
   int i = _find_managed(p);
   if (i>-1 && OBSOLETE(managed[i]))
      i = -1;
   return i;
}

/*
 * Add address to list of managed addresses in O(1) 
 * amortized time. Do not check for duplicates.
 */
STATICFUNC void add_managed(C99_CONST void *p)
{
   man_last++;
   if (man_last == man_size) {
      man_size *= 2;
      debug("enlarging managed table to %d\n", man_size);
      managed = (void **)realloc(managed, man_size*sizeof(void *));
      ABORT_WHEN_OOM(managed);
   }
   assert(man_last < man_size);
   managed[man_last] = (void *)p;
}

STATICFUNC void manage(C99_CONST void *p, mt_t t)
{
   assert(ADDRESS_VALID(p));

   ptrdiff_t a = ((char *)p) - heap;
   if (a>=0 && a<(long)heapsize) {
      assert(!HMAP_MANAGED(a));
      HMAP_MARK_MANAGED(a);
   } else
      add_managed(p);
   
   stack_push(_cmm_transients, p);

   if (profile)
      profile[t]++;
}

STATICFUNC void collect_prologue(void)
{
   assert(!collect_in_progress);
   assert(!collecting_child);
   assert(!stack_overflowed2);
   if (cmm_debug_enabled)
      assert(no_marked_live());
   
   /* prepare managed array and poplar data */
   update_man_k();
   for (int n = 0; n < man_t; n++)
      sort_poplar(n);
   assert(man_k == man_last);

   collect_in_progress = true;
   if (cmm_debug_enabled) {
      check_num_free_blocks();
      debug("%dth collect after %d allocations:\n",
            num_collects, num_allocs);
      debug("mean alloc %.2f bytes, %d free blocks, down %d blocks)\n",
            (num_allocs ? ((double)vol_allocs)/num_allocs : 0),
            num_free_blocks, num_alloc_blocks);
   }
}

STATICFUNC void collect_epilogue(void)
{
   assert(collect_in_progress);

   if (stack_overflowed2) {
      /* only effective with synchronous collect */
      stack_size *= 2;
      debug("enlarging marking stack to %d\n", stack_size);
      stack_overflowed2 = false;
   }

   marking_type = mt_undefined;
   marking_object = NULL;
   collect_requested = false;

   heap_exhausted = false;
   collect_in_progress = false;
   num_alloc_blocks = 0;
   num_collects += 1;
   num_allocs = 0;
   vol_allocs = 0;
   fetch_backlog = 0;
   compact_managed();
}

/*
 * Push address onto marking stack and mark it if requested.
 */

STATICFUNC void __cmm_push(C99_CONST void *p)
{
   mark_live(p);
   
   stack_last++;
   if (stack_last == stack_size) {
      stack_overflowed = true;
      stack_last--;
   } else {
      stack[stack_last] = p;
   }
}

void _cmm_push(C99_CONST void *p)
{
   if (live(p))
      return;
   else
      __cmm_push(p);
}

static C99_CONST void *pop(void)
{
   return ((stack_last<0) ? NULL : stack[stack_last--]);
}

STATICFUNC bool empty(void)
{
   return stack_last==-1;
}

STATICFUNC void recover_stack(void)
{
   d();
   if (!stack_overflowed) return;
   debug("marking stack overflowed, recovering\n");
   assert(empty());
   stack_overflowed = false;
   stack_overflowed2 = true;

   /* mark children of all live objects */
//   DO_HEAP(a, b) {
   {
      { uintptr_t __a = 0; 
	 for (int b = 0; b < num_blocks; b++, __a += (1<<12)) { 
	    if (blockrecs[b].in_use > 0) { 
	       uintptr_t __a_next = __a + (1<<12); 
	       for (uintptr_t a = __a; a < __a_next; a += (1<<3)) { 
		  if ((hmap[(((uintptr_t)(a))>>(3 + 3))] & ((8) << (((((uintptr_t)(a))>>3) & (8-1)) * 4))))

		     
		     if (HMAP_LIVE(a)) {
			mark_func_t *mark = types[blockrecs[b].t].mark;
			if (mark)
			   mark(heap + a);
		     }
	       } 
	    }
	 }
      }
   }
//   } DO_HEAP_END;
      
//   DO_MANAGED(i) {
   { 
      int __lasti = collect_in_progress ? man_k : man_last; 
      for (int i = 0; i <= __lasti; i++) {
	 {

	    if (LIVE(managed[i])) {
	       mt_t t  = INFO_T(managed[i]);
	       if (types[t].mark)
		  types[t].mark(CLRPTR(managed[i]));
	    }
	 } 
	 

      }
   }
   // DO_MANAGED_END;
}

void _cmm_check_managed(C99_CONST void *p)
{
   if (!cmm_ismanaged(p)) {
      const char *name = (marking_type == mt_undefined) ?
         "undefined" : types[marking_type].name;
      warn("attempt to mark non-managed address\n");
      if (marking_object)
         warn(" 0x%lx (%s) -> 0x%lx\n", 
              PPTR(marking_object), name, PPTR(p));
      else
         warn(" 0x%lx\n", PPTR(p));
      abort();
   }
}

/* trace starting from objects currently in the stack */
STATICFUNC void trace_from_stack(void)
{

process_stack:
   while (!empty()) {
      C99_CONST void *p = marking_object = pop();
      mt_t t = marking_type = cmm_typeof(p);
      if (types[t].mark)
         types[t].mark(p);
   }
   if (stack_overflowed) {
      recover_stack();
      goto process_stack;
   }
}

/* run finalizer, return true if q may be reclaimed */
STATICFUNC bool run_finalizer(finalize_func_t *f, void *q)
{
   DISABLE_GC;

   errno = 0;
   bool ok = f(q);
   if (errno) {
      char *errmsg = strerror(errno);
      warn("finalizer of object 0x%lx (%s) caused an error:\n%s\n",
           PPTR(q), types[cmm_typeof(q)].name, errmsg);
      /* harsh, but this might be a buggy finalizer */
      abort();
   }

   ENABLE_GC;
   return ok;
}


STATICFUNC void reclaim_inheap(void *q)
{
   C99_CONST int b = BLOCK(q);
   C99_CONST mt_t t = blockrecs[b].t;
   finalize_func_t *f = types[t].finalize;
   if (f)
      if (!run_finalizer(f, q))
         return;

   { 
      C99_CONST ptrdiff_t a = ((char *)q) - heap;
      assert(HMAP_MANAGED(a));
      if (HMAP_NOTIFY(a)) {
         HMAP_UNMARK_NOTIFY(a);
         client_notify((void *)q);
      }
      HMAP_UNMARK_MANAGED(a);
   }
   VALGRIND_MEMPOOL_FREE(heap, q);
   
   assert(blockrecs[b].in_use > 0);
   blockrecs[b].in_use--;      
   if (blockrecs[b].in_use == 0) {
      blockrecs[b].t = mt_undefined;
      num_free_blocks++;
      VALGRIND_DISCARD((block_t *)BLOCK_ADDR(q));
   }
   if (b < types[t].next_b)
      types[t].next_b = b;
}


STATICFUNC void reclaim_offheap(int i)
{
   assert(!OBSOLETE(managed[i]));

   void *q = CLRPTR(managed[i]);
   if (!BLOB(managed[i])) {
      info_t *info_q = (info_t*)unseal(q); 
      finalize_func_t *f = types[info_q->t].finalize;
      if (f)
         if (!run_finalizer(f, q))
            return;
      q = info_q;
   }

   if (NOTIFY(managed[i])) {
      UNMARK_NOTIFY(managed[i]);
      client_notify(managed[i]);
   }
   
   MARK_OBSOLETE(managed[i]);
   man_is_compact = false;

   free(q);
}

#if 0
STATICFUNC int sweep_now(void)
{
   int n = 0;
   
   /* objects in small object heap */
   DO_HEAP(a, b) {
      if (HMAP_LIVE(a)) {
         HMAP_UNMARK_LIVE(a);
      } else {
         reclaim_inheap(heap + a);
         n++;
      }
   } DO_HEAP_END;

   /* malloc'ed objects */
   DO_MANAGED(i) {
      if (LIVE(managed[i])) {
         UNMARK_LIVE(managed[i]);
      } else {
         reclaim_offheap(i);
         n++;
      }
   } DO_MANAGED_END;

   debug("%d objects reclaimed\n", n);
   return n;
}
#endif


int sweep_now(void)
{
   int n = 0;

//   /* objects in small object heap */
//   DO_HEAP(a, b) {
   { 
      uintptr_t __a = 0; 
      for (int b = 0; b < num_blocks; b++, __a += (1<<12)) {
	 if (blockrecs[b].in_use > 0) { 
	    uintptr_t __a_next = __a + (1<<12); 

	    for (uintptr_t a = __a; a < __a_next; a += (1<<3)) { 
	       if ((hmap[(((uintptr_t)(a))>>(3 + 3))] & ((8) << (((((uintptr_t)(a))>>3) & (8 -1)) * 4)))) {
		  if ((hmap[(((uintptr_t)(a))>>(3 + 3))] & ((1) << (((((uintptr_t)(a))>>3) & (8 -1)) * 4)))) {
		     (hmap[(((uintptr_t)(a))>>(3 + 3))] &=~ ((1) << (((((uintptr_t)(a))>>3) & (8 -1)) * 4)));
		  } else {
		     reclaim_inheap(heap + a);
		     n++;
		  }
	       } 
	    }
	 }
      }
   };

//   } DO_HEAP_END;
//
//   /* malloc'ed objects */
//   DO_MANAGED(i) {

   { 
      int __lasti = collect_in_progress ? man_k : man_last; for (int i = 0; i <= __lasti; i++) {
	 {
	    if ((((uintptr_t)(managed[i])) & 1)) {
	       { managed[i] = (void *)((uintptr_t)(managed[i]) & ~1); };
	    } else {
	       reclaim_offheap(i);
	       n++;
	    }
	 } 
      }
   };

//    } DO_MANAGED_END;
   
   debug("%d objects reclaimed\n", n);
   return n;
}


/*
 * The transient object stack is implemented as a linked
 * list of chunks and is itself managed.
 */

#define STACK_ELTS_PER_CHUNK  (BLOCKSIZE/sizeof(void *) - 1)

typedef struct stack_chunk {
   struct stack_chunk  *prev;
   stack_elem_t        elems[STACK_ELTS_PER_CHUNK];
} stack_chunk_t;

struct stack {
   stack_ptr_t     sp;
   stack_ptr_t     sp_min;
   stack_ptr_t     sp_max;
   stack_chunk_t  *current;
   stack_elem_t    temp;
};

#define ELT_0(c)      ((c)->elems)
#define ELT_N(c)      ((c)->elems + STACK_ELTS_PER_CHUNK)

#define CURRENT_0(st) (ELT_0((st)->current))
#define CURRENT_N(st) (ELT_N((st)->current))

#define STACK_VALID(st)  ((!(st)->sp && !(st)->current) || \
			  (CURRENT_0(st) <= (st)->sp && \
			   (st)->sp <= CURRENT_N(st)))

STATICFUNC void mark_stack(cmmstack_t *st)
{
   assert(STACK_VALID(st));

   if (st->temp) _cmm_push(st->temp);

   if (st->sp > CURRENT_0(st))
      *(st->sp - 1) = NULL;

   if (st->current) _cmm_push(st->current);
}

STATICFUNC void mark_stack_chunk(stack_chunk_t *c)
{
   if (c->prev) _cmm_push(c->prev);
   for (int i = STACK_ELTS_PER_CHUNK-1; i >= 0; i--) {
      if (c->elems[i])
         _cmm_push(c->elems[i]);
      else
         break;
   }
 }

STATICFUNC void add_chunk(cmmstack_t *st)
{
   /* We're not using cmm_alloc, so we don't trigger a collection.
    * Also, stack_chunk allocation is hidden from profiling.
    */
   stack_chunk_t *c = NULL;
   if ((c = (stack_chunk_t*)alloc_variable_sized(mt_stack_chunk, sizeof(stack_chunk_t)))) {
      ptrdiff_t a = ((char *)c) - heap;
      if (a>=0 && a<(long)heapsize)
         HMAP_MARK_MANAGED(a);
      else
         add_managed(c);
   }
   ABORT_WHEN_OOM(c);
   memset(c, 0, sizeof(stack_chunk_t));
   c->prev = st->current;
   st->current = c;
   st->sp_min = ELT_0(c);
   st->sp = st->sp_max = ELT_N(c);
}


void _cmm_pop_chunk(cmmstack_t *st)
{
   if (INHEAP(st->current)) {
      /* reclaim stack chunks immediately */
      int b = BLOCK(st->current);
      HMAP_UNMARK_MANAGED(b*BLOCKSIZE);
      blockrecs[b].t = mt_undefined;
      blockrecs[b].in_use = 0;
      num_free_blocks++;
      types[mt_stack_chunk].next_b = b;
   }
   st->current = st->current->prev;
   if (!st->current) {
      warn("stack underflow\n");
      abort();
   }
   st->sp_min = st->sp = ELT_0(st->current);
   st->sp_max = ELT_N(st->current);
}


static cmmstack_t *make_stack(void)
{
   DISABLE_GC;  // expands to: bool __nogc = gc_disabled; gc_disabled = true;
   
   size_t s = MIN_HUNKSIZE*(1+(sizeof(cmmstack_t)/MIN_HUNKSIZE)); // expands to: (1<<3)*(1+(sizeof(cmmstack_t)/(1<<3)));
   cmmstack_t *st = (cmmstack_t*)alloc_variable_sized(mt_stack, s);
   ABORT_WHEN_OOM(st);         // expands to:  if (!(st)) { warn("allocation failed\n"); abort(); }
   assert(st && !INHEAP(st));  // expands to: (((char *)(st) >= heap) && ((char *)(st) < (heap + heapsize)))
   memset(st, 0, sizeof(cmmstack_t));
   add_managed(st);
   add_chunk(st);

   ENABLE_GC; // expands to: gc_disabled = __nogc;
   return st;
}

STATICFUNC void stack_push(cmmstack_t *st, stack_elem_t e)
{
   assert(e != 0);
   if (st->sp == st->sp_min) {
      st->temp = e;   // save
      add_chunk(st);  // add_chunk() might trigger a collection
      st->temp = 0;   // restore
      assert(st->sp == st->sp_max);
   }
   st->sp--;
   *(st->sp) = e;
}

STATICFUNC bool stack_empty(cmmstack_t *st)
{
   assert(st->current);
   return (st->current->prev==NULL && 
           st->sp==st->sp_max &&
           !st->temp);
}

STATICFUNC int stack_depth(cmmstack_t *st)
{
   int d = st->sp_max - st->sp;
   stack_chunk_t *c = st->current;
   while (c->prev) {
      d = d + STACK_ELTS_PER_CHUNK;
      c = c->prev;
   }
   return d;
}

static size_t stack_sizeof(cmmstack_t *st)
{
   size_t s = sizeof(cmmstack_t);
   stack_chunk_t *c = st->current;
   while (c->prev) {
      s = s + sizeof(stack_chunk_t);
      c = c->prev;
   }
   return s;
}

static stack_elem_t stack_peek(cmmstack_t *st)
{
   assert(STACK_VALID(st));

   if (st->sp == st->sp_max) {
      assert(st->current->prev);
      stack_ptr_t sp = ELT_0(st->current->prev);
      return *sp;
   } else
      return *(st->sp);
}

static stack_elem_t stack_pop(cmmstack_t *st)
{
   assert(STACK_VALID(st));

   if (st->sp == st->sp_max)
      _cmm_pop_chunk(st);

   return *(st->sp++);
}

static inline void stack_reset(cmmstack_t *st, stack_ptr_t sp)
{
   assert(STACK_VALID(st));

   while (st->sp>sp || sp>st->sp_max)
      _cmm_pop_chunk(st);
   st->sp = sp;
}

static stack_elem_t stack_elt(cmmstack_t *st, int i)
{
   assert(STACK_VALID(st));
   
   stack_chunk_t *c = st->current;
   stack_ptr_t sp = st->sp;
   while (sp+i >= ELT_N(c)) {
      i -= ELT_N(c) - sp;
      c = c->prev;
      if (!c) {
         debug("stack underflow\n");
         assert(c);
      }
      sp = ELT_0(c);
   }
   return sp[i];
}

/* push five chunks worth of numbers on stack,
 * index, and pop.
 */

STATICFUNC bool stack_works_fine(cmmstack_t *st)
{
   intptr_t n = 5*STACK_ELTS_PER_CHUNK+2;
   
   assert(sizeof(stack_chunk_t) ==
          (STACK_ELTS_PER_CHUNK+1)*sizeof(stack_elem_t));
   
   for (intptr_t i = 0; i < n ; i++) {
      stack_push(st, (void *)(i+1));
   }

   /* avoid 'defined but not used' warning */
   (void)stack_peek(st);

   /* test indexing */
   for (intptr_t i = 0; i < n; i++) {
      if ((n-i) != (intptr_t)stack_elt(st, i)) {
         warn("unexpected element on stack: %ld (should be %ld)\n",
              (intptr_t)stack_elt(st,i), (n-i));
         return false;
      }
   }

   /* test popping and stack_empty */
   while (!stack_empty(st)) {
      intptr_t i = (intptr_t)stack_pop(st);
      if (n != i) {
         warn("unexpected element on stack: %ld (should be %ld)\n",
                   i, n);
         return false;
      }
      n--;
      if (stack_depth(st)!=n) {
         warn("stack_depth reports wrong depth: %d (should be %ld)\n",
              stack_depth(st), n);
         return false;
      }
   }
   if (n) {
      cmm_printf("inconsistent element count: %" "ld" " (should be 0)\n", n);
      return false;
   }
   return true;
}

/*
 * CMM API
 */

#if 1
static stack_ptr_t _cmm_begin_anchored(void)
{
   return _cmm_transients->sp;
}

STATICFUNC void  _cmm_end_anchored(stack_ptr_t sp)
{
   stack_reset(_cmm_transients, sp);
}
#else
stack_ptr_t _cmm_begin_anchored(void)
{
   return _cmm_transients->sp;
}

void _cmm_end_anchored(stack_ptr_t sp)
{
   stack_reset(_cmm_transients, sp);
}
#endif

void cmm_anchor(C99_CONST void *p)
{
   if (p)
      stack_push(_cmm_transients, p);
}

void cmm_debug(bool e)
{
   cmm_debug_enabled = e;
}


mt_t cmm_regtype(const char *n, size_t s, 
                clear_func_t c, mark_func_t *m, finalize_func_t *f)
{
   if (!heap) {
      warn("library not initialized (call cmm_init first)\n");
      abort();
   }
   if (!n || !n[0]) {
      warn("first argument invalid\n");
      abort();
   }
   if (num_profiles) {
      warn("cannot register while profiling is active\n");
      abort();
   }

   /* check if type is already registered */
   for (int t = 0; t <= types_last; t++) {
      if (0==strcmp(types[t].name, n)) {
         warn("attempt to re-register memory type\n");
         assert(s <= types[t].size);
         assert(c == types[t].clear);
         assert(m == types[t].mark);
         assert(f == types[t].finalize);
         return t;
      }
   }

   /* register new memtype */
   types_last++;
   if (types_last == types_size) {
      debug("enlarging type directory\n");
      types_size *= 2;
      types = (typerec_t*)realloc(types, types_size*sizeof(typerec_t));
      assert(types);
   }
   assert(types_last < types_size);
   assert(types_last < num_blocks);

   typerec_t *rec = &(types[types_last]);
   rec->name = strdup(n);
   VALGRIND_CHECK_MEM_IS_DEFINED(types[types_last].name, strlen(n)+1);
   rec->size = MIN_HUNKSIZE * (s/MIN_HUNKSIZE);
   while (rec->size < s)
      rec->size += MIN_HUNKSIZE;
   rec->clear = c;
   rec->mark = m;
   rec->finalize = f;
   if (rec->size > 0) {
      rec->current_a = 0;
      rec->current_amax = rec->current_a + AMAX(rec->size);
      rec->next_b = types_last;
   }
   return types_last;
}


void *cmm_alloc(mt_t t)
{
   if (t == mt_undefined) {
      warn("attempt to allocate with undefined memory type\n");
      abort();
   }
   assert(TYPE_VALID(t));

   if (types[t].size==0) {
      extern void dump_types(void);
      warn("attempt to allocate variable-sized object (%s)\n\n",
           types[t].name);
      stdlog = stderr;
      dump_types();
      abort();
   }
   
   void *p = alloc_variable_sized(t, types[t].size);
   ABORT_WHEN_OOM(p);

   if (types[t].clear)
      types[t].clear(p, types[t].size);
   manage(p, t);
   return p;
}



void *cmm_malloc(mt_t t, size_t s)
{
   if (t == mt_undefined) {
      warn("attempt to allocate with undefined memory type\n");
      abort();
   }
   assert(TYPE_VALID(t));
   if (s == 0) {
      warn("attempt to allocate object of size zero\n");
      abort();
   }
   
   FIX_SIZE(s);
   void *p = alloc_variable_sized(t, s);
   if (p) {
      if (types[t].clear)
         types[t].clear(p, s);
      manage(p, t);
   }
   return p;
}


void *cmm_allocv(mt_t t, size_t s)
{
   void *p = cmm_malloc(t, s);
   ABORT_WHEN_OOM(p);
   return p;
}


void *cmm_blob(size_t s)
{
   if (s <= 256) {
      mt_t mt = mt_blob256;
      if      (s <=  8)   mt = mt_blob8;
      else if (s <= 16)   mt = mt_blob16;
      else if (s <= 32)   mt = mt_blob32;
      else if (s <= 64)   mt = mt_blob64;
      else if (s <=128)   mt = mt_blob128;
      return cmm_alloc(mt);
   } else
      return cmm_allocv(mt_blob, s);
}


char *cmm_strdup(C99_CONST char *str)
{
   char *str2 = (char*)cmm_blob(strlen(str)+1);
   return strcpy(str2, str);
}


void cmm_manage(C99_CONST void *p)
{
   if (!ADDRESS_VALID(p)) {
      warn(" 0x%lx is not a valid address\n", PPTR(p));
      abort();
   }
   if (cmm_debug_enabled)
      if (cmm_ismanaged(p)) {
         warn(" address 0x%lx already managed\n", PPTR(p));
         abort();
      }
   add_managed(p);
   MARK_BLOB(managed[man_last]);
   cmm_anchor(p);
}


void cmm_notify(C99_CONST void *p, bool set)
{
   assert(ADDRESS_VALID(p));

   if (!client_notify && set) {
      warn("no notification function specified\n");
      abort();
   }
   
   ptrdiff_t a = ((char *)p) - heap;
   if (a>=0 && a<(long)heapsize) {
         
      if (!HMAP_MANAGED(a)) {
         warn("not a managed address\n");
         abort();
      }
      if (set)
         HMAP_MARK_NOTIFY(a);
      else
         HMAP_UNMARK_NOTIFY(a);

      return;
   }

   int i = managed[man_last]==p ? man_last : find_managed(p);
   if (i<0) {
      warn("not a managed address\n");
      abort();
   }
   if (set) {
      MARK_NOTIFY(managed[i]);
   } else {
      UNMARK_NOTIFY(managed[i]);
   }
}


bool cmm_ismanaged(C99_CONST void *p)
{
   if (!ADDRESS_VALID(p)) {
      return false;
      
   } else {
      ptrdiff_t a = ((char *)p) - heap;
      if (a>=0 && a<(long)heapsize)
         return HMAP_MANAGED(a);
      else if (mark_in_progress)
         return _find_managed(p) != -1;
      else {
         if (!collect_in_progress)
            update_man_k();
         return find_managed(p) != -1;
      }
   }
}


mt_t cmm_typeof(C99_CONST void *p)
{
   assert(ADDRESS_VALID(p));
   if (INHEAP(p))
      return blockrecs[BLOCK(p)].t;
   else {
      int i = _find_managed(p);
      assert(i>-1);
      if (BLOB(managed[i]))
         return mt_blob;
      else
         return INFO_T(managed[i]);
   }
}


static size_t cmm_sizeof(C99_CONST void *p)
{
   assert(ADDRESS_VALID(p));
   if (INHEAP(p))
      return types[blockrecs[BLOCK(p)].t].size;
   else {
      int i = _find_managed(p);
      assert(i>-1);
      if (BLOB(managed[i]))
         return 0;
      else
         return INFO_S(managed[i]);
   }
}


void cmm_root(const void *_pr)
{
   void **pr = (void **)_pr;

   /* jea: would judy arrays minimize cache-fills and be faster here than linear search? */

   /* avoid creating duplicates */
   for (int i = roots_last; i >= 0 ; i--) {
      if (roots[i] == pr) {
         debug("attempt to add existing root (ignored)\n");
         return;
      }
   }
   if ((*pr) && !cmm_ismanaged(*pr)) {
      warn("root does not contain a managed address\n");
      warn("*(0x%" "lx" ") = 0x%" "lx" "\n", PPTR(pr), PPTR(*pr));
      abort();
   }

   roots_last++;
   if (roots_last == roots_size) {
      roots_size *= 2; 
      debug("enlarging root table to %d\n", roots_size);
      roots = (void***)realloc(roots, roots_size*sizeof(void *));
      assert(roots);
   }

   assert(roots_last < roots_size);
   roots[roots_last] = pr;
}


void cmm_unroot(const void *_pr)
{
   void **pr = (void **)_pr;

   assert(roots_last>=0);
   if (roots[roots_last] == pr) {
      roots_last--;
      return;
   }

   bool r_found = false;
   for (int i = 0; i < roots_last; i++) {
      if (roots[i] == pr)
         r_found = true;
      if (r_found)
         roots[i] = roots[i+1];
   }
   
   if (r_found)
      roots_last--;
   else
      warn("attempt to unroot non-existing root\n");
}

#define WITH_TEMP_STORAGE   { \
   C99_CONST void *marking_stack[stack_size]; \
   stack = marking_stack;

#define TEMP_STORAGE  \
   stack = NULL; }

STATICFUNC void mark(void)
{
   mark_in_progress = true;

   /* Trace live objects from root objects */
   for (int r = 0; r <= roots_last; r++) {
      if (*roots[r]) {
         if (!cmm_ismanaged(*roots[r])) {
            warn("root at 0x%" "lx" " is not a managed address\n",
                 PPTR(roots[r]));
            abort();
         }
         if (*roots[r]) __cmm_push(*roots[r]);
      }
   }
   trace_from_stack();

#if 0
   /* Mark dependencies of finalization-enabled objects */
   DO_HEAP (a, b) {
      finalize_func_t *finalize = types[blockrecs[b].t].finalize;
      mark_func_t *mark = types[blockrecs[b].t].mark;
      if (!HMAP_LIVE(a) && finalize) {
         void *p = heap + a;
         if (mark) mark(p);
         trace_from_stack();
         HMAP_UNMARK_LIVE(a);  /* break cycles */
      }
   } DO_HEAP_END;
   
   DO_MANAGED (i) {
      mt_t t = INFO_T(managed[i]);
      finalize_func_t *finalize = types[t].finalize;
      mark_func_t *mark = types[t].mark;
      if (!LIVE(managed[i]) && finalize) {
         if (mark) mark(CLRPTR(managed[i]));
         trace_from_stack();
         UNMARK_LIVE(managed[i]); /* break cycles */
      }
   } DO_MANAGED_END;
#endif


   {                                                               // first line of DO_HEAP(a,b)
      uintptr_t __a = 0;                                           // 
      for (int b = 0; b < num_blocks; b++, __a += (1<<12)) {       //
	 if (blockrecs[b].in_use > 0) {                            //
	    uintptr_t __a_next = __a + (1<<12);                    //
	    for (uintptr_t a = __a; a < __a_next; a += (1<<3)) {   //
	       if ((hmap[(((uintptr_t)(a))>>(3 + 3))] & ((8) << (((((uintptr_t)(a))>>3) & (8 -1)) * 4)))) {   // last line of DO_HEAP(a,b)
		  finalize_func_t *finalize = types[blockrecs[b].t].finalize;
		  mark_func_t *mark = types[blockrecs[b].t].mark;
		  if (!(hmap[(((uintptr_t)(a))>>(3 + 3))] & ((1) << (((((uintptr_t)(a))>>3) & (8 -1)) * 4))) && finalize) { // if (!HMAP_LIVE(a) && finalize)
		     void *p = heap + a;
		     if (mark) mark(p);
		     trace_from_stack();
		     (hmap[(((uintptr_t)(a))>>(3 + 3))] &=~ ((1) << (((((uintptr_t)(a))>>3) & (8 -1)) * 4))); // HMAP_UNMARK_LIVE(a);  /* break cycles */
		  }
	       } 
	    }
	 }
      }
   };
   
   { 
      int __lasti = collect_in_progress ? man_k : man_last;   // DO_MANAGED(i)
      for (int i = 0; i <= __lasti; i++) { {                  // DO_MANAGED(i)
	    mt_t t = ((((uintptr_t)(managed[i])) & 4) ? mt_blob : ((info_t *)(unseal((char *)managed[i])))->t); // mt_t t = INFO_T(managed[i]);
	    finalize_func_t *finalize = types[t].finalize;
	    mark_func_t *mark = types[t].mark;
	    if (!(((uintptr_t)(managed[i])) & 1) && finalize) {  // if (!LIVE(managed[i]) && finalize)
	       if (mark) mark(((void *)((((uintptr_t)(managed[i])) & ~((1<<3)-1)))));

	       trace_from_stack();
	       { 
		  managed[i] = (void *)((uintptr_t)(managed[i]) & ~1); // UNMARK_LIVE(managed[i]); /* break cycles */
	       };
	    }
	 } 
      }
   };


   assert(empty());
   mark_in_progress = false;
}


int cmm_collect_now(void)
{
   d();

   if (gc_disabled) {
      collect_requested = true;
      return 0;
   } 
   assert(!collect_in_progress);

   collect_prologue();
   d();
   
   int n = 0;
   { 
      // macro expansion of WITH_TEMP_STORAGE
      void *marking_stack[stack_size]; 
      stack = marking_stack;
      {
	 mark();
	 d();
	 n = sweep_now();
	 d();
      } 
      stack = __null; 
   };

   collect_epilogue();
   d();
   return n;
}


bool cmm_collect_in_progress(void)
{
   return collect_in_progress;
}


bool cmm_idle(void)
{
   static int ncalls = 0;

   if (collect_in_progress) {
      if (!gc_disabled) {
         int i = 100 + fetch_backlog;
         while (i>0 && collect_in_progress) {
            fetch_unreachables();
            i--;
         }
         return true;
      } else
         return false;

   } else {
      ncalls++;
      if (!collect_in_progress)
         update_man_k();
      if (ncalls<NUM_IDLE_CALLS) {
         return false;
      }
      /* create some work for ourselves */
      cmm_collect_now();
      ncalls = 0;
      return true;
   }
}


bool cmm_begin_nogc(bool dont_block)
{
   /* block when a collect is in progress */
   if (!dont_block) {
      assert(!collect_in_progress);
   }

   bool nogc = gc_disabled;
   gc_disabled = true;
   return nogc;
}


void cmm_end_nogc(bool nogc)
{
   gc_disabled = nogc;
   if (collect_requested && !gc_disabled) {
      cmm_collect();
      if (!collect_in_progress)
         /* could not spawn child, do it synchronously */
         cmm_collect_now();
   }
}

STATICFUNC void mark_refs(void **p)
{
   int n = cmm_sizeof(p)/sizeof(void *);
   for (int i = 0; i < n; i++)
      if (p[i]) _cmm_push(p[i]);
}

STATICFUNC void clear_refs(void **p, size_t s)
{
   memset(p, 0, s);
}


void cmm_init(int npages, notify_func_t *clnotify, FILE *log)
{
   assert(sizeof(info_t) <= MIN_HUNKSIZE);
   assert(sizeof(hunk_t) <= MIN_HUNKSIZE);
   assert((1<<HMAP_EPI_BITS) == HMAP_EPI);
   assert(sizeof(stack_chunk_t) == BLOCKSIZE);

   /* initial heap can be no bigger than 1GB */
   double bytes_requested = (double)npages * (double)PAGESIZE;
   if(!(bytes_requested <= (double)(1 << 30))) {
      fprintf(stderr,"cmm_init(npages=%d) is too big at %.6f GB: not allowed "
	      "to request more than 1GB heap. "
	      "(PAGESIZE=%d *  npages=%d == total requested bytes in decimal: %.0f\n", 
	      npages, 
	      (bytes_requested / (double)(1<<30)),
	      PAGESIZE, 
	      npages, 
	      bytes_requested );
      assert((double)npages * (double)PAGESIZE <= (double)(1 << 30));
   }

   client_notify = clnotify;
   
   if (log) {
      cmm_debug_enabled = true;
      stdlog = log;
   } else {
      cmm_debug_enabled = false;
      stdlog = stderr;
   }

   if (heap) {
      warn("cmm is already initialized\n");
      return;
   }

   /* allocate small-object heap */
   num_blocks = max((PAGESIZE*npages)/BLOCKSIZE, MIN_NUMBLOCKS);

   /* jea add */
   assert(num_blocks);

   num_free_blocks = num_blocks;
   heapsize = num_blocks * BLOCKSIZE;
   assert(heapsize); /* overflow an int? yep, if we request in bytes instead of npages, of size 4096 */

   hmapsize = (heapsize/MIN_HUNKSIZE)/HMAP_EPI;
   assert(hmapsize); /* overflow */

   block_threshold = min((long)MAX_BLOCKS, (long)(num_blocks/3));
   volume_threshold = min(MAX_VOLUME, heapsize/2);

   blockrecs = (blockrec_t *)malloc(num_blocks * sizeof(blockrec_t));
   assert(blockrecs);
   heap = (char *) malloc(heapsize + PAGESIZE);
   VALGRIND_MAKE_MEM_NOACCESS(heap, heapsize);
   if (!heap) {
      warn("could not allocate heap\n");
      abort();
   }

   /* make sure heap is PAGESIZE-aligned */
   heap = (char *)(((uintptr_t)(heap + PAGESIZE-1)) & ~(PAGESIZE-1));
   VALGRIND_CREATE_MEMPOOL(heap, 0, 0);

  /* jea add debug
   printf("* cmm_init() debugging 2 : \n"
	  "  heap: %p, \n"
	  "  heap + PAGESIZE-1: %p \n"
	  "  PAGESIZE-1: %d \n"
	  "  (uintptr_t)(heap + PAGESIZE-1): %ld  \n"
	  "  (((uintptr_t)(heap + PAGESIZE-1)) & ~(PAGESIZE-1)): %ld \n",
	  heap,
	  heap + PAGESIZE-1,
	  PAGESIZE-1,
	  (uintptr_t)(heap + PAGESIZE-1),
	  (((uintptr_t)(heap + PAGESIZE-1)) & ~(PAGESIZE-1))
      );
  */

   hmap = (unsigned int*)calloc(hmapsize, sizeof(unsigned int));
   if (!hmap) {
      warn("could not allocate heap map\n");
      abort();
   }

   assert(heapsize);
   assert(hmapsize);

   debug("heapsize  : %6""ld"" KByte (%d %d KByte blocks)\n",
         (heapsize/(1<<10)), num_blocks, BLOCKSIZE/(1<<10));
   debug("hmapsize  : %6""ld"" KByte\n", (hmapsize*sizeof(int))/(1<<10));
   debug("threshold : %6""ld"" KByte\n", volume_threshold/(1<<10));

   /* initialize block records */
   for (int i = 0; i < num_blocks; i++) {
      blockrecs[i].t = mt_undefined;
      blockrecs[i].in_use = 0;
   }
   assert(no_marked_live());

   /* set up type directory */
   types = (typerec_t *) malloc(MIN_TYPES * sizeof(typerec_t));
   assert(types);
   types_size = MIN_TYPES;
   {
      /* register internal and pre-defined types */
      mt_t mt;
      mt_stack = CMM_REGTYPE("cmm_stack", sizeof(cmmstack_t), 0, mark_stack, 0);
      assert(mt_stack == 0);
      mt_stack_chunk = CMM_REGTYPE("cmm_stack_chunk", sizeof(stack_chunk_t),
                                  clear_refs, mark_stack_chunk, 0);
      assert(mt_stack_chunk == 1);
      mt = CMM_REGTYPE("blob8", 8, 0, 0, 0);
      assert(mt == mt_blob8);
      mt = CMM_REGTYPE("blob16", 16, 0, 0, 0);
      assert(mt == mt_blob16);
      mt = CMM_REGTYPE("blob32", 32, 0, 0, 0);
      assert(mt == mt_blob32);
      mt = CMM_REGTYPE("blob64", 64, 0, 0, 0);
      assert(mt == mt_blob64);
      mt = CMM_REGTYPE("blob128", 128, 0, 0, 0);
      assert(mt == mt_blob128);
      mt = CMM_REGTYPE("blob256", 256, 0, 0, 0);
      assert(mt == mt_blob256);
      mt = CMM_REGTYPE("blob", 0, 0, 0, 0);
      assert(mt == mt_blob);
      mt = CMM_REGTYPE("refs", 0, clear_refs, mark_refs, 0);
      assert(mt == mt_refs);
   }
   assert(types_last == mt_refs);

   /* set up other bookkeeping structures */
   managed = (void **)malloc(MIN_MANAGED * sizeof(void *));
   assert(managed);
   man_size = MIN_MANAGED;

   roots = (void***)malloc(MIN_ROOTS * sizeof(void *));
   assert(roots);
   roots_size = MIN_ROOTS;

   /* set up transient object stack */
   *(cmmstack_t **)&_cmm_transients = make_stack();
   CMM_ROOT(_cmm_transients); // macro expands to: cmm_root(&_cmm_transients);
   assert(stack_works_fine(_cmm_transients));
   assert(stack_empty(_cmm_transients));

   debug("done\n");
}

/* print diagnostic info to string */
char *cmm_info(int level)
{
#define PRINTBUFLEN 20000
#define BPRINTF(...) {               \
   sprintf(buf, __VA_ARGS__);        \
   buf += strlen(buf);               \
   assert((buf-buffer)<PRINTBUFLEN);\
   }

   char buffer[PRINTBUFLEN], *buf = buffer;
   
   if (level<=0)
      return NULL;

   int    total_blocks_in_use = 0;
   size_t total_memory_managed = 0;
   size_t total_memory_used_by_cmm = 0;
   size_t total_objects_inheap = 0;
   size_t total_objects_offheap = 0;
   size_t total_objects_per_type_ih[types_size];
   size_t total_objects_per_type_oh[types_size];
   memset(&total_objects_per_type_ih, 0, types_size*sizeof(size_t));
   memset(&total_objects_per_type_oh, 0, types_size*sizeof(size_t));

   for (int i=0; i<num_blocks; i++)
      if (blockrecs[i].in_use)
         total_blocks_in_use++;
   
   BPRINTF("Small object heap: %.2f MByte in %d blocks (%d used)\n",
           ((double)heapsize)/(1<<20), num_blocks, total_blocks_in_use);

   DO_HEAP(a, b) {
      total_objects_inheap++;
      mt_t t = blockrecs[b].t;
      total_objects_per_type_ih[t]++;
      total_memory_managed += types[t].size;
   } DO_HEAP_END;

   if (!collect_in_progress)
      update_man_k();

   DO_MANAGED(i) {
      total_objects_offheap++;
      mt_t t  = INFO_T(managed[i]);
      total_objects_per_type_oh[t]++;
      total_memory_managed += cmm_sizeof(CLRPTR(managed[i]));
      total_memory_used_by_cmm += BLOB(managed[i]) ? 0 : MIN_HUNKSIZE;
   } DO_MANAGED_END;

   BPRINTF("Managed memory   : %.2f MByte in %""ld"" + %""ld"" objects\n",
           ((double)total_memory_managed)/(1<<20),
           total_objects_inheap, total_objects_offheap);

   total_memory_used_by_cmm += hmapsize;
   total_memory_used_by_cmm += man_size*sizeof(managed[0]);
   total_memory_used_by_cmm += types_size*sizeof(typerec_t);
   total_memory_used_by_cmm += num_blocks*sizeof(blockrec_t);
   total_memory_used_by_cmm += stack_sizeof(_cmm_transients);
   BPRINTF("Memory used by CMM: %.2f MByte total\n",
           ((double)total_memory_used_by_cmm)/(1<<20));
   if (level>2) {
      BPRINTF("                 : %.2f MByte for inheap array\n",
              ((double)hmapsize)/(1<<20));
      BPRINTF("                 : %.2f MByte for offheap array\n",
              ((double)man_size*sizeof(managed[0]))/(1<<20));
   }
   if (gc_disabled)
      BPRINTF("!!! Garbage collection is disabled !!!\n");

   if (level<=1)
      return cmm_strdup(buffer);

   BPRINTF("Page size        : %d bytes\n", PAGESIZE);
   BPRINTF("Block size       : %d bytes\n", BLOCKSIZE);
   BPRINTF("GC threshold     : %d blocks / %.2f MByte\n", 
           block_threshold, (double)volume_threshold/(1<<20));
   if (cmm_debug_enabled) {
      BPRINTF("Debug code       : enabled\n");
   } else {
      BPRINTF("Debug code       : disabled\n");
   }

   if (collect_in_progress)
      BPRINTF("*** GC in progress ***\n");

   int active_roots = 0;
   for (int i = 0; i<=roots_last; i++)
      if (*roots[i])
         active_roots++;

   BPRINTF("Memory roots     : %d total, %d active\n", roots_last+1, active_roots);
   BPRINTF("Transient stack  : %d objects\n", stack_depth(_cmm_transients));
   if (level<=2)
      return cmm_strdup(buffer);

   BPRINTF("\n");
   BPRINTF(" Memory type    | size  | # inheap (blocks) | # malloced \n");
   BPRINTF("---------------------------------------------------------\n");
   
   for (int t = 0; t <= types_last; t++) {
      int n = 0;
      for (int b = 0; b < num_blocks; b++)
         if (blockrecs[b].t == t)
            n++;
      BPRINTF(" %14s | %5""ld"" | %7""ld""  (%6d) |    %7""ld"" \n",
              types[t].name,
              types[t].size,
              total_objects_per_type_ih[t], n,
              total_objects_per_type_oh[t]);
   }
   return cmm_strdup(buffer);
}

/* initialize profile array */
int cmm_prof_start(int *h)
{
   int n = types_last+1;

   if (!h)
      return n;

   if (!profile) {
      profile = (int*)malloc(n*sizeof(int));
      ABORT_WHEN_OOM(profile);
      memset(profile, 0, n*sizeof(int));
   }

   /* initialize h */
   memcpy(h, profile, n*sizeof(int));
   num_profiles++;
   return num_profiles;
}

/* update profile array */
void cmm_prof_stop(int *h)
{
   if (!num_profiles) {
      warn("invalid call to cmm_prof_stop ignored\n");
      return;
   }

   for (int i=0; i<=types_last; i++)
      h[i] = profile[i]-h[i];

   /* stop profiling if this closes the outermost session */
   if (--num_profiles==0) {
      free(profile);
      profile = NULL;
   }
}

/* create key for profile data */
char **cmm_prof_key(void)
{
   char **k = (char**)cmm_allocv(mt_refs, sizeof(void *)*(types_last+1));
   {
      C99_CONST void **sp = _cmm_begin_anchored();
      for (int i=0; i<=types_last; i++)
         k[i] = cmm_strdup(types[i].name);
      _cmm_end_anchored(sp);
   }
   return k;
}


/*
 * Debugging aides
 */

/* dump all of type t or all if t = mt_undefined */
void dump_managed(mt_t t)
{
   cmm_printf("Dumping managed list (%d of %d in poplars)...\n",
             man_k+1, man_last+1);
   for (int i = 0; i <= man_last; i++) {
      mt_t ti = cmm_typeof(CLRPTR(managed[i]));
      if (t==mt_undefined || t==ti)
         cmm_printf("%4d : %16lx, %1s%1s %20s\n", 
                   i, PPTR(CLRPTR(managed[i])),
                   OBSOLETE(managed[i]) ? "o" : " ",
                   NOTIFY(managed[i]) ? "n" : " ",
                   types[ti].name);
   }
   cmm_printf("\n");
   fflush(stdlog);
}

void dump_types(void)
{
   cmm_printf("Dumping type registry (%d types)...\n", types_last+1);
   for (int t = 0; t <= types_last; t++) {
      int n = 0;
      for (int b = 0; b < num_blocks; b++)
         if (blockrecs[b].t == t)
            n++;
      cmm_printf("%3d: %15s  %4""ld"" 0x%lx 0x%lx  0x%lx (%d in freelist)\n",
                t,
                types[t].name,
                types[t].size,
                PPTR(types[t].clear),
                PPTR(types[t].mark),
                PPTR(types[t].finalize),
                n);
   }

   cmm_printf("\n");
   fflush(stdlog);
}

void dump_roots(void)
{
   cmm_printf("Dumping roots...\n");
   for (int r = 0; r <= roots_last; r++) {
      cmm_printf(" loc 0x%lx --> 0x%lx\n", 
		 PPTR(roots[r]), PPTR(*(roots[r])));
   }
   cmm_printf("\n");
   fflush(stdlog);
}

void dump_stack(cmmstack_t *st)
{
   cmm_printf("Dumping %s stack...\n",
             st == _cmm_transients ? "transient" : "finalizer");
   if (st->temp)
      cmm_printf("temp = 0x%lx\n\n", PPTR(st->temp));

   int n = stack_depth(st);
   for (int i = 0; i < n; i++)
      cmm_printf("%3d : 0x%lx\n", i, PPTR(stack_elt(st,i)));
   
   cmm_printf("\n");
   fflush(stdlog);
}

void dump_stack_depth(void)
{
   cmm_printf("depth of transients stack: %d\n", stack_depth(_cmm_transients));
   cmm_printf("\n");
   fflush(stdlog);
}

void dump_heap_stats(void)
{
   int counts[types_size];
   memset(counts, 0, sizeof(counts));
   int num_ih = 0;

   for (int i = 0; i<=man_last; i++) {
      counts[cmm_typeof(CLRPTR(managed[i]))]++;
      if (INHEAP(managed[i])) num_ih++;
   }

   cmm_printf("\n");
   for (int t = 0; t <= types_last; t++)
      cmm_printf(" [%3d] %15s : %8d\n",
                t, types[t].name, counts[t]);
   cmm_printf(" total : %d, in heap %d\n\n", man_last+1, num_ih);
}


void dump(const char* where, int line, void*  cmmstack_t_ptr ) {

   cmmstack_t* st = (cmmstack_t*)cmmstack_t_ptr;

   printf("\n&&&&&&&&&&&&&&&&&&&&&&&&&=========================\n");
   printf("&&&&&& BEGIN     %s   : line %d\n",where,line);
   printf("&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&\n");
 


   printf("\n=========================\n");
   printf("dump_types:\n");
   dump_types();


   printf("\n=========================\n");
   printf("dump_stats:\n");
   dump_heap_stats();

// can't do this here, because mem_info can
   // spawn a collection, -> infinite loop.
//   printf("\n=========================\n");
//   printf("mem_info(3) %s:\n",cmm_info(3));

   printf("\n=========================\n");
   printf("dump_stack(st):");
   if (st) {
      printf("\n");
      dump_stack(st);
   } else {
      printf("  -- st was null, not calling dump_stack(st) -- \n");
   }

   printf("\n=========================\n");
   printf("dump_stack_depth:\n");
   dump_stack_depth();

   printf("\n=========================\n");
   printf("dump_roots:\n");
   dump_roots();
   
   printf("\n=========================\n");
   printf("dump_managed:\n");
   dump_managed(mt_undefined);

   printf("\n&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&\n");
   printf("&&&&&& END     %s   : line %d\n",where,line);
   printf("&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&\n");

}


#ifdef __cplusplus
}
#endif 

/* -------------------------------------------------------------
   Local Variables:
   c-file-style: "k&r"
   c-basic-offset: 3
   End:
   ------------------------------------------------------------- */
