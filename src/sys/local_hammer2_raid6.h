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

#ifndef _VFS_HAMMER2_RAID6_H_
#define _VFS_HAMMER2_RAID6_H_

/*
 * HAMMER2 RAID 6 Support
 *
 * GF(2^8) arithmetic using polynomial x^8 + x^4 + x^3 + x^2 + 1
 * (0x11d, reduction constant 0x1d).
 *
 * P parity = XOR of all data disks
 * Q parity = Reed-Solomon syndrome using generator 2 in GF(2^8)
 */

#define HAMMER2_RAID6_POLY	0x1d	/* x^8+x^4+x^3+x^2+1 reduction */
#define HAMMER2_RAID6_MIN_DISKS	4	/* minimum: 2 data + 2 parity */

/*
 * Disk states and array flags are persisted: see hammer2_disk.h.
 */

/*
 * GF(2^8) lookup tables.
 */
extern uint8_t hammer2_gf_exp[256];
extern uint8_t hammer2_gf_log[256];
extern uint8_t hammer2_gf_inv[256];
extern uint8_t hammer2_gf_mul_table[256][256];

void hammer2_raid6_xor(void *dst, const void *src, size_t bytes);
void hammer2_raid6_mul(void *dst, const void *src, size_t bytes, uint8_t c,
		       int acc);

/*
 * Inline GF(2^8) multiply by 2 (generator).
 */
static __inline uint8_t
hammer2_gf_mul2(uint8_t x)
{
	return (x << 1) ^ ((x & 0x80) ? HAMMER2_RAID6_POLY : 0);
}

/*
 * Inline GF(2^8) multiply using table.
 */
static __inline uint8_t
hammer2_gf_mul(uint8_t a, uint8_t b)
{
	return hammer2_gf_mul_table[a][b];
}

/*
 * Inline GF(2^8) division using table + inverse.
 */
static __inline uint8_t
hammer2_gf_div(uint8_t a, uint8_t b)
{
	return hammer2_gf_mul_table[a][hammer2_gf_inv[b]];
}

/*
 * Inline GF(2^8) power using exp/log tables.
 */
static __inline uint8_t
hammer2_gf_pow(uint8_t base, uint8_t exp)
{
	int log_base;

	if (base == 0)
		return 0;
	log_base = hammer2_gf_log[base];
	return hammer2_gf_exp[((int)log_base * exp) % 255];
}

/*
 * v3 stripe-row layout.  A row is one stripe_unit-sized slot at the same
 * per-disk physical offset on every disk.  P lives on disk slot % ndisks,
 * Q on the next disk, and the remaining disks hold data columns in
 * disk-index order.  Keep every use of this rotation behind these inlines.
 */
static __inline uint64_t
hammer2_raid6_slot(hammer2_off_t phys_off, uint64_t stripe_unit)
{
	return (phys_off - HAMMER2_ZONE_SEG64) / stripe_unit;
}

static __inline void
hammer2_raid6_pq(uint64_t slot, int ndisks, int *p_disk, int *q_disk)
{
	*p_disk = (int)(slot % ndisks);
	*q_disk = (*p_disk + 1) % ndisks;
}

/*
 * Logical column of physical disk d: data columns count the non-P/Q
 * disks below d, P is column ndata and Q is column ndata + 1.
 */
static __inline int
hammer2_raid6_lcol(int d, int p_disk, int q_disk, int ndata)
{
	int di, col = 0;

	if (d == p_disk)
		return ndata;
	if (d == q_disk)
		return ndata + 1;
	for (di = 0; di < d; di++) {
		if (di != p_disk && di != q_disk)
			col++;
	}
	return col;
}

/*
 * Round-robin data disk picker: advances hmp->stripe_next_disk and
 * returns the first disk that is neither P, Q nor in skip_mask, or -1.
 * Caller holds stripe_bitmap_spin.
 */
static __inline int
hammer2_raid6_pick_data_disk(hammer2_dev_t *hmp, int p_disk, int q_disk,
			     uint32_t skip_mask)
{
	int ndisks = hmp->raid_config.ndisks;
	int candidate, tries;

	for (tries = 0; tries < ndisks; tries++) {
		candidate = (int)(hmp->stripe_next_disk % (u_int)ndisks);
		hmp->stripe_next_disk++;
		if (candidate == p_disk || candidate == q_disk)
			continue;
		if (skip_mask & (1U << candidate))
			continue;
		return candidate;
	}
	return -1;
}

/*
 * v3 metadata copy placement (oct-review 12).  A metadata block at
 * per-disk offset off is written to its primary (the disk the dio is
 * keyed on, always disk 0 today) and to `ncopies - 1` mirrors; ncopies
 * is 3 on a layout 3 array and ndisks on a layout 2 one (every disk
 * mirrors).  Copy k = 0 is the primary.  Mirrors are picked among the
 * other ndisks - 1 disks, in order primary + 1 ..., from the 64 KB
 * block number b: copy 1 is the (b % n1)th, copy 2 is s further on,
 * s in 1..n1-1 stepped by b / n1, so the two are distinct and every
 * ordered pair of disks occurs.  Keep every use of this rule behind
 * these inlines.
 */
#define HAMMER2_MD_COPIES	3

static __inline int
hammer2_md_ncopies(int ndisks, int layout)
{
	if (layout >= 3 && ndisks > HAMMER2_MD_COPIES)
		return HAMMER2_MD_COPIES;
	return ndisks;
}

static __inline int
hammer2_md_copy_disk(int ndisks, int ncopies, int primary, uint64_t off,
		     int k)
{
	uint64_t b = off >> 16;
	int n1 = ndisks - 1;
	int j;

	if (k == 0)
		return primary;
	if (ncopies >= ndisks)
		return (primary + k) % ndisks;
	j = (int)(b % n1);
	if (k == 2)
		j = (j + 1 + (int)((b / n1) % (n1 - 1))) % n1;
	return (primary + 1 + j) % ndisks;
}

static __inline int
hammer2_md_is_copy(int ndisks, int ncopies, int primary, uint64_t off,
		   int disk)
{
	int k;

	for (k = 0; k < ncopies; k++) {
		if (hammer2_md_copy_disk(ndisks, ncopies, primary, off,
					 k) == disk)
			return 1;
	}
	return 0;
}

/*
 * Function prototypes are declared in hammer2.h (which all kernel
 * files include) to avoid redundant-declaration warnings.
 */

#endif /* !_VFS_HAMMER2_RAID6_H_ */
