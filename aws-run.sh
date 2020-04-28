#!/bin/bash
/usr/sbin/sshd -D &

get_num_local_procs() {
    availablecores=$(nproc)
    echo $nproc / 4 | bc
}

PATH="$PATH:/opt/openmpi/bin/"
BASENAME="${0##*/}"
log () {
  echo "${BASENAME} - ${1}"
}
HOST_FILE_PATH="/tmp/hostfile"
#aws s3 cp $S3_INPUT $SCRATCH_DIR
#tar -xvf $SCRATCH_DIR/*.tar.gz -C $SCRATCH_DIR

sleep 2
echo reporting from $(hostname)
echo main node: ${AWS_BATCH_JOB_MAIN_NODE_INDEX}
echo this node: ${AWS_BATCH_JOB_NODE_INDEX}
echo number of nodes: ${AWS_BATCH_JOB_NUM_NODES}
echo Downloading problem from S3: ${COMP_S3_PROBLEM_PATH}

if [[ "${COMP_S3_PROBLEM_PATH}" == *".xz" ]];
then
  aws s3 cp s3://${S3_BKT}/${COMP_S3_PROBLEM_PATH} supervised-scripts/test.cnf.xz
  unxz supervised-scripts/test.cnf.xz
else
  aws s3 cp s3://${S3_BKT}/${COMP_S3_PROBLEM_PATH} supervised-scripts/test.cnf
fi

# Set child by default switch to main if on main node container
NODE_TYPE="child"
if [ "${AWS_BATCH_JOB_MAIN_NODE_INDEX}" == "${AWS_BATCH_JOB_NODE_INDEX}" ]; then
  log "Running synchronize as the main node"
  NODE_TYPE="main"
fi

# wait for all nodes to report
wait_for_nodes () {
  log "Running as master node"

  touch $HOST_FILE_PATH
  ip=$(/sbin/ip -o -4 addr list eth0 | awk '{print $4}' | cut -d/ -f1)
  slots=$(get_num_local_procs)
  
  log "master details -> $ip:$slots"
  echo "$ip slots=$slots" >> $HOST_FILE_PATH
#  echo "$ip" >> $HOST_FILE_PATH
  lines=$(ls -dq /tmp/hostfile* | wc -l)
  while [ "${AWS_BATCH_JOB_NUM_NODES}" -gt "${lines}" ]
  do
    cat $HOST_FILE_PATH
    lines=$(ls -dq /tmp/hostfile* | wc -l)

    log "$lines out of $AWS_BATCH_JOB_NUM_NODES nodes joined, check again in 1 second"
    sleep 1
#    lines=$(sort $HOST_FILE_PATH|uniq|wc -l)
  done

  # All of the hosts report their IP and number of processors. Combine all these
  # into one file with the following script:
  python supervised-scripts/make_combined_hostfile.py ${ip}
  cat combined_hostfile
  
  # Add up all available slots from all nodes
  numproc=0
  while read -r line ; do
      numproc=$(($numproc+$(echo $line|awk '{print $2}'|grep -oE "[0-9]+")))
  done < combined_hostfile
  echo "total of $numproc MPI processes"
  #np=${AWS_BATCH_JOB_NUM_NODES}
  np=$numproc
  
  # REPLACE THE FOLLOWING LINE WITH YOUR PARTICULAR SOLVER
  time mpirun --mca btl_tcp_if_include eth0 --allow-run-as-root -np $np --hostfile combined_hostfile /build/mallob -sinst=supervised-scripts/test.cnf -ba=4 -cbbs=1500 -cbdf=0.75 -cg -derandomize -icpr=0.8 -jc=0 -log=/dev/null -mcl=5 -s=1 -sleep=1000 -T=300 -t=4 -v=4
}

# Fetch and run a script
report_to_master () {
  # get own ip and num cpus
  #
  ip=$(/sbin/ip -o -4 addr list eth0 | awk '{print $4}' | cut -d/ -f1)
  slots=$(get_num_local_procs)
  
  log "I am a child node -> $ip:$slots, reporting to the master node -> ${AWS_BATCH_JOB_MAIN_NODE_PRIVATE_IPV4_ADDRESS}"

  echo "$ip slots=$slots" >> $HOST_FILE_PATH${AWS_BATCH_JOB_NODE_INDEX}
#  echo "$ip" >> $HOST_FILE_PATH${AWS_BATCH_JOB_NODE_INDEX}
  ping -c 3 ${AWS_BATCH_JOB_MAIN_NODE_PRIVATE_IPV4_ADDRESS}
  until scp $HOST_FILE_PATH${AWS_BATCH_JOB_NODE_INDEX} ${AWS_BATCH_JOB_MAIN_NODE_PRIVATE_IPV4_ADDRESS}:$HOST_FILE_PATH${AWS_BATCH_JOB_NODE_INDEX}
  do
    echo "Sleeping 5 seconds and trying again"
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