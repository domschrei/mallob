#!/bin/bash

solvers=$1

# MergeSAT
if echo $solvers|grep -q "m"; then
    if [ ! -d mergesat ]; then
        if [ ! -f mergesat-patched.tar.gz ]; then
            wget -nc https://dominikschreiber.de/mergesat-patched.tar.gz
        fi
        tar xzvf mergesat-patched.tar.gz
    fi
fi

# Glucose
if echo $solvers|grep -q "g"; then
    if [ ! -d glucose ]; then
        if [ ! -f glucose-syrup-4.1.tgz ]; then
            wget -nc https://www.labri.fr/perso/lsimon/downloads/softwares/glucose-syrup-4.1.tgz
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
            wget -nc http://fmv.jku.at/yalsat/yalsat-03v.zip
        fi
        unzip yalsat-03v.zip
        mv yalsat-03v yalsat
    fi
fi

# Lingeling
if echo $solvers|grep -q "l"; then
    if [ ! -d lingeling ]; then
        if [ ! -f lingeling-isc22.zip ]; then
            wget -nc https://dominikschreiber.de/share/lingeling-isc22.zip
        fi
        unzip lingeling-isc22.zip
        mv lingeling-*/ lingeling
    fi
fi

# Kissat
if echo $solvers|grep -q "k"; then
    if [ ! -d kissat ]; then
        if [ ! -f kissat.zip ]; then
            # for fixing a branch instead of a commit, prepend "refs/heads/"
            branchorcommit="9d4beeb181128a4f2fa0856ef8f620d4b6d54f16"
            wget -nc https://github.com/domschrei/kissat/archive/${branchorcommit}.zip -O kissat.zip
        fi
        unzip kissat.zip
        mv kissat-* kissat
    fi
fi

# Normal (non-LRAT) CaDiCaL
if echo $solvers|grep -q "c"; then
    if [ ! -d cadical ]; then
        if [ ! -f cadical.zip ]; then
            # for fixing a branch instead of a commit, prepend "refs/heads/"
            branchorcommit="0bb5565ea61e8c0b356cc9df85f80baa1b82127b"
            wget -nc https://github.com/domschrei/cadical/archive/${branchorcommit}.zip -O cadical.zip
        fi
        unzip cadical.zip
        mv cadical-* cadical
    fi
fi

# LRAT-logging CaDiCaL
if echo $solvers|grep -q "p"; then
    if [ ! -d lrat-cadical ]; then
        if [ ! -f lrat-cadical.zip ]; then
            branchorcommit="refs/heads/certified-integrated"
            wget -nc https://github.com/domschrei/cadical/archive/${branchorcommit}.zip -O lrat-cadical.zip
        fi
        unzip lrat-cadical.zip
        mv cadical-* lrat-cadical
    fi
fi
