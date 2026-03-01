/*
 * HAMMER2 RAID6 parity diagnostic:
 *   - Reads disk0 (P), disk2_original (col0), disk3 (col1) for stripe 1056
 *   - Computes expected P = col0 XOR col1
 *   - Compares expected vs actual P and reports first mismatches
 *   - Also computes and checks the PFS inode checksum
 *
 * Compile on DragonFlyBSD VM:
 *   cd /var/tmp && gcc -O0 -I/usr/src/sys/vfs/hammer2/xxhash \
 *       -I/usr/src/sys h2diag2.c /usr/src/sys/vfs/hammer2/xxhash/xxhash.c -o h2diag2
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define XXH_NAMESPACE h2_
#include "xxhash.h"

#define XXH_HAMMER2_SEED  0x4d617474446c6c6eULL

#define HAMMER2_PBUFSIZE        65536ULL
#define HAMMER2_ZONE_SEG        (4ULL * 1024 * 1024)
#define NDISKS                  4
#define NDATA                   2

/* Stripe 1056: pbase=0x8400000 holds LOCAL PFS inode at buf_off=0x400 */
#define STRIPE_NUM              1056
#define INODE_BUF_OFF           0x400
#define INODE_BYTES             1024

/* Physical offset for stripe 1056 on each disk */
#define PHYS_OFF   (HAMMER2_ZONE_SEG + (uint64_t)STRIPE_NUM * HAMMER2_PBUFSIZE)

/* Disk images */
static const char *disk0 = "/build/var.tmp/disk0.img";    /* P parity */
static const char *disk1 = "/build/var.tmp/disk1.img";    /* Q parity */
static const char *disk2 = "/build/var.tmp/disk2.img";    /* col0 ORIGINAL (not mounted) */
static const char *disk3 = "/build/var.tmp/disk3.img";    /* col1 */
static const char *disk4 = "/build/var.tmp/disk4.img";    /* col0 RESILVERED (mounted as vn2) */

static int
read_at(const char *path, uint64_t offset, void *buf, size_t n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return -1; }
    if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
        perror("lseek"); close(fd); return -1;
    }
    ssize_t r = read(fd, buf, n);
    close(fd);
    if (r != (ssize_t)n) {
        fprintf(stderr, "%s: short read %zd/%zu at 0x%llx\n",
                path, r, n, (unsigned long long)offset);
        return -1;
    }
    return 0;
}

static void
hexdump(const char *label, const uint8_t *buf, size_t n)
{
    size_t i;
    printf("%s:", label);
    for (i = 0; i < n; i++) printf(" %02x", buf[i]);
    printf("\n");
}

int
main(void)
{
    static uint8_t p_buf[HAMMER2_PBUFSIZE];    /* disk0 P actual */
    static uint8_t q_buf[HAMMER2_PBUFSIZE];    /* disk1 Q actual */
    static uint8_t col0_orig[HAMMER2_PBUFSIZE]; /* disk2 col0 original */
    static uint8_t col0_resil[HAMMER2_PBUFSIZE];/* disk4 col0 resilvered */
    static uint8_t col1_buf[HAMMER2_PBUFSIZE];  /* disk3 col1 */
    uint64_t phys_off = PHYS_OFF;
    int i, mismatches;

    printf("=== HAMMER2 RAID6 Parity Diagnostic ===\n");
    printf("Stripe %d, phys_off=0x%llx\n\n", STRIPE_NUM, (unsigned long long)phys_off);

    /* Read all five disk images at stripe 1056 */
    printf("Reading disk images at phys 0x%llx...\n", (unsigned long long)phys_off);
    if (read_at(disk0, phys_off, p_buf,     HAMMER2_PBUFSIZE) < 0) return 1;
    if (read_at(disk1, phys_off, q_buf,     HAMMER2_PBUFSIZE) < 0) return 1;
    if (read_at(disk2, phys_off, col0_orig, HAMMER2_PBUFSIZE) < 0) return 1;
    if (read_at(disk4, phys_off, col0_resil,HAMMER2_PBUFSIZE) < 0) return 1;
    if (read_at(disk3, phys_off, col1_buf,  HAMMER2_PBUFSIZE) < 0) return 1;
    printf("  OK\n\n");

    /* Show first 32 bytes of each disk at inode offset */
    printf("--- First 32 bytes at inode offset (0x%x within 64KB block) ---\n", INODE_BUF_OFF);
    hexdump("disk0 P      ", p_buf     + INODE_BUF_OFF, 32);
    hexdump("disk1 Q      ", q_buf     + INODE_BUF_OFF, 32);
    hexdump("disk2 col0   ", col0_orig + INODE_BUF_OFF, 32);
    hexdump("disk3 col1   ", col1_buf  + INODE_BUF_OFF, 32);
    hexdump("disk4 resilvd", col0_resil+ INODE_BUF_OFF, 32);

    /* Compute expected P = col0 XOR col1 */
    uint8_t expected_p[32];
    for (i = 0; i < 32; i++)
        expected_p[i] = col0_orig[INODE_BUF_OFF + i] ^ col1_buf[INODE_BUF_OFF + i];
    hexdump("expected P   ", expected_p, 32);
    printf("\n");

    /* Compare actual P vs expected P over the full 64KB block */
    printf("--- Full 64KB P parity check (disk0 vs col0 XOR col1) ---\n");
    mismatches = 0;
    for (i = 0; i < (int)HAMMER2_PBUFSIZE; i++) {
        uint8_t exp = col0_orig[i] ^ col1_buf[i];
        if (p_buf[i] != exp) {
            if (mismatches < 20) {
                printf("  MISMATCH at block_off=0x%04x: actual_P=0x%02x expected=0x%02x "
                       "(col0=0x%02x col1=0x%02x)\n",
                       i, p_buf[i], exp, col0_orig[i], col1_buf[i]);
            }
            mismatches++;
        }
    }
    if (mismatches == 0) {
        printf("  ALL BYTES MATCH - P parity is correct!\n");
    } else {
        printf("  Total mismatches: %d / %llu bytes\n", mismatches, (unsigned long long)HAMMER2_PBUFSIZE);
    }
    printf("\n");

    /* Check inode hash from disk2 (original col0) */
    {
        uint64_t computed = h2_XXH64(col0_orig + INODE_BUF_OFF, INODE_BYTES, XXH_HAMMER2_SEED);
        printf("--- PFS inode hash from disk2 (original col0) ---\n");
        printf("  computed hash: 0x%016llx\n", (unsigned long long)computed);
        printf("  Note: stored hash in blockref = 0x0e9b264a97ce8215 (from h2diag run)\n");
        printf("  Match: %s\n\n", computed == 0x0e9b264a97ce8215ULL ? "YES" : "NO");
    }

    /* Check inode hash from disk4 (resilvered col0) */
    {
        uint64_t computed = h2_XXH64(col0_resil + INODE_BUF_OFF, INODE_BYTES, XXH_HAMMER2_SEED);
        printf("--- PFS inode hash from disk4 (resilvered col0) ---\n");
        printf("  computed hash: 0x%016llx\n", (unsigned long long)computed);
        printf("  Match: %s\n\n", computed == 0x0e9b264a97ce8215ULL ? "YES" : "NO");
    }

    /* Check: does disk4 == P XOR col1? (should always be true if resilver was correct) */
    {
        int bad = 0;
        for (i = 0; i < (int)HAMMER2_PBUFSIZE; i++) {
            uint8_t reconstructed = p_buf[i] ^ col1_buf[i];
            if (col0_resil[i] != reconstructed) { bad++; break; }
        }
        printf("--- Resilver check: disk4 == disk0 XOR disk3? ---\n");
        printf("  %s\n\n", bad == 0 ? "YES (resilver was mathematically consistent)" :
               "NO (resilver was inconsistent)");
    }

    /* Show first 32 bytes of each disk at block offset 0 */
    printf("--- First 32 bytes at block start (offset 0x0000) ---\n");
    hexdump("disk0 P   ", p_buf,      32);
    hexdump("disk1 Q   ", q_buf,      32);
    hexdump("disk2 col0", col0_orig,  32);
    hexdump("disk3 col1", col1_buf,   32);
    hexdump("disk4 rsil", col0_resil, 32);
    printf("\n");

    /* Find first non-zero region in each buffer */
    {
        int first_nz0 = -1, first_nz1 = -1, first_nz2 = -1, first_nz3 = -1;
        for (i = 0; i < (int)HAMMER2_PBUFSIZE; i++) {
            if (first_nz0 < 0 && p_buf[i] != 0) first_nz0 = i;
            if (first_nz1 < 0 && col0_orig[i] != 0) first_nz1 = i;
            if (first_nz2 < 0 && col1_buf[i] != 0) first_nz2 = i;
            if (first_nz3 < 0 && col0_resil[i] != 0) first_nz3 = i;
        }
        printf("--- First non-zero byte offset in each 64KB block ---\n");
        printf("  disk0 P:        0x%04x\n", first_nz0 >= 0 ? first_nz0 : -1);
        printf("  disk2 col0_orig:0x%04x\n", first_nz1 >= 0 ? first_nz1 : -1);
        printf("  disk3 col1:     0x%04x\n", first_nz2 >= 0 ? first_nz2 : -1);
        printf("  disk4 resilverd:0x%04x\n", first_nz3 >= 0 ? first_nz3 : -1);
        printf("\n");
    }

    return 0;
}
