#!/bin/bash

set -e

lib/cadical/build/cadical $1 -q -c 0 -o _removed-units.cnf 1>&2 2>/dev/null
cat _removed-units.cnf
rm _removed-units.cnf
