#!/bin/bash

# Only needed if building with -DMALLOB_APP_SAT=1 (enabled by default).
# For non-x86-64 architectures (ARM, POWER9, etc.), prepend `export DISABLE_FPU=1;`.
scripts/setup/sat-setup.sh

# Only needed if building with -DMALLOB_APP_MAXSAT=1 and -DMALLOB_APP_SMT=1, respectively.
scripts/setup/maxsat-setup.sh
scripts/setup/smt-setup.sh

# Build Mallob. You can modify and/or append build options like -DMALLOB_APP_MAXSAT=1.
# Find all build options at: docs/setup.md
scripts/setup/cmake-make.sh build -DMALLOB_APP_MAXSAT=1 -DMALLOB_APP_SMT=1 -DMALLOB_APP_INCSAT=1 -DMALLOB_APP_SATWITHPRE=1 -DMALLOB_BUILD_LRAT_MODULES=1
