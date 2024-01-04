################### Build Mallob
FROM ubuntu:23.04
USER root
#  Install required softwares
RUN apt-get update
RUN DEBIAN_FRONTEND=noninteractive apt install -y git cmake build-essential zlib1g-dev libopenmpi-dev wget unzip build-essential zlib1g-dev cmake python3 build-essential gfortran wget curl libjemalloc-dev libjemalloc2 gdb psmisc

# Fetch Mallob
RUN git clone https://github.com/domschrei/mallob && cd mallob && git checkout ade591635c8a9cdd8af78c1f10f43e78dcc74f05
# Fetch solvers, build all of them
RUN cd mallob/lib && bash fetch_and_build_solvers.sh gylck
# Disable building test classes to save time & energy
RUN cd mallob && sed -i 's/^new_test.*$//g' CMakeLists.txt src/app/sat/setup.cmake 
# Build Mallob
RUN cd mallob && mkdir -p build && cd build \
  && cmake -DCMAKE_BUILD_TYPE=RELEASE \
    -DMALLOB_SUBPROC_DISPATCH_PATH='"./build/"' -DMALLOB_ASSERT=1 -DMALLOB_USE_GLUCOSE=1 \
    -DMALLOB_USE_ASAN=0 -DMALLOB_USE_JEMALLOC=1 -DMALLOB_JEMALLOC_DIR=/usr/lib/x86_64-linux-gnu \
    -DMALLOB_LOG_VERBOSITY=4 -DMALLOB_APP_SAT=1 -DMALLOB_MAX_N_APPTHREADS_PER_PROCESS=64 .. \
  && VERBOSE=1 make -j \
  && cd ..
