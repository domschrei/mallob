# GPU support

To use GPU accelerated sat solving, the mallob gpu api (MGI) needs to be enabled.

## Prerequisits

> [!WARNING]  
> Currently MGI only supports **OpenCL 3.0** as backend

The usage of `clang` and the respective packages for `llvm-spirv` are highly recommanded and are default enabled.
`clang++`, `llvm-spirv` and all required packages are assumed to be installed. 
If you want to disable OpenCL C to SPIR-V auto-compile use the appropriate flag in the flags section.

### NVIDIA

You need the newest [CUDA Toolkit](https://developer.nvidia.com/cuda-downloads) from NVIDIA

> [!NOTE]
> This is currently tested on *Fedora 43* and *Debian 13*

### AMD

TODO

> [!WARNING]  
> AMD Support is currently untested

### Intel

The Intel [Graphics Compute Runtime](https://github.com/intel/compute-runtime) is needed for OICDs.
Hardware support and installation instructions can be found on their Github page.

> [!WARNING]  
> Intel Support is currently untested

### WSL

TODO -> Try

## Usage

In order to compile mallob with GPU features the support must be enabled through CMake.

### Using presets

We provide a `CMakePresets.json` with GPU [configure presets](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html) for ease of use.
The `linux-debug-gpu` (Nativ Linux GPU Debug) preset is currently the main gpu preset.
You can either enable this your IDE/CMake Extension or manually use it in command line.
> cmake --preset linux-debug-gpu

### Manually

Otherwise the `MALLOB_USE_GPU` cmake flag can be set.

> cmake ./ -DMALLOB_USE_GPU=1

Additionally if you want more output consider using the `MALLOB_ASSERT` flag which enables debug output for the API layer.
Provided the `MALLOB_LOG_VERBOSITY` is set correctly (>=debug).

## Flags

|Flag|Usage|Default|
|-|-|-|
|`MALLOB_MGI_OCL_LAYERS`| Automatically download and add a compile target for the OpenCL validation layers. Needs environment configuration. This is automatically configured if the preset is used | On
|`MALLOB_MGI_TEST_COMPILE`| Enable clang OpenCL-C++ to SPIR-V compiler that automatically run to check your kernels for errors. | Off (On in preset)

## Development

## Defines

These defines can be used to check for features in the code C++ host code.

|Flag|Usage|
|-|-|
|`MGI_API_OCL`| Enabled if OpenCL is used as backend 
|`MALLOB_USE_GPU`| Enabled if MGI is enabled.


## Basics

![Arch](pictures/arch.png)

### Memory types

`Allocation` type describes the allocation location and features of the memory. This is controlled by the host and is represented by the `MemoryType` enum. They control which device (Host or GPU) can read and write to the memory.
If both device have access at all time to the memory it highers the amount of overhead for each memory access. While memory with less access rights needs more time during host updates (with writeMemory, readMemory).

```cpp
    enum class MemoryType
    {
        Global,      // All read/write
        Uniform,     // Host read/write, Kernel read only
        DeviceLocal, // Kernel read/write only
        Constant     // Kernel read only
    };
```

This is not to be confused with `Device` memory types. Device memory types are used in the shaders and only control which memory in the device hirachie is used. Memory allocated by the host is always `MGI_GLOBAL` memory. The other types can be seen in the diagram. `MGI_LOCAL` are registers which can be seen in the subgroup space. `MGI_SHARED` is the shared memory that is shared between subgroups. They have increasingly less latency.

```cpp
    #define MGI_GLOBAL global 
    #define MGI_SHARED local 
    #define MGI_CONST const constant
    #define MGI_LOCAL private 
```

### Architecture

![ArchMGI](pictures/mgi.png)

#### Memory Managment



- `allocate` reservers new device memory. The new buffers might or might not be allocated in the same memory region depending on the `strategy`
```cpp
std::vector<Memory> allocate(span<const AllocationInfo> infos, const AllocationStrategy &strategy = {})
```

- `readMemory` makes device memory available to the host. Depending on the memory type this requires copying the requested sections. It returns a lock which synchronizes host access to the memory. It does **NOT** synchronize the device. Synchronizing device memory access is up to the user.
```cpp
ReadLock readMemory(Memory memory, span<const ReadInfo> reads)
```

- `writeMemory` writes the given memory to the device. Depending on the memory type this might require an additional copying of the requested sections. The memory is protected for host writes. It does **NOT** synchronize the device. Synchronizing device memory access is up to the user.
```cpp
void writeMemory(Memory memory, span<const BufferUpdateInfo> updates)
```

- `copyMemory` copies memory from one device memory to an other. This operation is a **deferred** operation. If you want the operation to finish after the call has completed us the `*Wait` version.
For more information on the deffered operation see [Task Managment](#tasks)
```cpp
Task copyMemory(Memory source, Memory destination, span<const MemoryCopyInfo> copyInfos)

void copyMemoryWait(Memory source, Memory destination, span<const MemoryCopyInfo> copyInfos)
```

- `freeObj` frees the memory object.
```cpp
void freeObj(Memory memory)
```

#### Task Managment {#tasks}

- `queueTasks` launches an operation on the device. This operation is a **deferred** operation. The call returns before the operations are completed. The call is thread safe on the host. To wait for the tasks use the `waitTasks` call or none deferred variant.
```cpp
std::vector<Task> queueTasks(span<const TaskInfo> tasks, const TaskStrategy &strategy = {})

std::vector<TaskStatus> queueWaitTasks(span<const TaskInfo> taskInfos, const TaskStrategy &strategy = {})

void waitTasks(span<const Task> tasks)
```

- `getStatus` returns the current status of the operation. What this can return is dependent on the backend used.
```cpp
std::vector<TaskStatus> getStatus(span<const Task> tasks)
void freeObj(Task task)
```

#### Kernel

- `loadKernel` loads a kernel file and compiles it on the host. This operation is not synchronized (This might change). Kernels are loaded from the 
```cpp
Kernel loadKernel(const std::string &file, const KernelCache &cache = {})
```

#### Intigration

### Kernel Example

TODO -> From Notes
