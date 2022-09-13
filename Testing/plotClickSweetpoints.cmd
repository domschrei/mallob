SETLOCAL ENABLEDELAYEDEXPANSION


set cm=python ../scripts/plot/plot_curves.py -linestyles="None,--,-" -markers="^,None,s" -xy -xlabel="worksize k*n*d" -ylabel="demand" -title="best demand compared to worksize adjusted"

set cm=!cm! .\FinalSweetpointsTrimmedPlot.txt -l="adjusted points"
set cm=!cm! .\regression.txt -l="power regression"
set cm=!cm! .\stairs.txt -l="demand function"

set cm=!cm!  -o=.\FinalSweetpointsTrimmedPlot.pdf
call !cm!

pause
