SETLOCAL ENABLEDELAYEDEXPANSION
set folder=nTest256core100d150exact
set pcName=i10pc138
for %%t in (Restricted Unrestricted) do (
    
        set cm=python ../scripts/plot/plot_curves.py -xy -xlabel="seconds" -ylabel="solved jobs"
        
        set cm=!cm! .\%folder%\cdf-runtimes%%t.txt -l="%%t"

        set cm=!cm!  -o=.\%folder%\Graph.pdf
        call !cm!

)
pause
