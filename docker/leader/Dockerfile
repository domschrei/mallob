################### Build Mallob
FROM satcomp-base:leader AS builder
USER root
# SHELL ["/bin/bash", "-c"]
#  Install required softwares
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt install -y cmake build-essential zlib1g-dev libopenmpi-dev wget unzip build-essential zlib1g-dev cmake python3 build-essential gfortran wget curl libjemalloc-dev libjemalloc2 gdb libboost-all-dev
# Install Rust for checking tools.
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o rustup.sh
RUN chmod 775 rustup.sh
RUN ./rustup.sh -y -q
ENV PATH="/root/.cargo/bin:$PATH"

# Build Dawn's tools..skipping for now; difficult to get package rights for code.amazon.com
# RUN git clone ssh://git.amazon.com/pkg/AtsScienceDistributedProofComposition
# RUN cd AtsScienceDistributedProofComposition
# RUN make
# RUN find .
# RUN cd ..

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
#RUN find /usr -name '*libjemalloc*'
RUN find . -name frat*

################### Extract Mallob in run stage
FROM satcomp-base:leader AS mallob_liaison
RUN whoami
USER root
RUN DEBIAN_FRONTEND=noninteractive apt install -y libjemalloc-dev libjemalloc2 gdb psmisc
USER ecs-user
WORKDIR /
# Copy mallob and solver scripts
COPY --from=builder /mallob/build/mallob mallob
COPY --from=builder /mallob/build/mallob_sat_process mallob_sat_process
COPY --from=builder /mallob/build/mallob_process_dispatcher mallob_process_dispatcher
COPY --from=builder /mallob/lib/frat/frat-rs frat_rs
RUN echo "Installing Dawn's tool by copying (Gitfarm is too difficult to add)."
RUN echo "If this fails, clone: ssh://git.amazon.com/pkg/AtsScienceDistributedProofComposition "
RUN echo "Then copy the tool (in build/compose-proofs) into the root level of mallob."
COPY --chown=ecs-user compose-proofs /competition
COPY --chown=ecs-user /docker/leader/run_mallob.sh /competition
COPY --chown=ecs-user /docker/leader/solver /competition
COPY --chown=ecs-user /docker/leader/cleanup /competition
USER ecs-user
RUN chmod +x /competition/run_mallob.sh
RUN chmod +x /competition/solver
RUN chmod +x /competition/cleanup
