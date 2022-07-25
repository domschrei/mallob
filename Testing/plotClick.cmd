SETLOCAL ENABLEDELAYEDEXPANSION
set folder=dTest
set pcName=i10pc135
for %%t in (times relSpeedup efficiency) do (
    set cm=python ../scripts/plot/plot_curves.py -xy
    for %%k in (30) do ( Rem for /l %%k in (1, 1, 100) do (
        for %%d in (500000 250000 100000 10000) do (
            set cm=!cm! .\%folder%\%%t-%pcName%-%%k-%%d.txt -l="k=%%k d=%%d"
        )
    )
    set cm=!cm!  -o=.\%folder%\Graph-%%t-%pcName%-kd.pdf
    call !cm!
)
pause
