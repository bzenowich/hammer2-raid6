/*
 * Copyright (c) 2026 The DragonFly Project.  All rights reserved.
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
 * HAMMER2 RAID6 parity loops for arm64 NEON, called from
 * hammer2_raid6_xor() and hammer2_raid6_mul() between kernel_fpu_begin()
 * and kernel_fpu_end().  bytes is a nonzero multiple of 64.
 *
 * A GF(2^8) product c * x is c * (x & 15) ^ c * (x & 0xf0), each a tbl
 * lookup in a 16-byte table: tbl[0..15] = c * i, tbl[16..31] = c * (i << 4).
 */
#include <machine/asm.h>

	.arch_extension simd	/* the kernel is built -mgeneral-regs-only */
	.text

/* void hammer2_raid6_xor_neon(uint8_t *dst, const uint8_t *src, size_t bytes) */
ENTRY(hammer2_raid6_xor_neon)
1:	ld1	{v0.16b-v3.16b}, [x1], #64
	ld1	{v4.16b-v7.16b}, [x0]
	eor	v4.16b, v4.16b, v0.16b
	eor	v5.16b, v5.16b, v1.16b
	eor	v6.16b, v6.16b, v2.16b
	eor	v7.16b, v7.16b, v3.16b
	st1	{v4.16b-v7.16b}, [x0], #64
	subs	x2, x2, #64
	b.ne	1b
	ret
END(hammer2_raid6_xor_neon)

/*
 * The products of v0-v3 into v0-v3.  v16/v17 the tables, v18 0x0f.
 */
.macro	gfmul4
	ushr	v4.16b, v0.16b, #4
	ushr	v5.16b, v1.16b, #4
	ushr	v6.16b, v2.16b, #4
	ushr	v7.16b, v3.16b, #4
	and	v0.16b, v0.16b, v18.16b
	and	v1.16b, v1.16b, v18.16b
	and	v2.16b, v2.16b, v18.16b
	and	v3.16b, v3.16b, v18.16b
	tbl	v4.16b, {v17.16b}, v4.16b
	tbl	v5.16b, {v17.16b}, v5.16b
	tbl	v6.16b, {v17.16b}, v6.16b
	tbl	v7.16b, {v17.16b}, v7.16b
	tbl	v0.16b, {v16.16b}, v0.16b
	tbl	v1.16b, {v16.16b}, v1.16b
	tbl	v2.16b, {v16.16b}, v2.16b
	tbl	v3.16b, {v16.16b}, v3.16b
	eor	v0.16b, v0.16b, v4.16b
	eor	v1.16b, v1.16b, v5.16b
	eor	v2.16b, v2.16b, v6.16b
	eor	v3.16b, v3.16b, v7.16b
.endm

/*
 * void hammer2_raid6_mul_neon(uint8_t *dst, const uint8_t *src,
 *     size_t bytes, const uint8_t *tbl): dst = c * src.  dst may be src.
 */
ENTRY(hammer2_raid6_mul_neon)
	ld1	{v16.16b, v17.16b}, [x3]
	movi	v18.16b, #0x0f
1:	ld1	{v0.16b-v3.16b}, [x1], #64
	gfmul4
	st1	{v0.16b-v3.16b}, [x0], #64
	subs	x2, x2, #64
	b.ne	1b
	ret
END(hammer2_raid6_mul_neon)

/*
 * void hammer2_raid6_mulacc_neon(uint8_t *dst, const uint8_t *src,
 *     size_t bytes, const uint8_t *tbl): dst ^= c * src.  dst may be src.
 */
ENTRY(hammer2_raid6_mulacc_neon)
	ld1	{v16.16b, v17.16b}, [x3]
	movi	v18.16b, #0x0f
1:	ld1	{v0.16b-v3.16b}, [x1], #64
	gfmul4
	ld1	{v4.16b-v7.16b}, [x0]
	eor	v0.16b, v0.16b, v4.16b
	eor	v1.16b, v1.16b, v5.16b
	eor	v2.16b, v2.16b, v6.16b
	eor	v3.16b, v3.16b, v7.16b
	st1	{v0.16b-v3.16b}, [x0], #64
	subs	x2, x2, #64
	b.ne	1b
	ret
END(hammer2_raid6_mulacc_neon)
