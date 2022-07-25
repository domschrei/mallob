
set folder=strange
set pcName=i10pc135
for %%t in (times relSpeedup efficiency) do (
    for %%k in (30) do ( Rem for /l %%k in (1, 1, 100) do (
        for %%d in (500000 250000 100000 10000 1000 100) do (
            python ../scripts/plot/plot_curves.py -xy .\%folder%\%%t-%pcName%-%%k-%%d.txt -l="k=%%k d=%%d" -o=.\%folder%\Graph-%%t-%pcName%-%%k-%%d.pdf
        )
    )
)
pause


