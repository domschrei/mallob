f=$HOME/PhD/instances/MUS11/all/4pipe_4_ooo.cnf
cp $f in.cnf
for i in {1..50}; do 
    echo -ne $(head -1 in.cnf) "=> ... "
    scripts/run/mallob_local.sh $(config/presets/monolithic-proof) -mono=in.cnf -proof=/dev/null -core=out.cnf -q > /dev/null
    echo -ne "=>" $(head -1 out.cnf)
    echo "   t=$(date +%T)"
    cp out.cnf in.cnf
done
