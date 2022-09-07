SETLOCAL ENABLEDELAYEDEXPANSION
set folder=nTest256core100d150exact
set pcName=i10pc138
for %%t in (times relSpeedup efficiency) do (
    for %%n in (10000 30000 50000) do ( Rem  50000 10000
        set cm=python ../scripts/plot/plot_curves.py -xy -xlabel="num Workers" -ylabel="%%t"
        for %%k in (10 30 50 100) do ( Rem for /l %%k in (1, 1, 100) do ( Rem 10 30 50 70 100
            set cm=!cm! .\%folder%\%%t-%pcName%-%%k-%%n.txt -l="k=%%k n=%%n"
        )
        set cm=!cm!  -o=.\%folder%\Graph-%%n-%%t-%pcName%-kn.pdf
        call !cm!
    )
)
pause
