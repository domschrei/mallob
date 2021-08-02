################
FROM ubuntu:20.04 AS mallob_base
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt install -y openssh-server iproute2 openmpi-bin openmpi-common iputils-ping \
    && mkdir /var/run/sshd \
    && sed 's@session\s*required\s*pam_loginuid.so@session optional pam_loginuid.so@g' -i /etc/pam.d/sshd \
    && setcap CAP_NET_BIND_SERVICE=+eip /usr/sbin/sshd \
    && useradd -ms /bin/bash mallob \
    && chown -R mallob /etc/ssh/ \
    && su - mallob -c \
        'ssh-keygen -q -t rsa -f ~/.ssh/id_rsa -N "" \
        && cp ~/.ssh/id_rsa.pub ~/.ssh/authorized_keys \
        && cp /etc/ssh/sshd_config ~/.ssh/sshd_config \
        && sed -i "s/UsePrivilegeSeparation yes/UsePrivilegeSeparation no/g" ~/.ssh/sshd_config \
        && printf "Host *\n\tStrictHostKeyChecking no\n" >> ~/.ssh/config'
WORKDIR /home/mallob
ENV NOTVISIBLE "in users profile"
RUN echo "export VISIBLE=now" >> /etc/profile
EXPOSE 22
################
FROM ubuntu:20.04 AS builder
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt install -y cmake build-essential zlib1g-dev libopenmpi-dev git wget unzip build-essential zlib1g-dev iproute2 cmake python3 python3-pip build-essential gfortran wget curl
# Build mallob
ADD src src
ADD lib lib
ADD CMakeLists.txt .
RUN ls -lt
RUN cd lib && bash fetch_and_build_sat_solvers.sh && cd ..
RUN mkdir build
RUN cd build && cmake -DCMAKE_BUILD_TYPE=RELEASE -DMALLOB_USE_RESTRICTED=1 .. && VERBOSE=1 make && cd ..
################
FROM mallob_base AS mallob_liaison
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt install -y awscli python3 mpi
COPY --from=builder build/mallob mallob
COPY --from=builder build/mallob_sat_process mallob_sat_process
RUN chmod 755 mallob
RUN chmod 755 mallob_sat_process
ADD mpi-run-aws.sh supervised-scripts/mpi-run.sh
RUN chmod 755 supervised-scripts/mpi-run.sh
USER mallob
CMD ["/usr/sbin/sshd", "-D", "-f", "/home/mallob/.ssh/sshd_config"]
CMD supervised-scripts/mpi-run.sh
