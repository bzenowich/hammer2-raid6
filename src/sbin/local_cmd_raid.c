/*
 * v3 RAIDZ2-native userspace front-end for the RAID6 ioctls.
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
 *   raid scrub [-n]
 *	HAMMER2IOC_RAID_SCRUB — walk every live DATA/DIRENT bref, verify
 *	the CHECK code, parity-repair on mismatch.  Default is blocking;
 *	with -n, forks a child to run the blocking ioctl and polls
 *	HAMMER2IOC_RAID_SCRUB_STATUS from the parent for live progress.
 */

#include "hammer2.h"

#include <sys/wait.h>
#include <unistd.h>

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
	printf("ndisks:         %u\n", st.ndisks);
	{
		uint32_t i;
		for (i = 0; i < st.ndisks && i < HAMMER2_MAX_VOLUMES; i++) {
			const char *label;
			switch (st.disk_state[i]) {
			case HAMMER2_RAID6_DISK_FAILED:
				label = "FAILED";
				break;
			case HAMMER2_RAID6_DISK_ONLINE:
				label = "ONLINE";
				break;
			default:
				label = "UNKNOWN";
				break;
			}
			printf("disk[%u]:        %s\n", i, label);
		}
	}
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

static void
raid_scrub_print_final(const struct hammer2_ioc_raid_scrub *rs)
{
	printf("scrub complete:\n");
	printf("  brefs_done:         %llu\n",
	       (unsigned long long)rs->brefs_done);
	printf("  brefs_bad:          %llu\n",
	       (unsigned long long)rs->brefs_bad);
	printf("  brefs_repaired:     %llu\n",
	       (unsigned long long)rs->brefs_repaired);
	printf("  brefs_unrepairable: %llu\n",
	       (unsigned long long)rs->brefs_unrepairable);
	printf("  error:              %d\n", rs->error);
}

/*
 * Blocking scrub.  Returns the kernel's final counters; one ioctl
 * per call.
 */
static int
raid_scrub_blocking(const char *sel_path)
{
	struct hammer2_ioc_raid_scrub rs;
	int fd;
	int rc;

	fd = hammer2_ioctl_handle(sel_path);
	if (fd < 0)
		return 1;
	bzero(&rs, sizeof(rs));
	rc = ioctl(fd, HAMMER2IOC_RAID_SCRUB, &rs);
	close(fd);
	if (rc < 0) {
		fprintf(stderr, "raid scrub: %s\n", strerror(errno));
		return 1;
	}
	raid_scrub_print_final(&rs);
	return (rs.error || rs.brefs_unrepairable) ? 1 : 0;
}

/*
 * Polling scrub (-n): fork; the child issues the blocking SCRUB
 * ioctl and exits; the parent polls SCRUB_STATUS once a second and
 * prints progress on every change.  Useful for long scrubs where
 * the user wants visibility into bref/sec progress without tail-f'ing
 * dmesg.
 */
static int
raid_scrub_polling(const char *sel_path)
{
	struct hammer2_ioc_raid_scrub rs;
	uint64_t last_done = ~(uint64_t)0;
	pid_t pid;
	int status;
	int fd;

	fd = hammer2_ioctl_handle(sel_path);
	if (fd < 0)
		return 1;

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "raid scrub -n: fork: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	if (pid == 0) {
		/* Child: blocking scrub. */
		bzero(&rs, sizeof(rs));
		(void)ioctl(fd, HAMMER2IOC_RAID_SCRUB, &rs);
		_exit(rs.error ? 1 : 0);
	}

	/* Parent: poll status until the child completes. */
	for (;;) {
		pid_t w;

		sleep(1);
		w = waitpid(pid, &status, WNOHANG);
		if (w == pid)
			break;
		if (w < 0) {
			fprintf(stderr, "raid scrub -n: waitpid: %s\n",
				strerror(errno));
			break;
		}
		bzero(&rs, sizeof(rs));
		if (ioctl(fd, HAMMER2IOC_RAID_SCRUB_STATUS, &rs) < 0)
			continue;
		if (rs.brefs_done != last_done) {
			printf("\rscrub: %llu brefs done, %llu bad, "
			       "%llu repaired, %llu unrep    ",
			       (unsigned long long)rs.brefs_done,
			       (unsigned long long)rs.brefs_bad,
			       (unsigned long long)rs.brefs_repaired,
			       (unsigned long long)rs.brefs_unrepairable);
			fflush(stdout);
			last_done = rs.brefs_done;
		}
	}
	printf("\n");

	/* Final snapshot from STATUS (child has exited, scrub_running=0). */
	bzero(&rs, sizeof(rs));
	(void)ioctl(fd, HAMMER2IOC_RAID_SCRUB_STATUS, &rs);
	close(fd);
	raid_scrub_print_final(&rs);
	return (rs.error || rs.brefs_unrepairable) ? 1 : 0;
}

int
cmd_raid(const char *sel_path, int ac, const char **av)
{
	if (ac < 1) {
		fprintf(stderr,
		    "raid: subcommand required (status|fail-disk|replace|scrub)\n");
		return 1;
	}
	if (strcmp(av[0], "status") == 0) {
		/* Optional positional path overrides -s sel_path. */
		const char *path = (ac >= 2) ? av[1] : sel_path;
		return raid_status(path);
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
	} else if (strcmp(av[0], "scrub") == 0) {
		if (ac >= 2 && strcmp(av[1], "-n") == 0)
			return raid_scrub_polling(sel_path);
		return raid_scrub_blocking(sel_path);
	}
	fprintf(stderr, "raid: unknown subcommand '%s'\n", av[0]);
	return 1;
}
