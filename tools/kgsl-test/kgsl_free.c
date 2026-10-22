/* kgsl_free: how much of the GPU pool is free right now (allocates 4 MiB buffers until ENOMEM,
 * then closes: the kernel frees them). */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <string.h>
#include <unistd.h>
struct kgsl_gpumem_alloc_id { uint32_t id, flags; uint64_t size, mmapsize, gpuaddr, pad[2]; };
#define ALLOC_ID _IOWR(0x09, 0x34, struct kgsl_gpumem_alloc_id)
int main(void)
{
	int fd = open("/dev/kgsl-3d0", O_RDWR);
	if (fd < 0) { perror("open"); return 1; }
	unsigned n = 0;
	for (;;) {
		struct kgsl_gpumem_alloc_id a = { .size = 4u << 20 };
		if (ioctl(fd, ALLOC_ID, &a)) { printf("stop after %u x 4 MiB: %s\n", n, strerror(errno)); break; }
		n++;
	}
	printf("GPU POOL FREE: %u MiB (in 4 MiB pieces)\n", n * 4);
	close(fd);
	return 0;
}
