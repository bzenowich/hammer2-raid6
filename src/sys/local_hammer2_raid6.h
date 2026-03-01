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
 * RAID 6 disk states
 */
#define HAMMER2_RAID6_DISK_ONLINE	0
#define HAMMER2_RAID6_DISK_FAILED	1
#define HAMMER2_RAID6_DISK_REBUILDING	2
#define HAMMER2_RAID6_DISK_SPARE	3

/*
 * RAID 6 array flags
 */
#define HAMMER2_RAID6_FLAG_DEGRADED	0x0001
#define HAMMER2_RAID6_FLAG_REBUILDING	0x0002

/*
 * GF(2^8) lookup tables.
 */
extern uint8_t hammer2_gf_exp[256];
extern uint8_t hammer2_gf_log[256];
extern uint8_t hammer2_gf_inv[256];
extern uint8_t hammer2_gf_mul_table[256][256];

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
 * Function prototypes are declared in hammer2.h (which all kernel
 * files include) to avoid redundant-declaration warnings.
 */

#endif /* !_VFS_HAMMER2_RAID6_H_ */
