#!/bin/bash

solvers=$1

echo "Cloning FRAT proof checker - eventually we will want to choose proof checker"
git clone https://github.com/digama0/frat.git

if echo $solvers|grep -q "m"; then wget -nc https://dominikschreiber.de/mergesat-patched.tar.gz ; fi
if echo $solvers|grep -q "g"; then wget -nc https://www.labri.fr/perso/lsimon/downloads/softwares/glucose-syrup-4.1.tgz ; fi
if echo $solvers|grep -q "y"; then wget -nc http://fmv.jku.at/yalsat/yalsat-03v.zip ; fi
#if echo $solvers|grep -q "l"; then wget -nc http://fmv.jku.at/lingeling/lingeling-bcj-78ebb86-180517.tar.gz ; fi
#if echo $solvers|grep -q "l" && [ ! -d lingeling ]; then git clone git@github.com:arminbiere/lingeling.git && cd lingeling && git checkout 708beb2 && cd .. ; fi
#if echo $solvers|grep -q "c" && [ ! -d cadical ]; then git clone git@github.com:domschrei/cadical.git && cd cadical && git checkout 20a9484 && cd .. ; fi
#if echo $solvers|grep -q "k" && [ ! -d kissat ]; then git clone git@github.com:domschrei/kissat.git && cd kissat && git checkout clauseexport && git checkout f2d7275 && cd .. ; fi

if echo $solvers|grep -q "l" && [ ! -d lingeling ]; then wget -nc https://dominikschreiber.de/share/lingeling-isc22.zip ; fi
if echo $solvers|grep -q "c" && [ ! -d cadical ]; then
    git clone https://github.com/RandomActsOfGrammar/cadical.git
    cd cadical
    git checkout compaction
    cd ..
fi
if echo $solvers|grep -q "k" && [ ! -d kissat ]; then wget -nc https://github.com/domschrei/kissat/archive/refs/heads/master.zip && mv master.zip kissat-isc22.zip ; fi
