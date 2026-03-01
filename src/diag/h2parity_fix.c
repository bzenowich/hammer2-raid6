/*
 * HAMMER2 RAID6 parity repair tool.
 *
 * Reads data columns from disk2.img (col0 source) and disk3.img (col1 source)
 * — the original, correct disk images — recomputes correct P and Q parity
 * for every stripe, and writes them back to disk0.img (P) and disk1.img (Q).
 *
 * This fixes parity corruption introduced by the GETBLK_NOWAIT bug (sibling
 * reads that returned zero-filled buffers when the block was not in cache).
 *
 * Run this tool with the HAMMER2 filesystem UNMOUNTED.
 *
 * Compile on DragonFlyBSD VM:
 *   cd /var/tmp && gcc -O2 h2parity_fix.c -o h2parity_fix
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define HAMMER2_PBUFSIZE	65536ULL
#define HAMMER2_ZONE_SEG	(4ULL * 1024 * 1024)
#define NDISKS			4
#define NDATA			2

/* GF(2^8) multiply by 2 (primitive polynomial 0x1d) */
static uint8_t
gf_mul2(uint8_t b)
{
	return (b & 0x80) ? (((b << 1) & 0xFF) ^ 0x1d) : ((b << 1) & 0xFF);
}

/*
 * For ndisks=4, ndata=2, left-symmetric rotation:
 *   stripe N: p_disk = N%4, q_disk = (N+1)%4
 *   data disks (in column order, skipping p and q):
 *     N%4==0: p=0 q=1 → col0=disk2, col1=disk3
 *     N%4==1: p=1 q=2 → col0=disk0, col1=disk3
 *     N%4==2: p=2 q=3 → col0=disk0, col1=disk1
 *     N%4==3: p=3 q=0 → col0=disk1, col1=disk2
 */
static void
stripe_map(uint64_t stripe_num,
	   int *p_disk, int *q_disk,
	   int *col0_disk, int *col1_disk)
{
	int p, q, di, pd;

	p = (int)(stripe_num % NDISKS);
	q = (p + 1) % NDISKS;
	*p_disk = p;
	*q_disk = q;

	di = 0;
	for (pd = 0; pd < NDISKS; pd++) {
		if (pd == p || pd == q)
			continue;
		if (di == 0)
			*col0_disk = pd;
		else
			*col1_disk = pd;
		di++;
	}
}

static int fds[NDISKS];

static const char *disk_paths[NDISKS] = {  /* overridable via argv */
	"/build/var.tmp/disk0.img",
	"/build/var.tmp/disk1.img",
	"/build/var.tmp/disk2.img",   /* original col0 — use this, NOT disk4 */
	"/build/var.tmp/disk3.img",
};

static int
read_stripe(int disk, uint64_t phys_off, void *buf)
{
	ssize_t r;
	if (lseek(fds[disk], (off_t)phys_off, SEEK_SET) < 0) {
		fprintf(stderr, "lseek disk%d @ 0x%llx: %s\n",
			disk, (unsigned long long)phys_off, strerror(errno));
		return -1;
	}
	r = read(fds[disk], buf, HAMMER2_PBUFSIZE);
	if (r != (ssize_t)HAMMER2_PBUFSIZE) {
		fprintf(stderr, "read disk%d @ 0x%llx: got %zd/%llu: %s\n",
			disk, (unsigned long long)phys_off,
			r, (unsigned long long)HAMMER2_PBUFSIZE,
			strerror(errno));
		return -1;
	}
	return 0;
}

static int
write_stripe(int disk, uint64_t phys_off, const void *buf)
{
	ssize_t w;
	if (lseek(fds[disk], (off_t)phys_off, SEEK_SET) < 0) {
		fprintf(stderr, "lseek disk%d @ 0x%llx: %s\n",
			disk, (unsigned long long)phys_off, strerror(errno));
		return -1;
	}
	w = write(fds[disk], buf, HAMMER2_PBUFSIZE);
	if (w != (ssize_t)HAMMER2_PBUFSIZE) {
		fprintf(stderr, "write disk%d @ 0x%llx: got %zd/%llu: %s\n",
			disk, (unsigned long long)phys_off,
			w, (unsigned long long)HAMMER2_PBUFSIZE,
			strerror(errno));
		return -1;
	}
	return 0;
}

int
main(int argc, char *argv[])
{
	static uint8_t col0_buf[HAMMER2_PBUFSIZE];
	static uint8_t col1_buf[HAMMER2_PBUFSIZE];
	static uint8_t p_buf[HAMMER2_PBUFSIZE];
	static uint8_t q_buf[HAMMER2_PBUFSIZE];
	static uint8_t p_cur[HAMMER2_PBUFSIZE];
	static uint8_t q_cur[HAMMER2_PBUFSIZE];
	off_t disk_size;
	uint64_t usable;
	uint64_t num_stripes;
	uint64_t stripe_num;
	uint64_t phys_off;
	int p_disk, q_disk, col0_disk, col1_disk;
	int dry_run = 0;
	uint64_t p_fixed = 0, q_fixed = 0, errors = 0;
	size_t i;

	int argi = 1;
	if (argi < argc && strcmp(argv[argi], "-n") == 0) {
		dry_run = 1;
		argi++;
	}

	/* Accept optional disk image paths as positional arguments */
	if (argc - argi == NDISKS) {
		for (i = 0; i < NDISKS; i++)
			disk_paths[i] = argv[argi + i];
	} else if (argc - argi != 0) {
		fprintf(stderr, "usage: h2parity_fix [-n] "
			"[disk0 disk1 disk2 disk3]\n");
		return 1;
	}

	for (i = 0; i < NDISKS; i++) {
		int flags = dry_run ? O_RDONLY : O_RDWR;
		fds[i] = open(disk_paths[i], flags);
		if (fds[i] < 0) {
			fprintf(stderr, "open %s: %s\n",
				disk_paths[i], strerror(errno));
			return 1;
		}
	}

	/* Determine disk size (use disk0 as reference) */
	disk_size = lseek(fds[0], 0, SEEK_END);
	if (disk_size < 0) {
		perror("lseek SEEK_END");
		return 1;
	}
	printf("Disk size: %lld bytes (%.0f MB)\n",
	       (long long)disk_size, (double)disk_size / (1024*1024));

	usable = (uint64_t)disk_size - HAMMER2_ZONE_SEG;
	num_stripes = usable / HAMMER2_PBUFSIZE;
	printf("Usable per disk: %llu bytes, num_stripes = %llu\n",
	       (unsigned long long)usable, (unsigned long long)num_stripes);
	if (dry_run)
		printf("DRY RUN — will not write anything\n");
	printf("\n");

	for (stripe_num = 0; stripe_num < num_stripes; stripe_num++) {
		phys_off = HAMMER2_ZONE_SEG + stripe_num * HAMMER2_PBUFSIZE;

		stripe_map(stripe_num, &p_disk, &q_disk, &col0_disk, &col1_disk);

		/* Read data columns */
		if (read_stripe(col0_disk, phys_off, col0_buf) < 0) {
			errors++;
			continue;
		}
		if (read_stripe(col1_disk, phys_off, col1_buf) < 0) {
			errors++;
			continue;
		}

		/* Compute correct P = col0 XOR col1 */
		/* Compute correct Q = gf_mul2(col1) XOR col0 */
		for (i = 0; i < HAMMER2_PBUFSIZE; i++) {
			p_buf[i] = col0_buf[i] ^ col1_buf[i];
			q_buf[i] = gf_mul2(col1_buf[i]) ^ col0_buf[i];
		}

		/* Read current P and compare */
		if (read_stripe(p_disk, phys_off, p_cur) < 0) {
			errors++;
			continue;
		}
		if (memcmp(p_buf, p_cur, HAMMER2_PBUFSIZE) != 0) {
			p_fixed++;
			if (p_fixed <= 5) {
				printf("Fixing P: stripe %llu, p_disk=%d, "
				       "phys=0x%llx\n",
				       (unsigned long long)stripe_num,
				       p_disk,
				       (unsigned long long)phys_off);
				printf("  col0[0..7]: %02x %02x %02x %02x "
				       "%02x %02x %02x %02x\n",
				       col0_buf[0], col0_buf[1], col0_buf[2],
				       col0_buf[3], col0_buf[4], col0_buf[5],
				       col0_buf[6], col0_buf[7]);
				printf("  col1[0..7]: %02x %02x %02x %02x "
				       "%02x %02x %02x %02x\n",
				       col1_buf[0], col1_buf[1], col1_buf[2],
				       col1_buf[3], col1_buf[4], col1_buf[5],
				       col1_buf[6], col1_buf[7]);
				printf("  P_exp[0..7]: %02x %02x %02x %02x "
				       "%02x %02x %02x %02x\n",
				       p_buf[0], p_buf[1], p_buf[2], p_buf[3],
				       p_buf[4], p_buf[5], p_buf[6], p_buf[7]);
				printf("  P_act[0..7]: %02x %02x %02x %02x "
				       "%02x %02x %02x %02x\n",
				       p_cur[0], p_cur[1], p_cur[2], p_cur[3],
				       p_cur[4], p_cur[5], p_cur[6], p_cur[7]);
				/* Diagnose pattern */
				{
					int all_zero_act = 1, act_eq_col0 = 1;
					size_t j;
					for (j = 0; j < HAMMER2_PBUFSIZE; j++) {
						if (p_cur[j] != 0)
							all_zero_act = 0;
						if (p_cur[j] != col0_buf[j])
							act_eq_col0 = 0;
					}
					if (all_zero_act)
						printf("  DIAG: P_actual is all-zeros "
						       "(parity never written)\n");
					else if (act_eq_col0)
						printf("  DIAG: P_actual == col0 "
						       "(col1 was zero-filled)\n");
					else
						printf("  DIAG: P_actual is other "
						       "(computation mismatch?)\n");
				}
			}
			if (!dry_run) {
				if (write_stripe(p_disk, phys_off, p_buf) < 0) {
					errors++;
					continue;
				}
			}
		}

		/* Read current Q and compare */
		if (read_stripe(q_disk, phys_off, q_cur) < 0) {
			errors++;
			continue;
		}
		if (memcmp(q_buf, q_cur, HAMMER2_PBUFSIZE) != 0) {
			q_fixed++;
			if (q_fixed <= 5) {
				printf("Fixing Q: stripe %llu, q_disk=%d, "
				       "phys=0x%llx\n",
				       (unsigned long long)stripe_num,
				       q_disk,
				       (unsigned long long)phys_off);
				printf("  col0[0..7]: %02x %02x %02x %02x "
				       "%02x %02x %02x %02x\n",
				       col0_buf[0], col0_buf[1], col0_buf[2],
				       col0_buf[3], col0_buf[4], col0_buf[5],
				       col0_buf[6], col0_buf[7]);
				printf("  col1[0..7]: %02x %02x %02x %02x "
				       "%02x %02x %02x %02x\n",
				       col1_buf[0], col1_buf[1], col1_buf[2],
				       col1_buf[3], col1_buf[4], col1_buf[5],
				       col1_buf[6], col1_buf[7]);
				printf("  Q_exp[0..7]: %02x %02x %02x %02x "
				       "%02x %02x %02x %02x\n",
				       q_buf[0], q_buf[1], q_buf[2], q_buf[3],
				       q_buf[4], q_buf[5], q_buf[6], q_buf[7]);
				printf("  Q_act[0..7]: %02x %02x %02x %02x "
				       "%02x %02x %02x %02x\n",
				       q_cur[0], q_cur[1], q_cur[2], q_cur[3],
				       q_cur[4], q_cur[5], q_cur[6], q_cur[7]);
				/* Diagnose pattern */
				{
					int all_zero_act = 1;
					/* Q_exp_col0only = gf_mul2(0) XOR col0 = col0 */
					int act_eq_col0 = 1;
					size_t j;
					for (j = 0; j < HAMMER2_PBUFSIZE; j++) {
						if (q_cur[j] != 0)
							all_zero_act = 0;
						if (q_cur[j] != col0_buf[j])
							act_eq_col0 = 0;
					}
					if (all_zero_act)
						printf("  DIAG: Q_actual is all-zeros "
						       "(parity never written)\n");
					else if (act_eq_col0)
						printf("  DIAG: Q_actual == col0 "
						       "(col1 was zero-filled)\n");
					else
						printf("  DIAG: Q_actual is other "
						       "(computation mismatch?)\n");
				}
			}
			if (!dry_run) {
				if (write_stripe(q_disk, phys_off, q_buf) < 0) {
					errors++;
					continue;
				}
			}
		}
	}

	printf("\n=== Parity Repair Summary ===\n");
	printf("Stripes scanned:  %llu\n", (unsigned long long)num_stripes);
	printf("P stripes fixed:  %llu\n", (unsigned long long)p_fixed);
	printf("Q stripes fixed:  %llu\n", (unsigned long long)q_fixed);
	printf("I/O errors:       %llu\n", (unsigned long long)errors);

	for (i = 0; i < NDISKS; i++)
		close(fds[i]);

	return (errors > 0) ? 1 : 0;
}
