/* sm_cp NAME DEST: copy NAME from sweetrun's upload dir to DEST (absolute path) atomically, mode 0755
 * (rename over the old file: processes that mapped it keep the old inode). Root fs needs rootrw=1. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc != 3 || argv[2][0] != '/' || strchr(argv[1], '/')) {
		printf("usage: sm_cp NAME /DEST\n");
		return 2;
	}
	char from[512], tmp[512];
	snprintf(from, sizeof from, "/usr/local/sweettest/%s", argv[1]);
	snprintf(tmp, sizeof tmp, "%s.sm_cp_new", argv[2]);
	int in = open(from, O_RDONLY), out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (in < 0 || out < 0) {
		printf("FAIL open %s / %s: %s\n", from, tmp, strerror(errno));
		return 1;
	}
	char buf[1 << 16];
	ssize_t n;
	long total = 0;
	while ((n = read(in, buf, sizeof buf)) > 0) {
		if (write(out, buf, (size_t)n) != n) {
			printf("FAIL write: %s\n", strerror(errno));
			return 1;
		}
		total += n;
	}
	fsync(out);
	close(out);
	close(in);
	if (rename(tmp, argv[2])) {
		printf("FAIL rename %s: %s\n", argv[2], strerror(errno));
		return 1;
	}
	printf("installed %s (%ld bytes)\n", argv[2], total);
	return 0;
}
