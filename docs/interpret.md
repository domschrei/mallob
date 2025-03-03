
# Interpreting a Mallob run

_Note: This documentation will be expanded upon over time._

## Extracting response times

Quick-and-dirty way to extract all response times of tasks in Mallob (e.g., if you are benchmarking MallobSat) and compute some basic measures based on that:

```bash
# Generate text file where each line features an instance name and the corresponding response time
basedir="my/path/to/logs/sat-benchmark-run-4nodes"
for d in $basedir/*/; do
    if [ ! -f $d/instance.txt ]; then
        continue
    fi
    echo $(basename $(cat $d/instance.txt)) $(grep RESPONSE_TIME $d/0/log.0|awk '{print $6}')
done | sort -k 1,1b > $basedir/qualified-times.txt

# Convert to raw, nicely plottable CDF data (x coord = run time, y coord: # solved instances)
cat $basedir/qualified-times.txt | awk '{print $2}' | sort -g | awk '{print $1,NR}' > $basedir/cdf.txt

# Compute PAR-2 score (assuming a 300s time limit and 500 instances)
cat $basedir/cdf.txt | awk '$1 <= 300 {s+=$1; n+=1} END {print (s + (500-n)*2*300)/500}'
```
