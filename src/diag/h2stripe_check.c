/*
 * h2stripe_check - HAMMER2 RAIDZ2-native (format version 4) stripe checker.
 *
 * Opens a HAMMER2 RAID6 array from raw device paths, reads the space map
 * (docs/capacity.md) committed by the newest volume header, and verifies
 * parity consistency for every allocated stripe slot.
 *
 * For each allocated slot:
 *   - Reads all data columns from their respective disks
 *   - Recomputes P (XOR) and Q (GF(2^8) syndrome) from the data columns
 *   - Reads the on-disk P and Q
 *   - Reports any mismatch
 *
 * Usage:
 *   h2stripe_check [-v] /dev/da0 /dev/da1 /dev/da2 ... /dev/daN
 *
 * Options:
 *   -v   Verbose: print per-stripe status for OK stripes too.
 *
 * The number of devices must match the ndisks value in the volume header.
 *
 * Compile on DragonFlyBSD:
 *   gcc -O2 -o h2stripe_check h2stripe_check.c
 *
 * The array must be UNMOUNTED when running this tool.
 *
 * Exit code: 0 if all checked stripes are consistent, 1 on any parity error
 * or read error.
 */

#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* -------------------------------------------------------------------------
 * HAMMER2 on-disk constants (replicated from hammer2_disk.h to avoid kernel
 * header dependency in user-space).
 * ------------------------------------------------------------------------- */

#define HAMMER2_PBUFSIZE		65536ULL	/* 64 KB: DIO / stripe unit */
#define HAMMER2_ZONE_SEG		(4ULL * 1024 * 1024)	/* 4 MB zone */
#define HAMMER2_ZONE_BYTES64		(2ULL * 1024 * 1024 * 1024) /* 2 GB zone */

/* Volume header copies: one at the start of each of the first 4 zones */
#define HAMMER2_NUM_VOLHDRS		4

/* Space map (docs/capacity.md) */
#define HAMMER2_RZ_LAYOUT		2
#define HAMMER2_SM_MAGIC		0x48324d5053325248ULL
#define HAMMER2_SM_PAGE_SLOTS		HAMMER2_PBUFSIZE

/* Volume header magic */
#define HAMMER2_VOLUME_ID_HBO		0x48414d3205172011ULL

/* Volume version where RAID6 config is valid (v3 = RAIDZ2-native). */
#define HAMMER2_VOL_VERSION_RAIDZ2	3

/* RAID type stored in hammer2_raid_config.raid_type */
#define HAMMER2_RAID_TYPE_RAID6		6

/* Per-disk state values */
#define HAMMER2_RAID6_DISK_ONLINE	0
#define HAMMER2_RAID6_DISK_FAILED	1
#define HAMMER2_RAID6_DISK_REBUILDING	2
#define HAMMER2_RAID6_DISK_SPARE	3
#define HAMMER2_RAID6_DISK_ABSENT	4

#define HAMMER2_MAX_VOLUMES		64

/* GF(2^8) primitive polynomial: x^8+x^4+x^3+x^2+1 (0x11d, mod-out = 0x1d) */
#define HAMMER2_RAID6_POLY		0x1d

/* -------------------------------------------------------------------------
 * Volume header layout (byte offsets, little-endian).
 * We read only what we need rather than mapping the full struct.
 * ------------------------------------------------------------------------- */

/* Byte offset of version field in the volume header sector 0 */
#define VOLHDR_VERSION_OFF	0x0030		/* uint32_t version */
#define VOLHDR_MAGIC_OFF	0x0000		/* uint64_t magic */
#define VOLHDR_MIRROR_TID_OFF	0x0078		/* uint64_t mirror_tid */

/*
 * Sector 3 of the volume header (bytes 0x0600..0x07FF) holds
 * hammer2_raid_config when version >= HAMMER2_VOL_VERSION_RAIDZ2.
 *
 * hammer2_raid_config layout (512 bytes total, __packed):
 *   uint8_t  raid_type;		+0x00
 *   uint8_t  ndisks;			+0x01
 *   uint8_t  ndata;			+0x02
 *   uint8_t  stripe_shift;		+0x03
 *   uint32_t flags;			+0x04
 *   uint64_t stripe_unit;		+0x08
 *   uint64_t array_size;		+0x10
 *   uint8_t  disk_state[64];		+0x18
 */
#define VOLHDR_RAID_CONFIG_OFF	0x0600

#define RAIDCFG_RAID_TYPE_OFF	(VOLHDR_RAID_CONFIG_OFF + 0x00)
#define RAIDCFG_NDISKS_OFF	(VOLHDR_RAID_CONFIG_OFF + 0x01)
#define RAIDCFG_NDATA_OFF	(VOLHDR_RAID_CONFIG_OFF + 0x02)
#define RAIDCFG_FLAGS_OFF	(VOLHDR_RAID_CONFIG_OFF + 0x04)	/* uint32_t */
#define RAIDCFG_STRIPE_UNIT_OFF	(VOLHDR_RAID_CONFIG_OFF + 0x08)	/* uint64_t */
#define RAIDCFG_DISK_STATE_OFF	(VOLHDR_RAID_CONFIG_OFF + 0x18)	/* uint8_t[64] */
#define RAIDCFG_RZ_LAYOUT_OFF	0x0700		/* uint32_t */
#define RAIDCFG_RZ_SM_PAGES_OFF	0x0704		/* uint32_t */
#define RAIDCFG_RZ_NUM_SLOTS_OFF 0x0708		/* uint64_t */
#define RAIDCFG_RZ_SM_OFF_OFF	0x0710		/* uint64_t */
#define RAIDCFG_RZ_SM_COPY_OFF	0x0718		/* uint64_t */
#define RAIDCFG_RZ_SM_GEN_OFF	0x0720		/* uint64_t */

/* struct hammer2_sm_header, page 0 of a space map copy */
#define SMHDR_MAGIC_OFF		0x00		/* uint64_t */
#define SMHDR_NUM_SLOTS_OFF	0x18		/* uint64_t */
#define SMHDR_GENERATION_OFF	0x20		/* uint64_t */
#define SMHDR_NPAGES_OFF	0x30		/* uint32_t */

/* -------------------------------------------------------------------------
 * GF(2^8) tables
 * ------------------------------------------------------------------------- */

static uint8_t gf_exp[256];
static uint8_t gf_log[256];
static uint8_t gf_mul_table[256][256];

static uint8_t
gf_mul2(uint8_t x)
{
	return (uint8_t)((x << 1) ^ ((x & 0x80) ? HAMMER2_RAID6_POLY : 0));
}

static void
gf_init(void)
{
	int i, j;
	uint8_t x;

	x = 1;
	for (i = 0; i < 255; i++) {
		gf_exp[i] = x;
		gf_log[x] = (uint8_t)i;
		x = gf_mul2(x);
	}
	gf_log[0]   = 0;
	gf_exp[255] = gf_exp[0]; /* wraps: 2^255 = 1 */

	/* Build multiplication table: gf_mul(a,b) = exp[(log[a]+log[b])%255] */
	for (i = 0; i < 256; i++) {
		gf_mul_table[0][i] = 0;
		gf_mul_table[i][0] = 0;
	}
	for (i = 1; i < 256; i++) {
		for (j = 1; j < 256; j++) {
			int ls = (int)gf_log[i] + (int)gf_log[j];
			if (ls >= 255)
				ls -= 255;
			gf_mul_table[i][j] = gf_exp[ls];
		}
	}
}

static inline uint8_t
gf_mul(uint8_t a, uint8_t b)
{
	return gf_mul_table[a][b];
}

/* -------------------------------------------------------------------------
 * Per-disk state
 * ------------------------------------------------------------------------- */

#define MAX_DISKS	HAMMER2_MAX_VOLUMES

static int	fds[MAX_DISKS];
static char    *disk_paths[MAX_DISKS];
static int	disk_state[MAX_DISKS];	/* HAMMER2_RAID6_DISK_* */
static int	ndisks_arg;		/* number of disk paths given on cmdline */

/* -------------------------------------------------------------------------
 * Low-level I/O helpers
 * ------------------------------------------------------------------------- */

/*
 * Read exactly 'len' bytes from disk fd[disk] at physical byte offset 'off'
 * into 'buf'.  Returns 0 on success, -1 on I/O error.
 */
static int
disk_read(int disk, uint64_t off, void *buf, size_t len)
{
	ssize_t r;

	if (fds[disk] < 0) {
		/* Disk was opened with error — treat as absent */
		return -1;
	}
	if (lseek(fds[disk], (off_t)off, SEEK_SET) < 0) {
		fprintf(stderr, "  lseek disk%d (%s) @ 0x%llx: %s\n",
		    disk, disk_paths[disk], (unsigned long long)off,
		    strerror(errno));
		return -1;
	}
	r = read(fds[disk], buf, len);
	if (r != (ssize_t)len) {
		fprintf(stderr, "  read disk%d (%s) @ 0x%llx: got %zd/%zu: %s\n",
		    disk, disk_paths[disk], (unsigned long long)off,
		    r, len, (r < 0) ? strerror(errno) : "short read");
		return -1;
	}
	return 0;
}

/* -------------------------------------------------------------------------
 * Volume header parsing
 * ------------------------------------------------------------------------- */

/*
 * Read the volume header from disk 0 (the root volume) and populate:
 *   *ndisks_out, *ndata_out, *stripe_unit_out, disk_state[]
 *
 * Returns 0 on success, -1 on failure.
 */
static uint8_t volhdr[HAMMER2_PBUFSIZE];

static int
read_volhdr(int *ndisks_out, int *ndata_out, uint64_t *stripe_unit_out)
{
	static uint8_t buf[HAMMER2_PBUFSIZE];
	uint64_t magic;
	uint64_t tid;
	uint64_t best_tid = 0;
	uint32_t version;
	uint8_t  raid_type, nd, ndat;
	uint64_t su;
	int      best = -1;
	int      i;

	/*
	 * Flushes rotate through the volume header copies; the one with
	 * the highest mirror_tid is current.
	 */
	for (i = 0; i < HAMMER2_NUM_VOLHDRS; i++) {
		if (fds[0] < 0 ||
		    pread(fds[0], buf, HAMMER2_PBUFSIZE,
			  (off_t)i * HAMMER2_ZONE_BYTES64) != HAMMER2_PBUFSIZE)
			break;
		memcpy(&magic, buf + VOLHDR_MAGIC_OFF, 8);
		memcpy(&tid, buf + VOLHDR_MIRROR_TID_OFF, 8);
		if (magic != HAMMER2_VOLUME_ID_HBO)
			continue;
		if (best < 0 || tid > best_tid) {
			best = i;
			best_tid = tid;
			memcpy(volhdr, buf, HAMMER2_PBUFSIZE);
		}
	}
	if (best < 0) {
		fprintf(stderr, "No valid volume header on disk 0\n");
		return -1;
	}

	memcpy(&version, volhdr + VOLHDR_VERSION_OFF,  4);

	if (version < HAMMER2_VOL_VERSION_RAIDZ2) {
		fprintf(stderr,
		    "Volume version %u < %u: not a RAID6 array\n",
		    version, HAMMER2_VOL_VERSION_RAIDZ2);
		return -1;
	}

	raid_type = volhdr[RAIDCFG_RAID_TYPE_OFF];
	if (raid_type != HAMMER2_RAID_TYPE_RAID6) {
		fprintf(stderr,
		    "raid_config.raid_type = %u, expected %u (RAID6)\n",
		    raid_type, HAMMER2_RAID_TYPE_RAID6);
		return -1;
	}

	nd   = volhdr[RAIDCFG_NDISKS_OFF];
	ndat = volhdr[RAIDCFG_NDATA_OFF];
	memcpy(&su, volhdr + RAIDCFG_STRIPE_UNIT_OFF, 8);

	if (nd < 4 || nd > MAX_DISKS) {
		fprintf(stderr, "ndisks=%u out of range [4..%d]\n",
		    nd, MAX_DISKS);
		return -1;
	}
	if (ndat != nd - 2) {
		fprintf(stderr,
		    "ndata=%u inconsistent with ndisks=%u (expected %u)\n",
		    ndat, nd, nd - 2);
		return -1;
	}
	if (su == 0 || (su & (su - 1)) != 0) {
		fprintf(stderr,
		    "stripe_unit=%llu is not a power of two\n",
		    (unsigned long long)su);
		return -1;
	}

	/* Copy per-disk state array */
	for (i = 0; i < nd; i++)
		disk_state[i] = volhdr[RAIDCFG_DISK_STATE_OFF + i];

	*ndisks_out      = (int)nd;
	*ndata_out       = (int)ndat;
	*stripe_unit_out = su;

	printf("Volume header %d: version=%u, ndisks=%u, ndata=%u, "
	    "stripe_unit=%llu bytes\n",
	    best, version, nd, ndat, (unsigned long long)su);

	for (i = 0; i < nd; i++) {
		const char *state_str;
		switch (disk_state[i]) {
		case HAMMER2_RAID6_DISK_ONLINE:     state_str = "ONLINE";     break;
		case HAMMER2_RAID6_DISK_FAILED:     state_str = "FAILED";     break;
		case HAMMER2_RAID6_DISK_REBUILDING: state_str = "REBUILDING"; break;
		case HAMMER2_RAID6_DISK_SPARE:      state_str = "SPARE";      break;
		case HAMMER2_RAID6_DISK_ABSENT:     state_str = "ABSENT";     break;
		default:                            state_str = "UNKNOWN";    break;
		}
		printf("  disk%d: %s (%s)\n", i,
		    (i < ndisks_arg) ? disk_paths[i] : "(not provided)",
		    state_str);
	}
	printf("\n");

	return 0;
}

/* -------------------------------------------------------------------------
 * Space map
 * ------------------------------------------------------------------------- */

/*
 * Read the space map copy committed by the volume header (generation
 * rz_sm_gen, copy rz_sm_gen & 1) from the first disk that has it.  Slot
 * S is allocated when its refcount byte is non-zero.  Page CRCs are not
 * checked: the kernel would have rebuilt a bad copy at mount.
 *
 * Returns a malloc'd refcount array (one byte per slot) or NULL.
 */
static uint8_t *
read_space_map(int ndisks, uint64_t *nslots_out)
{
	uint8_t  *hdr;
	uint8_t  *rc;
	uint32_t layout, npages, hpages;
	uint64_t nslots, sm_off, sm_copy, gen;
	uint64_t magic, hgen, hslots;
	uint64_t base;
	uint32_t pg;
	int      d;

	memcpy(&layout, volhdr + RAIDCFG_RZ_LAYOUT_OFF, 4);
	memcpy(&npages, volhdr + RAIDCFG_RZ_SM_PAGES_OFF, 4);
	memcpy(&nslots, volhdr + RAIDCFG_RZ_NUM_SLOTS_OFF, 8);
	memcpy(&sm_off, volhdr + RAIDCFG_RZ_SM_OFF_OFF, 8);
	memcpy(&sm_copy, volhdr + RAIDCFG_RZ_SM_COPY_OFF, 8);
	memcpy(&gen, volhdr + RAIDCFG_RZ_SM_GEN_OFF, 8);

	if (layout != HAMMER2_RZ_LAYOUT) {
		fprintf(stderr, "stripe layout %u, this tool reads %u\n",
		    layout, HAMMER2_RZ_LAYOUT);
		return NULL;
	}
	base = sm_off + (gen & 1) * sm_copy;
	hdr = malloc(HAMMER2_PBUFSIZE);
	rc = calloc(1, (size_t)npages * HAMMER2_SM_PAGE_SLOTS);
	if (hdr == NULL || rc == NULL) {
		fprintf(stderr, "malloc failed\n");
		free(hdr);
		free(rc);
		return NULL;
	}
	for (d = 0; d < ndisks; d++) {
		if (disk_state[d] != HAMMER2_RAID6_DISK_ONLINE ||
		    disk_read(d, base, hdr, HAMMER2_PBUFSIZE) < 0)
			continue;
		memcpy(&magic, hdr + SMHDR_MAGIC_OFF, 8);
		memcpy(&hslots, hdr + SMHDR_NUM_SLOTS_OFF, 8);
		memcpy(&hgen, hdr + SMHDR_GENERATION_OFF, 8);
		memcpy(&hpages, hdr + SMHDR_NPAGES_OFF, 4);
		if (magic != HAMMER2_SM_MAGIC || hgen != gen ||
		    hslots != nslots || hpages != npages)
			continue;
		for (pg = 0; pg < npages; pg++) {
			if (disk_read(d, base + (pg + 1) * HAMMER2_PBUFSIZE,
			    rc + (size_t)pg * HAMMER2_SM_PAGE_SLOTS,
			    HAMMER2_PBUFSIZE) < 0)
				break;
		}
		if (pg == npages)
			break;
	}
	free(hdr);
	if (d == ndisks) {
		fprintf(stderr, "No disk has space map generation %llu "
		    "(copy %llu @ 0x%llx)\n", (unsigned long long)gen,
		    (unsigned long long)(gen & 1), (unsigned long long)base);
		free(rc);
		return NULL;
	}
	printf("Space map: generation %llu from disk%d, %llu slots\n",
	    (unsigned long long)gen, d, (unsigned long long)nslots);
	*nslots_out = nslots;
	return rc;
}

/* -------------------------------------------------------------------------
 * Stripe column-to-disk mapping
 * ------------------------------------------------------------------------- */

/*
 * For stripe slot S with ndisks total disks:
 *   p_disk = S % ndisks
 *   q_disk = (p_disk + 1) % ndisks
 *   data columns (0..ndata-1): remaining disks in ascending disk-index order
 *
 * Fill col_disk[0..ndisks-1]:
 *   col_disk[c] for c in [0..ndata-1] = data disk for column c
 *   col_disk[ndata]   = p_disk
 *   col_disk[ndata+1] = q_disk
 *
 * This matches the kernel's enumeration in hammer2_io_raid6_write():
 *   di = 0;
 *   for (phys_disk = 0; phys_disk < ndisks; phys_disk++) {
 *       if (phys_disk == p_disk)     col = ndata;
 *       else if (phys_disk == q_disk) col = ndata + 1;
 *       else                          col = di++;
 *   }
 */
static void
stripe_col_map(uint64_t slot, int ndisks, int ndata,
    int *p_disk_out, int *q_disk_out, int col_disk[])
{
	int p_disk, q_disk;
	int phys_disk;
	int di;

	p_disk = (int)(slot % (uint64_t)ndisks);
	q_disk = (p_disk + 1) % ndisks;

	*p_disk_out = p_disk;
	*q_disk_out = q_disk;

	di = 0;
	for (phys_disk = 0; phys_disk < ndisks; phys_disk++) {
		int col;
		if (phys_disk == p_disk)
			col = ndata;
		else if (phys_disk == q_disk)
			col = ndata + 1;
		else
			col = di++;
		col_disk[col] = phys_disk;
	}
}

/* -------------------------------------------------------------------------
 * Syndrome computation (P and Q)
 * ------------------------------------------------------------------------- */

/*
 * Compute P (XOR) and Q (GF syndrome) for the given data columns.
 * ptrs[0..ndata-1]: data column buffers
 * p_out, q_out:     output buffers (each stripe_unit bytes)
 *
 * P[d] = XOR of ptrs[col][d] for col in [0..ndata-1]
 * Q[d] = XOR of gf_mul(gf_exp[col], ptrs[col][d]) for col in [0..ndata-1]
 *
 * This matches hammer2_raid6_gen_syndrome() in the kernel.
 */
static void
gen_syndrome(int ndata, size_t stripe_unit,
    uint8_t *ptrs[], uint8_t *p_out, uint8_t *q_out)
{
	size_t d;
	int    col;

	memset(p_out, 0, stripe_unit);
	memset(q_out, 0, stripe_unit);

	for (col = 0; col < ndata; col++) {
		uint8_t coeff = gf_exp[col]; /* 2^col */
		for (d = 0; d < stripe_unit; d++) {
			p_out[d] ^= ptrs[col][d];
			q_out[d] ^= gf_mul(coeff, ptrs[col][d]);
		}
	}
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

static void
usage(const char *prog)
{
	fprintf(stderr,
	    "Usage: %s [-v] /dev/disk0 /dev/disk1 ... /dev/diskN\n"
	    "  -v   Verbose: print per-stripe OK status too\n"
	    "  Disks must be listed in HAMMER2 volume order (disk 0 first).\n"
	    "  The array must be unmounted.\n",
	    prog);
}

int
main(int argc, char *argv[])
{
	int      ndisks, ndata;
	uint64_t stripe_unit;
	uint8_t *refcount;
	uint64_t max_stripes;
	uint64_t slot;

	/* Per-stripe working buffers — allocated once, reused each iteration */
	uint8_t **col_data  = NULL; /* col_data[col] = data column buffer */
	uint8_t  *p_computed = NULL;
	uint8_t  *q_computed = NULL;
	uint8_t  *p_ondisk   = NULL;
	uint8_t  *q_ondisk   = NULL;
	int      *col_disk   = NULL;

	uint64_t n_checked   = 0;
	uint64_t n_ok        = 0;
	uint64_t n_parity_err = 0;
	uint64_t n_read_err   = 0;
	uint64_t n_degraded   = 0;

	int verbose = 0;
	int argi    = 1;
	int i, col;
	int ret;

	/* Parse options */
	if (argi < argc && strcmp(argv[argi], "-v") == 0) {
		verbose = 1;
		argi++;
	}

	ndisks_arg = argc - argi;
	if (ndisks_arg < 4 || ndisks_arg > MAX_DISKS) {
		usage(argv[0]);
		return 1;
	}
	for (i = 0; i < ndisks_arg; i++)
		disk_paths[i] = argv[argi + i];

	/* Open all disks read-only */
	for (i = 0; i < ndisks_arg; i++) {
		fds[i] = open(disk_paths[i], O_RDONLY);
		if (fds[i] < 0) {
			fprintf(stderr, "open %s: %s\n",
			    disk_paths[i], strerror(errno));
			/*
			 * Do not abort — mark the disk as failed so we can
			 * still check degraded arrays.  The volume header read
			 * below will fail if disk 0 can't be opened.
			 */
		}
	}

	gf_init();

	/* Read volume header from disk 0 */
	if (read_volhdr(&ndisks, &ndata, &stripe_unit) < 0) {
		ret = 1;
		goto out_fds;
	}

	if (ndisks_arg != ndisks) {
		fprintf(stderr,
		    "Error: volume header says ndisks=%d but %d disk paths "
		    "were given on the command line.\n",
		    ndisks, ndisks_arg);
		ret = 1;
		goto out_fds;
	}

	/* Read the space map */
	refcount = read_space_map(ndisks, &max_stripes);
	if (refcount == NULL) {
		ret = 1;
		goto out_fds;
	}

	/* Count allocated slots for progress reporting */
	{
		uint64_t allocated = 0;
		for (slot = 0; slot < max_stripes; slot++) {
			if (refcount[slot] != 0)
				allocated++;
		}
		printf("Allocated stripe slots: %llu\n\n",
		    (unsigned long long)allocated);
	}

	/* Allocate per-stripe buffers */
	col_disk   = calloc(ndisks, sizeof(int));
	p_computed = malloc(stripe_unit);
	q_computed = malloc(stripe_unit);
	p_ondisk   = malloc(stripe_unit);
	q_ondisk   = malloc(stripe_unit);
	col_data   = calloc(ndisks, sizeof(uint8_t *));

	if (!col_disk || !p_computed || !q_computed ||
	    !p_ondisk  || !q_ondisk   || !col_data) {
		fprintf(stderr, "malloc failed\n");
		ret = 1;
		goto out_bufs;
	}

	/* Allocate per-column data buffers */
	for (col = 0; col < ndata; col++) {
		col_data[col] = malloc(stripe_unit);
		if (col_data[col] == NULL) {
			fprintf(stderr, "malloc(col_data[%d]) failed\n", col);
			ret = 1;
			goto out_bufs;
		}
	}

	/* ------------------------------------------------------------------ */
	/* Main scan loop: iterate over all possible stripe slots               */
	/* ------------------------------------------------------------------ */

	for (slot = 0; slot < max_stripes; slot++) {
		int    p_disk, q_disk;
		uint64_t phys_off;
		int    n_failed_data = 0;
		int    read_error    = 0;

		/* Skip unallocated slots */
		if (refcount[slot] == 0)
			continue;

		n_checked++;

		/* Physical byte offset on each disk for this stripe slot */
		phys_off = HAMMER2_ZONE_SEG + slot * stripe_unit;

		/* Compute which disk holds each column */
		stripe_col_map(slot, ndisks, ndata,
		    &p_disk, &q_disk, col_disk);

		/*
		 * Read data columns.  A disk that is absent/failed/not-opened
		 * produces a read error; we still attempt the check if enough
		 * columns are healthy (degrade gracefully).
		 */
		for (col = 0; col < ndata; col++) {
			int disk = col_disk[col];
			int ds   = (disk < ndisks_arg) ?
			    disk_state[disk] : HAMMER2_RAID6_DISK_ABSENT;

			if (ds != HAMMER2_RAID6_DISK_ONLINE ||
			    fds[disk] < 0) {
				/*
				 * Absent / failed disk: zero-fill so syndrome
				 * computation can still proceed on surviving
				 * columns.  Mark the column as bad.
				 */
				memset(col_data[col], 0, stripe_unit);
				n_failed_data++;
				continue;
			}

			if (disk_read(disk, phys_off,
			    col_data[col], stripe_unit) < 0) {
				read_error = 1;
				n_read_err++;
				break;
			}
		}

		if (read_error) {
			printf("SLOT %7llu  READ_ERROR (phys=0x%llx "
			    "p_disk=%d q_disk=%d)\n",
			    (unsigned long long)slot,
			    (unsigned long long)phys_off,
			    p_disk, q_disk);
			continue;
		}

		/*
		 * Read P and Q from their respective disks.  If either parity
		 * disk is failed/absent, we can still compute the syndrome but
		 * cannot compare — flag as degraded.
		 */
		int p_ok = 1, q_ok = 1;

		{
			int ds_p = (p_disk < ndisks_arg) ?
			    disk_state[p_disk] : HAMMER2_RAID6_DISK_ABSENT;
			if (ds_p != HAMMER2_RAID6_DISK_ONLINE ||
			    fds[p_disk] < 0) {
				p_ok = 0;
			} else if (disk_read(p_disk, phys_off,
			    p_ondisk, stripe_unit) < 0) {
				read_error = 1;
				n_read_err++;
			}
		}
		if (read_error) {
			printf("SLOT %7llu  READ_ERROR P-disk (phys=0x%llx "
			    "p_disk=%d q_disk=%d)\n",
			    (unsigned long long)slot,
			    (unsigned long long)phys_off,
			    p_disk, q_disk);
			continue;
		}

		{
			int ds_q = (q_disk < ndisks_arg) ?
			    disk_state[q_disk] : HAMMER2_RAID6_DISK_ABSENT;
			if (ds_q != HAMMER2_RAID6_DISK_ONLINE ||
			    fds[q_disk] < 0) {
				q_ok = 0;
			} else if (disk_read(q_disk, phys_off,
			    q_ondisk, stripe_unit) < 0) {
				read_error = 1;
				n_read_err++;
			}
		}
		if (read_error) {
			printf("SLOT %7llu  READ_ERROR Q-disk (phys=0x%llx "
			    "p_disk=%d q_disk=%d)\n",
			    (unsigned long long)slot,
			    (unsigned long long)phys_off,
			    p_disk, q_disk);
			continue;
		}

		/*
		 * If any data or parity column is on a failed disk we cannot
		 * fully verify parity — report as degraded and skip.
		 */
		if (n_failed_data > 0 || !p_ok || !q_ok) {
			n_degraded++;
			if (verbose) {
				printf("SLOT %7llu  DEGRADED  "
				    "(phys=0x%llx p_disk=%d q_disk=%d "
				    "failed_data_cols=%d p_avail=%d q_avail=%d)\n",
				    (unsigned long long)slot,
				    (unsigned long long)phys_off,
				    p_disk, q_disk,
				    n_failed_data, p_ok, q_ok);
			}
			continue;
		}

		/* Compute expected P and Q from the data columns */
		gen_syndrome(ndata, (size_t)stripe_unit,
		    (uint8_t **)col_data, p_computed, q_computed);

		/* Compare computed vs on-disk */
		int p_mismatch = memcmp(p_computed, p_ondisk, stripe_unit);
		int q_mismatch = memcmp(q_computed, q_ondisk, stripe_unit);

		if (p_mismatch || q_mismatch) {
			n_parity_err++;
			printf("SLOT %7llu  PARITY_ERROR  "
			    "(phys=0x%llx p_disk=%d q_disk=%d",
			    (unsigned long long)slot,
			    (unsigned long long)phys_off,
			    p_disk, q_disk);
			if (p_mismatch)
				printf(" P_MISMATCH");
			if (q_mismatch)
				printf(" Q_MISMATCH");
			printf(")\n");

			/* Print first 8 bytes of each column for diagnosis */
			if (verbose) {
				for (col = 0; col < ndata; col++) {
					printf("  data[%d][0..7]:", col);
					for (i = 0; i < 8; i++)
						printf(" %02x",
						    col_data[col][i]);
					printf("\n");
				}
				if (p_mismatch) {
					printf("  P_computed[0..7]:");
					for (i = 0; i < 8; i++)
						printf(" %02x",
						    p_computed[i]);
					printf("\n");
					printf("  P_ondisk  [0..7]:");
					for (i = 0; i < 8; i++)
						printf(" %02x",
						    p_ondisk[i]);
					printf("\n");
				}
				if (q_mismatch) {
					printf("  Q_computed[0..7]:");
					for (i = 0; i < 8; i++)
						printf(" %02x",
						    q_computed[i]);
					printf("\n");
					printf("  Q_ondisk  [0..7]:");
					for (i = 0; i < 8; i++)
						printf(" %02x",
						    q_ondisk[i]);
					printf("\n");
				}
			}
		} else {
			n_ok++;
			if (verbose) {
				printf("SLOT %7llu  OK  "
				    "(phys=0x%llx p_disk=%d q_disk=%d)\n",
				    (unsigned long long)slot,
				    (unsigned long long)phys_off,
				    p_disk, q_disk);
			}
		}
	}

	/* ------------------------------------------------------------------ */
	/* Summary                                                              */
	/* ------------------------------------------------------------------ */

	printf("\n=== h2stripe_check Summary ===\n");
	printf("Allocated slots checked : %llu\n",
	    (unsigned long long)n_checked);
	printf("OK                      : %llu\n",
	    (unsigned long long)n_ok);
	printf("Parity errors           : %llu\n",
	    (unsigned long long)n_parity_err);
	printf("Read errors             : %llu\n",
	    (unsigned long long)n_read_err);
	printf("Degraded (skipped)      : %llu\n",
	    (unsigned long long)n_degraded);

	ret = (n_parity_err > 0 || n_read_err > 0) ? 1 : 0;

out_bufs:
	if (col_data) {
		for (col = 0; col < ndata; col++)
			free(col_data[col]);
		free(col_data);
	}
	free(col_disk);
	free(p_computed);
	free(q_computed);
	free(p_ondisk);
	free(q_ondisk);
	free(refcount);

out_fds:
	for (i = 0; i < ndisks_arg; i++) {
		if (fds[i] >= 0)
			close(fds[i]);
	}

	return ret;
}
