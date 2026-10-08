/*
 * Copyright (c) 2013-2023 The DragonFly Project.  All rights reserved.
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

#include <sys/kthread.h>

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
static void dio_write_stats_update(hammer2_io_t *dio, struct buf *bp);

static hammer2_io_t *hammer2_io_hash_lookup(hammer2_dev_t *hmp,
			hammer2_off_t pbase, uint64_t *refsp);
static hammer2_io_t *hammer2_io_hash_enter(hammer2_dev_t *hmp,
			hammer2_io_t *dio, uint64_t *refsp);
static void hammer2_io_hash_cleanup(hammer2_dev_t *hmp, int dio_limit);

void
hammer2_io_hash_init(hammer2_dev_t *hmp)
{
	hammer2_io_hash_t *hash;
	int i;

	for (i = 0; i < HAMMER2_IOHASH_SIZE; ++i) {
		hash = &hmp->iohash[i];
		hammer2_spin_init(&hash->spin, "h2iohash");
	}
}

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
 * Compute the DIO tree key for a logical I/O target.
 *
 * Two dispatch paths; downstream cache and strategy code consumes
 * (pbase, dbase, devvp, disk_idx) and stays encoding-blind.
 *
 * Path A — v3 RAIDZ2-native DATA/DIRENT:
 *   bref->copyid   = physical disk index (0..ndisks-1)
 *   bref->data_off = per_disk_phys_off | radix     (no disk bits)
 *   pbase = (disk_idx << 56) | per_disk_phys_off   (synthetic cache key)
 *   dbase = disk_idx << 56                          (so dev_pbase = per_disk_phys)
 *
 *   The (disk_idx<<56) bits are NOT stored on disk; they live only in
 *   the in-memory DIO cache key so distinct disks at the same phys_off
 *   don't alias.
 *
 * Path C — JBOD / non-RAID, and v3 metadata:
 *   pbase = data_off & pmask
 *   vol   = hammer2_get_volume(pbase)
 *   dbase = vol->offset                 (so dev_pbase = pbase - vol->offset)
 */
static __inline void
hammer2_dio_key(hammer2_dev_t *hmp, hammer2_key_t data_off,
		const hammer2_blockref_t *bref,
		hammer2_key_t *pbase_out, hammer2_off_t *dbase_out,
		struct vnode **devvp_out, int *disk_idx_out)
{
	hammer2_off_t pmask = ~(hammer2_off_t)(HAMMER2_PBUFSIZE - 1);
	hammer2_off_t lbase = data_off & ~HAMMER2_OFF_MASK_RADIX;
	hammer2_off_t pbase = lbase & pmask;
	hammer2_off_t dbase;
	hammer2_volume_t *vol;
	int disk_idx = -1;

	if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6 &&
	    hmp->voldata.version >= HAMMER2_VOL_VERSION_RAIDZ2 &&
	    bref != NULL &&
	    (bref->type == HAMMER2_BREF_TYPE_DATA ||
	     bref->type == HAMMER2_BREF_TYPE_DIRENT)) {
		/* Path A: v3 RAIDZ2-native DATA/DIRENT. */
		disk_idx = (int)bref->copyid;
		KKASSERT(disk_idx >= 0 && disk_idx < hmp->nvolumes);
		vol = &hmp->volumes[disk_idx];
		dbase = (hammer2_off_t)disk_idx << HAMMER2_RAID6_DISK_SHIFT;
		/* Synthesize the per-disk cache key. */
		pbase = dbase | pbase;
		*devvp_out = vol->dev ? vol->dev->devvp : NULL;
	} else {
		/* Path C: JBOD / non-RAID, or v3 metadata. */
		vol = hammer2_get_volume(hmp, pbase);
		dbase = vol->offset;
		*devvp_out = vol->dev->devvp;
		/*
		 * v3 RAIDZ2-native metadata: identify the primary disk so
		 * putblk can mirror the write to surviving siblings, and
		 * getblk can read from a sibling if this disk is failed.
		 */
		if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6 &&
		    hmp->voldata.version >= HAMMER2_VOL_VERSION_RAIDZ2)
			disk_idx = vol->id;
	}

	*pbase_out = pbase;
	*dbase_out = dbase;
	*disk_idx_out = disk_idx;
}

/*
 * Returns the DIO corresponding to the data|radix, creating it if necessary.
 *
 * If createit is 0, NULL can be returned indicating that the DIO does not
 * exist.  (btype) is ignored when createit is 0.
 */
static __inline
hammer2_io_t *
hammer2_io_alloc(hammer2_dev_t *hmp, hammer2_off_t data_off, uint8_t btype,
		 int createit, int *isgoodp,
		 const hammer2_blockref_t *bref)
{
	hammer2_io_t *dio;
	hammer2_io_t *xio;
	hammer2_off_t lbase;
	hammer2_off_t pbase;
	hammer2_off_t dbase;
	struct vnode *devvp;
	uint64_t refs;
	int lsize;
	int psize;
	int disk_idx;

	psize = HAMMER2_PBUFSIZE;
	if ((int)(data_off & HAMMER2_OFF_MASK_RADIX))
		lsize = 1 << (int)(data_off & HAMMER2_OFF_MASK_RADIX);
	else
		lsize = 0;
	lbase = data_off & ~HAMMER2_OFF_MASK_RADIX;

	{
		hammer2_off_t pmask = ~(hammer2_off_t)(psize - 1);

		pbase = lbase & pmask;
		if (pbase == 0 ||
		    ((lbase + lsize - 1) & pmask) != pbase) {
			kprintf("Illegal: %016jx %016jx+%08x / %016jx\n",
				(uintmax_t)pbase, (uintmax_t)lbase,
				lsize, (uintmax_t)pmask);
		}
		KKASSERT(pbase != 0 &&
			 ((lbase + lsize - 1) & pmask) == pbase);
	}

	hammer2_dio_key(hmp, data_off, bref, &pbase, &dbase, &devvp,
			&disk_idx);
	*isgoodp = 0;

	/*
	 * Access/Allocate the DIO, bump dio->refs to prevent destruction.
	 *
	 * If DIO_GOOD is set the ref should prevent it from being cleared
	 * out from under us, we can set *isgoodp, and the caller can operate
	 * on the buffer without any further interaction.
	 */
	dio = hammer2_io_hash_lookup(hmp, pbase, &refs);
	if (dio) {
		if (refs & HAMMER2_DIO_GOOD)
			*isgoodp = 1;
	} else if (createit) {
		refs = 0;
		dio = kmalloc_obj(sizeof(*dio), hmp->mio, M_INTWAIT | M_ZERO);
		dio->hmp = hmp;
		dio->devvp = devvp;
		dio->dbase = dbase;
		dio->disk_idx = disk_idx;
		/* dbase must be 1GB-aligned for JBOD; RAID6 uses stripe offsets */
		KKASSERT(hmp->raid_type == HAMMER2_RAID_TYPE_RAID6 ||
			 (dio->dbase & HAMMER2_FREEMAP_LEVEL1_MASK) == 0);
		dio->pbase = pbase;
		dio->psize = psize;
		dio->btype = btype;
		dio->refs = refs + 1;
		dio->act = 5;
		xio = hammer2_io_hash_enter(hmp, dio, &refs);
		if (xio == NULL) {
			atomic_add_int(&hammer2_dio_count, 1);
		} else {
			if (refs & HAMMER2_DIO_GOOD)
				*isgoodp = 1;
			kfree_obj(dio, hmp->mio);
			dio = xio;
		}
	} else {
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
		   int lsize, int op, const hammer2_blockref_t *bref
		   HAMMER2_IO_DEBUG_ARGS)
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
		dio = hammer2_io_alloc(hmp, lbase, btype, 0, &isgood, bref);
		if (dio == NULL)
			return NULL;
		op = HAMMER2_DOP_READ;
	} else {
		dio = hammer2_io_alloc(hmp, lbase, btype, 1, &isgood, bref);
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
			if (dio->bp)
				bkvasync(dio->bp);

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
		char *preempt_data;

		if (dio->devvp) {
			dio->bp = getblk(dio->devvp, dev_pbase, dio->psize,
					 GETBLK_KVABIO, 0);
			preempt_data = dio->bp ? dio->bp->b_data : NULL;
		} else {
			/*
			 * Absent disk (no devvp): cannot call getblk()
			 * without a valid vnode.  Allocate a kmalloc buffer
			 * for the reconstruction data; callers access it
			 * via hammer2_io_data() which checks absent_data.
			 */
			dio->absent_data = kmalloc(dio->psize, M_HAMMER2,
						   M_INTWAIT | M_ZERO);
			preempt_data = dio->absent_data;
		}
		if (preempt_data) {
			int is_meta = (dio->btype ==
				    HAMMER2_BREF_TYPE_INODE ||
				   dio->btype ==
				    HAMMER2_BREF_TYPE_INDIRECT ||
				   dio->btype ==
				    HAMMER2_BREF_TYPE_FREEMAP_NODE ||
				   dio->btype ==
				    HAMMER2_BREF_TYPE_FREEMAP_LEAF);
			if (dio->bp)
				bkvasync(dio->bp);
			if (is_meta &&
			    hmp->voldata.version >=
			     HAMMER2_VOL_VERSION_RAIDZ2) {
				/*
				 * I5: metadata is N-way-mirrored, so a
				 * failed primary disk falls back to any
				 * surviving sibling at the same per-disk
				 * byte offset — no parity reconstruction.
				 */
				error = hammer2_io_metadata_mirror_read(hmp,
				    dio->disk_idx, dev_pbase,
				    preempt_data, dio->psize);
			} else {
				error = hammer2_io_raid6_read_degraded(
					hmp, dio->pbase, dio->disk_idx,
					preempt_data, dio->psize,
					(dio->btype ==
					    HAMMER2_BREF_TYPE_DATA ||
					 dio->btype ==
					    HAMMER2_BREF_TYPE_DIRENT) ? 1 : 0);
			}
			switch(op) {
			case HAMMER2_DOP_NEW:
				if (dio->pbase ==
				    (lbase & ~HAMMER2_OFF_MASK_RADIX) &&
				    dio->psize == lsize)
					bzero(preempt_data, dio->psize);
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
			 * COW writes go to fresh stripe slots whose other
			 * data columns are guaranteed zero (newplan.md
			 * §5.4), so no pre-read is needed for any RAID6
			 * write path.  Just getblk() and zero the buffer
			 * for DOP_NEW.
			 *
			 * Failed disk on RAID6: reconstruct from parity
			 * so the dirty buffer reflects the live content
			 * before the caller overwrites it.
			 */
			if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6 &&
			    hmp->raid_nfailed > 0 &&
			    dio->disk_idx >= 0 &&
			    hmp->raid_failed[dio->disk_idx]) {
				dio->bp = getblk(dio->devvp, dev_pbase,
				    dio->psize, GETBLK_KVABIO, 0);
				if (dio->bp) {
					bkvasync(dio->bp);
					error = hammer2_io_raid6_read_degraded(
					    hmp, dio->pbase, dio->disk_idx,
					    dio->bp->b_data, dio->psize,
					    (dio->btype ==
						HAMMER2_BREF_TYPE_DATA ||
					     dio->btype ==
						HAMMER2_BREF_TYPE_DIRENT)
					    ? 1 : 0);
					if (op == HAMMER2_DOP_NEW)
						bzero(dio->bp->b_data,
						    dio->psize);
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

	/*
	 * EIO injection (test sysctl): synthesize the error AFTER the I/O
	 * completes so dio->bp / buf lock state stays consistent.  The
	 * RAID6 reconstruction block below sees error != 0, releases the
	 * read bp via brelse, then getblk()s a fresh one for reconstruction.
	 */
	if (error == 0 && dio->disk_idx >= 0 &&
	    hammer2_inject_eio(dio->disk_idx)) {
		error = EIO;
	}
	dio->error = error;

	/*
	 * RAID6: on read error, mark disk failed and attempt degraded
	 * reconstruction from surviving disks + parity.
	 */
	if (error && hmp->raid_type == HAMMER2_RAID_TYPE_RAID6) {
		int injected = (dio->disk_idx >= 0 &&
				hammer2_inject_eio(dio->disk_idx) != 0);
		/*
		 * Mark this disk as failed (auto-detect from I/O error).
		 * Skip the auto-fail when the EIO came from the test
		 * injection sysctl so the test stays repeatable.
		 */
		if (!injected && dio->disk_idx >= 0 &&
		    !hmp->raid_failed[dio->disk_idx]) {
			hmp->raid_failed[dio->disk_idx] = 1;
			atomic_add_int(&hmp->raid_nfailed, 1);
			kprintf("hammer2: RAID6 disk %d failed (I/O error)\n",
				dio->disk_idx);
		}
		if (hmp->raid_nfailed <= 2) {
			int is_meta = (dio->btype ==
				    HAMMER2_BREF_TYPE_INODE ||
				   dio->btype ==
				    HAMMER2_BREF_TYPE_INDIRECT ||
				   dio->btype ==
				    HAMMER2_BREF_TYPE_FREEMAP_NODE ||
				   dio->btype ==
				    HAMMER2_BREF_TYPE_FREEMAP_LEAF);
			if (dio->bp) {
				brelse(dio->bp);
				dio->bp = NULL;
			}
			dio->bp = getblk(dio->devvp, dev_pbase, dio->psize,
					 GETBLK_KVABIO, 0);
			if (dio->bp) {
				bkvasync(dio->bp);
				if (is_meta &&
				    hmp->voldata.version >=
				     HAMMER2_VOL_VERSION_RAIDZ2) {
					error =
					    hammer2_io_metadata_mirror_read(
					    hmp, dio->disk_idx, dev_pbase,
					    dio->bp->b_data, dio->psize);
				} else {
					error =
					    hammer2_io_raid6_read_degraded(
					    hmp, dio->pbase, dio->disk_idx,
					    dio->bp->b_data, dio->psize,
					    (dio->btype ==
						HAMMER2_BREF_TYPE_DATA ||
					     dio->btype ==
						HAMMER2_BREF_TYPE_DIRENT)
					    ? 1 : 0);
				}
				dio->error = error;
				/*
				 * io_done's BUF_KERNPROC ran on the original
				 * bread bp; the replacement getblk bp must be
				 * detached from this thread or a later putblk
				 * on another thread brelse()s a lock it does
				 * not own (lockmgr panic).
				 */
				BUF_KERNPROC(dio->bp);
				dio->bp->b_flags &= ~B_AGE;
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

	if ((orefs & HAMMER2_DIO_GOOD) && (bp || dio->absent_data)) {
		/*
		 * Non-errored disposal of bp
		 */
		if (orefs & HAMMER2_DIO_DIRTY) {
			char *raid6_data = NULL;
			char *md_mirror_data = NULL;
			int rz_meta = 0;
			int dropped;

			dio_write_stats_update(dio, bp);

			/*
			 * v3 RAIDZ2-native: capture the data column for
			 * parity-protected btypes (DATA/DIRENT) before
			 * releasing bp.  Parity is computed after bp is
			 * released so write_scratch's P/Q writes do not
			 * contend with this bp.
			 *
			 * For metadata btypes (INODE/INDIRECT/FREEMAP_*)
			 * we capture for the N-way mirror write to sibling
			 * disks (I5).
			 */
			if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6 &&
			    hmp->voldata.version >= HAMMER2_VOL_VERSION_RAIDZ2) {
				if (dio->btype == HAMMER2_BREF_TYPE_DATA ||
				    dio->btype == HAMMER2_BREF_TYPE_DIRENT) {
					if (bp) {
						bkvasync(bp);
						raid6_data = kmalloc(psize,
						    M_HAMMER2, M_WAITOK);
						bcopy(bp->b_data, raid6_data,
						    psize);
					} else if (dio->absent_data) {
						raid6_data = dio->absent_data;
						dio->absent_data = NULL;
					}
				} else if (dio->btype ==
					    HAMMER2_BREF_TYPE_INODE ||
					   dio->btype ==
					    HAMMER2_BREF_TYPE_INDIRECT ||
					   dio->btype ==
					    HAMMER2_BREF_TYPE_FREEMAP_NODE ||
					   dio->btype ==
					    HAMMER2_BREF_TYPE_FREEMAP_LEAF) {
					rz_meta = 1;
					if (bp) {
						bkvasync(bp);
						md_mirror_data = kmalloc(psize,
						    M_HAMMER2, M_WAITOK);
						bcopy(bp->b_data,
						    md_mirror_data, psize);
					} else if (dio->absent_data) {
						md_mirror_data =
						    dio->absent_data;
						dio->absent_data = NULL;
					}
				}
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
				if (bp)
					brelse(bp);
				bp = NULL;
			}
			dropped = (bp == NULL);	/* primary not written */

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
					bp->b_flags &= ~B_CLUSTEROK;
					if ((hce = hammer2_cluster_write)
					    != 0) {
						peof = (pbase + HAMMER2_SEGMASK64)
						    & ~HAMMER2_SEGMASK64;
						peof -= dio->dbase;
						bp->b_flags |= B_CLUSTEROK;
						cluster_write(bp, peof,
						    psize, hce);
					} else {
						bawrite(bp);
					}
				} else {
					bp->b_flags &= ~B_CLUSTEROK;
					bdwrite(bp);
				}
			}
			/*
			 * v3 RAIDZ2-native deferred parity (6C packing).
			 *
			 * Hand the col bytes to the open-row tracker.  P/Q
			 * is computed when the row fills (n_alloc==ndata
			 * and all data present) or at TXG flush boundary
			 * via hammer2_raid6_seal_all_open_rows().
			 *
			 * Ownership of raid6_data passes to the tracker;
			 * it kfree's after the row seals.
			 */
			if (raid6_data) {
				hammer2_off_t phys_off =
				    pbase & HAMMER2_RAID6_PHYS_MASK;
				hammer2_raid6_open_row_add_data(hmp,
				    phys_off, dio->disk_idx,
				    raid6_data, psize, dropped);
				raid6_data = NULL;
			}

			/*
			 * v3 RAIDZ2-native metadata mirror (I5).  After the
			 * primary bp has been disposed above (bdwrite /
			 * cluster_write / bawrite to dio->devvp), replicate
			 * the same payload synchronously to every surviving
			 * sibling disk at the same per-disk byte offset.
			 * skip_disk_idx avoids re-writing to the primary,
			 * unless the primary was skipped above (failed, or
			 * being rebuilt and so wanting the copy).
			 */
			if (rz_meta && md_mirror_data) {
				hammer2_off_t per_disk_off = pbase - dio->dbase;
				hammer2_io_metadata_mirror_write(hmp,
				    dropped ? -1 : dio->disk_idx, per_disk_off,
				    md_mirror_data, psize);
				kfree(md_mirror_data, M_HAMMER2);
				md_mirror_data = NULL;
			}
		} else if (bp) {
			/* Non-dirty, non-RAID6-absent disposal of bp */
			if (bp->b_flags & (B_ERROR | B_INVAL | B_RELBUF))
				brelse(bp);
			else
				bqrelse(bp);
		}
	} else if (bp) {
		/*
		 * Errored disposal of bp
		 */
		brelse(bp);
	}

	if (dio->absent_data) {
		kfree(dio->absent_data, M_HAMMER2);
		dio->absent_data = NULL;
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
	if (hmp->iofree_count > dio_limit)
		hammer2_io_hash_cleanup(hmp, dio_limit);
}

/*
 * Returns a pointer to the requested data.
 */
char *
hammer2_io_data(hammer2_io_t *dio, off_t lbase)
{
	struct buf *bp;
	int off;

	lbase -= dio->dbase;
	off = (int)((lbase & ~HAMMER2_OFF_MASK_RADIX) -
		    (off_t)(dio->pbase - dio->dbase));

	/*
	 * Absent disk (no devvp): data is in a kmalloc buffer.
	 */
	if (dio->absent_data) {
		KKASSERT(off >= 0 && off < dio->psize);
		return (dio->absent_data + off);
	}

	bp = dio->bp;
	KKASSERT(bp != NULL);
	bkvasync(bp);
	KKASSERT(off >= 0 && off < bp->b_bufsize);
	return(bp->b_data + off);
}

int
hammer2_io_new(hammer2_dev_t *hmp, int btype, off_t lbase, int lsize,
	       hammer2_io_t **diop, const hammer2_blockref_t *bref)
{
	*diop = hammer2_io_getblk(hmp, btype, lbase, lsize, HAMMER2_DOP_NEW,
				  bref);
	return ((*diop)->error);
}

int
hammer2_io_newnz(hammer2_dev_t *hmp, int btype, off_t lbase, int lsize,
		 hammer2_io_t **diop, const hammer2_blockref_t *bref)
{
	*diop = hammer2_io_getblk(hmp, btype, lbase, lsize, HAMMER2_DOP_NEWNZ,
				  bref);
	return ((*diop)->error);
}

int
_hammer2_io_bread(hammer2_dev_t *hmp, int btype, off_t lbase, int lsize,
		hammer2_io_t **diop, const hammer2_blockref_t *bref
		HAMMER2_IO_DEBUG_ARGS)
{
#ifdef HAMMER2_IO_DEBUG
	hammer2_io_t *dio;
#endif

	*diop = _hammer2_io_getblk(hmp, btype, lbase, lsize,
				   HAMMER2_DOP_READ, bref
				   HAMMER2_IO_DEBUG_CALL);
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
				 HAMMER2_DOP_READQ, NULL
				 HAMMER2_IO_DEBUG_CALL);
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

	dio = hammer2_io_alloc(hmp, bref->data_off, bref->type, 1, &isgood, NULL);
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
 *
 * Signature kept stable so the upstream hammer2_bulkfree.c (not in our
 * local_*.c set) keeps linking.  v3 RAIDZ2-native DATA never reaches
 * the freemap that bulkfree walks (DATA dispatches to stripe_alloc),
 * so the bref-NULL Path C fallback in dio_alloc covers the only
 * scenarios this is actually called from.
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
	dio = hammer2_io_alloc(hmp, data_off, btype, 0, &isgood, NULL);
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
			       0, &isgood, NULL);
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
	if (bp && (bp->b_flags & B_DELWRI))
		return;
	hammer2_adjwritecounter(dio->btype, dio->psize);
}

void
hammer2_io_bkvasync(hammer2_io_t *dio)
{
	if (dio->absent_data)
		return;	/* kmalloc buffer is always CPU-accessible, no KVABIO needed */
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
 * Auto-fail a disk that returned EIO during a RAID6 I/O operation.
 *
 * Updates both the runtime hmp->raid_failed[] state and the on-disk
 * voldata.raid_config, so the failed state survives unmount/remount.
 *
 * If raid_nfailed would reach 3 (triple failure), returns ENXIO to
 * signal unrecoverable array state; otherwise marks the disk failed
 * and returns 0.
 *
 * Called from I/O context — does NOT close the vnode.  The disk is
 * excluded from future I/Os via raid_failed[]; the vnode is released
 * at unmount.
 */
int
hammer2_raid6_auto_fail_disk(hammer2_dev_t *hmp, int disk_idx)
{
	hammer2_voldata_lock(hmp);
	if (hmp->raid_failed[disk_idx]) {
		hammer2_voldata_unlock(hmp);
		return 0;
	}
	if (hmp->raid_nfailed >= 2) {
		hammer2_voldata_unlock(hmp);
		kprintf("hammer2: RAID6 unrecoverable: I/O error on disk %d "
			"but already %d disk(s) failed\n",
			disk_idx, hmp->raid_nfailed);
		return ENXIO;
	}
	kprintf("hammer2: RAID6 disk %d auto-failed due to I/O error\n",
		disk_idx);
	hmp->raid_failed[disk_idx] = 1;
	atomic_add_int(&hmp->raid_nfailed, 1);
	hmp->raid_config.disk_state[disk_idx] = HAMMER2_RAID6_DISK_FAILED;
	hmp->raid_config.flags |= HAMMER2_RAID6_FLAG_DEGRADED;
	hmp->voldata.raid_config.disk_state[disk_idx] = HAMMER2_RAID6_DISK_FAILED;
	hmp->voldata.raid_config.flags |= HAMMER2_RAID6_FLAG_DEGRADED;
	hammer2_voldata_modify(hmp);
	hammer2_voldata_unlock(hmp);
	return 0;
}

/*
 * A RAID6 write to disk_idx returned error.  A healthy member is
 * auto-failed, so its stale column stops counting as redundancy.  The
 * disk being resilvered is already failed; the failure is recorded in
 * rebuild_werror instead and fails the resilver, which would otherwise
 * bring the disk online with a hole.
 *
 * Returns hammer2_raid6_auto_fail_disk()'s result (ENXIO past two
 * failed disks), else 0.
 */
int
hammer2_raid6_write_failed(hammer2_dev_t *hmp, int disk_idx, int error)
{
	if (hmp->raid_failed[disk_idx]) {
		if (hmp->rebuild_active && hmp->rebuild_disk == disk_idx &&
		    hmp->rebuild_werror == 0) {
			kprintf("hammer2: RAID6 write error %d on resilvering "
				"disk %d\n", error, disk_idx);
			hmp->rebuild_werror = error;
		}
		return 0;
	}
	return hammer2_raid6_auto_fail_disk(hmp, disk_idx);
}

/*
 * Read-path self-heal repair queue (bitrot.md §7.4).
 *
 * The reader that detects a CHECK failure still holds the DIO buf
 * covering the corrupt block, so it cannot write the repair itself
 * (getblk on the same (devvp, offset) would deadlock on the busy buf —
 * the same constraint that makes the scrub release its read dio before
 * repairing).  Instead the verified-good bytes are queued here and a
 * per-device kthread performs the write once the reader's buf ref
 * drains.
 *
 * COW note: repair targets are always live (allocated) slots that held
 * the corrupt data before the heal, and hammer2 never rewrites a live
 * slot in place, so the deferred write cannot collide with new row
 * writes.  The residual freed-and-reallocated window is shared with the
 * scrub repair design and is bounded by the queue latency (typically
 * milliseconds).
 */
#define HAMMER2_REPAIR_QUEUE_MAX	256

static void
hammer2_io_repair_execute(hammer2_dev_t *hmp, hammer2_repair_req_t *req)
{
	hammer2_volume_t *vol;
	struct vnode *devvp;
	struct buf *bp;
	int error;

	if (req->disk_idx < 0 || req->disk_idx >= hmp->raid_config.ndisks)
		return;
	if (hmp->raid_failed[req->disk_idx])
		return;
	vol = &hmp->volumes[req->disk_idx];
	if (vol->dev == NULL || vol->dev->devvp == NULL || !vol->dev->open)
		return;
	devvp = vol->dev->devvp;

	if (req->patch_off == 0 && req->patch_len == req->bufsize) {
		/*
		 * Full-buffer overwrite (DATA/DIRENT stripe column).
		 */
		bp = getblk(devvp, req->base, req->bufsize, GETBLK_KVABIO, 0);
		if (bp == NULL)
			return;
		bkvasync(bp);
		bcopy(req->data, bp->b_data, req->bufsize);
	} else {
		/*
		 * Sub-buffer patch (metadata block inside a shared device
		 * buffer): read-modify-write so sibling blocks in the same
		 * buffer are preserved.
		 */
		bp = NULL;
		error = bread(devvp, req->base, req->bufsize, &bp);
		if (error || bp == NULL) {
			if (bp)
				brelse(bp);
			return;
		}
		bkvasync(bp);
		bcopy(req->data, (char *)bp->b_data + req->patch_off,
		      req->patch_len);
	}
	error = bwrite(bp);
	if (error == 0) {
		atomic_add_64(&hmp->repair_writes_done, 1);
		kprintf("hammer2: selfheal: repair write disk %d base %016jx "
		    "off %d len %d\n",
		    req->disk_idx, (uintmax_t)req->base,
		    req->patch_off, req->patch_len);
	} else {
		kprintf("hammer2: selfheal: repair write FAILED disk %d "
		    "base %016jx err %d\n",
		    req->disk_idx, (uintmax_t)req->base, error);
	}
}

static void
hammer2_io_repair_thread(void *arg)
{
	hammer2_dev_t *hmp = arg;
	hammer2_repair_req_t *req;

	for (;;) {
		hammer2_spin_ex(&hmp->repair_spin);
		req = TAILQ_FIRST(&hmp->repair_queue);
		if (req) {
			TAILQ_REMOVE(&hmp->repair_queue, req, entry);
			hmp->repair_queue_count--;
		}
		hammer2_spin_unex(&hmp->repair_spin);

		if (req) {
			hammer2_io_repair_execute(hmp, req);
			kfree(req->data, M_HAMMER2);
			kfree(req, M_HAMMER2);
			continue;
		}
		if (hmp->repair_thread_exit)
			break;
		tsleep(&hmp->repair_queue, 0, "h2repi", hz);
	}
	hmp->repair_thread_running = 0;
	wakeup(__DEVOLATILE(void *, &hmp->repair_thread_running));
	kthread_exit();
}

void
hammer2_io_repair_start(hammer2_dev_t *hmp)
{
	int error;

	if (hmp->repair_thread_running)
		return;
	hammer2_spin_init(&hmp->repair_spin, "h2repair");
	TAILQ_INIT(&hmp->repair_queue);
	hmp->repair_queue_count = 0;
	hmp->repair_thread_exit = 0;
	hmp->repair_thread_running = 1;
	error = kthread_create(hammer2_io_repair_thread, hmp,
			       &hmp->repair_td, "h2repair");
	if (error) {
		hmp->repair_thread_running = 0;
		kprintf("hammer2: selfheal: repair kthread create failed "
		    "(%d) — read-path heals will be memory-only\n", error);
	}
}

void
hammer2_io_repair_stop(hammer2_dev_t *hmp)
{
	hammer2_repair_req_t *req;

	if (hmp->repair_thread_running) {
		/*
		 * Ask the thread to exit; it drains the queue first (all
		 * devvps are still open at this point in unmount).
		 */
		hmp->repair_thread_exit = 1;
		wakeup(&hmp->repair_queue);
		while (hmp->repair_thread_running)
			tsleep(__DEVOLATILE(void *, &hmp->repair_thread_running),
			       0, "h2reps", hz / 10);
	}
	/* Safety net: free anything left if the thread never started. */
	while ((req = TAILQ_FIRST(&hmp->repair_queue)) != NULL) {
		TAILQ_REMOVE(&hmp->repair_queue, req, entry);
		kfree(req->data, M_HAMMER2);
		kfree(req, M_HAMMER2);
	}
	hmp->repair_queue_count = 0;
}

/*
 * Queue a repair write.  Takes ownership of `data` (kmalloc'd); frees
 * it if the request is dropped (thread not running, queue full, or a
 * repair for the same buffer already queued).
 */
void
hammer2_io_repair_enqueue(hammer2_dev_t *hmp, int disk_idx,
			  hammer2_off_t base, int bufsize, int patch_off,
			  int patch_len, char *data)
{
	hammer2_repair_req_t *req;
	hammer2_repair_req_t *scan;

	if (!hmp->repair_thread_running) {
		kfree(data, M_HAMMER2);
		return;
	}
	req = kmalloc(sizeof(*req), M_HAMMER2, M_WAITOK | M_ZERO);
	req->disk_idx = disk_idx;
	req->base = base;
	req->bufsize = bufsize;
	req->patch_off = patch_off;
	req->patch_len = patch_len;
	req->data = data;

	hammer2_spin_ex(&hmp->repair_spin);
	if (hmp->repair_queue_count >= HAMMER2_REPAIR_QUEUE_MAX) {
		hammer2_spin_unex(&hmp->repair_spin);
		atomic_add_64(&hmp->repair_writes_dropped, 1);
		kfree(req->data, M_HAMMER2);
		kfree(req, M_HAMMER2);
		kprintf("hammer2: selfheal: repair queue full, dropped "
		    "disk %d base %016jx (scrub will re-detect)\n",
		    disk_idx, (uintmax_t)base);
		return;
	}
	TAILQ_FOREACH(scan, &hmp->repair_queue, entry) {
		if (scan->disk_idx == disk_idx && scan->base == base &&
		    scan->patch_off == patch_off) {
			/* already queued; keep the earlier payload */
			hammer2_spin_unex(&hmp->repair_spin);
			kfree(req->data, M_HAMMER2);
			kfree(req, M_HAMMER2);
			return;
		}
	}
	TAILQ_INSERT_TAIL(&hmp->repair_queue, req, entry);
	hmp->repair_queue_count++;
	hammer2_spin_unex(&hmp->repair_spin);
	wakeup(&hmp->repair_queue);
}

/*
 * Read a sibling column or mirror copy into dst for reconstruction.
 *
 * The caller may hold the buffer for its own column locked (self-heal
 * runs under the DIO buf), and another thread healing a block in the
 * same row holds that sibling's buffer while it waits for ours.  A
 * blocking getblk here deadlocks the two (ABBA).  Take the cached
 * buffer only if it is free; when another thread holds it, read the
 * media directly through a pbuf instead.  The rows read here are
 * committed, so the media copy is current.
 *
 * nocache is for streaming passes (resilver) that touch each block
 * once: a block that was not already cached is dropped after the read,
 * its pages freed rather than left on the inactive queue.  A block
 * that was cached stays cached.
 */
static int
hammer2_io_raid6_read_bufcache(struct vnode *devvp, off_t off,
			       void *dst, int bytes, int nocache)
{
	struct buf *bp;
	int error;

	bp = getblk(devvp, off, bytes, GETBLK_NOWAIT, 0);
	if (bp == NULL)
		return EWOULDBLOCK;
	if (nocache && (bp->b_flags & (B_CACHE | B_DELWRI)) == 0)
		bp->b_flags |= B_RELBUF | B_DIRECT;
	error = breadnx(devvp, off, bytes, 0, NULL, NULL, 0, &bp);
	if (error == 0) {
		bkvasync(bp);
		bcopy(bp->b_data, dst, bytes);
	}
	brelse(bp);
	return error;
}

static int
hammer2_io_raid6_read_sibling(struct vnode *devvp, off_t off,
			      void *dst, int bytes, int nocache)
{
	struct buf *bp;
	int error;

	error = hammer2_io_raid6_read_bufcache(devvp, off, dst, bytes,
					       nocache);
	if (error != EWOULDBLOCK)
		return error;

	bp = getpbuf_mem(NULL);
	KKASSERT(bytes <= bp->b_bufsize);
	bp->b_cmd = BUF_CMD_READ;
	bp->b_bcount = bytes;
	bp->b_resid = bytes;
	bp->b_bio1.bio_offset = off;
	bp->b_bio1.bio_done = biodone_sync;
	bp->b_bio1.bio_flags |= BIO_SYNC;
	vn_strategy(devvp, &bp->b_bio1);
	error = biowait(&bp->b_bio1, "h2sib");
	if (error == 0)
		bcopy(bp->b_data, dst, bytes);
	relpbuf(bp, NULL);
	return error;
}

/*
 * Write a column to a disk being rebuilt.  Callers hold rebuild_lk,
 * which the resilver takes exclusive, so this must never wait on a
 * buffer: a thread holding that buffer through a DIO can itself be
 * queued for rebuild_lk.  Take the cached buffer only if it is free,
 * otherwise write the media directly through a pbuf.  The holder's
 * copy is of the same block, so the cache does not go stale.
 */
static int
hammer2_io_raid6_write_flags(struct vnode *devvp, off_t off,
			     const void *data, int bytes, int nocache)
{
	struct buf *bp;
	int error;

	bp = getblk(devvp, off, bytes, GETBLK_NOWAIT | GETBLK_KVABIO, 0);
	if (bp) {
		if (nocache && (bp->b_flags & (B_CACHE | B_DELWRI)) == 0)
			bp->b_flags |= B_RELBUF | B_DIRECT;
		bkvasync(bp);
		bcopy(data, bp->b_data, bytes);
		return bwrite(bp);
	}

	bp = getpbuf_mem(NULL);
	KKASSERT(bytes <= bp->b_bufsize);
	bcopy(data, bp->b_data, bytes);
	bp->b_cmd = BUF_CMD_WRITE;
	bp->b_bcount = bytes;
	bp->b_resid = bytes;
	bp->b_bio1.bio_offset = off;
	bp->b_bio1.bio_done = biodone_sync;
	bp->b_bio1.bio_flags |= BIO_SYNC;
	vn_strategy(devvp, &bp->b_bio1);
	error = biowait(&bp->b_bio1, "h2rbw");
	relpbuf(bp, NULL);
	return error;
}

int
hammer2_io_raid6_write_nowait(struct vnode *devvp, off_t off,
			      const void *data, int bytes)
{
	return hammer2_io_raid6_write_flags(devvp, off, data, bytes, 0);
}

/*
 * write_nowait for the resilver, which writes each block of the new
 * disk once: the written block is not kept cached unless it already
 * was.  See read_sibling.
 */
static int
hammer2_io_raid6_write_stream(struct vnode *devvp, off_t off,
			      const void *data, int bytes)
{
	return hammer2_io_raid6_write_flags(devvp, off, data, bytes, 1);
}

/*
 * Point the cached DIOs of one disk at a new device vnode (disk
 * replacement).  A DIO keeps the devvp it was created with.
 */
void
hammer2_io_retarget_disk(hammer2_dev_t *hmp, int disk_idx,
			 struct vnode *devvp)
{
	hammer2_io_hash_t *hash;
	hammer2_io_t *dio;
	int i;

	for (i = 0; i < HAMMER2_IOHASH_SIZE; ++i) {
		hash = &hmp->iohash[i];
		hammer2_spin_ex(&hash->spin);
		for (dio = hash->base; dio; dio = dio->next) {
			if (dio->disk_idx == disk_idx)
				dio->devvp = devvp;
		}
		hammer2_spin_unex(&hash->spin);
	}
}

/*
 * Metadata mirror check-aware failover (bitrot.md §7.4 bullet 2, and
 * metadata_zone.md "Read path": on CHECK FAIL, try disk 1, disk 2, ...
 * until a copy verifies).
 *
 * Reads each surviving sibling's copy of the device buffer
 * (dev_pbase, psize), verifies the chain's range [off, off+bytes)
 * against the bref CHECK code, and on the first verifying copy fixes
 * *bdata in place and queues a patch-repair of the primary disk.
 *
 * Returns 0 on success, EIO if no sibling has a verifying copy.
 */
int
hammer2_io_metadata_mirror_heal(hammer2_dev_t *hmp, int bad_disk_idx,
				hammer2_off_t dev_pbase, int psize, int off,
				const hammer2_blockref_t *bref,
				void *bdata, size_t bytes)
{
	hammer2_volume_t *vol;
	char *copy;
	char *patch;
	int i;
	int error;

	KKASSERT(hmp->raid_type == HAMMER2_RAID_TYPE_RAID6);
	KKASSERT(hmp->voldata.version >= HAMMER2_VOL_VERSION_RAIDZ2);

	copy = kmalloc(psize, M_HAMMER2, M_WAITOK);
	for (i = 0; i < hmp->raid_config.ndisks; i++) {
		if (i == bad_disk_idx)
			continue;
		if (hmp->raid_failed[i])
			continue;
		vol = &hmp->volumes[i];
		if (vol->dev == NULL || vol->dev->devvp == NULL ||
		    !vol->dev->open)
			continue;

		error = hammer2_inject_eio(i);
		if (error == 0)
			error = hammer2_io_raid6_read_sibling(vol->dev->devvp,
			    dev_pbase, copy, psize, 0);
		if (error)
			continue;
		if (hammer2_bref_check_match(bref, copy + off, bytes)) {
			bcopy(copy + off, bdata, bytes);
			kfree(copy, M_HAMMER2);
			patch = kmalloc(bytes, M_HAMMER2, M_WAITOK);
			bcopy(bdata, patch, bytes);
			hammer2_io_repair_enqueue(hmp, bad_disk_idx,
			    dev_pbase, psize, off, (int)bytes, patch);
			return 0;
		}
	}
	kfree(copy, M_HAMMER2);
	return EIO;
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
			       int data_disk_idx, void *buf, size_t bytes,
			       int is_physical)
{
	return hammer2_io_raid6_read_degraded_x(hmp, logical_off,
	    data_disk_idx, buf, bytes, is_physical, -1, NULL);
}

/*
 * As hammer2_io_raid6_read_degraded, also treating extra_disk's column
 * as an erasure (-1 for none).  A column that reads without error can
 * still be silently corrupt; when a reconstruction fails its CHECK,
 * the caller retries with each other column erased in turn.  With
 * extra_buf non-NULL, the reconstructed extra column is copied there
 * so the caller can repair it.  Fails with EIO when the erasures
 * exceed what P and Q can recover.
 */
int
hammer2_io_raid6_read_degraded_x(hammer2_dev_t *hmp,
				 hammer2_off_t logical_off,
				 int data_disk_idx, void *buf, size_t bytes,
				 int is_physical, int extra_disk,
				 void *extra_buf)
{
	hammer2_raid_config_t *rc = &hmp->raid_config;
	int ndisks = rc->ndisks;
	int ndata = rc->ndata;
	uint64_t stripe_unit = rc->stripe_unit;
	int p_disk, q_disk;
	int target_col;
	int phys_disk, di, col;
	int extra_col = -1;
	int nerased = 0;
	hammer2_off_t phys_off;
	void *ptrs[HAMMER2_MAX_VOLUMES];
	void *col_bufs[HAMMER2_MAX_VOLUMES];
	hammer2_volume_t *vol;
	int error = 0;
	int fail_data_a = -1, fail_data_b = -1;
	int fail_p = 0, fail_q = 0;

	bzero(ptrs, sizeof(ptrs));
	bzero(col_bufs, sizeof(col_bufs));

	KKASSERT(bytes == stripe_unit);
	KKASSERT(hmp->raid_type == HAMMER2_RAID_TYPE_RAID6);
	KKASSERT(hmp->voldata.version >= HAMMER2_VOL_VERSION_RAIDZ2);
	if (is_physical) {
		/*
		 * v3 RAIDZ2-native physical addressing for DATA/DIRENT.
		 *
		 * logical_off is the encoded key (top byte = disk_idx,
		 * middle bits = per-disk physical offset; see
		 * HAMMER2_RAID6_DISK_SHIFT in hammer2_disk.h).  All
		 * columns in the same stripe slot share the same
		 * per-disk physical offset.
		 *
		 * P disk = stripe_slot % ndisks
		 * Q disk = (P disk + 1) % ndisks
		 * Data columns: all other disks in disk-index order.
		 *
		 * target_col: the logical column index for data_disk_idx.
		 *   data_disk_idx == p_disk -> target_col = ndata (P)
		 *   data_disk_idx == q_disk -> target_col = ndata+1 (Q)
		 *   otherwise count data disks below data_disk_idx.
		 */
		uint64_t stripe_slot;
		int d_col;

		phys_off    = logical_off & HAMMER2_RAID6_PHYS_MASK;
		stripe_slot = (phys_off - HAMMER2_ZONE_SEG64) / stripe_unit;
		p_disk      = (int)(stripe_slot % ndisks);
		q_disk      = (p_disk + 1) % ndisks;

		if (data_disk_idx == p_disk) {
			target_col = ndata;
		} else if (data_disk_idx == q_disk) {
			target_col = ndata + 1;
		} else {
			/* Count data disks with index < data_disk_idx */
			d_col = 0;
			for (di = 0; di < data_disk_idx; di++) {
				if (di != p_disk && di != q_disk)
					d_col++;
			}
			target_col = d_col;
		}

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

			if (phys_disk == extra_disk)
				extra_col = col;
			if (phys_disk == data_disk_idx ||
			    phys_disk == extra_disk) {
				/*
				 * A column being reconstructed.
				 * Leave the buffer zeroed and mark failed.
				 */
				++nerased;
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

			if (hmp->raid_failed[phys_disk]) {
				++nerased;
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
			lerror = hammer2_inject_eio(phys_disk);
			if (lerror == 0)
				lerror = hammer2_io_raid6_read_sibling(
				    vol->dev->devvp, phys_off,
				    col_bufs[col], (int)stripe_unit, 0);
			if (lerror) {
				int injected = hammer2_inject_eio(phys_disk);
				if (!injected) {
					int aerr =
					    hammer2_raid6_auto_fail_disk(
						hmp, phys_disk);
					if (aerr) {
						error = EIO;
						break;
					}
				}
				++nerased;
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
		goto do_recovery;
	}

	/*
	 * Non-physical call: INODE/INDIRECT/FREEMAP on a failed disk.
	 * Pre-Group I there is no metadata mirror to read from, and v3
	 * logical reconstruction is gone. Return EIO; Group I lands the
	 * metadata-zone mirror.
	 */
	error = EIO;
	goto read_degraded_done;

do_recovery:
	/* Perform recovery */
	if (error)
		goto read_degraded_done;
	if (nerased > 2) {
		error = EIO;
		goto read_degraded_done;
	}
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
	/*
	 * Every data column is now valid.  An erased P or Q column is
	 * still zero (the recovery above only rebuilds data), so
	 * regenerate both before handing out a parity column.
	 */
	if (error == 0 && (fail_p || fail_q))
		hammer2_raid6_gen_syndrome(ndisks, stripe_unit, ptrs);

	if (error == 0) {
		bcopy(ptrs[target_col], buf, stripe_unit);
		if (extra_buf && extra_col >= 0)
			bcopy(ptrs[extra_col], extra_buf, stripe_unit);
	}
read_degraded_done:
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
	int lerror;
	int i;

	KKASSERT(hmp->raid_type == HAMMER2_RAID_TYPE_RAID6);
	KKASSERT(stripe_unit > 0);
	KKASSERT(ndata > 0);
	KKASSERT(failed_disk_idx >= 0 && failed_disk_idx < ndisks);

	if (hmp->voldata.version >= HAMMER2_VOL_VERSION_RAIDZ2) {
		/*
		 * v3 (RAIDZ2-native): iterate over all possible stripe slots
		 * in the bitmap.  Only process allocated (bit-set) slots.
		 */
		num_stripes = hmp->stripe_num_slots;
	} else {
		num_stripes = rc->array_size / ((uint64_t)ndata * stripe_unit);
	}

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
			hmp->resilver_running = 0;
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
					/*
					 * Patch the copy for the target disk.
					 * It stays FAILED until the resilver
					 * completes and the replace ioctl
					 * syncs ONLINE headers; a crash in
					 * between leaves it failed.
					 */
					voldata = (hammer2_volume_data_t *)
					    wbp->b_data;
					voldata->volu_id = failed_disk_idx;
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
	 * Phase A: Metadata-zone sequential copy (metadata_zone.md §Resilver).
	 *
	 * Metadata blocks (INODE/INDIRECT/FREEMAP_*) are an N-way mirror at
	 * the same per-disk byte offset on every disk; reconstructing the
	 * failed disk's copy is a straight sequential bulk read from any
	 * surviving disk followed by a bulk write to new_devvp at the same
	 * offset.  No parity, no GF math.  Order of magnitude minutes vs the
	 * hours a blockref walk of metadata used to take.
	 *
	 * Done before Phase 3 so the stripe loop has up-to-date freemap and
	 * inode topology to read against.
	 */
	if (hmp->voldata.version >= HAMMER2_VOL_VERSION_RAIDZ2 &&
	    hmp->md_nextents > 0) {
		/*
		 * bread/getblk panic if size > MAXBSIZE (64 KB on DragonFly).
		 * Stay at HAMMER2_PBUFSIZE so each chunk maps to one buf-cache
		 * block.  Extent walks issue more loop iterations but stay
		 * within buffer-cache limits.
		 */
		const size_t MD_CHUNK = HAMMER2_PBUFSIZE;
		void *md_buf = kmalloc(MD_CHUNK, M_HAMMER2, M_WAITOK);
		hammer2_off_t md_end;
		uint32_t nzones;
		uint32_t ex;
		int surv = -1;
		int pass;
		int prim;

		for (i = 0; i < ndisks; i++) {
			if (i != failed_disk_idx && !hmp->raid_failed[i] &&
			    hmp->volumes[i].dev != NULL &&
			    hmp->volumes[i].dev->devvp != NULL &&
			    hmp->volumes[i].dev->open) {
				surv = i;
				break;
			}
		}
		if (surv < 0) {
			kprintf("hammer2: resilver Phase A: no surviving "
				"disk to read metadata from\n");
			kfree(md_buf, M_HAMMER2);
			hmp->resilver_running = 0;
			return ENXIO;
		}

		/*
		 * Mirrored ranges: the metadata extents, then the reserved
		 * segment of every 2 GB zone the extents reach, which holds
		 * the freemap blocks for that zone.  The volume header at
		 * the start of each segment was written by Phase 1.
		 */
		md_end = 0;
		for (ex = 0; ex < hmp->md_nextents; ex++) {
			if (md_end < hmp->md_extents[ex].md_off +
				     hmp->md_extents[ex].md_size)
				md_end = hmp->md_extents[ex].md_off +
					 hmp->md_extents[ex].md_size;
		}
		nzones = (md_end + HAMMER2_ZONE_MASK64) / HAMMER2_ZONE_BYTES64;

		for (ex = 0; ex < hmp->md_nextents + nzones; ex++) {
			hammer2_off_t mo;
			hammer2_off_t ms;
			hammer2_off_t off;

			if (ex < hmp->md_nextents) {
				mo = hmp->md_extents[ex].md_off;
				ms = hmp->md_extents[ex].md_size;
			} else {
				mo = (hammer2_off_t)(ex - hmp->md_nextents) *
				     HAMMER2_ZONE_BYTES64 + HAMMER2_VOLUME_BYTES;
				ms = HAMMER2_ZONE_SEG64 - HAMMER2_VOLUME_BYTES;
			}

			for (off = 0; off < ms; off += MD_CHUNK) {
				size_t this_chunk =
				    (size_t)((ms - off > MD_CHUNK) ?
					     MD_CHUNK : (ms - off));
				/*
				 * dscheck rejects non-sector-aligned bcount.
				 * The trailing partial chunk of a non-aligned
				 * extent must round up; the few extra bytes
				 * land in the kmalloc()'d md_buf safely.
				 */
				this_chunk = (this_chunk + DEV_BSIZE - 1) &
				    ~((size_t)DEV_BSIZE - 1);
				if (this_chunk > MD_CHUNK)
					this_chunk = MD_CHUNK;

				/*
				 * Every survivor holds a mirror copy; on a
				 * read error try the next one.  Mirror copies
				 * are written synchronously, so read those
				 * first (pass 0): the primary's newest copy
				 * may still be a delayed write held by a DIO,
				 * and read_sibling then reads the media.
				 *
				 * Exclusive rebuild_lk keeps a concurrent
				 * mirror write from landing between our read
				 * and our write to the new disk.
				 */
				prim = -1;	/* as hammer2_get_volume() */
				for (i = 0; i < hmp->nvolumes; i++) {
					vol = &hmp->volumes[i];
					if (mo + off >= vol->offset &&
					    mo + off < vol->offset + vol->size) {
						prim = i;
						break;
					}
				}
				lockmgr(&hmp->rebuild_lk, LK_EXCLUSIVE);
				lerror = EIO;
				for (pass = 0; pass < 2 && lerror; pass++) {
				    for (i = surv; i < ndisks; i++) {
					if (i == failed_disk_idx ||
					    hmp->raid_failed[i] ||
					    hmp->volumes[i].dev == NULL ||
					    hmp->volumes[i].dev->devvp == NULL ||
					    !hmp->volumes[i].dev->open)
						continue;
					if ((pass == 0) == (i == prim))
						continue;
					vol = &hmp->volumes[i];
					lerror = hammer2_inject_eio(i);
					if (lerror == 0)
						lerror =
						 hammer2_io_raid6_read_sibling(
						    vol->dev->devvp,
						    (off_t)(mo + off), md_buf,
						    (int)this_chunk, 1);
					if (lerror == 0)
						break;
				    }
				}
				if (lerror) {
					lockmgr(&hmp->rebuild_lk, LK_RELEASE);
					kprintf("hammer2: resilver Phase A: "
						"read err %d at off %jx "
						"on every survivor\n",
						lerror, (intmax_t)(mo + off));
					kfree(md_buf, M_HAMMER2);
					hmp->resilver_running = 0;
					return EIO;
				}

				lerror = hammer2_io_raid6_write_stream(
				    new_devvp, (off_t)(mo + off), md_buf,
				    (int)this_chunk);
				lockmgr(&hmp->rebuild_lk, LK_RELEASE);
				if (lerror) {
					kfree(md_buf, M_HAMMER2);
					hmp->resilver_running = 0;
					return lerror;
				}
				if ((off >> 22) % 16 == 0)
					lwkt_yield();
			}
		}
		kfree(md_buf, M_HAMMER2);
	}

	/*
	 * Phase 2: Allocate per-column buffers.
	 */
	for (i = 0; i < ndisks; i++)
		col_bufs[i] = kmalloc(stripe_unit, M_HAMMER2, M_WAITOK | M_ZERO);

	/*
	 * Phase 3: Rebuild each stripe.
	 *
	 * Flush all pending writes first so that the rows sealed before
	 * the rebuild started are on media before we read them.  Rows
	 * sealed since then wrote the new disk's column themselves (the
	 * replace ioctl set rebuild_active before calling us) and are
	 * marked in rebuild_done.
	 */
	hammer2_vfs_sync_pmp(pmp, MNT_WAIT);

	for (stripe_num = 0; stripe_num < num_stripes; stripe_num++) {
		/*
		 * v3 (RAIDZ2-native): skip unallocated stripe slots.
		 * Bitmap lives in hmp->stripe_bitmap.  Gated by sysctl
		 * vfs.hammer2.resilver_skip_unalloc (default 1) — set 0 for
		 * full-iteration regression baseline.
		 */
		if (hmp->voldata.version >= HAMMER2_VOL_VERSION_RAIDZ2 &&
		    hammer2_raid6_resilver_skip_unalloc &&
		    hmp->stripe_bitmap != NULL) {
			int byte_idx = (int)(stripe_num / 8);
			int bit_idx  = (int)(stripe_num % 8);
			if (byte_idx >= (int)hmp->stripe_bitmap_size ||
			    (hmp->stripe_bitmap[byte_idx] &
			     (1 << bit_idx)) == 0)
				continue;
		}

		/*
		 * Slots that never hold a row (metadata zone, zone reserved
		 * segments, space map) were copied by Phase A or are
		 * rewritten at sync; a parity
		 * "rebuild" there would overwrite them with garbage.
		 */
		if (hmp->voldata.version >= HAMMER2_VOL_VERSION_RAIDZ2 &&
		    !hammer2_raid6_slot_is_data(hmp, stripe_num))
			continue;

		phys_off = HAMMER2_ZONE_SEG64 + stripe_num * stripe_unit;

		/*
		 * Exclusive rebuild_lk holds off seals, the only writers
		 * of a sealed slot's parity, until our copy is on the new
		 * disk; a seal that follows rewrites the new disk's column
		 * itself.  A slot already sealed during the rebuild is
		 * done: copying it from media could read a data column
		 * that is still a delayed write.
		 */
		lockmgr(&hmp->rebuild_lk, LK_EXCLUSIVE);
		if (hmp->rebuild_done != NULL &&
		    stripe_num / 8 < hmp->stripe_bitmap_size &&
		    (hmp->rebuild_done[stripe_num / 8] &
		     (1 << (stripe_num % 8)))) {
			lockmgr(&hmp->rebuild_lk, LK_RELEASE);
			continue;
		}

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
			lerror = hammer2_inject_eio(phys_disk);
			if (lerror == 0)
				lerror = hammer2_io_raid6_read_sibling(
				    vol->dev->devvp, phys_off,
				    col_bufs[col], (int)stripe_unit, 1);
			if (lerror) {
				int injected = hammer2_inject_eio(phys_disk);
				/*
				 * The unreadable column is a second
				 * erasure for this stripe, which P+Q
				 * still recover.  A third is fatal.
				 * Injected EIO does not auto-fail the
				 * disk so the test stays repeatable.
				 */
				if (other_failed_col >= 0 ||
				    (!injected &&
				     hammer2_raid6_auto_fail_disk(hmp,
							phys_disk))) {
					lockmgr(&hmp->rebuild_lk, LK_RELEASE);
					error = EIO;
					kprintf("hammer2: resilver stripe"
						" %llu: unrecoverable"
						" I/O error on disk %d\n",
						(unsigned long long)
						stripe_num, phys_disk);
					goto resilver_done;
				}
				bzero(col_bufs[col], stripe_unit);
				other_failed_col = col;
			}
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
		lerror = hammer2_io_raid6_write_stream(new_devvp, phys_off,
		    ptrs[failed_col], (int)stripe_unit);
		lockmgr(&hmp->rebuild_lk, LK_RELEASE);
		if (lerror && !error)
			error = lerror;

		/* Update progress and yield every 256 stripes */
		if ((stripe_num & 255) == 255) {
			hmp->resilver_stripes_done = stripe_num + 1;
			lwkt_yield();
		}
	}

resilver_done:
	/* Free column buffers */
	for (i = 0; i < ndisks; i++)
		kfree(col_bufs[i], M_HAMMER2);

	/* Mark resilver complete */
	hmp->resilver_stripes_done = num_stripes;
	hmp->resilver_running = 0;

	return error;
}

/*
 * RAID6 online scrub (M3 / docs/zfs_compare.md item 3).
 *
 * Walks every live DATA/DIRENT blockref in the chain tree, verifies the
 * data on its primary disk against the bref's CHECK code, and on
 * mismatch reconstructs the column from P+Q parity.  If the
 * reconstruction's CHECK matches, writes the corrected column back to
 * the corrupt disk.
 *
 * Concurrency model (mirrors bulkfree): operates on a chain snapshot
 * taken via hammer2_chain_bulksnap() so live writes proceed unimpeded.
 * Inside the walk we use SHARED chain locks and a bulkfree-style
 * unlock-recurse-relock so writers that need to take EXCLUSIVE on a
 * chain we're past can do so without waiting for the whole walk.  The
 * snapshot view is "state as of the last TXG flush"; chains created
 * since then aren't visible to this scrub but will be covered by the
 * next one.
 *
 * Repair side: the scrub repair-write goes raw against the failed
 * disk's devvp (see hammer2_scrub_verify_bref) and the slots it
 * touches are always "old" (already on disk before the scrub started),
 * so it never collides with the new chains that concurrent writers
 * land in fresh slots from COW.
 */
struct hammer2_scrub_ctx {
	hammer2_dev_t	*hmp;
	uint64_t	done;
	uint64_t	bad;
	uint64_t	repaired;
	uint64_t	unrepairable;
	char		*rbuf;		/* HAMMER2_PBUFSIZE, uncached reads */
};

/*
 * Recompute the bref's CHECK code over (data,bytes) and return 1 on
 * match.  Mirrors hammer2_chain_testcheck() but operates on a raw
 * buffer (no chain pointer needed) so the scrub can verify a
 * parity-reconstructed candidate without manufacturing a fake chain.
 */
static int
hammer2_scrub_check_match(const hammer2_blockref_t *bref,
			  const void *data, size_t bytes)
{
	/*
	 * Shared quiet matcher from hammer2_chain.c — covers every check
	 * method including SHA192 (previously skip-verified here because
	 * io.c lacked the SHA headers).
	 */
	return hammer2_bref_check_match(bref, data, bytes);
}

static int
hammer2_scrub_verify_bref(struct hammer2_scrub_ctx *ctx,
			  const hammer2_blockref_t *bref)
{
	hammer2_dev_t *hmp = ctx->hmp;
	hammer2_io_t *dio = NULL;
	hammer2_off_t lbase;
	hammer2_off_t pbase;
	void *bdata;
	void *recon;
	uint64_t stripe_unit;
	size_t off;
	int lsize;
	int disk_idx;
	int error;
	int repaired = 0;

	if (bref->type != HAMMER2_BREF_TYPE_DATA &&
	    bref->type != HAMMER2_BREF_TYPE_DIRENT)
		return 0;
	if ((bref->data_off & ~HAMMER2_OFF_MASK_RADIX) == 0)
		return 0;

	lbase = bref->data_off & ~HAMMER2_OFF_MASK_RADIX;
	lsize = 1 << (int)(bref->data_off & HAMMER2_OFF_MASK_RADIX);
	disk_idx = (int)bref->copyid;
	stripe_unit = hmp->raid_config.stripe_unit;

	/*
	 * The scrub reads every live block once, so it must not leave
	 * them all in the buffer cache.  On a healthy disk, read the
	 * primary column through the buffer cache key the DIO would use
	 * (so a cached or delayed-write copy is seen), dropping the
	 * buffer afterwards unless it was already cached.  v3 DATA/DIRENT
	 * live at the same byte offset on disk bref->copyid.
	 *
	 * Anything else (failed disk, injected or real read error, the
	 * buffer busy in a DIO) goes through the normal DIO path, which
	 * fails the disk on a read error and transparently reconstructs
	 * from P/Q, so a chain whose primary disk is missing passes scrub
	 * against the surviving parity: no false positives in degraded
	 * mode.
	 */
	bdata = NULL;
	if (disk_idx >= 0 && disk_idx < hmp->nvolumes &&
	    !hmp->raid_failed[disk_idx] &&
	    hammer2_inject_eio(disk_idx) == 0 &&
	    hmp->volumes[disk_idx].dev != NULL &&
	    hmp->volumes[disk_idx].dev->devvp != NULL &&
	    hmp->volumes[disk_idx].dev->open &&
	    hammer2_io_raid6_read_bufcache(
	     hmp->volumes[disk_idx].dev->devvp,
	     (off_t)(lbase & ~HAMMER2_PBUFMASK64), ctx->rbuf,
	     HAMMER2_PBUFSIZE, 1) == 0) {
		bdata = ctx->rbuf + (lbase & HAMMER2_PBUFMASK64);
	}
	if (bdata && hammer2_scrub_check_match(bref, bdata, lsize)) {
		ctx->done++;
		return 0;
	}

	/* A mismatch above is rechecked through the DIO before repair. */
	error = hammer2_io_bread(hmp, bref->type, bref->data_off, lsize,
				 &dio, bref);
	if (error || dio == NULL) {
		if (dio)
			hammer2_io_putblk(&dio);
		ctx->bad++;
		ctx->done++;
		if (disk_idx >= 0 && disk_idx < hmp->raid_config.ndisks)
			atomic_add_64(&hmp->raid_cksum_errors[disk_idx], 1);
		return 0;
	}
	bdata = hammer2_io_data(dio, bref->data_off);
	if (hammer2_scrub_check_match(bref, bdata, lsize)) {
		hammer2_io_putblk(&dio);
		ctx->done++;
		return 0;
	}

	/*
	 * CHECK FAIL on the disk-resident column.  Try a parity
	 * reconstruction over the whole stripe_unit column (treating this
	 * disk as failed), and if the reconstruction's CHECK matches the
	 * bref, write the reconstructed column back to disk *directly*
	 * via raw getblk/bwrite on the failed disk's devvp.
	 *
	 * Two reasons to bypass hammer2_io_bwrite():
	 *
	 *  1. Under v3 the DIO putblk path for DATA/DIRENT btypes hands the
	 *     payload to hammer2_raid6_open_row_add_data() — a repair routed
	 *     through the DIO write API would land in an open packed-row
	 *     tracker with ncols=1 and at seal would zero the surviving
	 *     sibling data column, silently corrupting the row.
	 *
	 *  2. We must release the read-side dio *before* the write-side
	 *     getblk on the same (devvp, phys_off).  The dio holds the buf
	 *     busy via its internal getblk; a second getblk on the same
	 *     key would deadlock waiting for that ref to drop.
	 */
	ctx->bad++;
	if (disk_idx >= 0 && disk_idx < hmp->raid_config.ndisks)
		atomic_add_64(&hmp->raid_cksum_errors[disk_idx], 1);
	hammer2_io_putblk(&dio);

	recon = kmalloc((size_t)stripe_unit, M_HAMMER2, M_WAITOK | M_ZERO);
	pbase = lbase & ~(hammer2_off_t)(stripe_unit - 1);
	error = hammer2_io_raid6_read_degraded(hmp, lbase, disk_idx,
					       recon, (size_t)stripe_unit, 1);
	if (error == 0) {
		off = (size_t)(lbase - pbase);
		if (hammer2_scrub_check_match(bref,
		    (uint8_t *)recon + off, lsize)) {
			struct vnode *devvp;
			struct buf *wbp;
			int werr;

			devvp = hmp->volumes[disk_idx].dev->devvp;
			wbp = getblk(devvp, pbase, (int)stripe_unit,
				     GETBLK_KVABIO, 0);
			if (wbp) {
				if ((wbp->b_flags & (B_CACHE | B_DELWRI)) == 0)
					wbp->b_flags |= B_RELBUF | B_DIRECT;
				bkvasync(wbp);
				bcopy(recon, wbp->b_data, (size_t)stripe_unit);
				werr = bwrite(wbp);
				if (werr == 0) {
					ctx->repaired++;
					repaired = 1;
					if (disk_idx >= 0 &&
					    disk_idx < hmp->raid_config.ndisks) {
						atomic_add_64(
						    &hmp->raid_cksum_healed[
							disk_idx], 1);
					}
					kprintf("hammer2: scrub: repaired "
					    "data_off %016jx disk %d\n",
					    (uintmax_t)bref->data_off,
					    disk_idx);
				} else {
					error = werr;
				}
			}
		}
	}
	if (!repaired) {
		ctx->unrepairable++;
		if (disk_idx >= 0 && disk_idx < hmp->raid_config.ndisks) {
			atomic_add_64(&hmp->raid_cksum_unrepairable[disk_idx],
				      1);
		}
		kprintf("hammer2: scrub: CHECK FAIL data_off %016jx disk %d "
		    "(parity could not repair, err=%d)\n",
		    (uintmax_t)bref->data_off, disk_idx, error);
	}
	kfree(recon, M_HAMMER2);
	ctx->done++;
	return 0;
}

/*
 * Recursive walker.  Caller passes parent *unlocked*; walker locks
 * parent SHARED at entry, unlocks at out:.  Before descending into a
 * child it drops both locks and re-acquires them after the recursive
 * call returns — bulkfree's pattern so writers that need EXCLUSIVE on
 * a chain we already walked past can take it without waiting for the
 * full scrub.
 */
static int
hammer2_scrub_walk(struct hammer2_scrub_ctx *ctx, hammer2_chain_t *parent)
{
	hammer2_chain_t *chain = NULL;
	hammer2_blockref_t bref;
	int first = 1;
	int error = 0;
	int e;

	hammer2_chain_lock(parent, HAMMER2_RESOLVE_ALWAYS |
				   HAMMER2_RESOLVE_SHARED);

	if (parent->error & HAMMER2_ERROR_CHECK) {
		/*
		 * The chain load already attempted a read-path self-heal
		 * (mirror-copy failover for metadata); a persisting CHECK
		 * error means no verifying copy exists.  Count it as
		 * bad+unrepairable instead of silently pruning the
		 * subtree (bitrot.md §7.2 sharp edge).
		 */
		ctx->bad++;
		ctx->unrepairable++;
		ctx->hmp->scrub_brefs_bad = ctx->bad;
		ctx->hmp->scrub_brefs_unrepairable = ctx->unrepairable;
		kprintf("hammer2: scrub: metadata CHECK FAIL data_off %016jx "
		    "type %d — subtree skipped\n",
		    (uintmax_t)parent->bref.data_off, parent->bref.type);
		hammer2_chain_unlock(parent);
		return 0;
	}

	for (;;) {
		e = hammer2_chain_scan(parent, &chain, &bref, &first,
				       HAMMER2_LOOKUP_NODATA |
				       HAMMER2_LOOKUP_SHARED);
		if (e & HAMMER2_ERROR_EOF)
			break;
		if (e & ~HAMMER2_ERROR_CHECK) {
			error = e;
			break;
		}

		hammer2_scrub_verify_bref(ctx, &bref);
		ctx->hmp->scrub_brefs_done = ctx->done;
		ctx->hmp->scrub_brefs_bad = ctx->bad;
		ctx->hmp->scrub_brefs_repaired = ctx->repaired;
		ctx->hmp->scrub_brefs_unrepairable = ctx->unrepairable;

		if (chain == NULL)
			continue;

		/*
		 * Recurse only into interior chain types.  chain_scan
		 * panics on leaves (DATA / DIRENT / FREEMAP_LEAF) and
		 * leaves don't have children to walk anyway.
		 */
		switch (chain->bref.type) {
		case HAMMER2_BREF_TYPE_INODE:
		case HAMMER2_BREF_TYPE_INDIRECT:
		case HAMMER2_BREF_TYPE_VOLUME:
		case HAMMER2_BREF_TYPE_FREEMAP:
		case HAMMER2_BREF_TYPE_FREEMAP_NODE:
			if ((chain->error & HAMMER2_ERROR_CHECK) == 0) {
				hammer2_chain_unlock(chain);
				hammer2_chain_unlock(parent);
				error = hammer2_scrub_walk(ctx, chain);
				hammer2_chain_lock(parent,
				    HAMMER2_RESOLVE_ALWAYS |
				    HAMMER2_RESOLVE_SHARED);
				hammer2_chain_lock(chain,
				    HAMMER2_RESOLVE_ALWAYS |
				    HAMMER2_RESOLVE_SHARED);
				if (error)
					goto out;
			} else {
				/* self-heal failed on an interior chain */
				ctx->bad++;
				ctx->unrepairable++;
				ctx->hmp->scrub_brefs_bad = ctx->bad;
				ctx->hmp->scrub_brefs_unrepairable =
				    ctx->unrepairable;
				kprintf("hammer2: scrub: metadata CHECK FAIL "
				    "data_off %016jx type %d — subtree "
				    "skipped\n",
				    (uintmax_t)chain->bref.data_off,
				    chain->bref.type);
			}
			break;
		default:
			break;
		}
		if ((ctx->done & 255) == 0)
			lwkt_yield();
	}
out:
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
	hammer2_chain_unlock(parent);
	return error;
}

int
hammer2_io_raid6_scrub(hammer2_dev_t *hmp)
{
	struct hammer2_scrub_ctx ctx;
	hammer2_chain_t *vsnap;
	int error;

	KKASSERT(hmp->raid_type == HAMMER2_RAID_TYPE_RAID6);
	KKASSERT(hmp->voldata.version >= HAMMER2_VOL_VERSION_RAIDZ2);

	/* Serialize concurrent scrub invocations */
	if (atomic_cmpset_int((volatile u_int *)&hmp->scrub_running, 0, 1) == 0)
		return EBUSY;

	bzero(&ctx, sizeof(ctx));
	ctx.hmp = hmp;
	ctx.rbuf = kmalloc(HAMMER2_PBUFSIZE, M_HAMMER2, M_WAITOK);

	hmp->scrub_brefs_done = 0;
	hmp->scrub_brefs_bad = 0;
	hmp->scrub_brefs_repaired = 0;
	hmp->scrub_brefs_unrepairable = 0;
	hmp->scrub_error = 0;

	/*
	 * Bulksnap the volume chain so the walker operates on the
	 * synced state from the last TXG flush.  Live writes proceed
	 * against the real vchain unimpeded; chains created since the
	 * last sync aren't covered by this scrub but the next one
	 * picks them up.  Caller passes vsnap *unlocked* — the walker
	 * locks and unlocks parent at every level itself.
	 */
	vsnap = hammer2_chain_bulksnap(hmp);
	error = hammer2_scrub_walk(&ctx, vsnap);
	hammer2_chain_bulkdrop(vsnap);
	kfree(ctx.rbuf, M_HAMMER2);

	hmp->scrub_brefs_done = ctx.done;
	hmp->scrub_brefs_bad = ctx.bad;
	hmp->scrub_brefs_repaired = ctx.repaired;
	hmp->scrub_brefs_unrepairable = ctx.unrepairable;
	hmp->scrub_error = error;
	hmp->scrub_running = 0;

	kprintf("hammer2: scrub complete: %llu brefs, %llu bad, "
	    "%llu repaired, %llu unrepairable, err=%d\n",
	    (unsigned long long)ctx.done,
	    (unsigned long long)ctx.bad,
	    (unsigned long long)ctx.repaired,
	    (unsigned long long)ctx.unrepairable,
	    error);

	return error;
}

static __inline hammer2_io_hash_t *
hammer2_io_hashv(hammer2_dev_t *hmp, hammer2_off_t pbase)
{
	int hv;

	hv = (int)pbase + (int)(pbase >> 16);
	return (&hmp->iohash[hv & HAMMER2_IOHASH_MASK]);
}

/*
 * Lookup and reference the requested dio
 */
static hammer2_io_t *
hammer2_io_hash_lookup(hammer2_dev_t *hmp, hammer2_off_t pbase, uint64_t *refsp)
{
	hammer2_io_hash_t *hash;
	hammer2_io_t *dio;
	uint64_t refs;

	*refsp = 0;
	hash = hammer2_io_hashv(hmp, pbase);
	hammer2_spin_sh(&hash->spin);
	for (dio = hash->base; dio; dio = dio->next) {
		if (dio->pbase == pbase) {
			refs = atomic_fetchadd_64(&dio->refs, 1);
			if ((refs & HAMMER2_DIO_MASK) == 0)
				atomic_add_int(&dio->hmp->iofree_count, -1);
			*refsp = refs;
			break;
		}
	}
	hammer2_spin_unsh(&hash->spin);

	return dio;
}

/*
 * Enter a dio into the hash.  If the pbase already exists in the hash,
 * the xio in the hash is referenced and returned.  If dio is sucessfully
 * entered into the hash, NULL is returned.
 */
static hammer2_io_t *
hammer2_io_hash_enter(hammer2_dev_t *hmp, hammer2_io_t *dio, uint64_t *refsp)
{
	hammer2_io_t *xio;
	hammer2_io_t **xiop;
	hammer2_io_hash_t *hash;
	uint64_t refs;

	*refsp = 0;
	hash = hammer2_io_hashv(hmp, dio->pbase);
	hammer2_spin_ex(&hash->spin);
	for (xiop = &hash->base; (xio = *xiop) != NULL; xiop = &xio->next) {
		if (xio->pbase == dio->pbase) {
			refs = atomic_fetchadd_64(&xio->refs, 1);
			if ((refs & HAMMER2_DIO_MASK) == 0)
				atomic_add_int(&xio->hmp->iofree_count, -1);
			*refsp = refs;
			goto done;
		}
	}
	dio->next = NULL;
	*xiop = dio;
done:
	hammer2_spin_unex(&hash->spin);

	return xio;
}

/*
 * Clean out a limited number of freeable DIOs
 */
static void
hammer2_io_hash_cleanup(hammer2_dev_t *hmp, int dio_limit)
{
	hammer2_io_hash_t *hash;
	hammer2_io_t *dio;
	hammer2_io_t **diop;
	hammer2_io_t **cleanapp;
	hammer2_io_t *cleanbase;
	int count;
	int maxscan;
	int i;

	count = hmp->iofree_count - dio_limit + 32;
	if (count <= 0)
		return;
	cleanbase = NULL;
	cleanapp = &cleanbase;

	i = hmp->io_iterator++;
	maxscan = HAMMER2_IOHASH_SIZE;
	while (count > 0 && maxscan--) {
		hash = &hmp->iohash[i & HAMMER2_IOHASH_MASK];
		hammer2_spin_ex(&hash->spin);
		diop = &hash->base;
		while ((dio = *diop) != NULL) {
			if ((dio->refs & (HAMMER2_DIO_MASK |
					  HAMMER2_DIO_INPROG)) != 0)
			{
				diop = &dio->next;
				continue;
			}
			if (dio->act > 0) {
				int act;

				act = dio->act - (ticks - dio->ticks) / hz - 1;
				dio->act = (act < 0) ? 0 : act;
			}
			if (dio->act) {
				diop = &dio->next;
				continue;
			}
			KKASSERT(dio->bp == NULL);
			*diop = dio->next;
			dio->next = NULL;
			*cleanapp = dio;
			cleanapp = &dio->next;
			--count;
			/* diop remains unchanged */
			atomic_add_int(&hmp->iofree_count, -1);
		}
		hammer2_spin_unex(&hash->spin);
		i = hmp->io_iterator++;
	}

	/*
	 * Get rid of dios on clean list without holding any locks
	 */
	while ((dio = cleanbase) != NULL) {
		cleanbase = dio->next;
		dio->next = NULL;
		KKASSERT(dio->bp == NULL &&
		    (dio->refs & (HAMMER2_DIO_MASK |
				  HAMMER2_DIO_INPROG)) == 0);
		if (dio->refs & HAMMER2_DIO_DIRTY) {
			kprintf("hammer2_io_cleanup: Dirty buffer "
				"%016jx/%d (bp=%p)\n",
				dio->pbase, dio->psize, dio->bp);
		}
		kfree_obj(dio, hmp->mio);
		atomic_add_int(&hammer2_dio_count, -1);
	}
}

/*
 * Destroy all DIOs associated with the media
 */
void
hammer2_io_hash_cleanup_all(hammer2_dev_t *hmp)
{
	hammer2_io_hash_t *hash;
	hammer2_io_t *dio;
	int i;

	for (i = 0; i < HAMMER2_IOHASH_SIZE; ++i) {
		hash = &hmp->iohash[i];

		while ((dio = hash->base) != NULL) {
			hash->base = dio->next;
			dio->next = NULL;
			KKASSERT(dio->bp == NULL &&
			    (dio->refs & (HAMMER2_DIO_MASK |
					  HAMMER2_DIO_INPROG)) == 0);
			if (dio->refs & HAMMER2_DIO_DIRTY) {
				kprintf("hammer2_io_cleanup: Dirty buffer "
					"%016jx/%d (bp=%p)\n",
					dio->pbase, dio->psize, dio->bp);
			}
			kfree_obj(dio, hmp->mio);
			atomic_add_int(&hammer2_dio_count, -1);
			atomic_add_int(&hmp->iofree_count, -1);
		}
	}
}
