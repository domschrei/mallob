
# SLURM Scripting Setup

This directory contains the following files (in chronological order w.r.t. the usual workflow):

### `account.sh`

Fill in your project name and user name.

### `prolog.sh`, `epilog.sh`

Scripts that are executed before and after a Mallob run respectively. As long as you follow the default setup, you shouldn't need to make any changes in there.

### `sbatch.sh`

A template for the core script that represents the job(s) to be executed in the HPC cluster. Adjust the SLURM variables at the top to your liking and take especial care for the places marked with `TODO`.

### `generate-job-chain.sh`

A script that takes `sbatch.sh` and "instantiates" it to an actual SBATCH file. Use as follows:

```
DS_NODES=1 DS_RUNTIME=720 DS_PARTITION=micro DS_SECONDSPERJOB=300 scripts/slurm/generate-job-chain.sh sat-remadetest-1node scripts/slurm/sbatch.sh 1 50 10
```

* `DS_NODES`: Number of compute nodes to use
* `DS_RUNTIME`: The maximum runtime of a single SBATCH job, including setup/teardown. Can encompass several Mallob runs.
* `DS_PARTITION`: The HPC partition to execute the job in (e.g., `micro`, `general`)
* `DS_SECONDSPERJOB`: Duration of each individual Mallob run (excluding some leniency for setup, teardown).
* 1st argument: The job name.
* 2nd argument: The SBATCH script template to use.
* 3rd argument: The index of the first instance to solve.
* 4th argument: The index of the last instance to solve.
* 5th argument: The number of SBATCH jobs that should run concurrently (beware user job limits, e.g., 50 for SuperMUC).

The script tells you what to execute to submit your (meta-)job.

### `postrun.sh`

Execute with your job name as the only argument (e.g., `scripts/slurm/postrun.sh sat-remadetest-1node`) to move all generated data into a single unified directory.

### `basic-eval.sh`

Provide the directory produced by `postrun.sh` as the only argument. Creates a file `qtimes.txt` ("qualified running times") within that directory that features basic by-instance results.

