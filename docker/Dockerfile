################### Build Mallob
FROM ubuntu:24.04
USER root
#  Install required softwares
RUN apt-get update
RUN DEBIAN_FRONTEND=noninteractive apt install -y git cmake build-essential zlib1g-dev \
libopenmpi-dev wget unzip build-essential zlib1g-dev cmake python3 build-essential \
gfortran wget curl libjemalloc-dev libjemalloc2 gdb psmisc \
meson python3-mesonpy ninja-build libgmp-dev pkgconf libmpfr-dev cargo

# Fetch Mallob
RUN git clone https://github.com/domschrei/mallob && cd mallob && git checkout \
181460362a76c9f9327a6fc833239157a25e7125
# Build Mallob
RUN cd mallob && bash scripts/setup/cmake-make.sh build \
-DMALLOB_MAX_N_APPTHREADS_PER_PROCESS=64 -DMALLOB_APP_SMT=1 -DMALLOB_APP_MAXSAT=1 \
-DMALLOB_BUILD_IMPCHECK=1
