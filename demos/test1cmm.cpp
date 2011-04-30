/*

  test1.cpp: figure out how to use libcmm garbage collection.

 */

//#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include "cmm.h"

#include <vector>
using std::vector;


typedef struct tree_node Tree;
struct tree_node {
    Tree * left, * right;
    void * key;
    int size;   /* maintained to be the number of nodes rooted here */
};

typedef vector<Tree*> tree_v;
typedef tree_v::iterator tree_vit;
 
/* clear and mark function for libcmm */
static void clear_tree(Tree *t)
{
  //  printf("clear_tree() called! at %p\n",t);
    t->left = NULL;
    t->right = NULL;
    t->key = NULL;
}

static void mark_tree(Tree *t)
{
  //  printf("mark_tree() called! at %p\n",t);
    CMM_MARK(t->left);
    CMM_MARK(t->right);
    CMM_MARK(t->key);
}

mt_t mt_tree = mt_undefined;


#define MBYTE (1<<20)

/* typedef bool finalize_func_t(void *); */
/* global counter of number of finalizers run */
int nfinal = 0;

bool tree_finalizer(void* p) {
#if 0
  char buf[500];
  sprintf(buf,"/home/jaten/dj/strongref/libcmm/finalizer.log.number_%d.pid_%d__and_ppid_%d.tid_%ld",nfinal,getpid(),getppid(),syscall(SYS_gettid));
  FILE* finallog = fopen(buf,"w");
  fprintf(finallog,"%d time in tree_finalizer, now for object %p\n",nfinal++,p);
  fclose(finallog);
  sync();
#endif
  return true;
}


void fill_tree_vector(tree_v& v, int reps) {
  d();
  CMM_ENTER; // expands to:  void **__cmm_stack_pointer = _cmm_begin_anchored()
  for (int n = 0; n < reps; n++) {
       d();
       // allocate and discard all these lines
       Tree* root = (Tree*)cmm_alloc(mt_tree);
       // CMM_ANCHOR(root) won't protect them, b/c no root points to them, so they all get collected upon CMM_EXIT.
       // So instead we'll try to make them all roots:
       //cmm_root(root); // don't use CMM_ROOT, because that takes the address of its argument!
       // and don't use cmm_root b/c that won't protect them.
       CMM_ANCHOR(root);
       v.push_back(root);
       printf("v[%d]: root is managed? %d  \n",n,(int)cmm_ismanaged(root));
  }
  d();
  CMM_EXIT; // expands to: _cmm_end_anchored(__cmm_stack_pointer)
  d();

  char* msg100 = strdup(cmm_info(3));
  printf("after 100 pointers cmm_anchored: mem_info(3): '%s'\n", msg100);

  /* currently this last call to cmm_info(3) results in: (when running "test1 100")
=========================
dump_stats:

Program received signal SIGSEGV, Segmentation fault.
0x000000000040ad71 in dump_heap_stats () at src/cmm_no_snapshot.cpp:2346
(gdb) bt
#0  0x000000000040ad71 in dump_heap_stats () at src/cmm_no_snapshot.cpp:2346
#1  0x000000000040afdc in dump (where=0x40c8a0 "cmm_collect_now", line=1862, cmmstack_t_ptr=0x0) at src/cmm_no_snapshot.cpp:2375
#2  0x00000000004082b6 in cmm_collect_now () at src/cmm_no_snapshot.cpp:1862
#3  0x0000000000401894 in main (argc=2, argv=0x7fffffffdb58) at demos/test1cmm.cpp:149
(gdb) 

*/
}


int main(int argc, char **argv)
{
  if (argc < 2) {
    fprintf(stderr,"supply number of trees to allocate and forget as the only argument to test1.\n");
    exit(1);
  }
  int reps = atoi(argv[1]);

  /* maually test that finalizer works */
  tree_finalizer(0);

    int   n = 0;
    Tree* root = 0;

    FILE* flog = fopen("test1-gc-test.log","w");

    int nbytes = 4096;
    int npages = nbytes / 4096;
    npages=1;

    cmm_init(npages, 0, flog); /* request memory in terms of npages, not nbytes */
    d();
    cmm_debug(true); /* jea debug add */

    mt_tree = CMM_REGTYPE("tree", 1024, clear_tree, mark_tree, tree_finalizer);

    n =0;

    char* msg0 = strdup(cmm_info(3));
    printf("prior to any allocation -- mem_info(3): '%s'\n", msg0);

    tree_v v;

    // call subroutine 
    d();
    fill_tree_vector(v,reps);
    d();

    printf("Just allocated 100 Trees in a subroutine.\n");


    int i = 0;
    for(tree_vit it = v.begin(); it != v.end(); ++it, ++i ) {
      printf("   v[%d] = Tree at address %p\n", i, *it);
    }



    char* msg1 = strdup(cmm_info(3));
    printf("before cmm_collect_now() and cmm_idle()-- mem_info(3): '%s'\n", msg1);

    root = 0;

    d();
    cmm_collect_now();


    // do collection in background
    while(cmm_idle()) cmm_idle();

    char* msg2 = strdup(cmm_info(3));
    printf("after cmm_collect_now() and cmm_idle()-- mem_info(3): '%s'\n", msg2);


    // do collection
    while(cmm_idle()) cmm_idle();

    cmm_collect_now();

    char* msg3 = strdup(cmm_info(3));    
    printf("after 2nd attempt at collection: cmm_collect_now() and cmm_idle()-- mem_info(3): '%s'\n", msg3);



    return 0;
}

