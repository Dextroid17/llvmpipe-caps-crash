// Standalone reproducer for the llvmpipe crash in agc_driver_bda_device_tests.
// Loads the SPIR-V that AnyPS5 emits for the test's third case and dispatches it
// with the same three storage buffers.  Run with:
//   GALLIUM_OVERRIDE_CPU_CAPS=avx  ./bda-repro bda_run2.spv   # segfaults
//   GALLIUM_OVERRIDE_CPU_CAPS=avx2 ./bda-repro bda_run2.spv   # passes
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <fstream>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::fprintf(stderr, "%s -> VkResult %d\n", #x, (int)r_); return 1; } } while (0)
#define REQUIRE(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::fprintf(stderr, "%s -> VkResult %d\n", #x, (int)r_); std::exit(1); } } while (0)

struct Header { std::uint32_t version, count, entryBytes, reserved; };
struct Range  { std::uint64_t begin, end, deviceAddress; std::uint32_t permissions, reserved; };
struct Fault  { std::uint32_t state, reason; std::uint64_t address; std::uint32_t bytes, stage, instruction, reserved; };
static_assert(sizeof(Header) == 16);
static_assert(sizeof(Range) == 32);
static_assert(sizeof(Fault) == 32);

static std::vector<std::uint32_t> ReadFile(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); std::exit(2); }
    const auto n = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<std::uint32_t> v(n / 4);
    f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(n));
    return v;
}

struct Buf { VkBuffer buffer; VkDeviceMemory memory; void* map; VkDeviceSize size; };

int main(int argc, char** argv) {
    const char* spvPath = argc > 1 ? argv[1] : "bda_run2.spv";
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto code = ReadFile(spvPath);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance;
    CHECK(vkCreateInstance(&ici, nullptr, &instance));

    std::uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    if (count == 0) { std::fprintf(stderr, "no Vulkan device\n"); return 1; }
    std::vector<VkPhysicalDevice> devices(count);
    CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    VkPhysicalDevice physical = devices.front();
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);
    std::printf("device: %s  api %u.%u.%u\n", props.deviceName,
                VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

    VkPhysicalDeviceBufferDeviceAddressFeatures address{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkPhysicalDevice8BitStorageFeatures bytes{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES};
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    address.pNext = &bytes;
    query.pNext = &address;
    vkGetPhysicalDeviceFeatures2(physical, &query);
    if (address.bufferDeviceAddress != VK_TRUE || bytes.storageBuffer8BitAccess != VK_TRUE || query.features.shaderInt64 != VK_TRUE) {
        std::fprintf(stderr, "required features unavailable (bda=%d bytes=%d int64=%d)\n", address.bufferDeviceAddress, bytes.storageBuffer8BitAccess, query.features.shaderInt64);
        return 1;
    }
    address = VkPhysicalDeviceBufferDeviceAddressFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, nullptr, VK_TRUE, VK_FALSE, VK_FALSE};
    bytes = VkPhysicalDevice8BitStorageFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, nullptr, VK_TRUE, VK_FALSE, VK_FALSE};
    address.pNext = &bytes;

    std::uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
    std::uint32_t family = 0;
    while (family < familyCount && (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) ++family;
    if (family == familyCount) { std::fprintf(stderr, "no compute queue\n"); return 1; }

    const float priority = 1;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures enabled{};
    enabled.shaderInt64 = VK_TRUE;
    const char* extensions[] = {VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, VK_KHR_8BIT_STORAGE_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &address};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 2;
    deviceInfo.ppEnabledExtensionNames = extensions;
    deviceInfo.pEnabledFeatures = &enabled;
    VkDevice device;
    CHECK(vkCreateDevice(physical, &deviceInfo, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);

    auto getBufferDeviceAddress = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(vkGetDeviceProcAddr(device, "vkGetBufferDeviceAddress"));
    if (getBufferDeviceAddress == nullptr) getBufferDeviceAddress = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(vkGetDeviceProcAddr(device, "vkGetBufferDeviceAddressKHR"));
    if (getBufferDeviceAddress == nullptr) { std::fprintf(stderr, "vkGetBufferDeviceAddress is unavailable\n"); return 1; }

    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    std::uint32_t hostVisible = 0;
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) { hostVisible = i; break; }
    }

    const auto make = [&](VkDeviceSize size, bool addressable) -> Buf {
        Buf r{};
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = size;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | (addressable ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
        REQUIRE(vkCreateBuffer(device, &bufferInfo, nullptr, &r.buffer));
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, r.buffer, &requirements);
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = hostVisible;
        if (addressable) { flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT; allocate.pNext = &flags; }
        REQUIRE(vkAllocateMemory(device, &allocate, nullptr, &r.memory));
        REQUIRE(vkBindBufferMemory(device, r.buffer, r.memory, 0));
        REQUIRE(vkMapMemory(device, r.memory, 0, size, 0, &r.map));
        r.size = size;
        return r;
    };
    const auto addressOf = [&](VkBuffer buffer) {
        VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        info.buffer = buffer;
        return getBufferDeviceAddress(device, &info);
    };

    Buf first = make(3, true);
    Buf second = make(2, true);
    Buf table = make(sizeof(Header) + 2 * sizeof(Range), false);
    Buf fault = make(sizeof(Fault), false);
    Buf output = make(4, false);

    const std::uint64_t guest = 0x7fff12340001ULL;
    static_cast<std::uint8_t*>(first.map)[0] = 0x11;
    static_cast<std::uint8_t*>(first.map)[1] = 0x22;
    static_cast<std::uint8_t*>(first.map)[2] = 0x33;
    static_cast<std::uint8_t*>(second.map)[0] = 0x44;
    static_cast<std::uint8_t*>(second.map)[1] = 0x55;
    const Header header{1, 2, sizeof(Range), 0};
    const Range ranges[2] = {{guest, guest + 3, addressOf(first.buffer), 1, 0}, {guest + 3, guest + 5, addressOf(second.buffer), 1, 0}};
    std::memcpy(table.map, &header, sizeof(header));
    std::memcpy(static_cast<std::uint8_t*>(table.map) + sizeof(header), ranges, sizeof(ranges));
    std::memset(fault.map, 0, sizeof(Fault));
    const std::uint32_t sentinel = 0xdeadbeef;
    std::memcpy(output.map, &sentinel, sizeof(sentinel));

    VkDescriptorSetLayoutBinding bindings[3]{};
    for (std::uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setLayoutInfo.bindingCount = 3;
    setLayoutInfo.pBindings = bindings;
    VkDescriptorSetLayout setLayout;
    CHECK(vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &setLayout));
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool));
    VkDescriptorSetAllocateInfo allocateSet{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateSet.descriptorPool = pool;
    allocateSet.descriptorSetCount = 1;
    allocateSet.pSetLayouts = &setLayout;
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(device, &allocateSet, &set));
    const VkBuffer buffers[3] = {table.buffer, fault.buffer, output.buffer};
    const VkDeviceSize sizes[3] = {sizeof(Header) + 2 * sizeof(Range), sizeof(Fault), 4};
    VkDescriptorBufferInfo infos[3];
    VkWriteDescriptorSet writes[3]{};
    for (std::uint32_t i = 0; i < 3; ++i) {
        infos[i] = {buffers[i], 0, sizes[i]};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout));
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = code.size() * sizeof(std::uint32_t);
    moduleInfo.pCode = code.data();
    VkShaderModule module;
    CHECK(vkCreateShaderModule(device, &moduleInfo, nullptr, &module));
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    pipelineInfo.layout = layout;
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));

    VkCommandPoolCreateInfo commandPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commandPoolInfo.queueFamilyIndex = family;
    VkCommandPool commandPool;
    CHECK(vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commandPool));
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = commandPool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(device, &commandInfo, &command));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(command, &begin));
    VkMemoryBarrier upload{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &upload, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(command, 1, 1, 1);
    VkMemoryBarrier download{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &download, 0, nullptr, 0, nullptr);
    CHECK(vkEndCommandBuffer(command));

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));

    Fault report{};
    std::memcpy(&report, fault.map, sizeof(report));
    std::uint32_t result = 0;
    std::memcpy(&result, output.map, sizeof(result));
    std::printf("done: fault.state=%u reason=%u address=%#llx  result=%#x (expected 0x55443322)\n",
                static_cast<unsigned>(report.state), static_cast<unsigned>(report.reason),
                static_cast<unsigned long long>(report.address), result);
    return result == 0x55443322u ? 0 : 3;
}
