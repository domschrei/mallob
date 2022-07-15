
set folder=latestResults
set pcName=i10pc135
for %%t in (times relSpeedup efficiency) do (
    for %%k in (5 10 20 30 40 50 60 70 80) do (
        python ../scripts/plot/plot_curves.py -xy .\%folder%\%%t-%pcName%-%%k.txt -l="k=%%k" -o=.\%folder%\Graph-%%t-%pcName%-%%k.pdf
    )
)
pause


