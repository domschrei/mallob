
set folder=1_160
set pcName=i10pc135
for %%t in (times relSpeedup efficiency) do (
    for %%k in (20) do ( Rem for /l %%k in (1, 1, 100) do (
        python ../scripts/plot/plot_curves.py -xy .\%folder%\%%t-%pcName%-%%k.txt -l="k=%%k" -o=.\%folder%\Graph-%%t-%pcName%-%%k.pdf
    )
)
pause


