for %%k in (5 10 20 30 40) do (
    python ../scripts/plot/plot_curves.py -xy times-%%k.txt -l="k=%%k" -o=Graph%%k.pdf
)
pause


