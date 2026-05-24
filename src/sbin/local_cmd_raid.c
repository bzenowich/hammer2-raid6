/*
 * v4 RAIDZ2-native userspace front-end for the RAID6 ioctls.
 *
 * Subcommands:
 *   raid status
 *	HAMMER2IOC_RAID_RESILVER_STATUS
 *   raid fail-disk <dev>
 *	HAMMER2IOC_RAID_FAIL_DISK — stop kernel I/O to <dev>, leaving the
 *	array degraded.  Idempotent if already failed.
 *   raid replace <old> <new>
 *	HAMMER2IOC_RAID_REPLACE — synchronous online resilver of <old>'s
 *	stripe positions onto <new>.  Same path for both args is the
 *	"reattach the same slot" case used by the test suite.
 */

#include "hammer2.h"

int cmd_raid(const char *sel_path, int ac, const char **av);

static int
raid_status(const char *sel_path)
{
	struct hammer2_ioc_resilver_status st;
	int fd;
	int rc;

	fd = hammer2_ioctl_handle(sel_path);
	if (fd < 0)
		return 1;
	bzero(&st, sizeof(st));
	rc = ioctl(fd, HAMMER2IOC_RAID_RESILVER_STATUS, &st);
	close(fd);
	if (rc < 0) {
		fprintf(stderr, "raid status: %s\n", strerror(errno));
		return 1;
	}
	printf("running:        %d\n", st.running);
	printf("disk_idx:       %d\n", st.disk_idx);
	printf("progress:       %d%%\n", st.progress);
	printf("stripes_done:   %llu\n", (unsigned long long)st.stripes_done);
	printf("stripes_total:  %llu\n", (unsigned long long)st.stripes_total);
	printf("error:          %d\n", st.error);
	return 0;
}

static int
raid_fail_disk(const char *sel_path, const char *dev)
{
	struct hammer2_ioc_raid_fail_disk rfd;
	int fd;
	int rc;

	fd = hammer2_ioctl_handle(sel_path);
	if (fd < 0)
		return 1;
	bzero(&rfd, sizeof(rfd));
	snprintf(rfd.dev, sizeof(rfd.dev), "%s", dev);
	rc = ioctl(fd, HAMMER2IOC_RAID_FAIL_DISK, &rfd);
	close(fd);
	if (rc < 0) {
		fprintf(stderr, "raid fail-disk %s: %s\n",
			dev, strerror(errno));
		return 1;
	}
	printf("disk %d (%s) marked failed\n", rfd.disk_idx, dev);
	return 0;
}

static int
raid_replace(const char *sel_path, const char *old_dev, const char *new_dev)
{
	struct hammer2_ioc_raid_replace rr;
	int fd;
	int rc;

	fd = hammer2_ioctl_handle(sel_path);
	if (fd < 0)
		return 1;
	bzero(&rr, sizeof(rr));
	snprintf(rr.old_dev, sizeof(rr.old_dev), "%s", old_dev);
	snprintf(rr.new_dev, sizeof(rr.new_dev), "%s", new_dev);
	rc = ioctl(fd, HAMMER2IOC_RAID_REPLACE, &rr);
	close(fd);
	if (rc < 0) {
		fprintf(stderr, "raid replace %s -> %s: %s\n",
			old_dev, new_dev, strerror(errno));
		return 1;
	}
	if (rr.error) {
		fprintf(stderr, "raid replace: kernel reported error %d\n",
			rr.error);
		return 1;
	}
	printf("raid replace %s -> %s complete\n", old_dev, new_dev);
	return 0;
}

int
cmd_raid(const char *sel_path, int ac, const char **av)
{
	if (ac < 1) {
		fprintf(stderr,
		    "raid: subcommand required (status|fail-disk|replace)\n");
		return 1;
	}
	if (strcmp(av[0], "status") == 0) {
		return raid_status(sel_path);
	} else if (strcmp(av[0], "fail-disk") == 0) {
		if (ac != 2) {
			fprintf(stderr, "raid fail-disk: requires <dev>\n");
			return 1;
		}
		return raid_fail_disk(sel_path, av[1]);
	} else if (strcmp(av[0], "replace") == 0) {
		if (ac != 3) {
			fprintf(stderr,
			    "raid replace: requires <old_dev> <new_dev>\n");
			return 1;
		}
		return raid_replace(sel_path, av[1], av[2]);
	}
	fprintf(stderr, "raid: unknown subcommand '%s'\n", av[0]);
	return 1;
}
