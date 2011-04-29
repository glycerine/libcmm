
/* private definitions */

/* jea enable debug */
#define STATICFUNC

struct cmm_stack {
   const void  **sp;
   const void  **sp_min;
   const void  **sp_max;
};

extern struct cmm_stack *const _cmm_transients;

#define st _cmm_transients

STATICFUNC inline void _cmm_anchor(C99_CONST void *p)
{
   if (st->sp > st->sp_min)
      *(--(st->sp)) = p;
   else
      cmm_anchor(p);
}

STATICFUNC inline C99_CONST void **_cmm_begin_anchored(void)
{
   return (C99_CONST void**)st->sp;
}

STATICFUNC inline void _cmm_end_anchored(C99_CONST void **sp)
{
   extern void _cmm_pop_chunk(struct cmm_stack *);

   while (st->sp>sp || sp>st->sp_max)
      _cmm_pop_chunk(st);
   st->sp = (const void**)sp;
}
#undef st

STATICFUNC inline void _cmm_mark(C99_CONST void *p)
{
   extern const bool cmm_debug_enabled;
   extern void _cmm_check_managed(C99_CONST void *);
   extern void _cmm_push(C99_CONST void *);
   if (cmm_debug_enabled) _cmm_check_managed(p);
   _cmm_push(p);
}


/* -------------------------------------------------------------
   Local Variables:
   c-file-style: "k&r"
   c-basic-offset: 3
   End:
   ------------------------------------------------------------- */
