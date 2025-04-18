/*   $Source: bitbucket.org:berkeleylab/gasnet.git/gasnet_am.c $
 * Description: GASNet conduit-independent code for Active Messages
 * Copyright 2002, Dan Bonachea <bonachea@cs.berkeley.edu>
 * Copyright 2018, The Regents of the University of California
 * Terms of use are as specified in license.txt
 */

#include <gasnet_internal.h>
#include <gasnet_am.h>

/* ------------------------------------------------------------------------------------ */
extern void gasneti_defaultAMHandler(gex_Token_t token) {
  gex_Token_Info_t info;
  info.gex_srcrank = GEX_RANK_INVALID; // to print -1 if query were to fail
  gex_TI_t rc = gex_Token_Info(token, &info, GEX_TI_SRCRANK | GEX_TI_ENTRY);
  gex_Rank_t srcnode = info.gex_srcrank;
  if (rc & GEX_TI_ENTRY) gasneti_assert(info.gex_entry);
  gasneti_fatalerror("GASNet node %i/%i received an AM message from node %i for a handler index%s "
                     "with no associated AM handler function registered", 
                     (int)gasneti_mynode, (int)gasneti_nodes, (int)srcnode,
                     (rc & GEX_TI_ENTRY)
                         ? gasneti_dynsprintf(" %d", (int)(uintptr_t)info.gex_entry->gex_cdata)
                         : "");
}
/* ------------------------------------------------------------------------------------ */

// Validate a handler table prior to registration
static void gasneti_am_validate(
                        const gex_AM_Entry_t *table,
                        int numentries)
{
  if (!numentries) return;

  // Internally-constructed legacy table should be all-or-nothing.
  if (table[0].gex_nargs == GASNETI_HANDLER_NARGS_UNK ||
      table[0].gex_flags & GASNETI_FLAG_INIT_LEGACY) {
    for (int i = 0; i < numentries; ++i) {
       gasneti_assert_always_uint(table[i].gex_nargs ,==, GASNETI_HANDLER_NARGS_UNK);
       gasneti_assert_always_uint(table[i].gex_flags ,==, (GASNETI_FLAG_AM_ANY | GASNETI_FLAG_INIT_LEGACY));
    }
    return;
  }

  // Normal tables have several rules to check:
  for (int i = 0; i < numentries; ++i) {
    int idx = table[i].gex_index;

    if_pf (table[i].gex_nargs > gex_AM_MaxArgs()) {
      gasneti_fatalerror("AM Handler table entry %d: invalid gex_nargs: %d (Max %d)",
                         i, (int)table[i].gex_nargs, (int)gex_AM_MaxArgs());
    }

    if_pf (0 == (table[i].gex_flags & (GEX_FLAG_AM_REQUEST|GEX_FLAG_AM_REPLY))) {
      gasneti_fatalerror("AM Handler table entry %d(idx=%d): invalid gex_flags: contains neither GEX_FLAG_AM_REQUEST nor GEX_FLAG_AM_REPLY", i, idx);
    }

    gex_Flags_t category = table[i].gex_flags & (GEX_FLAG_AM_SHORT|GEX_FLAG_AM_MEDIUM|GEX_FLAG_AM_LONG);
    const char *cat_msg = NULL;
    switch (category) {
    case 0:
      cat_msg = "none of GEX_FLAG_AM_SHORT, GEX_FLAG_AM_MEDIUM, or GEX_FLAG_AM_LONG";
      break;
    case GEX_FLAG_AM_SHORT|GEX_FLAG_AM_MEDIUM|GEX_FLAG_AM_LONG:
      cat_msg = "invalid combination (GEX_FLAG_AM_SHORT | GEX_FLAG_AM_MEDIUM | GEX_FLAG_AM_LONG)";
      break;
    case GEX_FLAG_AM_SHORT|GEX_FLAG_AM_MEDIUM:
      cat_msg = "invalid combination (GEX_FLAG_AM_SHORT | GEX_FLAG_AM_MEDIUM )";
      break;
    case GEX_FLAG_AM_SHORT|GEX_FLAG_AM_LONG:
      cat_msg = "invalid combination (GEX_FLAG_AM_SHORT | GEX_FLAG_AM_LONG)";
      break;
    }
    if_pf (cat_msg) {
      gasneti_fatalerror("AM Handler table entry %d(idx=%d): invalid gex_flags: contains %s", i, idx, cat_msg);
    }
  }
}

#if GASNETC_AMREGISTER
  /* Use a conduit-specific hook at registration */
  extern int gasnetc_amregister(gex_AM_Index_t, gex_AM_Entry_t *);
#endif

// Register handlers in the range [lowlimit,highlimit)
// Thread-safety is the caller's responsibility
extern int gasneti_amregister( gasneti_EP_t i_ep,
                               gex_AM_Entry_t *input, int numentries,
                               int lowlimit, int highlimit,
                               int dontcare, int *numregistered) {
  int i;
  *numregistered = 0;

  gasneti_am_validate(input, numentries);

  gex_AM_Entry_t *output = i_ep->_amtbl;

  for (i = 0; i < numentries; i++) {
    int newindex;

    if ((input[i].gex_index == 0 && !dontcare) ||
        (input[i].gex_index && dontcare)) continue;
    else if (input[i].gex_index) newindex = input[i].gex_index;
    else { /* deterministic assignment of dontcare indexes from top down */
      for (newindex = highlimit-1; newindex >= lowlimit; newindex--) {
        if (!output[newindex].gex_index) break; // 0 index marks free entry
      }
      if (newindex < lowlimit) {
        char s[255];
        snprintf(s, sizeof(s), "Too many handlers. (limit=%i)", highlimit - lowlimit);
        GASNETI_RETURN_ERRR(BAD_ARG, s);
      }
    }

    /*  ensure handlers fall into the proper range of pre-assigned values */
    if (newindex < lowlimit || newindex >= highlimit) {
      char s[255];
      snprintf(s, sizeof(s), "handler index (%i) out of range [%i..%i)", newindex, lowlimit, highlimit);
      GASNETI_RETURN_ERRR(BAD_ARG, s);
    }

    /* discover duplicates */
    if (output[newindex].gex_index) // entry is taken
      GASNETI_RETURN_ERRR(BAD_ARG, "handler index not unique");

    /* register a single handler with conduit-specifc hook, if any */
  #if GASNETC_AMREGISTER
    int rc = gasnetc_amregister((gex_AM_Index_t)newindex, &input[i]);
    if (GASNET_OK != rc) return rc;
  #endif

    /* The check below for !input[i].index is redundant and present
     * only to defeat the over-aggressive optimizer in pathcc 2.1
     */
    if (dontcare && !input[i].gex_index) input[i].gex_index = newindex;

    /* Install the entire table entry */
    gasneti_assert(! output[newindex].gex_index);
    output[newindex] = input[i];

    #if GASNET_TRACE
    {
      const char *name = input[i].gex_name
                         ? gasneti_dynsprintf(", name='%s'", input[i].gex_name) : "";
      void *fnptr = *(void**)&input[i].gex_fnptr; // level of indirection avoids -pedantic warning
      char *flags = gasneti_malloc(gasneti_format_flags_amreg(NULL, input[i].gex_flags));
      gasneti_format_flags_amreg(flags, input[i].gex_flags);
      const char *nargs = (input[i].gex_nargs == GASNETI_HANDLER_NARGS_UNK)
                          ? "" : gasneti_dynsprintf(", nargs=%u", input[i].gex_nargs);
      if (newindex >= GASNETI_CLIENT_HANDLER_BASE) {
        GASNETI_TRACE_PRINTF(O,("Registered AM handler %d: client table entry=%d, flags=%s%s, fnptr=%p%s%s",
                                newindex, i, flags, nargs, fnptr, name,
                                dontcare ? ", input index was zero" : ""));
      } else {
        gasneti_static_assert(GASNETE_HANDLER_BASE > GASNETC_HANDLER_BASE);
        const char *api = (newindex >= GASNETE_HANDLER_BASE) ? "extended" : "core";
        GASNETI_TRACE_PRINTF(D,("Registered AM handler %d: %s API, flags=%s%s, fnptr=%p%s",
                                newindex, api, flags, nargs, fnptr, name));
      }
      gasneti_free(flags);
    }
    #endif

    (*numregistered)++;
  }
  return GASNET_OK;
}

// Register client handlers
// This function backs gex_EP_RegisterHandlers() and gasnet_attach()
// and provides per-EP serialization of such calls.
// Internal calls occuring within gex_Client_Init() and gex_Client_Create()
// do not participate in this serialization, since they operated exclusively
// on an EP prior to returning it to the client.
extern int gasneti_amregister_client(
                        gasneti_EP_t i_ep,
                        gex_AM_Entry_t *input,
                        size_t numentries)
{
  if_pf (numentries == 0) return GASNET_OK;
  if_pf (numentries > GASNETC_MAX_NUMHANDLERS - GEX_AM_INDEX_BASE) 
      GASNETI_RETURN_ERRR(BAD_ARG,"Tried to register too many handlers");
  if_pf (input == NULL)
      GASNETI_RETURN_ERRR(BAD_ARG,"Invalid AM handler table");

  gasneti_mutex_lock(&i_ep->_amtbl_lock);

  /*  first pass - assign all fixed-index handlers */
  int numreg1 = 0;
  if (gasneti_amregister(i_ep, input, numentries,
                         GASNETI_CLIENT_HANDLER_BASE, GASNETC_MAX_NUMHANDLERS,
                         0, &numreg1) != GASNET_OK) {
      gasneti_mutex_unlock(&i_ep->_amtbl_lock);
      GASNETI_RETURN_ERRR(RESOURCE,"Error registering fixed-index client handlers");
  }

  /*  second pass - fill in dontcare-index handlers */
  int numreg2 = 0;
  if (gasneti_amregister(i_ep, input, numentries,
                         GASNETI_CLIENT_HANDLER_BASE, GASNETC_MAX_NUMHANDLERS,
                         1, &numreg2) != GASNET_OK) {
      gasneti_mutex_unlock(&i_ep->_amtbl_lock);
      GASNETI_RETURN_ERRR(RESOURCE,"Error registering variable-index client handlers");
  }

  gasneti_mutex_unlock(&i_ep->_amtbl_lock);

  gasneti_assert_uint(numreg1 + numreg2 ,==, numentries);

  return GASNET_OK;
}

// Wrapper to provide continued support for GASNet-1 legacy handler tables,
// such as through gasnet_attach().  Only supports the clients's index range.
// TODO-EX: should be absorbed into an eventual conduit-indep gasnet_attach()
extern int gasneti_amregister_legacy( gasneti_EP_t i_ep,
                                      gasnet_handlerentry_t *table, int numentries) {

  if_pf (numentries == 0) return GASNET_OK;
  if_pf (numentries > GASNETC_MAX_NUMHANDLERS - GEX_AM_INDEX_BASE) 
      GASNETI_RETURN_ERRR(BAD_ARG,"Tried to register too many handlers");
  if_pf (numentries < 0) 
      GASNETI_RETURN_ERRR(BAD_ARG,"Invalid AM handler table size");
  if_pf (table == NULL)
      GASNETI_RETURN_ERRR(BAD_ARG,"Invalid AM handler table");

  /* create temporary ex-compatible table */
  gex_AM_Entry_t *extable = gasneti_calloc(numentries, sizeof(gex_AM_Entry_t));
  for (int i = 0; i < numentries; ++i) {
    extable[i].gex_index = table[i].index;
    extable[i].gex_fnptr = table[i].fnptr;
    extable[i].gex_nargs = GASNETI_HANDLER_NARGS_UNK;
    extable[i].gex_flags = GASNETI_FLAG_AM_ANY | GASNETI_FLAG_INIT_LEGACY;
  }

  /* register */
  if (gasneti_amregister_client(i_ep, extable, numentries) != GASNET_OK) {
      gasneti_free(extable);
      GASNETI_RETURN_ERRR(RESOURCE,"Error registering client handlers");
  }

  /* copy back from temporary ex-compatible table */
  for (int i = 0; i < numentries; ++i) {
    table[i].index = extable[i].gex_index;
  }
  gasneti_free(extable);

  return GASNET_OK;
}

// Initialize handler table in a given EP
extern int gasneti_amtbl_init(gasneti_EP_t i_ep) {
  gex_AM_Entry_t *output = i_ep->_amtbl;
  static const char *fnname = "gasneti_defaultAMHandler";
  for (int i = 0; i < GASNETC_MAX_NUMHANDLERS; i++) {
    output[i].gex_index = 0; // marks an unused entry
    output[i].gex_nargs = GASNETI_HANDLER_NARGS_UNK;
    output[i].gex_flags = GASNETI_FLAG_AM_ANY;
    output[i].gex_fnptr = gasneti_defaultAMHandler;
    output[i].gex_cdata = (void *)(uintptr_t)i;
    output[i].gex_name  = fnname;
  }
  gasneti_mutex_init(&i_ep->_amtbl_lock);
  return GASNET_OK;
}

#if GASNET_DEBUG && !defined(gasneti_amtbl_check)
// Validate call to a handler
extern void gasneti_amtbl_check(const gex_AM_Entry_t *entry, int nargs,
                                gasneti_category_t category, int isReq) {
  char buf[128] = {'\0'};
  const char *msg = NULL;
  if ((entry->gex_nargs != nargs) && (entry->gex_nargs != GASNETI_HANDLER_NARGS_UNK)) {
    snprintf(buf, sizeof(buf), "registered with nargs=%d but called with %d", entry->gex_nargs, nargs);
    msg = buf;
  } else if (isReq && !(entry->gex_flags & GEX_FLAG_AM_REQUEST)) {
    msg = "invoked as a Request handler, but not registered with GEX_FLAG_AM_REQUEST";
  } else if (!isReq && !(entry->gex_flags & GEX_FLAG_AM_REPLY)) {
    msg = "invoked as a Reply handler, but not registered with GEX_FLAG_AM_REPLY";
  } else if (category == gasneti_Short && !(entry->gex_flags & GEX_FLAG_AM_SHORT)) {
    msg = "invoked as a Short handler, but not registered with GEX_FLAG_AM_SHORT";
  } else if (category == gasneti_Medium && !(entry->gex_flags & GEX_FLAG_AM_MEDIUM)) {
    msg = "invoked as a Medium handler, but not registered with GEX_FLAG_AM_MEDIUM";
  } else if (category == gasneti_Long && !(entry->gex_flags & GEX_FLAG_AM_LONG)) {
    msg = "invoked as a Long handler, but not registered with GEX_FLAG_AM_LONG";
  }
  if (msg) {
    char fnaddr[32];
    const char *fnname = entry->gex_name;
    if (!fnname) {
      (void) snprintf(fnaddr, sizeof(fnaddr), "%p", 
                     *(void**)&entry->gex_fnptr); // level of indirection avoids -pedantic warning
      fnname = fnaddr;
    }
    gasneti_fatalerror("AM handler %d (%s) %s", entry->gex_index, fnname, msg);
  }
}
#endif

/* ------------------------------------------------------------------------------------ */
extern int gex_EP_RegisterHandlers(
                gex_EP_t                ep,
                gex_AM_Entry_t          *table,
                size_t                  numentries)
{
  GASNETI_TRACE_PRINTF(O,("gex_EP_RegisterHandlers: ep=%p table=%p numentries=%"PRIuSZ,
                          (void*)ep, (void*)table, numentries));
  return gasneti_amregister_client(gasneti_import_ep(ep), table, numentries);
}

/* ------------------------------------------------------------------------------------ */
#if GASNET_DEBUG
// Post processing of gex_Token_Info() results
extern gex_TI_t gasneti_token_info_return(gex_TI_t result, gex_Token_Info_t *info, gex_TI_t mask) {
  // Validate client's requested mask
  if (mask & ~GEX_TI_ALL) {
    gasneti_fatalerror("Mask argument to gex_Token_Info() includes unknown bits");
  }

  // Validate conduit's returned mask (any requested+required fields missing?);
  gasneti_assert_uint( (~result & (mask & GASNETI_TI_REQUIRED)) ,==, 0);
#if GASNET_SUPPORTS_TI_ENTRY
  gasneti_assert_uint( (~result & (mask & GEX_TI_ENTRY)) ,==, 0);
#endif
#if GASNET_SUPPORTS_TI_IS_REQ
  gasneti_assert_uint( (~result & (mask & GEX_TI_IS_REQ)) ,==, 0);
#endif
#if GASNET_SUPPORTS_TI_IS_LONG
  gasneti_assert_uint( (~result & (mask & GEX_TI_IS_LONG)) ,==, 0);
#endif

  // For each field set: validate
  if (result & GEX_TI_SRCRANK) {
    gasneti_assert_uint(info->gex_srcrank ,<, gasneti_nodes);
  }
  if (result & GEX_TI_EP) {
    // TODO-EX: will need some means to validate in conduit-independent manner
    // NULL THUNK_TM may occur in bootstrap collectives before ep0 exists
    gasneti_assert(!gasneti_THUNK_TM || info->gex_ep == gasneti_THUNK_EP);
  }
  if (result & GEX_TI_ENTRY) {
    gasneti_assert(info->gex_entry);
    gasneti_am_validate(info->gex_entry, 1);
  }
  if (result & GEX_TI_IS_REQ) {
    gasneti_assert_uint(info->gex_is_req ,==, !!info->gex_is_req); // Is 0 or 1
    if (result & GEX_TI_ENTRY) {
      gasneti_assert(info->gex_entry->gex_flags &
                     (info->gex_is_req ? GEX_FLAG_AM_REQUEST : GEX_FLAG_AM_REPLY));
    }
  }
  if (result & GEX_TI_IS_LONG) {
    gasneti_assert_uint(info->gex_is_long ,==, !!info->gex_is_long); // Is 0 or 1
    if (result & GEX_TI_ENTRY) {
      gasneti_assert(info->gex_entry->gex_flags &
                     (info->gex_is_long ? GEX_FLAG_AM_LONG : GEX_FLAG_AM_SHORT|GEX_FLAG_AM_MEDIUM));
    }
  }

  // From here forward, consider only the requested subset of the conduit-provided result
  result &= mask;

  // For each field not requested or requested but not set: INvalidate
  if (!(result & GEX_TI_SRCRANK)) {
    info->gex_srcrank = GEX_RANK_INVALID;
  }
  if (!(result & GEX_TI_EP)) {
    info->gex_ep = NULL;
  }
  if (!(result & GEX_TI_ENTRY)) {
    info->gex_entry = NULL;
  }
  if (!(result & GEX_TI_IS_REQ)) {
    info->gex_is_req = 2; // true invalidation not possible for a boolean
  }
  if (!(result & GEX_TI_IS_LONG)) {
    info->gex_is_long = 2; // true invalidation not possible for a boolean
  }

  return result;
}
#endif

/* ------------------------------------------------------------------------------------ */
// Error checking for AM payload queries

#if GASNET_DEBUG
// TODO-EX: are there any additional checks still possible here?

static void check_max_payload_args(
           const char *fname, gasneti_category_t category, int isReq,
           const gex_Event_t *lc_opt, gex_Flags_t flags,
           unsigned int nargs)
{
  if (lc_opt == GEX_EVENT_DEFER) {
    gasneti_fatalerror("Call to %s() with invalid lc_opt=GEX_EVENT_DEFER", fname);
  }
  if (!isReq && (lc_opt == GEX_EVENT_GROUP)) {
    gasneti_fatalerror("Call to %s() with invalid lc_opt=GEX_EVENT_GROUP", fname);
  }
  if (lc_opt && gasneti_leaf_is_pointer(lc_opt)) {
    // Following assumes minimum 4-byte alignment of gex_Event_t
    if (0x3 & (uintptr_t)lc_opt) {
      gasneti_fatalerror("Call to %s() with invalid lc_opt=%p", fname, (void *)lc_opt);
    }
    // Following attempts to elicit SIGSEGV/SIGBUS/SIGILL on bogus pointers
    static uintptr_t dummy;
    dummy += (uintptr_t) *(volatile gex_Event_t *)lc_opt;
  }
  if ((flags & GEX_FLAG_AM_PREPARE_LEAST_CLIENT) &&
      (flags & GEX_FLAG_AM_PREPARE_LEAST_ALLOC)) {
    gasneti_fatalerror("Call to %s() with mutually-exclusive "
                       "GEX_FLAG_AM_PREPARE_LEAST_CLIENT and "
                       "GEX_FLAG_AM_PREPARE_LEAST_ALLOC both set in flags argument",
                       fname);
  }
  if (nargs > gex_AM_MaxArgs()) {
    gasneti_fatalerror("Call to %s() with nargs=%u greater than gex_AM_MaxArgs()=%u",
                       fname, nargs, (unsigned int)gex_AM_MaxArgs());
  }
}

static void check_max_payload_result(gex_Flags_t flags, size_t lub, size_t result)
{
  gasneti_assert_uint(result ,>=, 512);
  gasneti_assert((result >= lub) ||
                 (flags & GEX_FLAG_AM_PREPARE_LEAST_CLIENT) ||
                 (flags & GEX_FLAG_AM_PREPARE_LEAST_ALLOC));
}

#define GASNETC_IS_REQ(reqrep) _GASNETC_IS_REQ_##reqrep
#define _GASNETC_IS_REQ_Request 1
#define _GASNETC_IS_REQ_Reply   0

#define DEFN_AM_MAX_FN(reqrep,cat) \
size_t gex_AM_Max##reqrep##cat(                                            \
           gex_TM_t tm, gex_Rank_t rank,                                   \
           const gex_Event_t *lc_opt, gex_Flags_t flags,                   \
           unsigned int nargs)                                             \
{                                                                          \
  const char *fname = "gex_AM_Max" #reqrep #cat;                           \
  gasneti_TM_t real_tm = gasneti_import_tm(tm);                            \
  /* TODO-EX: remove allowance for real_tm == NULL */                      \
  gex_Rank_t tm_size = gasneti_i_tm_size(real_tm);                         \
  if (real_tm && (rank != GEX_RANK_INVALID) && (rank >= tm_size)) {        \
    gasneti_fatalerror("Call to %s() with invalid rank=%i",                \
                       fname, (int)rank);                                  \
  }                                                                        \
  gasneti_category_t category = gasneti_##cat;                             \
  int isReq = GASNETC_IS_REQ(reqrep);                                      \
  check_max_payload_args(fname, category, isReq,lc_opt, flags, nargs);     \
  size_t result = gasnetc_AM_Max##reqrep##cat(tm,rank,lc_opt,flags,nargs); \
  check_max_payload_result(flags, gex_AM_LUB##reqrep##cat(), result);      \
  return result;                                                           \
}

#define DEFN_TOKEN_MAX_FN(reqrep,cat) \
size_t gex_Token_Max##reqrep##cat(                                          \
           gex_Token_t token,                                               \
           const gex_Event_t *lc_opt, gex_Flags_t flags,                    \
           unsigned int nargs)                                              \
{                                                                           \
  const char *fname = "gex_Token_Max" #reqrep #cat;                         \
  gasneti_category_t category = gasneti_##cat;                              \
  int isReq = GASNETC_IS_REQ(reqrep);                                       \
  check_max_payload_args(fname, category, isReq,lc_opt, flags, nargs);      \
  size_t result = gasnetc_Token_Max##reqrep##cat(token,lc_opt,flags,nargs); \
  check_max_payload_result(flags, gex_AM_LUB##reqrep##cat(), result);       \
  return result;                                                            \
}

DEFN_AM_MAX_FN(Request,Medium)
DEFN_AM_MAX_FN(Request,Long)
DEFN_AM_MAX_FN(Reply,Medium)
DEFN_AM_MAX_FN(Reply,Long)

DEFN_TOKEN_MAX_FN(Reply,Medium)
DEFN_TOKEN_MAX_FN(Reply,Long)

#undef DEFN_AM_MAX_FN
#undef DEFN_TOKEN_MAX_FN
#endif

/* ------------------------------------------------------------------------------------ */
// Implementation of Negotiated-Payload AMs
//
// For conduit's without specialization of NP-AM, this provides the entire
// default implementation, using malloc() to obtain gasnet-owned buffers (if
// any) at Prepare and using gasneti_AM{Request,Reply}{Medium,Long}V() to
// perform the AM injection at Commit.
//
// TODO-EX: This default is not a "good" implementation for any conduit.
// TODO-EX: Native conduits should provide their own negotiated-payload and
//          ideally implement it and fixed-payload in terms of a common base.
// TODO-EX: This default's use of GEX_EVENT_NOW for gasnet-owned buffers could
//          be replaced with &event, *if* a progress function (or dependent
//          operation) were available to reap them and free buffers.  However,
//          that is only fruitful with a conduit which can provide asynchronous
//          local completion other than by copying the payload.

// Conduits can enable build (possibly name-shifted?) of the four pieces of the
// reference implementation by defining GASNETC_BUILD_NP_{REQ,REP}_{MEDIUM,LONG}
// to '1'; or they may disable them by defining to '0'.
// Otherwise, these default to following !GASNET_NATIVE_NP_ALLOC_*.
//
#ifdef GASNETC_BUILD_NP_REQ_MEDIUM
  // Keep conduit's definition
#elif !GASNET_NATIVE_NP_ALLOC_REQ_MEDIUM
  #define GASNETC_BUILD_NP_REQ_MEDIUM 1
#endif
#ifdef GASNETC_BUILD_NP_REP_MEDIUM
  // Keep conduit's definition
#elif !GASNET_NATIVE_NP_ALLOC_REP_MEDIUM
  #define GASNETC_BUILD_NP_REP_MEDIUM 1
#endif
#ifdef GASNETC_BUILD_NP_REQ_LONG
  // Keep conduit's definition
#elif !GASNET_NATIVE_NP_ALLOC_REQ_LONG
  #define GASNETC_BUILD_NP_REQ_LONG 1
#endif
#ifdef GASNETC_BUILD_NP_REP_LONG
  // Keep conduit's definition
#elif !GASNET_NATIVE_NP_ALLOC_REP_LONG
  #define GASNETC_BUILD_NP_REP_LONG 1
#endif

#ifndef _GEX_AM_SRCDESC_T
#ifndef gasneti_import_srcdesc
gasneti_AM_SrcDesc_t gasneti_import_srcdesc(gex_AM_SrcDesc_t _srcdesc) {
  const gasneti_AM_SrcDesc_t _real_srcdesc = GASNETI_IMPORT_POINTER(gasneti_AM_SrcDesc_t,_srcdesc);
  GASNETI_CHECK_MAGIC(_real_srcdesc, GASNETI_AM_SRCDESC_MAGIC);
  gasneti_assert(!_real_srcdesc || (_real_srcdesc->_thread == _gasneti_mythread_slow()));
  return _real_srcdesc;
}
#endif

#ifndef gasneti_export_srcdesc
gex_AM_SrcDesc_t gasneti_export_srcdesc(gasneti_AM_SrcDesc_t _real_srcdesc) {
  GASNETI_CHECK_MAGIC(_real_srcdesc, GASNETI_AM_SRCDESC_MAGIC);
  return GASNETI_EXPORT_POINTER(gex_AM_SrcDesc_t, _real_srcdesc);
}
#endif

#if GASNETI_NEED_INIT_SRCDESC
void gasneti_init_srcdesc(GASNETI_THREAD_FARG_ALONE)
{
  gasneti_threaddata_t * const mythread = GASNETI_MYTHREAD;
  gasneti_assert(! mythread->sd_is_init);

  // Yes, we start "BAD":
  GASNETI_INIT_MAGIC(&mythread->request_sd, GASNETI_AM_SRCDESC_BAD_MAGIC);
  GASNETI_INIT_MAGIC(&mythread->reply_sd, GASNETI_AM_SRCDESC_BAD_MAGIC);

  mythread->request_sd._thread = mythread;
  mythread->reply_sd._thread = mythread;

#if GASNET_DEBUG
  mythread->request_sd._isreq = 1;
  mythread->reply_sd._isreq = 0;
#endif

  gasneti_assert( mythread->request_sd._tofree == NULL );
  gasneti_assert( mythread->reply_sd._tofree == NULL );

  mythread->sd_is_init = 1;
}
#endif // GASNETI_NEED_INIT_SRCDESC

#if GASNET_DEBUG
void gasneti_checknpam(int for_reply GASNETI_THREAD_FARG) {
  gasneti_threaddata_t * const mythread = GASNETI_MYTHREAD;
  if (mythread->sd_is_init) {
    // Never valid to communicate between Prepare/Commit of Reply
    if (mythread->reply_sd._magic._u == GASNETI_AM_SRCDESC_MAGIC) {
      gasneti_fatalerror("Invalid GASNet call (communication injection or poll) between gex_AM_PrepareReply() and the corresponding Commit on this thread");
    }
    // It *is* valid to send a Reply which may dynamically run
    // *within* the execution of gex_AM_{Prepare,Commit}Request()
    if (!for_reply && mythread->request_sd._magic._u == GASNETI_AM_SRCDESC_MAGIC) {
      gasneti_fatalerror("Invalid GASNet call (communication injection or poll) between gex_AM_PrepareRequest() and the corresponding Commit on this thread");
    }
  }
}

// Conduits can call this from gasnet_exit() to disarm gasneti_checknpam() to
// enable use of AM for exit coordination even if exiting between a Prepare
// and the following Commit.
//
// TODO: Opt-in use of gex_AM_Cancel*() when available and safe.
void gasneti_checknpam_disarm(void) {
  GASNET_BEGIN_FUNCTION(); // OK - not a critical-path
  gasneti_threaddata_t * const mythread = GASNETI_MYTHREAD;
  if (mythread->sd_is_init) {
    GASNETI_INIT_MAGIC(&mythread->request_sd, GASNETI_AM_SRCDESC_BAD_MAGIC);
    GASNETI_INIT_MAGIC(&mythread->reply_sd,   GASNETI_AM_SRCDESC_BAD_MAGIC);
  }
}
#endif // GASNET_DEBUG
#endif // _GEX_AM_SRCDESC_T

#if GASNETC_BUILD_NP_REQ_MEDIUM
extern gex_AM_SrcDesc_t gasnetc_AM_PrepareRequestMedium(
                       gex_TM_t           tm,
                       gex_Rank_t         rank,
                       const void        *client_buf,
                       size_t             least_payload,
                       size_t             most_payload,
                       gex_Event_t       *lc_opt,
                       gex_Flags_t        flags
                       GASNETI_THREAD_FARG,
                       unsigned int       nargs)
{
    GASNETI_TRACE_PREP_REQUESTMEDIUM(tm,rank,client_buf,least_payload,most_payload,flags,nargs);

    gex_Rank_t jobrank = gasneti_e_tm_rank_to_jobrank(tm, rank);

    // Ensure at least one poll upon Request injection (exactly one if possible)
#if GASNETC_REQUESTV_POLLS // Conduit's Request{Medium,Long}V will AMPoll in Commit
    if (GASNETI_NBRHD_JOBRANK_IS_LOCAL(jobrank)) GASNETC_IMMEDIATE_MAYBE_POLL(flags);
#else
    GASNETC_IMMEDIATE_MAYBE_POLL(flags);
#endif

    gasneti_AM_SrcDesc_t sd = gasneti_init_request_srcdesc(GASNETI_THREAD_PASS_ALONE);
    GASNETI_COMMON_PREP_REQ(sd,tm,rank,client_buf,least_payload,most_payload,NULL,lc_opt,flags,nargs,Medium);

    flags &= ~(GEX_FLAG_AM_PREPARE_LEAST_CLIENT | GEX_FLAG_AM_PREPARE_LEAST_ALLOC);

    if (GASNETI_NBRHD_JOBRANK_IS_LOCAL(jobrank)) {
        sd = gasnetc_nbrhd_PrepareRequest(sd, gasneti_Medium, jobrank,
                                               client_buf, least_payload, most_payload,
                                               NULL, lc_opt, flags, nargs);
    } else {
        // In reference implementation, GEX_FLAG_AM_PREPARE_LEAST_ALLOC is also the MAX we allocate
        gex_Flags_t limit_flags = client_buf ? flags : (flags | GEX_FLAG_AM_PREPARE_LEAST_ALLOC);
        size_t limit = gex_AM_MaxRequestMedium(tm, rank, lc_opt, limit_flags, nargs);
        size_t size = MIN(most_payload, limit);
        sd->_tofree = gasneti_prepare_request_common(sd, tm, rank, client_buf, size, lc_opt, flags, nargs);
        gasneti_init_sd_poison(sd);
    }

    GASNETI_TRACE_PREP_RETURN(REQUEST_MEDIUM, sd);
    GASNETI_CHECK_SD(client_buf, least_payload, most_payload, sd);
    return gasneti_export_srcdesc(sd);
}
#endif // GASNETC_BUILD_NP_REQ_MEDIUM

#if GASNETC_BUILD_NP_REP_MEDIUM
extern gex_AM_SrcDesc_t gasnetc_AM_PrepareReplyMedium(
                       gex_Token_t        token,
                       const void        *client_buf,
                       size_t             least_payload,
                       size_t             most_payload,
                       gex_Event_t       *lc_opt,
                       gex_Flags_t        flags,
                       unsigned int       nargs)
{
    GASNETI_TRACE_PREP_REPLYMEDIUM(token,client_buf,least_payload,most_payload,flags,nargs);

    gasneti_AM_SrcDesc_t sd;
    flags &= ~(GEX_FLAG_AM_PREPARE_LEAST_CLIENT | GEX_FLAG_AM_PREPARE_LEAST_ALLOC);

    if (gasnetc_token_in_nbrhd(token)) {
        sd = gasnetc_nbrhd_PrepareReply(gasneti_Medium, token,
                                             client_buf, least_payload, most_payload,
                                             NULL, lc_opt, flags, nargs);
    } else {
        GASNET_BEGIN_FUNCTION(); // conduit-specialization should post from token instead
        sd = gasneti_init_reply_srcdesc(GASNETI_THREAD_PASS_ALONE);
        GASNETI_COMMON_PREP_REP(sd,token,client_buf,least_payload,most_payload,NULL,lc_opt,flags,nargs,Medium);

        // In reference implementation, GEX_FLAG_AM_PREPARE_LEAST_ALLOC is also the MAX we allocate
        gex_Flags_t limit_flags = client_buf ? flags : (flags | GEX_FLAG_AM_PREPARE_LEAST_ALLOC);
        size_t limit = gex_Token_MaxReplyMedium(token, lc_opt, limit_flags, nargs);
        size_t size = MIN(most_payload, limit);
        sd->_tofree = gasneti_prepare_reply_common(sd, token, client_buf, size, lc_opt, flags, nargs);
        gasneti_init_sd_poison(sd);
    }

    GASNETI_TRACE_PREP_RETURN(REPLY_MEDIUM, sd);
    GASNETI_CHECK_SD(client_buf, least_payload, most_payload, sd);
    return gasneti_export_srcdesc(sd);
}
#endif // GASNETC_BUILD_NP_REP_MEDIUM

#if GASNETC_BUILD_NP_REQ_LONG
extern gex_AM_SrcDesc_t gasnetc_AM_PrepareRequestLong(
                       gex_TM_t           tm,
                       gex_Rank_t         rank,
                       const void        *client_buf,
                       size_t             least_payload,
                       size_t             most_payload,
                       void              *dest_addr,
                       gex_Event_t       *lc_opt,
                       gex_Flags_t        flags
                       GASNETI_THREAD_FARG,
                       unsigned int       nargs)
{
    GASNETI_TRACE_PREP_REQUESTLONG(tm,rank,client_buf,least_payload,most_payload,dest_addr,flags,nargs);

    gex_Rank_t jobrank = gasneti_e_tm_rank_to_jobrank(tm, rank);
    // Ensure at least one poll upon Request injection (exactly one if possible)
#if GASNETC_REQUESTV_POLLS // Conduit's Request{Medium,Long}V will AMPoll in Commit
    if (GASNETI_NBRHD_JOBRANK_IS_LOCAL(jobrank)) GASNETC_IMMEDIATE_MAYBE_POLL(flags);
#else
    GASNETC_IMMEDIATE_MAYBE_POLL(flags);
#endif

    gasneti_AM_SrcDesc_t sd = gasneti_init_request_srcdesc(GASNETI_THREAD_PASS_ALONE);
    GASNETI_COMMON_PREP_REQ(sd,tm,rank,client_buf,least_payload,most_payload,dest_addr,lc_opt,flags,nargs,Long);

    flags &= ~(GEX_FLAG_AM_PREPARE_LEAST_CLIENT | GEX_FLAG_AM_PREPARE_LEAST_ALLOC);

    if (GASNETI_NBRHD_JOBRANK_IS_LOCAL(jobrank)) {
        sd = gasnetc_nbrhd_PrepareRequest(sd, gasneti_Long, jobrank,
                                               client_buf, least_payload, most_payload,
                                               dest_addr, lc_opt, flags, nargs);
    } else {
        // In reference implementation, GEX_FLAG_AM_PREPARE_LEAST_ALLOC is also the MAX we allocate
        gex_Flags_t limit_flags = client_buf ? flags : (flags | GEX_FLAG_AM_PREPARE_LEAST_ALLOC);
        size_t limit = gex_AM_MaxRequestLong(tm, rank, lc_opt, limit_flags, nargs);
        size_t size = MIN(most_payload, limit);
        sd->_tofree = gasneti_prepare_request_common(sd, tm, rank, client_buf, size, lc_opt, flags, nargs);
        sd->_dest_addr = dest_addr;
        gasneti_init_sd_poison(sd);
    }

    GASNETI_TRACE_PREP_RETURN(REQUEST_LONG, sd);
    GASNETI_CHECK_SD(client_buf, least_payload, most_payload, sd);
    return gasneti_export_srcdesc(sd);
}
#endif // GASNETC_BUILD_NP_REQ_LONG

#if GASNETC_BUILD_NP_REP_LONG
extern gex_AM_SrcDesc_t gasnetc_AM_PrepareReplyLong(
                       gex_Token_t        token,
                       const void        *client_buf,
                       size_t             least_payload,
                       size_t             most_payload,
                       void              *dest_addr,
                       gex_Event_t       *lc_opt,
                       gex_Flags_t        flags,
                       unsigned int       nargs)
{
    GASNETI_TRACE_PREP_REPLYLONG(token,client_buf,least_payload,most_payload,dest_addr,flags,nargs);

    gasneti_AM_SrcDesc_t sd;
    flags &= ~(GEX_FLAG_AM_PREPARE_LEAST_CLIENT | GEX_FLAG_AM_PREPARE_LEAST_ALLOC);

    if (gasnetc_token_in_nbrhd(token)) {
        sd = gasnetc_nbrhd_PrepareReply(gasneti_Long, token,
                                             client_buf, least_payload, most_payload,
                                             dest_addr, lc_opt, flags, nargs);
    } else {
        GASNET_BEGIN_FUNCTION(); // conduit-specialization should post from token instead
        sd = gasneti_init_reply_srcdesc(GASNETI_THREAD_PASS_ALONE);
        GASNETI_COMMON_PREP_REP(sd,token,client_buf,least_payload,most_payload,dest_addr,lc_opt,flags,nargs,Long);

        // In reference implementation, GEX_FLAG_AM_PREPARE_LEAST_ALLOC is also the MAX we allocate
        gex_Flags_t limit_flags = client_buf ? flags : (flags | GEX_FLAG_AM_PREPARE_LEAST_ALLOC);
        size_t limit = gex_Token_MaxReplyLong(token, lc_opt, limit_flags, nargs);
        size_t size = MIN(most_payload, limit);
        sd->_tofree = gasneti_prepare_reply_common(sd, token, client_buf, size, lc_opt, flags, nargs);
        sd->_dest_addr = dest_addr;
        gasneti_init_sd_poison(sd);
    }

    GASNETI_TRACE_PREP_RETURN(REPLY_LONG, sd);
    GASNETI_CHECK_SD(client_buf, least_payload, most_payload, sd);
    return gasneti_export_srcdesc(sd);
}
#endif // GASNETC_BUILD_NP_REP_LONG

#if GASNETC_BUILD_NP_REQ_MEDIUM
void gasnetc_AM_CommitRequestMediumM(
                       gex_AM_Index_t          handler,
                       size_t                  nbytes
                       GASNETI_THREAD_FARG,
                     #if GASNET_DEBUG
                       unsigned int            nargs_arg,
                     #endif
                       gex_AM_SrcDesc_t        sd_arg, ...)
{
    // Conduit authors are cautioned against use of gasneti_consume_srcdesc() in native
    // NPAM implementations.  See the comment preceding its definition in gasnet_am.h.
    gasneti_AM_SrcDesc_t sd = gasneti_consume_srcdesc(sd_arg);

    GASNETI_COMMON_COMMIT_REQ(sd,handler,nbytes,NULL,nargs_arg,Medium);

    va_list argptr;
    va_start(argptr, sd_arg);
    if (sd->_is_nbrhd) {
        gasnetc_nbrhd_CommitRequest(sd, gasneti_Medium, handler, nbytes, NULL, argptr);
    } else {   GASNET_POST_THREADINFO(GASNETI_THREAD_PASS_ALONE);
        gex_TM_t   tm          = sd->_dest._request._tm;
        gex_Rank_t rank        = sd->_dest._request._rank;
        void *src_addr         = sd->_addr;
        gex_Event_t *lc_opt    = sd->_lc_opt ? sd->_lc_opt : /* GASNet-owned buffer: */ GEX_EVENT_NOW;
        gex_Flags_t flags      = sd->_flags & ~(GEX_FLAG_IMMEDIATE |
                                                GEX_FLAG_AM_PREPARE_LEAST_CLIENT |
                                                GEX_FLAG_AM_PREPARE_LEAST_ALLOC);
        unsigned int nargs     = sd->_nargs;

        int rc = gasneti_AMRequestMediumV(tm, rank, handler, src_addr, nbytes, lc_opt, flags, nargs, argptr);
        gasneti_assert(!rc); // IMMEDIATE is only permissible reason to return non-zero

        if (sd->_tofree) {
          gasneti_free_npam_buffer(sd);
        }
    }
    va_end(argptr);
}
#endif // GASNETC_BUILD_NP_REQ_MEDIUM

#if GASNETC_BUILD_NP_REP_MEDIUM
void gasnetc_AM_CommitReplyMediumM(
                       gex_AM_Index_t          handler,
                       size_t                  nbytes,
                     #if GASNET_DEBUG
                       unsigned int            nargs_arg,
                     #endif
                       gex_AM_SrcDesc_t        sd_arg, ...)
{
    // Conduit authors are cautioned against use of gasneti_consume_srcdesc() in native
    // NPAM implementations.  See the comment preceding its definition in gasnet_am.h.
    gasneti_AM_SrcDesc_t sd = gasneti_consume_srcdesc(sd_arg);

    GASNETI_COMMON_COMMIT_REP(sd,handler,nbytes,NULL,nargs_arg,Medium);

    va_list argptr;
    va_start(argptr, sd_arg);
    if (sd->_is_nbrhd) {
        gasnetc_nbrhd_CommitReply(sd, gasneti_Medium, handler, nbytes, NULL, argptr);
    } else {
        gex_Token_t token      = sd->_dest._reply._token;
        void *src_addr         = sd->_addr;
        gex_Event_t *lc_opt    = sd->_lc_opt ? sd->_lc_opt : /* GASNet-owned buffer: */ GEX_EVENT_NOW;
        gex_Flags_t flags      = sd->_flags & ~(GEX_FLAG_IMMEDIATE |
                                                GEX_FLAG_AM_PREPARE_LEAST_CLIENT |
                                                GEX_FLAG_AM_PREPARE_LEAST_ALLOC);
        unsigned int nargs     = sd->_nargs;

        int rc = gasneti_AMReplyMediumV(token, handler, src_addr, nbytes, lc_opt, flags, nargs, argptr);
        gasneti_assert(!rc); // IMMEDIATE is only permissible reason to return non-zero

        if (sd->_tofree) {
          gasneti_free_npam_buffer(sd);
        }
    }
    va_end(argptr);
}
#endif // GASNETC_BUILD_NP_REP_MEDIUM

#if GASNETC_BUILD_NP_REQ_LONG
void gasnetc_AM_CommitRequestLongM(
                       gex_AM_Index_t          handler,
                       size_t                  nbytes,
                       void                    *dest_addr
                       GASNETI_THREAD_FARG,
                     #if GASNET_DEBUG
                       unsigned int            nargs_arg,
                     #endif
                       gex_AM_SrcDesc_t        sd_arg, ...)
{
    // Conduit authors are cautioned against use of gasneti_consume_srcdesc() in native
    // NPAM implementations.  See the comment preceding its definition in gasnet_am.h.
    gasneti_AM_SrcDesc_t sd = gasneti_consume_srcdesc(sd_arg);

    GASNETI_COMMON_COMMIT_REQ(sd,handler,nbytes,dest_addr,nargs_arg,Long);

    va_list argptr;
    va_start(argptr, sd_arg);
    if (sd->_is_nbrhd) {
        gasnetc_nbrhd_CommitRequest(sd, gasneti_Long, handler, nbytes, dest_addr, argptr);
    } else {   GASNET_POST_THREADINFO(GASNETI_THREAD_PASS_ALONE);
        gex_TM_t   tm          = sd->_dest._request._tm;
        gex_Rank_t rank        = sd->_dest._request._rank;
        void *src_addr         = sd->_addr;
        gex_Event_t *lc_opt    = sd->_lc_opt ? sd->_lc_opt : /* GASNet-owned buffer: */ GEX_EVENT_NOW;
        gex_Flags_t flags      = sd->_flags & ~(GEX_FLAG_IMMEDIATE |
                                                GEX_FLAG_AM_PREPARE_LEAST_CLIENT |
                                                GEX_FLAG_AM_PREPARE_LEAST_ALLOC);
        unsigned int nargs     = sd->_nargs;

        int rc = gasneti_AMRequestLongV(tm, rank, handler, src_addr, nbytes, dest_addr, lc_opt, flags, nargs, argptr);
        gasneti_assert(!rc); // IMMEDIATE is only permissible reason to return non-zero

        if (sd->_tofree) {
          gasneti_free_npam_buffer(sd);
        }
    }
    va_end(argptr);
}
#endif // GASNETC_BUILD_NP_REQ_LONG

#if GASNETC_BUILD_NP_REP_LONG
void gasnetc_AM_CommitReplyLongM(
                       gex_AM_Index_t          handler,
                       size_t                  nbytes,
                       void                    *dest_addr,
                     #if GASNET_DEBUG
                       unsigned int            nargs_arg,
                     #endif
                       gex_AM_SrcDesc_t        sd_arg, ...)
{
    // Conduit authors are cautioned against use of gasneti_consume_srcdesc() in native
    // NPAM implementations.  See the comment preceding its definition in gasnet_am.h.
    gasneti_AM_SrcDesc_t sd = gasneti_consume_srcdesc(sd_arg);

    GASNETI_COMMON_COMMIT_REP(sd,handler,nbytes,dest_addr,nargs_arg,Long);

    va_list argptr;
    va_start(argptr, sd_arg);
    if (sd->_is_nbrhd) {
        gasnetc_nbrhd_CommitReply(sd, gasneti_Long, handler, nbytes, dest_addr, argptr);
    } else {
        gex_Token_t token      = sd->_dest._reply._token;
        void *src_addr         = sd->_addr;
        gex_Event_t *lc_opt    = sd->_lc_opt ? sd->_lc_opt : /* GASNet-owned buffer: */ GEX_EVENT_NOW;
        gex_Flags_t flags      = sd->_flags & ~(GEX_FLAG_IMMEDIATE |
                                                GEX_FLAG_AM_PREPARE_LEAST_CLIENT |
                                                GEX_FLAG_AM_PREPARE_LEAST_ALLOC);
        unsigned int nargs     = sd->_nargs;

        int rc = gasneti_AMReplyLongV(token, handler, src_addr, nbytes, dest_addr, lc_opt, flags, nargs, argptr);
        gasneti_assert(!rc); // IMMEDIATE is only permissible reason to return non-zero

        if (sd->_tofree) {
          gasneti_free_npam_buffer(sd);
        }
    }
    va_end(argptr);
}
#endif // GASNETC_BUILD_NP_REP_LONG

/* ------------------------------------------------------------------------------------ */

// gasneti_free_aligned() is a macro, preventing direct registration as a cleanupfn
void gasneti_medium_buffer_cleanup_threaddata(void *buf) {
  gasneti_free_aligned(buf);
}

extern gex_TI_t gasnetc_nbrhd_Token_Info(
                gex_Token_t         token,
                gex_Token_Info_t    *info,
                gex_TI_t            mask)
{
  gasneti_assert(token);
  gasneti_assert(info);

  *info = ((gasnetc_nbrhd_token_t *)(1^(uintptr_t)token))->ti;
  gex_TI_t result = GEX_TI_SRCRANK | GEX_TI_EP | GEX_TI_ENTRY | GEX_TI_IS_REQ | GEX_TI_IS_LONG;
  return GASNETI_TOKEN_INFO_RETURN(result, info, mask);
}

/* ------------------------------------------------------------------------------------ */

size_t gasneti_format_flags_amreg(char *buf, gex_Flags_t flags) {
  size_t rc = 1;
  if (buf) buf[0] = '\0';
  #define APPEND_TOKEN(string) do {    \
      if (buf) strcat(buf, string); \
      rc += strlen(string);         \
    } while (0)
  #define CHECK_ONE_FLAG(value) \
    if ((flags & GEX_FLAG_AM_##value) == GEX_FLAG_AM_##value) { APPEND_TOKEN(#value); }
  if (flags & GASNETI_FLAG_INIT_LEGACY) {
    APPEND_TOKEN("GASNet-1");
  } else if ((flags & GASNETI_FLAG_AM_ANY) == GASNETI_FLAG_AM_ANY) {
    APPEND_TOKEN("WILDCARD");
  } else {
    CHECK_ONE_FLAG(MEDLONG) else // must check first in this group
    CHECK_ONE_FLAG(SHORT)   else
    CHECK_ONE_FLAG(MEDIUM)  else
    CHECK_ONE_FLAG(LONG)
    APPEND_TOKEN("|");
    CHECK_ONE_FLAG(REQREP)  else // must check first in this group
    CHECK_ONE_FLAG(REQUEST) else
    CHECK_ONE_FLAG(REPLY)
  }
  #undef CHECK_ONE_FLAG
  #undef APPEND_TOKEN
  if (buf) gasneti_assert_uint(rc ,==, 1+strlen(buf));
  return rc;
}

