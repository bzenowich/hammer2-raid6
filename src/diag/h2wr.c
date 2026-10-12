/*
 * h2wr <file> <byte> <nbytes> [fsync]: overwrite from offset 0 with
 * <nbytes> of <byte>, through an O_SYNC descriptor (tests/v3 group R).
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int
main(int argc, char **argv)
{
	size_t n = (size_t)atol(argv[3]);
	char *buf = malloc(n);
	int fd = open(argv[1], O_WRONLY | O_CREAT | O_SYNC, 0644);

	if (fd < 0 || buf == NULL)
		return 1;
	memset(buf, argv[2][0], n);
	if (pwrite(fd, buf, n, 0) != (ssize_t)n)
		return 1;
	if (argc > 4 && fsync(fd) != 0)
		return 1;
	return close(fd) != 0;
}
