---
title: 'Mallob: Scalable SAT Solving On Demand With Decentralized Job Scheduling'
tags:
  - C++
  - online job scheduling
  - malleability
  - load balancing
  - propositional satisfiability
  - SAT solving
  - parallel processing
  - HPC
authors:
  - name: Peter Sanders
    orcid: 0000-0003-3330-9349
    affiliation: 1
  - name: Dominik Schreiber
    orcid: 0000-0002-4185-1851
    affiliation: 1
affiliations:
 - name: Karlsruhe Institute of Technology, Germany
   index: 1
date: 30 June 2022
bibliography: paper.bib
---

# Summary

Propositional satisfiability (SAT) is the problem of finding a variable assignment for a given propositional formula (i.e., a composition of Boolean variables using logical operators NOT, AND, OR) such that the formula evaluates to **true**, or reporting that no such assignment exists. The platform **Mallob** (**Mal**leable **Lo**ad **B**alancer, or **Ma**ssively P**a**ra**ll**el **Lo**gic **B**ackend) allows to process SAT jobs in a (massively) parallel and distributed system on demand. Mallob's flexible, fair, and decentralized approach to online job scheduling results in scheduling latencies in the range of milliseconds, near-optimal system utilization, and high resource efficiency.

# Statement of need

Despite SAT being a notoriously difficult problem [@cook1971complexity], practically efficient SAT solving approaches have empowered a wide range of real-world applications for SAT such as software verification [@buning2019using], circuit design [@goldberg2001using], cryptography [@massacci2000logical], automated planning [@schreiber2021lilotane], and theorem proving [@heule2016solving].
With respect to modern computing paradigms such as cloud computing and high-performance computing (HPC), the limited scalability of established parallel and distributed SAT solvers has become a pressing issue [@hamadi2012seven].
Moreover, since processing times of SAT jobs are unknown in advance, conventional HPC scheduling approaches applied to such jobs can lead to prohibitively large scheduling latencies and to suboptimal utilization of computational resources. Instead, we suggest to make use of so-called _malleable scheduling_. A parallel computing task is called malleable if the amount of computational resources allotted (i.e., the number of cores or machines) can be adjusted during its execution [@feitelson1997job]. Malleability is a powerful paradigm in the field of job scheduling and load balancing as it allows to schedule incoming jobs rapidly and to continuously rebalance the present tasks according to their priority and other properties. 

We believe that a cloud service which combines a scalable SAT solving engine with malleable job scheduling can significantly improve many SAT-related academic and industrial workflows in terms of productivity and (resource-)efficiency. Moreover, our decentralized scheduling approach is applicable to further applications beyond SAT where processing times are unknown in advance and a modest amount of data is transferred between the workers.

# System overview

Mallob is a C++ application designed for parallel and distributed systems between 16 and 10000 cores and allows to resolve propositional problems in a (massively) parallel manner. Our SAT solving engine scales up to two thousand cores [@schreiber2021scalable] using a portfolio of established sequential SAT solvers [@biere2017cadical; @biere2020cadical; @audemard2009predicting] and a careful exchange of information among them.
Our submissions of Mallob to the International SAT Competitions [@schreiber2020engineering;@schreiber2021mallob] won multiple prizes [@froleyks2021sat;@balyo2021results]. Following this success, Mallob has been referred to as "by a _wide_ margin, the most powerful SAT solver on the planet" [@cook2022automated].
Each distributed SAT solving task is malleable, allowing users to submit formulae to Mallob at will with scheduling latencies in the range of milliseconds [@sanders2022decentralized]. Computational resources are allotted proportional to each job's priority and also respecting each job's (maximum) demand of resources. Our scheduling approach is fully decentralized and uses a small part of each worker's CPU time (< 5%) to perform scheduling negotiations. As such, Mallob achieves optimal system utilization, i.e., either all processes are utilized or all resource demands are fully met. We achieve this feat by arranging each active job as a binary tree of workers and growing or shrinking each job tree dynamically based on the current system state.
For an in-depth discussion of these techniques, we refer to @schreiber2021scalable, @sanders2022artifact, and @sanders2022decentralized.

![Technology stack of Mallob.\label{fig:mallob-stack}](mallob_stack.pdf){ width=65% }

The further development of Mallob is an ongoing effort. As such, we are in the process of integrating engines for NP-hard applications beyond SAT, such as k-Means clustering or hierarchical planning, into Mallob.

# Acknowledgements

We wish to thank the numerous people whose code we make thankful use of, including @balyo2015hordesat, @biere2020cadical, @audemard2009predicting, @een2003extensible, @lohmann2022json, @goetghebuer2022implementation, @ankerl2022robin, @dusikova2022compile, and @rasiukevicius2019lockfree.
The authors gratefully acknowledge the Gauss Centre for Supercomputing e.V. (www.gauss-centre.eu) for funding this project by providing computing time on the GCS Supercomputer SuperMUC-NG at Leibniz Supercomputing Centre (www.lrz.de). Moreover, some of this work was performed on the HoreKa supercomputer funded by the Ministry of Science, Research and the Arts Baden-Württemberg and by the Federal Ministry of Education and Research.
This project has received funding from the European Research Council (ERC) under the European Union’s Horizon 2020 research and innovation programme (grant agreement No. 882500).

![](logo_erc_eu_tight.jpg){ width=30% }

# References 
