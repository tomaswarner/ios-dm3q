/* sm_install: installs SweetMetal.bundle from sweetrun's upload dir into the root filesystem
 * (/System/Library/Extensions/SweetMetal.bundle/{Info.plist,SweetMetal}), where Metal's
 * _MTLRegisterDevices looks for the IOAcceleratorES service's MetalPluginName. Run through
 * sweetrun (root, writable root with rootrw=1): sweetrun.py -f SweetMetal -f Info.plist -f sm_install sm_install */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SRC "/usr/local/sweettest/"
#define DST "/System/Library/Extensions/SweetMetal.bundle/"

static int copy(const char *name, mode_t mode)
{
	char from[256], to[256], tmp[256];
	snprintf(from, sizeof from, SRC "%s", name);
	snprintf(to, sizeof to, DST "%s", name);
	snprintf(tmp, sizeof tmp, DST ".%s.new", name);
	int in = open(from, O_RDONLY), out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (in < 0 || out < 0) {
		printf("FAIL open %s / %s: %s\n", from, tmp, strerror(errno));
		return 1;
	}
	char buf[1 << 16];
	ssize_t n;
	long total = 0;
	while ((n = read(in, buf, sizeof buf)) > 0) {
		if (write(out, buf, (size_t)n) != n) {
			printf("FAIL write %s: %s\n", tmp, strerror(errno));
			return 1;
		}
		total += n;
	}
	close(in);
	fsync(out);
	close(out);
	/* rename over the old file: processes that mapped it keep the old inode */
	if (rename(tmp, to) != 0) {
		printf("FAIL rename %s: %s\n", to, strerror(errno));
		return 1;
	}
	printf("installed %s (%ld bytes)\n", to, total);
	return 0;
}

int main(void)
{
	if (mkdir(DST, 0755) != 0 && errno != EEXIST) {
		printf("FAIL mkdir " DST ": %s\n", strerror(errno));
		return 1;
	}
	int bad = copy("Info.plist", 0644) + copy("SweetMetal", 0755);
	sync();
	printf(bad ? "SM_INSTALL FAIL\n" : "SM_INSTALL OK\n");
	return bad;
}
