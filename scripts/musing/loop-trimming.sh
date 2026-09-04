#!/bin/bash
INPUT_CNF="$1"
LOOPS="$2"
TMP="tmp"
mkdir -p $TMP
CNF="$TMP/tmp.cnf"
CORE="unsatcoreIDs.txt"
cp $INPUT_CNF $CNF
echo "ORIG $(cat $CNF | wc -l)"
for ((i=1; i<=$LOOPS; i++)); do
  echo "Loop $i"
  ./scripts/musing/run-trimming.sh $CNF
  NEWTMP="$TMP/tmp$i.cnf"
  ./scripts/musing/ids2cnf.sh $CNF $CORE $NEWTMP
  echo "CORE $(cat $NEWTMP | wc -l)"
  cp $CORE "$TMP/core$i.txt"
  rm $CORE
  CNF=$NEWTMP
done
