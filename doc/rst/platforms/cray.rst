.. _readme-cray:

================================
Using Chapel on HPE Cray Systems
================================

The following information is assembled to help Chapel users get up and
running on HPE Cray systems including the HPE Cray EX and Cray XC
series systems.

.. contents::


--------------------------------------------------------
Getting Started with Chapel on HPE Cray EX or XC Systems
--------------------------------------------------------

Chapel is available as a module for HPE Cray EX and XC systems.  When
it is installed on your system, you do not need to build Chapel from
the source release (though you can).  Using the module systems on such
platforms, you can use Chapel with the default settings and confirm it
is correctly installed, as follows:

1) Ensure this required module is loaded.  Normally it will be loaded
   for you, but under some circumstances you may need to load or
   restore it yourself:

   .. code-block:: sh

      PrgEnv-gnu or PrgEnv-cray


2) Load the Chapel module::

      module load chapel


3) Compile an example program using::

     chpl $CHPL_HOME/examples/hello6-taskpar-dist.chpl


4) Execute the resulting executable (on four locales)::

     ./hello6-taskpar-dist -nl 4


This should be all that is necessary to use Chapel on an HPE Cray EX or XC
system.  If the installation setup by your system administrator
deviates from the default settings, or you are interested in other
configuration options, see `Using Chapel on an HPE Cray System`_ below.  If
instead you wish to build Chapel from source, continue on to `Building
Chapel for an HPE Cray System from Source`_ just below.

Note that the Chapel module for HPE Cray EX systems does not currently have
the GASNet communication layer built into it.  For multilocale execution
on EX systems please use the ofi communication layer instead.

For information on obtaining and installing the Chapel module please
contact your system administrator.


-------------------------------------------------------------
Getting Started with Chapel on HPE Apollo and Cray CS Systems
-------------------------------------------------------------

On HPE Apollo and Cray CS systems, Chapel is not currently available
as a module due to the wide diversity of configurations that these
systems support.  For this reason, Chapel must be built from source on
these systems using the `Building Chapel for an HPE Cray System from
Source`_ instructions just below.


--------------------------------------------------
Building Chapel for an HPE Cray System from Source
--------------------------------------------------

1) Set ``CHPL_HOST_PLATFORM`` to the string representing your system
   type.

   For example:

    .. code-block:: sh

      export CHPL_HOST_PLATFORM=hpe-cray-ex

   The following table lists the supported systems and strings.  Note
   that on HPE Cray EX and XC systems, these values should typically
   be inferred automatically and not need to be set manually.  That said,
   there is also no downside to setting them manually.  As with most
   `CHPL_*` environment variables, the current set and inferred values
   can be determined by running ``$CHPL_HOME/util/printchplenv``.

       =============  ==================
       System         CHPL_HOST_PLATFORM
       =============  ==================
       EX series      hpe-cray-ex
       Apollo series  hpe-apollo
       XD series      hpe-cray-xd
       XC series      cray-xc
       CS series      cray-cs
       =============  ==================


2) Optionally, set the ``CHPL_LAUNCHER`` environment variable to indicate
   how Chapel should launch jobs on your system:

      ========================================  =========================
      On an HPE Cray EX system, ...             set CHPL_LAUNCHER to...
      ========================================  =========================
      ...queue jobs using SLURM (sbatch)        slurm-srun
      ...run jobs interactively using PALS      pals
      ========================================  =========================

      ========================================  =========================
      On a HPE Apollo or Cray CS system, to...  set CHPL_LAUNCHER to...
      ========================================  =========================
      ...run jobs interactively on your system  gasnetrun_ibv
      ...queue jobs using SLURM (sbatch)        slurm-gasnetrun_ibv
      ...queue jobs using LSF (bsub)            lsf-gasnetrun_ibv
      ...queue jobs using PBS (qsub)            pbs-gasnetrun_ibv
      ========================================  =========================

      ========================================  =========================
      On a Cray XC system, to...                set CHPL_LAUNCHER to...
      ========================================  =========================
      ...run jobs interactively on your system  aprun
      ...queue jobs using PBS (qsub)            pbs-aprun
      ...queue jobs using SLURM (sbatch)        slurm-srun
      ========================================  =========================

   On HPE Cray EX systems, ``CHPL_LAUNCHER`` defaults to ``slurm-srun``
   if ``srun`` is in your path and ``none`` otherwise.

   On HPE Apollo and Cray CS systems, ``CHPL_LAUNCHER`` defaults to
   ``gasnetrun_ibv``.

   On Cray XC systems, ``CHPL_LAUNCHER`` defaults to ``aprun`` if
   ``aprun`` is in your path, ``slurm-srun`` if ``srun`` is in your path
   and ``none`` otherwise.

   You can also set CHPL_LAUNCHER to ``none`` if you prefer to manually
   manage all queuing and job launch commands yourself, though the
   advantage of using a launcher is to make sure that Chapel maps
   processes and threads to the appropriate target hardware units.

   For more information on Chapel's launcher capabilities and options,
   refer to :ref:`readme-launcher`.


3) Select the target compiler that Chapel should use when compiling
   code for the compute node:

   On an HPE Apollo or Cray CS series system, set the
   ``CHPL_TARGET_COMPILER`` environment variable to indicate which
   compiler to use (and make sure that the compiler is in your path).

      ===========================  ==============================
      To request...                set CHPL_TARGET_COMPILER to...
      ===========================  ==============================
      ...the LLVM/clang backend    llvm (default)
      ...the GNU compiler (gcc)    gnu
      ...the Clang compiler        clang
      ...the Intel compiler (icc)  intel
      ===========================  ==============================

   On an HPE Cray EX or Cray XC system, when using the C back-end,
   ensure that you have one of the following Programming Environment
   modules loaded to specify your target compiler::

       PrgEnv-allinea (ARM only)
       PrgEnv-cray
       PrgEnv-gnu
       PrgEnv-intel


4) Make sure you're in the top-level chapel/ directory and make/re-make the
   compiler and runtime::

     gmake

   Note that a single Chapel installation can support multiple
   configurations simultaneously and that you can switch between them
   simply by changing any of the above settings.  However, each
   configuration must be built separately.  Thus, you can change any of
   the settings in the steps before this, and then re-run this step in
   order to create additional installations.  Thereafter, you can switch
   between any of these configurations without rebuilding.


----------------------------------
Using Chapel on an HPE Cray System
----------------------------------

1) If you are working from a Chapel module:

     a) Load the module using ``module load chapel``
     b) Optionally select a launcher, as in step 2 above
     c) Select a target compiler, as in step 3 above

   If you are working from a source installation:

     a) Set your host platform as in step 1 above
     b) Optionally select a launcher, as in step 2 above
     c) Select a target compiler, as in step 3 above
     d) Set ``CHPL_HOME`` and your paths by invoking the appropriate
        ``util/setchplenv`` script for your shell.  For example:

      .. code-block:: sh

        source util/setchplenv.bash


2) On HPE Cray EX systems with ``CHPL_COMM=ofi``, optionally, load the
   Cray PMI modules::

      module load cray-pmi{,-lib}

   Often this is not required.  Usually the default PMI support has
   sufficient capabilities to support Chapel program startup.  But under
   certain circumstances it does not, and when you run a Chapel program
   that was built without these loaded you will see messages like this
   one:

   .. code-block:: sh

      [PE_0]:_pmi2_add_kvs:ERROR: The KVS data segment of <num> entries
      is not large enough.  Increase the number of KVS entries by
      setting env variable PMI_MAX_KVS_ENTRIES to a higher value.

   Having the Cray PMI modules loaded when the program is compiled
   will prevent this problem.  We expect that eventually these modules
   will be loaded by default on EX systems, but so far this has not
   consistently been the case.


3) Compile your Chapel program.  For example:

   .. code-block:: sh

      chpl $CHPL_HOME/examples/hello6-taskpar-dist.chpl

   See :ref:`readme-compiling` or  ``man chpl`` for further details.


4) If ``CHPL_LAUNCHER`` is set to anything other than ``none``, when you
   compile a Chapel program for your Cray system, you will see two
   binaries (e.g., ``hello6-taskpar-dist`` and ``hello6-taskpar-dist_real``).
   The first binary contains code to launch the Chapel program onto
   the compute nodes, as specified by your ``CHPL_LAUNCHER`` setting.  The
   second contains the program code itself; it is not intended to be
   executed directly from the shell prompt.

   You can use the ``-v`` or ``--dry-run`` flags to see the commands
   used by the launcher binary to start your program.

   If ``CHPL_LAUNCHER`` is ``pbs-aprun``:

     a) You can optionally specify a queue name using the environment
        variable ``CHPL_LAUNCHER_QUEUE``.  For example:

          .. code-block:: sh

            export CHPL_LAUNCHER_QUEUE=batch

        If this variable is left unset, no queue name will be
        specified.  Alternatively, you can set the queue name on your
        Chapel program command line using the ``--queue`` flag.

     b) You can also optionally set a wall clock time limit for the
        job using ``CHPL_LAUNCHER_WALLTIME``.  For example to specify a
        10-minute time limit, use:

          .. code-block:: sh

            export CHPL_LAUNCHER_WALLTIME=00:10:00

        Alternatively, you can set the wall clock time limit on your
        Chapel program command line using the ``--walltime`` flag.

   For further information about launchers, please refer to
   :ref:`readme-launcher`.


5) Execute your Chapel program.  Multi-locale executions require the
   number of locales (compute nodes) to be specified on the command
   line.  For example::

     ./hello6-taskpar-dist -nl 2

   Requests the program to be executed using two locales.


6) If your HPE Cray system has compute nodes with varying numbers of
   cores, you can request nodes with at least a certain number of
   cores using the variable ``CHPL_LAUNCHER_CORES_PER_LOCALE``.  For
   example, on a system in which some compute nodes have 24 or
   more cores per compute node, you could request nodes with at least
   24 cores using:

   .. code-block:: sh

      export CHPL_LAUNCHER_CORES_PER_LOCALE=24

   This variable may be needed when you are using the aprun launcher and
   running Chapel programs within batch jobs you are managing yourself.
   The aprun launcher currently creates aprun commands that request the
   maximum number of cores per locale found on any locale in the system,
   irrespective of the fact that the batch job may have a lower limit
   than that on the number of cores per locale.  If the batch job limit
   is less than the maximum number of cores per locale, you will get the
   following error message when you try to run a Chapel program::

      apsched: claim exceeds reservation's CPUs

   You can work around this by setting ``CHPL_LAUNCHER_CORES_PER_LOCALE`` to
   the same or lesser value as the number of cores per locale specified
   for the batch job (for example, the mppdepth resource for the PBS
   qsub command).  In the future we hope to achieve better integration
   between Chapel launchers and workload managers.


7) If your HPE Cray system has compute nodes with varying numbers of CPUs
   per compute unit, you can request nodes with a certain number of
   CPUs per compute unit using the variable ``CHPL_LAUNCHER_CPUS_PER_CU``.
   For example, on a Cray XC system with some nodes having at
   least 2 CPUs per compute unit, to request running on those nodes
   you would use:

   .. code-block:: sh

      export CHPL_LAUNCHER_CPUS_PER_CU=2

   Currently, the only legal values for ``CHPL_LAUNCHER_CPUS_PER_CU`` are
   0 (the default), 1, and 2.


========================================  =============================
For more information on...                see...
========================================  =============================
...CHPL_* environment settings            :ref:`readme-chplenv`
...Compiling Chapel programs              :ref:`readme-compiling`
...Launcher options                       :ref:`readme-launcher`
...Executing Chapel programs              :ref:`readme-executing`
...Running multi-locale Chapel programs   :ref:`readme-multilocale`
========================================  =============================


--------------------------------------
Cray File Systems and Chapel execution
--------------------------------------

For best results, it is recommended that you execute your Chapel
program by placing the binaries on a file system shared between the
login node and compute nodes (typically Lustre), as this will provide
the greatest degree of transparency when executing your program.  In
some cases, running a Chapel program from a non-shared file system
will make it impossible to launch onto the compute nodes.  In other
cases, the launch will succeed, but any files read or written by the
Chapel program will be opened relative to the compute node's file
system rather than the login node's.


----------------------------------------------------
Special Notes for Cray XC Series Systems
----------------------------------------------------

.. _ugni-comm-on-cray:

Native ugni Communication Layer
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The :ref:`readme-multilocale` page describes the runtime communication
layer implementations that can be used by Chapel programs.  In addition
to the standard ones, Chapel supports a Cray-specific ``ugni``
communication layer.  The ugni communication layer interacts with
the system's network interface very closely through a lightweight
interface called uGNI (user Generic Network Interface).  On Cray XC
systems the ugni communication layer is the default.


Using the ugni Communications Layer
___________________________________

To use ugni communications:

1) Leave your ``CHPL_COMM`` environment variable unset or set it to
   ``ugni``:

   .. code-block:: sh

      export CHPL_COMM=ugni

   This specifies that you wish to use the Cray-specific communication
   layer.


2) *(Optional)* Load an appropriate ``craype-hugepages`` module.  For example::

     module load craype-hugepages16M

   The ugni communication layer can be used with or without so-called
   *hugepages*.  Performance for remote variable references is much
   better when hugepages are used.  The only downside of using hugepages
   is that the tasking layer may not be able to detect task stack
   overflows by means of guard pages (see below).

   To use hugepages, you must have a ``craype-hugepages`` module loaded
   both when building your program and when running it.  There are
   several hugepage modules, with suffixes indicating the page size they
   support.  For example, ``craype-hugepages16M`` supports 16 MiB
   hugepages.  It does not matter which ``craype-hugepages`` module you
   have loaded when you build your program.  Any of them will do.  Which
   one you have loaded when you run a program does matter, however.  For
   general use, the Chapel group recommends the ``craype-hugepages16M``
   module.  You can read on for more information about hugepage modules
   if you would like, but the recommended ``craype-hugepages16M`` module
   will probably give you satisfactory results.

   The Cray network interface chips (NICs) can only address memory that
   has been registered with them. In practical terms, the Aries(TM) NIC
   on Cray XC systems is not limited as to how much memory it can
   register.  However, it does have an on-board cache of 512 registered
   page table entries, and registering more than this can cause reduced
   performance if the program's memory reference pattern causes refills
   in this cache.  We have seen up to a 15% reduction from typical
   nightly XC-16 performance in an ra-rmo run using hugepages small
   enough that every reference should have missed in this cache.
   Covering an entire 128 GiB XC compute node with only 512 hugepages
   will require at least the ``craype-hugepages256M`` module's 256 MiB
   hugepages.

   Offsetting this, using larger hugepages may reduce performance because
   it can result in poorer NUMA affinity.  With the ugni communication
   layer, arrays larger than 2 hugepages are allocated separately from the
   heap, which improves NUMA affinity.  An obvious side effect of using
   larger hugepages is that an array has to be larger to qualify.  Thus,
   achieving the best performance for any given program may require
   striking a balance between using larger hugepages to reduce NIC page
   table cache refills and using smaller ones to improve NUMA locality.

   Note that when hugepages are used with the ugni comm layer, tasking
   layers cannot use guard pages for stack overflow detection.  Qthreads
   tasking cannot detect stack overflow except by means of guard pages,
   so if ugni communications is combined with qthreads tasking and a
   hugepage module is loaded, stack overflow detection is unavailable.


Network Atomics
_______________

The Aries networks on Cray XC series systems support remote atomic
memory operations (AMOs).  When the ``CHPL_NETWORK_ATOMICS`` environment
variable is set to ``ugni``, the following operations on remote atomics
are done using the network::

    32- and 64-bit signed and unsigned integer types:
    32- and 64-bit real types:
      read()
      write()
      exchange()
      compareAndSwap()
      add(), fetchAdd()
      sub(), fetchSub()

    32- and 64-bit signed and unsigned integer types:
      or(),  fetchOr()
      and(), fetchAnd()
      xor(), fetchXor()

All of the operations shown above are done natively by the network
hardware except 64-bit real add, which is disabled in hardware and thus
done using ``on`` statements.


ugni Communication Layer and the Heap
_____________________________________

The "heap" is an area of memory used for dynamic allocation of
everything from user data to internal management data structures.
When running on Cray XC systems using the default configuration
with the ugni comm layer and a ``craype-hugepages`` module loaded, the
heap is used for all dynamic allocations except data space for arrays
larger than 2 hugepages.  (See `Using the ugni Communications Layer`_,
just above, for more about hugepages.)  It is normally extended
dynamically, as needed.  But if desired, the heap can instead be created
at a specified fixed size at the beginning of execution.  In some cases
this will reduce certain internal comm layer overheads and marginally
improve performance.

The disadvantage of a fixed heap is that it usually produces worse NUMA
affinity, it limits available heap memory to the specified fixed size,
and it limits memory for arrays to whatever remains after the fixed-size
heap is created.  If either of the latter are less than what a program
needs, it will terminate prematurely with an "Out of memory" message.

To specify a fixed heap, set the ``CHPL_RT_MAX_HEAP_SIZE`` environment
variable to indicate its size.  For the value of this variable you can
use any of the following formats, where *num* is a positive integer
number:

    ======= ==========================================
    Format  Resulting Heap Size
    ======= ==========================================
    num     num bytes
    num[kK] num * 2**10 bytes
    num[mM] num * 2**20 bytes
    num[gG] num * 2**30 bytes
    num%    percentage of compute node physical memory
    ======= ==========================================

Any of the following would specify an approximately 1 GiB heap on a
128-GiB compute node, for example:

  .. code-block:: sh

    export CHPL_RT_MAX_HEAP_SIZE=1073741824
    export CHPL_RT_MAX_HEAP_SIZE=1048576k
    export CHPL_RT_MAX_HEAP_SIZE=1024m
    export CHPL_RT_MAX_HEAP_SIZE=1g
    export CHPL_RT_MAX_HEAP_SIZE=1% # 1.28 GiB, really

Note that the resulting heap size may get rounded up to match the page
alignment.  How much this will add, if any, depends on the hugepage size
in any ``craype-hugepage`` module you have loaded at the time you
execute the program.  It may also be reduced, if some resource
limitation prevents making the heap as large as requested.


ugni Communication Layer Registered Memory Regions
__________________________________________________

The ugni communication layer maintains information about every memory
region it registers with Aries NIC.  Roughly speaking there are a few
memory regions for each tasking layer thread, plus one for each array
larger than 2 hugepages allocated and registered separately from the
heap.  By default the comm layer can handle up to 16k (2**14) total
memory regions, which is plenty under normal circumstances.  In the
event a program needs more than this, a message like the following will
be printed:

  .. code-block:: sh

    warning: no more registered memory region table entries (max is 16384).
             Change using CHPL_RT_COMM_UGNI_MAX_MEM_REGIONS.

To provide for more registered regions, set the
``CHPL_RT_COMM_UGNI_MAX_MEM_REGIONS`` environment variable to a number
indicating how many you want to allow.  For example:

  .. code-block:: sh

    export CHPL_RT_COMM_UGNI_MAX_MEM_REGIONS=30000

Note that there are certain comm layer overheads that are proportional to
the number of registered memory regions, so allowing a very high number of
them may lead to reduced performance.


ugni Hugepage-related Warnings
______________________________

   Communication performance with ugni is so much better when hugepages
   are used that if you do not use them, the runtime will print the
   following warning when a multilocale program starts::

      warning: without hugepages, communication performance will suffer

   If you definitely do not want to use hugepages you can quiet this
   warning by giving the ``--quiet`` or ``-q`` option when you run the
   executable.  Otherwise, load a hugepage module as described above in
   `Using the ugni Communications Layer`_ before running.

   When you are using hugepages and do not have a fixed heap (that is,
   the ``CHPL_RT_MAX_HEAP_SIZE`` environment variable is not set), the
   Chapel runtime expects certain hugepage-related environment variables
   to have been set by the Chapel launcher.  If you do not use a Chapel
   launcher you have to provide these settings yourself.  Not doing so
   will result in one or both of the following messages::

      warning: dynamic heap on hugepages needs HUGETLB_NO_RESERVE set to something
      warning: dynamic heap on hugepages needs CHPL_JE_MALLOC_CONF set properly

   To quiet these warnings, use the following settings:

    .. code-block:: sh

      export HUGETLB_NO_RESERVE=yes
      export CHPL_JE_MALLOC_CONF=purge:decay,lg_chunk:log2HPS

   where *log2HPS* is the base-2 log of the hugepage size.  For example,
   with 16 MiB hugepages you would use:

    .. code-block:: sh

      export CHPL_JE_MALLOC_CONF=purge:decay,lg_chunk:24


gasnet Communication Layer
~~~~~~~~~~~~~~~~~~~~~~~~~~

The GASNet-based communication layer discussed in the
:ref:`readme-multilocale` page can be used on all Cray systems.  For
best performance it should be used with native substrates and fixed
segments, though even then its performance will rarely match that of the
ugni communication layer.  The relevant configurations are::

  CHPL_COMM=gasnet
    CHPL_COMM_SUBSTRATE=aries (for XC)
    CHPL_GASNET_SEGMENT=fast or large

In these configurations the heap is created with a fixed size at the
beginning of execution.  The default size works well in most cases but
if it doesn't a different size can be specified, as discussed in the
following section.


gasnet Communication Layer and the Heap
_______________________________________

In contrast to the dynamic heap extension available in the ugni comm
layer, when the gasnet comm layer is used with a native substrate for
higher network performance, the runtime must know up front the maximum
size the heap will grow to during execution.

In these cases the heap is used for all dynamic allocations, including
arrays.  By default it will occupy as much of the free memory on each
compute node as the runtime can acquire, less some small amount to allow
for demands from other (system) programs running there.  Advanced users
may want to make the heap smaller than the default.  Programs start more
quickly with a smaller heap, and in the unfortunate event that you need
to produce core files, those will be written more quickly if the heap is
smaller.  Specify the heap size using the ``CHPL_RT_MAX_HEAP_SIZE``
environment variable, as discussed above in `ugni Communication Layer
and the Heap`_.  But be aware that just as in the ``CHPL_COMM=ugni``
case, if you reduce the heap size to less than the amount your program
actually needs and then run it, it will terminate prematurely due to not
having enough memory.

Note that for ``CHPL_COMM=gasnet``, ``CHPL_RT_MAX_HEAP_SIZE`` is
synonymous with ``GASNET_MAX_SEGSIZE``, and the former overrides the
latter if both are set.


.. _readme-cray-constraints:

--------------------------
Known Constraints and Bugs
--------------------------

* Our PBS launcher explicitly supports PBS Pro, Moab/Torque, and the
  NCCS site versions of PBS.  It may also work with other versions.
  If our PBS launcher does not work for you, you can fall back on a
  more manual launch of your program. For example, supposing the
  program is compiled to ``myprogram``:

  - Launch the ``myprogram_real`` binary manually using aprun and your own
    qsub script or command.

  - Use ``./myprogram --generate-qsub-script`` to generate a qsub script.
    Then edit the generated script and launch the ``myprogram_real`` binary
    manually as above.

* Redirecting stdin when executing a Chapel program under PBS/qsub
  may not work due to limitations of qsub.

* For EX and XC systems, there is a known issue with the Cray MPI
  release that causes some programs to assert and then hang during
  exit.  A workaround is to set the environment variable,
  ``MPICH_GNI_DYNAMIC_CONN`` to ``disabled``.  Setting this environment
  variable affects all MPI programs, so remember to unset it after
  running your Chapel program.

* The amount of memory available to a Chapel program running over
  GASNet with the aries conduit is allocated at program start up.  The
  default memory segment size may be too high on some platforms,
  resulting in an internal Chapel error or a GASNet initialization
  error such as::

     node 1 log gasnetc_init_segment() at $CHPL_HOME/third-party/gasnet/gasnet-src/aries-conduit/gasnet_aries.c:<line#>: MemRegister segment fault 8 at  0x2aab6ae00000 60000000, code GNI_RC_ERROR_RESOURCE

  If your Chapel program exits with such an error, try setting the
  environment variable ``CHPL_RT_MAX_HEAP_SIZE`` or ``GASNET_MAX_SEGSIZE`` to a
  lower value than the default (say 1G) and re-running your program.
  For more information, refer to the discussion of ``CHPL_RT_MAX_HEAP_SIZE``
  above and/or the discussion of ``GASNET_MAX_SEGSIZE`` here::

     $CHPL_HOME/third-party/gasnet/gasnet-src/README

.. |reg|    unicode:: U+000AE .. REGISTERED SIGN
.. |trade|  unicode:: U+02122 .. TRADE MARK SIGN
