
FROM ubuntu:20.04
USER root

#  Install required softwares
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt install -y cmake build-essential zlib1g-dev libopenmpi-dev wget unzip build-essential zlib1g-dev curl libjemalloc-dev libjemalloc2 gdb git

# Build Mallob
# This is a single command such that a change in the commit hash will make Docker re-fetch the repository
RUN git clone https://github.com/domschrei/mallob
WORKDIR mallob

RUN cd lib && bash fetch_and_build_sat_solvers.sh kcly && cd ..
RUN mkdir build
RUN cd build && cmake -DCMAKE_BUILD_TYPE=RELEASE -DMALLOB_SUBPROC_DISPATCH_PATH=\"./\" -DMALLOB_ASSERT=1 -DMALLOB_USE_JEMALLOC=1 -DMALLOB_JEMALLOC_DIR=/usr/lib/x86_64-linux-gnu -DMALLOB_LOG_VERBOSITY=4 .. && VERBOSE=1 make -j4 && cd ..
