
[![KIT - SAtRes group](https://img.shields.io/badge/KIT-SAtRes_group-009682)](https://satres.kikit.kit.edu/research/mallob/)
[![Helmholtz RSD - /software/mallob](https://img.shields.io/badge/Helmholtz_RSD-%2Fsoftware%2Fmallob-002864)](https://satres.kikit.kit.edu)
[![Zenodo](https://zenodo.org/badge/DOI/10.5281/zenodo.19841974.svg)](https://doi.org/10.5281/zenodo.19841974)
[![JOSS](https://joss.theoj.org/papers/700e9010c4080ffe8ae4df21cf1cc899/status.svg)](https://joss.theoj.org/papers/700e9010c4080ffe8ae4df21cf1cc899)
[![Max. tested scale - 6400 cores](https://img.shields.io/badge/Max._tested_scale-6400_cores-red)](https://jair.org/index.php/jair/article/view/15827)
[![License - MIT or LGPL](https://img.shields.io/badge/License-MIT_or_LGPL-ffffbb)](#Licensing)

# Mallob

**Mallob** (**Mal**leable **Lo**ad **B**alancer, or **Ma**ssively P**a**ra**ll**el **Lo**gic **B**ackend) is a distributed platform for automated reasoning in modern large-scale HPC and cloud environments.

Mallob primarily solves instances of _propositional satisfiability_ (SAT) – an essential building block at the core of Symbolic AI. Mallob with its SAT solving engine **MallobSat** is leading the respective (massively) parallel tracks of the International SAT Competition since 2020. 

Each SAT solving task in Mallob can be _incremental_ (allowing for efficient interactive solving procedures over evolving formulas) and can produce and/or check _proof information_ (offering full confidence in each obtained result).

Mallob's flexible and decentralized approach to job scheduling allows to concurrently process many tasks of varying priority by different users.

Building upon Mallob's job scheduling and SAT solving capabilities, Mallob also features engines for state-of-the-art distributed **MaxSAT solving** (_MallobMax_) and bit-precise **SMT solving** (_Bitwuzllob_ - parallelizing [Bitwuzla](https://github.com/bitwuzla/bitwuzla)).


## Setup

Mallob uses MPI (Message Passing Interface) and is built using CMake.

For a default, full featured build, execute [`bash scripts/setup/cmake-make.sh build`](scripts/setup/build.sh).

[**Find detailed instructions at docs/setup.md.**](docs/setup.md)

### Docker

We also provide a setup based on Docker containerization. Please consult the (for now separate) documentation in the `docker/` directory.


## Usage

**Quick Start:**

If you just want to use Mallob on a single, parallel machine, then the script `scripts/run/mallob_local.sh` automatically retrieves a suitable process+thread configuration of Mallob for your hardware that makes use of the entire machine. Useful presets can be applied by calling the scripts at `config/presets/`. Examples:

```bash
# SAT solving (default, simple setup)
scripts/run/mallob_local.sh -mono=instances/r3unsat_300.cnf
# SAT solving (SAT Competition 2026 winning configuration, with Satsuma)
scripts/run/mallob_local.sh $(config/presets/satcomp26-quick) -mono=instances/r3unsat_300.cnf
# SAT solving (with real-time proof checking and assignment checking)
scripts/run/mallob_local.sh $(config/presets/satcomp26-safe) -mono=instances/r3unsat_300.cnf
# SMT solving
scripts/run/mallob_local.sh $(config/presets/smtcomp26) -mono=path/to/problem.smt2
# MaxSAT solving
scripts/run/mallob_local.sh -mono=path/to/problem.wcnf -mono-app=MAXSAT
```

**Always make sure to execute Mallob and its wrapper run scripts from Mallob's home directory**, otherwise Mallob will not find critical executables in `build/` and **will not work correctly**.

**More general settings:**

Run `build/mallob --help` for an overview of all Mallob options.

E.g., to run MallobSat with one single (MPI) process with twelve Kissat threads, you can execute `build/mallob -mono=path/to/problem.cnf -t=12 -satsolver=k`.

For multi-process and distributed execution, prepend the command by `mpirun` or `mpiexec` followed by appropriate MPI options, as returned by the script `scripts/run/mallob_local.sh` (see above).
E.g., using Open MPI, the following command runs Mallob as a service (taking JSON job submissions on demand at `.api/jobs.0/`) with a total of eight processes à four threads.

```bash
RDMAV_FORK_SAFE=1; mpirun --bind-to core --map-by ppr:8:node:pe=4 -np 8 build/mallob -t=4
```

[**Find more detailed instructions at docs/execute.md.**](docs/execute.md)  
For trouble-shooting, see also [FAQ:Execution](docs/faq.md#execution).

## Development and Debugging

[**Find detailed instructions at docs/develop.md.**](docs/develop.md)


## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).


## Licensing

First of all, **please let us know if you make use of Mallob!** We like to hear about it and depend on it for continued support and further development.

Mallob and its source code can be used, changed and redistributed under the terms of the [**MIT License**](/LICENSE_MIT) _or_ the [**Lesser General Public License (LGPLv3)**](/LICENSE_LGPL). (One exception is the Glucose interface, excluded from compilation by default - see below.)

Depending on the application engines included in the particular build, the Mallob executable includes a number of liberally licensed solvers and/or (pre-)processors, which are listed at the top of every Mallob execution output together with their main authors.  
There is also a Glucose interface for Mallob, which is subject to the [non-free license of (parallel-ready) Glucose](https://github.com/mi-ki/glucose-syrup/blob/master/LICENCE). Notably, its usage in competitive events is restricted. This interface is however **disabled** by default.

Note that a full build of Mallob may also download and build GPL-licensed software, which is then called by Mallob **as external software** (i.e., not compiled or integrated into the Mallob codebase). As of August 2026, this specifically concerns the Satsuma dependency Cliquer.

Within our codebase we further make thankful use of the following liberally licensed projects:

* [Compile Time Regular Expressions](https://github.com/hanickadot/compile-time-regular-expressions) by Hana Dusíková, for matching particular user inputs
* [JSON for Modern C++](https://github.com/nlohmann/json) by Niels Lohmann, for reading and writing JSON files
* [ringbuf](https://github.com/rmind/ringbuf) by Mindaugas Rasiukevicius, for efficient ring buffers
* [robin_hood hashing](https://github.com/martinus/robin-hood-hashing) by Martin Ankerl, for efficient unordered maps and sets
* [robin-map](https://github.com/Tessil/robin-map) by Thibaut Goetghebuer-Planchon, for efficient unordered maps and sets
* [SipHash C reference implementation](https://github.com/veorq/SipHash) by Jean-Philippe Aumasson, for Message Authentication during trusted distributed clause-sharing solving


## Bibliography

If you make use of Mallob in an academic / scientific setting or in a competitive event, please cite the most relevant / recent among [this list of publications](https://satres.kikit.kit.edu/publications/?filter=keyword%3AMallob) (all Open Access). A good, recent candidate is [CAV'26](https://satres.kikit.kit.edu/publications/?filter=title%3AMallob%3A+Scalable+Automated+Reasoning+On+Demand).


## Further references

* **[Mallob IPASIR Bridge for incremental SAT solving](https://github.com/domschrei/mallob-ipasir-bridge)**
* **[ImpCheck - Immediate Massively Parallel Propositional Proof Checking](https://github.com/domschrei/impcheck)**
* **[Comprehensive 2023 Experimental data at Zenodo](https://zenodo.org/doi/10.5281/zenodo.10184679)**
* **[Mallob at Helmholtz Research Software Directory (RSD)](https://helmholtz.software/software/mallob)**
