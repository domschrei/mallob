#!/bin/bash
/usr/sbin/sshd -D &


threads_per_proc=4

# The command launching mallob-mono using the combined hostfile with specified slots.
get_command() {

    global_num_procs=$1
    input=test.cnf
    timelim_secs=1000
    verbosity=4
    
    options="-mono=$input -satsolver=llcgllcgllgcllgcllgcllgcll -cbdf=0.9 -cfhl=300 -log=/dev/null -ihlbd=0 -iflbd=0 -islbd=0 ihlbd=0 -fslbd=0 -fhlbd=0 \
    -smcl=0 -hmcl=0 -s=1 -sleep=1000 -T=$timelim_secs -t=$threads_per_proc -v=$verbosity"
    
    echo mpirun --mca btl_tcp_if_include eth0 --allow-run-as-root -np $global_num_procs --hostfile combined_hostfile ./mallob $options
}

# Evaluates how many instances of my solver can be run ON THIS NODE: available cores divided by threads per process.
get_num_local_procs() {
    availablecores=$(nproc)
    nlp=$(expr ${availablecores} / ${threads_per_proc})
    if [ "$nlp" == "0" ]; then
        nlp=1
    fi
    echo $nlp
}


PATH="$PATH:/opt/openmpi/bin/:/"
BASENAME="${0##*/}"
log () {
  echo "${BASENAME} - ${1}"
}
HOST_FILE_PATH="/tmp/hostfile"
#aws s3 cp $S3_INPUT $SCRATCH_DIR
#tar -xvf $SCRATCH_DIR/*.tar.gz -C $SCRATCH_DIR

sleep 2
echo main node: ${AWS_BATCH_JOB_MAIN_NODE_INDEX}
echo this node: ${AWS_BATCH_JOB_NODE_INDEX}" @ $(hostname)"
echo "IP information: $(hostname -I)"
echo "$(nproc) cores available => $(get_num_local_procs) slots"

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

# Get local IP address and #slots on this host
#ip=$(/sbin/ip -o -4 addr list eth0 | awk '{print $4}' | cut -d/ -f1)
ip=$(hostname -I|awk '{print $2}')
numslots=$(get_num_local_procs)
lochostfile=${HOST_FILE_PATH}${AWS_BATCH_JOB_NODE_INDEX}
# Report to (local) hostfile
echo "$ip slots=$numslots" > $lochostfile
file $lochostfile


# [Master] wait for all nodes to report
wait_for_nodes () {
  log "Running as master node"
  log "master details -> $ip:$numslots"
  log "main IP: $ip"

  lines=$(ls -dq ${HOST_FILE_PATH}* | wc -l)
  while [ "${AWS_BATCH_JOB_NUM_NODES}" -gt "${lines}" ]
  do
    ls ${HOST_FILE_PATH}*
    log "$lines out of $AWS_BATCH_JOB_NUM_NODES nodes joined, check again in 1 second"
    sleep 1
    lines=$(ls -dq ${HOST_FILE_PATH}* | wc -l)
  done

  # All of the hosts report their IP and number of processors. Combine all these
  # into one file with the following script:
  supervised-scripts/make_combined_hostfile.py
  cat combined_hostfile

  num_global_procs=$(($AWS_BATCH_JOB_NUM_NODES * $numslots))
  file ./mallob
  time $(get_command $num_global_procs)
}

# [Child] report hostfile to master
report_to_master () {
  log "I am a child node -> $ip:$numslots, reporting to the master node -> ${AWS_BATCH_JOB_MAIN_NODE_PRIVATE_IPV4_ADDRESS}"

  if [ "$ip" == "$AWS_BATCH_JOB_MAIN_NODE_PRIVATE_IPV4_ADDRESS" ]; then
    echo "Same IP as master node! No transfer necessary."
  else
    echo "Transferring hostfile to $AWS_BATCH_JOB_MAIN_NODE_PRIVATE_IPV4_ADDRESS"
    ping -c 3 ${AWS_BATCH_JOB_MAIN_NODE_PRIVATE_IPV4_ADDRESS}
    until scp $lochostfile ${AWS_BATCH_JOB_MAIN_NODE_PRIVATE_IPV4_ADDRESS}:$lochostfile
    do
        ls ${HOST_FILE_PATH}*
        echo "Sleeping 5 seconds and trying again"
        sleep 5
    done
  fi
  log "Report done! Waiting ..."
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
