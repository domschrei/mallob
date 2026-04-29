#!/bin/bash

#!/bin/bash
timelim_sec="$1"
instance="$2"

options=(
    -n
    --statistics=1
    --verbose=0
    --sweepcomplete=1
    --sweepdepth=4
    --sweepmaxdepth=6
    --substituterounds=2
    --neverdelay=1
    --puresweep=1
    --puresweep_tocompletion=1
    --puresweep_iterations=3
    --puresweep_minExitSwept=0
    --puresweep_termNoProgress=0
    --puresweep_timelim="${timelim_sec}"
    --mallob_custom_sweep_verbosity=2
)

#-n : dont print satisfiying solution

echo "kissat options: $options"
echo "kissat options: ${options[@]}"
echo "instance: $instance"
echo ""
~/PhD/ksst-sweep/kissat/build/kissat ${options[@]} $instance
