#!/bin/bash

solvers=$1
switch_to_sweep_kissat=$2

# MergeSAT
if echo $solvers|grep -q "m"; then
    if [ ! -d mergesat ]; then
        if [ ! -f mergesat-patched.tar.gz ]; then
           #wget -nc https://dominikschreiber.de/mergesat-patched.tar.gz
	       curl -O https://dominikschreiber.de/mergesat-patched.tar.gz
        fi
        tar xzvf mergesat-patched.tar.gz
    fi
fi

# Glucose
if echo $solvers|grep -q "g"; then
    if [ ! -d glucose ]; then
        if [ ! -f glucose-syrup-4.1.tgz ]; then
            curl -O https://www.labri.fr/perso/lsimon/downloads/softwares/glucose-syrup-4.1.tgz
            #wget -nc https://www.labri.fr/perso/lsimon/downloads/softwares/glucose-syrup-4.1.tgz
        fi
        tar xzvf glucose-syrup-4.1.tgz
        rm ._glucose-syrup-4.1
        mv glucose-syrup-4.1 glucose
    fi
fi

# YalSAT
if echo $solvers|grep -q "y"; then
    if [ ! -d yalsat ]; then
        if [ ! -f yalsat-03v.zip ]; then
		  curl -L -o yalsat-03v.zip -C - http://fmv.jku.at/yalsat/yalsat-03v.zip
          #wget -nc http://fmv.jku.at/yalsat/yalsat-03v.zip
        fi
        unzip yalsat-03v.zip
        mv yalsat-03v yalsat
    fi
fi

# Lingeling
if echo $solvers|grep -q "l"; then
    if [ ! -d lingeling ]; then
        if [ ! -f lingeling.zip ]; then
        # for fixing a branch instead of a commit, prepend "refs/heads/"
	    branchorcommit="89a167d0d2efe98d983c87b5b84175b40ea55842" # version 1.0.0, March 2024
        curl -L -o lingeling.zip https://github.com/arminbiere/lingeling/archive/${branchorcommit}.zip
        #wget -nc https://github.com/arminbiere/lingeling/archive/${branchorcommit}.zip -O lingeling.zip
	    fi
        unzip lingeling.zip
        mv lingeling-* lingeling
    fi
fi


# Kissat
if echo $solvers|grep -q "k"; then
    if [ ! -d kissat ]; then
        if [ ! -f kissat.zip ]; then
            # for fixing a branch instead of a commit, prepend "refs/heads/"
            echo "switch to sweep kissat " $switch_to_sweep_kissat
            if [ $switch_to_sweep_kissat="1" ]; then
                curl -L -o kissat.zip https://github.com/nrilu/kissat/archive/refs/heads/update24.zip
                echo "Downloading Nicco's Kissat Fork for Equivalence Sweeping"
            else
                branchorcommit="53b0ce61b0ce8b1d91e5c302d8060f8597364137" # updated 2024-04-02
                curl -L -o kissat.zip https://github.com/domschrei/kissat/archive/${branchorcommit}.zip
            #wget -nc https://github.com/domschrei/kissat/archive/${branchorcommit}.zip -O kissat.zip
            fi
		fi
        unzip kissat.zip
        mv kissat-* kissat
    fi
fi

# CaDiCaL
if echo $solvers|grep -q "c"; then
    if [ ! -d cadical ]; then
        if [ ! -f cadical.zip ]; then
            # for fixing a branch instead of a commit, prepend "refs/heads/"
            branchorcommit="6cc6ad0de76cbad1ec04ad4f9ec22bcf1e4b02d0"
            curl -L -o cadical.zip https://github.com/domschrei/cadical/archive/${branchorcommit}.zip
            #wget -nc https://github.com/domschrei/cadical/archive/${branchorcommit}.zip -O cadical.zip
        fi
        unzip cadical.zip
        mv cadical-* cadical
    fi
fi
