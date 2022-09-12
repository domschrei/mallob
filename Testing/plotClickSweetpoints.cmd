SETLOCAL ENABLEDELAYEDEXPANSION


set cm=python ../scripts/plot/plot_curves.py -linestyles="None,-" -markers="^,None" -xy -xlabel="worksize k*n*d" -ylabel="demand" -title="best demand compared to worksize adjusted" -nolegend

set cm=!cm! .\FinalSweetpointsTrimmedPlot.txt 
set cm=!cm! .\regression.txt

set cm=!cm!  -o=.\FinalSweetpointsTrimmedPlot.pdf
call !cm!

pause
