
python3 analyze/bernoulli_dist.py > num_hops_bernoulli_cdf
cat num_hops_bernoulli_cdf|gawk '{print 1 - $1}' > num_hops_bernoulli_cdf_rev

str_cdf=""
str_diff=""
str_reversed=""
for n in 64 256 1024 4096 ; do
    for r in 8 ; do
        cdf="num_hops_comp_mallob_cdf_n${n}_r$r"
        diff="num_hops_diff_cdf_bernoulli-compmallob_n${n}_r$r"
        reversed="num_hops_comp_mallob_cdf_n${n}_r${r}_rev"
        
        python3 analyze/cover_distribution.py $n $r > $cdf
        
        paste -d " " num_hops_bernoulli_cdf $cdf|head -300|gawk '{print $1-$2}' > $diff
        cat $cdf|gawk '{print 1 - $1}' > $reversed
        
        str_cdf="$str_cdf $cdf ""-l=m=${n}" #,r=$r\""
        str_diff="$str_diff $diff ""-l=m=${n}" #,r=$r\""
        str_reversed="$str_reversed $reversed ""-l=m=${n}" #,r=$r\""
    done
done

str_cdf="$str_cdf num_hops_bernoulli_cdf -l=Bernoulli"
str_reversed="$str_reversed num_hops_bernoulli_cdf_rev -l=Bernoulli"

python3 analyze/plot_curves.py $str_reversed -xlabel='$h$ (\#Hops)' -ylabel='$P(x > h)$' -xmin=0 -xmax=200 -ymin=0.00001 -ymax=1.01 -xsize=5 -ysize=3.5 -no-markers -logy -grid -o="viz/hop_cdfs.pdf"

#python3 analyze/plot_curves.py $str_cdf -xlabel='$h$ (\#Hops)' -ylabel='$cdf(h)$' -xmin=0 -xmax=210 -xsize=5 -ysize=3.5 -no-linestyles -logy \
#logs/fh2/mallob_eval01_103x5x4_99475s/num_hops_cdf -l="mallob" \

#python3 analyze/plot_curves.py $str_diff -xlabel='$h$ (\#Hops)' -ylabel='$cdf_{\textit{Bn}}(h) - \hat{cdf}_{G_r}(h)$' -xmin=0 -xmax=210 -xsize=5 -ysize=3.5 -no-linestyles
