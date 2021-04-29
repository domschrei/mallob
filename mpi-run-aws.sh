#!/bin/bash
/usr/sbin/sshd -D &

PATH="$PATH:/opt/openmpi/bin/"
BASENAME="${0##*/}"
log () {
  echo "${BASENAME} - ${1}"
}
HOST_FILE_PATH="/tmp/mallob_hostfile"

# Workaround for MPI error messages "Read -1, expected <some number>, errno = 1"
OMPI_MCA_btl_vader_single_copy_mechanism=none
export OMPI_MCA_btl_vader_single_copy_mechanism=none

sleep 2

if [ -z ${AWS_BATCH_JOB_NODE_INDEX} ]; then
    # Parallel (non-distributed) version: only given S3_BKT and COMP_S3_PROBLEM_PATH
    echo running on a single node
    AWS_BATCH_JOB_MAIN_NODE_INDEX=0
    AWS_BATCH_JOB_NODE_INDEX=0
    AWS_BATCH_JOB_NUM_NODES=1
else
    echo main node: ${AWS_BATCH_JOB_MAIN_NODE_INDEX}
    echo this node: ${AWS_BATCH_JOB_NODE_INDEX}
fi

# Download instance to solve
echo Downloading problem from S3: ${COMP_S3_PROBLEM_PATH}
if [[ "${COMP_S3_PROBLEM_PATH}" == *".xz" ]];
then
  aws s3 cp s3://${S3_BKT}/${COMP_S3_PROBLEM_PATH} test.cnf.xz
  unxz test.cnf.xz
else
  aws s3 cp s3://${S3_BKT}/${COMP_S3_PROBLEM_PATH} test.cnf
fi

# Set child by default switch to main if on main node container
NODE_TYPE="child"
if [ "${AWS_BATCH_JOB_MAIN_NODE_INDEX}" == "${AWS_BATCH_JOB_NODE_INDEX}" ]; then
  log "Running synchronize as the main node"
  NODE_TYPE="main"
fi

availablecores=$(nproc) # available hardware threads on this physical node
numthreads=4 # threads per MPI process
procspernode=$(($availablecores / $numthreads)) # MPI processes per physical node
# total number of processes to be started
numprocesses=$(($procspernode * $AWS_BATCH_JOB_NUM_NODES))
echo "$availablecores local cores, $procspernode local MPI procs, $numprocesses MPI procs in total"

# wait for all nodes to report
wait_for_nodes () {
  log "Running as master node"

  touch $HOST_FILE_PATH
  if [ "$AWS_BATCH_JOB_NUM_NODES" == "1" ]; then
    ip=localhost
  else
    ip=$(/sbin/ip -o -4 addr list eth0 | awk '{print $4}' | cut -d/ -f1)
  fi

  log "master details -> $ip:$availablecores"
  log "main IP: $ip"
  ls -lt
  
  echo "$ip slots=$procspernode" >> $HOST_FILE_PATH
  lines=$(ls -dq /tmp/mallob_hostfile* | wc -l)
  while [ "${AWS_BATCH_JOB_NUM_NODES}" -gt "${lines}" ] ; do
    cat $HOST_FILE_PATH
    lines=$(ls -dq /tmp/mallob_hostfile* | wc -l)
    log "$lines out of $AWS_BATCH_JOB_NUM_NODES nodes joined, check again in 1 second"
    sleep 1
  done

  # All of the hosts report their IP and number of processors. Combine all these
  # into one file with the following script:
  cat /tmp/mallob_hostfile* > combined_hostfile
  cat combined_hostfile

  ##################################################################################
  # Configuration and execution of Mallob

  time=1000 # time limit in seconds

  # v switch is for verbosity, 0o switch suppresses all output from ranks > 0
  # To receive more information, you can set this to "-v=4" (without -0o=1) instead
  verbosity="-v=2 -0o=1"

  # Reduce #threads per process if this limit of imported literals per process would be exceeded.
  # Set to 50M literals per thread, should be fine for m4.16xlarge and m4.4xlarge.
  max_literals_per_process=$((50000000 * $numthreads))

  # Portfolio of solvers to cycle through: l=lingeling/yalsat g=glucose c=cadical
  # Due to the SC2021 competition rules, only lingeling+yalsat is allowed
  portfolio="l"

  # Clause buffer decay factor: 1 for single-node, 0.9 for multi-node
  if [ "$AWS_BATCH_JOB_NUM_NODES" == "1" ]; then
    cbdf="1.0"
  else
    cbdf="0.9"
  fi

  log "start total of $numprocesses MPI processes"

  time mpirun --mca btl_tcp_if_include eth0 --allow-run-as-root -np $numprocesses \
  --hostfile combined_hostfile --use-hwthread-cpus --map-by node:PE=$numthreads --bind-to none --report-bindings \
  ./mallob -mono=test.cnf -satsolver=$portfolio -cbbs=1500 -cbdf=$cbdf \
  -shufinp=0.03 -shufshcls=1 -slpp=$max_literals_per_process \
  -cfhl=300 -ihlbd=8 -islbd=8 -fhlbd=8 -fslbd=8 -smcl=30 -hmcl=30 \
  -s=1 -sleep=1000 -T=$time -t=$numthreads -appmode=thread -nolog $verbosity
}

# Fetch and run a script
report_to_master () {
  # get own ip and num cpus
  #
  ip=$(/sbin/ip -o -4 addr list eth0 | awk '{print $4}' | cut -d/ -f1)
  log "I am a child node -> $ip:$availablecores, reporting to the master node -> ${AWS_BATCH_JOB_MAIN_NODE_PRIVATE_IPV4_ADDRESS}"

#  echo "$ip slots=$availablecores" >> $HOST_FILE_PATH${AWS_BATCH_JOB_NODE_INDEX}
  echo "$ip slots=$procspernode" >> $HOST_FILE_PATH${AWS_BATCH_JOB_NODE_INDEX}
  ping -c 3 ${AWS_BATCH_JOB_MAIN_NODE_PRIVATE_IPV4_ADDRESS}
  until scp $HOST_FILE_PATH${AWS_BATCH_JOB_NODE_INDEX} ${AWS_BATCH_JOB_MAIN_NODE_PRIVATE_IPV4_ADDRESS}:$HOST_FILE_PATH${AWS_BATCH_JOB_NODE_INDEX}
  do
    echo "Sleeping 5 seconds and trying again"
    sleep 5
  done
  log "done! goodbye"
  ps -ef | grep sshd
  tail -f /dev/null
}
##
#
# Main - dispatch user request to appropriate function
log $NODE_TYPE
case $NODE_TYPE in
  main)
    wait_for_nodes "${@}"
    ;;

  child)
    report_to_master "${@}"
    ;;

  *)
    log $NODE_TYPE
    usage "Could not determine node type. Expected (main/child)"
    ;;
esac
