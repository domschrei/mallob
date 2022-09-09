SETLOCAL ENABLEDELAYEDEXPANSION
set folder=maxDemandDefaultTest138
set pcName=i10pc138
set cm=python ../scripts/plot/plot_curves.py -nomarkers -linestyles="-,--" -xy -xlabel="seconds" -ylabel="solved jobs" 

for %%t in (Restricted Unrestricted) do (
    set cm=!cm! .\%folder%\cdf-runtimes%%t.txt -l="%%t"
)

set cm=!cm!  -o=.\%folder%\Graph.pdf
call !cm!

pause
