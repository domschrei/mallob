# Using Mallob on a custom server

On machines that use the [spack](https://spack.readthedocs.io/en/latest/index.html) package manager and the [slurm](https://ae.iti.kit.edu/wiki/index.php?title=Slurm) job scheduler, Mallob can be used in the following way. 

On the login node, from within the ```mallob/``` directory, execute
    
    source scripts/server/create_mallob_env.sh 

This creates a persistent spack environment ```mallob_env``` that contains all necessary compilers and libraries (see [create_mallob_env.sh](/scripts/server/create_mallob_env.sh) for details). If you want to extend this environment, run the command above with the flag ```--fresh``` to force a clean reinstall.

Still from within the ```mallob/``` directory, you can now execute

    sbatch --partition=blum scripts/server/build_and_run_example.sh

to run the script ```build_and_run_example.sh``` on a chosen target machine, here examplary on ```blum```. We include a compilation step in the submitted job to have the binary be compiled (and thus optimized) for the CPU of the target machine.   

See [build_and_run_example.sh](/scripts/server/build_and_run_example.sh) for more details on building Mallob and [run_example.sh](/scripts/server/run_example.sh) for a simple loop that executes Mallob on some instances, and outputs the log files and results.

To check for available machines

    sinfo

To see all currently running machines 

    squeue

To stop all of your jobs

    scancel -u $USER

While the job is running, its terminal output is redirected to a growing ```slurm-*``` file in your ```mallob/``` directory. You can check this for general progress and some potential Error messages.  

Log files are written to ```scripts/server/example_logsntraces```, at least for this dummy example. A simple check for any errors can be done there via

    grep ERROR -r *

which should ideally return nothing. Similarly, a simple check for the SAT solving times can be done there via

    grep SOLUTION -r *

See [develop.md](/docs/develop.md) for more information on the log files and how to use them.

