/*
 * First Vulkan on the DM3Q Adreno 740: loads Mesa Turnip (libvulkan_freedreno.dylib,
 * KGSL backend on xnu's /dev/kgsl-3d0) directly through its ICD entry point, creates an
 * instance and a device, and runs vkCmdFillBuffer + vkCmdUpdateBuffer on a host-visible
 * buffer, checked from the CPU.
 *
 * Build: xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2
 *          -I ~/hios/third_party/mesa/include -o vk_test vk_test.c && codesign -s - vk_test
 * Run:   tools/sweetrun.py -f tools/kgsl-test/vk_test -f .../libvulkan_freedreno.dylib vk_test
 *        (the dylib lands next to it in /usr/local/sweettest/)
 */
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>
#include <sys/ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

static PFN_vkGetInstanceProcAddr gipa;
static int fails;

#define CHECK(cond, ...) do { \
		if (cond) { printf("ok   "); } else { printf("FAIL "); fails++; } \
		printf(__VA_ARGS__); printf("\n"); fflush(stdout); \
	} while (0)
#define VKCHECK(r, what) CHECK((r) == VK_SUCCESS, "%s (VkResult %d)", what, (int)(r))

/* which function trapped: pc, lr and a few frames, symbolized with dladdr */
static void
crash(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = ctx;
	uint64_t pc = uc->uc_mcontext->__ss.__pc, lr = uc->uc_mcontext->__ss.__lr, fp = uc->uc_mcontext->__ss.__fp;
	uint64_t addrs[12] = { pc, lr };
	int n = 2;
	for (; n < 12 && fp; n++) {
		uint64_t *f = (uint64_t *)fp;
		addrs[n] = f[1];
		fp = f[0];
	}
	printf("CRASH signal %d addr %p\n", sig, si->si_addr);
	for (int i = 0; i < n; i++) {
		Dl_info d;
		if (dladdr((void *)addrs[i], &d) && d.dli_sname) {
			printf("  #%d %s+0x%llx (%s +0x%llx)\n", i, d.dli_sname, (unsigned long long)(addrs[i] - (uint64_t)d.dli_saddr),
			    strrchr(d.dli_fname, '/') ? strrchr(d.dli_fname, '/') + 1 : d.dli_fname,
			    (unsigned long long)(addrs[i] - (uint64_t)d.dli_fbase));
		} else {
			printf("  #%d %p\n", i, (void *)addrs[i]);
		}
	}
	fflush(stdout);
	_exit(128 + sig);
}

int
main(int argc, char **argv)
{
	struct sigaction sa = { .sa_sigaction = crash, .sa_flags = SA_SIGINFO };
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGTRAP, &sa, NULL);
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	sigaction(SIGALRM, &sa, NULL);  /* hang watchdog: where the main thread is after 25 s */
	alarm(25);
	setenv("TU_DEBUG", "startup", 1);
	setenv("MESA_DEBUG", "1", 1);
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	const char *path = argc > 1 ? argv[1] : "/usr/local/sweettest/libvulkan_freedreno.dylib";
	void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	CHECK(lib != NULL, "dlopen %s %s", path, lib ? "" : dlerror());
	if (!lib) {
		return 1;
	}
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
	CHECK(gipa != NULL, "vk_icdGetInstanceProcAddr");
	if (!gipa) {
		return 1;
	}
#define I(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
	VkInstance inst = VK_NULL_HANDLE;
	I(vkCreateInstance);
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk_test",
		                  .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkResult r = vkCreateInstance(&ici, NULL, &inst);
	VKCHECK(r, "vkCreateInstance");
	if (r) {
		return 1;
	}
	I(vkEnumeratePhysicalDevices);
	I(vkGetPhysicalDeviceProperties);
	I(vkGetPhysicalDeviceMemoryProperties);
	I(vkCreateDevice);
	I(vkGetDeviceProcAddr);
	uint32_t n = 0;
	r = vkEnumeratePhysicalDevices(inst, &n, NULL);
	CHECK(r == VK_SUCCESS && n > 0, "vkEnumeratePhysicalDevices: %u device(s) (VkResult %d)", n, (int)r);
	if (!n) {
		return 1;
	}
	VkPhysicalDevice pd;
	n = 1;
	vkEnumeratePhysicalDevices(inst, &n, &pd);
	VkPhysicalDeviceProperties pp;
	vkGetPhysicalDeviceProperties(pd, &pp);
	printf("     device: %s, Vulkan %u.%u.%u, driver %08x, vendor %04x id %08x\n", pp.deviceName,
	    VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion), VK_API_VERSION_PATCH(pp.apiVersion),
	    pp.driverVersion, pp.vendorID, pp.deviceID);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryHeapCount; i++) {
		printf("     heap %u: %llu MiB flags %x\n", i, (unsigned long long)(mp.memoryHeaps[i].size >> 20),
		    mp.memoryHeaps[i].flags);
	}

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
		                        .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
		                   .pQueueCreateInfos = &qci };
	VkDevice dev;
	r = vkCreateDevice(pd, &dci, NULL, &dev);
	VKCHECK(r, "vkCreateDevice");
	if (r) {
		return 1;
	}
#define D(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
	D(vkGetDeviceQueue);
	D(vkCreateBuffer);
	D(vkGetBufferMemoryRequirements);
	D(vkAllocateMemory);
	D(vkBindBufferMemory);
	D(vkMapMemory);
	D(vkCreateCommandPool);
	D(vkAllocateCommandBuffers);
	D(vkBeginCommandBuffer);
	D(vkCmdFillBuffer);
	D(vkCmdUpdateBuffer);
	D(vkCmdPipelineBarrier);
	D(vkEndCommandBuffer);
	D(vkCreateFence);
	D(vkQueueSubmit);
	D(vkWaitForFences);
	D(vkDeviceWaitIdle);
	D(vkDestroyDevice);
	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);

	const VkDeviceSize size = 1 << 20;
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
		                   .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkBuffer buf;
	VKCHECK(vkCreateBuffer(dev, &bci, NULL, &buf), "vkCreateBuffer 1 MiB");
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	uint32_t type = ~0u;
	for (uint32_t i = 0; i < mp.memoryTypeCount && type == ~0u; i++) {
		if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags &
		    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
		    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
			type = i;
		}
	}
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		                     .memoryTypeIndex = type };
	VkDeviceMemory mem;
	VKCHECK(vkAllocateMemory(dev, &mai, NULL, &mem), "vkAllocateMemory (host visible + coherent)");
	VKCHECK(vkBindBufferMemory(dev, buf, mem, 0), "vkBindBufferMemory");
	volatile uint32_t *p = NULL;
	VKCHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&p), "vkMapMemory");
	if (!p) {
		return 1;
	}
	memset((void *)p, 0x11, size);

	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
	VkCommandPool pool;
	VKCHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool), "vkCreateCommandPool");
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
		                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cb;
	VKCHECK(vkAllocateCommandBuffers(dev, &cbai, &cb), "vkAllocateCommandBuffers");
	VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	vkBeginCommandBuffer(cb, &cbbi);
	vkCmdFillBuffer(cb, buf, 0, size, 0xa740a740u);
	VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		               .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
	static const uint32_t hello[4] = { 0x6c6c6548, 0x7266206f, 0x41206d6f, 0x30343720 };   /* "Hello from A740" */
	vkCmdUpdateBuffer(cb, buf, 64, sizeof(hello), hello);
	mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
	VKCHECK(vkEndCommandBuffer(cb), "vkEndCommandBuffer");

	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	VKCHECK(vkCreateFence(dev, &fci, NULL, &fence), "vkCreateFence");
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	VKCHECK(vkQueueSubmit(q, 1, &si, fence), "vkQueueSubmit");
	r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	VKCHECK(r, "vkWaitForFences");
	printf("     submit -> fence: %.2f ms\n", (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
	uint32_t bad = 0;
	for (uint32_t i = 0; i < size / 4; i++) {
		if ((i < 16 || i >= 20) && p[i] != 0xa740a740u) {
			bad++;
		}
	}
	CHECK(bad == 0, "vkCmdFillBuffer: 1 MiB filled with a740a740 by the GPU (%u wrong words, p[0] %08x)", bad, p[0]);
	char text[17] = { 0 };
	memcpy(text, (const void *)(p + 16), 16);
	CHECK(!memcmp(text, hello, 16), "vkCmdUpdateBuffer: \"%s\"", text);
	vkDeviceWaitIdle(dev);
	vkDestroyDevice(dev, NULL);
	printf("%s: %d failure(s)\n", fails ? "VULKAN TEST FAILED" : "VULKAN TEST PASSED", fails);
	return fails != 0;
}
