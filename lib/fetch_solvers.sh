#!/bin/bash

solvers=$1

if echo $solvers|grep -q "m"; then wget -nc https://dominikschreiber.de/mergesat-patched.tar.gz ; fi
if echo $solvers|grep -q "g"; then wget -nc https://www.labri.fr/perso/lsimon/downloads/softwares/glucose-syrup-4.1.tgz ; fi
if echo $solvers|grep -q "y"; then wget -nc http://fmv.jku.at/yalsat/yalsat-03v.zip ; fi
if echo $solvers|grep -q "l"; then wget -nc http://fmv.jku.at/lingeling/lingeling-bcj-78ebb86-180517.tar.gz ; fi
if echo $solvers|grep -q "c"; then git clone git@github.com:domschrei/cadical.git ; fi
