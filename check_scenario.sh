scenario_file="$1"

# Test if scenario was provided
if [ "x$1" == "x" ]; then
    echo "No scenario was provided"
    exit 1
fi

# Skip first line
# Then check all formulas if they exist
sed 1d "$scenario_file" | while read -r line; do 
    filename=$(echo $line | awk '{print $4}')
    if [ ! -r $filename ]; then
        jobid=$(echo $line | awk '{print $1}')
        echo "Missing file "$filename" for job with id "$jobid
    fi
done
