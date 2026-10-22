/*
 * sm_input: drive the phone from the Mac (xnu sweet_input.c, sysctl kern.sweet_input):
 *   sm_input tap X Y                 one finger down and up at logical point X,Y (1125x2436)
 *   sm_input swipe X1 Y1 X2 Y2 [MS]  drag in ~60 Hz steps over MS milliseconds (default 300)
 *   sm_input hold X Y MS             long press
 *   sm_input key home|power|volup|voldown [MS]   press and release a button
 * Build: xcrun -sdk iphoneos clang -target arm64-apple-ios16.0 -O2 sm_input.c -o sm_input && codesign -s - sm_input
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

static int send(const char *cmd)
{
	if (sysctlbyname("kern.sweet_input", NULL, NULL, (void *)cmd, strlen(cmd))) {
		perror("kern.sweet_input");
		return -1;
	}
	return 0;
}

static int touch(int down, int x, int y)
{
	char b[64];
	snprintf(b, sizeof b, "t 0 %d %d %d", down, x, y);
	return send(b);
}

int main(int argc, char **argv)
{
	if (argc >= 4 && !strcmp(argv[1], "tap")) {
		int x = atoi(argv[2]), y = atoi(argv[3]);
		if (touch(1, x, y))
			return 1;
		usleep(80000);
		return touch(0, x, y) ? 1 : 0;
	}
	if (argc >= 5 && !strcmp(argv[1], "hold")) {
		int x = atoi(argv[2]), y = atoi(argv[3]);
		if (touch(1, x, y))
			return 1;
		usleep(atoi(argv[4]) * 1000);
		return touch(0, x, y) ? 1 : 0;
	}
	if (argc >= 6 && !strcmp(argv[1], "swipe")) {
		int x1 = atoi(argv[2]), y1 = atoi(argv[3]), x2 = atoi(argv[4]), y2 = atoi(argv[5]);
		int ms = argc >= 7 ? atoi(argv[6]) : 300, steps = ms / 16 > 2 ? ms / 16 : 2;
		if (touch(1, x1, y1))
			return 1;
		for (int i = 1; i <= steps; i++) {
			usleep(16000);
			touch(1, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
		}
		usleep(16000);
		return touch(0, x2, y2) ? 1 : 0;
	}
	if (argc >= 3 && !strcmp(argv[1], "key")) {
		static const char *names[] = { "power", "volup", "voldown", "home" };
		int key = -1;
		for (int i = 0; i < 4; i++)
			if (!strcmp(argv[2], names[i]))
				key = i;
		if (key < 0)
			return 2;
		char b[32];
		snprintf(b, sizeof b, "k %d 1", key);
		if (send(b))
			return 1;
		usleep((argc >= 4 ? atoi(argv[3]) : 100) * 1000);
		snprintf(b, sizeof b, "k %d 0", key);
		return send(b) ? 1 : 0;
	}
	fprintf(stderr, "usage: sm_input tap X Y | hold X Y MS | swipe X1 Y1 X2 Y2 [MS] | key NAME [MS]\n");
	return 2;
}
