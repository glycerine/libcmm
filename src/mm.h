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

/* 
 * There are two kinds of projects that use LIBCMM: Those
 * that need to call mm_init, and those that don't.
 * The bootstrap or initialization code of applications must
 * call mm_init before calling any other LIBCMM functions.
 * Initialization code of a "LIBCMM-ready library" must not
 * call mm_init but leave this to the library client.
 */

#ifndef MM_INCLUDED
#define MM_INCLUDED

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#define NVALGRIND
#define MM_SIZE_MAX    (UINT32_MAX*MIN_HUNKSIZE)
// #define MM_SNAPSHOT_GC

typedef void clear_func_t(void *, size_t);
typedef void mark_func_t(const void *);
typedef bool finalize_func_t(void *);
typedef void notify_func_t(void *);

typedef short mt_t;

/* pre-defined memory types */
enum mt {
   mt_undefined = -1,
   /* 0 & 1 for internal use */
   mt_blob8     =  2,
   mt_blob16    =  3,
   mt_blob32    =  4,
   mt_blob64    =  5,
   mt_blob128   =  6,
   mt_blob256   =  7,
   mt_blob      =  8,
   mt_refs      =  9,
};

/* Administration */
void    mm_init(int, notify_func_t *, FILE *); // initialize manager
void    mm_debug(bool);                  // enable/disable debug code
mt_t    mm_regtype(const char *, size_t, clear_func_t, mark_func_t *, finalize_func_t *);
void    mm_root(const void *);           // add a root location
void    mm_unroot(const void *);         // remove a root location
bool    mm_idle(void);                   // do work, return true when more work

/* Garbage collection */
int     mm_collect_now(void);            // trigger garbage collection
bool    mm_collect_in_progress(void);    // true if gc is under way

/* Allocation functions */
void   *mm_alloc(mt_t);                  // allocate fixed-size object
void   *mm_allocv(mt_t, size_t);         // allocate variable-sized object
void   *mm_malloc(mt_t, size_t);         // allocate variable-sized object
void   *mm_blob(size_t);                 // allocate blob of size
char   *mm_strdup(const char *);         // create managed copy of string

/* Properties of managed objects */
bool    mm_ismanaged(const void *);      // true if managed object
void    mm_manage(const void *);         // manage malloc'ed address 
void    mm_notify(const void *, bool);   // set or unset notify flag
mt_t    mm_typeof(const void *);         // type of object

/* Diagnostics */
char   *mm_info(int);                    // diagnostic message in managed string
int     mm_prof_start(int *);            // start profiling, initialize histogram
void    mm_prof_stop(int *);             // stop profiling and write data
char  **mm_prof_key(void);               // make key for profile data

/* MACROS */
#define MM_MARK(p)              { if (p) _mm_mark(p); }
#define MM_REGTYPE(n,s,c,m,f)   mm_regtype(n, s, (clear_func_t *)c, (mark_func_t *)m, (finalize_func_t *)f)
#define MM_ROOT(p)              mm_root(&p)
#define MM_UNROOT(p)            mm_unroot(&p)

#define MM_ENTER                const void **__mm_stack_pointer = _mm_begin_anchored()
#define MM_EXIT                 _mm_end_anchored(__mm_stack_pointer)
#define MM_ANCHOR(p)            { if (p) _mm_anchor(p); }
#define MM_RETURN(p)            { void *pp = p; MM_EXIT; MM_ANCHOR(pp); return pp; }
#define MM_RETURN_VOID          MM_EXIT; return

#define MM_NOGC                 bool __mm_nogc = mm_begin_nogc(false)
#define MM_NOGC_END             mm_end_nogc(__mm_nogc)
#define MM_PAUSEGC              bool __mm_pausegc = mm_begin_nogc(true)
#define MM_PAUSEGC_END          mm_end_nogc(__mm_pausegc)

void    mm_anchor(const void *);
bool    mm_begin_nogc(bool);
void    mm_end_nogc(bool);

#ifndef MM_INTERNAL
#include "mm_private.h"
#endif

#endif /* MM_INCLUDED */


/* -------------------------------------------------------------
   Local Variables:
   c-file-style: "k&r"
   c-basic-offset: 3
   End:
   ------------------------------------------------------------- */
