/*
 * sweettest: run at boot by launchd (entry from tools/add-sweettest.py), it
 * starts the pre-installed test programs named by the kernel boot-arg
 * sweettest=NAME[,NAME...] from /usr/local/sweettest/, one after the other.
 * No boot-arg: does nothing. Output goes to stdout/stderr, which launchd points
 * at /dev/null; with the test names in boot-arg sweetnull (xnu patch 0048) the
 * kernel copies those lines into its console, i.e. the USB live log.
 * Changing the test = re-flashing only the boot image with another boot-arg.
 */
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

#define DIR "/usr/local/sweettest/"

extern char **environ;

int main(void)
{
	char args[1024] = "";
	size_t len = sizeof(args) - 1;
	if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0)
		return 1;
	char *p = strstr(args, "sweettest=");
	if (!p) {
		printf("sweettest: no sweettest= boot-arg, nothing to run\n");
		return 0;
	}
	p += strlen("sweettest=");
	char list[256];
	size_t n = strcspn(p, " ");
	if (n >= sizeof(list))
		n = sizeof(list) - 1;
	memcpy(list, p, n);
	list[n] = 0;
	for (char *name = strtok(list, ","); name; name = strtok(NULL, ",")) {
		if (strchr(name, '/') || !*name)
			continue;               /* names only, inside DIR */
		char path[512];
		snprintf(path, sizeof(path), DIR "%s", name);
		char *argv[] = { path, NULL };
		pid_t pid;
		printf("sweettest: run %s\n", path);
		fflush(stdout);
		int rc = posix_spawn(&pid, path, NULL, NULL, argv, environ);
		if (rc) {
			printf("sweettest: spawn %s failed %d\n", path, rc);
			continue;
		}
		int st = 0;
		waitpid(pid, &st, 0);
		printf("sweettest: %s exited, status 0x%x\n", name, st);
		fflush(stdout);
	}
	return 0;
}
