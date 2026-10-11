/*
 * h2devstat — print per-device I/O totals from devstat(9).
 *
 * Usage: h2devstat dev ...      (e.g. h2devstat vbd1 vbd2, da0 da1)
 * Output, one line per named device that exists:
 *   <dev> <num_reads> <bytes_read> <num_writes> <bytes_written>
 *
 * Build: cc -o h2devstat h2devstat.c -ldevstat
 * Used by tests/v3/test_p_perfshape.sh to measure I/O shape.
 */
#include <sys/types.h>
#include <devstat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int ac, char **av)
{
	struct statinfo s;
	int i, j;

	memset(&s, 0, sizeof(s));
	s.dinfo = calloc(1, sizeof(struct devinfo));
	if (s.dinfo == NULL || getdevs(&s) < 0) {
		fprintf(stderr, "h2devstat: %s\n", devstat_errbuf);
		return 1;
	}
	for (i = 0; i < s.dinfo->numdevs; i++) {
		struct devstat *d = &s.dinfo->devices[i];
		char name[64];

		snprintf(name, sizeof(name), "%s%d", d->device_name,
		    d->unit_number);
		for (j = 1; j < ac; j++) {
			if (strcmp(name, av[j]) != 0)
				continue;
			printf("%s %ju %ju %ju %ju\n", name,
			    (uintmax_t)d->num_reads,
			    (uintmax_t)d->bytes_read,
			    (uintmax_t)d->num_writes,
			    (uintmax_t)d->bytes_written);
		}
	}
	return 0;
}
