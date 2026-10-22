/*
 * /dev/kgsl-3d0 smoke test for the DM3Q xnu (0112 bsd/dev/sweet_kgsl.c), the
 * path Turnip takes: properties, a context, two buffers (an IB and a target)
 * mapped into this process, an IB with CP_MEM_WRITE + CP_WAIT_MEM_WRITES
 * submitted with GPU_COMMAND, wait for its timestamp, check the target from
 * the CPU. Then 1000 submits for a rough rate, and a 64 MiB allocation.
 *
 * Build: xcrun -sdk iphoneos clang -arch arm64 -O2 -o kgsl_test kgsl_test.c
 *        && codesign -s - kgsl_test
 * Run:   tools/sweetrun.py -f tools/kgsl-test/kgsl_test kgsl_test
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

struct kgsl_devinfo {
	uint32_t device_id, chip_id, mmu_enabled;
	uint64_t gmem_gpubaseaddr;
	uint32_t gpu_id;
	uint64_t gmem_sizebytes;
};
struct kgsl_device_getproperty {
	uint32_t type;
	uint64_t value, sizebytes;
};
struct kgsl_device_waittimestamp_ctxtid {
	uint32_t context_id, timestamp, timeout;
};
struct kgsl_drawctxt_create {
	uint32_t flags, drawctxt_id;
};
struct kgsl_drawctxt_destroy {
	uint32_t drawctxt_id;
};
struct kgsl_cmdstream_readtimestamp_ctxtid {
	uint32_t context_id, type, timestamp;
};
struct kgsl_gpumem_alloc_id {
	uint32_t id, flags;
	uint64_t size, mmapsize, gpuaddr, pad[2];
};
struct kgsl_gpumem_free_id {
	uint32_t id, pad;
};
struct kgsl_command_object {
	uint64_t offset, gpuaddr, size;
	uint32_t flags, id;
};
struct kgsl_gpu_command {
	uint64_t flags, cmdlist;
	uint32_t cmdsize, numcmds;
	uint64_t objlist;
	uint32_t objsize, numobjs;
	uint64_t synclist;
	uint32_t syncsize, numsyncs, context_id, timestamp;
};
struct kgsl_ios_map {
	uint32_t id, flags;
	uint64_t addr, size;
};

#define K 0x09
#define GETPROPERTY     _IOWR(K, 0x02, struct kgsl_device_getproperty)
#define WAITTS          _IOW(K, 0x07, struct kgsl_device_waittimestamp_ctxtid)
#define CTX_CREATE      _IOWR(K, 0x13, struct kgsl_drawctxt_create)
#define CTX_DESTROY     _IOW(K, 0x14, struct kgsl_drawctxt_destroy)
#define READTS          _IOWR(K, 0x16, struct kgsl_cmdstream_readtimestamp_ctxtid)
#define ALLOC_ID        _IOWR(K, 0x34, struct kgsl_gpumem_alloc_id)
#define FREE_ID         _IOWR(K, 0x35, struct kgsl_gpumem_free_id)
#define GPU_COMMAND     _IOWR(K, 0x4a, struct kgsl_gpu_command)
#define IOS_MAP         _IOWR(K, 0xe0, struct kgsl_ios_map)

static int fails;

#define CHECK(cond, ...) do { \
		if (cond) { printf("ok   "); } else { printf("FAIL "); fails++; } \
		printf(__VA_ARGS__); printf("\n"); fflush(stdout); \
	} while (0)

static uint32_t
parity(uint32_t v)
{
	v ^= v >> 16;
	v ^= v >> 8;
	v ^= v >> 4;
	return (0x9669u >> (v & 0xfu)) & 1u;
}

static uint32_t
pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000u | (cnt & 0x3fffu) | (parity(cnt) << 15) | ((op & 0x7fu) << 16) | (parity(op) << 23);
}

static double
now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static int
alloc_map(int fd, uint64_t size, struct kgsl_gpumem_alloc_id *a, volatile uint32_t **cpu)
{
	memset(a, 0, sizeof(*a));
	a->size = size;
	if (ioctl(fd, ALLOC_ID, a)) {
		return -errno;
	}
	struct kgsl_ios_map m = { .id = a->id };
	if (ioctl(fd, IOS_MAP, &m)) {
		return -errno;
	}
	*cpu = (volatile uint32_t *)(uintptr_t)m.addr;
	return 0;
}

static int
submit(int fd, uint32_t ctx, uint64_t ib_va, uint32_t ib_bytes, uint32_t ib_id, uint32_t *ts)
{
	struct kgsl_command_object obj = { .gpuaddr = ib_va, .size = ib_bytes, .flags = 1, .id = ib_id };
	struct kgsl_gpu_command c = {
		.flags = 4, .cmdlist = (uint64_t)(uintptr_t)&obj, .cmdsize = sizeof(obj), .numcmds = 1, .context_id = ctx,
	};
	int r = ioctl(fd, GPU_COMMAND, &c);
	*ts = c.timestamp;
	return r ? -errno : 0;
}

int
main(void)
{
	int fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
	CHECK(fd >= 0, "open /dev/kgsl-3d0 (%s)", fd >= 0 ? "fd" : strerror(errno));
	if (fd < 0) {
		return 1;
	}
	struct kgsl_devinfo di = { 0 };
	struct kgsl_device_getproperty gp = { .type = 1, .value = (uint64_t)(uintptr_t)&di, .sizebytes = sizeof(di) };
	int r = ioctl(fd, GETPROPERTY, &gp);
	CHECK(!r && di.chip_id == 0x43050a01u, "DEVICE_INFO chip %08x gmem %llx +%llx", di.chip_id,
	    (unsigned long long)di.gmem_gpubaseaddr, (unsigned long long)di.gmem_sizebytes);
	uint64_t va64 = 0;
	gp = (struct kgsl_device_getproperty){ .type = 0x2c, .value = (uint64_t)(uintptr_t)&va64, .sizebytes = 8 };
	r = ioctl(fd, GETPROPERTY, &gp);
	CHECK(!r, "GPU_VA64_SIZE %llu MiB", (unsigned long long)(va64 >> 20));

	struct kgsl_drawctxt_create cc = { .flags = 0x13 };
	r = ioctl(fd, CTX_CREATE, &cc);
	CHECK(!r && cc.drawctxt_id, "DRAWCTXT_CREATE id %u", cc.drawctxt_id);

	struct kgsl_gpumem_alloc_id ib, tg;
	volatile uint32_t *ibp = NULL, *tgp = NULL;
	r = alloc_map(fd, 0x1000, &ib, &ibp);
	CHECK(!r, "IB buffer id %u gpu %llx size %llx cpu %p (%d)", ib.id, (unsigned long long)ib.gpuaddr,
	    (unsigned long long)ib.mmapsize, (void *)ibp, r);
	r |= alloc_map(fd, 0x10000, &tg, &tgp);
	CHECK(!r, "target buffer id %u gpu %llx cpu %p", tg.id, (unsigned long long)tg.gpuaddr, (void *)tgp);
	if (r) {
		return 1;
	}
	CHECK(tgp[0] == 0 && tgp[100] == 0, "buffers come zeroed");
	tgp[1] = 0x12345678u;
	CHECK(tgp[1] == 0x12345678u, "CPU write/read through the mapping");

	/* IB: CP_MEM_WRITE target[0] = C0FFEE42, CP_WAIT_MEM_WRITES */
	uint64_t dst = tg.gpuaddr;
	uint32_t n = 0;
	ibp[n++] = pkt7(0x3d, 3);
	ibp[n++] = (uint32_t)dst;
	ibp[n++] = (uint32_t)(dst >> 32);
	ibp[n++] = 0xc0ffee42u;
	ibp[n++] = pkt7(0x12, 0);
	__asm__ volatile ("dsb sy" ::: "memory");
	uint32_t ts = 0;
	r = submit(fd, cc.drawctxt_id, ib.gpuaddr, n * 4, ib.id, &ts);
	CHECK(!r && ts, "GPU_COMMAND ts %u (%d)", ts, r);
	struct kgsl_device_waittimestamp_ctxtid w = { .context_id = cc.drawctxt_id, .timestamp = ts, .timeout = 1000 };
	r = ioctl(fd, WAITTS, &w);
	CHECK(!r, "WAITTIMESTAMP (%s)", r ? strerror(errno) : "retired");
	CHECK(tgp[0] == 0xc0ffee42u, "GPU wrote the target from our IB: %08x", tgp[0]);
	struct kgsl_cmdstream_readtimestamp_ctxtid rt = { .context_id = cc.drawctxt_id, .type = 2 };
	r = ioctl(fd, READTS, &rt);
	CHECK(!r && rt.timestamp == ts, "READTIMESTAMP %u", rt.timestamp);

	/* 1000 submits, each writes its index */
	n = 0;
	ibp[n++] = pkt7(0x3d, 3);
	ibp[n++] = (uint32_t)(dst + 8);
	ibp[n++] = (uint32_t)((dst + 8) >> 32);
	uint32_t vpos = n++;
	ibp[n++] = pkt7(0x12, 0);
	double t0 = now();
	int ok = 1;
	for (uint32_t i = 1; i <= 1000 && ok; i++) {
		ibp[vpos] = i;
		__asm__ volatile ("dsb sy" ::: "memory");
		ok = !submit(fd, cc.drawctxt_id, ib.gpuaddr, n * 4, ib.id, &ts);
		w.timestamp = ts;
		ok = ok && !ioctl(fd, WAITTS, &w) && tgp[2] == i;
	}
	double dt = now() - t0;
	CHECK(ok && tgp[2] == 1000, "1000 submit+wait round trips in %.1f ms (%.0f us each)", dt * 1e3, dt * 1e3);

	struct kgsl_gpumem_alloc_id big;
	volatile uint32_t *bigp = NULL;
	r = alloc_map(fd, 64u << 20, &big, &bigp);
	CHECK(!r, "64 MiB buffer gpu %llx", (unsigned long long)big.gpuaddr);
	if (!r) {
		bigp[(64u << 20) / 4 - 1] = 7;
		CHECK(bigp[(64u << 20) / 4 - 1] == 7, "last word of the 64 MiB buffer");
		struct kgsl_gpumem_free_id f = { .id = big.id };
		CHECK(!ioctl(fd, FREE_ID, &f), "FREE_ID 64 MiB");
	}
	struct kgsl_drawctxt_destroy cd = { .drawctxt_id = cc.drawctxt_id };
	CHECK(!ioctl(fd, CTX_DESTROY, &cd), "DRAWCTXT_DESTROY");
	close(fd);
	printf("%s: %d failure(s)\n", fails ? "KGSL TEST FAILED" : "KGSL TEST PASSED", fails);
	return fails != 0;
}
