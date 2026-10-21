/* sm_kill NAME...: SIGKILL every process with one of these names (launchd respawns KeepAlive jobs) */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

int main(int argc, char **argv)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
	size_t len = 0;
	sysctl(mib, 3, NULL, &len, NULL, 0);
	struct kinfo_proc *p = malloc(len);
	if (!p || sysctl(mib, 3, p, &len, NULL, 0) != 0)
		return 1;
	for (size_t i = 0; i < len / sizeof *p; i++)
		for (int a = 1; a < argc; a++)
			if (!strcmp(p[i].kp_proc.p_comm, argv[a])) {
				int r = kill(p[i].kp_proc.p_pid, SIGKILL);
				printf("kill %s[%d] -> %d\n", argv[a], p[i].kp_proc.p_pid, r);
			}
	return 0;
}
