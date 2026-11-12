#pragma once

#include "DeferredAPICommon.hpp"

#include <vulkan/vulkan.hpp>

#define CHECK_RESULT(result)                                             \
    if (result != vk::Result::eSuccess)                                  \
    {                                                                    \
        LOG(V0_CRIT, "VkResult is %s\n", vk::to_string(result).c_str()); \
        LOG(V0_CRIT, "In %s:%d\n", __FILE__, __LINE__);                  \
    }

namespace mgi
{
    using MemTypeTable = std::array<uint32_t, (uint32_t)MemoryType::Constant + 1>;

    struct VulkanSetup
    {
        vk::Instance instance;
        std::vector<vk::PhysicalDevice> physicalDevices;
        std::vector<uint32_t> queueFamilies;
        std::vector<vk::Device> devices;
        std::vector<vk::Queue> queues;
        std::vector<vk::CommandPool> commandPools;
        std::vector<MemTypeTable> memoryTypeIndices;
        std::vector<std::mutex *> queuesProtects;
    };

    struct PackedBuffer
    {
        vk::Buffer buffer;
        vk::DeviceMemory memory;

        inline void destroy(vk::Device device)
        {
            device.destroy(buffer);
            device.free(memory);
        }
    };

    struct VulkanMemoryRegion
    {
        vk::DeviceMemory memory;
        size_t offset;
    };

    struct CDVulkanHostAccessable
    {
        vk::Device device;
        vk::DeviceMemory memory;
    };

    struct CDVulkanDeviceLocal
    {
        vk::Device device;
        PackedBuffer packed;
    };


    struct VulkanPipelineInfo {
        vk::DescriptorSetLayout setLayout;
        vk::PipelineLayout pipeLayout;
        vk::Pipeline pipeline;
        vk::DescriptorPool pool;
        std::vector<uint32_t> bindings;
        std::vector<vk::PushConstantRange> pushConstants;
    };

    class VulkanDeferredAPI
    {

        VulkanSetup setup;
        std::vector<vk::ShaderModule> shaderModules;
        std::shared_mutex shaderModuleLock;
        KernelLoader loader;
        ProtectedMap<std::unordered_map<size_t, VulkanMemoryRegion>> bufferToMemLookup;
        ProtectedMap<std::unordered_map<size_t, size_t>> memoryCounter;

        ProtectedMap<std::unordered_map<size_t, MemoryType>> memoryType;
        ProtectedMap<std::unordered_map<size_t, std::shared_mutex *>> memoryMutex;
        std::vector<std::unordered_map<std::string, VulkanPipelineInfo>> pipelineInfo;

        friend class KernelLoader;

        uint32_t selectDevice()
        {
            // TODO make this device selection MPI dependent
            return 0;
        }

        vk::Queue selectQueue()
        {
            // TODO make this device selection MPI dependent
            return setup.queues[0];
        }

        inline PackedBuffer allocatePackedBuffer(size_t size, vk::Device device, size_t memType, vk::BufferUsageFlags flags)
        {
            vk::BufferCreateInfo bufferInfo({}, size, flags);
            const auto buffer = device.createBuffer(bufferInfo);
            const auto memRequ = device.getBufferMemoryRequirements(buffer);

            vk::MemoryAllocateInfo allocInfo(memRequ.size, memType);
            const auto mem = device.allocateMemory(allocInfo);
            device.bindBufferMemory(buffer, mem, 0);
            return {buffer, mem};
        }

        template <class T>
        inline void upload(vk::Device device, vk::DeviceMemory memory, span<const T> values, size_t offset = 0)
        {
            T *ptr = (T *)device.mapMemory(memory, offset, values.size_bytes());
            std::copy(values.begin(), values.end(), ptr);
            device.unmapMemory(memory);
        }

        inline void endSubmitAndWait(vk::Device device, vk::CommandPool cmdPool, vk::CommandBuffer cmd)
        {
            cmd.end();
            const auto queue = selectQueue();
            vk::SubmitInfo submitInfo = {{}, {}, cmd};
            const auto fence = device.createFence({});
            queue.submit(submitInfo, fence);
            const auto result = device.waitForFences(fence, true, UINT64_MAX);
            CHECK_RESULT(result);
            device.freeCommandBuffers(cmdPool, cmd);
            device.destroy(fence);
        }

        inline vk::CommandBuffer beginAndGet(vk::Device device, vk::CommandPool cmdPool) {
            const auto cmd = device.allocateCommandBuffers({cmdPool, vk::CommandBufferLevel::ePrimary, 1}).back();
            vk::CommandBufferBeginInfo beginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
            cmd.begin(beginInfo);
            return cmd;
        }

        static void deleteHostAccessable(std::vector<void *> &ptr, void *cData)
        {
            CDVulkanHostAccessable *data = (CDVulkanHostAccessable *)cData;
            data->device.unmapMemory(data->memory);
            delete data;
        }

        static void deleteDeviceLocal(std::vector<void *> &ptr, void *cData)
        {
            CDVulkanDeviceLocal *data = (CDVulkanDeviceLocal *)cData;
            data->device.unmapMemory(data->packed.memory);
            data->packed.destroy(data->device);
            delete data;
        }

    public:
        VulkanDeferredAPI(VulkanSetup &&setup) : setup(std::move(setup)) {}

        Kernel loadKernel(const std::string &file, const KernelCache &cache = {})
        {
            return loader.loadKernel(this, file);
        }

        std::vector<Memory> allocate(span<const AllocationInfo> infos, const AllocationStrategy &strategy = {})
        {
            std::vector<Memory> allocated;
            const auto deviceID = selectDevice();
            const auto &memTypeIndices = setup.memoryTypeIndices[deviceID];
            const auto device = setup.devices[deviceID];

            std::vector<MemoryRequirements> requirements;
            for (const auto &region : infos)
            {
                auto bufferUsage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc;
                vk::BufferCreateInfo bufferInfo({}, region.size, bufferUsage);
                const auto buffer = device.createBuffer(bufferInfo);
                const auto memRequ = device.getBufferMemoryRequirements(buffer);
                requirements.push_back({memRequ.size, memRequ.alignment});
                Memory m;
                m.internal = (size_t)(VkBuffer)buffer;
                allocated.push_back(m);
            }

            const auto slabsToAlloc = strategy.slabs(infos, requirements);
            std::vector<vk::DeviceMemory> allocatedMem;
            for (const auto &slab : slabsToAlloc)
            {
                const auto memTypeIndex = memTypeIndices[(uint32_t)slab.type];
                vk::MemoryAllocateInfo allocInfo(slab.size, memTypeIndex);
                allocatedMem.push_back(device.allocateMemory(allocInfo));
            }

            const auto cmdPool = setup.commandPools[deviceID];
            const auto regionsToAlloc = strategy.regions(infos, requirements);
            std::vector<PackedBuffer> packedBuffer;

            const auto cmd = beginAndGet(device, cmdPool);
            for (size_t i = 0; i < regionsToAlloc.size(); i++)
            {
                const auto &region = regionsToAlloc[i];
                const auto intMem = allocated[i];
                const vk::Buffer buffer = *((VkBuffer *)&intMem.internal);
                const auto mem = allocatedMem[region.index];
                device.bindBufferMemory(buffer, mem, region.offset);

                bufferToMemLookup.insert({intMem.internal, VulkanMemoryRegion{mem, region.offset}});
                memoryMutex.insert({intMem.internal, new std::shared_mutex});
                const auto memid = *((size_t *)&mem);
                memoryCounter.insertOrUpdate(memid, 1, [](size_t el)
                                             { return el + 1; });

                const auto &info = infos[i];
                memoryType.insert({intMem.internal, info.type});
                if (info.initialSize != 0 && info.initialMemory)
                {
                    if (isHostAccessable(info.type))
                    {
                        upload<uint8_t>(device, mem, span<uint8_t>{(uint8_t *)info.initialMemory, info.initialSize}, region.offset);
                    }
                    else
                    {
                        const auto packBuffer = allocatePackedBuffer(info.initialSize, device,
                                                                     memTypeIndices[(uint32_t)MemoryType::Global], vk::BufferUsageFlagBits::eTransferSrc);
                        upload<uint8_t>(device, packBuffer.memory, span<uint8_t>{(uint8_t *)info.initialMemory, info.initialSize});
                        packedBuffer.push_back(packBuffer);
                        vk::BufferCopy copy(0, 0, info.initialSize);
                        cmd.copyBuffer(packBuffer.buffer, buffer, copy);
                    }
                }
            }
            endSubmitAndWait(device, cmdPool, cmd);
            for (auto &b : packedBuffer)
                b.destroy(device);
            return allocated;
        }

        ReadLock readMemory(Memory memory, span<const ReadInfo> reads)
        {
            ReadLock rl;
            rl.lock = std::shared_lock(*memoryMutex[memory.internal]);
            MemoryType type = memoryType[memory.internal];
            const auto infoMem = bufferToMemLookup[memory.internal];
            const auto deviceID = selectDevice();
            const auto device = setup.devices[deviceID];

            if (isHostAccessable(type))
            {
                uint8_t *ptr = (uint8_t *)device.mapMemory(infoMem.memory, infoMem.offset, VK_WHOLE_SIZE);
                rl.ptr.reserve(reads.size());
                rl.releaseFunction = &deleteHostAccessable;
                rl.customData = new CDVulkanHostAccessable{device, infoMem.memory};
                for (const auto &r : reads)
                {
                    rl.ptr.push_back(ptr + r.offset);
                }
            }
            else
            {
                vk::Buffer bufferDst = *((vk::Buffer*)&memory.internal);
                const auto &memTypeIndices = setup.memoryTypeIndices[deviceID];
                size_t amount = std::accumulate(reads.begin(), reads.end(), 0u, [](size_t t, const auto &read)
                                                { return t + read.size; });
                PackedBuffer buffer = allocatePackedBuffer(amount, device, memTypeIndices[(size_t)MemoryType::Global], vk::BufferUsageFlagBits::eTransferDst);
                const auto cmdPool = setup.commandPools[deviceID];

                const auto cmd = beginAndGet(device, cmdPool);
                std::vector<vk::BufferCopy> copies;
                size_t destOffset = 0;
                for(const auto& r : reads) {
                    vk::BufferCopy cCopy(r.offset, destOffset, r.size);
                    destOffset += r.size;
                    copies.push_back(cCopy);
                }
                cmd.copyBuffer(bufferDst, buffer.buffer, copies);
                endSubmitAndWait(device, cmdPool, cmd);
                
                uint8_t *ptr = (uint8_t *)device.mapMemory(buffer.memory, 0, VK_WHOLE_SIZE);
                rl.ptr.reserve(reads.size());
                rl.releaseFunction = &deleteDeviceLocal;
                rl.customData = new CDVulkanDeviceLocal{device, buffer};
                destOffset = 0;
                for (const auto &r : reads)
                {
                    rl.ptr.push_back(ptr + destOffset);
                    destOffset += r.size;
                }
            }
            return rl;
        }

        inline void writeMemory(Memory memory, span<const BufferUpdateInfo> updates)
        {
            if (updates.empty() || !memory) {
                LOG(V1_WARN, "writeMemory called with empty updates or invalid memory!\n");
                return;
            }
            std::vector<PackedBuffer> packedBuffer;
            const auto deviceID = selectDevice();
            const auto device = setup.devices[deviceID];
            const auto cmdPool = setup.commandPools[deviceID];
            const auto &memTypeIndices = setup.memoryTypeIndices[deviceID];

            const auto infoMem = bufferToMemLookup[memory.internal];
            const auto mem = infoMem.memory;
            const auto LG = memoryMutex[memory.internal];
            std::lock_guard lock(*LG);

            const auto memType = memoryType[memory.internal];
            const auto cmd = beginAndGet(device, cmdPool);
            // TODO: Seperate loops!
            for (size_t i = 0; i < updates.size(); i++)
            {
                const auto &region = updates[i];
                if (region.size != 0)
                {
                    if (isHostAccessable(memType))
                    {
                        upload<uint8_t>(device, mem, mgi::span<uint8_t>{(uint8_t*)region.data, region.size}, 
                            region.destinationOffset + infoMem.offset);
                    }
                    else
                    {
                        const vk::Buffer buffer = *((VkBuffer *)&memory.internal);
                        const auto packBuffer = allocatePackedBuffer(region.size, device,
                                                                     memTypeIndices[(uint32_t)MemoryType::Global], vk::BufferUsageFlagBits::eTransferSrc);
                        upload<uint8_t>(device, packBuffer.memory, span<uint8_t>{(uint8_t *)region.data, region.size});
                        packedBuffer.push_back(packBuffer);
                        vk::BufferCopy copy(0, region.destinationOffset, region.size);
                        cmd.copyBuffer(packBuffer.buffer, buffer, copy);
                    }
                }
            }
            endSubmitAndWait(device, cmdPool, cmd);
            for (auto &b : packedBuffer)
                b.destroy(device);
        }

        inline std::array<uint32_t, 3> getWorkgroupAmount(const TaskInfo& info, Kernel kernel)
        {
            float DIVIDER[3] = {1, 1, 1};
            size_t counter = 0;
            for(auto x : info.groupSizes) {
                DIVIDER[counter] = x; 
                if((x != 32 && x != 1)) {
                    LOG(V1_WARN, "Vulkan workgroup sizes must be 1 or 32, will be promoted to 32!\n");
                    DIVIDER[counter] = 32;
                }
                counter++;
            }
            return {(uint32_t)ceil(info.range[0] / DIVIDER[0]), (uint32_t)ceil(info.range[1] / DIVIDER[1]), 
                (uint32_t)ceil(info.range[2] / DIVIDER[2])};
        }

        std::vector<Task> queueTasks(span<const TaskInfo> tasks, const TaskStrategy &strategy = {})
        {
            const auto deviceID = selectDevice();
            const auto device = setup.devices[deviceID];
            const auto cmdPool = setup.commandPools[deviceID];
            
            auto cmd = beginAndGet(device, cmdPool);
            std::vector<Task> queuedTasks;
            queuedTasks.reserve(tasks.size());
            for(auto& task : tasks) {
                const auto event = device.createEvent({});
                const auto &pipeInfo = pipelineInfo[task.kernel.internal][task.function];
                const auto descriptorSet = device.allocateDescriptorSets({pipeInfo.pool, pipeInfo.setLayout}).back();

                // TODO Unroll and enable prebacked!
                size_t counter = 0;
                for(const auto descriptor : task.descriptor.memory) {
                    vk::DescriptorBufferInfo bufferInfo{(VkBuffer)descriptor.internal, 0, VK_WHOLE_SIZE};
                    vk::WriteDescriptorSet write(descriptorSet, pipeInfo.bindings[counter], 0, vk::DescriptorType::eStorageBuffer, {}, bufferInfo);
                    device.updateDescriptorSets(write, {});
                    counter++;
                }

                cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeInfo.pipeline);
                if(!pipeInfo.pushConstants.empty()) {
                    uint32_t sizes[3] = {0, 0, 0}; // Todo recheck offsetting
                    cmd.pushConstants(pipeInfo.pipeLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(sizes), &sizes);
                }
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeInfo.pipeLayout, 0, descriptorSet, {});
                const auto workgroupAmount = getWorkgroupAmount(task, task.kernel);
                cmd.dispatch(workgroupAmount[0], workgroupAmount[1], workgroupAmount[2]);
                // TODO use set 2
                cmd.setEvent(event, vk::PipelineStageFlagBits::eComputeShader);
                queuedTasks.push_back({(size_t)(VkEvent)event});
            }
            cmd.end();
            const auto queue = selectQueue();
            vk::SubmitInfo submitInfo = {{}, {}, cmd};
            queue.submit(submitInfo, nullptr);
            return queuedTasks;
        }

        void waitTasks(span<const Task> tasks)
        {
            const auto deviceID = selectDevice();
            const auto device = setup.devices[deviceID];

            for(const auto event : mgi::span((vk::Event*)tasks.data(), tasks.size())) {
                while(device.getEventStatus(event) != vk::Result::eEventSet) {
                    std::this_thread::yield();
                }
            }
        }

        std::vector<TaskStatus> queueWaitTasks(span<const TaskInfo> taskInfos, const TaskStrategy &strategy = {})
        {
            waitTasks(queueTasks(taskInfos, strategy));
            return std::vector<TaskStatus>(taskInfos.size(), TaskStatus::Complete);
        }

        std::vector<TaskStatus> getStatus(span<const Task> tasks)
        {
            std::vector<TaskStatus> statuses;
            statuses.reserve(tasks.size());

            const auto deviceID = selectDevice();
            const auto device = setup.devices[deviceID];
            for(const auto event : mgi::span((vk::Event*)tasks.data(), tasks.size())) {
                const auto status = device.getEventStatus(event) != vk::Result::eEventSet ? TaskStatus::Running : TaskStatus::Complete;
                statuses.push_back(status);
            }
            return statuses;
        }

        Task copyMemory(Memory source, Memory destination, span<const MemoryCopyInfo> copyInfos)
        {
            return {};
        }

        void copyMemoryWait(Memory source, Memory destination, span<const MemoryCopyInfo> copyInfos)
        {
            const auto task = copyMemory(source, destination, copyInfos);
            waitTasks({&task, 1});
        }

        inline void freeObj(Memory memory)
        {
            const auto deviceID = selectDevice();
            const auto device = setup.devices[deviceID];
            device.destroyBuffer((VkBuffer)memory.internal);
            memoryCounter.insertOrUpdate(memory.internal, 0, [](size_t el)
                                             { return el - 1; });
            if(memoryCounter[memory.internal] == 0) {
                const auto infoMem = bufferToMemLookup[memory.internal];
                device.freeMemory(infoMem.memory);
                memoryMutex.insert({memory.internal, nullptr});
            }
            bufferToMemLookup.insert({memory.internal, VulkanMemoryRegion{nullptr, 0}});
        }

        inline void freeObj(Task task)
        {
            const auto deviceID = selectDevice();
            const auto device = setup.devices[deviceID];
            device.destroyEvent((VkEvent)task.internal);
        }
    };

    inline VulkanSetup initMGI(const InitInfo &info = {})
    {
        VulkanSetup setup;

        std::vector<const char *> extensions;
        std::array<const char *, 1> layersRequested = {"VK_LAYER_KHRONOS_validation"};
        std::vector<const char *> layers;
        const auto layerProperties = vk::enumerateInstanceLayerProperties();
        for (const auto layer : layersRequested)
        {
            const auto found = std::find_if(layerProperties.begin(), layerProperties.end(), [=](const vk::LayerProperties &lp)
                                            {
                LOG(V3_VERB, "Found Vulkan layer: %s\n", lp.layerName.data());
                return strcmp(lp.layerName.data(), layer) == 0; });
            if (found == layerProperties.end())
            {
                LOG(V1_WARN, "Vulkan layer %s not found, skipping!\n", layer);
                continue;
            }
            layers.push_back(layer);
        }

        static vk::ApplicationInfo appInfo("Mallob", VK_MAKE_VERSION(1, 0, 0), "Mallob", VK_MAKE_VERSION(1, 0, 0), VK_API_VERSION_1_3);
        vk::InstanceCreateInfo instanceCreateInfo({}, &appInfo, layers, extensions);
        setup.instance = vk::createInstance(instanceCreateInfo);

        std::vector<const char *> deviceExtensions;
        const auto physicalDevices = setup.instance.enumeratePhysicalDevices();

        vk::DeviceQueueCreateInfo queueCreateInfo({}, 0, info.queuePriorities);
        static vk::StructureChain<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features, vk::PhysicalDeviceVulkan13Features, vk::PhysicalDeviceVulkan12Features> features;
        auto &vulkan11FeaturesSet = features.get<vk::PhysicalDeviceVulkan11Features>();
        vulkan11FeaturesSet.variablePointersStorageBuffer = true;
        vulkan11FeaturesSet.variablePointers = true;
        auto &vulkan13FeaturesSet = features.get<vk::PhysicalDeviceVulkan13Features>();
        vulkan13FeaturesSet.synchronization2 = true;
        auto &vulkan12FeaturesSet = features.get<vk::PhysicalDeviceVulkan12Features>();
        vulkan12FeaturesSet.bufferDeviceAddress = true;
        vulkan12FeaturesSet.storageBuffer8BitAccess = true;
        vulkan12FeaturesSet.shaderInt8 = true;
        auto &physicalDeviceFeaturesSet = features.get<vk::PhysicalDeviceFeatures2>();
        physicalDeviceFeaturesSet.features.shaderInt64 = true;
        vk::DeviceCreateInfo deviceCreateInfo({}, queueCreateInfo, {}, deviceExtensions);
        deviceCreateInfo.pNext = &physicalDeviceFeaturesSet;

        for (const auto &phyDevice : physicalDevices)
        {
            const auto queueFamilies = phyDevice.getQueueFamilyProperties();
            const auto computeQueueFamily = std::find_if(queueFamilies.begin(), queueFamilies.end(), [](const vk::QueueFamilyProperties &qf)
                                                         { return qf.queueFlags & vk::QueueFlagBits::eCompute; });
            if (computeQueueFamily == queueFamilies.end())
            {
                LOG(V1_WARN, "No compute queue family found on device, skipping!\n");
                continue;
            }
            const auto deviceProperties = phyDevice.getProperties();
            if (deviceProperties.apiVersion < VK_API_VERSION_1_3)
            {
                LOG(V1_WARN, "Vulkan device %s does not support Vulkan 1.3, skipping!\n", deviceProperties.deviceName.data());
                continue;
            }
            vk::StructureChain<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features, vk::PhysicalDeviceVulkan13Features, vk::PhysicalDeviceVulkan12Features> featurePresent;
            phyDevice.getFeatures2(&featurePresent.get<vk::PhysicalDeviceFeatures2>());
            const auto &vulkan11Features = featurePresent.get<vk::PhysicalDeviceVulkan11Features>();
            if (!vulkan11Features.variablePointersStorageBuffer || !vulkan11Features.variablePointers)
            {
                LOG(V1_WARN, "Vulkan device %s does not support variable pointers, skipping!\n", deviceProperties.deviceName.data());
                continue;
            }
            const auto &physicalDeviceFeatures = featurePresent.get<vk::PhysicalDeviceFeatures2>();
            if (!physicalDeviceFeatures.features.shaderInt64)
            {
                LOG(V1_WARN, "Vulkan device %s does not support shaderInt64, skipping!\n", deviceProperties.deviceName.data());
                continue;
            }
            const auto &vulkan13Features = featurePresent.get<vk::PhysicalDeviceVulkan13Features>();
            if (!vulkan13Features.synchronization2)
            {
                LOG(V1_WARN, "Vulkan device %s does not support synchronization2, skipping!\n", deviceProperties.deviceName.data());
                continue;
            }
            const auto &vulkan12Features = featurePresent.get<vk::PhysicalDeviceVulkan12Features>();
            if (!vulkan12Features.bufferDeviceAddress)
            {
                LOG(V1_WARN, "Vulkan device %s does not support bufferDeviceAddress, skipping!\n", deviceProperties.deviceName.data());
                continue;
            }
            LOG(V2_INFO, "Found Vulkan device: %s\n", deviceProperties.deviceName.data());

            const auto computeQueueFamilyIndex = std::distance(queueFamilies.begin(), computeQueueFamily);
            queueCreateInfo.queueFamilyIndex = computeQueueFamilyIndex;
            const auto device = phyDevice.createDevice(deviceCreateInfo);
            setup.devices.push_back(device);
            setup.queueFamilies.push_back(computeQueueFamilyIndex);
            setup.physicalDevices.push_back(phyDevice);
            for (size_t i = 0; i < queueCreateInfo.queueCount; i++)
            {
                setup.queues.push_back(device.getQueue(computeQueueFamilyIndex, i));
            }
            vk::CommandPoolCreateInfo poolCreateInfo(vk::CommandPoolCreateFlagBits::eResetCommandBuffer, computeQueueFamilyIndex);
            setup.commandPools.push_back(device.createCommandPool(poolCreateInfo));
            MemTypeTable memTypeTable;
            const auto memProperties = phyDevice.getMemoryProperties();
            bool foundDeviceLocal = false, foundHostVisible = false;
            for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
            {
                const auto memType = memProperties.memoryTypes[i];
                const auto flags = memType.propertyFlags;
                if (!foundDeviceLocal && flags & vk::MemoryPropertyFlagBits::eDeviceLocal)
                {
                    memTypeTable[(uint32_t)MemoryType::DeviceLocal] = i;
                    memTypeTable[(uint32_t)MemoryType::Constant] = i;
                    foundDeviceLocal = true;
                }
                if (!foundHostVisible && flags & vk::MemoryPropertyFlagBits::eHostVisible)
                {
                    memTypeTable[(uint32_t)MemoryType::Uniform] = i;
                    memTypeTable[(uint32_t)MemoryType::Global] = i;
                    foundHostVisible = true;
                }
            }
            setup.memoryTypeIndices.push_back(memTypeTable);
        }
        setup.queuesProtects.resize(setup.queues.size());
        for (auto &m : setup.queuesProtects)
            m = new std::mutex;
        if (setup.devices.empty())
        {
            LOG(V0_CRIT, "No suitable Vulkan devices found!\n");
            return {};
        }
        return setup;
    }
}
