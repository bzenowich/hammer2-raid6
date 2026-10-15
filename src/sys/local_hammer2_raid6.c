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

#include <sys/kthread.h>
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
 * Timed waits for the explicit bios sent to a member (R11).
 *
 * A hung drive never completes a command, and nothing can take a bio
 * back from a driver: the buffer must stay owned until the driver
 * returns it.  A waiter that gives up (ETIMEDOUT) therefore hands the
 * buffer over to the completion, which releases it whenever the driver
 * finally returns it; the waiter must not touch it again.  bio_caller_info3
 * is the hand-over: 0 running, 1 completed (the waiter releases), 2
 * abandoned (the completion releases).  bio_caller_info2 says whether the
 * buffer is a pbuf (else a buffer cache buffer).
 */
/* Test-only: complete a masked disk's timed bio late (inject_hold_ms). */
static void hammer2_biodone_timed(struct bio *bio);

static void
hammer2_hold_thread(void *arg)
{
	struct bio *bio = arg;

	tsleep(bio, 0, "h2hold", hammer2_inject_hold_ms * hz / 1000 + 1);
	hammer2_biodone_timed(bio);
	kthread_exit();
}

static void
hammer2_biodone_timed(struct bio *bio)
{
	struct buf *bp = bio->bio_buf;
	int pbuf = bio->bio_caller_info2.index;
	int di = bio->bio_caller_info1.index;

	if (di >= 0 && di < 32 && hammer2_inject_hold_ms > 0 &&
	    (hammer2_inject_hold_ms_disk_mask & (1U << di))) {
		bio->bio_caller_info1.index = -1;
		kthread_create(hammer2_hold_thread, bio, NULL, "h2hold");
		return;
	}

	if (atomic_cmpset_int(&bio->bio_caller_info3.value, 0, 1)) {
		biodone_sync(bio);
		return;
	}
	/*
	 * Abandoned by its waiter.  biowait() would have cleared BIO_DONE
	 * and BIO_SYNC; nobody will now, and the buffer (or pbuf) is
	 * recycled with b_bio1.bio_flags as it is left, so a stale
	 * BIO_DONE would trip the assertion in the next vn_strategy().
	 */
	biodone_sync(bio);
	atomic_clear_int(&bio->bio_flags, BIO_DONE | BIO_SYNC);
	if (pbuf) {
		relpbuf(bp, NULL);
	} else {
		bp->b_flags |= B_INVAL | B_NOCACHE;
		brelse(bp);
	}
}

void
hammer2_bio_timed_init(struct buf *bp, int pbuf, int disk_idx)
{
	/*
	 * The completion may release the buffer from the driver's thread,
	 * and lockmgr refuses a release by anyone but the holder: hand the
	 * buffer lock to the kernel, as an async write does.
	 */
	BUF_KERNPROC(bp);
	bp->b_bio1.bio_done = hammer2_biodone_timed;
	bp->b_bio1.bio_flags |= BIO_SYNC;
	bp->b_bio1.bio_caller_info1.index = disk_idx;
	bp->b_bio1.bio_caller_info2.index = pbuf;
	bp->b_bio1.bio_caller_info3.value = 0;
}

/*
 * Wait for a bio set up by hammer2_bio_timed_init(), at most
 * vfs.hammer2.io_timeout_ms.  Returns ETIMEDOUT if the waiter gave up, in
 * which case the buffer belongs to the completion and must not be
 * released.  Otherwise returns the I/O error, and the caller releases the
 * buffer.
 */
int
hammer2_biowait_timed(struct buf *bp, const char *wmesg)
{
	struct bio *bio = &bp->b_bio1;
	int to = hammer2_io_timeout_ms;
	int error;

	if (to <= 0)
		return biowait(bio, wmesg);
	error = biowait_timeout(bio, wmesg, (int)((int64_t)to * hz / 1000) + 1);
	if (error == EWOULDBLOCK &&
	    atomic_cmpset_int(&bio->bio_caller_info3.value, 0, 2)) {
		kprintf("hammer2: I/O to a member did not complete in %d ms; "
			"abandoned\n", to);
		return ETIMEDOUT;
	}
	if (error == EWOULDBLOCK) {
		/* the completion won the race, and is about to finish */
		error = biowait(bio, wmesg);
	}
	return error;
}

/*
 * bwrite() in two halves, so that the writes of a row's P and Q columns,
 * or of a metadata block's mirror copies, are all in flight at once
 * instead of one disk after another.  The buffers stay in the buffer
 * cache (other readers of those device offsets see the new data) unless
 * the caller set B_NOCACHE, which drops them at the wait.  Returns 0 if the buffer was B_INVAL and released without a write.
 */
int
hammer2_bwrite_start(struct buf *bp, int disk_idx)
{
	if (bp->b_flags & B_INVAL) {
		brelse(bp);
		return 0;
	}
	bp->b_flags &= ~(B_ERROR | B_EINTR);
	bp->b_flags |= B_CACHE;
	bp->b_cmd = BUF_CMD_WRITE;
	bp->b_error = 0;
	hammer2_bio_timed_init(bp, 0, disk_idx);
	vfs_busy_pages(bp->b_vp, bp);
	bsetrunningbufspace(bp, bp->b_bufsize);
	hammer2_inject_delay(disk_idx);
	if (hammer2_inject_wio(disk_idx)) {
		/* complete as a failed write, as a driver would */
		bp->b_error = EIO;
		bp->b_flags |= B_ERROR;
		bp->b_resid = bp->b_bcount;
		biodone(&bp->b_bio1);
		return 1;
	}
	vn_strategy(bp->b_vp, &bp->b_bio1);
	return 1;
}

/*
 * Returns ETIMEDOUT if the write did not complete in time; the buffer
 * is then not ours any more (see hammer2_biowait_timed).
 */
int
hammer2_bwrite_wait(struct buf *bp)
{
	int error;

	error = hammer2_biowait_timed(bp, "h2bww");
	if (error != ETIMEDOUT)
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
 *
 * pe != NULL (async seal): the P and Q writes are started (or, for
 * the test hook, left for the reaper to start) and added to pe, which
 * the caller hands to the reaper; nothing is waited for and 0 is
 * returned.
 */
int
hammer2_io_raid6_write_row(hammer2_dev_t *hmp, hammer2_off_t phys_off,
			   const hammer2_row_col_t *cols, int ncols,
			   size_t bytes, struct hammer2_pq_ent *pe)
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
	struct timespec ts0, ts1;
	int error = 0;
	int c;

	KKASSERT(hmp->raid_type == HAMMER2_RAID_TYPE_RAID6);
	KKASSERT(bytes == (size_t)stripe_unit);
	KKASSERT(ncols >= 1 && ncols <= ndata);

	stripe_slot = hammer2_raid6_slot(phys_off, stripe_unit);
	hammer2_raid6_pq(stripe_slot, ndisks, &p_disk, &q_disk);

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
		int my_col;

		KKASSERT(col->disk_idx != p_disk &&
			 col->disk_idx != q_disk);

		my_col = hammer2_raid6_lcol(col->disk_idx, p_disk, q_disk,
					    ndata);

		if (p_dst)
			hammer2_raid6_xor(p_dst, src, stripe_unit);
		if (q_dst) {
			hammer2_raid6_mul(q_dst, src, stripe_unit,
					  hammer2_gf_exp[my_col % 255], 1);
		}
	}

	/* P and Q go out together; the rebuild-target copies below too. */
	if (pbp || p_mem)
		HAMMER2_RSTAT_WR(hmp, p_disk, HAMMER2_RSC_P, stripe_unit);
	if (qbp || q_mem)
		HAMMER2_RSTAT_WR(hmp, q_disk, HAMMER2_RSC_Q, stripe_unit);
	if (pbp) {
		/* parity is never read back on the normal path: drop it */
		pbp->b_flags |= B_RELBUF | B_NOCACHE;
		if (pe && pe->defer)
			BUF_KERNPROC(pbp);
		else if (!hammer2_bwrite_start(pbp, p_disk))
			pbp = NULL;
	}
	if (qbp) {
		qbp->b_flags |= B_RELBUF | B_NOCACHE;
		if (pe && pe->defer)
			BUF_KERNPROC(qbp);
		else if (!hammer2_bwrite_start(qbp, q_disk))
			qbp = NULL;
	}
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
	if (pe) {
		if (pbp)
			hammer2_pq_add(pe, pbp, p_disk);
		if (qbp)
			hammer2_pq_add(pe, qbp, q_disk);
		return 0;
	}
	nanouptime(&ts0);
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
	nanouptime(&ts1);
	atomic_add_long(&hammer2_rz_pq_wait_ns,
	    (long)((ts1.tv_sec - ts0.tv_sec) * 1000000000L +
		   (ts1.tv_nsec - ts0.tv_nsec)));
	atomic_add_long(&hammer2_rz_pq_wait_cnt, 1);

	return error;
}

/*
 * Async seal (oct-review P3 phase 2).  See the design there; in short:
 * the writer registers a hammer2_pq_ent for the row under rebuild_lk
 * shared, starts the P/Q/zero-column writes, and hands the entry to the
 * reaper kthread, which waits for them (timed, R11), releases the
 * buffers, fails a member whose write failed, and finally drops the
 * row's rz_sealing reference.  Entries complete in registration order.
 */
void
hammer2_pq_add(struct hammer2_pq_ent *pe, struct buf *bp, int disk)
{
	KKASSERT(pe->nbuf < HAMMER2_PQ_MAXBUFS);
	pe->bp[pe->nbuf] = bp;
	pe->disk[pe->nbuf] = disk;
	pe->nbuf++;
}

/*
 * Hold the writer back while async_seal_max rows are in flight.  Before
 * rebuild_lk is taken, so a sleeping writer does not hold it.
 */
void
hammer2_pq_throttle(hammer2_dev_t *hmp)
{
	while (hmp->pq_count >= hammer2_async_seal_max && hmp->pq_running) {
		tsleep_interlock(__DEVOLATILE(void *, &hmp->pq_done_seq), 0);
		if (hmp->pq_count < hammer2_async_seal_max)
			break;
		hammer2_rz_pq_bp_wait++;
		tsleep(__DEVOLATILE(void *, &hmp->pq_done_seq), PINTERLOCKED,
		       "h2pqbp", hz);
	}
}

/*
 * Called before the row's parity work, with rebuild_lk held shared.  The
 * rz_sealing reference of the row passes to the entry.
 */
struct hammer2_pq_ent *
hammer2_pq_register(hammer2_dev_t *hmp, hammer2_off_t phys_off)
{
	struct hammer2_pq_ent *pe;

	pe = kmalloc(sizeof(*pe), M_HAMMER2, M_WAITOK | M_ZERO);
	pe->phys_off = phys_off;
	pe->defer = hammer2_inject_pq_defer_ms > 0;
	pe->tick = ticks;
	hammer2_spin_ex(&hmp->pq_spin);
	pe->seq = ++hmp->pq_seq;
	TAILQ_INSERT_TAIL(&hmp->pq_list, pe, entry);
	if (++hmp->pq_count > hammer2_rz_pq_high)
		hammer2_rz_pq_high = hmp->pq_count;
	hammer2_spin_unex(&hmp->pq_spin);
	return pe;
}

/* The row's writes are started (or prepared): the reaper may take it. */
void
hammer2_pq_handoff(hammer2_dev_t *hmp, struct hammer2_pq_ent *pe)
{
	atomic_add_long(&hammer2_rz_pq_async, 1);
	/* nbuf and the buffers are set before this; the reaper reads them after */
	cpu_sfence();
	pe->ready = 1;
	wakeup(&hmp->pq_list);
}

/* An entry is on the list but not yet done: wait for it, by seq. */
static void
hammer2_pq_wait_seq(hammer2_dev_t *hmp, uint64_t seq)
{
	while (hmp->pq_done_seq < seq) {
		tsleep_interlock(__DEVOLATILE(void *, &hmp->pq_done_seq), 0);
		if (hmp->pq_done_seq >= seq)
			break;
		tsleep(__DEVOLATILE(void *, &hmp->pq_done_seq), PINTERLOCKED,
		       "h2pqw", hz);
	}
}

/*
 * Wait until the parity-side writes to this per-disk offset (any
 * member) that are in flight are done.  Callers read P/Q or a zero
 * column of the row from the media.
 */
void
hammer2_pq_wait_slot(hammer2_dev_t *hmp, hammer2_off_t phys_off)
{
	struct hammer2_pq_ent *pe;
	uint64_t seq = 0;

	if (hmp->pq_count == 0)
		return;
	hammer2_spin_sh(&hmp->pq_spin);
	TAILQ_FOREACH(pe, &hmp->pq_list, entry) {
		if (pe->phys_off == phys_off && pe->seq > seq)
			seq = pe->seq;
	}
	hammer2_spin_unsh(&hmp->pq_spin);
	if (seq && hmp->pq_done_seq < seq) {
		hammer2_rz_pq_rd_wait++;
		hammer2_pq_wait_seq(hmp, seq);
	}
}

/* Wait for every entry registered so far. */
void
hammer2_pq_drain(hammer2_dev_t *hmp)
{
	uint64_t seq;

	if (hmp->pq_count == 0)
		return;
	hammer2_spin_sh(&hmp->pq_spin);
	seq = hmp->pq_seq;
	hammer2_spin_unsh(&hmp->pq_spin);
	if (hmp->pq_done_seq < seq) {
		hammer2_rz_pq_drain_wait++;
		hammer2_pq_wait_seq(hmp, seq);
	}
}

int
hammer2_pq_busy(hammer2_dev_t *hmp)
{
	return hmp->pq_count != 0;
}

/*
 * A failed parity write.  The reaper must not sleep for the voldata lock
 * (a reader that holds it may be waiting for this very entry), so the
 * failure is recorded in memory at once and persisted by the task; the
 * flush persists it before the volume header (hammer2_raid6_persist_failed).
 */
static void
hammer2_pq_write_failed(hammer2_dev_t *hmp, int disk, int error)
{
	if (hmp->raid_failed[disk])
		hammer2_raid6_write_failed(hmp, disk, error);
	else
		(void)hammer2_raid6_mark_failed(hmp, disk, HAMMER2_FAIL_DEFER);
}

static void
hammer2_pq_reap(hammer2_dev_t *hmp, struct hammer2_pq_ent *pe)
{
	int i;

	if (pe->defer) {
		/*
		 * Test: start the writes inject_pq_defer_ms after the row
		 * was registered; setting the sysctl to 0 releases them.
		 */
		while (hammer2_inject_pq_defer_ms > 0 &&
		       (int)(ticks - pe->tick) <
		       hammer2_inject_pq_defer_ms * hz / 1000)
			tsleep(pe, 0, "h2pqdf", hz / 20 + 1);
		for (i = 0; i < pe->nbuf; i++)
			(void)hammer2_bwrite_start(pe->bp[i], pe->disk[i]);
	}
	for (i = 0; i < pe->nbuf; i++) {
		int e = hammer2_bwrite_wait(pe->bp[i]);

		if (e) {
			hammer2_rz_pq_err++;
			hammer2_pq_write_failed(hmp, pe->disk[i], e);
		}
	}
}

static void
hammer2_pq_thread(void *arg)
{
	hammer2_dev_t *hmp = arg;
	struct hammer2_pq_ent *pe;

	for (;;) {
		tsleep_interlock(&hmp->pq_list, 0);
		hammer2_spin_sh(&hmp->pq_spin);
		pe = TAILQ_FIRST(&hmp->pq_list);
		hammer2_spin_unsh(&hmp->pq_spin);
		if (pe == NULL) {
			if (hmp->pq_exit)
				break;
			tsleep(&hmp->pq_list, PINTERLOCKED, "h2pqrp", hz);
			continue;
		}
		if (pe->ready == 0) {
			/* the writer is still preparing it */
			tsleep(&hmp->pq_list, PINTERLOCKED, "h2pqrp", hz);
			continue;
		}
		hammer2_pq_reap(hmp, pe);
		hammer2_spin_ex(&hmp->pq_spin);
		TAILQ_REMOVE(&hmp->pq_list, pe, entry);
		hmp->pq_count--;
		hmp->pq_done_seq = pe->seq;
		hammer2_spin_unex(&hmp->pq_spin);
		atomic_add_int(&hmp->rz_sealing, -1);
		wakeup(__DEVOLATILE(void *, &hmp->pq_done_seq));
		kfree(pe, M_HAMMER2);
	}
	hmp->pq_running = 0;
	wakeup(__DEVOLATILE(void *, &hmp->pq_running));
	kthread_exit();
}

void
hammer2_pq_start(hammer2_dev_t *hmp)
{
	hammer2_spin_init(&hmp->pq_spin, "h2pq");
	TAILQ_INIT(&hmp->pq_list);
	hmp->pq_exit = 0;
	hmp->pq_running = 1;
	if (kthread_create(hammer2_pq_thread, hmp, NULL, "h2parity")) {
		hmp->pq_running = 0;
		kprintf("hammer2: parity reaper kthread create failed; "
			"async_seal is off for this mount\n");
	}
}

/*
 * Drain and stop the reaper.  Nothing can seal any more (the final flush
 * is done); the devices are still open.
 */
void
hammer2_pq_stop(hammer2_dev_t *hmp)
{
	hammer2_pq_drain(hmp);
	if (hmp->pq_running) {
		hmp->pq_exit = 1;
		wakeup(&hmp->pq_list);
		while (hmp->pq_running) {
			tsleep(__DEVOLATILE(void *, &hmp->pq_running), 0,
			       "h2pqx", hz / 10);
			wakeup(&hmp->pq_list);
		}
	}
}

/*
 * v3 RAIDZ2-native: write a metadata block to its copies, at the same
 * per-disk byte offset on each: the primary and the mirrors that
 * hammer2_md_copy_disk() names (3 copies, or every disk on a layout 2
 * array).  No parity — the copies are their own redundancy.
 *
 * primary: the disk the block's dio is keyed on.
 * skip_disk_idx: if >= 0, don't write to this disk (the caller has
 * already issued the primary bdwrite/bwrite to it through the buffer
 * cache).  Pass -1 to write every healthy copy, the primary's included.
 *
 * The copies are started with bawrite() and not waited for; the caller
 * (the sync) gets its completion from VOP_FSYNC(MNT_WAIT) on each member,
 * which waits for the devvp's write track, before the volume header.  A
 * failed write re-dirties its buffer B_ERROR, which the sync turns into
 * hammer2_raid6_write_failed() for that member like a data column.  A
 * disk being rebuilt gets the copy too, written without waiting on its
 * buffers, under rebuild_lk so the resilver's metadata copy does not
 * overwrite it with an older one.
 *
 * Returns 0 if at least one disk write was started; EIO if every other
 * disk was either failed or could not be given a buffer.
 */
int
hammer2_io_metadata_mirror_write(hammer2_dev_t *hmp, int primary,
				 int skip_disk_idx,
				 hammer2_off_t per_disk_off,
				 void *data, size_t bytes)
{
	int k;
	hammer2_volume_t *vol;
	struct buf *bp;
	int i;
	int ok = 0;
	int last_err = 0;
	int wrote_any = 0;

	KKASSERT(hammer2_raid6_v3(hmp));

	/* the copies are asynchronous: keep a big queue from piling up */
	waitrunningbufspace();
	lockmgr(&hmp->rebuild_lk, LK_SHARED);
	for (k = 0; k < hmp->md_ncopies; k++) {
		i = hammer2_md_copy_disk(hmp->raid_config.ndisks,
		    hmp->md_ncopies, primary, per_disk_off, k);
		if (i == skip_disk_idx)
			continue;
		if (!hammer2_raid6_disk_writable(hmp, i))
			continue;
		vol = &hmp->volumes[i];
		if (vol->dev == NULL || vol->dev->devvp == NULL ||
		    !vol->dev->open)
			continue;

		HAMMER2_RSTAT_WR(hmp, i, HAMMER2_RSC_MMIRROR, bytes);
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
		/* only the failover read wants a mirror, and it re-reads */
		bp->b_flags |= B_RELBUF | B_NOCACHE;
		if (hammer2_inject_wio(i))
			hammer2_inject_wio_dirty(bp);
		else
			bawrite(bp);	/* B_INVAL: released unwritten */
		ok++;
		wrote_any = 1;
	}
	lockmgr(&hmp->rebuild_lk, LK_RELEASE);
	if (skip_disk_idx >= 0)
		return ok ? 0 : (last_err ? last_err : EIO);
	return wrote_any ? 0 : (last_err ? last_err : EIO);
}

/*
 * Deferred metadata mirror copies.
 *
 * The inode, directory and freemap blocks of a busy TXG are released
 * dirty over and over, and each release used to write the whole 64 KB
 * block to every sibling disk at once (bdwrite of the primary coalesces,
 * the mirrors did not).  putblk now hands the copy here instead; a later
 * release of the same block replaces it, and the sync writes each block
 * once, before the VOP_FSYNC drain and the volume header, so the copies
 * are on the disks (though not flushed from their caches) no later than
 * before.  hammer2_io_metadata_mirror_read() looks here first, so a
 * failover read never sees an older copy.
 *
 * Invariant: a block the flush has committed is not queued again until
 * the next TXG.  The flush drains the queue after hammer2_flush() has
 * released every dirty dio of the TXG (the sync runs it after the chain
 * flush), and a chain whose flush cleared MODIFIED is copied on write by
 * the next hammer2_chain_modify() (v3 always sets newmod).  That covers
 * FREEMAP_LEAF/NODE too: hammer2_freemap_alloc() gives them a new offset
 * from hammer2_freemap_reserve(), which rotates through the 8 reserved
 * copies on every COW.  So a copy queued after the drain belongs to a
 * block of the next TXG, and a copy replaced in the queue is a rewrite
 * within one TXG.
 */
static __inline int
hammer2_mirror_hash(hammer2_off_t off)
{
	return ((off >> 16) & (HAMMER2_MIRROR_HASH - 1));
}

static struct hammer2_mirror_pend *
hammer2_mirror_find_locked(hammer2_dev_t *hmp, hammer2_off_t off)
{
	struct hammer2_mirror_pend *mp;

	TAILQ_FOREACH(mp, &hmp->mirror_pend[hammer2_mirror_hash(off)], entry) {
		if (mp->off == off)
			return mp;
	}
	return NULL;
}

/*
 * Queue the mirror copy of a metadata block.  Takes ownership of data
 * (kmalloc'd, M_HAMMER2, bytes long).  primary is the disk that holds
 * the block's own copy, or -1.
 */
void
hammer2_io_metadata_mirror_defer(hammer2_dev_t *hmp, int primary,
				 hammer2_off_t per_disk_off, char *data,
				 int bytes)
{
	struct hammer2_mirror_pend *mp;
	struct hammer2_mirror_pend *nmp;
	char *odata = NULL;
	int count;

	nmp = kmalloc(sizeof(*nmp), M_HAMMER2, M_WAITOK | M_ZERO);
	hammer2_spin_ex(&hmp->mirror_spin);
	mp = hammer2_mirror_find_locked(hmp, per_disk_off);
	if (mp) {
		odata = mp->data;
		mp->data = data;
		mp->bytes = bytes;
		mp->primary = primary;
	} else {
		mp = nmp;
		nmp = NULL;
		mp->off = per_disk_off;
		mp->bytes = bytes;
		mp->primary = primary;
		mp->data = data;
		TAILQ_INSERT_TAIL(&hmp->mirror_pend[
		    hammer2_mirror_hash(per_disk_off)], mp, entry);
		++hmp->mirror_pend_count;
	}
	count = hmp->mirror_pend_count;
	hammer2_spin_unex(&hmp->mirror_spin);
	if (nmp)
		kfree(nmp, M_HAMMER2);
	if (odata)
		kfree(odata, M_HAMMER2);

	atomic_add_long(&hammer2_p10_mf_defers, 1);
	if (count > HAMMER2_MIRROR_MAXPEND) {
		atomic_add_long(&hammer2_p10_mf_maxpend, 1);
		hammer2_io_metadata_mirror_flush(hmp);
	}
}

/*
 * Write every queued mirror copy.  Entries stay visible to mirror reads
 * until written.  A block queued again while the flush runs is written
 * by the next one (it belongs to a later TXG).
 */
void
hammer2_io_metadata_mirror_flush(hammer2_dev_t *hmp)
{
	struct hammer2_mirror_pend *mp;
	int budget;
	int h;

	if (hmp->mirror_pend_count == 0)
		return;
	lockmgr(&hmp->mirror_flush_lk, LK_EXCLUSIVE);
	budget = hmp->mirror_pend_count + HAMMER2_MIRROR_MAXPEND;
	while (budget-- > 0) {
		hammer2_spin_ex(&hmp->mirror_spin);
		mp = NULL;
		for (h = 0; h < HAMMER2_MIRROR_HASH; ++h) {
			mp = TAILQ_FIRST(&hmp->mirror_pend[h]);
			if (mp)
				break;
		}
		if (mp) {
			TAILQ_REMOVE(&hmp->mirror_pend[h], mp, entry);
			--hmp->mirror_pend_count;
			hmp->mirror_inflight = mp;
		}
		hammer2_spin_unex(&hmp->mirror_spin);
		if (mp == NULL)
			break;
		atomic_add_long(&hammer2_p10_mf_blocks, 1);

		/*
		 * A primary that is down (failed before the release, or
		 * since) has no dirty buffer to carry the block; the copy
		 * goes to every disk, a rebuilding one included.
		 */
		hammer2_io_metadata_mirror_write(hmp,
		    mp->primary >= 0 ? mp->primary : 0,
		    (mp->primary >= 0 && !hmp->raid_failed[mp->primary]) ?
		    mp->primary : -1, mp->off, mp->data, mp->bytes);

		hammer2_spin_ex(&hmp->mirror_spin);
		hmp->mirror_inflight = NULL;
		hammer2_spin_unex(&hmp->mirror_spin);
		kfree(mp->data, M_HAMMER2);
		kfree(mp, M_HAMMER2);
	}
	lockmgr(&hmp->mirror_flush_lk, LK_RELEASE);
}

/*
 * v3 RAIDZ2-native metadata read from one member.  Returns 0 with the
 * copy in buf, ENXIO if the member cannot be read (failed, absent or
 * closed; a member being rebuilt counts as failed until it is done),
 * EWOULDBLOCK if its buffer is busy and block is 0 (a copy is being
 * written), or the read error.  A copy queued for the next sync is newer than the
 * disks' and is returned instead.  The copy is not kept in the member's
 * buffer cache unless it was there already: the dio that asks keeps the
 * block, and a second copy in the cache is a waste of RAM.
 */
int
hammer2_io_metadata_read_one(hammer2_dev_t *hmp, int disk_idx,
			     hammer2_off_t per_disk_off,
			     void *buf, size_t bytes, int block)
{
	hammer2_volume_t *vol;
	int error;

	KKASSERT(hammer2_raid6_v3(hmp));

	if (hmp->mirror_pend_count || hmp->mirror_inflight) {
		struct hammer2_mirror_pend *mp;

		hammer2_spin_ex(&hmp->mirror_spin);
		mp = hammer2_mirror_find_locked(hmp, per_disk_off);
		if (mp == NULL && hmp->mirror_inflight &&
		    hmp->mirror_inflight->off == per_disk_off)
			mp = hmp->mirror_inflight;
		if (mp && (size_t)mp->bytes >= bytes) {
			bcopy(mp->data, buf, bytes);
			hammer2_spin_unex(&hmp->mirror_spin);
			return 0;
		}
		hammer2_spin_unex(&hmp->mirror_spin);
	}

	if (hmp->raid_failed[disk_idx])
		return ENXIO;
	vol = &hmp->volumes[disk_idx];
	if (vol->dev == NULL || vol->dev->devvp == NULL || !vol->dev->open)
		return ENXIO;
	error = hammer2_inject_eio(disk_idx);
	if (error)
		return error;
	/*
	 * Never the raw media read that hammer2_io_raid6_read_sibling()
	 * falls back to when the buffer is busy: the sync writes the
	 * copies with bawrite(), and until it completes the media holds
	 * the old one while the queue no longer has the new.  A busy
	 * buffer is EWOULDBLOCK; the caller chooses another copy or waits.
	 */
	if (!block)
		return hammer2_io_raid6_read_bufcache(hmp, disk_idx,
		    vol->dev->devvp, per_disk_off, buf, (int)bytes, 1,
		    HAMMER2_RSC_META);
	{
		struct buf *bp = NULL;

		HAMMER2_RSTAT_RD(hmp, disk_idx, HAMMER2_RSC_META, bytes);
		error = bread(vol->dev->devvp, per_disk_off, (int)bytes, &bp);
		if (error == 0) {
			bkvasync(bp);
			bcopy(bp->b_data, buf, bytes);
		}
		if (bp)
			brelse(bp);
	}
	return error;
}

/*
 * The copy that serves a cold read of the metadata block at
 * per_disk_off: one per 64 KB block, round robin over the block's
 * copies, so a walk of the metadata zone uses every disk.  All copies
 * are identical (see hammer2_io_metadata_mirror_defer), so the choice
 * only spreads the load.
 */
int
hammer2_io_metadata_read_disk(hammer2_dev_t *hmp, int primary,
			      hammer2_off_t per_disk_off)
{
	return hammer2_md_copy_disk(hmp->raid_config.ndisks, hmp->md_ncopies,
	    primary, per_disk_off,
	    (int)((per_disk_off >> 16) % hmp->md_ncopies));
}

/*
 * v3 RAIDZ2-native: read a metadata block from a surviving copy.
 * Used when the primary disk for a metadata DIO is failed (skip_disk_idx
 * is that primary).  Tries the copy that hammer2_io_metadata_read_disk()
 * names first, then the others of the block in copy order, skipping
 * skip_disk_idx and any disk marked failed or closed, until one read
 * succeeds.  Disks that do not hold a copy of the block are never tried.
 *
 * Returns 0 on success and copies bytes into buf; EIO if no surviving
 * disk has a usable copy.
 */
int
hammer2_io_metadata_mirror_read(hammer2_dev_t *hmp, int skip_disk_idx,
				hammer2_off_t per_disk_off,
				void *buf, size_t bytes)
{
	int ndisks = hmp->raid_config.ndisks;
	int ncopies = hmp->md_ncopies;
	int start;
	int i;
	int k;
	int pass;
	int error;
	int last_err = 0;

	/* the primary is the disk the caller skips (it is the failed one) */
	start = (int)((per_disk_off >> 16) % ncopies);
	for (pass = 0; pass < 2; pass++) {
		for (k = 0; k < ncopies; k++) {
			i = hammer2_md_copy_disk(ndisks, ncopies, skip_disk_idx,
			    per_disk_off, (start + k) % ncopies);
			if (i == skip_disk_idx)
				continue;
			error = hammer2_io_metadata_read_one(hmp, i,
			    per_disk_off, buf, bytes, pass);
			if (error == 0)
				return 0;
			if (error != ENXIO && error != EWOULDBLOCK)
				last_err = error;
		}
		/* second pass: wait for copies still being written */
	}

	return last_err ? last_err : EIO;
}
