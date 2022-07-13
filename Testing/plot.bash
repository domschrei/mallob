#!/bin/bash
kList=(5 10 20 30 40)
for k in ${kList[@]}; do
    python3 scripts/plot/plot_curves.py -xy ./Testing/times-${k}.txt -l="k=${k}" -o=./Testing/Graph${k}.pdf
done

