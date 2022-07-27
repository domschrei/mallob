SETLOCAL ENABLEDELAYEDEXPANSION
set folder=dTest256core
set pcName=i10pc138
for %%t in (times relSpeedup efficiency) do (
    set cm=python ../scripts/plot/plot_curves.py -xy
    for %%k in (30) do ( Rem for /l %%k in (1, 1, 100) do (
        for %%d in (500000 300000 100000 50000 10000) do (
            set cm=!cm! .\%folder%\%%t-%pcName%-%%k-%%d.txt -l="k=%%k d=%%d"
        )
    )
    set cm=!cm!  -o=.\%folder%\Graph-%%t-%pcName%-kd.pdf
    call !cm!
)
pause
