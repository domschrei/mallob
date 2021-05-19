#!/bin/bash

slvlabel="Lingeling"
for slv in lingeling kissat; do

    for d in RESULTS/horde_new_n{1,2,8,32,128} RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n{1,2,8,32,128} ; do
        ncores=$(($(echo $d|grep -oE "_n[0-9]+"|grep -oE "[0-9]+")*20))
        cat $d/speedups_$slv |awk '{printf("%.3f\n", ($3/'$ncores' > 1 ? 1.01 : $3/'$ncores'))}' > $d/efficiency_${slv}
        cat $d/efficiency_${slv}| sort -g| awk '{print NR-1,$1}' > _
        numinst=$(cat _|wc -l)
        cat _ | awk '{print $1/'$numinst',$2}' > $d/efficiency_${slv}_sorted
    done
    
    limits="-xmin=0 -xmax=1 -ymin=0 -ymax=1"
    #-logx -logy -xmin=1 -xmax=60000 -ymin=0.1 -ymax=2000000
    
    python3 scripts/plot_curves.py -xlabel='Percentile $q$ of solved instances sorted by efficiency' -ylabel='$\min\ \{$1, Efficiency at $q\}$' \
    RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n1/efficiency_${slv}_sorted -l="M1x3x4" \
    RESULTS/horde_new_n1/efficiency_${slv}_sorted -l="H1x3x4" \
    RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n2/efficiency_${slv}_sorted -l="M2x5x4" \
    RESULTS/horde_new_n2/efficiency_${slv}_sorted -l="H2x5x4" \
    RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n8/efficiency_${slv}_sorted -l="M8x5x4" \
    RESULTS/horde_new_n8/efficiency_${slv}_sorted -l="H8x5x4" \
    RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n32/efficiency_${slv}_sorted -l="M32x5x4" \
    RESULTS/horde_new_n32/efficiency_${slv}_sorted -l="H32x5x4" \
    RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n128/efficiency_${slv}_sorted -l="M128x5x4" \
    RESULTS/horde_new_n128/efficiency_${slv}_sorted -l="H128x5x4" \
    -xy $limits -title="Efficiency over $slvlabel" \
    -markers=*,+ -linestyles=--,: \
    '-colors=#373eb8,#373eb8,#ff7f00,#ff7f00,#e41a1c,#e41a1c,#f781bf,#f781bf,#dede00,#dede00' \
    -xsize=5.5 -ysize=4 -o="efficiency_${slv}.pdf"

    #python3 scripts/plot_curves.py -xlabel='Percentile $q$ of instances sorted by efficiency' -ylabel='$\min\ \{$1, Efficiency at $q\}$' \
    #RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n128/efficiency_${slv}_sorted -l="M128x5x4" \
    #RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n32/efficiency_${slv}_sorted -l="M32x5x4" \
    #RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n8/efficiency_${slv}_sorted -l="M8x5x4" \
    #RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n2/efficiency_${slv}_sorted -l="M2x5x4" \
    #RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n1/efficiency_${slv}_sorted -l="M1x3x4" \
    #-xy $limits \
    #-size=3.5 -o="speedup_scatter_${slv}_mallob.pdf"
    
    slvlabel="Kissat"
done

#cp speedup_scatter_*.pdf ../paper_sat2021_mallob/
