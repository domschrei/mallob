scenario_file="$1"

# Test if scenario was provided
if [ "x$1" == "x" ]; then
    echo "No scenario was provided"
    exit 1
fi

>baseline.txt

# Skip first line
# Then check all formulas if they exist
sed 1d "$scenario_file" | while read -r line; do

    jobid=$(echo $line | awk '{print $1}')
    filename=$(echo $line | awk '{print $4}')

    if [ -r $filename ]; then
        # Capture start time in seconds
        let start=$(date +%s)
        # Run cadical 1000s and capture result
        RESULT=$(src/app/sat/hordesat/cadical/build/cadical -n -t 1000 $filename)
        # Capture end time in seconds
        let end=$(date +%s)

        # Calculate duration
        let duration=$(expr $b - $a)

        echo "Solving of formula "$filename" with id "$jobid" took "$duration" seconds and terminated with Result: "$RESULT"" >>baseline.txt
    else
        echo "Missing file "$filename" for job with id "$jobid"" >>baseline.txt
    fi
done
