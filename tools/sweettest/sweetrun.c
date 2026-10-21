/*
 * sweetrun: developer test runner for the S23 iOS port, so userland tests no
 * longer need a USERDATA re-flash. Started by launchd (tools/add-sweetrun.sh).
 * Listens only on the USB Ethernet address 192.168.7.2:5141 (point-to-point
 * cable to the Mac, nothing else routes there) and requires the token baked
 * into the root image at /usr/local/sweettest/sweetrun.token.
 *
 * Protocol (one client at a time, text lines then raw bytes):
 *   TOKEN <token>
 *   FILE <name> <size>     then <size> bytes; saved as /usr/local/sweettest/<name>, mode 755
 *   RUN <name> [arg...]    runs /usr/local/sweettest/<name>; its stdout+stderr stream back
 *   -> the server ends with "SWEETRUN EXIT <wait status>" and closes.
 * Names are plain file names (no '/'). The client hanging up kills the test.
 * Host side: tools/sweetrun.py.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef DIR
#define DIR "/usr/local/sweettest/"
#endif
#ifndef ADDR
#define ADDR "192.168.7.2"
#endif
#define PORT 5141
#define MAXARGS 16

static char token[128];

static int read_line(int fd, char *buf, size_t len)
{
	size_t n = 0;
	while (n + 1 < len) {
		ssize_t r = read(fd, buf + n, 1);
		if (r <= 0)
			return -1;
		if (buf[n] == '\n')
			break;
		n++;
	}
	buf[n] = 0;
	return (int)n;
}

static void say(int fd, const char *msg)
{
	(void)write(fd, msg, strlen(msg));
}

static int valid_name(const char *s)
{
	return *s && !strchr(s, '/') && strcmp(s, ".") && strcmp(s, "..");
}

static int recv_file(int c, const char *name, long size)
{
	char path[512], tmp[520];
	snprintf(path, sizeof(path), DIR "%s", name);
	snprintf(tmp, sizeof(tmp), "%s.part", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (fd < 0)
		return -1;
	static char buf[65536];
	while (size > 0) {
		ssize_t r = read(c, buf, size < (long)sizeof(buf) ? (size_t)size : sizeof(buf));
		if (r <= 0 || write(fd, buf, (size_t)r) != r) {
			close(fd);
			unlink(tmp);
			return -1;
		}
		size -= r;
	}
	fchmod(fd, 0755);
	close(fd);
	return rename(tmp, path);   /* replace a running binary safely */
}

static void run(int c, char *line)
{
	char *argv[MAXARGS + 1];
	int argc = 0;
	for (char *t = strtok(line, " "); t && argc < MAXARGS; t = strtok(NULL, " "))
		argv[argc++] = t;
	argv[argc] = NULL;
	if (!argc || !valid_name(argv[0])) {
		say(c, "SWEETRUN ERR bad name\n");
		return;
	}
	char path[512];
	snprintf(path, sizeof(path), DIR "%s", argv[0]);
	argv[0] = path;
	pid_t pid = fork();
	if (pid == 0) {
		dup2(c, 1);
		dup2(c, 2);
		close(c);
		chdir(DIR);
		signal(SIGPIPE, SIG_DFL);
		execv(path, argv);
		printf("SWEETRUN ERR exec %s: %s\n", path, strerror(errno));
		_exit(127);
	}
	if (pid < 0) {
		say(c, "SWEETRUN ERR fork\n");
		return;
	}
	/* Kill the test if the client hangs up. */
	int st = 0;
	for (;;) {
		pid_t w = waitpid(pid, &st, WNOHANG);
		if (w == pid)
			break;
		struct pollfd p = { .fd = c, .events = POLLIN };
		if (poll(&p, 1, 200) > 0) {
			char b;
			if (recv(c, &b, 1, MSG_PEEK) <= 0) {
				kill(pid, SIGKILL);
				waitpid(pid, &st, 0);
				return;
			}
		}
	}
	char msg[64];
	snprintf(msg, sizeof(msg), "SWEETRUN EXIT 0x%x\n", st);
	say(c, msg);
}

static void serve(int c)
{
	char line[1024];
	if (read_line(c, line, sizeof(line)) < 0 || strncmp(line, "TOKEN ", 6) ||
	    !token[0] || strcmp(line + 6, token)) {
		say(c, "SWEETRUN ERR token\n");
		return;
	}
	while (read_line(c, line, sizeof(line)) >= 0) {
		if (!strncmp(line, "FILE ", 5)) {
			char name[256];
			long size;
			if (sscanf(line + 5, "%255s %ld", name, &size) != 2 || !valid_name(name) || size < 0 ||
			    recv_file(c, name, size)) {
				say(c, "SWEETRUN ERR file\n");
				return;
			}
		} else if (!strncmp(line, "RUN ", 4)) {
			run(c, line + 4);
			return;
		} else {
			say(c, "SWEETRUN ERR command\n");
			return;
		}
	}
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN);
	FILE *f = fopen(DIR "sweetrun.token", "r");
	if (!f || !fgets(token, sizeof(token), f)) {
		printf("sweetrun: no token, not serving\n");
		return 1;
	}
	fclose(f);
	token[strcspn(token, "\r\n")] = 0;

	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(PORT) };
	inet_pton(AF_INET, ADDR, &sa.sin_addr);
	int s = -1;
	/* The USB interface gets its address some time after launchd starts us. */
	for (;;) {
		s = socket(AF_INET, SOCK_STREAM, 0);
		int one = 1;
		setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(s, 1) == 0)
			break;
		close(s);
		sleep(2);
	}
	printf("sweetrun: listening on " ADDR ":%d\n", PORT);
	fflush(stdout);
	for (;;) {
		int c = accept(s, NULL, NULL);
		if (c < 0)
			continue;
		serve(c);
		close(c);
	}
}
