#!/bin/bash

suffix="full"  # "full" or "mini"

function die() {
    echo $@
    echo ""
    exit 1
}
function check_deps() {
    for cmd in $@ ; do
        ( for flag in -h -V --version -v; do
            $cmd $flag >/dev/null 2>&1 && return 0
        done ) || die "Error ($suffix installation): Command $cmd does not work - must be installed on your system."
    done
}

set -e

# Basic dependency checks
check_deps git mpirun cmake g++ xz killall python3 wget curl gdb

# Fetch Mallob repository
if [ ! -d automallob-$suffix ]; then
    git clone git@github.com:domschrei/mallob.git automallob-$suffix
fi
cd automallob-$suffix
git checkout chaincheck

# Build Mallob
if [ "x$suffix" == "xmini" ]; then
    MALLOB_MINIMAL=1 scripts/setup/cmake-make.sh build || \
        die "Build error. Please consult: https://github.com/domschrei/mallob/blob/master/docs/"
else
    check_deps meson ninja pkgconf cargo
    scripts/setup/cmake-make.sh build || \
        die "Build error. Please consult: https://github.com/domschrei/mallob/blob/master/docs/"
fi

echo ""
echo "Mallob ($suffix installation) ready to go at: $(pwd)"
echo "Run all Mallob related commands from that directory."
echo ""
echo "* Try it out: cd automallob-$suffix && scripts/run/mallob_local.sh -mono=instances/r3unsat_300.cnf"
echo ""
echo "* Expand the installation by re-building with additional flags. For example:"
echo "    - MALLOB_MINIMAL=1 scripts/setup/cmake-make.sh build -DMALLOB_USE_CADICAL=1"
echo "      (+ CaDiCaL solver backend)"
echo "    - MALLOB_MINIMAL=1 scripts/setup/cmake-make.sh build -DMALLOB_APP_MAXSAT=1 -DMALLOB_APP_SMT=1"
echo "      (+ MaxSAT and SMT solving engines with dependencies)"
echo "    - rm -rf build  && scripts/setup/cmake-make.sh build -DMALLOB_APP_SMT=0"
echo "      (full-blown installation but without SMT)"
echo ""
echo "For comprehensive documentation on usage and configuration, please visit"
echo "  https://github.com/domschrei/mallob/tree/master/docs"
echo "or, equivalently, the local documentation files at"
echo "  $(pwd)/automallob-$suffix/docs/"
echo ""
echo "Have fun! The SAtRes team <https://satres.eu>"
echo ""
