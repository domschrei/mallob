#!/bin/bash

# Table of average / median response times, other measures??
echo ' & \multicolumn{2}{c}{Response times (all)} & \multicolumn{2}{c}{Response times (slv.)} \\'
echo 'Configuration & avg. & med. & avg. & med.\\'

for f in RESULTS/mallob_sched_burst_cfhl90_2/cdf_donetimes RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_all_n128/cdf_cumulative_runtimes RESULTS/kissat_all/cdf_runtimes RESULTS/lingeling_all/cdf_runtimes; do
    echo -ne ' & '$(cat $f|awk '$1 <= 7200 {s+=$1} END {s+=(400-NR)*7200; printf("%.1f\n",s/400)}')
    if [ $(cat $f|wc -l) -ge 200 ]; then 
        echo -ne ' & '$(head -200 $f|tail -1|awk '{printf("%.1f\n",$1)}')
    else 
        echo -ne ' & 7200'
    fi
    echo -ne ' & '$(cat $f|awk '$1 <= 7200 {s+=$1} END {printf("%.1f\n",s/NR)}')
    echo -ne ' & '$(head -$(($(cat $f|wc -l)/2)) $f|tail -1|awk '{printf("%.1f\n",$1)}')' \\\\'
    echo
done


exit

python3 scripts/plot_curves.py \
RESULTS/mallob_sched_burst_cfhl90_2/cdf_donetimes -l='Mallob $J=\infty$' \
RESULTS/kissat_all/cdf_runtimes -l='$400\times$Kissat' \
RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_all_n128/cdf_cumulative_runtimes -l="Opt. sched. Mallob-mono" \
RESULTS/lingeling_all/cdf_runtimes -l='$400\times$Lingeling' \
-xy -xsize=4 -ysize=2.7 -xmin=0 -xmax=7200 -ymin=0 \
-xlabel="Run time / s" -ylabel='\# solved instances' -o=burst-performance.pdf
cp burst-performance.pdf ../paper_sat2021_mallob/

python3 scripts/plot_curves.py \
RESULTS/mallob_sched_burst_cfhl90_2/cores_per_job -l='Mallob $J=\infty$' \
-xy -xsize=4 -ysize=2.7 -xmin=0 -xmax=7200 -ymin=0 -ymax=30 -nomarkers -nolinestyles -lw=2 \
-xlabel="Run time / s" -ylabel="Average \# cores per job" -o=burst-cores-per-job.pdf
cp burst-cores-per-job.pdf ../paper_sat2021_mallob/


exit

python3 report_optimal_schedulers.py

for J in 4 16 64; do
    cat RESULTS/mallob_sched_uniform${J}_g0_cfhl90/donetimes|awk '{print $3,NR}' > RESULTS/mallob_sched_uniform${J}_g0_cfhl90/cdf_donetimes
done

python3 scripts/plot_curves.py -xy \
RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n128/optimal_scheduler_times -l="m=128 opt" \
RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n32/optimal_scheduler_times -l="m=32 opt" \
RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n8/optimal_scheduler_times -l="m=8 opt" \
RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n2/optimal_scheduler_times -l="m=2 opt" \
RESULTS/mallob_sched_uniform4_g0_cfhl90/cdf_donetimes -l='sched $J=4$' \
RESULTS/mallob_sched_uniform16_g0_cfhl90/cdf_donetimes -l='sched $J=16$' \
RESULTS/mallob_sched_uniform64_g0_cfhl90/cdf_donetimes -l='sched $J=64$' \
RESULTS/mallob_sched_burst_cfhl90_selection/cdf_donetimes -l='$m=128, j=80, J=\infty$, T=7200'


#RESULTS/mallob_sched_burst_cfhl90/cdf_donetimes -l='$m=128, J=\infty$' \
#RESULTS/mallob_sched_burst_cfhl90_2/cdf_donetimes -l='$m=128, j=400, J=\infty$, T=7200' \
