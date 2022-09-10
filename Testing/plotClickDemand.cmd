SETLOCAL ENABLEDELAYEDEXPANSION
set folder=maxDemandDefaultTest138Version3
set pcName=i10pc138
set cores=128

set cm=python ../scripts/plot/plot_curves.py -nomarkers -linestyles="-,--" -xy -xlabel="seconds" -ylabel="solved jobs" -title="%cores% cores"

for %%t in (Restricted Unrestricted) do (
    set cm=!cm! .\%folder%\cdf-runtimes%%t.txt -l="%%t"
)

set cm=!cm!  -o=.\%folder%\Graph.pdf
call !cm!

pause
