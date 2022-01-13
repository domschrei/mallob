#!/bin/bash

timeout=300

# RESULTS/horde_new_n{1,2,8,32,128} RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n{1,2,8,32,128}
#if false; then
horde=false

# RESULTS/horde_* RESULTS/mallob_mono_*

# RESULTS/horde_new_n{1,2,8,32,128} RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n{1,2,8,32,128}

for d in RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_all_n128 ; do

    if echo $d|grep -q "horde"; then horde=true; else horde=false; fi

    > $d/runtimes
    > $d/avg_comm_volumes
    > $d/conflicts
    > $d/props
    > $d/cdf_runtimes
    > $d/speedups_kissat
    > $d/speedups_lingeling
    
    solved=0
    solved_sat=0
    solved_unsat=0
    score=0
    
    for i in {1..400}; do
        
        f=$(echo $d/*_i${i}_*log)
        
        echo $d $i $f
        
        # Report solved time
        if $horde; then
            solveline=$(cat $f|grep "glob")
            if echo $solveline|grep -q "res:10"; then
                solved_sat=$((solved_sat+1))
            elif echo $solveline|grep -q "res:20"; then
                solved_unsat=$((solved_unsat+1))
            fi
            solvetime=$(cat $f|grep "solved:1"|grep -oE "^\[[0-9\.]+\]"|head -1|grep -oE "[0-9\.]+")
        else
            solveline=$(cat $f|grep -oE "found result (SAT|UNSAT)"|head -1)
            
            solvetime=$(cat $f|grep "found result"|grep -oE "^c [0-9\.]+ "|head -1|grep -oE "[0-9\.]+")
            if [ "$solvetime" != "" ]; then
                starttime=$(cat $f|grep " 0 Program options"|head -1|awk '{print $2}')
                solvetime=$(echo "$solvetime - $starttime"|bc -l)
                if echo $solveline|grep -q UNSAT; then
                    solved_unsat=$((solved_unsat+1))
                else
                    solved_sat=$((solved_sat+1))
                fi
            fi
        fi
        if [ "$solvetime" != "" ]; then
            echo $i xxx $solvetime >> $d/runtimes
            score=$(echo "$score + $solvetime"|bc -l)
            solved=$((solved+1))
            
            for slv in kissat lingeling; do
                rt_seq=$(grep -E "^${i} " RESULTS/$slv/runtimes|awk '{print $3}')
                if [ "$rt_seq" == "" ]; then
                    rt_seq=50000
                fi
                echo $i xxx $(echo "$rt_seq / $solvetime"|bc -l) $rt_seq $solvetime >> $d/speedups_$slv
            done
        else
            score=$(echo "$score + 2 * $timeout"|bc -l)
        fi
        
        if ! $horde; then
            commvol=$(cat $f|grep ":0 : broadcast s="|grep -oE "s=[0-9]+"|grep -oE "[0-9]+"|awk 'BEGIN {c=0} {c++; s+=$1} END {print c,s}')
            if [ $(echo $commvol|awk '{print $1}') -gt 0 ]; then
                commvol_avg=$(echo $commvol|awk '{print $2/$1}')
                echo $i xxx $commvol_avg >> $d/avg_comm_volumes
            fi
            
            solverstats=$(cat $f|grep " END S")
            
            conflicts=$(echo $solverstats|grep -oE "cnfs:[0-9]+"|grep -oE "[0-9]+"|awk '{s+=$1} END {print s}')
            if [ "$conflicts" != "" ]; then
                echo $i xxx $conflicts >> $d/conflicts
            fi
            
            props=$(echo $solverstats|grep -oE "pps:[0-9]+"|grep -oE "[0-9]+"|awk '{s+=$1} END {print s}')
            if [ "$props" != "" ]; then
                echo $i xxx $props >> $d/props
            fi
        fi
    done
    
    echo $score > $d/par2score
    all_solved=$((solved_sat+solved_unsat))
    if [ $all_solved -ne $solved ]; then
        echo "ERROR: $solved_sat + $solved_unsat != $solved"
        exit 1
    fi
    echo $d : PAR2=$score solved=$solved SAT=$solved_sat UNSAT=$solved_unsat
    echo $solved_sat > $d/solved_sat
    echo $solved_unsat > $d/solved_unsat
    
    # Compute CDF of runtimes
    i=1
    cat $d/runtimes|awk '{print $3}'|sort -g|while read -r line; do
        echo $line $i >> $d/cdf_runtimes
        i=$((i+1))
    done
done

exit

#fi

#d1=RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n32
#d2=RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_sortlbd_n32
#python3 scripts/plot_1v1.py $d1/runtimes -l="$d1" $d2/runtimes -l="$d2" -T=300
#python3 scripts/plot_1v1.py $dir1/avg_comm_volumes -l="$dir1" $dir2/avg_comm_volumes -l="$dir2" -T=1000000
#python3 scripts/plot_1v1.py $dir1/conflicts -l="$dir1" $dir2/conflicts -l="$dir2" -T=9999999999
#python3 scripts/plot_1v1.py $dir1/props -l="$dir1" $dir2/props -l="$dir2" -T=999999999999

#cmd='python3 scripts/plot_curves.py -xy '
#for d in RESULTS/horde_old_n128/ RESULTS/horde_new_n128/ RESULTS/#mallob_mono_{basic,cbdf0.875,cbdf0.875_mlbd0_mcl10,cbdf0.875_cfhl90}_n128 ; do 
#    c=$(echo $d|sed 's,RESULTS/,,g'|sed 's/_/-/g')
#    cmd="$cmd $d/cdf_runtimes -l=$c"
#done
#$cmd -xlabel="Run time / s" -ylabel="\\# instances solved in $\\leq t$ s"

#python3 scripts/plot_curves.py -xy \
#RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_all_n128/cdf_runtimes -l='Mallob $\alpha=7/8$' \
#RESULTS/horde_new_all/cdf_runtimes -l='Hordesat (new)' \
#-xlabel='Run time $t$ / s' -ylabel="\\# instances solved in $\\leq t$ s" \
#-xsize=4.4 -ysize=3.4 -o=horde_mallob_400instances.pdf
#cp horde_mallob_400instances.pdf ../paper_sat2021_mallob/

# Hordesat vs. base mallob, cbdf=7/8 vs. 8/8
python3 scripts/plot_curves.py -xy -xmin=0 -xmax=300 -ymin=0 -ymax=70 -xsize=4.5 -ysize=3.3 \
RESULTS/mallob_mono_basic_n128/cdf_runtimes -l='Mallob $\alpha=8/8$' \
RESULTS/mallob_mono_cbdf0.875_n128/cdf_runtimes -l='Mallob $\alpha=7/8$' \
RESULTS/mallob_mono_cbdf0.75_n128/cdf_runtimes -l='Mallob $\alpha=6/8$' \
RESULTS/mallob_mono_cbdf0.625_n128/cdf_runtimes -l='Mallob $\alpha=5/8$' \
RESULTS/mallob_mono_cbdf0.5_n128/cdf_runtimes -l='Mallob $\alpha=4/8$' \
RESULTS/horde_new_n128/cdf_runtimes -l="HordeSat (new)" \
RESULTS/horde_old_n128/cdf_runtimes -l="HordeSat (old)" \
-xlabel="Run time / s" -ylabel="\\# instances solved in $\\leq t$ s" \
-o=horde_mallob_overview.pdf
#cp horde_mallob_overview.pdf ../paper_sat2021_mallob/


# Hordesat scaling
python3 scripts/plot_curves.py -xy -xmin=0 -xmax=300 -ymin=0 -ymax=70 -size=3.25 -nolegend -ygrid \
RESULTS/horde_new_n128/cdf_runtimes -l='$128\times 5\times 4$ HordeSat' \
RESULTS/horde_new_n32/cdf_runtimes -l='$32\times 5\times 4$ HordeSat' \
RESULTS/horde_new_n8/cdf_runtimes -l='$8\times 5\times 4$ HordeSat' \
RESULTS/horde_new_n2/cdf_runtimes -l='$2\times 5\times 4$ HordeSat' \
RESULTS/horde_new_n1/cdf_runtimes -l='$1\times 3\times 4$ HordeSat' \
RESULTS/kissat/cdf_runtimes -l='Kissat' \
RESULTS/lingeling/cdf_runtimes -l='Lingeling' \
-title="HordeSat" -xlabel="Run time / s" -ylabel="\\# instances solved in $\\leq t$ s" \
-o=scaling_horde.pdf
cp scaling_horde.pdf ../paper_sat2021_mallob/

# Mallob scaling
python3 scripts/plot_curves.py -xy -xmin=0 -xmax=300 -ymin=0 -ymax=70 -xsize=4.6 -ysize=3.25 -legendright -ygrid \
RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n128/cdf_runtimes -l='$128\times 5\times 4$' \
RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n32/cdf_runtimes -l='$32\times 5\times 4$' \
RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n8/cdf_runtimes -l='$8\times 5\times 4$' \
RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n2/cdf_runtimes -l='$2\times 5\times 4$' \
RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n1/cdf_runtimes -l='$1\times 3\times 4$' \
RESULTS/kissat/cdf_runtimes -l='Kissat' \
RESULTS/lingeling/cdf_runtimes -l='Lingeling' \
-title="Mallob" -xlabel="Run time / s" -ylabel="\\# instances solved in $\\leq t$ s" \
-o=scaling_mallob.pdf
cp scaling_mallob.pdf ../paper_sat2021_mallob/

exit

# Clause filter half lifes
python3 scripts/plot_curves.py -xy \
RESULTS/mallob_mono_cbdf0.875_cfhl90_n128/cdf_runtimes          -l='$X=90$' \
RESULTS/mallob_mono_cbdf0.875_cfhl30_n128/cdf_runtimes          -l='$X=30$' \
RESULTS/mallob_mono_cbdf0.875_cfhl10_n128/cdf_runtimes          -l='$X=10$' \
RESULTS/mallob_mono_cbdf0.875_n128/cdf_runtimes                 -l='$X=\infty$' \
-xlabel="Run time / s" -ylabel="\\# instances solved in $\\leq t$ s"
exit 0

# LBD limits
python3 scripts/plot_curves.py -xy \
RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n128/cdf_runtimes     -l='lbd=$\infty$' \
RESULTS/mallob_mono_cbdf0.875_n128/cdf_runtimes                 -l='lbd=$2\rightarrow \infty$' \
RESULTS/mallob_mono_cbdf0.875_mlbd8_mcl0_n128/cdf_runtimes      -l='lbd=$2\rightarrow 8$' \
-xlabel="Run time / s" -ylabel="\\# instances solved in $\\leq t$ s" \
-o=lbd_limits.pdf

# Clause length limits (under no LBD limit)
python3 scripts/plot_curves.py -xy \
RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl5_n128/cdf_runtimes     -l='cl=5' \
RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl10_n128/cdf_runtimes    -l='cl=10' \
RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n128/cdf_runtimes     -l='cl=inf' \
-xlabel="Run time / s" -ylabel="\\# instances solved in $\\leq t$ s" \
-o=clauselen_limits.pdf


# Best configurations
python3 scripts/plot_curves.py -xy \
RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl10_n128/cdf_runtimes        -l="mallob-mono bdf=7/8 lbd=inf mcl=10" \
RESULTS/mallob_mono_cbdf0.875_mlbd00_cfhl90_n128/cdf_runtimes       -l="mallob-mono bdf=7/8 lbd=inf cfhl=90" \
RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n128/cdf_runtimes         -l="mallob-mono bdf=7/8 lbd=inf" \
RESULTS/mallob_mono_cbdf0.875_mlbd00_cfhl90_mcl10_n128/cdf_runtimes -l="mallob-mono bdf=7/8 lbd=inf cfhl=90 mcl=10" \
-xlabel="Run time / s" -ylabel="\\# instances solved in $\\leq t$ s"


#RESULTS/mallob_mono_cbdf0.875_mlbd0_mcl5_n128/cdf_runtimes      -l="mallob-mono bdf=7/8 lbd=2->inf mcl=5" \
#RESULTS/mallob_mono_cbdf0.875_mlbd0_mcl10_n128/cdf_runtimes     -l="mallob-mono bdf=7/8 lbd=2->inf mcl=10" \
#RESULTS/mallob_mono_cbdf0.875_mlbd8_mcl5_n128/cdf_runtimes      -l="mallob-mono bdf=7/8 lbd=2->8 mcl=5" \
#RESULTS/mallob_mono_cbdf0.875_mlbd8_mcl10_n128/cdf_runtimes     -l="mallob-mono bdf=7/8 lbd=2->8 mcl=10" \

#RESULTS/mallob_mono_cbdf0.875_cfhl90_n128/cdf_runtimes -l="mallob-mono bdf=7/8 cfhl=90" \

