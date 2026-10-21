/*
 * jit_test: can a process on the S23 iOS port make and run code at runtime?
 * lavapipe/llvmpipe need it (Mesa os_probe_jit_allowed does the same mmap).
 * Static analysis says yes with cs_enforcement_disable=1 (sweet's AMFI has no
 * proc_check_map_anon hook, so every process is jit_entitled).
 */
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <TargetConditionals.h>

int main(void)
{
	size_t page = (size_t)getpagesize();
	void *p = mmap(NULL, page, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE | MAP_JIT, -1, 0);
	if (p == MAP_FAILED) {
		perror("jit_test: mmap RWX MAP_JIT");
		return 1;
	}
	printf("jit_test: MAP_JIT page at %p\n", p);
	static const uint32_t code[] = { 0x52800540 /* mov w0, #42 */, 0xd65f03c0 /* ret */ };
#if TARGET_OS_OSX
	pthread_jit_write_protect_np(0);	/* macOS host check only; the iOS port's JIT page is plain RWX */
#endif
	memcpy(p, code, sizeof(code));
#if TARGET_OS_OSX
	pthread_jit_write_protect_np(1);
#endif
	sys_icache_invalidate(p, sizeof(code));
	int (*fn)(void) = (int (*)(void))p;
	int r = fn();
	printf("jit_test: generated code returned %d (%s)\n", r, r == 42 ? "JIT OK" : "WRONG");
	fflush(stdout);
	return r == 42 ? 0 : 2;
}
