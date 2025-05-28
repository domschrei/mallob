
[![KIT - SAtRes group](https://img.shields.io/badge/KIT-SAtRes_group-009682)](https://satres.kikit.kit.edu/research/mallob/)
[![Helmholtz RSD - /software/mallob](https://img.shields.io/badge/Helmholtz_RSD-%2Fsoftware%2Fmallob-002864)](https://satres.kikit.kit.edu)
[![Zenodo](https://zenodo.org/badge/DOI/10.5281/zenodo.6890239.svg)](https://doi.org/10.5281/zenodo.6890239)
[![JOSS](https://joss.theoj.org/papers/700e9010c4080ffe8ae4df21cf1cc899/status.svg)](https://joss.theoj.org/papers/700e9010c4080ffe8ae4df21cf1cc899)
[![Max. tested scale - 6400 cores](https://img.shields.io/badge/Max._tested_scale-6400_cores-red)](https://jair.org/index.php/jair/article/view/15827)
[![License - LGPL (or ask us)](https://img.shields.io/badge/License-LGPL_(or_ask_us)-ffffbb)](#Licensing)

# Mallob

**Mallob** (**Mal**leable **Lo**ad **B**alancer, or **Ma**ssively P**a**ra**ll**el **Lo**gic **B**ackend) is a distributed platform for automated reasoning in modern large-scale HPC and cloud environments. Mallob primarily solves instances of _propositional satisfiability_ (SAT) – an essential building block at the core of Symbolic AI. Mallob's flexible and decentralized approach to job scheduling allows to concurrently process many tasks of varying priority by different users. As such, Mallob can be used to scale up academic or industrial workflows tied to automated reasoning.

Mallob and its tightly integrated distributed general-purpose SAT solving engine, **MallobSat**, received a large amount of attention, including five gold medals in the [International SAT Competition](https://satcompetition.github.io/)'s Cloud Track in a row, [high-profile scientific awards](https://www.informatik.kit.edu/english/11147_14198.php), and a [highlight on Amazon Science](https://www.amazon.science/blog/automated-reasonings-scientific-frontiers).
Mallob is the first distributed system that supports _incremental SAT solving_, i.e., interactive solving procedures over evolving formulas, and is also the first system transferring _proof technology_ to parallel and distributed SAT solving in a scalable manner.


## Setup

Mallob uses MPI (Message Passing Interface) and is built using CMake.

For a default quick-start build, execute [`scripts/setup/build.sh`](scripts/setup/build.sh) (and/or modify it as needed).

[**Find detailed instructions at docs/setup.md.**](docs/setup.md)

### Docker

We also provide a setup based on Docker containerization. Please consult the (for now separate) documentation in the `docker/` directory.

### Bash Autocompletion

To enable bash auto-completion by pressing TAB, execute `source scripts/run/autocomplete.sh` from Mallob's base directory.
You should now be able to autocomplete program options by pressing TAB once or twice from this directory.


## Usage

**Quick Start:**

Run `build/mallob --help` for an overview of all Mallob options.
E.g., to run MallobSat with a single (MPI) process with twelve Kissat threads, execute `build/mallob -mono=path/to/problem.cnf -t=12 -satsolver=k`. Make sure to execute Mallob from it's home directory, otherwise some relative paths might not work per default.

For trouble-shooting, see also [FAQ:Execution](docs/faq.md#execution).

For multi-process and distributed execution, prepend the command by `mpirun` or `mpiexec` followed by appropriate MPI options.
E.g., using Open MPI, the following command runs Mallob as a service (taking JSON job submissions on demand at `.api/jobs.0/`) with a total of eight processes à four threads.

```bash
RDMAV_FORK_SAFE=1; mpirun -np 8 --bind-to core --map-by ppr:1:node:pe=4 build/mallob -t=4
```

[**Find detailed instructions at docs/execute.md.**](docs/execute.md)


## Development and Debugging

[**Find detailed instructions at docs/develop.md.**](docs/develop.md)


## Licensing

The source code of Mallob can be used, changed and redistributed under the terms of the **Lesser General Public License (LGPLv3)**, one exception being the Glucose interface which is excluded from compilation by default (see below).
**Please approach us if you require a deviating license.**

The used versions of Lingeling, YalSAT, CaDiCaL, and Kissat are MIT-licensed, as is HordeSat (the massively parallel solver system our SAT engine was based on) and the proof-related tools which are included and/or fetched in the `tools/` directory.

The Glucose interface of Mallob (only included when explicitly compiled with `-DMALLOB_USE_GLUCOSE=1`) is subject to the [non-free license of (parallel-ready) Glucose](https://github.com/mi-ki/glucose-syrup/blob/master/LICENCE). Notably, its usage in competitive events is restricted.

Within our codebase we make thankful use of the following liberally licensed projects:

* [robin-map](https://github.com/Tessil/robin-map) by Thibaut Goetghebuer-Planchon, for efficient unordered maps and sets
* [libcuckoo](https://github.com/efficient/libcuckoo) by Manu Goyal et al., for concurrent hash tables
* [JSON for Modern C++](https://github.com/nlohmann/json) by Niels Lohmann, for reading and writing JSON files
* [Compile Time Regular Expressions](https://github.com/hanickadot/compile-time-regular-expressions) by Hana Dusíková, for matching particular user inputs
* [robin_hood hashing](https://github.com/martinus/robin-hood-hashing) by Martin Ankerl, for efficient unordered maps and sets


## Bibliography

If you make use of Mallob in an academic setting or in a competitive event, please cite the most relevant among the following publications.

#### JAIR'24: Focus on SAT Solving
```bibtex
@article{schreiber2024mallobsat,
	title={{MallobSat}: Scalable {SAT} Solving by Clause Sharing},
	author={Schreiber, Dominik and Sanders, Peter},
	journal={Journal of Artificial Intelligence Research (JAIR)},
	year={2024},
	volume={80},
	pages={1437--1495},
	doi={10.1613/jair.1.15827},
}
```
#### Euro-Par'22: Focus on job scheduling
```bibtex
@inproceedings{sanders2022decentralized,
  title={Decentralized Online Scheduling of Malleable {NP}-hard Jobs},
  author={Sanders, Peter and Schreiber, Dominik},
  booktitle={International European Conference on Parallel and Distributed Computing},
  pages={119--135},
  year={2022},
  organization={Springer},
  doi={10.1007/978-3-031-12597-3_8}
}
```
#### TACAS'23: Proofs of unsatisfiability
```bibtex
@InProceedings{michaelson2023unsatisfiability,
  author={Michaelson, Dawn and Schreiber, Dominik and Heule, Marijn J. H. and Kiesl-Reiter, Benjamin and Whalen, Michael W.},
  title={Unsatisfiability proofs for distributed clause-sharing SAT solvers},
  booktitle={Tools and Algorithms for the Construction and Analysis of Systems (TACAS)},
  year={2023},
  organization={Springer},
  pages={348--366},
  doi={10.1007/978-3-031-30823-9_18},
}
```
#### SAT'24: Real-time proof checking with MallobSat + ImpCheck
```bibtex
@inproceedings{schreiber2024trusted,
	title={Trusted Scalable {SAT} Solving with on-the-fly {LRAT} Checking},
	author={Schreiber, Dominik},
	booktitle={Theory and Applications of Satisfiability Testing (SAT)},
	year={2024},
	pages={25:1--25:19},
	organization={Schloss Dagstuhl -- Leibniz-Zentrum für Informatik},
	doi={10.4230/LIPIcs.SAT.2024.25},
}
```
#### Most recent SAT Competition TR
```bibtex
@inproceedings{schreiber2024mallob,
	title={{MallobSat} and {MallobSat-ImpCheck} in the {SAT} Competition 2024},
	author={Schreiber, Dominik},
	booktitle={SAT Competition 2024: Solver, Benchmark and Proof Checker Descriptions},
	year={2024},
	pages={21--22},
	note={\url{http://hdl.handle.net/10138/584822}},
}
```
#### Doctoral thesis (1st presentation of Incremental Distributed SAT)
```bibtex
@phdthesis{schreiber2023scalable,
  author={Dominik Schreiber},
  title={Scalable {SAT} Solving and its Application},
  year={2023},
  school={Karlsruhe Institute of Technology},
  doi={10.5445/IR/1000165224}
}
```


## Further references

* **[Mallob IPASIR Bridge for incremental SAT solving](https://github.com/domschrei/mallob-ipasir-bridge)**
* **[ImpCheck - Immediate Massively Parallel Propositional Proof Checking](https://github.com/domschrei/impcheck)**
* **[Experimental data at Zenodo](https://zenodo.org/doi/10.5281/zenodo.10184679)**
* **[Mallob at Helmholtz Research Software Directory (RSD)](https://helmholtz.software/software/mallob)**
