#!/bin/bash
set -e

src=$1; nums=$2; dst=$3
tmp=$dst.$$

awk 'NR==FNR      { want[(($1+1))]; n++; next }
     FNR==1       { $4 = n; print; next }
     FNR in want' "$nums" "$src" > "$tmp"

mv -- "$tmp" "$dst"
