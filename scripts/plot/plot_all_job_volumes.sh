#!/bin/bash

for i in {1..1000}; do

    if [ ! -f logs/latest/assignment_history_#$i ]; then continue; fi
    if [ ! -f logs/latest/volume_history_#$i ]; then continue; fi

    python3 scripts/plot_curves.py -xy logs/latest/assignment_history_#$i -l="Assignment" logs/latest/volume_history_#$i -l="Actual volume" -nomarkers -o=logs/latest/assignment_vs_volume_#$i.png
    
done
