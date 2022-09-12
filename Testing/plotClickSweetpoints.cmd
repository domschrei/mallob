SETLOCAL ENABLEDELAYEDEXPANSION


set cm=python ../scripts/plot/plot_curves.py -linestyles="None,--" -markers="^,None" -xy -xlabel="worksize k*n*d" -ylabel="demand" -title="best demand compared to worksize adjusted"

set cm=!cm! .\FinalSweetpointsTrimmedPlot.txt -l=""
set cm=!cm! .\regression.txt -l="power regression"

set cm=!cm!  -o=.\FinalSweetpointsTrimmedPlot.pdf
call !cm!

pause
