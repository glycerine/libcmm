
/* private definitions */

/* jea enable debug */
#define STATICFUNC

struct cmm_stack {
   const void  **sp;
   const void  **sp_min;
   const void  **sp_max;
};

extern struct cmm_stack *const _cmm_transients;

// jea comment & replace st with _cmm_transients
/* #define st _cmm_transients */

STATICFUNC inline void _cmm_anchor(C99_CONST void *p)
{
   if (_cmm_transients->sp > _cmm_transients->sp_min)
      *(--(_cmm_transients->sp)) = p;
   else
      cmm_anchor(p);
}

STATICFUNC inline C99_CONST void **_cmm_begin_anchored(void)
{
   return (C99_CONST void**)_cmm_transients->sp;
}

STATICFUNC inline void _cmm_end_anchored(C99_CONST void **sp)
{
   extern void _cmm_pop_chunk(struct cmm_stack *);

   while (_cmm_transients->sp>sp || sp>_cmm_transients->sp_max)
      _cmm_pop_chunk(_cmm_transients);
   _cmm_transients->sp = (const void**)sp;
}
// jea comment
/* #undef st */ 

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
