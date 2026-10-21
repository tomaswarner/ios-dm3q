/*
 * vk_test: Mesa lavapipe (CPU Vulkan, cross-built for iOS 16 arm64) on the S23
 * iOS port, without a Vulkan loader: dlopen the ICD, negotiate the interface,
 * then instance -> llvmpipe physical device -> device -> a compute shader
 * (fill.comp, compiled to SPIR-V on the Mac) that writes v[i] = 2i + 1 into a
 * host-visible buffer. The dispatch makes llvmpipe compile through LLVM's JIT.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "fill_spv.h"

/* vk_icd.h's loader-ICD negotiation entry point */
typedef VkResult (VKAPI_PTR *PFN_vkNegotiateLoaderICDInterfaceVersion)(uint32_t *pVersion);

#define LVP "/usr/local/sweettest/libvulkan_lvp.dylib"
#define N 256

static PFN_vkGetInstanceProcAddr gipa;
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("vk_test: %s = %d\n", #x, r_); return 1; } } while (0)

int main(void)
{
	void *h = dlopen(LVP, RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		printf("vk_test: dlopen: %s\n", dlerror());
		return 1;
	}
	PFN_vkNegotiateLoaderICDInterfaceVersion nego = (PFN_vkNegotiateLoaderICDInterfaceVersion)dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
	if (!nego || !gipa) {
		printf("vk_test: ICD entry points missing\n");
		return 1;
	}
	uint32_t ver = 5;
	nego(&ver);
	printf("vk_test: lavapipe loaded, ICD interface %u\n", ver);

	PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance inst;
	CHECK(createInstance(&ici, NULL, &inst));
#define IFN(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
	IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties); IFN(vkCreateDevice);
	IFN(vkGetPhysicalDeviceMemoryProperties); IFN(vkGetDeviceProcAddr);
	uint32_t count = 1;
	VkPhysicalDevice pd;
	CHECK(vkEnumeratePhysicalDevices(inst, &count, &pd));
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	printf("vk_test: device '%s' api %u.%u\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
	fflush(stdout);

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	VkDevice dev;
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
#define DFN(n) PFN_##n n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
	DFN(vkGetDeviceQueue); DFN(vkCreateBuffer); DFN(vkGetBufferMemoryRequirements); DFN(vkAllocateMemory);
	DFN(vkBindBufferMemory); DFN(vkMapMemory); DFN(vkCreateShaderModule); DFN(vkCreateDescriptorSetLayout);
	DFN(vkCreatePipelineLayout); DFN(vkCreateComputePipelines); DFN(vkCreateDescriptorPool);
	DFN(vkAllocateDescriptorSets); DFN(vkUpdateDescriptorSets); DFN(vkCreateCommandPool);
	DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer); DFN(vkCmdBindPipeline);
	DFN(vkCmdBindDescriptorSets); DFN(vkCmdDispatch); DFN(vkEndCommandBuffer); DFN(vkQueueSubmit);
	DFN(vkQueueWaitIdle);
	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);

	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = N * 4,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
	VkBuffer buf;
	CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	uint32_t type = 0;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
			type = i;
			break;
		}
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = type };
	VkDeviceMemory mem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
	CHECK(vkBindBufferMemory(dev, buf, mem, 0));
	uint32_t *ptr;
	CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&ptr));
	memset(ptr, 0, N * 4);

	VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(fill_spv), .pCode = fill_spv };
	VkShaderModule sm;
	CHECK(vkCreateShaderModule(dev, &smci, NULL, &sm));
	VkDescriptorSetLayoutBinding b = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
	VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b };
	VkDescriptorSetLayout dsl;
	CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
	VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
	VkPipelineLayout pl;
	CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
	VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = sm, .pName = "main" }, .layout = pl };
	VkPipeline pipe;
	CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));
	printf("vk_test: compute pipeline compiled (llvmpipe JIT)\n");
	fflush(stdout);

	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
	VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
	VkDescriptorPool dp;
	CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
	VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
		.descriptorSetCount = 1, .pSetLayouts = &dsl };
	VkDescriptorSet ds;
	CHECK(vkAllocateDescriptorSets(dev, &dsai, &ds));
	VkDescriptorBufferInfo dbi = { buf, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi };
	vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

	VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	VkCommandPool cp;
	CHECK(vkCreateCommandPool(dev, &cpi, NULL, &cp));
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
	VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CHECK(vkBeginCommandBuffer(cb, &cbbi));
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
	vkCmdDispatch(cb, N / 64, 1, 1);
	CHECK(vkEndCommandBuffer(cb));
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
	CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
	CHECK(vkQueueWaitIdle(q));

	int bad = 0;
	for (uint32_t i = 0; i < N; i++)
		bad += ptr[i] != i * 2u + 1u;
	printf("vk_test: compute result v[0..3] = %u %u %u %u, mismatches %d/%d (%s)\n",
	    ptr[0], ptr[1], ptr[2], ptr[3], bad, N, bad ? "FAIL" : "VULKAN OK");
	fflush(stdout);
	return bad ? 2 : 0;
}
