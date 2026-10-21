/* sm_touch PATH [0]: create PATH (or remove it with a second argument 0) - toggles driver traces */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	if (argc < 2)
		return 2;
	int r = argc > 2 && argv[2][0] == '0' ? unlink(argv[1]) : close(open(argv[1], O_CREAT | O_WRONLY, 0644));
	printf("%s %s -> %d\n", argc > 2 ? "rm" : "touch", argv[1], r);
	return 0;
}
