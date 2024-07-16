#!/bin/bash

rm .plot.profile-histogram-* .profile-histogram-* || :

cat | python3 scripts/profile/interpret_solver_profile_internal.py

for f in .profile-histogram-* ; do
    plot_curves.py $f -xy -nolegend -title="$(echo $f|sed 's/.profile-histogram-//g'|tr '[:lower:]' '[:upper:]')" \
    -labelx='Ratio $\alpha$ of total run time' -labely='Share of runs with ratio $\leq \alpha$' -gridx -gridy -sizex=3.2 -sizey=3.2 \
    -minx=0 -maxx=1 -miny=0 -maxy=1 -nomarkers -lw=1.5 \
    -ticks{x,y}=0,0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8,0.9,1 -o=.plot${f}.pdf
    
    pdfcrop .plot${f}.pdf .cropped.plot${f}.pdf
done

montage -density 300 .cropped.plot.profile-histogram-* -tile 4x -geometry 1000x1000+20+20 profile-overview.png

rm .plot.profile-histogram-* .profile-histogram-* || :
