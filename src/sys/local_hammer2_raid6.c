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
}

/*
 * Generate P and Q syndromes for the given disk array.
 *
 * ptrs[0..ndisks-3] = data disk buffers
 * ptrs[ndisks-2]    = P parity buffer (output)
 * ptrs[ndisks-1]    = Q parity buffer (output)
 *
 * P = data[0] ^ data[1] ^ ... ^ data[ndata-1]
 * Q = data[0]*2^0 ^ data[1]*2^1 ^ ... ^ data[ndata-1]*2^(ndata-1)
 *   simplified using Horner's method (evaluate from highest to lowest):
 * Q = (...((data[ndata-1] * 2) ^ data[ndata-2]) * 2 ^ ...) * 2 ^ data[0]
 */
void
hammer2_raid6_gen_syndrome(int ndisks, size_t bytes, void **ptrs)
{
	int ndata = ndisks - 2;
	uint8_t *p = ptrs[ndisks - 2];
	uint8_t *q = ptrs[ndisks - 1];
	size_t d;
	int z;

	for (d = 0; d < bytes; d++) {
		uint8_t pv = 0;
		uint8_t qv = 0;
		for (z = 0; z < ndata; z++) {
			uint8_t c = ((uint8_t **)ptrs)[z][d];
			pv ^= c;
			qv ^= hammer2_gf_mul(hammer2_gf_exp[z], c);
		}
		p[d] = pv;
		q[d] = qv;
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
	size_t d;
	int z;

	dpa = ptrs[faila];
	dpb = ptrs[failb];

	/* Precompute coefficients */
	coeff_a = hammer2_gf_exp[faila];		/* 2^faila */
	coeff_b = hammer2_gf_exp[failb];		/* 2^failb */
	coeff_diff_inv = hammer2_gf_inv[coeff_a ^ coeff_b]; /* 1/(2^a ^ 2^b) */

	for (d = 0; d < bytes; d++) {
		uint8_t Pxy, Qxy, B, A;

		/*
		 * Compute Pxy = P ^ XOR of all surviving data disks.
		 * This equals data[faila] ^ data[failb].
		 */
		Pxy = p[d];
		for (z = 0; z < ndata; z++) {
			if (z != faila && z != failb)
				Pxy ^= ((uint8_t *)ptrs[z])[d];
		}

		/*
		 * Compute Qxy = Q ^ syndrome of all surviving data disks.
		 * This equals data[faila]*2^faila ^ data[failb]*2^failb.
		 */
		Qxy = q[d];
		for (z = 0; z < ndata; z++) {
			if (z != faila && z != failb)
				Qxy ^= hammer2_gf_mul(hammer2_gf_exp[z],
						       ((uint8_t *)ptrs[z])[d]);
		}

		/*
		 * Solve: B = (Pxy * 2^faila ^ Qxy) / (2^faila ^ 2^failb)
		 *        A = Pxy ^ B
		 */
		B = hammer2_gf_mul(hammer2_gf_mul(Pxy, coeff_a) ^ Qxy,
				   coeff_diff_inv);
		A = Pxy ^ B;

		dpa[d] = A;
		dpb[d] = B;
	}
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
	uint8_t coeff_inv;
	size_t d;
	int z;

	dpa = ptrs[faila];
	coeff_inv = hammer2_gf_inv[hammer2_gf_exp[faila]]; /* 1/2^faila */

	for (d = 0; d < bytes; d++) {
		uint8_t Qx;

		/* Compute Qx = Q ^ syndrome of surviving disks */
		Qx = q[d];
		for (z = 0; z < ndata; z++) {
			if (z != faila)
				Qx ^= hammer2_gf_mul(hammer2_gf_exp[z],
						      ((uint8_t *)ptrs[z])[d]);
		}

		/* data[faila] = Qx / 2^faila */
		dpa[d] = hammer2_gf_mul(Qx, coeff_inv);
	}

	/* Recompute P from all data disks */
	for (d = 0; d < bytes; d++) {
		uint8_t pv = 0;
		for (z = 0; z < ndata; z++)
			pv ^= ((uint8_t *)ptrs[z])[d];
		p[d] = pv;
	}
}

/*
 * Router: recover from 2 disk failures.
 *
 * faila, failb are physical disk indices (0..ndisks-1) with faila < failb.
 * Dispatches to the appropriate recovery function based on which disks failed.
 */
void
hammer2_raid6_dual_recov(int ndisks, size_t bytes,
			 int faila, int failb, void **ptrs)
{
	int ndata = ndisks - 2;

	if (faila >= ndata) {
		/*
		 * Both P and Q failed - just regenerate them.
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
			uint8_t *p = ptrs[ndisks - 2];
			uint8_t *dpa = ptrs[faila];
			size_t d;
			int z;

			for (d = 0; d < bytes; d++) {
				uint8_t val = p[d];
				for (z = 0; z < ndata; z++) {
					if (z != faila)
						val ^= ((uint8_t *)ptrs[z])[d];
				}
				dpa[d] = val;
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
