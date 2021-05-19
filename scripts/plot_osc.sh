#!/bin/bash

scripts/plot_curves.py -xy -xmin=0 -xmax=300 -ymin=30 -ymax=65 -xsize=3.7 -ysize=2.3125 -nomarkers \
RESULTS/mallob_sched_osc_cfhl0_big/cdf_runtimes -l='$64{\times}6{\times}4$' \
RESULTS/mallob_sched_osc_cfhl0_2/cdf_runtimes -l='$64{\times}6{\times}4$ disturbed' \
RESULTS/mallob_sched_osc_cfhl0_small/cdf_runtimes -l='$32{\times}6{\times}4$' \
-xlabel='Run time $t$ / s' -ylabel='\# instances solved in $\leq t$~s' -o=disturbance.pdf
#RESULTS/mallob_sched_osc_cfhl90_2/cdf_runtimes -l='$64{\times}6{\times}4$ disturbed $X=90$' \
#RESULTS/mallob_sched_osc_cfhl30_2/cdf_runtimes -l='$64{\times}6{\times}4$ disturbed $X=30$' \
#RESULTS/mallob_sched_osc_cfhl10/cdf_runtimes -l='$64{\times}6{\times}4$ disturbed $X=10$' \


