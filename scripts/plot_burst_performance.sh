#!/bin/bash

python3 scripts/plot_curves.py RESULTS/mallob_sched_burst_cfhl90_2/cdf_donetimes -l='Mallob $J=\infty$' RESULTS/kissat_all/cdf_runtimes -l='$400\times$Kissat' RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_all_n128/cdf_cumulative_runtimes -l="HOSS" RESULTS/lingeling_all/cdf_runtimes -l='$400\times$Lingeling' -xy -xsize=3.75 -ysize=2.53125 -xmin=0 -xmax=7200 -ymin=0 -xlabel="Elapsed time / s" -ylabel='\# solved instances' -o=burst-performance.pdf -nomarkers
