/*
 * Copyright (c) 2024 The DragonFly Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * HAMMER2 RAID 6 - GF(2^8) Arithmetic and Syndrome Operations
 *
 * Implements dual-parity (P+Q) RAID using Reed-Solomon coding in GF(2^8)
 * with the irreducible polynomial x^8 + x^4 + x^3 + x^2 + 1 (0x11d).
 *
 * P = XOR of all data disks (same as RAID 5)
 * Q = sum of (data[i] * 2^i) in GF(2^8)
 *
 * This allows recovery from any combination of up to 2 disk failures.
 */

#include "hammer2.h"
#include "hammer2_raid6.h"

/*
 * GF(2^8) lookup tables - initialized by hammer2_raid6_init().
 */
uint8_t hammer2_gf_exp[256];		/* generator powers: 2^i mod poly */
uint8_t hammer2_gf_log[256];		/* discrete logarithm base 2 */
uint8_t hammer2_gf_inv[256];		/* multiplicative inverse */
uint8_t hammer2_gf_mul_table[256][256];	/* full multiplication table */

#ifdef __aarch64__
#include <sys/sysctl.h>
#include <machine/md_var.h>

#define HAMMER2_RAID6_SIMD_STEP	64

void hammer2_raid6_xor_neon(uint8_t *dst, const uint8_t *src, size_t bytes);
void hammer2_raid6_mul_neon(uint8_t *dst, const uint8_t *src, size_t bytes,
			    const uint8_t *tbl);
void hammer2_raid6_mulacc_neon(uint8_t *dst, const uint8_t *src,
			       size_t bytes, const uint8_t *tbl);

int hammer2_raid6_simd = 1;
static void hammer2_raid6_simd_selftest(void);
SYSCTL_DECL(_vfs_hammer2);
SYSCTL_INT(_vfs_hammer2, OID_AUTO, raid6_simd, CTLFLAG_RW,
	   &hammer2_raid6_simd, 0, "RAID6 parity uses NEON");
#endif

/*
 * Initialize all GF(2^8) lookup tables.
 * Must be called once at module load time.
 */
void
hammer2_raid6_init(void)
{
	int i, j;
	uint8_t x;

	/*
	 * Build exp and log tables.  The generator is 2.
	 * exp[i] = 2^i in GF(2^8), log[exp[i]] = i.
	 */
	x = 1;
	for (i = 0; i < 255; i++) {
		hammer2_gf_exp[i] = x;
		hammer2_gf_log[x] = (uint8_t)i;
		x = hammer2_gf_mul2(x);
	}
	/* log[0] is undefined, set to 0 for safety */
	hammer2_gf_log[0] = 0;
	/* exp[255] wraps to exp[0] = 1 */
	hammer2_gf_exp[255] = hammer2_gf_exp[0];

	/*
	 * Build inverse table.  inv[x] = x^254 = x^(-1) in GF(2^8).
	 * Since the multiplicative group has order 255, x^255 = 1,
	 * so x^(-1) = x^254 = 2^((log[x] * 254) mod 255).
	 */
	hammer2_gf_inv[0] = 0;	/* 0 has no inverse */
	for (i = 1; i < 256; i++) {
		/* inv[x] = exp[(255 - log[x]) % 255] */
		hammer2_gf_inv[i] = hammer2_gf_exp[255 - hammer2_gf_log[i]];
	}

	/*
	 * Build full multiplication table using exp/log.
	 * mul[a][b] = exp[(log[a] + log[b]) % 255] for a,b != 0.
	 */
	for (i = 0; i < 256; i++) {
		hammer2_gf_mul_table[i][0] = 0;
		hammer2_gf_mul_table[0][i] = 0;
	}
	for (i = 1; i < 256; i++) {
		for (j = 1; j < 256; j++) {
			int log_sum = (int)hammer2_gf_log[i] +
				      (int)hammer2_gf_log[j];
			if (log_sum >= 255)
				log_sum -= 255;
			hammer2_gf_mul_table[i][j] =
				hammer2_gf_exp[log_sum];
		}
	}
#ifdef __aarch64__
	hammer2_raid6_simd_selftest();
#endif
}

/*
 * Block primitives.  All parity math runs a column at a time on these:
 * dst ^= src, and dst = c * src or dst ^= c * src in GF(2^8).  On arm64
 * the bulk goes to the NEON loops in hammer2_raid6_neon.S, 64 bytes a
 * step, with the multiply done as two 16-entry table lookups (the
 * products of c with the low and the high nibble).  The kernel is built
 * without FP/SIMD, so each call brackets the NEON part with
 * kernel_fpu_begin()/kernel_fpu_end().  vfs.hammer2.raid6_simd=0 forces
 * the C loops.
 */
void
hammer2_raid6_xor(void *dstp, const void *srcp, size_t bytes)
{
	uint8_t *dst = dstp;
	const uint8_t *src = srcp;
	size_t i;

#ifdef __aarch64__
	if (hammer2_raid6_simd && bytes >= HAMMER2_RAID6_SIMD_STEP) {
		size_t n = bytes & ~(size_t)(HAMMER2_RAID6_SIMD_STEP - 1);

		kernel_fpu_begin();
		hammer2_raid6_xor_neon(dst, src, n);
		kernel_fpu_end();
		dst += n;
		src += n;
		bytes -= n;
	}
#endif
	if ((((uintptr_t)dst | (uintptr_t)src | bytes) & 7) == 0) {
		for (i = 0; i < bytes; i += 8)
			*(uint64_t *)(dst + i) ^= *(const uint64_t *)(src + i);
	} else {
		for (i = 0; i < bytes; i++)
			dst[i] ^= src[i];
	}
}

/*
 * dst = c * src (acc == 0) or dst ^= c * src (acc != 0).  dst may be
 * src.
 */
void
hammer2_raid6_mul(void *dstp, const void *srcp, size_t bytes, uint8_t c,
		  int acc)
{
	uint8_t *dst = dstp;
	const uint8_t *src = srcp;
	const uint8_t *t = hammer2_gf_mul_table[c];
	size_t i;

	if (c == 1) {
		if (acc)
			hammer2_raid6_xor(dst, src, bytes);
		else if (dst != src)
			bcopy(src, dst, bytes);
		return;
	}
#ifdef __aarch64__
	if (hammer2_raid6_simd && bytes >= HAMMER2_RAID6_SIMD_STEP) {
		size_t n = bytes & ~(size_t)(HAMMER2_RAID6_SIMD_STEP - 1);
		uint8_t tbl[32] __aligned(16);

		for (i = 0; i < 16; i++) {
			tbl[i] = t[i];
			tbl[16 + i] = t[i << 4];
		}
		kernel_fpu_begin();
		if (acc)
			hammer2_raid6_mulacc_neon(dst, src, n, tbl);
		else
			hammer2_raid6_mul_neon(dst, src, n, tbl);
		kernel_fpu_end();
		dst += n;
		src += n;
		bytes -= n;
	}
#endif
	if (acc) {
		for (i = 0; i < bytes; i++)
			dst[i] ^= t[src[i]];
	} else {
		for (i = 0; i < bytes; i++)
			dst[i] = t[src[i]];
	}
}

#ifdef __aarch64__
/*
 * Check the NEON loops against the C ones for every multiplier, with a
 * tail the C loop finishes.  A mismatch turns them off.
 */
static void
hammer2_raid6_simd_selftest(void)
{
	enum { N = 2 * HAMMER2_RAID6_SIMD_STEP + 7 };
	uint8_t *src, *ref, *dst;
	int c, i, acc, simd;

	src = kmalloc(3 * N, M_TEMP, M_WAITOK);
	ref = src + N;
	dst = ref + N;
	for (i = 0; i < N; i++)
		src[i] = (uint8_t)(i * 167 + 13);
	simd = hammer2_raid6_simd;
	for (c = 0; c < 256 && simd; c++) {
		for (acc = 0; acc < 3 && simd; acc++) {
			for (i = 0; i < N; i++)
				ref[i] = dst[i] = (uint8_t)(i ^ c);
			hammer2_raid6_simd = 0;
			if (acc == 2)
				hammer2_raid6_xor(ref, src, N);
			else
				hammer2_raid6_mul(ref, src, N, c, acc);
			hammer2_raid6_simd = 1;
			if (acc == 2)
				hammer2_raid6_xor(dst, src, N);
			else
				hammer2_raid6_mul(dst, src, N, c, acc);
			if (bcmp(ref, dst, N) != 0) {
				kprintf("hammer2: RAID6 NEON self-test failed "
					"(c=%d, %s), using C\n", c,
					acc == 2 ? "xor" : acc ? "mulacc" : "mul");
				simd = 0;
			}
		}
	}
	hammer2_raid6_simd = simd;
	kfree(src, M_TEMP);
}
#endif

/*
 * Generate P and Q syndromes for the given disk array.
 *
 * ptrs[0..ndisks-3] = data disk buffers
 * ptrs[ndisks-2]    = P parity buffer (output)
 * ptrs[ndisks-1]    = Q parity buffer (output)
 *
 * P = data[0] ^ data[1] ^ ... ^ data[ndata-1]
 * Q = data[0]*2^0 ^ data[1]*2^1 ^ ... ^ data[ndata-1]*2^(ndata-1)
 *
 * Column by column: P and Q start as copies of data[0], and each further
 * column is XORed into P and multiplied into Q.
 */
void
hammer2_raid6_gen_syndrome(int ndisks, size_t bytes, void **ptrs)
{
	int ndata = ndisks - 2;
	uint8_t *p = ptrs[ndisks - 2];
	uint8_t *q = ptrs[ndisks - 1];
	int z;

	bcopy(ptrs[0], p, bytes);
	bcopy(ptrs[0], q, bytes);		/* 2^0 = 1 */
	for (z = 1; z < ndata; z++) {
		hammer2_raid6_xor(p, ptrs[z], bytes);
		hammer2_raid6_mul(q, ptrs[z], bytes, hammer2_gf_exp[z], 1);
	}
}

/*
 * Recover two failed data disks using P and Q syndromes.
 *
 * faila < failb, both are data disk indices (0..ndata-1).
 *
 * Given:
 *   P_ok = XOR of surviving data = P ^ data[faila] ^ data[failb]
 *   Q_ok = Q syndrome of surviving data = Q ^ (data[faila]*2^faila)
 *                                            ^ (data[failb]*2^failb)
 *
 * We need to solve:
 *   data[faila] ^ data[failb] = P ^ P_ok           (equation from P)
 *   data[faila]*2^faila ^ data[failb]*2^failb = Q ^ Q_ok  (equation from Q)
 *
 * Let A = data[faila], B = data[failb]:
 *   A ^ B = Pxy
 *   A*2^faila ^ B*2^failb = Qxy
 *
 * Multiply first equation by 2^faila:
 *   A*2^faila ^ B*2^faila = Pxy*2^faila
 * XOR with second equation:
 *   B*(2^faila ^ 2^failb) = Pxy*2^faila ^ Qxy
 *   B = (Pxy*2^faila ^ Qxy) / (2^faila ^ 2^failb)
 *   A = Pxy ^ B
 */
void
hammer2_raid6_2data_recov(int ndisks, size_t bytes,
			  int faila, int failb, void **ptrs)
{
	int ndata = ndisks - 2;
	uint8_t *p = ptrs[ndisks - 2];
	uint8_t *q = ptrs[ndisks - 1];
	uint8_t *dpa, *dpb;
	uint8_t coeff_a, coeff_b, coeff_diff_inv;
	int z;

	dpa = ptrs[faila];
	dpb = ptrs[failb];

	/* Precompute coefficients */
	coeff_a = hammer2_gf_exp[faila];		/* 2^faila */
	coeff_b = hammer2_gf_exp[failb];		/* 2^failb */
	coeff_diff_inv = hammer2_gf_inv[coeff_a ^ coeff_b]; /* 1/(2^a ^ 2^b) */

	/*
	 * The failed columns serve as scratch.  dpa = Pxy = P ^ XOR of
	 * the surviving data, which equals data[faila] ^ data[failb].
	 * dpb = Qxy = Q ^ syndrome of the surviving data, which equals
	 * data[faila]*2^faila ^ data[failb]*2^failb.
	 */
	bcopy(p, dpa, bytes);
	bcopy(q, dpb, bytes);
	for (z = 0; z < ndata; z++) {
		if (z == faila || z == failb)
			continue;
		hammer2_raid6_xor(dpa, ptrs[z], bytes);
		hammer2_raid6_mul(dpb, ptrs[z], bytes, hammer2_gf_exp[z], 1);
	}

	/*
	 * Solve: B = (Pxy * 2^faila ^ Qxy) / (2^faila ^ 2^failb)
	 *          = Qxy * inv ^ Pxy * (2^faila * inv)
	 *        A = Pxy ^ B
	 */
	hammer2_raid6_mul(dpb, dpb, bytes, coeff_diff_inv, 0);
	hammer2_raid6_mul(dpb, dpa, bytes,
			  hammer2_gf_mul(coeff_a, coeff_diff_inv), 1);
	hammer2_raid6_xor(dpa, dpb, bytes);
}

/*
 * Recover one failed data disk + P parity using Q.
 *
 * faila = failed data disk index (0..ndata-1).
 * P is also bad/missing.
 *
 * From Q:
 *   Q = sum(data[i]*2^i)
 *   data[faila]*2^faila = Q ^ sum(surviving data[i]*2^i)
 *   data[faila] = (Q ^ sum(surviving data[i]*2^i)) / 2^faila
 *
 * Then recompute P from all data disks.
 */
void
hammer2_raid6_datap_recov(int ndisks, size_t bytes, int faila, void **ptrs)
{
	int ndata = ndisks - 2;
	uint8_t *p = ptrs[ndisks - 2];
	uint8_t *q = ptrs[ndisks - 1];
	uint8_t *dpa;
	int z;

	dpa = ptrs[faila];

	/* dpa = Qx = Q ^ syndrome of the surviving disks */
	bcopy(q, dpa, bytes);
	for (z = 0; z < ndata; z++) {
		if (z != faila)
			hammer2_raid6_mul(dpa, ptrs[z], bytes,
					  hammer2_gf_exp[z], 1);
	}

	/* data[faila] = Qx / 2^faila */
	hammer2_raid6_mul(dpa, dpa, bytes,
			  hammer2_gf_inv[hammer2_gf_exp[faila]], 0);

	/* Recompute P from all data disks */
	bcopy(ptrs[0], p, bytes);
	for (z = 1; z < ndata; z++)
		hammer2_raid6_xor(p, ptrs[z], bytes);
}

/*
 * Router: recover from 2 disk failures.
 *
 * faila, failb are logical column indices (0..ndisks-1).
 * Data columns are 0..ndata-1; P = ndata; Q = ndata+1.
 *
 * The dispatch logic expects: if one failure is a data column and the other
 * is a parity column, faila must be the DATA column.  Callers from the
 * resilver pass (failed_col, other_failed_col) which may violate this when
 * the resilvered disk holds P or Q for a given stripe.  Normalize here.
 */
void
hammer2_raid6_dual_recov(int ndisks, size_t bytes,
			 int faila, int failb, void **ptrs)
{
	int ndata = ndisks - 2;
	int tmp;

	/*
	 * Normalize: if faila is a parity column but failb is a data column,
	 * swap them.  The branches below rely on faila being the data column
	 * whenever exactly one failure is a data column.
	 */
	if (faila >= ndata && failb < ndata) {
		tmp = faila;
		faila = failb;
		failb = tmp;
	}

	if (faila >= ndata) {
		/*
		 * Both P and Q failed - just regenerate them from surviving
		 * data columns (which must be intact).
		 */
		hammer2_raid6_gen_syndrome(ndisks, bytes, ptrs);
	} else if (failb >= ndata) {
		if (failb == ndisks - 1) {
			/*
			 * One data disk + Q failed.
			 * Recover data from P, then regenerate Q.
			 *
			 * data[faila] = P ^ XOR(surviving data)
			 */
			uint8_t *dpa = ptrs[faila];
			int z;

			bcopy(ptrs[ndisks - 2], dpa, bytes);
			for (z = 0; z < ndata; z++) {
				if (z != faila)
					hammer2_raid6_xor(dpa, ptrs[z], bytes);
			}
			/* Regenerate Q */
			hammer2_raid6_gen_syndrome(ndisks, bytes, ptrs);
		} else {
			/*
			 * One data disk + P failed.
			 * Recover data from Q, then regenerate P.
			 */
			hammer2_raid6_datap_recov(ndisks, bytes, faila, ptrs);
		}
	} else {
		/*
		 * Two data disks failed.
		 */
		hammer2_raid6_2data_recov(ndisks, bytes, faila, failb, ptrs);
	}
}

/*
 * bwrite() in two halves, so that the writes of a row's P and Q columns,
 * or of a metadata block's mirror copies, are all in flight at once
 * instead of one disk after another.  The buffers stay in the buffer
 * cache (other readers of those device offsets see the new data).
 * Returns 0 if the buffer was B_INVAL and released without a write.
 */
int
hammer2_bwrite_start(struct buf *bp)
{
	if (bp->b_flags & B_INVAL) {
		brelse(bp);
		return 0;
	}
	bp->b_flags &= ~(B_ERROR | B_EINTR);
	bp->b_flags |= B_CACHE;
	bp->b_cmd = BUF_CMD_WRITE;
	bp->b_error = 0;
	bp->b_bio1.bio_done = biodone_sync;
	bp->b_bio1.bio_flags |= BIO_SYNC;
	vfs_busy_pages(bp->b_vp, bp);
	bsetrunningbufspace(bp, bp->b_bufsize);
	vn_strategy(bp->b_vp, &bp->b_bio1);
	return 1;
}

int
hammer2_bwrite_wait(struct buf *bp)
{
	int error;

	error = biowait(&bp->b_bio1, "h2bww");
	brelse(bp);
	return error;
}

/*
 * RAIDZ2-native (v3) parity write: compute P and Q from scratch over a
 * row of data columns and write all parity columns.
 *
 * Each row at per-disk offset phys_off can hold up to ndata independent
 * data columns (one per data-eligible disk).  Multi-column rows are 6C
 * packing; today's allocator gives one column per row so cols[]/ncols
 * is typically {single col, 1} but the API takes the row as a whole.
 *
 * COW invariant: a row is written exactly once, in one TXG.  All cols
 * that aren't in the cols[] array are zero on disk (slot just
 * allocated), so P/Q computed over `cols + zeros` is correct without
 * any prior reads.  No RMW.
 *
 * phys_off is the per-disk physical column offset for this row (same
 * on every disk).  Each col carries (disk_idx, data, bytes).  bytes
 * must equal stripe_unit for every column.
 *
 * Writes P and Q synchronously (bwrite); a past attempt at bawrite
 * here deadlocked the buffer cache under virtio-blk load (accumulation
 * outran completions).  Async P/Q is a follow-up perf task gated on
 * runningbufspace throttling.
 *
 * A failed P or Q write goes to hammer2_raid6_write_failed(), which
 * auto-fails that member.  Returns 0 if the row is written or degraded
 * within redundancy, ENXIO if a failure exceeded it.
 */
int
hammer2_io_raid6_write_row(hammer2_dev_t *hmp, hammer2_off_t phys_off,
			   const hammer2_row_col_t *cols, int ncols,
			   size_t bytes)
{
	hammer2_raid_config_t *rc = &hmp->raid_config;
	int ndisks = rc->ndisks;
	int ndata = rc->ndata;
	uint64_t stripe_unit = rc->stripe_unit;
	uint64_t stripe_slot;
	int p_disk, q_disk;
	hammer2_volume_t *vol;
	struct buf *pbp = NULL, *qbp = NULL;
	uint8_t *p_mem = NULL, *q_mem = NULL;
	uint8_t *p_dst = NULL;
	uint8_t *q_dst = NULL;
	int error = 0;
	int c, d;

	KKASSERT(hmp->raid_type == HAMMER2_RAID_TYPE_RAID6);
	KKASSERT(bytes == (size_t)stripe_unit);
	KKASSERT(ncols >= 1 && ncols <= ndata);

	stripe_slot = (phys_off - HAMMER2_ZONE_SEG64) / stripe_unit;
	p_disk      = (int)(stripe_slot % ndisks);
	q_disk      = (p_disk + 1) % ndisks;

	/*
	 * Allocate the P column buffer up-front and accumulate XOR into it.
	 * Skip the disk entirely if it's failed.  A disk being rebuilt
	 * gets its column through a private buffer and a non-blocking
	 * write (the caller holds rebuild_lk; see seal_row).
	 */
	if (hammer2_raid6_disk_writable(hmp, p_disk) &&
	    hmp->raid_failed[p_disk]) {
		p_mem = kmalloc(stripe_unit, M_HAMMER2, M_WAITOK | M_ZERO);
		p_dst = p_mem;
	} else if (!hmp->raid_failed[p_disk]) {
		vol = &hmp->volumes[p_disk];
		pbp = getblk(vol->dev->devvp, phys_off,
			     stripe_unit, GETBLK_KVABIO, 0);
		if (pbp) {
			bkvasync(pbp);
			bzero(pbp->b_data, stripe_unit);
			p_dst = (uint8_t *)pbp->b_data;
		}
	}

	if (hammer2_raid6_disk_writable(hmp, q_disk) &&
	    hmp->raid_failed[q_disk]) {
		q_mem = kmalloc(stripe_unit, M_HAMMER2, M_WAITOK | M_ZERO);
		q_dst = q_mem;
	} else if (!hmp->raid_failed[q_disk]) {
		vol = &hmp->volumes[q_disk];
		qbp = getblk(vol->dev->devvp, phys_off,
			     stripe_unit, GETBLK_KVABIO, 0);
		if (qbp) {
			bkvasync(qbp);
			bzero(qbp->b_data, stripe_unit);
			q_dst = (uint8_t *)qbp->b_data;
		}
	}

	/*
	 * Walk the cols[] array, accumulating P and Q.
	 * P[i] ^= col[i];   Q[i] ^= gf_mul(2^my_col, col[i])
	 * Unwritten data columns are implicitly zero in memory.  The
	 * caller (hammer2_raid6_seal_row_locked_to_unlocked) is
	 * responsible for zero-writing the disk locations of any
	 * truly-unused data cols *before* calling write_row, so parity
	 * reconstruction over the on-disk bytes agrees with the
	 * zero-padded math here.  See docs/DEVELOPER.md §6.
	 */
	for (c = 0; c < ncols; c++) {
		const hammer2_row_col_t *col = &cols[c];
		uint8_t *src = (uint8_t *)col->data;
		int my_col = 0;

		KKASSERT(col->disk_idx != p_disk &&
			 col->disk_idx != q_disk);

		for (d = 0; d < col->disk_idx; d++) {
			if (d != p_disk && d != q_disk)
				my_col++;
		}

		if (p_dst)
			hammer2_raid6_xor(p_dst, src, stripe_unit);
		if (q_dst) {
			hammer2_raid6_mul(q_dst, src, stripe_unit,
					  hammer2_gf_exp[my_col % 255], 1);
		}
	}

	/* P and Q go out together; the rebuild-target copies below too. */
	if (pbp && !hammer2_bwrite_start(pbp))
		pbp = NULL;
	if (qbp && !hammer2_bwrite_start(qbp))
		qbp = NULL;
	if (p_mem) {
		int e = hammer2_io_raid6_write_nowait(
		    hmp->volumes[p_disk].dev->devvp, phys_off,
		    p_mem, (int)stripe_unit);
		if (e)
			hammer2_raid6_write_failed(hmp, p_disk, e);
		kfree(p_mem, M_HAMMER2);
	}
	if (q_mem) {
		int e = hammer2_io_raid6_write_nowait(
		    hmp->volumes[q_disk].dev->devvp, phys_off,
		    q_mem, (int)stripe_unit);
		if (e)
			hammer2_raid6_write_failed(hmp, q_disk, e);
		kfree(q_mem, M_HAMMER2);
	}
	if (pbp) {
		int e = hammer2_bwrite_wait(pbp);
		if (e && (e = hammer2_raid6_write_failed(hmp, p_disk, e)) &&
		    !error)
			error = e;
	}
	if (qbp) {
		int e = hammer2_bwrite_wait(qbp);
		if (e && (e = hammer2_raid6_write_failed(hmp, q_disk, e)) &&
		    !error)
			error = e;
	}

	return error;
}

/*
 * Single-column convenience wrapper preserved for the current
 * one-chain-per-row putblk path.  6C-2 packing will convert call sites
 * to hammer2_io_raid6_write_row() directly.
 */
int
hammer2_io_raid6_write_scratch(hammer2_dev_t *hmp, hammer2_off_t pbase,
			       int data_disk_idx, void *data, size_t bytes)
{
	hammer2_row_col_t col = {
		.disk_idx = data_disk_idx,
		.data = data,
		.bytes = bytes,
	};
	hammer2_off_t phys_off = pbase & HAMMER2_RAID6_PHYS_MASK;

	return hammer2_io_raid6_write_row(hmp, phys_off, &col, 1, bytes);
}

/*
 * v3 RAIDZ2-native: synchronously mirror a metadata block to every
 * surviving disk at the same per-disk byte offset.  No parity — the
 * N-way mirror is its own redundancy.
 *
 * skip_disk_idx: if >= 0, don't write to this disk (the caller has
 * already issued the primary bdwrite/bwrite to it through the buffer
 * cache).  Pass -1 to mirror to every healthy disk.
 *
 * The copies are written in parallel: every healthy disk's write is
 * started, then all are waited for.  A disk being rebuilt gets the copy
 * too, written without waiting on its buffers, under rebuild_lk so the
 * resilver's metadata copy does not overwrite it with an older one.
 *
 * Returns 0 if at least one disk write succeeded; EIO if every other
 * disk was either failed or had an I/O error.
 */
int
hammer2_io_metadata_mirror_write(hammer2_dev_t *hmp, int skip_disk_idx,
				 hammer2_off_t per_disk_off,
				 void *data, size_t bytes)
{
	hammer2_volume_t *vol;
	struct buf *bp;
	struct buf *bps[HAMMER2_MAX_VOLUMES];
	int i;
	int ok = 0;
	int last_err = 0;
	int wrote_any = 0;
	int fatal = 0;

	KKASSERT(hmp->raid_type == HAMMER2_RAID_TYPE_RAID6);
	KKASSERT(hmp->voldata.version >= HAMMER2_VOL_VERSION_RAIDZ2);

	lockmgr(&hmp->rebuild_lk, LK_SHARED);
	for (i = 0; i < hmp->raid_config.ndisks; i++) {
		bps[i] = NULL;
		if (i == skip_disk_idx)
			continue;
		if (!hammer2_raid6_disk_writable(hmp, i))
			continue;
		vol = &hmp->volumes[i];
		if (vol->dev == NULL || vol->dev->devvp == NULL ||
		    !vol->dev->open)
			continue;

		if (hmp->raid_failed[i]) {
			/* rebuilding; its result does not count here */
			int e = hammer2_io_raid6_write_nowait(vol->dev->devvp,
			    per_disk_off, data, (int)bytes);
			if (e)
				hammer2_raid6_write_failed(hmp, i, e);
			continue;
		}

		bp = getblk(vol->dev->devvp, per_disk_off, bytes,
			    GETBLK_KVABIO, 0);
		if (bp == NULL) {
			last_err = ENOMEM;
			continue;
		}
		bkvasync(bp);
		bcopy(data, bp->b_data, bytes);
		if (hammer2_bwrite_start(bp)) {
			bps[i] = bp;
		} else {
			ok++;		/* B_INVAL: as bwrite() */
			wrote_any = 1;
		}
	}
	for (i = 0; i < hmp->raid_config.ndisks; i++) {
		int e;

		if (bps[i] == NULL)
			continue;
		e = hammer2_bwrite_wait(bps[i]);
		if (e) {
			int aerr;

			last_err = e;
			aerr = hammer2_raid6_auto_fail_disk(hmp, i);
			if (aerr && !fatal)
				fatal = aerr;	/* triple-failure */
			continue;
		}
		ok++;
		wrote_any = 1;
	}
	lockmgr(&hmp->rebuild_lk, LK_RELEASE);
	if (fatal)
		return fatal;

	if (skip_disk_idx >= 0)
		return ok ? 0 : (last_err ? last_err : EIO);
	return wrote_any ? 0 : (last_err ? last_err : EIO);
}

/*
 * v3 RAIDZ2-native: read a metadata block from the first surviving
 * mirror copy.  Used when the primary disk for a metadata DIO is
 * failed.  Iterates disks in index order, skipping skip_disk_idx and
 * any disk marked failed/closed, until one bread succeeds.
 *
 * Returns 0 on success and copies bytes into buf; EIO if no surviving
 * disk has a usable copy.
 */
int
hammer2_io_metadata_mirror_read(hammer2_dev_t *hmp, int skip_disk_idx,
				hammer2_off_t per_disk_off,
				void *buf, size_t bytes)
{
	hammer2_volume_t *vol;
	struct buf *bp;
	int i;
	int last_err = 0;

	KKASSERT(hmp->raid_type == HAMMER2_RAID_TYPE_RAID6);
	KKASSERT(hmp->voldata.version >= HAMMER2_VOL_VERSION_RAIDZ2);

	for (i = 0; i < hmp->raid_config.ndisks; i++) {
		if (i == skip_disk_idx)
			continue;
		if (hmp->raid_failed[i])
			continue;
		vol = &hmp->volumes[i];
		if (vol->dev == NULL || vol->dev->devvp == NULL ||
		    !vol->dev->open)
			continue;

		bp = NULL;
		last_err = hammer2_inject_eio(i);
		if (last_err == 0)
			last_err = bread(vol->dev->devvp, per_disk_off,
					 (int)bytes, &bp);
		if (last_err == 0 && bp) {
			bkvasync(bp);
			bcopy(bp->b_data, buf, bytes);
			brelse(bp);
			return 0;
		}
		if (bp)
			brelse(bp);
	}

	return last_err ? last_err : EIO;
}
