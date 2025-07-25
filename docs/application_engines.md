
# Developing an Application Engine for Mallob

Mallob can perform malleable job scheduling and load balancing for any kind of application that implements its interfaces.
In the following we explain which steps need to be performed and how exactly the Job interface works.

## Types of Application Engines

Mallob supports two kinds of application engines:

* **Regular application engines** are fully decentralized. Only the parsing of the job description (if at all needed) is performed by a single client process. All other application-specific logic applies uniformly to all processes where a job is scheduled. Examples are `dummy`, `kmeans`, and `sat`.
* **Client-side application engines** only consist of a single head program, which is executed directly at the client process receiving a job submission. Such a job is thus not distributed in and of itself. However, the head program can submit regular application tasks to Mallob (e.g., SAT solving sub-tasks) to exploit distributed computing. Examples are `maxsat` and `satwithpre`.

## Overview

To integrate your application into Mallob, create a subdirectory `app/yourappkey` where `yourappkey` is an all lower case identifier for your application within Mallob (e.g., `kmeans` or `smt`). In that directory, the following files are strictly required - each is explained in more detail further below.

* `setup.cmake`: Define the build process for your application.
* `options.hpp`: Define custom, application-specific program options.
* `register.hpp`: Define a global method `void register_mallob_app_yourappkey()` (replace "yourappkey" accordingly) where you define a bit of glue code for your app within the `app_registry`. This programmatically connects your application code with Mallob's program flow.

After all of this is set up, register your application within Mallob's build process by adding the following code to `CMakeLists.txt` below the line `# Include further applications here:`.
```cmake
if(MALLOB_APP_YOURAPPKEY) 
    register_mallob_app("yourappkey")
endif()
```
You can then build Mallob with CMake option `-DMALLOB_APP_YOURAPPKEY=1` to include your application.

## `setup.cmake`

The CMake file `app/yourappkey/setup.cmake` must contain all directives necessary to build and include your application in Mallob. In principle, if your code only consists of header files transitively included by a single entry point `register.hpp` and if no additional libraries are required, this file may be completely empty. Otherwise, you should define the compilation units (.cpp files) of your application, add additional include directories and libraries as necessary, and define any external executables your application calls. Please take a look at `app/dummy/setup.cmake` for a simple example which only adds additional compilation units, `app/sat/` for a reasonably complex example featuring external libraries and a separate subprocess executable, and `app/maxsat/` for a client-side application example.

## `options.hpp`

Options in Mallob are grouped into hierarchical categories, e.g., "app", "app/sat", "app/sat/sharing", and so on. While the base options of Mallob are defined in `src/optionslist.hpp`, you should define application-specific program options in `src/app/yourappkey/options.hpp`. In this file, create a base option group for your application like this:
```c++
OPTION_GROUP(grpAppYourappkey, "app/yourappkey", "Options for application XXX")
```
All options defined after this definition and before the next group definition are added to this group. You can create subgroups like "app/yourappkey/x" to organize your program options. Please take a look at `src/app/sat/options.hpp` for a complete example on how options are defined. The groups and options you defined will then be included in the output of `--help` automatically.

## `register.hpp`

A typical `register.hpp` for a **regular** application engine looks as follows:

```C++
#pragma once
#include "app/app_registry.hpp"
// further #includes as needed

void register_mallob_app_yourappkey() {
    // You need to call either app_registry::registerApplication or app_registry::registerClientSideApplication,
    // depending on whether you want to register a regular or a client-side application.
    // The latter expects a ClientSideProgramCreator instead of a JobCreator.
    // See `src/app/app_registry.hpp` for details.
    app_registry::registerApplication(
        // your key in all caps (!) goes here
        "YOURAPPKEY", 

        // Job reader: Given a number of input files and a JobDescription instance,
        // read the files into the JobDescription and return true iff everything went well.
        [](const std::vector<std::string>& files, JobDescription& desc) -> bool {
            // TODO If needed, perform parsing, write via desc.addData()
            return true;
        },

        // Job creator: Return an instance of your custom subclass of Job.
        [](const Parameters& params, const Job::JobSetup& setup) -> Job* {
            return new YourSubclassOfJob(params, setup);
        },

        // Job solution formatter: Given a JobResult instance, return a nlohmann::json
        // object which represents the found solution in some way.
        [](const JobResult& result) -> nlohmann::json {
            // Just create an array of strings, one for each integer in the solution.
            auto json = nlohmann::json::array();
            for (size_t i = 0; i < result.getSolutionSize(); ++i) {
                json.push_back(std::to_string(result.getSolution(i)));
            }
            return json;
        }

        // Optional: Specify a resource cleaner which cleans up any system-level resources
        // (files, shared-memory, ...) in between Mallob executions.
        //, [](const Parameters& params) {}
    );
}
```

In the following sections we shed more light on how to properly realize the job reader and job creator callbacks.

## Job reader

Given a number of input files and a mutable JobDescription instance, parse the files and serialize the job described by the files. A serialization of a job in Mallob is a flat sequence of 32-bit integer or float numbers which describe the job's entire payload. Push individual numbers to this serialization using `desc.addData()`.

Some global parameters can be stored seperately in the AppConfiguration via `desc.getAppConfiguration().updateFixedSizeEntry()`. Such stores need to be done **before** calling `desc.beginInitialization(0)`, otherwise the Job might not be scheduled correctly (as then the AppConfiguration and serialized data are no longer clearly separated).

Your job reading should return with a `bool` which expresses whether the parsing was successful. In case of returning false, please log some warning or error message describing what happened.

For the exact sequence of statements needed, please take a quick look at [dummy_reader.cpp](/src/app/dummy/dummy_reader.cpp) or at the following example:

```C++
bool MyNewReader::read(const std::vector<std::string>& filenames, JobDescription& desc) {
    // Parse header
    std::ifstream ifile(filenames[0].c_str(), std::ios::in);
    std::string line;
    int variables = 0;
    int clauses = 0;
    while (std::getline(ifile, line)) {
        if (line.empty()) continue;
        if (line[0] == 'c') continue;
        if (line[0] == 'p') {
            std::istringstream iss(line);
            std::string tmp;
            iss >> tmp;
            iss >> tmp;
            iss >> variables;
            iss >> clauses;
            break;
        }
    }
    // Store header information - must be done **before** desc.beginInitialization !
    auto& config = desc.getAppConfiguration();
    config.updateFixedSizeEntry("__NC", clauses);
    config.updateFixedSizeEntry("__NV", variables);

    // Parse the formula
    desc.beginInitialization(0);
    int lit;
    while (ifile >> lit) {
        desc.addData(lit);
    }

    // Conclude parsing
    desc.endInitialization();
    ifile.close();

    // read was successful
    return true;
}
```

## Job engine

For regular application engines, the core part is to create a custom subclass of `Job` (see `src/app/job.hpp`) and to implement all of its pure virtual methods.

The `dummy` application serves as an educative starting point - please take a look at the example implementations in `src/app/dummy/*_example_job.hpp`, which highlight different kinds of job-internal communication. You can run these examples by modifying the job creator in `src/app/dummy/register.hpp` accordingly.

* [pointtopoint_example_job.hpp](/src/app/dummy/pointtopoint_example_job.hpp): A simple example for point-to-point messaging, setting a job's desired resources, and waiting for sufficient resources to become active.
* [collectives_example_job.hpp](/src/app/dummy/collectives_example_job.hpp): An example for collective operations (broadcast, all-reduction), background computations, and how to properly clean everything up.

The `SAT` application is a more involved example for a full-featured application; its job logic is relatively complex due to sub-processing.

### Core principles

* Your subclass defines the logic that is being executed _at every process_ allotted for one of your application tasks.
* The important methods to override begin with `appl_*`. For Mallob's scheduling to remain interactive, these methods should not perform any heavy lifting; they should return within a millisecond or less. Thus, any heavy computation should be outsourced to it's own thread, e.g via `ProcessWideThreadPool::get().addTask(...)`. 
* The actual execution of a task begins with `appl_start`. In this method, you can assume that the complete job description is present (unless you modified the method `canHandleIncompleteRevision`). Separate threads should be launched to participate in processing the respective task.
* Communication across processes within a distributed task takes place via the two methods named `appl_communicate`. For outgoing communication, the method without any parameters is called periodically by the MPI process' main thread; here, you can send messages to other processes. For incoming communication, the method with several parameters is called whenever a message arrives that matches the current application context.
* You may modify the method `getDemand` to have your application signify how many workers it can currently handle. For instance, if your computation is in a stage where only a single worker can be used effectively, the method should return 1. For the default case, you should still call the superclass method `(Job::getDemand)`.

### Communication

The following communication options are available:

* `getJobTree()` returns the primary communication structure of your distributed task, the binary tree of workers. Use this object to find out the current job context's role (e.g., `getJobTree().getIndex()` for this worker's position in the job tree) and to send messages quickly and easily (e.g., `getJobTree().sendToParent()`).
* For aggregations like MPI all-reduction or all-gather operations, you can create a `JobTreeAllReduction` instance, which provides the logic for a single generic aggregation along the job tree followed by a broadcast of the result.
* For arbitrary point-to-point messages within your task, you can use the communication structure `getJobComm()` and the rank list provided within to send custom messages via `MyMpi::isend`. Note that this communicator needs to be activated explicitly via `-jcup=<non-zero update interval>` and only represents a snapshot of a task's workers.

Notable details:

* Application-specific messages are represented by instances of `JobMessage`. A message's payload must be given as a vector of integers; serialize your particular data accordingly. Define your own custom and fresh message tags (integers) for all of your application-specific messages; they are central for correct and unambiguous dispatching.
* You may need to explicitly handle messages that your logic previously sent but that turned out to be undeliverable. Such a situation presents itself via an incoming message with the `returnedToSender` flag set.

### Multi-threading

* You can use one or several instances of `BackgroundWorker` to perform long-term background tasks. Mind this object's life time! The destructor joins the associated background thread, but you need to make sure that the background tasks actually gets the memo to terminate (periodically check `worker.continueRunning()` in the background task of `worker`).
* You can concurrently perform a task by using Mallob's thread pool (`ProcessWideThreadPool::get().addTask`), which also comes with lowered overhead compared to a `BackgroundWorker` if an idle thread is available immediately. Use the `std::future` object returned by the thread pool method to wait for the task to finish (and to ensure that associated resources can be cleaned up).

## Client-side program

For client-side application engines, your application logic is provided via the client-side program. This program may spawn side threads (e.g., via `ProcessWideThreadPool::get().addTask(...)`).
It can also submit tasks to Mallob, by using the `APIConnector` instance provided in the constructor or by fetching it from anywhere via `APIRegistry::get()`.
See [src/app/satwithpre/sat_preprocess_solver.hpp](/src/app/satwithpre/sat_preprocess_solver.hpp) for an example.
