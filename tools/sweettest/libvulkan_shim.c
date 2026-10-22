/*
 * Minimal libvulkan.dylib for the S23 iOS port (no Khronos loader on iOS):
 * ash's Entry::load() dlopens "libvulkan.dylib" and resolves
 * vkGetInstanceProcAddr; Mesa's lavapipe only exports the ICD entry points.
 * Load lavapipe, negotiate the loader-ICD interface, and forward. Every other
 * command (global, instance, device) is reached through vkGetInstanceProcAddr.
 * Installed as /usr/lib/libvulkan.dylib (dyld's fallback path).
 *
 * Driver: $SWEET_VK_ICD if set; else Turnip (Adreno 740 over /dev/kgsl-3d0) when
 * /var/tmp/sweet_vk_turnip exists; else lavapipe. Chosen once per process, at load.
 */
#include <dlfcn.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define LVP "/usr/local/sweettest/libvulkan_lvp.dylib"
#define TURNIP "/usr/local/sweettest/libvulkan_freedreno.dylib"
#define TURNIP_FLAG "/var/tmp/sweet_vk_turnip"

typedef VkResult (VKAPI_PTR *PFN_negotiate)(uint32_t *);
static PFN_vkGetInstanceProcAddr icd_gipa;

__attribute__((constructor)) static void load_icd(void)
{
	const char *icd = getenv("SWEET_VK_ICD");
	if (!icd || !*icd)
		icd = access(TURNIP_FLAG, F_OK) == 0 ? TURNIP : LVP;
	void *h = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
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
