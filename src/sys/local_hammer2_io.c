/*
 * Copyright (c) 2013-2018 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@dragonflybsd.org>
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

#include "hammer2.h"
#include "hammer2_raid6.h"

#define HAMMER2_DOP_READ	1
#define HAMMER2_DOP_NEW		2
#define HAMMER2_DOP_NEWNZ	3
#define HAMMER2_DOP_READQ	4

/*
 * Implements an abstraction layer for synchronous and asynchronous
 * buffered device I/O.  Can be used as an OS-abstraction but the main
 * purpose is to allow larger buffers to be used against hammer2_chain's
 * using smaller allocations, without causing deadlocks.
 *
 * The DIOs also record temporary state with limited persistence.  This
 * feature is used to keep track of dedupable blocks.
 */
static int hammer2_io_cleanup_callback(hammer2_io_t *dio, void *arg);
static void dio_write_stats_update(hammer2_io_t *dio, struct buf *bp);

static int
hammer2_io_cmp(hammer2_io_t *io1, hammer2_io_t *io2)
{
	if (io1->pbase < io2->pbase)
		return(-1);
	if (io1->pbase > io2->pbase)
		return(1);
	return(0);
}

RB_PROTOTYPE2(hammer2_io_tree, hammer2_io, rbnode, hammer2_io_cmp, off_t);
RB_GENERATE2(hammer2_io_tree, hammer2_io, rbnode, hammer2_io_cmp,
		off_t, pbase);

struct hammer2_cleanupcb_info {
	struct hammer2_io_tree tmptree;
	int	count;
};

#if 0
static __inline
uint64_t
hammer2_io_mask(hammer2_io_t *dio, hammer2_off_t off, u_int bytes)
{
	uint64_t mask;
	int i;

	if (bytes < 1024)	/* smaller chunks not supported */
		return 0;

	/*
	 * Calculate crc check mask for larger chunks
	 */
	i = (((off & ~HAMMER2_OFF_MASK_RADIX) - dio->pbase) &
	     HAMMER2_PBUFMASK) >> 10;
	if (i == 0 && bytes == HAMMER2_PBUFSIZE)
		return((uint64_t)-1);
	mask = ((uint64_t)1U << (bytes >> 10)) - 1;
	mask <<= i;

	return mask;
}
#endif

#ifdef HAMMER2_IO_DEBUG

static __inline void
DIO_RECORD(hammer2_io_t *dio HAMMER2_IO_DEBUG_ARGS)
{
	int i;

	i = atomic_fetchadd_int(&dio->debug_index, 1) & HAMMER2_IO_DEBUG_MASK;

	dio->debug_file[i] = file;
	dio->debug_line[i] = line;
	dio->debug_refs[i] = dio->refs;
	dio->debug_td[i] = curthread;
}

#else

#define DIO_RECORD(dio)

#endif

/*
 * Returns the DIO corresponding to the data|radix, creating it if necessary.
 *
 * If createit is 0, NULL can be returned indicating that the DIO does not
 * exist.  (btype) is ignored when createit is 0.
 */
static __inline
hammer2_io_t *
hammer2_io_alloc(hammer2_dev_t *hmp, hammer2_key_t data_off, uint8_t btype,
		 int createit, int *isgoodp)
{
	hammer2_io_t *dio;
	hammer2_io_t *xio;
	hammer2_key_t lbase;
	hammer2_key_t pbase;
	hammer2_key_t pmask;
	hammer2_volume_t *vol;
	uint64_t refs;
	int lsize;
	int psize;

	psize = HAMMER2_PBUFSIZE;
	pmask = ~(hammer2_off_t)(psize - 1);
	if ((int)(data_off & HAMMER2_OFF_MASK_RADIX))
		lsize = 1 << (int)(data_off & HAMMER2_OFF_MASK_RADIX);
	else
		lsize = 0;
	lbase = data_off & ~HAMMER2_OFF_MASK_RADIX;
	pbase = lbase & pmask;

	if (pbase == 0 || ((lbase + lsize - 1) & pmask) != pbase) {
		kprintf("Illegal: %016jx %016jx+%08x / %016jx\n",
			pbase, lbase, lsize, pmask);
	}
	KKASSERT(pbase != 0 && ((lbase + lsize - 1) & pmask) == pbase);
	*isgoodp = 0;

	/*
	 * Access/Allocate the DIO, bump dio->refs to prevent destruction.
	 *
	 * If DIO_GOOD is set the ref should prevent it from being cleared
	 * out from under us, we can set *isgoodp, and the caller can operate
	 * on the buffer without any further interaction.
	 */
	hammer2_spin_sh(&hmp->io_spin);
	dio = RB_LOOKUP(hammer2_io_tree, &hmp->iotree, pbase);
	if (dio) {
		refs = atomic_fetchadd_64(&dio->refs, 1);
		if ((refs & HAMMER2_DIO_MASK) == 0) {
			atomic_add_int(&dio->hmp->iofree_count, -1);
		}
		if (refs & HAMMER2_DIO_GOOD)
			*isgoodp = 1;
		hammer2_spin_unsh(&hmp->io_spin);
	} else if (createit) {
		refs = 0;
		hammer2_spin_unsh(&hmp->io_spin);
		dio = kmalloc_obj(sizeof(*dio), hmp->mio, M_INTWAIT | M_ZERO);
		dio->hmp = hmp;
		if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6) {
			/*
			 * RAID 6: map logical offset to physical disk
			 * and physical offset.
			 *
			 * The DIO layer computes dev_pbase = pbase - dbase
			 * as the sector address on the physical device.
			 * For RAID 6, phys_off is the correct physical
			 * sector address, so set dbase = pbase - phys_off.
			 */
			int disk_idx;
			hammer2_off_t phys_off;

			hammer2_raid6_map(hmp, pbase,
					  &disk_idx, &phys_off);
			vol = &hmp->volumes[disk_idx];
			dio->devvp = vol->dev->devvp;
			dio->dbase = pbase - phys_off;
			dio->disk_idx = disk_idx;
		} else {
			vol = hammer2_get_volume(hmp, pbase);
			dio->devvp = vol->dev->devvp;
			dio->dbase = vol->offset;
			dio->disk_idx = -1;
		}
		/* dbase must be 1GB-aligned for JBOD; RAID6 uses stripe offsets */
		KKASSERT(hmp->raid_type == HAMMER2_RAID_TYPE_RAID6 ||
			 (dio->dbase & HAMMER2_FREEMAP_LEVEL1_MASK) == 0);
		dio->pbase = pbase;
		dio->psize = psize;
		dio->btype = btype;
		dio->refs = refs + 1;
		dio->act = 5;
		hammer2_spin_ex(&hmp->io_spin);
		xio = RB_INSERT(hammer2_io_tree, &hmp->iotree, dio);
		if (xio == NULL) {
			atomic_add_int(&hammer2_dio_count, 1);
			hammer2_spin_unex(&hmp->io_spin);
		} else {
			refs = atomic_fetchadd_64(&xio->refs, 1);
			if ((refs & HAMMER2_DIO_MASK) == 0)
				atomic_add_int(&xio->hmp->iofree_count, -1);
			if (refs & HAMMER2_DIO_GOOD)
				*isgoodp = 1;
			hammer2_spin_unex(&hmp->io_spin);
			kfree_obj(dio, hmp->mio);
			dio = xio;
		}
	} else {
		hammer2_spin_unsh(&hmp->io_spin);
		return NULL;
	}
	dio->ticks = ticks;
	if (dio->act < 10)
		++dio->act;

	return dio;
}

/*
 * Acquire the requested dio.  If DIO_GOOD is not set we must instantiate
 * a buffer.  If set the buffer already exists and is good to go.
 */
hammer2_io_t *
_hammer2_io_getblk(hammer2_dev_t *hmp, int btype, off_t lbase,
		   int lsize, int op HAMMER2_IO_DEBUG_ARGS)
{
	hammer2_io_t *dio;
	hammer2_off_t dev_pbase;
	off_t peof;
	uint64_t orefs;
	uint64_t nrefs;
	int isgood;
	int error;
	int hce;
	int bflags;

	bflags = ((btype == HAMMER2_BREF_TYPE_DATA) ? B_NOTMETA : 0);
	bflags |= B_KVABIO;

	KKASSERT((1 << (int)(lbase & HAMMER2_OFF_MASK_RADIX)) == lsize);

	if (op == HAMMER2_DOP_READQ) {
		dio = hammer2_io_alloc(hmp, lbase, btype, 0, &isgood);
		if (dio == NULL)
			return NULL;
		op = HAMMER2_DOP_READ;
	} else {
		dio = hammer2_io_alloc(hmp, lbase, btype, 1, &isgood);
	}

	for (;;) {
		orefs = dio->refs;
		cpu_ccfence();

		/*
		 * Buffer is already good, handle the op and return.
		 */
		if (orefs & HAMMER2_DIO_GOOD) {
			if (isgood == 0)
				cpu_mfence();
			bkvasync(dio->bp);

			/*
			 * RAID6: save pre-modification data for RMW
			 * delta parity computation.  Always saved (not
			 * just degraded) to handle the case where both
			 * data columns in a stripe are modified — the
			 * sibling's bdwrite may not have flushed when
			 * the parity thread reads it from disk.
			 */
			if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6 &&
			    dio->raid6_old_data == NULL) {
				dio->raid6_old_data = kmalloc(dio->psize,
				    M_HAMMER2, M_WAITOK);
				bcopy(dio->bp->b_data,
				    dio->raid6_old_data, dio->psize);
			}

			switch(op) {
			case HAMMER2_DOP_NEW:
				bzero(hammer2_io_data(dio, lbase), lsize);
				/* fall through */
			case HAMMER2_DOP_NEWNZ:
				atomic_set_long(&dio->refs, HAMMER2_DIO_DIRTY);
				break;
			case HAMMER2_DOP_READ:
			default:
				/* nothing to do */
				break;
			}
			DIO_RECORD(dio HAMMER2_IO_DEBUG_CALL);
			return (dio);
		}

		/*
		 * Try to own the DIO
		 */
		if (orefs & HAMMER2_DIO_INPROG) {
			nrefs = orefs | HAMMER2_DIO_WAITING;
			tsleep_interlock(dio, 0);
			if (atomic_cmpset_64(&dio->refs, orefs, nrefs)) {
				tsleep(dio, PINTERLOCKED, "h2dio", hz);
			}
			/* retry */
		} else {
			nrefs = orefs | HAMMER2_DIO_INPROG;
			if (atomic_cmpset_64(&dio->refs, orefs, nrefs)) {
				break;
			}
		}
	}

	/*
	 * We break to here if GOOD is not set and we acquired INPROG for
	 * the I/O.
	 */
	KKASSERT(dio->bp == NULL);
	if (btype == HAMMER2_BREF_TYPE_DATA)
		hce = hammer2_cluster_data_read;
	else
		hce = hammer2_cluster_meta_read;

	error = 0;
	dev_pbase = dio->pbase - dio->dbase;

	/*
	 * RAID6 degraded pre-emption: if this disk is already marked
	 * failed, skip device I/O entirely and reconstruct the data
	 * from parity right now.  This avoids I/O to a closed devvp
	 * (after hammer2 raid fail-disk releases VOP_OPEN).
	 *
	 * Handles all ops (READ, NEW, NEWNZ) uniformly for both
	 * full-buffer and sub-buffer cases.
	 */
	if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6 &&
	    hmp->raid_nfailed > 0 &&
	    dio->disk_idx >= 0 &&
	    hmp->raid_failed[dio->disk_idx]) {
		kprintf("h2getblk_deg: pbase=0x%016jx disk=%d op=%d "
			"btype=0x%02x\n",
			(uintmax_t)dio->pbase, dio->disk_idx, op,
			(int)btype);
		dio->bp = getblk(dio->devvp, dev_pbase, dio->psize,
				 GETBLK_KVABIO, 0);
		if (dio->bp) {
			bkvasync(dio->bp);
			error = hammer2_io_raid6_read_degraded(
					hmp, dio->pbase,
					dio->bp->b_data, dio->psize);
			if (error == 0) {
				dio->raid6_old_data = kmalloc(dio->psize,
				    M_HAMMER2, M_WAITOK);
				bcopy(dio->bp->b_data,
				    dio->raid6_old_data, dio->psize);
			}
			switch(op) {
			case HAMMER2_DOP_NEW:
				if (dio->pbase ==
				    (lbase & ~HAMMER2_OFF_MASK_RADIX) &&
				    dio->psize == lsize)
					bzero(dio->bp->b_data, dio->psize);
				else
					bzero(hammer2_io_data(dio, lbase),
					      lsize);
				/* fall through */
			case HAMMER2_DOP_NEWNZ:
				atomic_set_long(&dio->refs,
						HAMMER2_DIO_DIRTY);
				break;
			case HAMMER2_DOP_READ:
			default:
				break;
			}
		}
		dio->error = error;
		goto io_done;
	}

	if (dio->pbase == (lbase & ~HAMMER2_OFF_MASK_RADIX) &&
	    dio->psize == lsize) {
		switch(op) {
		case HAMMER2_DOP_NEW:
		case HAMMER2_DOP_NEWNZ:
			/*
			 * RAID6: read old data before clearing so we
			 * can compute RMW delta parity later.  Always
			 * done (not just degraded) to avoid parity race
			 * when both data columns in a stripe are modified.
			 *
			 * Failed disk: reconstruct from parity.
			 * Healthy disk: read directly via breadnx.
			 * Non-RAID6: original getblk (no I/O needed).
			 */
			if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6 &&
			    hmp->raid_nfailed > 0 &&
			    dio->disk_idx >= 0 &&
			    hmp->raid_failed[dio->disk_idx]) {
				/*
				 * DIO is on the failed disk: reconstruct
				 * old content from parity.
				 */
				dio->bp = getblk(dio->devvp, dev_pbase,
				    dio->psize, GETBLK_KVABIO, 0);
				if (dio->bp) {
					bkvasync(dio->bp);
					error = hammer2_io_raid6_read_degraded(
					    hmp, dio->pbase,
					    dio->bp->b_data, dio->psize);
					if (error == 0) {
						dio->raid6_old_data = kmalloc(
						    dio->psize, M_HAMMER2,
						    M_WAITOK);
						bcopy(dio->bp->b_data,
						    dio->raid6_old_data,
						    dio->psize);
					}
					if (op == HAMMER2_DOP_NEW)
						bzero(dio->bp->b_data,
						    dio->psize);
				}
			} else if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6) {
				/*
				 * Healthy disk: read old data directly.
				 */
				error = breadnx(dio->devvp, dev_pbase,
				    dio->psize, bflags,
				    NULL, NULL, 0, &dio->bp);
				if (dio->bp && error == 0) {
					bkvasync(dio->bp);
					dio->raid6_old_data = kmalloc(
					    dio->psize, M_HAMMER2, M_WAITOK);
					bcopy(dio->bp->b_data,
					    dio->raid6_old_data, dio->psize);
					if (op == HAMMER2_DOP_NEW)
						bzero(dio->bp->b_data,
						    dio->psize);
				} else {
					if (dio->bp == NULL)
						dio->bp = getblk(dio->devvp,
						    dev_pbase, dio->psize,
						    GETBLK_KVABIO, 0);
					if (dio->bp && op == HAMMER2_DOP_NEW) {
						bkvasync(dio->bp);
						bzero(dio->bp->b_data,
						    dio->psize);
					}
				}
			} else {
				dio->bp = getblk(dio->devvp,
						 dev_pbase, dio->psize,
						 GETBLK_KVABIO, 0);
				if (op == HAMMER2_DOP_NEW) {
					bkvasync(dio->bp);
					bzero(dio->bp->b_data, dio->psize);
				}
			}
			atomic_set_long(&dio->refs, HAMMER2_DIO_DIRTY);
			break;
		case HAMMER2_DOP_READ:
		default:
			KKASSERT(dio->bp == NULL);
			if (hce > 0) {
				/*
				 * Synchronous cluster I/O for now.
				 */
				peof = (dio->pbase + HAMMER2_SEGMASK64) &
				       ~HAMMER2_SEGMASK64;
				peof -= dio->dbase;
				error = cluster_readx(dio->devvp,
						     peof, dev_pbase,
						     dio->psize, bflags,
						     dio->psize,
						     HAMMER2_PBUFSIZE*hce,
						     &dio->bp);
			} else {
				error = breadnx(dio->devvp, dev_pbase,
						dio->psize, bflags,
					        NULL, NULL, 0, &dio->bp);
			}
			/*
			 * RAID6: save data just read for RMW delta
			 * parity computation.
			 */
			if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6 &&
			    error == 0 && dio->bp &&
			    dio->raid6_old_data == NULL) {
				bkvasync(dio->bp);
				dio->raid6_old_data = kmalloc(dio->psize,
				    M_HAMMER2, M_WAITOK);
				bcopy(dio->bp->b_data,
				    dio->raid6_old_data, dio->psize);
			}
			break;
		}
	} else {
		if (hce > 0) {
			/*
			 * Synchronous cluster I/O for now.
			 */
			peof = (dio->pbase + HAMMER2_SEGMASK64) &
			       ~HAMMER2_SEGMASK64;
			peof -= dio->dbase;
			error = cluster_readx(dio->devvp,
					      peof, dev_pbase, dio->psize,
					      bflags,
					      dio->psize, HAMMER2_PBUFSIZE*hce,
					      &dio->bp);
		} else {
			error = breadnx(dio->devvp, dev_pbase,
				        dio->psize, bflags,
					NULL, NULL, 0, &dio->bp);
		}
		if (dio->bp) {
			/*
			 * RAID6: save data just read for RMW delta
			 * parity computation (before NEW zeroing).
			 */
			if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6 &&
			    error == 0 &&
			    dio->raid6_old_data == NULL) {
				bkvasync(dio->bp);
				dio->raid6_old_data = kmalloc(dio->psize,
				    M_HAMMER2, M_WAITOK);
				bcopy(dio->bp->b_data,
				    dio->raid6_old_data, dio->psize);
			}

			/*
			 * Handle NEW flags
			 */
			switch(op) {
			case HAMMER2_DOP_NEW:
				bkvasync(dio->bp);
				bzero(hammer2_io_data(dio, lbase), lsize);
				/* fall through */
			case HAMMER2_DOP_NEWNZ:
				atomic_set_long(&dio->refs, HAMMER2_DIO_DIRTY);
				break;
			case HAMMER2_DOP_READ:
			default:
				break;
			}

			/*
			 * Tell the kernel that the buffer cache is not
			 * meta-data based on the btype.  This allows
			 * swapcache to distinguish between data and
			 * meta-data.
			 */
			switch(btype) {
			case HAMMER2_BREF_TYPE_DATA:
				dio->bp->b_flags |= B_NOTMETA;
				break;
			default:
				break;
			}
		}
	}

io_done:
	if (dio->bp) {
		bkvasync(dio->bp);
		BUF_KERNPROC(dio->bp);
		dio->bp->b_flags &= ~B_AGE;
		/* dio->bp->b_debug_info2 = dio; */
	}
	dio->error = error;

	/*
	 * RAID6: on read error, mark disk failed and attempt degraded
	 * reconstruction from surviving disks + parity.
	 */
	if (error && hmp->raid_type == HAMMER2_RAID_TYPE_RAID6) {
		/* Mark this disk as failed (auto-detect from I/O error) */
		if (dio->disk_idx >= 0 && !hmp->raid_failed[dio->disk_idx]) {
			hmp->raid_failed[dio->disk_idx] = 1;
			atomic_add_int(&hmp->raid_nfailed, 1);
			kprintf("hammer2: RAID6 disk %d failed (I/O error)\n",
				dio->disk_idx);
		}
		if (hmp->raid_nfailed <= 2) {
			if (dio->bp) {
				brelse(dio->bp);
				dio->bp = NULL;
			}
			dio->bp = getblk(dio->devvp, dev_pbase, dio->psize,
					 GETBLK_KVABIO, 0);
			if (dio->bp) {
				bkvasync(dio->bp);
				error = hammer2_io_raid6_read_degraded(
						hmp, dio->pbase,
						dio->bp->b_data, dio->psize);
				dio->error = error;
				/*
				 * Save reconstructed data as old_data
				 * for RMW delta parity computation.
				 */
				if (error == 0 &&
				    dio->raid6_old_data == NULL) {
					dio->raid6_old_data = kmalloc(
					    dio->psize, M_HAMMER2, M_WAITOK);
					bcopy(dio->bp->b_data,
					    dio->raid6_old_data, dio->psize);
				}
			}
		}
	}

	/*
	 * Clear INPROG and WAITING, set GOOD wake up anyone waiting.
	 */
	for (;;) {
		orefs = dio->refs;
		cpu_ccfence();
		nrefs = orefs & ~(HAMMER2_DIO_INPROG | HAMMER2_DIO_WAITING);
		if (error == 0)
			nrefs |= HAMMER2_DIO_GOOD;
		if (atomic_cmpset_64(&dio->refs, orefs, nrefs)) {
			if (orefs & HAMMER2_DIO_WAITING)
				wakeup(dio);
			break;
		}
		cpu_pause();
	}

	/* XXX error handling */
	DIO_RECORD(dio HAMMER2_IO_DEBUG_CALL);

	return dio;
}

/*
 * Release our ref on *diop.
 *
 * On the 1->0 transition we clear DIO_GOOD, set DIO_INPROG, and dispose
 * of dio->bp.  Then we clean up DIO_INPROG and DIO_WAITING.
 */
void
_hammer2_io_putblk(hammer2_io_t **diop HAMMER2_IO_DEBUG_ARGS)
{
	hammer2_dev_t *hmp;
	hammer2_io_t *dio;
	struct buf *bp;
	off_t pbase;
	int psize;
	int dio_limit;
	uint64_t orefs;
	uint64_t nrefs;

	dio = *diop;
	*diop = NULL;
	hmp = dio->hmp;
	DIO_RECORD(dio HAMMER2_IO_DEBUG_CALL);

	KKASSERT((dio->refs & HAMMER2_DIO_MASK) != 0);

	/*
	 * Drop refs.
	 *
	 * On the 1->0 transition clear GOOD and set INPROG, and break.
	 * On any other transition we can return early.
	 */
	for (;;) {
		orefs = dio->refs;
		cpu_ccfence();

		if ((orefs & HAMMER2_DIO_MASK) == 1 &&
		    (orefs & HAMMER2_DIO_INPROG) == 0) {
			/*
			 * Lastdrop case, INPROG can be set.  GOOD must be
			 * cleared to prevent the getblk shortcut.
			 */
			nrefs = orefs - 1;
			nrefs &= ~(HAMMER2_DIO_GOOD | HAMMER2_DIO_DIRTY);
			nrefs |= HAMMER2_DIO_INPROG;
			if (atomic_cmpset_64(&dio->refs, orefs, nrefs))
				break;
		} else if ((orefs & HAMMER2_DIO_MASK) == 1) {
			/*
			 * Lastdrop case, INPROG already set.  We must
			 * wait for INPROG to clear.
			 */
			nrefs = orefs | HAMMER2_DIO_WAITING;
			tsleep_interlock(dio, 0);
			if (atomic_cmpset_64(&dio->refs, orefs, nrefs)) {
				tsleep(dio, PINTERLOCKED, "h2dio", hz);
			}
			/* retry */
		} else {
			/*
			 * Normal drop case.
			 */
			nrefs = orefs - 1;
			if (atomic_cmpset_64(&dio->refs, orefs, nrefs))
				return;
			/* retry */
		}
		cpu_pause();
		/* retry */
	}

	/*
	 * Lastdrop (1->0 transition).  INPROG has been set, GOOD and DIRTY
	 * have been cleared.  iofree_count has not yet been incremented,
	 * note that another accessor race will decrement iofree_count so
	 * we have to increment it regardless.
	 * We can now dispose of the buffer.
	 */
	pbase = dio->pbase;
	psize = dio->psize;
	bp = dio->bp;
	dio->bp = NULL;

	if ((orefs & HAMMER2_DIO_GOOD) && bp) {
		/*
		 * Non-errored disposal of bp
		 */
		if (orefs & HAMMER2_DIO_DIRTY) {
			char *raid6_data = NULL;

			dio_write_stats_update(dio, bp);

			/*
			 * RAID6: save the data column to a local buffer
			 * before releasing bp.  Parity is computed after
			 * bp is released so that sibling reads can use
			 * blocking breadnx without deadlocking against
			 * this bp.
			 */
			if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6) {
				bkvasync(bp);
				raid6_data = kmalloc(psize, M_HAMMER2,
						     M_WAITOK);
				bcopy(bp->b_data, raid6_data, psize);
			}

			/*
			 * RAID6 degraded: if this DIO's device is a failed
			 * disk, skip writing the data column to it.
			 * Parity is updated below after bp is released.
			 */
			if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6 &&
			    hmp->raid_nfailed > 0 &&
			    dio->disk_idx >= 0 &&
			    hmp->raid_failed[dio->disk_idx]) {
				brelse(bp);
				bp = NULL;
			}

			/*
			 * Allows dirty buffers to accumulate and
			 * possibly be canceled (e.g. by a 'rm'),
			 * by default we will burst-write later.
			 *
			 * We generally do NOT want to issue an actual
			 * b[a]write() or cluster_write() here.  Due to
			 * the way chains are locked, buffers may be cycled
			 * in and out quite often and disposal here can cause
			 * multiple writes or write-read stalls.
			 *
			 * If FLUSH is set we do want to issue the actual
			 * write.  This typically occurs in the write-behind
			 * case when writing to large files.
			 */
			if (bp) {
				off_t peof;
				int hce;

				/*
				 * Debug: log writes to failed disk devvps
				 */
				if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6 &&
				    dio->disk_idx >= 0 &&
				    !hmp->volumes[dio->disk_idx].dev->open) {
					kprintf("hammer2: WARNING putblk "
					    "bdwrite to closed disk %d "
					    "pbase=%jx\n",
					    dio->disk_idx,
					    (intmax_t)pbase);
				}

				if (dio->refs & HAMMER2_DIO_FLUSH) {
					/*
					 * Degraded mode: use synchronous
					 * bwrite to prevent runningbufspace
					 * accumulation.  Async data bawrites
					 * can exhaust the buffer cache and
					 * deadlock against the vn device's
					 * UFS backing store.
					 */
					if (hmp->raid_nfailed > 0) {
						bp->b_flags &= ~B_CLUSTEROK;
						bwrite(bp);
					} else if ((hce = hammer2_cluster_write) != 0) {
						peof = (pbase + HAMMER2_SEGMASK64) &
						       ~HAMMER2_SEGMASK64;
						peof -= dio->dbase;
						bp->b_flags |= B_CLUSTEROK;
						cluster_write(bp, peof, psize, hce);
					} else {
						bp->b_flags &= ~B_CLUSTEROK;
						bawrite(bp);
					}
				} else {
					bp->b_flags &= ~B_CLUSTEROK;
					bdwrite(bp);
				}
			}
			/*
			 * RAID6 parity update.
			 *
			 * Degraded mode: process parity inline (synchronous)
			 * so that subsequent degraded reads reconstruct the
			 * correct data.  The buffer has been released above
			 * (brelse for failed disk, bdwrite for healthy), so
			 * no vn device locks are held — blocking sibling
			 * reads are safe here.
			 *
			 * Healthy mode: enqueue to background parity thread
			 * as before (data stays in buffer cache via bdwrite,
			 * so reads don't need parity reconstruction).
			 */
			if (raid6_data && hmp->raid_nfailed > 0) {
				hammer2_io_raid6_write(hmp, pbase,
				    raid6_data, dio->raid6_old_data,
				    psize);
				/*
				 * Track stripes written during an active
				 * resilver so the resilver can do a second
				 * pass over them after a sync.
				 */
				if (hmp->resilver_running) {
					uint64_t sn = pbase /
					    ((uint64_t)hmp->raid_config.ndata *
					     hmp->raid_config.stripe_unit);
					hammer2_spin_ex(&hmp->io_spin);
					if (sn < hmp->resilver_dirty_lo)
						hmp->resilver_dirty_lo = sn;
					if (sn > hmp->resilver_dirty_hi)
						hmp->resilver_dirty_hi = sn;
					hammer2_spin_unex(&hmp->io_spin);
				}
				kfree(raid6_data, M_HAMMER2);
				if (dio->raid6_old_data) {
					kfree(dio->raid6_old_data, M_HAMMER2);
					dio->raid6_old_data = NULL;
				}
				raid6_data = NULL;
			} else if (raid6_data) {
				hammer2_parity_work_t *pw;

				pw = kmalloc(sizeof(*pw), M_HAMMER2, M_WAITOK);
				pw->pbase = pbase;
				pw->psize = psize;
				pw->data = raid6_data;
				pw->old_data = dio->raid6_old_data;
				dio->raid6_old_data = NULL;
				spin_lock(&hmp->raid6_parity_spin);
				TAILQ_INSERT_TAIL(&hmp->raid6_parity_q,
				    pw, entry);
				spin_unlock(&hmp->raid6_parity_spin);
				wakeup(&hmp->raid6_parity_q);
			}
		} else if (bp->b_flags & (B_ERROR | B_INVAL | B_RELBUF)) {
			brelse(bp);
		} else {
			bqrelse(bp);
		}
	} else if (bp) {
		/*
		 * Errored disposal of bp
		 */
		brelse(bp);
	}

	/*
	 * RAID6: free old_data if not transferred to parity work item
	 */
	if (dio->raid6_old_data) {
		kfree(dio->raid6_old_data, M_HAMMER2);
		dio->raid6_old_data = NULL;
	}

	/*
	 * Update iofree_count before disposing of the dio
	 */
	hmp = dio->hmp;
	atomic_add_int(&hmp->iofree_count, 1);

	/*
	 * Clear INPROG, GOOD, and WAITING (GOOD should already be clear).
	 *
	 * Also clear FLUSH as it was handled above.
	 */
	for (;;) {
		orefs = dio->refs;
		cpu_ccfence();
		nrefs = orefs & ~(HAMMER2_DIO_INPROG | HAMMER2_DIO_GOOD |
				  HAMMER2_DIO_WAITING | HAMMER2_DIO_FLUSH);
		if (atomic_cmpset_64(&dio->refs, orefs, nrefs)) {
			if (orefs & HAMMER2_DIO_WAITING)
				wakeup(dio);
			break;
		}
		cpu_pause();
	}

	/*
	 * We cache free buffers so re-use cases can use a shared lock, but
	 * if too many build up we have to clean them out.
	 */
	dio_limit = hammer2_dio_limit;
	if (dio_limit < 256)
		dio_limit = 256;
	if (dio_limit > 1024*1024)
		dio_limit = 1024*1024;
	if (hmp->iofree_count > dio_limit) {
		struct hammer2_cleanupcb_info info;

		RB_INIT(&info.tmptree);
		hammer2_spin_ex(&hmp->io_spin);
		if (hmp->iofree_count > dio_limit) {
			info.count = hmp->iofree_count / 5;
			RB_SCAN(hammer2_io_tree, &hmp->iotree, NULL,
				hammer2_io_cleanup_callback, &info);
		}
		hammer2_spin_unex(&hmp->io_spin);
		hammer2_io_cleanup(hmp, &info.tmptree);
	}
}

/*
 * Cleanup any dio's with (INPROG | refs) == 0.
 */
static
int
hammer2_io_cleanup_callback(hammer2_io_t *dio, void *arg)
{
	struct hammer2_cleanupcb_info *info = arg;
	hammer2_io_t *xio __debugvar;

	if ((dio->refs & (HAMMER2_DIO_MASK | HAMMER2_DIO_INPROG)) == 0) {
		if (dio->act > 0) {
			int act;

			act = dio->act - (ticks - dio->ticks) / hz - 1;
			if (act > 0) {
				dio->act = act;
				return 0;
			}
			dio->act = 0;
		}
		KKASSERT(dio->bp == NULL);
		if (info->count > 0) {
			RB_REMOVE(hammer2_io_tree, &dio->hmp->iotree, dio);
			xio = RB_INSERT(hammer2_io_tree, &info->tmptree, dio);
			KKASSERT(xio == NULL);
			--info->count;
		}
	}
	return 0;
}

void
hammer2_io_cleanup(hammer2_dev_t *hmp, struct hammer2_io_tree *tree)
{
	hammer2_io_t *dio;

	while ((dio = RB_ROOT(tree)) != NULL) {
		RB_REMOVE(hammer2_io_tree, tree, dio);
		KKASSERT(dio->bp == NULL &&
		    (dio->refs & (HAMMER2_DIO_MASK | HAMMER2_DIO_INPROG)) == 0);
		if (dio->refs & HAMMER2_DIO_DIRTY) {
			kprintf("hammer2_io_cleanup: Dirty buffer "
				"%016jx/%d (bp=%p)\n",
				dio->pbase, dio->psize, dio->bp);
		}
		if (dio->raid6_old_data) {
			kfree(dio->raid6_old_data, M_HAMMER2);
			dio->raid6_old_data = NULL;
		}
		kfree_obj(dio, hmp->mio);
		atomic_add_int(&hammer2_dio_count, -1);
		atomic_add_int(&hmp->iofree_count, -1);
	}
}

/*
 * Returns a pointer to the requested data.
 */
char *
hammer2_io_data(hammer2_io_t *dio, off_t lbase)
{
	struct buf *bp;
	int off;

	bp = dio->bp;
	KKASSERT(bp != NULL);
	bkvasync(bp);
	lbase -= dio->dbase;
	off = (lbase & ~HAMMER2_OFF_MASK_RADIX) - bp->b_loffset;
	KKASSERT(off >= 0 && off < bp->b_bufsize);
	return(bp->b_data + off);
}

int
hammer2_io_new(hammer2_dev_t *hmp, int btype, off_t lbase, int lsize,
	       hammer2_io_t **diop)
{
	*diop = hammer2_io_getblk(hmp, btype, lbase, lsize, HAMMER2_DOP_NEW);
	return ((*diop)->error);
}

int
hammer2_io_newnz(hammer2_dev_t *hmp, int btype, off_t lbase, int lsize,
		 hammer2_io_t **diop)
{
	*diop = hammer2_io_getblk(hmp, btype, lbase, lsize, HAMMER2_DOP_NEWNZ);
	return ((*diop)->error);
}

int
_hammer2_io_bread(hammer2_dev_t *hmp, int btype, off_t lbase, int lsize,
		hammer2_io_t **diop HAMMER2_IO_DEBUG_ARGS)
{
#ifdef HAMMER2_IO_DEBUG
	hammer2_io_t *dio;
#endif

	*diop = _hammer2_io_getblk(hmp, btype, lbase, lsize,
				   HAMMER2_DOP_READ HAMMER2_IO_DEBUG_CALL);
#ifdef HAMMER2_IO_DEBUG
	if ((dio = *diop) != NULL) {
#if 0
		int i = (dio->debug_index - 1) & HAMMER2_IO_DEBUG_MASK;
		dio->debug_data[i] = debug_data;
#endif
	}
#endif
	return ((*diop)->error);
}

hammer2_io_t *
_hammer2_io_getquick(hammer2_dev_t *hmp, off_t lbase,
		     int lsize HAMMER2_IO_DEBUG_ARGS)
{
	hammer2_io_t *dio;

	dio = _hammer2_io_getblk(hmp, 0, lbase, lsize,
				 HAMMER2_DOP_READQ HAMMER2_IO_DEBUG_CALL);
	return dio;
}

void
_hammer2_io_bawrite(hammer2_io_t **diop HAMMER2_IO_DEBUG_ARGS)
{
	atomic_set_64(&(*diop)->refs, HAMMER2_DIO_DIRTY |
				      HAMMER2_DIO_FLUSH);
	_hammer2_io_putblk(diop HAMMER2_IO_DEBUG_CALL);
}

void
_hammer2_io_bdwrite(hammer2_io_t **diop HAMMER2_IO_DEBUG_ARGS)
{
	atomic_set_64(&(*diop)->refs, HAMMER2_DIO_DIRTY);
	_hammer2_io_putblk(diop HAMMER2_IO_DEBUG_CALL);
}

int
_hammer2_io_bwrite(hammer2_io_t **diop HAMMER2_IO_DEBUG_ARGS)
{
	atomic_set_64(&(*diop)->refs, HAMMER2_DIO_DIRTY |
				      HAMMER2_DIO_FLUSH);
	_hammer2_io_putblk(diop HAMMER2_IO_DEBUG_CALL);
	return (0);	/* XXX */
}

void
hammer2_io_setdirty(hammer2_io_t *dio)
{
	atomic_set_64(&dio->refs, HAMMER2_DIO_DIRTY);
}

/*
 * This routine is called when a MODIFIED chain is being DESTROYED,
 * in an attempt to allow the related buffer cache buffer to be
 * invalidated and discarded instead of flushing it to disk.
 *
 * At the moment this case is only really useful for file meta-data.
 * File data is already handled via the logical buffer cache associated
 * with the vnode, and will be discarded if it was never flushed to disk.
 * File meta-data may include inodes, directory entries, and indirect blocks.
 *
 * XXX
 * However, our DIO buffers are PBUFSIZE'd (64KB), and the area being
 * invalidated might be smaller.  Most of the meta-data structures above
 * are in the 'smaller' category.  For now, don't try to invalidate the
 * data areas.
 */
void
hammer2_io_inval(hammer2_io_t *dio, hammer2_off_t data_off, u_int bytes)
{
	/* NOP */
}

void
_hammer2_io_brelse(hammer2_io_t **diop HAMMER2_IO_DEBUG_ARGS)
{
	_hammer2_io_putblk(diop HAMMER2_IO_DEBUG_CALL);
}

void
_hammer2_io_bqrelse(hammer2_io_t **diop HAMMER2_IO_DEBUG_ARGS)
{
	_hammer2_io_putblk(diop HAMMER2_IO_DEBUG_CALL);
}

/*
 * Set dedup validation bits in a DIO.  We do not need the buffer cache
 * buffer for this.  This must be done concurrent with setting bits in
 * the freemap so as to interlock with bulkfree's clearing of those bits.
 */
void
hammer2_io_dedup_set(hammer2_dev_t *hmp, hammer2_blockref_t *bref)
{
	hammer2_io_t *dio;
	uint64_t mask;
	int lsize;
	int isgood;

	dio = hammer2_io_alloc(hmp, bref->data_off, bref->type, 1, &isgood);
	if ((int)(bref->data_off & HAMMER2_OFF_MASK_RADIX))
		lsize = 1 << (int)(bref->data_off & HAMMER2_OFF_MASK_RADIX);
	else
		lsize = 0;
	mask = hammer2_dedup_mask(dio, bref->data_off, lsize);
	atomic_clear_64(&dio->dedup_valid, mask);
	atomic_set_64(&dio->dedup_alloc, mask);
	hammer2_io_putblk(&dio);
}

/*
 * Clear dedup validation bits in a DIO.  This is typically done when
 * a modified chain is destroyed or by the bulkfree code.  No buffer
 * is needed for this operation.  If the DIO no longer exists it is
 * equivalent to the bits not being set.
 */
void
hammer2_io_dedup_delete(hammer2_dev_t *hmp, uint8_t btype,
			hammer2_off_t data_off, u_int bytes)
{
	hammer2_io_t *dio;
	uint64_t mask;
	int isgood;

	if ((data_off & ~HAMMER2_OFF_MASK_RADIX) == 0)
		return;
	if (btype != HAMMER2_BREF_TYPE_DATA)
		return;
	dio = hammer2_io_alloc(hmp, data_off, btype, 0, &isgood);
	if (dio) {
		if (data_off < dio->pbase ||
		    (data_off & ~HAMMER2_OFF_MASK_RADIX) + bytes >
		    dio->pbase + dio->psize) {
			panic("hammer2_io_dedup_delete: DATAOFF BAD "
			      "%016jx/%d %016jx\n",
			      data_off, bytes, dio->pbase);
		}
		mask = hammer2_dedup_mask(dio, data_off, bytes);
		atomic_clear_64(&dio->dedup_alloc, mask);
		atomic_clear_64(&dio->dedup_valid, mask);
		hammer2_io_putblk(&dio);
	}
}

/*
 * Assert that dedup allocation bits in a DIO are not set.  This operation
 * does not require a buffer.  The DIO does not need to exist.
 */
void
hammer2_io_dedup_assert(hammer2_dev_t *hmp, hammer2_off_t data_off, u_int bytes)
{
	hammer2_io_t *dio;
	int isgood;

	dio = hammer2_io_alloc(hmp, data_off, HAMMER2_BREF_TYPE_DATA,
			       0, &isgood);
	if (dio) {
		KASSERT((dio->dedup_alloc &
			  hammer2_dedup_mask(dio, data_off, bytes)) == 0,
			("hammer2_dedup_assert: %016jx/%d %016jx/%016jx",
			data_off,
			bytes,
			hammer2_dedup_mask(dio, data_off, bytes),
			dio->dedup_alloc));
		hammer2_io_putblk(&dio);
	}
}

static
void
dio_write_stats_update(hammer2_io_t *dio, struct buf *bp)
{
	if (bp->b_flags & B_DELWRI)
		return;
	hammer2_adjwritecounter(dio->btype, dio->psize);
}

void
hammer2_io_bkvasync(hammer2_io_t *dio)
{
	KKASSERT(dio->bp != NULL);
	bkvasync(dio->bp);
}

/*
 * Ref a dio that is already owned
 */
void
_hammer2_io_ref(hammer2_io_t *dio HAMMER2_IO_DEBUG_ARGS)
{
	DIO_RECORD(dio HAMMER2_IO_DEBUG_CALL);
	atomic_add_64(&dio->refs, 1);
}

/*
 * RAID6 parity write.
 *
 * Called from the parity background thread.  logical_off is the HAMMER2
 * logical base of the stripe unit that was just written; data contains
 * the bytes written to that column (stripe_unit bytes).
 *
 * Reads all sibling data columns from their physical devices:
 *  - If the sibling disk is healthy: blocking breadnx read.
 *  - If the sibling disk is failed and old_data is available: use RMW
 *    delta (P_new = P_old XOR D_old XOR D_new) instead of reconstructing
 *    the failed column.  The failed disk's data cancels out in the XOR.
 *  - If old_data is NULL (healthy mode): full-stripe gen_syndrome as before.
 *
 * No HAMMER2 locks are held.  Returns 0 on success.
 */
int
hammer2_io_raid6_write(hammer2_dev_t *hmp, hammer2_off_t logical_off,
		       void *data, void *old_data, size_t bytes)
{
	hammer2_raid_config_t *rc = &hmp->raid_config;
	int ndisks = rc->ndisks;
	int ndata = rc->ndata;
	uint64_t stripe_unit = rc->stripe_unit;
	uint64_t stripe_num;
	int my_col;
	int p_disk, q_disk;
	hammer2_off_t phys_off;
	void *col_bufs[HAMMER2_MAX_VOLUMES];
	void *ptrs[HAMMER2_MAX_VOLUMES];
	hammer2_volume_t *vol;
	struct buf *bp;
	int fail_data_a = -1, fail_data_b = -1;
	int fail_p = 0, fail_q = 0;
	int phys_disk, di, col;
	int i;

	KKASSERT(hmp->raid_type == HAMMER2_RAID_TYPE_RAID6);
	KKASSERT(bytes == (size_t)stripe_unit);

	stripe_num = logical_off / ((uint64_t)ndata * stripe_unit);
	my_col     = (int)((logical_off / stripe_unit) % (uint64_t)ndata);
	p_disk     = (int)(stripe_num % ndisks);
	q_disk     = (p_disk + 1) % ndisks;
	phys_off   = HAMMER2_ZONE_SEG64 + stripe_num * stripe_unit;

	for (i = 0; i < ndisks; i++)
		col_bufs[i] = NULL;

	di = 0;
	for (phys_disk = 0; phys_disk < ndisks; phys_disk++) {
		if (phys_disk == p_disk)
			col = ndata;
		else if (phys_disk == q_disk)
			col = ndata + 1;
		else
			col = di++;

		col_bufs[col] = kmalloc(stripe_unit, M_HAMMER2,
					M_WAITOK | M_ZERO);
		ptrs[col] = col_bufs[col];

		if (col == my_col) {
			/*
			 * This is the column we are updating.  Use the
			 * data supplied by the caller directly.
			 */
			bcopy(data, col_bufs[col], stripe_unit);
		} else if (hmp->raid_failed[phys_disk]) {
			/* Already failed: track for recovery */
			if (col < ndata) {
				if (fail_data_a == -1)
					fail_data_a = col;
				else if (fail_data_b == -1)
					fail_data_b = col;
			} else if (col == ndata) {
				fail_p = 1;
			} else {
				fail_q = 1;
			}
		} else {
			/*
			 * Healthy sibling.  When old_data is available for
			 * RMW delta, we only need P_old and Q_old — skip
			 * reading data columns to reduce buffer cache
			 * pressure.  Without this optimization, excessive
			 * breadnx calls can exhaust the buffer cache and
			 * deadlock against the vn device's UFS backing
			 * store.
			 */
			if (old_data != NULL && col < ndata) {
				/* RMW delta doesn't need sibling data */
				continue;
			}
			vol = &hmp->volumes[phys_disk];
			bp = NULL;
			if (breadnx(vol->dev->devvp, phys_off,
				    stripe_unit, 0, NULL, NULL, 0, &bp) == 0 &&
			    bp != NULL) {
				bkvasync(bp);
				bcopy(bp->b_data, col_bufs[col], stripe_unit);
				brelse(bp);
			} else {
				if (bp)
					brelse(bp);
				/* I/O error: track for recovery */
				if (col < ndata) {
					if (fail_data_a == -1)
						fail_data_a = col;
					else if (fail_data_b == -1)
						fail_data_b = col;
				} else if (col == ndata) {
					fail_p = 1;
				} else {
					fail_q = 1;
				}
			}
		}
	}

	/*
	 * RMW delta path: when old_data is available, apply delta
	 * directly to P_old and Q_old.  This is always correct and
	 * avoids two problems:
	 *  1) Degraded mode: we can't read the failed sibling column.
	 *  2) Healthy mode: sibling's bdwrite may not have flushed,
	 *     so reading it from disk gets stale data — corrupting
	 *     parity when both columns in a stripe are modified.
	 *
	 * P_new = P_old XOR D_old XOR D_new
	 * Q_new = Q_old XOR gf_mul(2^my_col, D_old XOR D_new)
	 */
	if (old_data != NULL) {
		uint8_t *od = (uint8_t *)old_data;
		uint8_t *nd = (uint8_t *)data;
		uint8_t coeff = hammer2_gf_exp[my_col];
		uint8_t *p;
		uint8_t *q;
		size_t si;

		p = col_bufs[ndata];
		if (!fail_p && p != NULL) {
			for (si = 0; si < stripe_unit; si++)
				p[si] ^= od[si] ^ nd[si];
		}
		q = col_bufs[ndata + 1];
		if (!fail_q && q != NULL) {
			for (si = 0; si < stripe_unit; si++)
				q[si] ^= hammer2_gf_mul(coeff,
						od[si] ^ nd[si]);
		}
		goto parity_write;
	}

	/*
	 * If we have failed data columns but no old_data (shouldn't
	 * happen in practice), fall back to reconstruction.
	 */
	if (fail_data_a != -1) {
		if (fail_data_b != -1) {
			/* Two data disks failed - requires BOTH P and Q */
			if (fail_p || fail_q) {
				kprintf("hammer2: RAID6 write: too many "
					"failures (data %d,%d parity %d,%d)\n",
					fail_data_a, fail_data_b, fail_p, fail_q);
			} else {
				if (fail_data_a > fail_data_b) {
					int tmp = fail_data_a;
					fail_data_a = fail_data_b;
					fail_data_b = tmp;
				}
				hammer2_raid6_dual_recov(ndisks, stripe_unit,
							 fail_data_a,
							 fail_data_b, ptrs);
			}
		} else {
			/* One data disk failed - requires either P or Q */
			if (fail_p) {
				if (fail_q) {
					kprintf("hammer2: RAID6 write: too "
						"many failures (data %d parity P,Q)\n",
						fail_data_a);
				} else {
					hammer2_raid6_datap_recov(ndisks,
								  stripe_unit,
								  fail_data_a,
								  ptrs);
				}
			} else {
				hammer2_raid6_dual_recov(ndisks, stripe_unit,
							 fail_data_a,
							 ndisks - 1, ptrs);
			}
		}
	}

	/* Compute NEW P and Q in-place in col_bufs[ndata] and col_bufs[ndata+1] */
	hammer2_raid6_gen_syndrome(ndisks, stripe_unit, ptrs);

parity_write:
	/*
	 * Write out parity columns (P and Q) to healthy disks only.
	 * Data columns are handled by the main thread via standard
	 * buffer cache disposal.
	 *
	 * Use synchronous bwrite() when called inline from the
	 * degraded flush path.  This prevents runningbufspace
	 * accumulation that can deadlock against the vn device's
	 * UFS backing store (buffer cache exhaustion).  The
	 * background parity thread uses bawrite() for throughput.
	 */
	di = 0;
	for (phys_disk = 0; phys_disk < ndisks; phys_disk++) {
		if (phys_disk == p_disk)
			col = ndata;
		else if (phys_disk == q_disk)
			col = ndata + 1;
		else
			col = di++;

		/* Only write parity columns here */
		if (col < ndata)
			continue;

		/* Skip write if disk is failed */
		if (hmp->raid_failed[phys_disk])
			continue;

		vol = &hmp->volumes[phys_disk];
		bp = getblk(vol->dev->devvp, phys_off,
			    stripe_unit, GETBLK_KVABIO, 0);
		if (bp) {
			bkvasync(bp);
			bcopy(col_bufs[col], bp->b_data, stripe_unit);
			if (hmp->raid_nfailed > 0)
				bwrite(bp);
			else
				bawrite(bp);
		}
	}

	for (i = 0; i < ndisks; i++) {
		if (col_bufs[i])
			kfree(col_bufs[i], M_HAMMER2);
	}

	return 0;
}

/*
 * RAID6 parity background thread.
 *
 * Processes queued parity work items.  Runs with no HAMMER2 locks
 * held, so blocking breadnx reads for sibling columns are safe.
 */
static void
hammer2_parity_thread(void *arg)
{
	hammer2_dev_t *hmp = arg;
	hammer2_parity_work_t *pw;

	for (;;) {
		spin_lock(&hmp->raid6_parity_spin);
		pw = TAILQ_FIRST(&hmp->raid6_parity_q);
		if (pw != NULL) {
			TAILQ_REMOVE(&hmp->raid6_parity_q, pw, entry);
			hmp->raid6_parity_processing = 1;
			spin_unlock(&hmp->raid6_parity_spin);
			hammer2_io_raid6_write(hmp, pw->pbase, pw->data,
					       pw->old_data, pw->psize);
			kfree(pw->data, M_HAMMER2);
			if (pw->old_data)
				kfree(pw->old_data, M_HAMMER2);
			kfree(pw, M_HAMMER2);
			spin_lock(&hmp->raid6_parity_spin);
			hmp->raid6_parity_processing = 0;
			wakeup(&hmp->raid6_parity_q);
			spin_unlock(&hmp->raid6_parity_spin);
			continue;
		}
		if (hmp->raid6_parity_exiting) {
			spin_unlock(&hmp->raid6_parity_spin);
			hmp->raid6_parity_td = NULL;
			wakeup(&hmp->raid6_parity_td);
			lwkt_exit();
			/* NOTREACHED */
		}
		tsleep_interlock(&hmp->raid6_parity_q, 0);
		spin_unlock(&hmp->raid6_parity_spin);
		tsleep(&hmp->raid6_parity_q, PINTERLOCKED, "h2par", hz);
	}
}

/*
 * Drain the parity work queue.
 *
 * Waits until the queue is empty AND the parity thread is not processing
 * any work item.  Called from hammer2_ioctl_raid_fail_disk before marking
 * a disk as failed, to ensure all pending parity writes complete using
 * the still-healthy disk layout before the failed flag is visible.
 */
void
hammer2_parity_drain(hammer2_dev_t *hmp)
{
	if (hmp->raid_type != HAMMER2_RAID_TYPE_RAID6 ||
	    hmp->raid6_parity_td == NULL)
		return;

	for (;;) {
		spin_lock(&hmp->raid6_parity_spin);
		if (TAILQ_EMPTY(&hmp->raid6_parity_q) &&
		    !hmp->raid6_parity_processing) {
			spin_unlock(&hmp->raid6_parity_spin);
			break;
		}
		tsleep_interlock(&hmp->raid6_parity_q, 0);
		spin_unlock(&hmp->raid6_parity_spin);
		tsleep(&hmp->raid6_parity_q, PINTERLOCKED, "h2pdr", hz);
	}
}

void
hammer2_parity_init(hammer2_dev_t *hmp)
{
	if (hmp->raid_type != HAMMER2_RAID_TYPE_RAID6)
		return;
	spin_init(&hmp->raid6_parity_spin, "h2par");
	TAILQ_INIT(&hmp->raid6_parity_q);
	hmp->raid6_parity_exiting = 0;
	hmp->raid6_parity_td = NULL;
	lwkt_create(hammer2_parity_thread, hmp, &hmp->raid6_parity_td,
		    NULL, 0, -1, "h2par-%s", hmp->devrepname);
}

void
hammer2_parity_uninit(hammer2_dev_t *hmp)
{
	if (hmp->raid_type != HAMMER2_RAID_TYPE_RAID6 ||
	    hmp->raid6_parity_td == NULL)
		return;
	/* Signal exit, wake thread */
	spin_lock(&hmp->raid6_parity_spin);
	hmp->raid6_parity_exiting = 1;
	spin_unlock(&hmp->raid6_parity_spin);
	wakeup(&hmp->raid6_parity_q);
	/* Wait for thread to drain queue and exit */
	while (hmp->raid6_parity_td != NULL)
		tsleep(&hmp->raid6_parity_td, 0, "h2parx", hz);
	spin_uninit(&hmp->raid6_parity_spin);
}

/*
 * RAID 6 degraded read.
 *
 * Read a data block from a RAID 6 array in degraded mode.
 * When the target disk has failed, reconstruct the data from
 * surviving disks + parity.
 *
 * Returns 0 on success, error code on failure.
 * The reconstructed data is placed in the provided buffer.
 */
int
hammer2_io_raid6_read_degraded(hammer2_dev_t *hmp, hammer2_off_t logical_off,
			       void *buf, size_t bytes)
{
	hammer2_raid_config_t *rc = &hmp->raid_config;
	int ndisks = rc->ndisks;
	int ndata = rc->ndata;
	uint64_t stripe_unit = rc->stripe_unit;
	uint64_t stripe_num;
	int p_disk, q_disk;
	int target_col;
	int phys_disk, di, col;
	hammer2_off_t phys_off;
	void *ptrs[HAMMER2_MAX_VOLUMES];
	void *col_bufs[HAMMER2_MAX_VOLUMES];
	hammer2_volume_t *vol;
	struct buf *bp;
	int error = 0;
	int fail_data_a = -1, fail_data_b = -1;
	int fail_p = 0, fail_q = 0;

	KKASSERT(bytes == stripe_unit);
	KKASSERT(hmp->raid_type == HAMMER2_RAID_TYPE_RAID6);

	stripe_num = logical_off / ((uint64_t)ndata * stripe_unit);

	/* P and Q disk positions */
	p_disk = (int)(stripe_num % ndisks);
	q_disk = (p_disk + 1) % ndisks;

	/* Logical column index of the block we're reconstructing */
	target_col = (int)((logical_off / stripe_unit) % ndata);

	kprintf("h2r6deg: loff=0x%016jx stripe=%llu tgt_col=%d "
		"p_disk=%d q_disk=%d\n",
		(uintmax_t)logical_off, (unsigned long long)stripe_num,
		target_col, p_disk, q_disk);

	/*
	 * Read all columns in the stripe (data + P + Q) in one loop.
	 */
	phys_off = HAMMER2_ZONE_SEG64 + stripe_num * stripe_unit;
	di = 0;
	for (phys_disk = 0; phys_disk < ndisks; phys_disk++) {
		int lerror;
		if (phys_disk == p_disk)
			col = ndata;
		else if (phys_disk == q_disk)
			col = ndata + 1;
		else
			col = di++;

		col_bufs[col] = kmalloc(stripe_unit, M_HAMMER2,
					M_WAITOK | M_ZERO);
		ptrs[col] = col_bufs[col];

		if (col == target_col) {
			/*
			 * Target column being reconstructed is already
			 * locked by caller. Mark failed to reconstruct.
			 */
			if (fail_data_a == -1)
				fail_data_a = col;
			else if (fail_data_b == -1)
				fail_data_b = col;
			continue;
		}

		if (hmp->raid_failed[phys_disk]) {
			/* Already failed: track for recovery */
			if (col < ndata) {
				if (fail_data_a == -1)
					fail_data_a = col;
				else if (fail_data_b == -1)
					fail_data_b = col;
			} else if (col == ndata) {
				fail_p = 1;
			} else {
				fail_q = 1;
			}
			continue;
		}

		vol = &hmp->volumes[phys_disk];
		bp = NULL;
		lerror = breadnx(vol->dev->devvp, phys_off,
				 stripe_unit, 0, NULL, NULL, 0, &bp);
		if (lerror == 0 && bp) {
			bkvasync(bp);
			bcopy(bp->b_data, col_bufs[col], stripe_unit);
			brelse(bp);
		} else {
			if (bp)
				brelse(bp);
			/* I/O error: track for recovery */
			if (col < ndata) {
				if (fail_data_a == -1)
					fail_data_a = col;
				else if (fail_data_b == -1)
					fail_data_b = col;
			} else if (col == ndata) {
				fail_p = 1;
			} else {
				fail_q = 1;
			}
		}
	}

	kprintf("h2r6deg: after read: fail_a=%d fail_b=%d fail_p=%d fail_q=%d\n",
		fail_data_a, fail_data_b, fail_p, fail_q);

	/* Perform recovery */
	error = 0;
	if (fail_data_a != -1) {
		if (fail_data_b != -1) {
			/* Two data disks failed - requires BOTH P and Q */
			if (fail_p || fail_q) {
				error = EIO;
			} else {
				if (fail_data_a > fail_data_b) {
					int tmp = fail_data_a;
					fail_data_a = fail_data_b;
					fail_data_b = tmp;
				}
				hammer2_raid6_dual_recov(ndisks, stripe_unit,
							 fail_data_a,
							 fail_data_b, ptrs);
			}
		} else {
			/* One data disk failed - requires either P or Q */
			if (fail_p) {
				if (fail_q) {
					/* Both P and Q failed */
					error = EIO;
				} else {
					/* Data fail_data_a + P failed: use Q */
					hammer2_raid6_datap_recov(ndisks,
								  stripe_unit,
								  fail_data_a,
								  ptrs);
				}
			} else {
				/* Use P to recover fail_data_a */
				hammer2_raid6_dual_recov(ndisks, stripe_unit,
							 fail_data_a,
							 ndisks - 1, ptrs);
			}
		}
	}

	/* If target_col failed and reconstruction was impossible, return error */
	if (error == 0) {
		bcopy(ptrs[target_col], buf, stripe_unit);
	}
	kprintf("h2r6deg: done error=%d phys_off=0x%016jx\n",
		error, (uintmax_t)(HAMMER2_ZONE_SEG64 + stripe_num * stripe_unit));

	/* Free temporary buffers */
	for (col = 0; col <= ndata + 1; col++) {
		if (col_bufs[col])
			kfree(col_bufs[col], M_HAMMER2);
	}

	return error;
}

/*
 * RAID 6 online resilver.
 *
 * Reconstruct all data for the disk at failed_disk_idx onto new_devvp.
 * Reads all surviving columns for each stripe, reconstructs the failed
 * column (data, P, or Q depending on stripe rotation), and writes it to
 * the replacement device.
 *
 * Also copies the volume header from a surviving disk to new_devvp.
 *
 * Returns 0 on success, errno on failure.
 */
int
hammer2_io_raid6_resilver(hammer2_dev_t *hmp, hammer2_pfs_t *pmp,
			  int failed_disk_idx, struct vnode *new_devvp)
{
	hammer2_raid_config_t *rc = &hmp->raid_config;
	int ndisks = rc->ndisks;
	int ndata = rc->ndata;
	uint64_t stripe_unit = rc->stripe_unit;
	uint64_t num_stripes;
	uint64_t stripe_num;
	int p_disk, q_disk;
	int phys_disk, di, col;
	int failed_col, other_failed_col;
	int phys_to_lcol[HAMMER2_MAX_VOLUMES];
	hammer2_off_t phys_off;
	void *ptrs[HAMMER2_MAX_VOLUMES];
	void *col_bufs[HAMMER2_MAX_VOLUMES];
	hammer2_volume_t *vol;
	struct buf *bp, *wbp;
	int error = 0;
	int i;

	KKASSERT(hmp->raid_type == HAMMER2_RAID_TYPE_RAID6);
	KKASSERT(stripe_unit > 0);
	KKASSERT(ndata > 0);
	KKASSERT(failed_disk_idx >= 0 && failed_disk_idx < ndisks);

	num_stripes = rc->array_size / ((uint64_t)ndata * stripe_unit);

	/* Publish progress so status ioctl can be polled from userspace */
	hmp->resilver_disk_idx = failed_disk_idx;
	hmp->resilver_stripes_total = num_stripes;
	hmp->resilver_stripes_done = 0;
	hmp->resilver_running = 1;

	/*
	 * Phase 1: Copy the volume header from a surviving disk to new_devvp.
	 * Volume headers live in the first HAMMER2_ZONE_SEG (4MB) on each
	 * physical disk.  We copy the primary header at offset 0.
	 */
	{
		int surv = -1;
		for (i = 0; i < ndisks; i++) {
			if (i != failed_disk_idx && !hmp->raid_failed[i]) {
				surv = i;
				break;
			}
		}
		if (surv < 0) {
			kprintf("hammer2: resilver: no surviving disks\n");
			return ENXIO;
		}
		vol = &hmp->volumes[surv];
		for (i = 0; i < HAMMER2_NUM_VOLHDRS; i++) {
			off_t hdr_off = (off_t)i * HAMMER2_ZONE_BYTES64;
			if ((uint64_t)hdr_off >= hmp->volumes[failed_disk_idx].size)
				break;
			bp = NULL;
			error = bread(vol->dev->devvp, hdr_off,
				      HAMMER2_VOLUME_BYTES, &bp);
			if (error == 0 && bp) {
				wbp = getblk(new_devvp, hdr_off,
					     HAMMER2_VOLUME_BYTES,
					     GETBLK_KVABIO, 0);
				if (wbp) {
					hammer2_volume_data_t *voldata;
					bkvasync(wbp);
					bcopy(bp->b_data, wbp->b_data,
					      HAMMER2_VOLUME_BYTES);
					/* Patch the copy for the target disk */
					voldata = (hammer2_volume_data_t *)
					    wbp->b_data;
					voldata->volu_id = failed_disk_idx;
					voldata->raid_config.disk_state[
					    failed_disk_idx] =
					    HAMMER2_RAID6_DISK_ONLINE;
					voldata->raid_config.flags &=
					    ~HAMMER2_RAID6_FLAG_DEGRADED;
					/* Recompute CRCs: ICRC0 first (covers
					 * volu_id), then ICRCVH (covers all,
					 * including updated ICRC0). */
					voldata->icrc_sects[HAMMER2_VOL_ICRC_SECT0] =
					    hammer2_icrc32(
					        (char *)voldata +
					        HAMMER2_VOLUME_ICRC0_OFF,
					        HAMMER2_VOLUME_ICRC0_SIZE);
					voldata->icrc_volheader =
					    hammer2_icrc32(
					        (char *)voldata +
					        HAMMER2_VOLUME_ICRCVH_OFF,
					        HAMMER2_VOLUME_ICRCVH_SIZE);
					bwrite(wbp);
				}
				brelse(bp);
			} else {
				if (bp)
					brelse(bp);
			}
		}
	}

	/*
	 * Phase 2: Allocate per-column buffers.
	 */
	for (i = 0; i < ndisks; i++)
		col_bufs[i] = kmalloc(stripe_unit, M_HAMMER2, M_WAITOK | M_ZERO);

	/*
	 * Phase 3: Rebuild each stripe.
	 *
	 * Flush all pending writes first so that parity on surviving disks
	 * is up-to-date before we read it.  Concurrent degraded writes that
	 * happen AFTER this sync go to new stripes; we track them in
	 * resilver_dirty_lo/hi for a second pass below.
	 */
	hmp->resilver_dirty_lo = UINT64_MAX;
	hmp->resilver_dirty_hi = 0;
	hammer2_vfs_sync_pmp(pmp, MNT_WAIT);

	for (stripe_num = 0; stripe_num < num_stripes; stripe_num++) {
		phys_off = HAMMER2_ZONE_SEG64 + stripe_num * stripe_unit;

		/* Left-symmetric P and Q disk positions */
		p_disk = (int)(stripe_num % ndisks);
		q_disk = (p_disk + 1) % ndisks;

		/* Build physical disk -> logical column mapping for this stripe */
		di = 0;
		for (phys_disk = 0; phys_disk < ndisks; phys_disk++) {
			if (phys_disk == p_disk)
				phys_to_lcol[phys_disk] = ndata;
			else if (phys_disk == q_disk)
				phys_to_lcol[phys_disk] = ndata + 1;
			else
				phys_to_lcol[phys_disk] = di++;
		}

		failed_col = phys_to_lcol[failed_disk_idx];

		/* Check for a second failed disk (dual failure) */
		other_failed_col = -1;
		for (i = 0; i < ndisks; i++) {
			if (i != failed_disk_idx && hmp->raid_failed[i]) {
				other_failed_col = phys_to_lcol[i];
				break;
			}
		}

		/* Read all surviving columns, zero the failed one */
		for (phys_disk = 0; phys_disk < ndisks; phys_disk++) {
			col = phys_to_lcol[phys_disk];
			bzero(col_bufs[col], stripe_unit);
			ptrs[col] = col_bufs[col];

			if (phys_disk == failed_disk_idx)
				continue; /* will reconstruct */
			if (hmp->raid_failed[phys_disk])
				continue; /* second failed disk, treat as 0 */

			vol = &hmp->volumes[phys_disk];
			bp = NULL;
			error = breadnx(vol->dev->devvp, phys_off,
					stripe_unit, 0, NULL, NULL, 0, &bp);
			if (error == 0 && bp) {
				bkvasync(bp);
				bcopy(bp->b_data, col_bufs[col], stripe_unit);
				brelse(bp);
			} else {
				if (bp)
					brelse(bp);
				/* treat as zeros */
			}
			error = 0;
		}

		/* Reconstruct the failed column */
		if (other_failed_col >= 0) {
			/* Dual failure: use P+Q reconstruction */
			hammer2_raid6_dual_recov(ndisks, stripe_unit,
						 failed_col, other_failed_col,
						 ptrs);
		} else {
			/* Single failure: pass failb = Q (last) column */
			hammer2_raid6_dual_recov(ndisks, stripe_unit,
						 failed_col, ndisks - 1,
						 ptrs);
		}

		/* Write reconstructed column to the new device */
		wbp = getblk(new_devvp, phys_off, stripe_unit,
			     GETBLK_KVABIO, 0);
		if (wbp) {
			bkvasync(wbp);
			bcopy(ptrs[failed_col], wbp->b_data, stripe_unit);
			bwrite(wbp); /* synchronous: correctness > performance */
		}

		/* Update progress and yield every 256 stripes */
		if ((stripe_num & 255) == 255) {
			hmp->resilver_stripes_done = stripe_num + 1;
			lwkt_yield();
		}
	}

	/*
	 * Phase 4: Second pass over stripes written during Phase 3.
	 *
	 * A concurrent degraded write (bwrite P, bwrite Q) may have raced
	 * with Phase 3 reading P/Q: the resilver read old parity (zeros for
	 * new stripes) and wrote zeros to the replacement disk.  After a
	 * full sync the parity is correct; re-resilver the dirty range.
	 */
	hammer2_vfs_sync_pmp(pmp, MNT_WAIT);

	if (hmp->resilver_dirty_lo <= hmp->resilver_dirty_hi) {
		uint64_t lo = hmp->resilver_dirty_lo;
		uint64_t hi = hmp->resilver_dirty_hi;

		kprintf("hammer2: resilver phase 4: re-processing stripes "
			"%ju..%ju\n", (uintmax_t)lo, (uintmax_t)hi);

		for (stripe_num = lo; stripe_num <= hi; stripe_num++) {
			phys_off = HAMMER2_ZONE_SEG64 + stripe_num * stripe_unit;
			p_disk = (int)(stripe_num % ndisks);
			q_disk = (p_disk + 1) % ndisks;

			di = 0;
			for (phys_disk = 0; phys_disk < ndisks; phys_disk++) {
				if (phys_disk == p_disk)
					phys_to_lcol[phys_disk] = ndata;
				else if (phys_disk == q_disk)
					phys_to_lcol[phys_disk] = ndata + 1;
				else
					phys_to_lcol[phys_disk] = di++;
			}

			failed_col = phys_to_lcol[failed_disk_idx];

			other_failed_col = -1;
			for (i = 0; i < ndisks; i++) {
				if (i != failed_disk_idx && hmp->raid_failed[i]) {
					other_failed_col = phys_to_lcol[i];
					break;
				}
			}

			for (phys_disk = 0; phys_disk < ndisks; phys_disk++) {
				col = phys_to_lcol[phys_disk];
				bzero(col_bufs[col], stripe_unit);
				ptrs[col] = col_bufs[col];
				if (phys_disk == failed_disk_idx)
					continue;
				if (hmp->raid_failed[phys_disk])
					continue;
				vol = &hmp->volumes[phys_disk];
				bp = NULL;
				error = breadnx(vol->dev->devvp, phys_off,
					stripe_unit, 0, NULL, NULL, 0, &bp);
				if (error == 0 && bp) {
					bkvasync(bp);
					bcopy(bp->b_data, col_bufs[col],
					      stripe_unit);
					brelse(bp);
				} else {
					if (bp)
						brelse(bp);
				}
				error = 0;
			}

			if (other_failed_col >= 0) {
				hammer2_raid6_dual_recov(ndisks, stripe_unit,
							 failed_col,
							 other_failed_col, ptrs);
			} else {
				hammer2_raid6_dual_recov(ndisks, stripe_unit,
							 failed_col,
							 ndisks - 1, ptrs);
			}

			wbp = getblk(new_devvp, phys_off, stripe_unit,
				     GETBLK_KVABIO, 0);
			if (wbp) {
				bkvasync(wbp);
				bcopy(ptrs[failed_col], wbp->b_data,
				      stripe_unit);
				bwrite(wbp);
			}

			if ((stripe_num & 255) == 255)
				lwkt_yield();
		}
	}

	/* Free column buffers */
	for (i = 0; i < ndisks; i++)
		kfree(col_bufs[i], M_HAMMER2);

	/* Mark resilver complete */
	hmp->resilver_stripes_done = num_stripes;
	hmp->resilver_running = 0;

	return error;
}
