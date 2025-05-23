/*   $Source: bitbucket.org:berkeleylab/gasnet.git/udp-conduit/gasnet_core.c $
 * Description: GASNet UDP conduit Implementation
 * Copyright 2002, Dan Bonachea <bonachea@cs.berkeley.edu>
 * Terms of use are as specified in license.txt
 */

#include <gasnet_internal.h>
#include <gasnet_core_internal.h>
#include <gasnet_am.h>

#include <amudp_spmd.h>

#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>

GASNETI_IDENT(gasnetc_IdentString_Version, "$GASNetCoreLibraryVersion: " GASNET_CORE_VERSION_STR " $");
GASNETI_IDENT(gasnetc_IdentString_Name,    "$GASNetCoreLibraryName: " GASNET_CORE_NAME_STR " $");

gex_AM_Entry_t *gasnetc_handler; // TODO-EX: will be replaced with per-EP tables

static void gasnetc_traceoutput(int);

static uint64_t gasnetc_networkpid;
eb_t gasnetc_bundle;
ep_t gasnetc_endpoint;

gasneti_mutex_t gasnetc_AMlock = GASNETI_MUTEX_INITIALIZER; /*  protect access to AMUDP */
volatile int gasnetc_AMLockYield = 0;

#if GASNET_TRACE || GASNET_DEBUG
  extern void gasnetc_enteringHandler_hook(amudp_category_t cat, int isReq, int handlerId, void *token, 
                                         void *buf, size_t nbytes, int numargs, uint32_t *args);
  extern void gasnetc_leavingHandler_hook(amudp_category_t cat, int isReq);
#endif

#if defined(GASNET_CSPAWN_CMD)
  #define GASNETC_DEFAULT_SPAWNFN C
  GASNETI_IDENT(gasnetc_IdentString_Default_CSpawnCommand, "$GASNetCSpawnCommand: " GASNET_CSPAWN_CMD " $");
#else /* AMUDP implicit ssh startup */
  #define GASNETC_DEFAULT_SPAWNFN S
#endif
GASNETI_IDENT(gasnetc_IdentString_DefaultSpawnFn, "$GASNetDefaultSpawnFunction: " _STRINGIFY(GASNETC_DEFAULT_SPAWNFN) " $");

/* ------------------------------------------------------------------------------------ */
/*
  Initialization
  ==============
*/
/* called at startup to check configuration sanity */
static void gasnetc_check_config(void) {
  gasneti_check_config_preinit();

  gasneti_static_assert(GASNET_MAXNODES <= AMUDP_MAX_SPMDPROCS);
  gasneti_static_assert(AMUDP_MAX_NUMHANDLERS >= 256);
  gasneti_static_assert(AMUDP_MAX_SEGLENGTH == (uintptr_t)-1);

  gasneti_static_assert(GASNET_ERR_NOT_INIT == AM_ERR_NOT_INIT);
  gasneti_static_assert(GASNET_ERR_RESOURCE == AM_ERR_RESOURCE);
  gasneti_static_assert(GASNET_ERR_BAD_ARG  == AM_ERR_BAD_ARG);
}

void gasnetc_bootstrapBarrier(void) {
   int retval;
   AM_ASSERT_LOCKED(); /* need this because SPMDBarrier may poll */
   GASNETI_AM_SAFE_NORETURN(retval,AMUDP_SPMDBarrier());
   if_pf (retval) gasneti_fatalerror("failure in gasnetc_bootstrapBarrier()");
}

void gasnetc_bootstrapExchange(void *src, size_t len, void *dest) {
  int retval;
  GASNETI_AM_SAFE_NORETURN(retval,AMUDP_SPMDAllGather(src, dest, len));
  if_pf (retval) gasneti_fatalerror("failure in gasnetc_bootstrapExchange()");
}

#if GASNET_PSHM // Currently used only in call to gasneti_pshm_init()
// Naive (poorly scaling) "reference" SubsetBroadcast via AMUDP_SPMDAllGather()
// Since every caller extracts the desired rootnode's contribution from an
// AllGather, the NbrhdBroadcast and HostBroadcast are identical.
static void gasnetc_bootstrapSubsetBroadcast(void *src, size_t len, void *dest, int rootnode) {
  void *tmp = gasneti_malloc(len * gasneti_nodes);
  gasneti_assert(NULL != src);
  if (gasneti_mynode != rootnode) {
     // silence a harmless valgrind error caused by sending potentially uninitialized bytes
     memset(src, 0, len);
  }
  gasnetc_bootstrapExchange(src, len, tmp);
  GASNETI_MEMCPY(dest, (void*)((uintptr_t)tmp + (len * rootnode)), len);
  gasneti_free(tmp);
}
#define gasnetc_bootstrapNbrhdBroadcast gasnetc_bootstrapSubsetBroadcast
#define gasnetc_bootstrapHostBroadcast gasnetc_bootstrapSubsetBroadcast
#endif

#define INITERR(type, reason) do {                                      \
   if (gasneti_VerboseErrors) {                                         \
     gasneti_console_message("ERROR","GASNet initialization encountered an error: %s\n" \
      "  in %s at %s:%i",                                               \
      #reason, GASNETI_CURRENT_FUNCTION,  __FILE__, __LINE__);          \
   }                                                                    \
   retval = GASNET_ERR_ ## type;                                        \
   goto done;                                                           \
 } while (0)

static int gasnetc_init(
                               gex_Client_t            *client_p,
                               gex_EP_t                *ep_p,
                               gex_TM_t                *tm_p,
                               const char              *clientName,
                               int                     *argc,
                               char                    ***argv,
                               gex_Flags_t             flags)
{
  int retval = GASNET_OK;

  /*  check system sanity */
  gasnetc_check_config();

  /* --------- begin Master code ------------ */
  if (!AMUDP_SPMDIsWorker(argv?*argv:NULL)) {
    /* assume we're an implicit master 
       (we don't currently support explicit workers spawned 
        without using the AMUDP SPMD API)   
     */
    int num_nodes;
    int i;
    char spawnfn;
    amudp_spawnfn_t fp = (amudp_spawnfn_t)NULL;

    if (!argv) {
      gasneti_fatalerror("implicit-master without argv not supported - use amudprun");
    }

    /* pretend we're node 0, for purposes of verbose env reporting */
    gasneti_init_done = 1;
    gasneti_mynode = 0;

    #if defined(GASNET_CSPAWN_CMD)
    { /* set configure default cspawn cmd */
      const char *cmd = gasneti_getenv_withdefault("GASNET_CSPAWN_CMD",GASNET_CSPAWN_CMD);
      gasneti_setenv("GASNET_CSPAWN_CMD",cmd);
    }
    #endif

    /* parse node count from command line */
    if (*argc < 2) {
      gasneti_console0_message("GASNet","Missing parallel node count");
      gasneti_console0_message("GASNet","Specify node count as first argument, or use programming model spawn script to start job");
      gasneti_console0_message("GASNet","Usage '%s <num_nodes> {program arguments}'", (*argv)[0]);
      exit(-1);
    }
    /*
     * argv[1] is number of nodes; argv[0] is program name; argv is
     * list of arguments including program name and number of nodes.
     * We need to remove argv[1] when the argument array is passed
     * to the tic_main().
     */
    num_nodes = atoi((*argv)[1]);
    if (num_nodes < 1) {
      gasneti_console0_message("GASNet","Invalid number of nodes: %s", (*argv)[1]);
      gasneti_console0_message("GASNet","Usage '%s <num_nodes> {program arguments}'", (*argv)[0]);
      exit (1);
    }

    /* remove the num_nodes argument */
    for (i = 1; i < (*argc)-1; i++) {
      (*argv)[i] = (*argv)[i+1];
    }
    (*argv)[(*argc)-1] = NULL;
    (*argc)--;

    /* get spawnfn */
    spawnfn = *gasneti_getenv_withdefault("GASNET_SPAWNFN", _STRINGIFY(GASNETC_DEFAULT_SPAWNFN));

    { /* ensure we pass the effective spawnfn to worker env */
      char spawnstr[2];
      spawnstr[0] = toupper(spawnfn);
      spawnstr[1] = '\0';
      gasneti_setenv("GASNET_SPAWNFN",spawnstr);
    }

    /* ensure reliable localhost operation by forcing use of 127.0.0.1
     * setting GASNET_MASTERIP to the empty string will prevent this */
    if (('L' == toupper(spawnfn)) && !gasneti_getenv("GASNET_MASTERIP")) {
      gasneti_setenv("GASNET_MASTERIP","127.0.0.1");
    }

    for (i=0; AMUDP_Spawnfn_Desc[i].abbrev; i++) {
      if (toupper(spawnfn) == toupper(AMUDP_Spawnfn_Desc[i].abbrev)) {
        fp = AMUDP_Spawnfn_Desc[i].fnptr;
        break;
      }
    }

    if (!fp) {
      gasneti_console0_message("GASNet","Invalid spawn function specified in GASNET_SPAWNFN");
      gasneti_console0_message("GASNet","The following mechanisms are available:");
      for (i=0; AMUDP_Spawnfn_Desc[i].abbrev; i++) {
        gasneti_console0_message("GASNet","    '%c'  %s\n",  
              toupper(AMUDP_Spawnfn_Desc[i].abbrev), AMUDP_Spawnfn_Desc[i].desc);
      }
      exit(1);
    }

    #if GASNET_DEBUG_VERBOSE
      gasneti_console_message("gasnetc_init","about to spawn..."); 
    #endif

    retval = AMUDP_SPMDStartup(argc, argv, 
      num_nodes, 0, fp,
      NULL, &gasnetc_bundle, &gasnetc_endpoint);
    /* master startup should never return */
    gasneti_fatalerror("master AMUDP_SPMDStartup() failed");
  }

  /* --------- begin Worker code ------------ */
  AMLOCK();
    if (gasneti_init_done) 
      INITERR(NOT_INIT, "GASNet already initialized");

    AMX_VerboseErrors = gasneti_VerboseErrors;
    AMUDP_SPMDkillmyprocess = gasneti_killmyprocess;

#if GASNETI_CALIBRATE_TSC
    // Early x86*/Linux timer initialization before AMUDP_SPMDStartup()
    //
    // udp-conduit does not support user-provided values for GASNET_TSC_RATE*
    // (which fine-tune timer calibration on x86/Linux).  This is partially due
    // to a dependency cycle at startup with envvar propagation, but more
    // importantly because the retransmission algorithm (and hence all conduit
    // comms) rely on gasnet timers to be accurate (at least approximately), so
    // we don't allow the user to weaken or disable their calibration.
    gasneti_unsetenv("GASNET_TSC_RATE");
    gasneti_unsetenv("GASNET_TSC_RATE_TOLERANCE");
    gasneti_unsetenv("GASNET_TSC_RATE_HARD_TOLERANCE");
    GASNETI_TICKS_INIT();
#endif

    /*  perform job spawn */
    retval = AMUDP_SPMDStartup(argc, argv, 
      0, 0, NULL, /* dummies */
      &gasnetc_networkpid, &gasnetc_bundle, &gasnetc_endpoint);
    if (retval != AM_OK) INITERR(RESOURCE, "worker AMUDP_SPMDStartup() failed");
    gasneti_init_done = 1; /* enable early to allow tracing */

    gasneti_getenv_hook = (/* cast drops const */ gasneti_getenv_fn_t*)&AMUDP_SPMDgetenvMaster;
    gasneti_check_env_prefix_hook = &AMUDP_check_env_prefix;
    gasneti_mynode = AMUDP_SPMDMyProc();
    gasneti_nodes = AMUDP_SPMDNumProcs();

    gasneti_freezeForDebugger(); // must come after getenv_hook is set

#if !GASNETI_CALIBRATE_TSC
    /* Must init timers after global env, and preferably before tracing */
    GASNETI_TICKS_INIT();
#endif

    /* enable tracing */
    gasneti_trace_init(argc, argv);
    GASNETI_AM_SAFE(AMUDP_SPMDSetExitCallback(gasnetc_traceoutput));

    /* for local spawn, assume we want to wait-block */
    if (gasneti_getenv("GASNET_SPAWNFN") && *gasneti_getenv("GASNET_SPAWNFN") == 'L') { 
      GASNETI_TRACE_PRINTF(C,("setting gasnet_set_waitmode(GASNET_WAIT_BLOCK) for localhost spawn"));
      gasnet_set_waitmode(GASNET_WAIT_BLOCK);
    }

    gasneti_spawn_verbose = gasneti_getenv_yesno_withdefault("GASNET_SPAWN_VERBOSE",0);

    if (gasneti_spawn_verbose) {
      gasneti_console_message("gasnetc_init","spawn successful - proc %i/%i starting...",
        gasneti_mynode, gasneti_nodes);
    }

    // Note intentional lack of env var tracing when just check for deprecated use
    if (gasneti_getenv("GASNET_USE_GETHOSTID") && !gasneti_getenv("GASNET_HOST_DETECT")) {
      // Legacy behavior: GASNET_USE_GETHOSTID demands use of gasneti_gethostid(),
      // but we ignore GASNET_USE_GETHOSTID if GASNET_HOST_DETECT is set.
      if (!gasneti_mynode) {
        gasneti_console_message("WARNING","GASNET_USE_GETHOSTID is deprecated.  "
                                          "Use GASNET_HOST_DETECT instead.");
      }
      if (gasneti_getenv_yesno_withdefault("GASNET_USE_GETHOSTID", 0)) {
        gasneti_setenv("GASNET_HOST_DETECT", "gethostid");
      }
    }
    {
      // Use (hash of) hostname and the local IP address to construct the nodemap
      // when GASNET_HOST_DETECT == "conduit"
      uint64_t local_id;
      en_t my_name;
      GASNETI_AM_SAFE( AM_GetTranslationName(gasnetc_endpoint, gasneti_mynode, &my_name) );
      uint64_t csum = gasneti_hosthash();
      local_id = GASNETI_MAKEWORD(GASNETI_HIWORD(csum) ^ GASNETI_LOWORD(csum),
                                  *(uint32_t *)(&my_name.sin_addr));
      gasneti_nodemapInit(&gasnetc_bootstrapExchange, &local_id, sizeof(local_id), 0);
    }

    #if GASNET_PSHM
      gasneti_pshm_init(&gasnetc_bootstrapNbrhdBroadcast, 0);
    #endif

    //  Create first Client, EP and TM *here*, for use in subsequent bootstrap communication
    {
      //  allocate the client object
      gasneti_Client_t client = gasneti_alloc_client(clientName, flags);
      *client_p = gasneti_export_client(client);

      //  create the initial endpoint with internal handlers
      if (gex_EP_Create(ep_p, *client_p, GEX_EP_CAPABILITY_ALL, flags))
        GASNETI_RETURN_ERRR(RESOURCE,"Error creating initial endpoint");
      gasneti_EP_t ep = gasneti_import_ep(*ep_p);
      gasnetc_handler = ep->_amtbl; // TODO-EX: this global variable to be removed

      //  create the tm
      gasneti_TM_t tm = gasneti_alloc_tm(ep, gasneti_mynode, gasneti_nodes, flags);
      *tm_p = gasneti_export_tm(tm);
    }

    gasnetc_bootstrapBarrier();
    gasneti_attach_done = 1; // Ready to use AM Short and Medium for bootstrap comms

    uintptr_t mmap_limit;
    #if HAVE_MMAP
    {
    AMUNLOCK();
      // Bound per-host (sharedLimit) argument to gasneti_segmentLimit()
      // while properly reserving space for aux segments.
      uint64_t sharedLimit = gasneti_sharedLimit();
      uint64_t hostAuxSegs = gasneti_myhost.node_count * gasneti_auxseg_preinit();
      if (sharedLimit <= hostAuxSegs) {
        gasneti_fatalerror("per-host segment limit %"PRIu64" is too small to accommodate %i aux segments, "
                           "total size %"PRIu64". You may need to adjust OS shared memory limits.",
                           sharedLimit, gasneti_myhost.node_count, hostAuxSegs);
      }
      sharedLimit -= hostAuxSegs;

      mmap_limit = gasneti_segmentLimit((uintptr_t)-1, sharedLimit, NULL, NULL);
    AMLOCK();
    }
    #else
      // TODO-EX: we can at least look at rlimits but such logic belongs in conduit-indep code
      mmap_limit = (intptr_t)-1;
    #endif

    /* allocate and attach an aux segment */

    (void) gasneti_auxsegAttach((uintptr_t)-1, &gasnetc_bootstrapExchange);

    /* determine Max{Local,GLobal}SegmentSize */
    gasneti_segmentInit(mmap_limit, &gasnetc_bootstrapExchange, flags);

  AMUNLOCK();

  gasneti_assert(retval == GASNET_OK);
  return retval;

done: /*  error return while locked */
  AMUNLOCK();
  GASNETI_RETURN(retval);
}

/* ------------------------------------------------------------------------------------ */
extern void _gasnetc_set_waitmode(int wait_mode) {
  if (wait_mode == GASNET_WAIT_BLOCK) {
    AMUDP_PoliteSync = 1;
  } else {
    AMUDP_PoliteSync = 0;
  }
}
/* ------------------------------------------------------------------------------------ */
extern int gasnetc_amregister(gex_AM_Index_t index, gex_AM_Entry_t *entry) {
  // NOTE: we do not currently attempt to hold the AMLOCK
  if (AM_SetHandler(gasnetc_endpoint, (handler_t)index, entry->gex_fnptr) != AM_OK)
    GASNETI_RETURN_ERRR(RESOURCE, "AM_SetHandler() failed while registering handlers");
  return GASNET_OK;
}
/* ------------------------------------------------------------------------------------ */
static gasnet_seginfo_t myseg;
extern int gasnetc_attach_primary(gex_Flags_t flags) {
  int retval = GASNET_OK;

  AMLOCK();
    /* pause to make sure all nodes have called attach 
       if a node calls gasnet_exit() between init/attach, then this allows us
       to process the AMUDP_SPMD control messages required for job shutdown
     */
    gasnetc_bootstrapBarrier();

    /* ------------------------------------------------------------------------------------ */
    /*  register fatal signal handlers */

    /* catch fatal signals and convert to SIGQUIT */
    gasneti_registerSignalHandlers(gasneti_defaultSignalHandler);

    // register process exit-time hook
    gasneti_registerExitHandler(gasnetc_exit);

    #if GASNET_TRACE || GASNET_DEBUG
     #if !GASNET_DEBUG
      if (GASNETI_TRACE_ENABLED(A))
     #endif
        GASNETI_AM_SAFE(AMUDP_SetHandlerCallbacks(gasnetc_endpoint,
          gasnetc_enteringHandler_hook, gasnetc_leavingHandler_hook));
    #endif

    // register all of memory as the AMX-level segment
    // this is needed for multi-segment support (aux + client at a minimum)
    retval = AM_SetSeg(gasnetc_endpoint, NULL, (uintptr_t)-1);
    if (retval != AM_OK) INITERR(RESOURCE, "AM_SetSeg() failed");

    /* ------------------------------------------------------------------------------------ */
    /*  primary attach complete */
    gasneti_attach_done = 1;
    gasnetc_bootstrapBarrier();
  AMUNLOCK();

  GASNETI_TRACE_PRINTF(C,("gasnetc_attach_primary(): primary attach complete\n"));

  gasnete_init(); /* init the extended API */

  gasneti_nodemapFini();

  /* ensure extended API is initialized across nodes */
  AMLOCK();
    gasnetc_bootstrapBarrier();
  AMUNLOCK();

  gasneti_assert(retval == GASNET_OK);
  return retval;

done: /*  error return while locked */
  AMUNLOCK();
  GASNETI_RETURN(retval);
}
/* ------------------------------------------------------------------------------------ */
// TODO-EX: this is a candidate for factorization (once we understand the per-conduit variations)
extern int gasnetc_Client_Init(
                               gex_Client_t            *client_p,
                               gex_EP_t                *ep_p,
                               gex_TM_t                *tm_p,
                               const char              *clientName,
                               int                     *argc,
                               char                    ***argv,
                               gex_Flags_t             flags)
{
  gasneti_assert(client_p);
  gasneti_assert(ep_p);
  gasneti_assert(tm_p);
  gasneti_assert(clientName);
#if !GASNET_NULL_ARGV_OK
  gasneti_assert(argc);
  gasneti_assert(argv);
#endif

  //  main init
  // TODO-EX: must split off per-client and per-endpoint portions
  if (!gasneti_init_done) { // First client
    // NOTE: gasnetc_init() creates the first Client, EP and TM for use in bootstrap comms
    int retval = gasnetc_init(client_p, ep_p, tm_p, clientName, argc, argv, flags);
    if (retval != GASNET_OK) GASNETI_RETURN(retval);
  #if 0
    /* called within gasnetc_init to allow init tracing */
    gasneti_trace_init(argc, argv);
  #endif
  } else {
    gasneti_fatalerror("No multi-client support");
  }

  // Do NOT move this prior to the gasneti_trace_init() call
  GASNETI_TRACE_PRINTF(O,("gex_Client_Init: name='%s' argc_p=%p argv_p=%p flags=%d",
                          clientName, (void *)argc, (void *)argv, flags));

  if (0 == (flags & GASNETI_FLAG_INIT_LEGACY)) {
    /*  primary attach  */
    if (GASNET_OK != gasnetc_attach_primary(flags))
      GASNETI_RETURN_ERRR(RESOURCE,"Error in primary attach");

    /* ensure everything is initialized across all nodes */
    gasnet_barrier(0, GASNET_BARRIERFLAG_UNNAMED);
  } else {
    gasneti_attach_done = 0; // Pending client call to gasnet_attach()
  }

  return GASNET_OK;
}
/* ------------------------------------------------------------------------------------ */
static int gasnetc_exitcalled = 0;
static void gasnetc_traceoutput(int exitcode) {
  if (!gasnetc_exitcalled) {
    gasneti_flush_streams();
    gasneti_trace_finish();
  }
}
extern void gasnetc_stats_dump(int reset) {
  /* dump AMUDP statistics */
  if (GASNETI_STATS_ENABLED(C) || reset) {
    const char *statdump;
    int isglobal = 0;
    int retval = 0;
    amudp_stats_t stats = AMUDP_initial_stats;

    /* bug 2181 - lock state is unknown, eg we may be in handler context */
    int shouldunlock;
    AMLOCK_CAUTIOUS(shouldunlock);

    if (isglobal) {
      /* TODO: tricky bit - if this exit is collective, we can display more interesting and useful
         statistics with collective cooperation. But there's no easy way to know for sure whether
         the exit is collective.
       */

      /* TODO: want a bootstrap barrier here for global stats to ensure network is 
         quiescent, but no way to do this unless we know things are collective */

      if (gasneti_mynode != 0) {
          GASNETI_AM_SAFE_NORETURN(retval, AMUDP_GetEndpointStatistics(gasnetc_endpoint, &stats)); /* get statistics */
        /* TODO: send stats to zero */
      } else {
        amudp_stats_t *remote_stats = NULL;
        /* TODO: gather stats from all nodes */
          GASNETI_AM_SAFE_NORETURN(retval, AMUDP_AggregateStatistics(&stats, remote_stats));
      }
    } else {
        GASNETI_AM_SAFE_NORETURN(retval, AMUDP_GetEndpointStatistics(gasnetc_endpoint, &stats)); /* get statistics */
    }
    if (reset && !retval) 
      GASNETI_AM_SAFE_NORETURN(retval, AMUDP_ResetEndpointStatistics(gasnetc_endpoint));

    if (GASNETI_STATS_ENABLED(C) && (gasneti_mynode == 0 || !isglobal) && !retval) {
      GASNETI_STATS_PRINTF(C,("--------------------------------------------------------------------------------"));
      GASNETI_STATS_PRINTF(C,("AMUDP Statistics:"));
      if (!isglobal)
        GASNETI_STATS_PRINTF(C,("*** AMUDP stat dump reflects only local node info, because gasnet_exit is non-collective ***"));
      statdump = AMUDP_DumpStatistics(NULL, &stats, isglobal);
      GASNETI_STATS_PRINTF(C,("\n%s",statdump)); /* note, dump has embedded '%' chars */
      GASNETI_STATS_PRINTF(C,("--------------------------------------------------------------------------------"));
    }
    if (shouldunlock) AMUNLOCK();
  }
}
extern void gasnetc_fatalsignal_callback(int sig) {
  if (gasnetc_exitcalled) {
  /* if we get a fatal signal during exit, it's almost certainly a signal-safety or UDP shutdown
     issue and not a client bug, so don't bother reporting it verbosely, 
     just die silently
   */
    #if 0
      gasneti_fatalerror("gasnetc_fatalsignal_callback aborting...");
    #endif
    gasneti_killmyprocess(1);
  }
}

extern void gasnetc_exit(int exitcode) {
  /* once we start a shutdown, ignore all future SIGQUIT signals or we risk reentrancy */
  gasneti_reghandler(SIGQUIT, SIG_IGN);
  gasnetc_exitcalled = 1;

  {  /* ensure only one thread ever continues past this point */
    static gasneti_mutex_t exit_lock = GASNETI_MUTEX_INITIALIZER;
    gasneti_mutex_lock(&exit_lock);
  }

  if (gasneti_spawn_verbose) gasneti_console_message("EXIT STATE","gasnet_exit(%i)",exitcode);
  else GASNETI_TRACE_PRINTF(C,("gasnet_exit(%i)\n", exitcode));

  gasneti_flush_streams();
  gasneti_trace_finish();
  gasneti_sched_yield();

  /* bug2181: try to prevent races where we exit while other local pthreads are in AMUDP
     can't use a blocking lock here, because may be in a signal context
  */
  int dummy;
  AMLOCK_CAUTIOUS(dummy);

  AMUDP_SPMDExit(exitcode);
  gasneti_fatalerror("AMUDP_SPMDExit failed!");
}

/* ------------------------------------------------------------------------------------ */
/*
  Misc. Active Message Functions
  ==============================
*/
#if GASNET_PSHM
/* (###) GASNETC_GET_HANDLER
 *   If your conduit will support PSHM, then there needs to be a way
 *   for PSHM to see your handler table.  If you use the recommended
 *   implementation then you don't need to do anything special.
 *   Othwerwise, #define GASNETC_GET_HANDLER in gasnet_core_fwd.h and
 *   implement gasnetc_get_handler() as a macro in
 *   gasnet_core_internal.h
 */
#endif

GASNETI_INLINE(gasnetc_msgsource)
gex_Rank_t gasnetc_msgsource(gex_Token_t token) {
  #if GASNET_PSHM
    gasneti_assert(! gasnetc_token_in_nbrhd(token));
  #endif
    gasneti_assert(token);

    int tmp; /* AMUDP wants an int, but gex_Rank_t is uint32_t */
    gasneti_assert_zeroret(AMUDP_GetSourceId(token, &tmp));
    gasneti_assert(tmp >= 0);
    gex_Rank_t sourceid = tmp;
    gasneti_assert_uint(sourceid ,<, gasneti_nodes);
    return sourceid;
}

extern gex_TI_t gasnetc_Token_Info(
                gex_Token_t         token,
                gex_Token_Info_t    *info,
                gex_TI_t            mask)
{
  gasneti_assert(token);
  gasneti_assert(info);

  if (gasnetc_token_in_nbrhd(token)) {
    return gasnetc_nbrhd_Token_Info(token, info, mask);
  }

  gex_TI_t result = 0;

  info->gex_srcrank = gasnetc_msgsource(token);
  result |= GEX_TI_SRCRANK;

  info->gex_ep = gasneti_THUNK_EP;
  result |= GEX_TI_EP;

  if (mask & (GEX_TI_ENTRY|GEX_TI_IS_REQ|GEX_TI_IS_LONG)) {
      handler_t index;
      amudp_category_t category;
      int is_req;
      gasneti_assert_zeroret(AMUDP_GetTokenInfo(token,&index,&category,&is_req));

      info->gex_entry = gasneti_import_ep(gasneti_THUNK_EP)->_amtbl + index;
      result |= GEX_TI_ENTRY;

      info->gex_is_req = is_req;
      result |= GEX_TI_IS_REQ;

      info->gex_is_long = (category == amudp_Long);
      result |= GEX_TI_IS_LONG;
  }

  return GASNETI_TOKEN_INFO_RETURN(result, info, mask);
}

extern int gasnetc_AMPoll(GASNETI_THREAD_FARG_ALONE) {
  int retval;
  GASNETI_CHECKATTACH();
#if GASNET_PSHM
  gasneti_AMPSHMPoll(0 GASNETI_THREAD_PASS);
#endif
  static unsigned int cntr;
  // In single-nbrhd case never need to poll the network for client AMs.
  // However, we'll still check for control traffic for orderly exit handling, on every 256th call.
  if (gasneti_mysupernode.grp_count > 1) {
    AMLOCK();
      GASNETI_AM_SAFE_NORETURN(retval,AM_Poll(gasnetc_bundle));
    AMUNLOCK();
  } else if (! (0xff & cntr++)) { // Thread race here is harmless (this is a heuristic)
    // TODO-EX: a lock-free peek would allow elimination of this lock cycle
    AMLOCK();
      GASNETI_AM_SAFE_NORETURN(retval,AMUDP_SPMDHandleControlTraffic(NULL));
    AMUNLOCK();
  } else {
    retval = GASNET_OK;
  }
  if_pf (retval) GASNETI_RETURN_ERR(RESOURCE);
  else return GASNET_OK;
}

/* ------------------------------------------------------------------------------------ */
/*
  Active Message Request Functions
  ================================
  TODO-EX: "nbrhd" support means we could remove the unreachable loopback paths in AMUDP
*/

GASNETI_INLINE(gasnetc_AMRequestShort)
int gasnetc_AMRequestShort( gex_TM_t tm, gex_Rank_t rank, gex_AM_Index_t handler,
                            gex_Flags_t flags,
                            int numargs, va_list argptr GASNETI_THREAD_FARG)
{
  int retval;
  gex_Rank_t jobrank = gasneti_e_tm_rank_to_jobrank(tm, rank);
  if (GASNETI_NBRHD_JOBRANK_IS_LOCAL(jobrank)) {
    GASNETC_IMMEDIATE_MAYBE_POLL(flags); /* poll at least once, to assure forward progress */
    retval = gasnetc_nbrhd_RequestGeneric( gasneti_Short, jobrank, handler,
                                           0, 0, 0,
                                           flags, numargs, argptr GASNETI_THREAD_PASS);
  } else {
    AMLOCK_TOSEND();
      GASNETI_AM_SAFE_NORETURN(retval,
               AMUDP_RequestVA(gasnetc_endpoint, jobrank, handler, 
                               numargs, argptr));
    AMUNLOCK();
    if_pf (retval) GASNETI_RETURN_ERR(RESOURCE);
  }
  return retval;
}

extern int gasnetc_AMRequestShortM( 
                            gex_TM_t tm,/* local context */
                            gex_Rank_t rank,       /* with tm, defines remote context */
                            gex_AM_Index_t handler, /* index into destination endpoint's handler table */
                            gex_Flags_t flags
                            GASNETI_THREAD_FARG,
                            int numargs, ...) {
  GASNETI_COMMON_AMREQUESTSHORT(tm,rank,handler,flags,numargs);

  va_list argptr;
  va_start(argptr, numargs); /*  pass in last argument */
  int retval = gasnetc_AMRequestShort(tm,rank,handler,flags,numargs,argptr GASNETI_THREAD_PASS);
  va_end(argptr);
  return retval;
}

GASNETI_INLINE(gasnetc_AMRequestMedium)
int gasnetc_AMRequestMedium(gex_TM_t tm, gex_Rank_t rank, gex_AM_Index_t handler,
                            void *source_addr, size_t nbytes,
                            gex_Event_t *lc_opt, gex_Flags_t flags,
                            int numargs, va_list argptr GASNETI_THREAD_FARG)
{
  int retval;
  gasneti_leaf_finish(lc_opt); // always locally completed
  gex_Rank_t jobrank = gasneti_e_tm_rank_to_jobrank(tm, rank);
  if (GASNETI_NBRHD_JOBRANK_IS_LOCAL(jobrank)) {
    GASNETC_IMMEDIATE_MAYBE_POLL(flags); /* poll at least once, to assure forward progress */
    retval = gasnetc_nbrhd_RequestGeneric( gasneti_Medium, jobrank, handler,
                                           source_addr, nbytes, 0,
                                           flags, numargs, argptr GASNETI_THREAD_PASS);
  } else {
    AMLOCK_TOSEND();
      GASNETI_AM_SAFE_NORETURN(retval,
               AMUDP_RequestIVA(gasnetc_endpoint, jobrank, handler, 
                                source_addr, nbytes, 
                                numargs, argptr));
    AMUNLOCK();
    if_pf (retval) GASNETI_RETURN_ERR(RESOURCE);
  }
  return retval;
}

extern int gasnetc_AMRequestMediumV(
                            gex_TM_t tm, gex_Rank_t rank, gex_AM_Index_t handler,
                            void *source_addr, size_t nbytes,
                            gex_Event_t *lc_opt, gex_Flags_t flags,
                            int numargs, va_list argptr GASNETI_THREAD_FARG)
{
  return gasnetc_AMRequestMedium(tm,rank,handler,source_addr,nbytes,lc_opt,flags,numargs,argptr GASNETI_THREAD_PASS);
}

extern int gasnetc_AMRequestMediumM( 
                            gex_TM_t tm,/* local context */
                            gex_Rank_t rank,       /* with tm, defines remote context */
                            gex_AM_Index_t handler, /* index into destination endpoint's handler table */
                            void *source_addr, size_t nbytes,   /* data payload */
                            gex_Event_t *lc_opt,       /* local completion of payload */
                            gex_Flags_t flags
                            GASNETI_THREAD_FARG,
                            int numargs, ...) {
  GASNETI_COMMON_AMREQUESTMEDIUM(tm,rank,handler,source_addr,nbytes,lc_opt,flags,numargs);

  va_list argptr;
  va_start(argptr, numargs); /*  pass in last argument */
  int retval = gasnetc_AMRequestMedium(tm,rank,handler,source_addr,nbytes,lc_opt,flags,numargs,argptr GASNETI_THREAD_PASS);
  va_end(argptr);
  return retval;
}

GASNETI_INLINE(gasnetc_AMRequestLong)
int gasnetc_AMRequestLong(  gex_TM_t tm, gex_Rank_t rank, gex_AM_Index_t handler,
                            void *source_addr, size_t nbytes, void *dest_addr,
                            gex_Event_t *lc_opt, gex_Flags_t flags,
                            int numargs, va_list argptr GASNETI_THREAD_FARG)
{
  int retval;
  gasneti_leaf_finish(lc_opt); // always locally completed
  gex_Rank_t jobrank = gasneti_e_tm_rank_to_jobrank(tm, rank);
  if (GASNETI_NBRHD_JOBRANK_IS_LOCAL(jobrank)) {
      GASNETC_IMMEDIATE_MAYBE_POLL(flags); /* poll at least once, to assure forward progress */
      retval = gasnetc_nbrhd_RequestGeneric( gasneti_Long, jobrank, handler,
                                             source_addr, nbytes, dest_addr,
                                             flags, numargs, argptr GASNETI_THREAD_PASS);
  } else {
    uintptr_t dest_offset = (uintptr_t)dest_addr;

    AMLOCK_TOSEND();
      GASNETI_AM_SAFE_NORETURN(retval,
               AMUDP_RequestXferVA(gasnetc_endpoint, jobrank, handler, 
                                   source_addr, nbytes, 
                                   dest_offset, 0,
                                   numargs, argptr));
    AMUNLOCK();
    if_pf (retval) GASNETI_RETURN_ERR(RESOURCE);
  }
  return retval;
}

extern int gasnetc_AMRequestLongV(
                            gex_TM_t tm, gex_Rank_t rank, gex_AM_Index_t handler,
                            void *source_addr, size_t nbytes, void *dest_addr,
                            gex_Event_t *lc_opt, gex_Flags_t flags,
                            int numargs, va_list argptr GASNETI_THREAD_FARG)
{
  return gasnetc_AMRequestLong(tm,rank,handler,source_addr,nbytes,dest_addr,lc_opt,flags,numargs,argptr GASNETI_THREAD_PASS);
}

extern int gasnetc_AMRequestLongM(
                            gex_TM_t tm,/* local context */
                            gex_Rank_t rank,       /* with tm, defines remote context */
                            gex_AM_Index_t handler, /* index into destination endpoint's handler table */
                            void *source_addr, size_t nbytes,   /* data payload */
                            void *dest_addr,                    /* data destination on destination node */
                            gex_Event_t *lc_opt,       /* local completion of payload */
                            gex_Flags_t flags
                            GASNETI_THREAD_FARG,
                            int numargs, ...) {
  GASNETI_COMMON_AMREQUESTLONG(tm,rank,handler,source_addr,nbytes,dest_addr,lc_opt,flags,numargs);

  va_list argptr;
  va_start(argptr, numargs); /*  pass in last argument */
  int retval = gasnetc_AMRequestLong(tm,rank,handler,source_addr,nbytes,dest_addr,lc_opt,flags,numargs,argptr GASNETI_THREAD_PASS);
  va_end(argptr);
  return retval;
}

GASNETI_INLINE(gasnetc_AMReplyShort)
int gasnetc_AMReplyShort(   gex_Token_t token, gex_AM_Index_t handler,
                            gex_Flags_t flags,
                            int numargs, va_list argptr)
{
  int retval;
  if_pt (gasnetc_token_in_nbrhd(token)) {
      retval = gasnetc_nbrhd_ReplyGeneric( gasneti_Short, token, handler,
                                           0, 0, 0,
                                           flags, numargs, argptr);
  } else {
    AM_ASSERT_LOCKED();
    GASNETI_AM_SAFE_NORETURN(retval,
              AMUDP_ReplyVA(token, handler, numargs, argptr));
    if_pf (retval) GASNETI_RETURN_ERR(RESOURCE);
  }
  return retval;
}

extern int gasnetc_AMReplyShortM( 
                            gex_Token_t token,     /* token provided on handler entry */
                            gex_AM_Index_t handler, /* index into destination endpoint's handler table */
                            gex_Flags_t flags,
                            int numargs, ...) {
  GASNETI_COMMON_AMREPLYSHORT(token,handler,flags,numargs);

  va_list argptr;
  va_start(argptr, numargs); /*  pass in last argument */
  int retval = gasnetc_AMReplyShort(token,handler,flags,numargs,argptr);
  va_end(argptr);
  return retval;
}

GASNETI_INLINE(gasnetc_AMReplyMedium)
int gasnetc_AMReplyMedium(  gex_Token_t token, gex_AM_Index_t handler,
                            void *source_addr, size_t nbytes,
                            gex_Event_t *lc_opt, gex_Flags_t flags,
                            int numargs, va_list argptr)
{
  int retval;
  gasneti_leaf_finish(lc_opt); // always locally completed
  if_pt (gasnetc_token_in_nbrhd(token)) {
       retval = gasnetc_nbrhd_ReplyGeneric( gasneti_Medium, token, handler,
                                            source_addr, nbytes, 0,
                                            flags, numargs, argptr);
  } else {
    AM_ASSERT_LOCKED();
    GASNETI_AM_SAFE_NORETURN(retval,
              AMUDP_ReplyIVA(token, handler, source_addr, nbytes, numargs, argptr));
    if_pf (retval) GASNETI_RETURN_ERR(RESOURCE);
  }
  return retval;
}

extern int gasnetc_AMReplyMediumV(
                            gex_Token_t token, gex_AM_Index_t handler,
                            void *source_addr, size_t nbytes,
                            gex_Event_t *lc_opt, gex_Flags_t flags,
                            int numargs, va_list argptr)
{
  return gasnetc_AMReplyMedium(token,handler,source_addr,nbytes,lc_opt,flags,numargs,argptr);
}

extern int gasnetc_AMReplyMediumM( 
                            gex_Token_t token,     /* token provided on handler entry */
                            gex_AM_Index_t handler, /* index into destination endpoint's handler table */
                            void *source_addr, size_t nbytes,   /* data payload */
                            gex_Event_t *lc_opt,       /* local completion of payload */
                            gex_Flags_t flags,
                            int numargs, ...) {
  GASNETI_COMMON_AMREPLYMEDIUM(token,handler,source_addr,nbytes,lc_opt,flags,numargs);

  va_list argptr;
  va_start(argptr, numargs); /*  pass in last argument */
  int retval = gasnetc_AMReplyMedium(token,handler,source_addr,nbytes,lc_opt,flags,numargs,argptr);
  va_end(argptr);
  return retval;
}

GASNETI_INLINE(gasnetc_AMReplyLong)
int gasnetc_AMReplyLong(    gex_Token_t token, gex_AM_Index_t handler,
                            void *source_addr, size_t nbytes, void *dest_addr,
                            gex_Event_t *lc_opt, gex_Flags_t flags,
                            int numargs, va_list argptr)
{
  int retval;
  gasneti_leaf_finish(lc_opt); // always locally completed
  if_pt (gasnetc_token_in_nbrhd(token)) {
      retval = gasnetc_nbrhd_ReplyGeneric( gasneti_Long, token, handler,
                                           source_addr, nbytes, dest_addr,
                                           flags, numargs, argptr);
  } else {
    uintptr_t dest_offset = (uintptr_t)dest_addr;

    AM_ASSERT_LOCKED();
    GASNETI_AM_SAFE_NORETURN(retval,
              AMUDP_ReplyXferVA(token, handler, source_addr, nbytes, dest_offset, numargs, argptr));
    if_pf (retval) GASNETI_RETURN_ERR(RESOURCE);
  }
  return retval;
}

extern int gasnetc_AMReplyLongV(
                            gex_Token_t token, gex_AM_Index_t handler,
                            void *source_addr, size_t nbytes, void *dest_addr,
                            gex_Event_t *lc_opt, gex_Flags_t flags,
                            int numargs, va_list argptr)
{
  return gasnetc_AMReplyLong(token,handler,source_addr,nbytes,dest_addr,lc_opt,flags,numargs,argptr);
}

extern int gasnetc_AMReplyLongM( 
                            gex_Token_t token,     /* token provided on handler entry */
                            gex_AM_Index_t handler, /* index into destination endpoint's handler table */
                            void *source_addr, size_t nbytes,   /* data payload */
                            void *dest_addr,                    /* data destination on destination node */
                            gex_Event_t *lc_opt,       /* local completion of payload */
                            gex_Flags_t flags,
                            int numargs, ...) {
  GASNETI_COMMON_AMREPLYLONG(token,handler,source_addr,nbytes,dest_addr,lc_opt,flags,numargs);

  va_list argptr;
  va_start(argptr, numargs); /*  pass in last argument */
  int retval = gasnetc_AMReplyLong(token,handler,source_addr,nbytes,dest_addr,lc_opt,flags,numargs,argptr);
  va_end(argptr);
  return retval;
}

/* ------------------------------------------------------------------------------------ */
/*
  Handler-safe locks
  ==================
*/
#if !GASNETC_NULL_HSL
extern void gasnetc_hsl_init   (gex_HSL_t *hsl) {
  GASNETI_CHECKATTACH();
  gasneti_mutex_init(&(hsl->lock));
}

extern void gasnetc_hsl_destroy(gex_HSL_t *hsl) {
  GASNETI_CHECKATTACH();
  gasneti_mutex_destroy(&(hsl->lock));
}

extern void gasnetc_hsl_lock   (gex_HSL_t *hsl) {
  GASNETI_CHECKATTACH();

  {
    #if GASNETI_STATS_OR_TRACE
      gasneti_tick_t startlock = GASNETI_TICKS_NOW_IFENABLED(L);
    #endif
    #if GASNETC_HSL_SPINLOCK
      if_pf (gasneti_mutex_trylock(&(hsl->lock)) == EBUSY) {
        if (gasneti_wait_mode == GASNET_WAIT_SPIN) {
          while (gasneti_mutex_trylock(&(hsl->lock)) == EBUSY) {
            gasneti_spinloop_hint();
          }
        } else {
          gasneti_mutex_lock(&(hsl->lock));
        }
      }
    #else
      gasneti_mutex_lock(&(hsl->lock));
    #endif
    #if GASNETI_STATS_OR_TRACE
      hsl->acquiretime = GASNETI_TICKS_NOW_IFENABLED(L);
      GASNETI_TRACE_EVENT_TIME(L, HSL_LOCK, hsl->acquiretime-startlock);
    #endif
  }
}

extern void gasnetc_hsl_unlock (gex_HSL_t *hsl) {
  GASNETI_CHECKATTACH();

  GASNETI_TRACE_EVENT_TIME(L, HSL_UNLOCK, GASNETI_TICKS_NOW_IFENABLED(L)-hsl->acquiretime);

  gasneti_mutex_unlock(&(hsl->lock));
}

extern int  gasnetc_hsl_trylock(gex_HSL_t *hsl) {
  GASNETI_CHECKATTACH();

  {
    int locked = (gasneti_mutex_trylock(&(hsl->lock)) == 0);

    GASNETI_TRACE_EVENT_VAL(L, HSL_TRYLOCK, locked);
    if (locked) {
      #if GASNETI_STATS_OR_TRACE
        hsl->acquiretime = GASNETI_TICKS_NOW_IFENABLED(L);
      #endif
    }

    return locked ? GASNET_OK : GASNET_ERR_NOT_READY;
  }
}
#endif
/* ------------------------------------------------------------------------------------ */
#if GASNET_TRACE || GASNET_DEBUG
  /* called when entering/leaving handler - also called when entering/leaving AM_Reply call */
  extern void gasnetc_enteringHandler_hook(amudp_category_t cat, int isReq, int handlerId, void *token, 
                                           void *buf, size_t nbytes, int numargs, uint32_t *args) {
    #if GASNET_DEBUG
      // TODO-EX: per-EP table
      const gex_AM_Entry_t * const handler_entry = &gasnetc_handler[handlerId];
      gasneti_amtbl_check(handler_entry, numargs, (gasneti_category_t)cat, isReq);
    #endif
    switch (cat) {
      case amudp_Short:
        if (isReq) GASNETI_TRACE_AMSHORT_REQHANDLER(handlerId, token, numargs, args);
        else       GASNETI_TRACE_AMSHORT_REPHANDLER(handlerId, token, numargs, args);
        break;
      case amudp_Medium:
        if (isReq) GASNETI_TRACE_AMMEDIUM_REQHANDLER(handlerId, token, buf, nbytes, numargs, args);
        else       GASNETI_TRACE_AMMEDIUM_REPHANDLER(handlerId, token, buf, nbytes, numargs, args);
        break;
      case amudp_Long:
        if (isReq) GASNETI_TRACE_AMLONG_REQHANDLER(handlerId, token, buf, nbytes, numargs, args);
        else       GASNETI_TRACE_AMLONG_REPHANDLER(handlerId, token, buf, nbytes, numargs, args);
        break;
      default: gasneti_unreachable_error(("Unknown handler type in gasnetc_enteringHandler_hook(): 0x%x",(int)cat));
    }
    GASNETI_HANDLER_ENTER(isReq);
  }
  extern void gasnetc_leavingHandler_hook(amudp_category_t cat, int isReq) {
    switch (cat) {
      case amudp_Short:
        GASNETI_TRACE_PRINTF(A,("AM%s_SHORT_HANDLER: handler execution complete", (isReq?"REQUEST":"REPLY"))); \
        break;
      case amudp_Medium:
        GASNETI_TRACE_PRINTF(A,("AM%s_MEDIUM_HANDLER: handler execution complete", (isReq?"REQUEST":"REPLY"))); \
        break;
      case amudp_Long:
        GASNETI_TRACE_PRINTF(A,("AM%s_LONG_HANDLER: handler execution complete", (isReq?"REQUEST":"REPLY"))); \
        break;
      default: gasneti_unreachable_error(("Unknown handler type in gasnetc_leavingHandler_hook(): 0x%x",(int)cat));
    }
    GASNETI_HANDLER_LEAVE(isReq);
  }
#endif

/* ------------------------------------------------------------------------------------ */
/*
  Private Handlers:
  ================
  see mpi-conduit and extended-ref for examples on how to declare AM handlers here
  (for internal conduit use in bootstrapping, job management, etc.)
*/
static gex_AM_Entry_t const gasnetc_handlers[] = {
  GASNETC_COMMON_HANDLERS(),

  /* ptr-width independent handlers */

  /* ptr-width dependent handlers */

  GASNETI_HANDLER_EOT
};

gex_AM_Entry_t const *gasnetc_get_handlertable(void) {
  return gasnetc_handlers;
}

/* ------------------------------------------------------------------------------------ */
