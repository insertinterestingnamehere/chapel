/*   $Source: bitbucket.org:berkeleylab/gasnet.git/ibv-conduit/gasnet_core_fwd.h $
 * Description: GASNet header for ibv conduit core (forward definitions)
 * Copyright 2002, Dan Bonachea <bonachea@cs.berkeley.edu>
 * Terms of use are as specified in license.txt
 */

#ifndef _IN_GASNETEX_H
  #error This file is not meant to be included directly- clients should include gasnetex.h
#endif

#ifndef _GASNET_CORE_FWD_H
#define _GASNET_CORE_FWD_H

#ifdef GASNET_CONDUIT_VAPI
  #error "VAPI-conduit is no longer supported"
#endif

#define GASNET_CORE_VERSION      2.16
#define GASNET_CORE_VERSION_STR  _STRINGIFY(GASNET_CORE_VERSION)
#define GASNET_CORE_NAME         IBV
#define GASNET_CORE_NAME_STR     _STRINGIFY(GASNET_CORE_NAME)
#define GASNET_CONDUIT_NAME      GASNET_CORE_NAME
#define GASNET_CONDUIT_NAME_STR  _STRINGIFY(GASNET_CONDUIT_NAME)
#define GASNET_CONDUIT_IBV       1

#define GASNETC_DEFAULT_SPAWNER  GASNETC_IBV_SPAWNER_CONF

// Client-facing indications of multirail support:
// GASNET_IBV_MULTIRAIL: 1/undef for enabled/disabled
// GASNET_IBV_MAX_HCAS: positive integer (1 when multirail disabled)
#if GASNETC_IBV_MAX_HCAS_CONFIGURE
  #define GASNET_IBV_MULTIRAIL 1
  #define GASNET_IBV_MAX_HCAS GASNETC_IBV_MAX_HCAS_CONFIGURE
#else
  #undef GASNET_IBV_MULTIRAIL
  #define GASNET_IBV_MAX_HCAS 1
#endif

#if defined(GASNET_SEGMENT_FAST)
  #define GASNETC_PIN_SEGMENT 1
#else
  #define GASNETC_PIN_SEGMENT 0
#endif

// Size of a buffer to contain any AM with all its header, padding and payload
#define GASNETC_BUFSZ GASNETC_IBV_MAX_MEDIUM

/* 16K is the limit on the LID space, but we must allow more than 1 proc per node */
/* 64K corresponds to 16 bits used in the AM Header and 16-bit gex_Rank_t */
#define GASNET_MAXNODES	65535

  /* GASNET_PSHM defined 1 if this conduit supports PSHM. leave undefined otherwise. */
/* As described in bug 3373, ibv_reg_mem() on Solaris only works with SYSV */
#if GASNETI_PSHM_ENABLED && !(PLATFORM_OS_SOLARIS && !GASNETI_PSHM_SYSV)
  #define GASNET_PSHM 1
#endif

// PSHM and loopback support need to know largest Medium if larger than MAX(LUB{Request,Reply}Medium)
#define GASNETC_MAX_MEDIUM_NBRHD GASNETC_BUFSZ

  /*  defined to be 1 if gasnet_init guarantees that the remote-access memory segment will be aligned  */
  /*  at the same virtual address on all nodes. defined to 0 otherwise */
#if GASNETI_DISABLE_ALIGNED_SEGMENTS || GASNET_PSHM
  #define GASNET_ALIGNED_SEGMENTS   0 /* user or PSHM disabled segment alignment */
#else
  #define GASNET_ALIGNED_SEGMENTS   1
#endif

  // If this conduit is considered a "portable conduit" only *conditionally*,
  // uncomment to enable calls to gasnetc_check_portable_conduit(void) as
  // described in gasnet_internal.c.
//#define GASNETC_CHECK_PORTABLE_CONDUIT_HOOK 1

  // uncomment for each MK_CLASS which the conduit supports. leave commented otherwise
#define GASNET_HAVE_MK_CLASS_CUDA_UVA (GASNETI_MK_CLASS_CUDA_UVA_ENABLED && GASNET_SEGMENT_FAST)
#define GASNET_HAVE_MK_CLASS_HIP (GASNETI_MK_CLASS_HIP_ENABLED && GASNET_SEGMENT_FAST)
//#define GASNET_HAVE_MK_CLASS_ZE GASNETI_MK_CLASS_ZE_ENABLED

  // define to 1 if your conduit has "private" thread(s) which can run AM handlers
#if GASNETC_IBV_RCV_THREAD
  #define GASNET_RCV_THREAD 1
#endif

  // define to 1 if your conduit has "private" thread(s) which progress sends of RMA and/or AM
#if GASNETC_IBV_SND_THREAD
  #define GASNET_SND_THREAD 1
#endif

#ifndef GASNETC_DYNAMIC_CONNECT
  #define GASNETC_DYNAMIC_CONNECT 1
#endif

#if GASNETC_IBV_RCV_THREAD || GASNETC_IBV_SND_THREAD || (GASNETC_DYNAMIC_CONNECT && GASNETC_IBV_CONN_THREAD)
  /* uncomment if your conduit has "private" threads which might run conduit
     code and/or the client's AM handlers, even under GASNET_SEQ.
     this ensures locking is still done correctly, etc
   */
  #define GASNETI_CONDUIT_THREADS 1
#endif

// Conduit-specific implementation of gex_System_QueryProgressThreads()
#if GASNETC_IBV_RCV_THREAD || GASNETC_IBV_SND_THREAD
  #define gex_System_QueryProgressThreads gasnetc_query_progress_threads
#endif

#if GASNETC_IBV_RCV_THREAD
  #define GASNET_HIDDEN_AM_CONCURRENCY_LEVEL 1
#else
  #define GASNET_HIDDEN_AM_CONCURRENCY_LEVEL 0
#endif

  /* define these to 1 if your conduit needs to augment the implementation
     of gasneti_reghandler() (in gasnet_internal.c)
   */
/* #define GASNETC_AMREGISTER 1 */

  /* define this to 1 if your conduit supports PSHM, but cannot use the
     default interfaces. (see template-conduit/gasnet_core.c and gasnet_pshm.h)
   */
/* #define GASNETC_GET_HANDLER 1 */

  /* uncomment each line for which your conduit supports the
     corresponding token info query.
  */
#define GASNET_SUPPORTS_TI_SRCRANK 1
#define GASNET_SUPPORTS_TI_EP 1
#define GASNET_SUPPORTS_TI_ENTRY 1
#define GASNET_SUPPORTS_TI_IS_REQ 1
#define GASNET_SUPPORTS_TI_IS_LONG 1

  /* uncomment for each {Request,Reply} X {Medium,Long} pair for which your
     conduit implements the corresponding gasnetc_AM_{Prepare,Commit}*() in
     a "native" manner which "can avoid one or more payload copies relative
     to the corresponding fixed-payload AM call under the right conditions".
     See also "GASNETC_BUILD_NP_*", immediately below.
   */
#define GASNET_NATIVE_NP_ALLOC_REQ_MEDIUM 1
#define GASNET_NATIVE_NP_ALLOC_REP_MEDIUM 1
#if GASNETC_PIN_SEGMENT
#define GASNET_NATIVE_NP_ALLOC_REQ_LONG 1
#define GASNET_NATIVE_NP_ALLOC_REP_LONG 1
#endif

  /* conduits may define to '1' (or '0') for {Request,Reply} X {Medium,Long}
     pairs to force (or prevent) compilation of the corresponding pieces of
     the conduit-independent reference implementation.
     If unset, the default is equivalent to '!GASNET_NATIVE_NP_ALLOC_[foo]'.
     In other words: by default each reference implementation is built if and
     only if the conduit is not claiming a "native" implementation.
     This default is correct for most conduits.

     The conduit-independent implementation works in terms of the internal
     functions gasnetc_AM{Request,Reply}{Medium,Long}V().  Therefore, your
     conduit must provide the V-suffixed functions for any case with the
     corresponding GASNETC_BUILD_NP_* equal to '1' (explicitly or by default).
   */
/* #define GASNETC_BUILD_NP_REQ_MEDIUM (###) */
/* #define GASNETC_BUILD_NP_REP_MEDIUM (###) */
/* #define GASNETC_BUILD_NP_REQ_LONG (###) */
/* #define GASNETC_BUILD_NP_REP_LONG (###) */

  /* uncomment for each conduit-provided Commit{Req,Rep}{Medium,Long}() which
     has the numargs argument even in an NDEBUG build (it is always passed in
     DEBUG builds).
   */
#define GASNETC_AM_COMMIT_REQ_MEDIUM_NARGS 1
#define GASNETC_AM_COMMIT_REP_MEDIUM_NARGS 1
#if GASNETC_PIN_SEGMENT
#define GASNETC_AM_COMMIT_REQ_LONG_NARGS 1
#define GASNETC_AM_COMMIT_REP_LONG_NARGS 1
#endif

#define GASNETI_AM_SRCDESC_EXTRA \
        int                 _have_flow;         \
        int                 _head_len;          \
        void *              _buf_alloc;         \
        void *              _cep;               \
        void *              _ep;                \
        uint8_t             _inline_buf[128+8];

  /* uncomment if your conduit's gasnetc_AMRequest{Short,Medium,Long}V()
     include a call to gasneti_AMPoll (or equivalent) for progress.
     The preferred implementation is to Poll only in the M-suffixed calls
     and not the V-suffixed calls (and GASNETC_REQUESTV_POLLS undefined).
     Used only by reference implementations (if any) of Prepare/Commit.
   */
/* #define GASNETC_REQUESTV_POLLS 1 */

  /* If your conduit uses conduit-specific extensions to the basic object
     types, then define the corresponding SIZEOF macros below to return
     the total length of the conduit-specific object, including the prefix
     portion which must be the matching GASNETI_[OBJECT]_COMMON fields.
     Similarly, *_HOOK macros should be defined as callbacks to perform
     conduit-specific initialization and finalization tasks, if any.
     If a given SIZEOF macro is defined, but the corresponding INIT_HOOK is
     not, then space beyond the COMMON fields will be zero-initialized.
     In all cases, GASNETC_[OBJECT]_EXTRA_DECLS provides the place to
     provide necessary declarations (since this file is included very early).
    */

//#define GASNETC_CLIENT_EXTRA_DECLS (###)
//#define GASNETC_CLIENT_INIT_HOOK(i_client) (###)
//#define GASNETC_CLIENT_FINI_HOOK(i_client) (###)
//#define GASNETC_SIZEOF_CLIENT_T() (###)

#define GASNETC_SEGMENT_EXTRA_DECLS \
  extern size_t gasnetc_sizeof_segment_t(void);
//#define GASNETC_SEGMENT_INIT_HOOK(i_segment) (###)
//#define GASNETC_SEGMENT_FINI_HOOK(i_segment) (###)
#define GASNETC_SIZEOF_SEGMENT_T() \
  gasnetc_sizeof_segment_t()

//#define GASNETC_TM_EXTRA_DECLS (###)
//#define GASNETC_TM_INIT_HOOK(i_tm) (###)
//#define GASNETC_TM_FINI_HOOK(i_tm) (###)
//#define GASNETC_SIZEOF_TM_T() (###)

#define GASNETC_EP_EXTRA_DECLS \
  extern size_t gasnetc_sizeof_ep_t(void); \
  extern int gasnetc_ep_init_hook(gasneti_EP_t);
#define GASNETC_EP_INIT_HOOK(i_ep) \
    gasnetc_ep_init_hook(i_ep)
//#define GASNETC_EP_FINI_HOOK(i_ep) (###)
#define GASNETC_SIZEOF_EP_T() \
  gasnetc_sizeof_ep_t()

  // Uncomment the following defines if conduit provides the corresponding hook.
  // See gasnet_internal.h for prototypes and brief descriptions.
#define GASNETC_SEGMENT_ATTACH_HOOK 1
#define GASNETC_SEGMENT_CREATE_HOOK 1
#define GASNETC_SEGMENT_DESTROY_HOOK 1
//#define GASNETC_EP_BINDSEGMENT_HOOK 1
#define GASNETC_EP_PUBLISHBOUNDSEGMENT_HOOK 1

  // Uncomment the following defines if conduit provides the corresponding hook.
  // See other/kinds/gasnet_kinds_internal.h for prototypes and brief descriptions.
//#define GASNETC_MK_CREATE_HOOK 1
//#define GASNETC_MK_DESTROY_HOOK 1

#if GASNETC_PIN_SEGMENT // multi-EP NOT supported with remote firehose
// If conduit supports GASNET_MAXEPS!=1, set default and (optional) max values here.
// Leaving GASNETC_MAXEPS_DFLT unset will result in GASNET_MAXEPS=1, independent
// of all other settings (appropriate for conduits without multi-ep support).
// If set, GASNETC_MAXEPS_MAX it is used to limit a user's --with-maxeps (and a
// global default limit is used otherwise).
#define GASNETC_MAXEPS_DFLT 33 // Initial (limited) multi-EP support
//#define GASNETC_MAXEPS_MAX ### // leave unset for default
#endif

  /* this can be used to add conduit-specific 
     statistical collection values (see gasnet_trace.h) */
#define GASNETC_CONDUIT_STATS(CNT,VAL,TIME)       \
        CNT(C, RCV_AM, cnt)                       \
        VAL(C, RDMA_PUT_IN_MOVE, bytes)           \
        VAL(C, RDMA_PUT_INLINE, bytes)            \
        VAL(C, RDMA_PUT_BOUNCE, bytes)            \
        VAL(C, RDMA_PUT_ZEROCP, bytes)            \
        VAL(C, RDMA_PUT_READONLY, bytes)          \
        VAL(C, RDMA_GET_BOUNCE, bytes)            \
        VAL(C, RDMA_GET_ZEROCP, bytes)            \
        CNT(C, ALLOC_AM_SPARE, cnt)	          \
        CNT(C, GET_AMREQ_CREDIT, cnt)             \
	TIME(C, GET_AMREQ_CREDIT_STALL, stalled time) \
	TIME(C, GET_AMREQ_BUFFER_STALL, stalled time) \
	CNT(C, GET_BBUF, cnt)                     \
	TIME(C, GET_BBUF_STALL, stalled time)     \
	CNT(C, SPARE_REPLY_BBUF, cnt)             \
	VAL(C, ALLOC_SREQ, sreqs)                 \
	VAL(C, POST_SR, segments)                 \
	CNT(C, POST_INLINE_SR, cnt)               \
	TIME(C, POST_SR_STALL_CQ, stalled time)   \
	TIME(C, POST_SR_STALL_SQ, stalled time)   \
	TIME(C, POST_SR_STALL_SQ2, stalled time)  \
	CNT(C, POST_SR_SPLIT, cnt)                \
	VAL(C, POST_SR_LIST, requests)            \
	CNT(C, SND_REAP_THR, cnt)                 \
	VAL(C, SND_REAP, reaped)                  \
	VAL(C, RCV_REAP, reaped)                  \
	CNT(C, CONN_STATIC, peers)                \
	CNT(C, CONN_DYNAMIC, peers)               \
	TIME(C, CONN_TIME_ACTV, active connect time) \
	TIME(C, CONN_TIME_PASV, passive connect time) \
	TIME(C, CONN_TIME_A2P, active-became-passive connect time) \
	TIME(C, CONN_REQ2REP, REQ-to-REP delay)   \
	TIME(C, CONN_RTU2ACK, RTU-to-ACK delay)   \
	VAL(C, CONN_REQ, resends)                 \
	VAL(C, CONN_RTU, resends)                 \
	CNT(C, CONN_REP, sent)                    \
	CNT(C, CONN_NOREP, not sent)              \
	CNT(C, CONN_ACK, sent)                    \
	CNT(C, CONN_NOACK, not sent)              \
	CNT(C, CONN_AAA, remained Active)         \
	CNT(C, CONN_AAP, became Passive)          \
	CNT(C, CONN_IMPLIED_ACK, cnt)             \
	TIME(C, CONN_STALL_CQ, stalled time)      \
	TIME(C, CONN_STALL_DESC, stalled time)    \
	TIME(C, FIREHOSE_MOVE, processing time)   \
	VAL(C, FIREHOSE_PIN, pages)               \
	VAL(C, FIREHOSE_UNPIN, pages)

#define GASNETC_FATALSIGNAL_CALLBACK(sig) gasnetc_fatalsignal_callback(sig)
	extern void gasnetc_fatalsignal_callback(int _sig);

#if GASNETC_IBV_ODP
  #define GASNETC_FATALSIGNAL_CLEANUP_CALLBACK(sig) gasnetc_fatalsignal_cleanup_callback(sig)
  extern void gasnetc_fatalsignal_cleanup_callback(int _sig);
#endif

#if PLATFORM_OS_DARWIN && !GASNET_SEQ
  #define GASNETC_PTHREAD_CREATE_OVERRIDE(create_fn, thread, attr, start_routine, arg) \
	gasnetc_pthread_create(create_fn, thread, attr, start_routine, arg)
#endif

/* ------------------------------------------------------------------------------------ */
/* handler table access for PSHM (temporary global impl until PSHM can pass actual ep) */
#define GASNETC_GET_HANDLER 1
#define gasnetc_get_hentry(_ep,_index) (&gasnetc_ep0->_amtbl[(_index)])
#define gasnetc_get_handler(_ep,_index,_field) (gasnetc_get_hentry((_ep),(_index))->gex_##_field)

#endif
