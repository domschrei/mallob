#!/bin/bash

python3 scripts/plot_curves.py  \
_latencies_balancing -l="Balancing" \
_latencies_scheduling -l="Scheduling"

#latencies_exp_avgs_normalized -l="Exp CA" \
#latencies_ca_avgs_normalized -l="Reg CA"

#latencies_hop_avgs -l="Hop" \
#latencies_ca100_avgs -l="100Hops-CA" \
#latencies_ca5_avgs -l="5Hops-CA" \
#latencies_ca3_avgs -l="3Hops-CA" \
 
#latencies_ca10_avgs -l="10Hops-CA" \
#latencies_ca6_avgs -l="6Hops-CA" \
#latencies_ca4_avgs -l="4Hops-CA" \
#latencies_ca2_avgs -l="2Hops-CA" \
#latencies_ca1_avgs -l="1Hop-CA" \
