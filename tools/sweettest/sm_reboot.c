/* sm_reboot download|normal: reboot the phone (xnu 0127 kern.sweet_reboot); download = Samsung Download
 * Mode, so the Mac can flash right after (tools/flash-and-wait.sh). */
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
int main(int argc, char **argv)
{
	const char *how = argc > 1 ? argv[1] : "download";
	printf("rebooting: %s\n", how);
	fflush(stdout);
	if (sysctlbyname("kern.sweet_reboot", NULL, NULL, (void *)how, strlen(how))) {
		perror("kern.sweet_reboot");
		return 1;
	}
	return 0;
}
