#!/bin/bash

for hard_only in false true; do
    
    if $hard_only; then echo "(hard instances only)"
    else echo "(all instances)"; fi
    
    #echo 'Configuration & \multicolumn{3}{|rrr|}{Lingeling} & \multicolumn{3}{|rrr|}{Kissat} \\'
    #echo ' & \#slv. & $S_{med}$ & $S_{tot}$ & \#slv. & $S_{med}$ & $S_{tot}$ \\'
    
    c=1
    for d in RESULTS/horde_new_n{1,2,8,32,128} RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n{1,2,8,32,128} ; do

        num_nodes=$(echo $d|grep -oE "_n[0-9]+"|grep -oE "[0-9]+")
        num_threads=$((20*$num_nodes))
        
        echo -ne "$(head -$c speedups_header|tail -1) "
        c=$((c+1))
        for slv in lingeling kissat; do
            
            count=0
            sum_of_rt_seq=0
            sum_of_rt_par=0
            > _speedups
            
            while read -r line; do
                
                speedup=$(echo $line|awk '{print $3}')
                rt_seq=$(echo $line|awk '{print $4}')
                rt_par=$(echo $line|awk '{print $5}')
                
                if $hard_only; then
                    if [ $(echo "$rt_seq < $num_threads"|bc -l) == 1 ]; then
                        continue
                    fi
                fi
                
                echo $speedup >> _speedups
                sum_of_rt_seq=$(echo "$sum_of_rt_seq+$rt_seq"|bc -l)
                sum_of_rt_par=$(echo "$sum_of_rt_par+$rt_par"|bc -l)
                count=$((count+1))
            
            done < $d/speedups_$slv
            
            sort -g _speedups -o _speedups
            
            if [ $count == 0 ]; then continue; fi
            
            lo_idx=$(echo "$count * 0.1"|bc -l|awk '{x=int($1); print(x>1?x:2)}')
            hi_idx=$(echo "$count * 0.9"|bc -l|awk '{x=int($1)>$1?int($1)+1:int($1); print(x<'$count'?x:'$count'-1)}')
            median_idx=$(($count/2))
            #echo total: $count indices: $lo_idx $median_idx $hi_idx
            sum_of_speedups=0
            #harm=0
            sc=0
            for i in $(seq $lo_idx $hi_idx); do
                s=$(head -$i _speedups|tail -1)
                sum_of_speedups=$(echo "$sum_of_speedups + $s"|bc -l)
                #harm=$(echo "$harm + 1.0/$s"|bc -l)
                sc=$((sc+1))
            done
            avg_speedup=$(echo "$sum_of_speedups / $sc"|bc -l)
            #havg_speedup=$(echo "$sc / $harm"|bc -l)
            
            if [ $median_idx == 0 ]; then median_idx=1; fi
            median_speedup=$(head -$median_idx _speedups|tail -1)
            total_speedup=$(echo "$sum_of_rt_seq / $sum_of_rt_par"|bc -l)
            
            LC_ALL=C printf '& %i & %.2f & %.2f & %.2f ' $count $avg_speedup $median_speedup $total_speedup
        done
        echo "\\\\"
    done
done
