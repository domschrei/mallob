
# Frequently Asked Questions

Here we gather some frequently asked questions on Mallob. We plan to extend this list on occasion. 

### General

#### Is it Mallob or MallobSat?

Mallob is the name of our overall distributed scheduling and solving framework, which can support different applications.  
MallobSat is the name of the distributed and malleable SAT solving engine that is tightly integrated within Mallob. We introduced this distinction only in 2023. As a result, early mentions of our SAT solving engine still go by the name of its surrounding framework, Mallob.

#### Can I execute the MallobSat solver in an isolated fashion? 

MallobSat can (currently) only be executed by executing Mallob and then spawning or submitting one or several SAT solving tasks to Mallob.
Option `-mono` is the seamless way to do this automatically for a single, specified SAT instance and then shut down Mallob as soon as the task is done.

#### Do I need to obey the LGPL license?

In general, yes. Please approach us if you would prefer not to.

### Setup and Hardware

#### What kind of hardware does Mallob need?

Mallob can be run on a single machine or on many machines at once.
In particular, Mallob(Sat) is a sensible SAT solving tool for hardware ranging from shared-memory parallelism (say, 8-16 cores and above) to distributed scales that amount to a few thousand cores.

In terms of node size, Mallob is rather well equipped to handle both a large number of thin nodes as well as a small number of fat nodes.
Our recommendation is to spawn one MPI process for each socket of a physical machine. The (maximum) number of solver threads per process should then be set to the number of (physical) cores per socket. This number can reasonably range from 4 to 32 cores with no problem. If the sockets are even larger, we recommend to try to spawn two MPI processes per socket to avoid overburdening the concurrent data structures.

Modern architectures that feature heterogeneous cores (e.g., x "economy cores" + y "performance cores) would need to be treated differently and likely require some additional care.

A reasonable amount of RAM per core is important. E.g., if only 2GB per physical core are available, MallobSat is sometimes forced to reduce the number of solver threads per node. For the actual SAT solving in the background, large and fast caches per core are likely beneficial.

#### Is the kind of interconnect (Ethernet / TCP/IP, InfiniBand, etc.) relevant?

When only performing single instance SAT solving (-mono=path/to/cnf), the interconnect between nodes (for message passing) is not too relevant (see [our JAIR'24 paper](https://jair.org/index.php/jair/article/view/15827)).  
If the intended use case is to use Mallob as an on-demand scheduling and solving platform (which drastically reduces latencies and increases resource efficiency if you have several SAT instances to solve), then high-speed interconnects likely help with scheduling and response time latencies (e.g., InfiniBand or Intel OmniPath rather than plain Ethernet).

#### Does Mallob support/use GPUs?

As of yet, no.

### Execution

#### Mallob doesn't solve my problem, it runs indefinitely or crashes.

* `[ERROR] execl returned errno 2`: Mallob was (most probably) not executed from it's home directory. In particular, Mallob needs to find the sub-process executables (`mallob_process_dispatcher`, `mallob_sat_process`, etc.) at the (relative) path provided via the build option `-DMALLOB_SUBPROC_PATH`. For most robust results, execute Mallob from its home directory. Alternatively, you can set the path to these sub-processes explictly.
* Make sure that the problem instance path you handed to Mallob exists; otherwise, Mallob may wait indefinitely for such a file to appear.

#### I try running $t$ solver threads per Mallob process and get an error "Option t: $t$ > 32(max)!"

You need to compile Mallob with support for a higher maximum number of solver threads per process. Set `-DMALLOB_MAX_N_APPTHREADS_PER_PROCESS` to a number that is at least as high as $t$.

#### Mallob performs badly and does not scale at all.

* Make sure that you use an appropriate number of threads per process (`-t`) and appropriate MPI options (including the number of processes and the mapping of processes to cores).
* While running Mallob, you can see via `htop` whether the SAT solving threads are in fact running at separate cores.
    * A high ratio of CPU time spent in kernel mode can also indicate issues with the setup, such as non scalable memory allocation or over-subscription of cores.
* You can also look out for log messages featuring the field `cpu_ratio`. For each solver thread (`td.*`), the reported value should be close to 1.

### Advanced Usage and Development

#### Can I compile Mallob as a backend SAT solver (e.g., IPASIR-style) into my application?

Not directly. Mallob is a distributed MPI application and hence follows a different execution model than a normal user application.
Compiling Mallob into another application would necessitate bubbling up all the handling of MPI matters, and then the question remains of what is the semantic of your application when it is executed in parallel at many compute nodes at once.

There are two recommended ways to connect Mallob to an application:
* The plug-and-play option: Compile the [Mallob IPASIR bridge](https://github.com/domschrei/mallob-ipasir-bridge) into your application and then let it talk to a running Mallob instance in the background.
* The more involved but likely more efficient option: Flip the setup on its head and compile your application logic into Mallob. [Integrate it as a new client-side application engine to Mallob.](/docs/application_engines.md)
