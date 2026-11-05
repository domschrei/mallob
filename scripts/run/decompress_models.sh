#!/usr/bin/env bash
# decompress_models.sh
#
# Usage: ./decompress_models.sh FILE [FILE...] > models.txt
#
# Each input file contains one compressed model per line, in the format
#   <nbVars>:<hex digits>
# where each hex digit encodes 4 consecutive variables (bit i set => variable positive).
# Output: the unique decompressed models, one per line, as
#   v <lit> <lit> ... 0
# in order of first occurrence.

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 FILE [FILE...]" >&2
    exit 1
fi

for f in "$@"; do
    if [[ ! -r "$f" ]]; then
        echo "Error: cannot read file '$f'" >&2
        exit 1
    fi
done

awk '
BEGIN {
    hex = "0123456789abcdef"
    w[0] = 1; w[1] = 2; w[2] = 4; w[3] = 8
}
{
    sub(/\r$/, "")                       # tolerate Windows line endings
    if ($0 == "") next                   # skip blank lines

    line = tolower($0)                   # strtol accepts A-F as well; normalize
    if (line !~ /^[0-9]+:[0-9a-f]*$/) {
        printf "Warning: skipping malformed line %d of %s\n", FNR, FILENAME > "/dev/stderr"
        next
    }

    if (line in seen) next               # same compressed string <=> same model
    seen[line] = 1

    n      = index(line, ":")
    nvars  = substr(line, 1, n - 1) + 0
    digits = substr(line, n + 1)

    out = "v"
    var = 1
    for (i = 1; i <= length(digits) && var <= nvars; i++) {
        num = index(hex, substr(digits, i, 1)) - 1
        for (b = 0; b < 4 && var <= nvars; b++) {
            out = out " " ((int(num / w[b]) % 2) ? var : -var)
            var++
        }
    }
    print out " 0"
}
' "$@"
