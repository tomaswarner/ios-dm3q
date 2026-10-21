/*
 * Minimal libvulkan.dylib for the S23 iOS port (no Khronos loader on iOS):
 * ash's Entry::load() dlopens "libvulkan.dylib" and resolves
 * vkGetInstanceProcAddr; Mesa's lavapipe only exports the ICD entry points.
 * Load lavapipe, negotiate the loader-ICD interface, and forward. Every other
 * command (global, instance, device) is reached through vkGetInstanceProcAddr.
 * Installed as /usr/lib/libvulkan.dylib (dyld's fallback path).
 */
#include <dlfcn.h>
#include <stddef.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define LVP "/usr/local/sweettest/libvulkan_lvp.dylib"

typedef VkResult (VKAPI_PTR *PFN_negotiate)(uint32_t *);
static PFN_vkGetInstanceProcAddr icd_gipa;

__attribute__((constructor)) static void load_icd(void)
{
	void *h = dlopen(LVP, RTLD_NOW | RTLD_LOCAL);
	if (!h)
		return;
	PFN_negotiate nego = (PFN_negotiate)dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
	icd_gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
	if (nego) {
		uint32_t v = 5;
		nego(&v);
	}
}

__attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
	return icd_gipa ? icd_gipa(instance, name) : NULL;
}
