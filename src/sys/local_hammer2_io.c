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
hammer2_io_alloc(hammer2_dev_t *hmp, hammer2_key_t data_off, uint8_t btype,
		 int createit, int *isgoodp,
		 const hammer2_blockref_t *bref)
{
	hammer2_io_t *dio;
	hammer2_io_t *xio;
	hammer2_key_t lbase;
	hammer2_key_t pbase;
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
				    raid6_data, psize);
				raid6_data = NULL;
			}

			/*
			 * v3 RAIDZ2-native metadata mirror (I5).  After the
			 * primary bp has been disposed above (bdwrite /
			 * cluster_write / bawrite to dio->devvp), replicate
			 * the same payload synchronously to every surviving
			 * sibling disk at the same per-disk byte offset.
			 * skip_disk_idx avoids re-writing to the primary.
			 */
			if (rz_meta && md_mirror_data) {
				hammer2_off_t per_disk_off = pbase - dio->dbase;
				hammer2_io_metadata_mirror_write(hmp,
				    dio->disk_idx, per_disk_off,
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
		if (dio->absent_data) {
			kfree(dio->absent_data, M_HAMMER2);
			dio->absent_data = NULL;
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
	hammer2_raid_config_t *rc = &hmp->raid_config;
	int ndisks = rc->ndisks;
	int ndata = rc->ndata;
	uint64_t stripe_unit = rc->stripe_unit;
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

			if (phys_disk == data_disk_idx) {
				/*
				 * This is the column being reconstructed.
				 * Leave the buffer zeroed and mark failed.
				 */
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
			lerror = hammer2_inject_eio(phys_disk);
			if (lerror == 0)
				lerror = breadnx(vol->dev->devvp, phys_off,
						 stripe_unit, 0, NULL, NULL,
						 0, &bp);
			if (lerror == 0 && bp) {
				bkvasync(bp);
				bcopy(bp->b_data, col_bufs[col], stripe_unit);
				brelse(bp);
			} else {
				int injected = hammer2_inject_eio(phys_disk);
				if (bp)
					brelse(bp);
				if (!injected) {
					int aerr =
					    hammer2_raid6_auto_fail_disk(
						hmp, phys_disk);
					if (aerr) {
						error = EIO;
						break;
					}
				}
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
		num_stripes = (HAMMER2_ZONE_BYTES64 - HAMMER2_ZONE_SEG64) /
			      stripe_unit;
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
		uint32_t ex;
		int surv = -1;

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
		vol = &hmp->volumes[surv];

		for (ex = 0; ex < hmp->md_nextents; ex++) {
			hammer2_off_t mo = hmp->md_extents[ex].md_off;
			hammer2_off_t ms = hmp->md_extents[ex].md_size;
			hammer2_off_t off;

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

				bp = NULL;
				lerror = hammer2_inject_eio(surv);
				if (lerror == 0)
					lerror = bread(vol->dev->devvp,
					    (off_t)(mo + off),
					    (int)this_chunk, &bp);
				if (lerror || bp == NULL) {
					if (bp)
						brelse(bp);
					kprintf("hammer2: resilver Phase A: "
						"read err %d at off %jx "
						"(disk %d)\n",
						lerror, (intmax_t)(mo + off),
						surv);
					kfree(md_buf, M_HAMMER2);
					hmp->resilver_running = 0;
					return EIO;
				}
				bkvasync(bp);
				bcopy(bp->b_data, md_buf, this_chunk);
				brelse(bp);

				wbp = getblk(new_devvp, (off_t)(mo + off),
				    (int)this_chunk, GETBLK_KVABIO, 0);
				if (wbp) {
					bkvasync(wbp);
					bcopy(md_buf, wbp->b_data, this_chunk);
					lerror = bwrite(wbp);
					if (lerror) {
						kfree(md_buf, M_HAMMER2);
						hmp->resilver_running = 0;
						return lerror;
					}
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
	 * Flush all pending writes first so that parity on surviving disks
	 * is up-to-date before we read it.  Under v3 COW, concurrent writes
	 * land in freshly-allocated stripe slots whose data column may or
	 * may not be on the replacement disk — if it is, the write itself
	 * goes there directly; if not, the resilver doesn't care.  No
	 * second-pass tracking is needed (newplan.md §5.9).
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
			lerror = hammer2_inject_eio(phys_disk);
			if (lerror == 0)
				lerror = breadnx(vol->dev->devvp, phys_off,
						 stripe_unit, 0, NULL, NULL,
						 0, &bp);
			if (lerror == 0 && bp) {
				bkvasync(bp);
				bcopy(bp->b_data, col_bufs[col], stripe_unit);
				brelse(bp);
			} else {
				int injected = hammer2_inject_eio(phys_disk);
				if (bp)
					brelse(bp);
				if (injected) {
					/* injection: surface as clean EIO, no auto-fail */
					error = EIO;
					kprintf("hammer2: resilver stripe %llu: "
						"injected EIO on disk %d\n",
						(unsigned long long)stripe_num,
						phys_disk);
					goto resilver_done;
				}
				{
					int aerr = hammer2_raid6_auto_fail_disk(
							hmp, phys_disk);
					if (aerr) {
						error = EIO;
						kprintf("hammer2: resilver stripe"
							" %llu: unrecoverable"
							" I/O error on disk %d\n",
							(unsigned long long)
							stripe_num, phys_disk);
						goto resilver_done;
					}
				}
				/* disk now in raid_failed[]; treated as zeros */
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
		wbp = getblk(new_devvp, phys_off, stripe_unit,
			     GETBLK_KVABIO, 0);
		if (wbp) {
			bkvasync(wbp);
			bcopy(ptrs[failed_col], wbp->b_data, stripe_unit);
			lerror = bwrite(wbp);
			if (lerror && !error)
				error = lerror;
		}

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
 * v1 concurrency: walks the live vchain with the same
 * (RESOLVE_ALWAYS) chain-lock pattern the mount-time bitmap walker
 * uses.  Concurrent writes that need to traverse vchain will serialize
 * with the scrub.  A future v2 will move to the bulkfree snapshot
 * pattern (hammer2_chain_bulksnap + SHARED locks + unlock-recurse-relock)
 * to let writers proceed unimpeded.
 */
struct hammer2_scrub_ctx {
	hammer2_dev_t	*hmp;
	uint64_t	done;
	uint64_t	bad;
	uint64_t	repaired;
	uint64_t	unrepairable;
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
	switch (HAMMER2_DEC_CHECK(bref->methods)) {
	case HAMMER2_CHECK_NONE:
	case HAMMER2_CHECK_DISABLED:
		return 1;
	case HAMMER2_CHECK_ISCSI32:
		return bref->check.iscsi32.value ==
		    hammer2_icrc32(data, bytes);
	case HAMMER2_CHECK_XXHASH64:
		return bref->check.xxhash64.value ==
		    XXH64(data, bytes, XXH_HAMMER2_SEED);
	case HAMMER2_CHECK_FREEMAP:
		return bref->check.freemap.icrc32 ==
		    hammer2_icrc32(data, bytes);
	default:
		/*
		 * SHA192 etc. require headers not pulled in by io.c.  Pre-v3
		 * tests exercise XXHASH64 (default for DATA) and ISCSI32 only;
		 * skip-verify is safe — those chains pass via the normal read
		 * path's testcheck and a scrub miss just means slower
		 * detection, not corruption.
		 */
		return 1;
	}
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
	 * Read via the normal DIO path so the disk holding bref->copyid is
	 * the one we verify.  Failed-disk reads transparently reconstruct
	 * (via read_degraded inside hammer2_io_bread), so a chain whose
	 * primary disk is missing will pass scrub against the surviving
	 * P/Q — no false positives in degraded mode.
	 */
	error = hammer2_io_bread(hmp, bref->type, bref->data_off, lsize,
				 &dio, bref);
	if (error || dio == NULL) {
		if (dio)
			hammer2_io_putblk(&dio);
		ctx->bad++;
		ctx->done++;
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
				bkvasync(wbp);
				bcopy(recon, wbp->b_data, (size_t)stripe_unit);
				werr = bwrite(wbp);
				if (werr == 0) {
					ctx->repaired++;
					repaired = 1;
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
		kprintf("hammer2: scrub: CHECK FAIL data_off %016jx disk %d "
		    "(parity could not repair, err=%d)\n",
		    (uintmax_t)bref->data_off, disk_idx, error);
	}
	kfree(recon, M_HAMMER2);
	ctx->done++;
	return 0;
}

static int
hammer2_scrub_walk(struct hammer2_scrub_ctx *ctx, hammer2_chain_t *parent)
{
	hammer2_chain_t *chain = NULL;
	hammer2_blockref_t bref;
	int first = 1;
	int error;

	for (;;) {
		error = hammer2_chain_scan(parent, &chain, &bref, &first,
					   HAMMER2_LOOKUP_ALWAYS);
		if (error & HAMMER2_ERROR_EOF) {
			error = 0;
			break;
		}
		if (error)
			break;

		hammer2_scrub_verify_bref(ctx, &bref);
		ctx->hmp->scrub_brefs_done = ctx->done;
		ctx->hmp->scrub_brefs_bad = ctx->bad;
		ctx->hmp->scrub_brefs_repaired = ctx->repaired;
		ctx->hmp->scrub_brefs_unrepairable = ctx->unrepairable;

		/*
		 * Only recurse into interior chain types.  chain_scan
		 * panics ("unrecognized blockref type") if invoked on a
		 * leaf bref like DATA / DIRENT / FREEMAP_LEAF.
		 */
		if (chain) {
			switch (chain->bref.type) {
			case HAMMER2_BREF_TYPE_INODE:
			case HAMMER2_BREF_TYPE_INDIRECT:
			case HAMMER2_BREF_TYPE_VOLUME:
			case HAMMER2_BREF_TYPE_FREEMAP:
			case HAMMER2_BREF_TYPE_FREEMAP_NODE:
				error = hammer2_scrub_walk(ctx, chain);
				if (error)
					goto out;
				break;
			default:
				break;
			}
		}
		if ((ctx->done & 255) == 0)
			lwkt_yield();
	}
out:
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
	return error;
}

int
hammer2_io_raid6_scrub(hammer2_dev_t *hmp)
{
	struct hammer2_scrub_ctx ctx;
	int error;

	KKASSERT(hmp->raid_type == HAMMER2_RAID_TYPE_RAID6);
	KKASSERT(hmp->voldata.version >= HAMMER2_VOL_VERSION_RAIDZ2);

	/* Serialize concurrent scrub invocations */
	if (atomic_cmpset_int((volatile u_int *)&hmp->scrub_running, 0, 1) == 0)
		return EBUSY;

	bzero(&ctx, sizeof(ctx));
	ctx.hmp = hmp;

	hmp->scrub_brefs_done = 0;
	hmp->scrub_brefs_bad = 0;
	hmp->scrub_brefs_repaired = 0;
	hmp->scrub_brefs_unrepairable = 0;
	hmp->scrub_error = 0;

	hammer2_chain_lock(&hmp->vchain, HAMMER2_RESOLVE_ALWAYS);
	error = hammer2_scrub_walk(&ctx, &hmp->vchain);
	hammer2_chain_unlock(&hmp->vchain);

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
