SETLOCAL ENABLEDELAYEDEXPANSION


set cm=python ../scripts/plot/plot_curves.py -xy -xlabel="worksize k*n*d" -ylabel="best demand" -title="best demand compared to worksize" -nolines -nolegend

set cm=!cm! .\FinalSweetpointsTrimmedPlot.txt

set cm=!cm!  -o=.\FinalSweetpointsTrimmedPlot.pdf
call !cm!

pause
