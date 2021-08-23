#!/bin/bash

if ! ping -c 1 google.com ; then echo "offline"; exit; fi
echo ""

wget -nc https://dominikschreiber.de/mergesat-patched.tar.gz
wget -nc https://www.labri.fr/perso/lsimon/downloads/softwares/glucose-syrup-4.1.tgz
wget -nc http://fmv.jku.at/yalsat/yalsat-03v.zip
wget -nc http://fmv.jku.at/lingeling/lingeling-bcj-78ebb86-180517.tar.gz
wget -nc https://dominikschreiber.de/cadical_clauseimport.tar.gz
