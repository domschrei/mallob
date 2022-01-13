#!/bin/bash

python3 scripts/plot_curves.py -xy -nomarkers -ymin=0 -ymax=310 -xmin=0 -xmax=300 \
RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_all_n128/cdf_runtimes -l='Mallob $\alpha=7/8$' \
RESULTS/horde_new_all/cdf_runtimes -l='HordeSat (new)' \
-xlabel='Run time $t$ / s' -ylabel="\\# instances solved in $\\leq t$ s" \
-xsize=3.5 -ysize=2.8 -o=horde_mallob_400instances.pdf

