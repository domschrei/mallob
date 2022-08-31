################### Build Mallob
FROM satcomp-base:leader AS builder
USER root
#  Install required softwares
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt install -y cmake build-essential zlib1g-dev libopenmpi-dev wget unzip build-essential zlib1g-dev cmake python3 build-essential gfortran wget curl libjemalloc-dev libjemalloc2 gdb libboost-all-dev

# Build Mallob
# Copy in all the files we need, rather than clone a repository
RUN mkdir mallob
COPY /CMakeLists.txt mallob/CMakeLists.txt
COPY /src mallob/src
COPY /scripts mallob/scripts
COPY /lib mallob/lib

# Build Mallob
WORKDIR mallob
RUN cd lib && bash fetch_and_build_sat_solvers.sh ckly && cd ..
RUN mkdir build
RUN cd build \
  && cmake -DMALLOB_CERTIFIED_UNSAT=1 -DCMAKE_BUILD_TYPE=RELEASE \
    -DMALLOB_SUBPROC_DISPATCH_PATH=\"./\" -DMALLOB_ASSERT=1 -DMALLOB_USE_GLUCOSE=0 \
    -DMALLOB_USE_ASAN=0 -DMALLOB_USE_JEMALLOC=1 -DMALLOB_JEMALLOC_DIR=/usr/lib/x86_64-linux-gnu \
    -DMALLOB_LOG_VERBOSITY=3 .. \
  && VERBOSE=1 make -j4 \
  && cd ..

# Build Dawn's combiner tools
WORKDIR /
RUN git clone https://github.com/RandomActsOfGrammar/distributed-unsat-composition.git
WORKDIR /distributed-unsat-composition
RUN git checkout lrat-input && \
    make && \
    cd ..

WORKDIR /

################### Extract Mallob in run stage
FROM satcomp-base:leader AS mallob_liaison
RUN whoami
USER root
RUN DEBIAN_FRONTEND=noninteractive apt install -y libjemalloc-dev libjemalloc2 gdb psmisc
RUN mkdir logs
RUN chown ecs-user logs
RUN apt-get update && apt install -y libboost-all-dev
USER ecs-user
WORKDIR /
# Copy mallob and solver scripts
COPY --from=builder /mallob/build/mallob mallob
COPY --from=builder /mallob/build/mallob_sat_process mallob_sat_process
COPY --from=builder /mallob/build/mallob_process_dispatcher mallob_process_dispatcher
COPY --from=builder /mallob/lib/drat-trim/lrat-check lrat-check

COPY --from=builder /distributed-unsat-composition/build/compose-proofs compose-proofs 
COPY --from=builder /distributed-unsat-composition/build/renumber-proofs renumber-proofs
COPY --chown=ecs-user /docker/leader/run_mallob.sh /competition
COPY --chown=ecs-user /docker/leader/proof_line_count.sh /competition
COPY --chown=ecs-user /docker/leader/solver /competition
COPY /docker/leader/cleanup /competition
USER root
RUN chmod +x /competition/cleanup
USER ecs-user
RUN chmod +x /competition/run_mallob.sh
RUN chmod +x /competition/solver
RUN chmod +x /competition/proof_line_count.sh
