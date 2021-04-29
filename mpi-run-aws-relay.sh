#!/bin/bash

set -e
wget https://dominikschreiber.de/mpi-run-aws.sh
ls -lt
chmod +x mpi-run-aws.sh
./mpi-run-aws.sh
