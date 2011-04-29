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

#ifndef CMM_INCLUDED
#define CMM_INCLUDED


#include <stdbool.h>
#include <stdio.h>
#include <stdint.h> /* defines UINT32_MAX as (4294967295U) */


#ifdef __cplusplus
extern "C" {

/* jea C99 <-> C++ portability stuff */

/* C++ */
#define UINT32_MAX (4294967295U)
#define STATICFUNC
#define RESTRICTC99
#define C99_CONST
#define C99__FUNC__  __FUNCTION__

#else

/* C99 */
#define C99__FUNC__    __func__ 
#define STATICFUNC     static 
#define RESTRICTC99    restrict 
#define C99_CONST      const 

#endif 


#define NVALGRIND
#define CMM_SIZE_MAX    (UINT32_MAX * MIN_HUNKSIZE)

/* 
   CMM_SNAPSHOT_GC turns on the feature that
   forks a child process to do garbage collection 
   concurrently in the background. If snapshots are
   used, the child process communicates
   the mark-and-sweep results back to
   the parent process using a pipe.

   Without this, GC is done synchronously
   in this same process, when cmm_collect_now()
   or cmm_idle() is called.
*/

// #define CMM_SNAPSHOT_GC

typedef void clear_func_t(void *, size_t);
typedef void mark_func_t(C99_CONST void *);
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
void    cmm_init(int, notify_func_t *, FILE *); // initialize manager
void    cmm_debug(bool);                  // enable/disable debug code
mt_t    cmm_regtype(const char *, size_t, clear_func_t, mark_func_t *, finalize_func_t *);
void    cmm_root(const void *);           // add a root location
void    cmm_unroot(const void *);         // remove a root location
bool    cmm_idle(void);                   // do work, return true when more work

/* Garbage collection */
int     cmm_collect_now(void);            // trigger garbage collection
bool    cmm_collect_in_progress(void);    // true if gc is under way

/* Allocation functions */
void   *cmm_alloc(mt_t);                  // allocate fixed-size object
void   *cmm_allocv(mt_t, size_t);         // allocate variable-sized object
void   *cmm_malloc(mt_t, size_t);         // allocate variable-sized object
void   *cmm_blob(size_t);                 // allocate blob of size
char   *cmm_strdup(C99_CONST char *);         // create managed copy of string

/* Properties of managed objects */
bool    cmm_ismanaged(C99_CONST void *);      // true if managed object
void    cmm_manage(C99_CONST void *);         // manage malloc'ed address 
void    cmm_notify(C99_CONST void *, bool);   // set or unset notify flag
mt_t    cmm_typeof(C99_CONST void *);         // type of object

/* Diagnostics */
char   *cmm_info(int);                    // diagnostic message in managed string
int     cmm_prof_start(int *);            // start profiling, initialize histogram
void    cmm_prof_stop(int *);             // stop profiling and write data
char  **cmm_prof_key(void);               // make key for profile data

/* MACROS */
#define CMM_MARK(p)              { if (p) _cmm_mark(p); }
#define CMM_REGTYPE(n,s,c,m,f)   cmm_regtype(n, s, (clear_func_t *)c, (mark_func_t *)m, (finalize_func_t *)f)
#define CMM_ROOT(p)              cmm_root(&p)
#define CMM_UNROOT(p)            cmm_unroot(&p)

#define CMM_ENTER                C99_CONST void **__cmm_stack_pointer = _cmm_begin_anchored()
#define CMM_EXIT                 _cmm_end_anchored(__cmm_stack_pointer)
#define CMM_ANCHOR(p)            { if (p) _cmm_anchor(p); }
#define CMM_RETURN(p)            { void* pp = p; CMM_EXIT; CMM_ANCHOR(pp); return pp; }
#define CMM_RETURN_TYPE(p,Type)  { Type pp = p; CMM_EXIT; CMM_ANCHOR(pp); return pp; }
#define CMM_RETURN_VOID          CMM_EXIT; return

#define CMM_NOGC                 bool __cmm_nogc = cmm_begin_nogc(false)
#define CMM_NOGC_END             cmm_end_nogc(__cmm_nogc)
#define CMM_PAUSEGC              bool __cmm_pausegc = cmm_begin_nogc(true)
#define CMM_PAUSEGC_END          cmm_end_nogc(__cmm_pausegc)

void    cmm_anchor(C99_CONST void *);
bool    cmm_begin_nogc(bool);
void    cmm_end_nogc(bool);


 /* dump() calling d and ds(cmmstack_t) debug macros */
void dump(const char* where, int line, void* cmmstack_t_ptr=0);
#ifndef NDEBUG
#define d()   dump(__FUNCTION__,__LINE__,0)
#define ds(st) dump(__FUNCTION__,__LINE__,st)
#else
#define d() 
#define ds(st)
#endif


#ifndef CMM_INTERNAL
#include "cmm_private.h"
#endif


#ifdef __cplusplus
}
#endif 


#endif /* CMM_INCLUDED */


/* -------------------------------------------------------------
   Local Variables:
   c-file-style: "k&r"
   c-basic-offset: 3
   End:
   ------------------------------------------------------------- */
