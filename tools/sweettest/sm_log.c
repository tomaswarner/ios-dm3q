/* sm_log [KIB]: the kernel's persistent console log (xnu 0047 SWEETLOG ring at the end of the root
 * partition, flushed every 10 s), last KIB KiB (default 256), read through /dev/rufsroot0: the whole
 * boot, including what happens before USB is up, without the logview flash. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioccom.h>
#define DKIOCGETBLOCKSIZE _IOR('d', 24, uint32_t)   /* sys/disk.h (not in the iOS SDK) */
#define DKIOCGETBLOCKCOUNT _IOR('d', 25, uint64_t)
#include <sys/ioctl.h>
#include <unistd.h>
#define BLK 4096u
#define RING (4u << 20)
int main(int argc, char **argv)
{
	uint64_t want = (argc > 1 ? strtoull(argv[1], NULL, 0) : 256) << 10;
	int fd = open("/dev/rufsroot0", O_RDONLY);
	uint64_t count = 0;
	uint32_t bs = 0;
	if (fd < 0 || ioctl(fd, DKIOCGETBLOCKCOUNT, &count) || ioctl(fd, DKIOCGETBLOCKSIZE, &bs)) {
		perror("rufsroot0");
		return 1;
	}
	uint64_t blocks = count * bs / BLK, base = (blocks - (1 + RING / BLK)) * BLK;
	static uint8_t ring[RING], h[BLK];
	if (pread(fd, h, BLK, (off_t)base) != BLK || memcmp(h, "SWEETLOG", 8)) {
		printf("no SWEETLOG header at %llu\n", (unsigned long long)base);
		return 1;
	}
	uint64_t total, seq;
	memcpy(&total, h + 16, 8);
	memcpy(&seq, h + 24, 8);
	if (pread(fd, ring, RING, (off_t)(base + BLK)) != RING) {
		perror("ring");
		return 1;
	}
	uint64_t valid = total < RING ? total : RING, n = want < valid ? want : valid;
	fprintf(stderr, "SWEETLOG total %llu seq %llu, last %llu bytes\n", (unsigned long long)total,
	    (unsigned long long)seq, (unsigned long long)n);
	for (uint64_t i = total - n; i < total; i++)
		putchar(ring[i % RING]);
	return 0;
}
