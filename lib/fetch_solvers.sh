#!/bin/bash

solvers=$1

if echo $solvers|grep -q "m"; then wget -nc https://dominikschreiber.de/mergesat-patched.tar.gz ; fi
if echo $solvers|grep -q "g"; then wget -nc https://www.labri.fr/perso/lsimon/downloads/softwares/glucose-syrup-4.1.tgz ; fi
if echo $solvers|grep -q "y"; then wget -nc http://fmv.jku.at/yalsat/yalsat-03v.zip ; fi
if echo $solvers|grep -q "l" && [ ! -d lingeling ]; then wget -nc https://dominikschreiber.de/share/lingeling-isc22.zip ; fi
if echo $solvers|grep -q "c" && [ ! -d cadical ]; then git clone git@github.com:domschrei/cadical.git && cd cadical && git checkout 20a9484 && cd .. ; fi
if echo $solvers|grep -q "p" && [ ! -d lrat-cadical ]; then 
    git clone https://github.com/domschrei/cadical.git lrat-cadical
    cd lrat-cadical
    git checkout raog
    cd ..
fi
if echo $solvers|grep -q "k" && [ ! -d kissat ]; then wget -nc https://github.com/domschrei/kissat/archive/refs/heads/master.zip && mv master.zip kissat-isc22.zip ; fi
