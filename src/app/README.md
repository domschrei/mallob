
# Developing an Application Engine for Mallob

Mallob can perform malleable job scheduling and load balancing for any kind of application that implements its interfaces.
In the following we explain which steps need to be performed and how exactly the Job interface works.

## Overview

To integrate your application into Mallob, create a subdirectory `app/yourappkey` where `yourappkey` is an all lower case identifier for your application within Mallob (e.g., `kmeans` or `smt`). In that directory, the following files are strictly required - each is explained in more detail further below.

* `setup.cmake`: Define the build process for your application.
* `options.hpp`: Define custom, application-specific program options.
* `register.hpp`: Define a global method `void register_mallob_app_yourappkey()` (replace "yourappkey" accordingly) where you define three particular lambda functions for your app within the `app_registry`. This bit of code programmatically connects your application code with Mallob's program flow.

After all of this is set up, register your application within Mallob's build process by adding the following code to `CMakeLists.txt` below the line `# Include further applications here:`.
```cmake
if(MALLOB_APP_YOURAPPKEY) 
    register_mallob_app("yourappkey")
endif()
```
You can then build Mallob with CMake option `-DMALLOB_APP_YOURAPPKEY=1` to include your application.

## `setup.cmake`

The CMake file `app/yourappkey/setup.cmake` must contain all directives necessary to build and include your application in Mallob. In principle, if your code only consists of header files transitively included by a single entry point `register.hpp` and if no additional libraries are required, this file may be completely empty. Otherwise, you should define the compilation units (.cpp files) of your application, add additional include directories and libraries as necessary, and define any external executables your application calls. Please take a look at `app/dummy/setup.cmake` for a simple example which only adds additional compilation units and `app/sat/setup.cmake` for a reasonably complex example featuring external libraries and a separate subprocess executable.

## `options.hpp`

Options in Mallob are grouped into hierarchical categories, e.g., "app", "app/sat", "app/sat/sharing", and so on. While the base options of Mallob are defined in `src/optionslist.hpp`, you should define application-specific program options in `src/app/yourappkey/options.hpp`. In this file, create a base option group for your application like this:
```c++
OPTION_GROUP(grpAppYourappkey, "app/yourappkey", "Options for application XXX")
```
All options defined after this definition and before the next group definition are added to this group. You can create subgroups like "app/yourappkey/x" to organize your program options. Please take a look at `src/app/sat/options.hpp` for a complete example on how options are defined. The groups and options you defined will then be included in the output of `--help` automatically.

## `register.hpp`

A typical `register.hpp` looks as follows:

```C++
#pragma once
#include "app/app_registry.hpp"
// further #includes as needed

void register_mallob_app_yourappkey() {
    app_registry::registerApplication(
        // your key in all caps goes here
        "YOURAPPKEY", 

        // Job reader: Given a number of input files and a JobDescription instance,
        // read the files into the JobDescription and return true iff everything went well.
        [](const std::vector<std::string>& files, JobDescription& desc) -> bool {
            // TODO perform parsing, write via desc.add{Permanent,Transient}Data()
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
    );
}
```

In the following sections we shed more light on how to properly realize the job reader and job creator callbacks.

## Job reader

Given a number of input files and a mutable JobDescription instance, parse the files and serialize the job described by the files. A serialization of a job in Mallob is a flat sequence of 32-bit integer or float numbers which describe the job's entire payload. Push individual numbers to this serialization using `desc.addPermanentData()`.

For incremental jobs, there is a second kind of serialization for each job revision introduced, which can be pushed to via `desc.addTransientData()`. The purpose of this serialization is to describe parts of the job payload which are to be acknowledged only for this very revision whereas data pushed via `addPermanentData()` is a fixed part of all subsequent revisions as well. This subdivision into two different serializations is purely for convenience; you may completely ignore this second kind of serialization and encode everything via `addPermanentData()`.

Your job reading should return with a `bool` which expresses whether the parsing was successful. In case of returning false, please log some warning or error message describing what happened.

## Job engine

The core part of adding a new application engine to Mallob is to create a custom subclass of `Job` (see `src/app/job.hpp`) and to implement all of its pure virtual methods. (More documentation coming soon.)
