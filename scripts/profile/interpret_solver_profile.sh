#!/bin/bash

baseprofile="$1"
shift 1
> .NOLEGEND

cat "$baseprofile" | while read -r line; do

    category=$(echo $line|awk '{print $1}')
    nvalues=$(echo $line|awk '{print NF}')
    echo $line|awk '{$1=""; print $0}'|tr ' ' '\n'|awk '{print 0.01*$1, NR/'$nvalues'}' > .data
    datafiles=".data"
    labels="-l=$(basename $baseprofile)"

    for profile in $@ ; do
        otherline=$(cat "$profile"|grep -E "^$category ")
        nvalues=$(echo $otherline|awk '{print NF}')
        fdata=.data.$(basename $profile)
        echo $otherline|awk '{$1=""; print $0}'|tr ' ' '\n'|awk '{print 0.01*$1, NR/'$nvalues'}' > $fdata
        datafiles="$datafiles $fdata"
        labels="$labels -l=$(basename $profile)"
    done

    plot_curves.py $datafiles $labels $(cat .NOLEGEND) -xy -title="$(echo $category|tr '[:lower:]' '[:upper:]')" \
        -labelx='Ratio $\alpha$ of total run time' -labely='Share of runs with ratio $\leq \alpha$' -gridx -gridy -sizex=3.2 -sizey=3.2 \
        -minx=0 -maxx=1 -miny=0 -maxy=1 -nomarkers -lw=1.5 \
        -ticks{x,y}=0,0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8,0.9,1 -o=.plot.${category}.pdf
    
    pdfcrop .plot.${category}.pdf .cropped.plot.${category}.pdf

    # Enable this line to only add the legend to the 1st plot
    #echo " -nolegend" > .NOLEGEND
done

montage -density 300 .cropped.plot.* -tile 4x -geometry 1000x1000+20+20 profile-overview.png

rm .plot.*.pdf .cropped.plot.*.pdf
