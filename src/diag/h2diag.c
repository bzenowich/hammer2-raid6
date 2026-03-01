/*
 * HAMMER2 RAID6 diagnostic: verify PFS inode checksums on disk.
 * Compile on DragonFlyBSD VM:
 *   cd /var/tmp && gcc -O0 -I/usr/src/sys/vfs/hammer2/xxhash \
 *       -I/usr/src/sys h2diag.c /usr/src/sys/vfs/hammer2/xxhash/xxhash.c -o h2diag
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* xxhash with HAMMER2 namespace prefix h2_ */
#define XXH_NAMESPACE h2_
#include "xxhash.h"

#define XXH_HAMMER2_SEED  0x4d617474446c6c6eULL

/* HAMMER2 constants */
#define HAMMER2_PBUFSIZE        65536ULL
#define HAMMER2_ZONE_SEG        (4ULL * 1024 * 1024)
#define HAMMER2_OFF_MASK_RADIX  0x3FULL
#define HAMMER2_OFF_MASK        0xFFFFFFFFFFFFFFC0ULL
#define HAMMER2_OFF_MASK_LO     (HAMMER2_OFF_MASK & 0xFFFFULL)   /* 0xFFC0 */

/* Byte offsets within a 128-byte blockref */
#define BREF_DATA_OFF_OFF       0x20   /* data_off field (uint64_t) */
#define BREF_CHECK_XXHASH64_OFF 0x40   /* check.xxhash64.value (uint64_t) */
#define BREF_SIZE               128

/* Byte offsets in volume header */
#define VOLHDR_SROOT_BSET_OFF   0x200  /* sroot_blockset */

/* Byte offset of u.blockset in hammer2_inode_data (after meta+filename) */
#define IDATA_BLOCKSET_OFF      0x200

#define HAMMER2_SET_COUNT       4
#define NDISKS                  4
#define NDATA                   2

/*
 * Current disk layout (vnconfig -l shows vn2 -> disk4.img after resilver):
 *   hmp->volumes[0] = vn0 = disk0.img
 *   hmp->volumes[1] = vn1 = disk1.img
 *   hmp->volumes[2] = vn2 = disk4.img  (resilvered replacement for disk2.img)
 *   hmp->volumes[3] = vn3 = disk3.img
 */
static const char *disks[NDISKS] = {
    "/build/var.tmp/disk0.img",
    "/build/var.tmp/disk1.img",
    "/build/var.tmp/disk4.img",
    "/build/var.tmp/disk3.img",
};

static int
read_at(const char *path, uint64_t offset, void *buf, size_t n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return -1; }
    if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
        perror("lseek"); close(fd); return -1;
    }
    ssize_t r = read(fd, buf, n);
    if (r != (ssize_t)n) {
        fprintf(stderr, "%s: short read %zd/%zu at 0x%llx\n",
                path, r, n, (unsigned long long)offset);
        close(fd); return -1;
    }
    close(fd);
    return 0;
}

static void
raid6_map(uint64_t pbase, int *di_out, uint64_t *po_out)
{
    uint64_t stripe_unit = HAMMER2_PBUFSIZE;
    uint64_t stripe_num  = pbase / ((uint64_t)NDATA * stripe_unit);
    int column   = (int)((pbase / stripe_unit) % NDATA);
    int p_disk   = (int)(stripe_num % NDISKS);
    int q_disk   = (p_disk + 1) % NDISKS;
    int di = 0, pd;

    for (pd = 0; pd < NDISKS; pd++) {
        if (pd == p_disk || pd == q_disk) continue;
        if (di == column) break;
        di++;
    }
    *di_out = pd;
    *po_out = HAMMER2_ZONE_SEG + stripe_num * stripe_unit + (pbase % stripe_unit);
}

static uint8_t volhdr[65536];
static uint8_t pbuf[65536];

int
main(void)
{
    uint32_t v32;
    int i, j;

    printf("=== HAMMER2 RAID6 Diagnostic ===\n\n");

    /* Step 1: read volume header from disk0 (root volume, not RAID6 striped) */
    if (read_at(disks[0], 0, volhdr, 65536) < 0) return 1;
    memcpy(&v32, volhdr, 4);
    printf("disk0 volume magic: 0x%08x  (expect 0xc0c1c0c1)\n\n", v32);

    /* Step 2: parse sroot blockref[0] from volume header */
    uint8_t *sbref = volhdr + VOLHDR_SROOT_BSET_OFF;
    uint64_t sroot_data_off, sroot_chk;
    memcpy(&sroot_data_off, sbref + BREF_DATA_OFF_OFF, 8);
    memcpy(&sroot_chk,      sbref + BREF_CHECK_XXHASH64_OFF, 8);
    printf("SUPROOT blockref:\n");
    printf("  data_off     = 0x%016llx\n", (unsigned long long)sroot_data_off);
    printf("  stored check = 0x%016llx\n", (unsigned long long)sroot_chk);

    /* Step 3: RAID6-map the 64KB block containing the SUPROOT */
    uint64_t lbase_s = sroot_data_off & ~HAMMER2_OFF_MASK_RADIX;
    uint64_t pbase_s = lbase_s & ~(HAMMER2_PBUFSIZE - 1);
    int di; uint64_t po;
    raid6_map(pbase_s, &di, &po);
    printf("  pbase = 0x%016llx -> disk%d (%s) phys = 0x%016llx\n\n",
           (unsigned long long)pbase_s, di, disks[di], (unsigned long long)po);

    /* Step 4: read the 64KB physical block */
    printf("Reading 64KB block from disk%d @ 0x%llx ...\n",
           di, (unsigned long long)po);
    if (read_at(disks[di], po, pbuf, 65536) < 0) return 1;

    /* Step 5: verify SUPROOT checksum */
    uint32_t sbuf_off = (uint32_t)(lbase_s & HAMMER2_OFF_MASK_LO);
    printf("SUPROOT at pbuf[0x%04x]\n", sbuf_off);
    printf("  first 16 bytes:");
    for (i = 0; i < 16; i++) printf(" %02x", pbuf[sbuf_off + i]);
    printf("\n");
    uint64_t sroot_computed = h2_XXH64(pbuf + sbuf_off, 1024, XXH_HAMMER2_SEED);
    printf("  stored=0x%016llx  computed=0x%016llx  %s\n\n",
           (unsigned long long)sroot_chk,
           (unsigned long long)sroot_computed,
           sroot_chk == sroot_computed ? "OK" : "MISMATCH!");

    /* Step 6: scan SUPROOT's u.blockset for PFS blockrefs */
    printf("SUPROOT u.blockset PFS entries:\n");
    uint8_t *bset = pbuf + sbuf_off + IDATA_BLOCKSET_OFF;
    static uint8_t pfs_buf[65536];

    for (i = 0; i < HAMMER2_SET_COUNT; i++) {
        uint8_t *br = bset + i * BREF_SIZE;
        uint8_t btype = br[0];
        uint64_t data_off, chk;
        if (btype == 0) continue;
        memcpy(&data_off, br + BREF_DATA_OFF_OFF, 8);
        memcpy(&chk,      br + BREF_CHECK_XXHASH64_OFF, 8);
        printf("  [%d] type=0x%02x data_off=0x%016llx stored_check=0x%016llx\n",
               i, btype, (unsigned long long)data_off, (unsigned long long)chk);

        /* RAID6-map the PFS inode's OWN 64KB block (may differ from SUPROOT's block) */
        uint64_t lb = data_off & ~HAMMER2_OFF_MASK_RADIX;
        uint64_t pfs_pbase = lb & ~(HAMMER2_PBUFSIZE - 1);
        uint32_t pfs_boff  = (uint32_t)(lb & HAMMER2_OFF_MASK_LO);
        int pfs_di; uint64_t pfs_po;
        raid6_map(pfs_pbase, &pfs_di, &pfs_po);
        printf("       pbase=0x%llx -> disk%d (%s) phys=0x%llx  buf_off=0x%04x\n",
               (unsigned long long)pfs_pbase, pfs_di, disks[pfs_di],
               (unsigned long long)pfs_po, pfs_boff);

        /* Is this PFS inode in the same physical block as SUPROOT? */
        uint8_t *use_buf;
        if (pfs_di == di && pfs_po == po) {
            printf("       (same physical block as SUPROOT — using cached pbuf)\n");
            use_buf = pbuf;
        } else {
            printf("       Reading PFS block from disk%d @ 0x%llx ...\n",
                   pfs_di, (unsigned long long)pfs_po);
            if (read_at(disks[pfs_di], pfs_po, pfs_buf, 65536) < 0) {
                printf("       READ FAILED\n"); continue;
            }
            use_buf = pfs_buf;
        }

        uint64_t computed = h2_XXH64(use_buf + pfs_boff, 1024, XXH_HAMMER2_SEED);
        printf("       stored =0x%016llx\n", (unsigned long long)chk);
        printf("       computed=0x%016llx  %s\n",
               (unsigned long long)computed,
               computed == chk ? "OK" : "MISMATCH!");
        printf("       first 16 bytes of PFS inode:");
        for (j = 0; j < 16; j++) printf(" %02x", use_buf[pfs_boff + j]);
        printf("\n");
    }

    return 0;
}
