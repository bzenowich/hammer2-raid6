/*
 * Copyright (c) 2020 Tomohiro Kusumi <tkusumi@netbsd.org>
 * Copyright (c) 2020 The DragonFly Project
 * All rights reserved.
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
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/queue.h>
#include <sys/nlookup.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/buf.h>
#include <sys/uuid.h>
#include <sys/objcache.h>
#include <sys/lock.h>

#include "hammer2.h"
#include "hammer2_raid6.h"

extern int hammer2_j2_allow_rollback;
extern int hammer2_j2_rollback_max;

#define hprintf(X, ...)	kprintf("hammer2_ondisk: " X, ## __VA_ARGS__)

static int
hammer2_lookup_device(const char *path, int rootmount, struct vnode **devvp)
{
	struct vnode *vp = NULL;
	struct nlookupdata nd;
	int error = 0;

	KKASSERT(path);
	KKASSERT(*path != '\0');

	if (rootmount) {
		error = bdevvp(kgetdiskbyname(path), &vp);
		if (error)
			hprintf("cannot find %s %d\n", path, error);
	} else {
		error = nlookup_init(&nd, path, UIO_SYSSPACE, NLC_FOLLOW);
		if (error == 0)
			error = nlookup(&nd);
		if (error == 0)
			error = cache_vref(&nd.nl_nch, nd.nl_cred, &vp);
		if (error)
			hprintf("failed to nlookup %s %d\n", path, error);
		nlookup_done(&nd);
	}

	if (error == 0) {
		KKASSERT(vp);
		if (!vn_isdisk(vp, &error)) {
			KKASSERT(error);
			hprintf("%s not a block device %d\n", path, error);
		}
	}

	if (error && vp) {
		vrele(vp);
		vp = NULL;
	}

	*devvp = vp;
	return error;
}

int
hammer2_open_devvp(const hammer2_devvp_list_t *devvpl, int ronly)
{
	hammer2_devvp_t *e;
	struct vnode *devvp;
	const char *path;
	int count, error, ndevs, nopen;

	ndevs = 0;
	nopen = 0;
	TAILQ_FOREACH(e, devvpl, entry)
		ndevs++;

	TAILQ_FOREACH(e, devvpl, entry) {
		devvp = e->devvp;
		path = e->path;
		/*
		 * Skip dummy vnodes for absent devices (degraded mount).
		 * These have v_rdev == NULL because they were not
		 * associated with a real device.
		 */
		if (devvp->v_rdev == NULL) {
			e->open = 0;
			continue;
		}
		count = vcount(devvp);
		if (count > 0) {
			hprintf("%s already has %d references\n", path, count);
			return EBUSY;
		}
		vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
		error = vinvalbuf(devvp, V_SAVE, 0, 0);
		if (error == 0) {
			KKASSERT(!e->open);
			error = VOP_OPEN(devvp, (ronly ? FREAD : FREAD|FWRITE),
					 FSCRED, NULL);
			if (error == 0)
				e->open = 1;
			else
				hprintf("failed to open %s %d\n", path, error);
		}
		vn_unlock(devvp);
		if (error) {
			/*
			 * For multi-device RAID, tolerate open failures.
			 * The device exists but can't be opened — mark it
			 * as not-open and continue.  Downstream code will
			 * treat it as a failed disk.
			 */
			if (ndevs > 1) {
				hprintf("%s open failed (%d), "
					"will attempt degraded mount\n",
					path, error);
				e->open = 0;
				continue;
			}
			return error;
		}
		nopen++;
	}

	/*
	 * For multi-device mounts, ensure at least some devices opened.
	 * Detailed count checks happen later in verify_volumes.
	 */
	if (ndevs > 1 && nopen == 0) {
		hprintf("no devices could be opened\n");
		return ENXIO;
	}

	return 0;
}

int
hammer2_close_devvp(const hammer2_devvp_list_t *devvpl, int ronly)
{
	hammer2_devvp_t *e;
	struct vnode *devvp;

	TAILQ_FOREACH(e, devvpl, entry) {
		devvp = e->devvp;
		if (devvp == NULL)
			continue;
		if (e->open) {
			struct buf *bp;

			/*
			 * Issue a device-level flush barrier to wait for
			 * all in-flight async I/O (bawrite/cluster_write)
			 * to complete.  vinvalbuf only sees buffers still
			 * on the vnode's buffer lists, but bawrite moves
			 * them off-list into the driver.  Without this
			 * barrier, vnconfig -u after unmount can destroy
			 * the device while I/O is in flight, permanently
			 * leaking runningbufspace.
			 */
			vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
			bp = getpbuf(NULL);
			bp->b_bio1.bio_offset = 0;
			bp->b_bufsize = 0;
			bp->b_bcount = 0;
			bp->b_cmd = BUF_CMD_FLUSH;
			bp->b_bio1.bio_done = biodone_sync;
			bp->b_bio1.bio_flags |= BIO_SYNC;
			vn_strategy(devvp, &bp->b_bio1);
			biowait(&bp->b_bio1, "h2cls");
			relpbuf(bp, NULL);

			vinvalbuf(devvp, (ronly ? 0 : V_SAVE), 0, 0);
			VOP_CLOSE(devvp, (ronly ? FREAD : FREAD|FWRITE), NULL);
			vn_unlock(devvp);
			e->open = 0;
		} else {
			/*
			 * Device already closed (failed disk).  Discard
			 * any buffers in the buffer cache that reference
			 * this devvp.  Without this, orphaned dirty
			 * buffers can permanently leak runningbufspace
			 * when the device is later destroyed (vnconfig -u).
			 */
			vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
			vinvalbuf(devvp, 0, 0, 0);
			vn_unlock(devvp);
		}
	}

	return 0;
}

int
hammer2_init_devvp(const char *blkdevs, int rootmount,
		   hammer2_devvp_list_t *devvpl)
{
	hammer2_devvp_t *e;
	struct vnode *devvp;
	const char *p;
	char *path;
	int i, error = 0;

	KKASSERT(TAILQ_EMPTY(devvpl));
	KKASSERT(blkdevs); /* could be empty string */
	p = blkdevs;

	path = objcache_get(namei_oc, M_WAITOK);
	while (1) {
		strcpy(path, "");
		if (*p != '/') {
			strcpy(path, "/dev/"); /* relative path */
		}
		/* scan beyond "/dev/" */
		for (i = strlen(path); i < MAXPATHLEN-1; ++i) {
			if (*p == '\0') {
				break;
			} else if (*p == ':') {
				p++;
				break;
			} else {
				path[i] = *p;
				p++;
			}
		}
		path[i] = '\0';
		/* path shorter than "/dev/" means invalid or done */
		if (strlen(path) <= strlen("/dev/")) {
			if (strlen(p)) {
				hprintf("ignore incomplete path %s\n", path);
				continue;
			} else {
				/* end of string */
				KKASSERT(*p == '\0');
				break;
			}
		}
		/* lookup path from above */
		devvp = NULL;
		error = hammer2_lookup_device(path, rootmount, &devvp);
		if (error) {
			KKASSERT(!devvp);
			/*
			 * For multi-device RAID configurations, tolerate
			 * device lookup failures.  Create a placeholder
			 * entry with devvp=NULL so the mount can proceed
			 * in degraded mode.  Single-device mounts still
			 * fail immediately.
			 */
			if (*p != '\0' || !TAILQ_EMPTY(devvpl)) {
				struct vnode *dummy_vp;

				hprintf("device %s not found (%d), "
					"will attempt degraded mount\n",
					path, error);
				/*
				 * Create a dummy vnode so the buffer cache
				 * has a valid anchor for degraded I/O paths
				 * (getblk/brelse).  Uses dead vnode ops so
				 * any accidental I/O returns an error.
				 */
				if (getspecialvnode(VT_NON, NULL,
				    &dead_vnode_vops_p, &dummy_vp, 0, 0)) {
					hprintf("cannot allocate dummy "
						"vnode for %s\n", path);
					break;
				}
				dummy_vp->v_type = VCHR;
				vx_unlock(dummy_vp);
				e = kmalloc(sizeof(*e), M_HAMMER2,
					    M_WAITOK | M_ZERO);
				e->devvp = dummy_vp;
				e->path = kstrdup(path, M_HAMMER2);
				e->open = 0;
				TAILQ_INSERT_TAIL(devvpl, e, entry);
				error = 0;
				continue;
			}
			hprintf("failed to lookup %s %d\n", path, error);
			break;
		}
		KKASSERT(devvp);
		e = kmalloc(sizeof(*e), M_HAMMER2, M_WAITOK | M_ZERO);
		e->devvp = devvp;
		e->path = kstrdup(path, M_HAMMER2);
		TAILQ_INSERT_TAIL(devvpl, e, entry);
	}
	objcache_put(namei_oc, path);

	return error;
}

void
hammer2_cleanup_devvp(hammer2_devvp_list_t *devvpl)
{
	hammer2_devvp_t *e;

	while (!TAILQ_EMPTY(devvpl)) {
		e = TAILQ_FIRST(devvpl);
		TAILQ_REMOVE(devvpl, e, entry);
		/* devvp */
		if (e->devvp) {
			if (e->devvp->v_rdev)
				e->devvp->v_rdev->si_mountpoint = NULL;
			vrele(e->devvp);
		}
		e->devvp = NULL;
		/* path */
		KKASSERT(e->path);
		kfree(e->path, M_HAMMER2);
		e->path = NULL;
		kfree(e, M_HAMMER2);
	}
}

static int
hammer2_verify_volumes_common(const hammer2_volume_t *volumes)
{
	const hammer2_volume_t *vol;
	struct partinfo part;
	const char *path;
	int i;

	for (i = 0; i < HAMMER2_MAX_VOLUMES; ++i) {
		vol = &volumes[i];
		if (vol->id == -1)
			continue;
		path = vol->dev->path;
		/*
		 * Skip volumes with unavailable devices (degraded mount).
		 * Absent disks have a dummy devvp but open=0.
		 */
		if (!vol->dev->open)
			continue;
		if (vol->offset == (hammer2_off_t)-1) {
			hprintf("%s has bad offset 0x%016jx\n", path,
				(intmax_t)vol->offset);
			return EINVAL;
		}
		if (vol->size == (hammer2_off_t)-1) {
			hprintf("%s has bad size 0x%016jx\n", path,
				(intmax_t)vol->size);
			return EINVAL;
		}
		/* check volume size vs block device size */
		if (VOP_IOCTL(vol->dev->devvp, DIOCGPART, (void*)&part, 0,
			      curthread->td_ucred , NULL) == 0) {
			if (vol->size > part.media_size) {
				hprintf("%s's size 0x%016jx exceeds device size "
					"0x%016jx\n", path, (intmax_t)vol->size,
					part.media_size);
				return EINVAL;
			}
		}
	}

	return 0;
}

static int
hammer2_verify_volumes_1(const hammer2_volume_t *volumes,
		         const hammer2_volume_data_t *rootvoldata)
{
	const hammer2_volume_t *vol;
	hammer2_off_t off;
	const char *path;
	int i, nvolumes = 0;

	/* check initialized volume count */
	for (i = 0; i < HAMMER2_MAX_VOLUMES; ++i) {
		vol = &volumes[i];
		if (vol->id != -1)
			nvolumes++;
	}
	if (nvolumes != 1) {
		hprintf("only 1 volume supported\n");
		return EINVAL;
	}

	/* check volume header */
	if (rootvoldata->volu_id) {
		hprintf("volume id %d must be 0\n", rootvoldata->volu_id);
		return EINVAL;
	}
	if (rootvoldata->nvolumes) {
		hprintf("volume count %d must be 0\n", rootvoldata->nvolumes);
		return EINVAL;
	}
	if (rootvoldata->total_size) {
		hprintf("total size 0x%016jx must be 0\n",
			(intmax_t)rootvoldata->total_size);
		return EINVAL;
	}
	for (i = 0; i < HAMMER2_MAX_VOLUMES; ++i) {
		off = rootvoldata->volu_loff[i];
		if (off) {
			hprintf("volume offset[%d] 0x%016jx must be 0\n", i,
				(intmax_t)off);
			return EINVAL;
		}
	}

	/* check volume */
	vol = &volumes[0];
	path = vol->dev->path;
	if (vol->id) {
		hprintf("%s has non zero id %d\n", path, vol->id);
		return EINVAL;
	}
	if (vol->offset) {
		hprintf("%s has non zero offset 0x%016jx\n", path,
			(intmax_t)vol->offset);
		return EINVAL;
	}
	if (vol->size & HAMMER2_VOLUME_ALIGNMASK64) {
		hprintf("%s's size is not 0x%016jx aligned\n", path,
			(intmax_t)HAMMER2_VOLUME_ALIGN);
		return EINVAL;
	}

	return 0;
}

static int
hammer2_verify_volumes_2(const hammer2_volume_t *volumes,
		         const hammer2_volume_data_t *rootvoldata)
{
	const hammer2_volume_t *vol;
	hammer2_off_t off, total_size = 0;
	const char *path;
	int i, nvolumes = 0;

	/* check initialized volume count */
	for (i = 0; i < HAMMER2_MAX_VOLUMES; ++i) {
		vol = &volumes[i];
		if (vol->id != -1) {
			nvolumes++;
			total_size += vol->size;
		}
	}

	/* check volume header */
	if (rootvoldata->volu_id != HAMMER2_ROOT_VOLUME) {
		hprintf("volume id %d must be %d\n", rootvoldata->volu_id,
			HAMMER2_ROOT_VOLUME);
		return EINVAL;
	}
	if (rootvoldata->nvolumes != nvolumes) {
		hprintf("volume header requires %d devices, %d specified\n",
			rootvoldata->nvolumes, nvolumes);
		return EINVAL;
	}
	if (rootvoldata->total_size != total_size) {
		hprintf("total size 0x%016jx does not equal sum of volumes 0x%016jx\n",
			rootvoldata->total_size, total_size);
		return EINVAL;
	}
	for (i = 0; i < nvolumes; ++i) {
		off = rootvoldata->volu_loff[i];
		if (off == (hammer2_off_t)-1) {
			hprintf("volume offset[%d] 0x%016jx must not be -1\n",
				i, (intmax_t)off);
			return EINVAL;
		}
	}
	for (i = nvolumes; i < HAMMER2_MAX_VOLUMES; ++i) {
		off = rootvoldata->volu_loff[i];
		if (off != (hammer2_off_t)-1) {
			hprintf("volume offset[%d] 0x%016jx must be -1\n",
				i, (intmax_t)off);
			return EINVAL;
		}
	}

	/* check volumes */
	for (i = 0; i < HAMMER2_MAX_VOLUMES; ++i) {
		vol = &volumes[i];
		if (vol->id == -1)
			continue;
		path = vol->dev->path;
		/* check offset */
		if (vol->offset & HAMMER2_FREEMAP_LEVEL1_MASK) {
			hprintf("%s's offset 0x%016jx not 0x%016jx aligned\n",
				path, (intmax_t)vol->offset,
				HAMMER2_FREEMAP_LEVEL1_SIZE);
			return EINVAL;
		}
		/* check vs previous volume */
		if (i) {
			if (vol->id <= (vol-1)->id) {
				hprintf("%s has inconsistent id %d\n", path,
					vol->id);
				return EINVAL;
			}
			if (vol->offset != (vol-1)->offset + (vol-1)->size) {
				hprintf("%s has inconsistent offset 0x%016jx\n",
					path, (intmax_t)vol->offset);
				return EINVAL;
			}
		} else { /* first */
			if (vol->offset) {
				hprintf("%s has non zero offset 0x%016jx\n",
					path, (intmax_t)vol->offset);
				return EINVAL;
			}
		}
		/* check size for non-last and last volumes */
		if (i != rootvoldata->nvolumes - 1) {
			if (vol->size < HAMMER2_FREEMAP_LEVEL1_SIZE) {
				hprintf("%s's size must be >= 0x%016jx\n", path,
					(intmax_t)HAMMER2_FREEMAP_LEVEL1_SIZE);
				return EINVAL;
			}
			if (vol->size & HAMMER2_FREEMAP_LEVEL1_MASK) {
				hprintf("%s's size is not 0x%016jx aligned\n",
					path,
					(intmax_t)HAMMER2_FREEMAP_LEVEL1_SIZE);
				return EINVAL;
			}
		} else { /* last */
			if (vol->size & HAMMER2_VOLUME_ALIGNMASK64) {
				hprintf("%s's size is not 0x%016jx aligned\n",
					path,
					(intmax_t)HAMMER2_VOLUME_ALIGN);
				return EINVAL;
			}
		}
	}

	return 0;
}

/*
 * Verify RAID 6 volume configuration.
 * All volumes must exist, minimum 4 disks, raid_config must be valid.
 */
static int
hammer2_verify_volumes_3(const hammer2_volume_t *volumes,
			 const hammer2_volume_data_t *rootvoldata)
{
	const hammer2_raid_config_t *rc = &rootvoldata->raid_config;
	const hammer2_volume_t *vol;
	int i, nvolumes = 0;
	hammer2_off_t min_size = (hammer2_off_t)-1;

	/* count present (open) volumes */
	for (i = 0; i < HAMMER2_MAX_VOLUMES; ++i) {
		vol = &volumes[i];
		if (vol->id != -1 && vol->dev && vol->dev->open) {
			nvolumes++;
			if (vol->size < min_size)
				min_size = vol->size;
		}
	}

	if (rc->raid_type != HAMMER2_RAID_TYPE_RAID6) {
		hprintf("RAID6 version but raid_type is %d\n", rc->raid_type);
		return EINVAL;
	}
	if (rc->ndisks < HAMMER2_RAID6_MIN_DISKS) {
		hprintf("RAID6 requires at least %d disks, have %d\n",
			HAMMER2_RAID6_MIN_DISKS, rc->ndisks);
		return EINVAL;
	}
	if (nvolumes > rc->ndisks || nvolumes < rc->ndisks - 2) {
		hprintf("RAID6 requires %d-%d disks, %d found\n",
			rc->ndisks - 2, rc->ndisks, nvolumes);
		return EINVAL;
	}
	if (nvolumes < rc->ndisks) {
		hprintf("RAID6 degraded mount: %d of %d disks present\n",
			nvolumes, rc->ndisks);
	}
	if (rc->ndata != rc->ndisks - 2) {
		hprintf("ndata %d inconsistent with ndisks %d\n",
			rc->ndata, rc->ndisks);
		return EINVAL;
	}
	if (rc->stripe_unit != HAMMER2_PBUFSIZE) {
		hprintf("stripe_unit 0x%jx must be 0x%x\n",
			(intmax_t)rc->stripe_unit, HAMMER2_PBUFSIZE);
		return EINVAL;
	}
	if (rootvoldata->volu_id != HAMMER2_ROOT_VOLUME &&
	    nvolumes == rc->ndisks) {
		hprintf("volume id %d must be %d\n",
			rootvoldata->volu_id, HAMMER2_ROOT_VOLUME);
		return EINVAL;
	}

	/*
	 * Each disk's volu_id must be within the array bounds.
	 * hammer2_init_volumes assigns disks to volumes[volu_id] slots,
	 * so a volu_id >= ndisks would index outside the active range.
	 */
	for (i = 0; i < HAMMER2_MAX_VOLUMES; i++) {
		vol = &volumes[i];
		if (vol->id != -1 && vol->dev && vol->dev->open &&
		    vol->id >= rc->ndisks) {
			hprintf("disk with volu_id %d exceeds ndisks %d\n",
				vol->id, rc->ndisks);
			return EINVAL;
		}
	}

	return 0;
}

static int
hammer2_verify_volumes(const hammer2_volume_t *volumes,
		       const hammer2_volume_data_t *rootvoldata)
{
	int error;

	error = hammer2_verify_volumes_common(volumes);
	if (error)
		return error;

	if (rootvoldata->version >= HAMMER2_VOL_VERSION_RAID6)
		return hammer2_verify_volumes_3(volumes, rootvoldata);
	else if (rootvoldata->version >= HAMMER2_VOL_VERSION_MULTI_VOLUMES)
		return hammer2_verify_volumes_2(volumes, rootvoldata);
	else
		return hammer2_verify_volumes_1(volumes, rootvoldata);
}

/*
 * Returns zone# of returned volume header or < 0 on failure.
 */
static int
hammer2_read_volume_header(struct vnode *devvp, const char *path,
			   hammer2_volume_data_t *voldata)
{
	hammer2_volume_data_t *vd;
	struct buf *bp = NULL;
	hammer2_crc32_t crc0, crc1;
	int zone = -1;
	int i;

	/*
	 * There are up to 4 copies of the volume header (syncs iterate
	 * between them so there is no single master).  We don't trust the
	 * volu_size field so we don't know precisely how large the filesystem
	 * is, so depend on the OS to return an error if we go beyond the
	 * block device's EOF.
	 */
	for (i = 0; i < HAMMER2_NUM_VOLHDRS; ++i) {
		if (bread(devvp, i * HAMMER2_ZONE_BYTES64, HAMMER2_VOLUME_BYTES,
			  &bp)) {
			brelse(bp);
			bp = NULL;
			continue;
		}

		vd = (struct hammer2_volume_data *)bp->b_data;
		/* verify volume header magic */
		if ((vd->magic != HAMMER2_VOLUME_ID_HBO) &&
		    (vd->magic != HAMMER2_VOLUME_ID_ABO)) {
			hprintf("%s #%d: bad magic\n", path, i);
			brelse(bp);
			bp = NULL;
			continue;
		}

		if (vd->magic == HAMMER2_VOLUME_ID_ABO) {
			/* XXX: Reversed-endianness filesystem */
			hprintf("%s #%d: reverse-endian filesystem detected\n",
				path, i);
			brelse(bp);
			bp = NULL;
			continue;
		}

		/* verify volume header CRC's */
		crc0 = vd->icrc_sects[HAMMER2_VOL_ICRC_SECT0];
		crc1 = hammer2_icrc32(bp->b_data + HAMMER2_VOLUME_ICRC0_OFF,
				      HAMMER2_VOLUME_ICRC0_SIZE);
		if (crc0 != crc1) {
			hprintf("%s #%d: volume header crc mismatch sect0 %08x/%08x\n",
				path, i, crc0, crc1);
			brelse(bp);
			bp = NULL;
			continue;
		}
		crc0 = vd->icrc_sects[HAMMER2_VOL_ICRC_SECT1];
		crc1 = hammer2_icrc32(bp->b_data + HAMMER2_VOLUME_ICRC1_OFF,
				      HAMMER2_VOLUME_ICRC1_SIZE);
		if (crc0 != crc1) {
			hprintf("%s #%d: volume header crc mismatch sect1 %08x/%08x\n",
				path, i, crc0, crc1);
			brelse(bp);
			bp = NULL;
			continue;
		}
		crc0 = vd->icrc_volheader;
		crc1 = hammer2_icrc32(bp->b_data + HAMMER2_VOLUME_ICRCVH_OFF,
				      HAMMER2_VOLUME_ICRCVH_SIZE);
		if (crc0 != crc1) {
			hprintf("%s #%d: volume header crc mismatch vh %08x/%08x\n",
				path, i, crc0, crc1);
			brelse(bp);
			bp = NULL;
			continue;
		}

		if (zone == -1 || voldata->mirror_tid < vd->mirror_tid) {
			*voldata = *vd;
			zone = i;
		}
		brelse(bp);
		bp = NULL;
	}

	if (zone == -1) {
		hprintf("%s has no valid volume headers\n", path);
		return -EINVAL;
	}
	return zone;
}

static void
hammer2_print_uuid_mismatch(uuid_t *uuid1, uuid_t *uuid2, const char *id)
{
	char buf1[64], buf2[64];

	snprintf_uuid(buf1, sizeof(buf1), uuid1);
	snprintf_uuid(buf2, sizeof(buf2), uuid2);

	hprintf("%s uuid mismatch %s vs %s\n", id, buf1, buf2);
}

int
hammer2_init_volumes(struct mount *mp, const hammer2_devvp_list_t *devvpl,
		     hammer2_volume_t *volumes,
		     hammer2_volume_data_t *rootvoldata,
		     int *rootvolzone,
		     struct vnode **rootvoldevvp)
{
	hammer2_devvp_t *e;
	hammer2_volume_data_t *voldata;
	hammer2_volume_t *vol;
	struct vnode *devvp;
	const char *path;
	uuid_t fsid, fstype;
	int i, zone, error = 0, version = -1, nvolumes = 0;
	/*
	 * J2-full: per-disk v4 sequence numbers captured during the
	 * read loop; resolved into a majority-supported target seqno
	 * after the loop completes.  Indexed by voldata->volu_id.
	 */
	uint64_t disk_seqs[HAMMER2_MAX_VOLUMES];
	int disk_seen[HAMMER2_MAX_VOLUMES];

	bzero(disk_seqs, sizeof(disk_seqs));
	bzero(disk_seen, sizeof(disk_seen));

	for (i = 0; i < HAMMER2_MAX_VOLUMES; ++i) {
		vol = &volumes[i];
		vol->dev = NULL;
		vol->id = -1;
		vol->offset = (hammer2_off_t)-1;
		vol->size = (hammer2_off_t)-1;
	}

	voldata = kmalloc(sizeof(*voldata), M_HAMMER2, M_WAITOK | M_ZERO);
	bzero(&fsid, sizeof(fsid));
	bzero(&fstype, sizeof(fstype));
	bzero(rootvoldata, sizeof(*rootvoldata));

	/*
	 * Count total entries to detect multi-device configurations.
	 */
	{
		int nentries = 0;
		TAILQ_FOREACH(e, devvpl, entry)
			nentries++;
		nvolumes = nentries; /* reuse, reset below */
	}

	TAILQ_FOREACH(e, devvpl, entry) {
		devvp = e->devvp;
		path = e->path;

		/*
		 * Skip devices that failed lookup or open (degraded mount).
		 * These will be assigned to uninitialized volume slots below.
		 */
		if (devvp == NULL || !e->open) {
			hprintf("\"%s\" skipped (device unavailable)\n", path);
			continue;
		}

		/* returns negative error or positive zone# */
		error = hammer2_read_volume_header(devvp, path, voldata);
		if (error < 0) {
			/*
			 * For multi-device RAID, tolerate volume header
			 * read failures.  The device may exist (e.g.,
			 * deconfigured vn device) but have no data.
			 * Mark it as unavailable and continue.
			 */
			if (nvolumes > 1) {
				hprintf("%s: volume header unreadable, "
					"treating as failed disk\n", path);
				/*
				 * Close the device since we can't use it.
				 */
				vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
				VOP_CLOSE(devvp,
					  FREAD | FWRITE, NULL);
				vn_unlock(devvp);
				e->open = 0;
				error = 0;
				continue;
			}
			hprintf("failed to read %s's volume header\n", path);
			error = -error;
			goto done;
		}
		zone = error;
		error = 0; /* reset error */

		if (voldata->volu_id >= HAMMER2_MAX_VOLUMES) {
			hprintf("%s has bad volume id %d\n", path,
				voldata->volu_id);
			error = EINVAL;
			goto done;
		}
		vol = &volumes[voldata->volu_id];
		if (vol->id != -1) {
			hprintf("volume id %d already initialized\n",
				voldata->volu_id);
			error = EINVAL;
			goto done;
		}
		/* all headers must have the same version, nvolumes and uuid */
		if (version == -1) {
			version = voldata->version;
			nvolumes = voldata->nvolumes;
			fsid = voldata->fsid;
			fstype = voldata->fstype;
		} else {
			if (version != (int)voldata->version) {
				hprintf("volume version mismatch %d vs %d\n",
					version, (int)voldata->version);
				error = ENXIO;
				goto done;
			}
			if (nvolumes != voldata->nvolumes) {
				hprintf("volume count mismatch %d vs %d\n",
					nvolumes, voldata->nvolumes);
				error = ENXIO;
				goto done;
			}
			if (bcmp(&fsid, &voldata->fsid, sizeof(fsid))) {
				hammer2_print_uuid_mismatch(&fsid,
							    &voldata->fsid, "fsid");
				error = ENXIO;
				goto done;
			}
			if (bcmp(&fstype, &voldata->fstype, sizeof(fstype))) {
				hammer2_print_uuid_mismatch(&fstype,
							    &voldata->fstype, "fstype");
				error = ENXIO;
				goto done;
			}
		}
		if (version < HAMMER2_VOL_VERSION_MIN ||
		    version > HAMMER2_VOL_VERSION_WIP) {
			hprintf("bad volume version %d\n", version);
			error = EINVAL;
			goto done;
		}

		/*
		 * v4 RAIDZ2-native: every disk carries the same array UUID
		 * (mkfs writes voldata.fsid identically into every disk).
		 * Per-disk v4_array_uuid is generated at format time and
		 * must match what we already verified via voldata.fsid above.
		 * v4_disk_id must match volu_id; v4_ndisks must agree with
		 * raid_config.ndisks.
		 *
		 * Full majority-quorum + multi-TXG rollback (volhdr_quorum.md)
		 * is deferred; today we log per-disk v4_txg_seq and any
		 * mismatched UUIDs.  Mount-time selection of the
		 * highest-seqno root voldata is a follow-up.
		 */
		if (voldata->version >= HAMMER2_VOL_VERSION_RAIDZ2 &&
		    voldata->raid_config.raid_type ==
		     HAMMER2_RAID_TYPE_RAID6) {
			const hammer2_raid_config_t *vrc =
			    &voldata->raid_config;
			if (bcmp(vrc->v4_array_uuid, &voldata->fsid,
				 sizeof(vrc->v4_array_uuid)) != 0) {
				hprintf("%s: v4_array_uuid does not match "
					"voldata.fsid; refusing\n", path);
				error = ENXIO;
				goto done;
			}
			if (vrc->v4_disk_id != voldata->volu_id) {
				hprintf("%s: v4_disk_id %u != volu_id %u\n",
					path, vrc->v4_disk_id,
					voldata->volu_id);
				error = EINVAL;
				goto done;
			}
			if (vrc->v4_ndisks != vrc->ndisks) {
				hprintf("%s: v4_ndisks %u != ndisks %u\n",
					path, vrc->v4_ndisks, vrc->ndisks);
				error = EINVAL;
				goto done;
			}
			disk_seqs[voldata->volu_id] = vrc->v4_txg_seq;
			disk_seen[voldata->volu_id] = 1;
			hprintf("%s: v4 RAID6 disk %u/%u txg_seq %ju\n",
				path, vrc->v4_disk_id, vrc->v4_ndisks,
				(uintmax_t)vrc->v4_txg_seq);
		}

		/* all per-volume tests passed */
		vol->dev = e;
		vol->id = voldata->volu_id;
		vol->offset = voldata->volu_loff[vol->id];
		vol->size = voldata->volu_size;
		/*
		 * Select the disk whose voldata becomes rootvoldata.
		 *
		 * J2-full: under v4 RAID6, prefer whichever disk has the
		 * highest v4_txg_seq so the in-memory state reflects the
		 * most-recently committed TXG.  Otherwise (v1/v2/v3) keep
		 * the legacy "first ROOT_VOLUME wins, else first present
		 * disk" behavior.
		 */
		if (voldata->version >= HAMMER2_VOL_VERSION_RAIDZ2 &&
		    voldata->raid_config.raid_type ==
		     HAMMER2_RAID_TYPE_RAID6) {
			if (*rootvoldevvp == NULL ||
			    voldata->raid_config.v4_txg_seq >
			     rootvoldata->raid_config.v4_txg_seq) {
				bcopy(voldata, rootvoldata,
				      sizeof(*rootvoldata));
				*rootvolzone = zone;
				*rootvoldevvp = e->devvp;
			}
		} else if (vol->id == HAMMER2_ROOT_VOLUME) {
			bcopy(voldata, rootvoldata, sizeof(*rootvoldata));
			*rootvolzone = zone;
			KKASSERT(*rootvoldevvp == NULL);
			*rootvoldevvp = e->devvp;
		} else if (*rootvoldevvp == NULL) {
			/*
			 * Root volume disk is absent (degraded mount).
			 * Use this disk's header as rootvoldata since
			 * all v3 RAID6 disks carry the same metadata.
			 */
			bcopy(voldata, rootvoldata, sizeof(*rootvoldata));
			*rootvolzone = zone;
			*rootvoldevvp = e->devvp;
		}
		devvp->v_rdev->si_mountpoint = mp;
		hprintf("\"%s\" zone=%d id=%d offset=0x%016jx size=0x%016jx\n",
			path, zone, vol->id, (intmax_t)vol->offset,
			(intmax_t)vol->size);
	}

	/*
	 * J2-full: majority quorum on the v4 TXG seqno
	 * (docs/volhdr_quorum.md §Mount-time discovery).
	 *
	 * We require ⌈N/2⌉+1 disks to report the same seqno as the disk
	 * whose voldata we adopted as rootvoldata.  Disks below that
	 * seqno are tagged for resilver (logged here; caller marks them
	 * failed during the normal degraded-mount path).  Disks above
	 * (none in practice — rootvoldata already tracks the max) would
	 * indicate corruption.
	 *
	 * No-majority case rolls back to the highest seqno that does
	 * have majority (down to a hard limit of HAMMER2_J2_ROLLBACK_MAX
	 * TXGs); if even seqno 0 has no majority, ENXIO.
	 */
	if (!error &&
	    rootvoldata->version >= HAMMER2_VOL_VERSION_RAIDZ2 &&
	    rootvoldata->raid_config.raid_type ==
	     HAMMER2_RAID_TYPE_RAID6) {
		const hammer2_raid_config_t *rrc = &rootvoldata->raid_config;
		uint32_t ndisks = rrc->ndisks;
		uint32_t majority = (ndisks / 2) + 1;
		uint64_t target = rrc->v4_txg_seq;
		uint64_t fallback = target;
		uint32_t at_target;
		uint64_t lowest = (uint64_t)-1;
		uint32_t seen_count = 0;
		uint32_t rollback_max = (uint32_t)
		    (hammer2_j2_rollback_max > 0 ?
		     hammer2_j2_rollback_max : 8);
		int j;

		for (j = 0; j < HAMMER2_MAX_VOLUMES; j++) {
			if (!disk_seen[j])
				continue;
			seen_count++;
			if (disk_seqs[j] < lowest)
				lowest = disk_seqs[j];
		}

		for (;;) {
			at_target = 0;
			for (j = 0; j < HAMMER2_MAX_VOLUMES; j++) {
				if (disk_seen[j] && disk_seqs[j] == fallback)
					at_target++;
			}
			if (at_target >= majority)
				break;
			if (fallback == 0 ||
			    target - fallback >= rollback_max) {
				hprintf("v4 quorum: no majority within "
					"%u-TXG rollback limit "
					"(target seq %ju, lowest %ju, "
					"%u disks seen of %u)\n",
					rollback_max,
					(uintmax_t)target,
					(uintmax_t)lowest,
					seen_count, ndisks);
				error = ENXIO;
				goto done;
			}
			fallback--;
		}

		if (fallback != target) {
			if (!hammer2_j2_allow_rollback) {
				hprintf("v4 quorum: would roll back "
					"%ju -> %ju (%u/%u disks at "
					"fallback seq); refusing.  Set "
					"vfs.hammer2.j2_allow_rollback=1 to "
					"authorize and accept the loss of "
					"writes between those TXGs.\n",
					(uintmax_t)target,
					(uintmax_t)fallback,
					at_target, ndisks);
				error = ENXIO;
				goto done;
			}
			hprintf("v4 quorum: rolling back %ju -> %ju "
				"(%u/%u disks at fallback seq) "
				"per j2_allow_rollback=1\n",
				(uintmax_t)target, (uintmax_t)fallback,
				at_target, ndisks);
			/*
			 * Re-read voldata from a fallback-seqno disk so
			 * rootvoldata reflects the seqno we'll actually
			 * mount at.  The first matching disk wins; the
			 * extent table / disk_state / etc. all come from
			 * that disk's copy.
			 */
			TAILQ_FOREACH(e, devvpl, entry) {
				if (e->devvp == NULL || !e->open)
					continue;
				if (hammer2_read_volume_header(e->devvp,
				    e->path, voldata) < 0)
					continue;
				if (voldata->version <
				    HAMMER2_VOL_VERSION_RAIDZ2)
					continue;
				if (voldata->raid_config.v4_txg_seq ==
				    fallback) {
					bcopy(voldata, rootvoldata,
					      sizeof(*rootvoldata));
					*rootvoldevvp = e->devvp;
					break;
				}
			}
		}

		/*
		 * Log any disks that fell behind so the operator (or a
		 * future auto-resilver) can address them.
		 */
		for (j = 0; j < HAMMER2_MAX_VOLUMES; j++) {
			if (disk_seen[j] && disk_seqs[j] < target) {
				hprintf("v4 quorum: disk %d at seq %ju "
					"(target %ju) — needs resilver\n",
					j, (uintmax_t)disk_seqs[j],
					(uintmax_t)target);
			}
		}
	}

	/*
	 * For RAID6 degraded mounts: assign unavailable device entries
	 * to uninitialized volume slots so vol->dev is non-NULL for all
	 * volumes.  This allows code that accesses vol->dev->path or
	 * vol->dev->open to work without NULL checks everywhere.
	 */
	if (!error && rootvoldata->version >= HAMMER2_VOL_VERSION_RAID6) {
		hammer2_raid_config_t *rc = &rootvoldata->raid_config;
		int slot;

		e = TAILQ_FIRST(devvpl);
		for (slot = 0; slot < rc->ndisks && e != NULL; slot++) {
			vol = &volumes[slot];
			if (vol->id != -1)
				continue;
			/* Find next unavailable entry */
			while (e != NULL && e->devvp != NULL && e->open)
				e = TAILQ_NEXT(e, entry);
			if (e == NULL)
				break;
			vol->dev = e;
			vol->id = slot;
			/*
			 * Use volume geometry from rootvoldata since we
			 * can't read this disk's header.
			 */
			vol->offset = rootvoldata->volu_loff[slot];
			vol->size = rootvoldata->volu_size;
			hprintf("\"%s\" id=%d assigned as failed "
				"(degraded mount)\n", e->path, slot);
			e = TAILQ_NEXT(e, entry);
		}
	}
done:
	if (!error) {
		if (!rootvoldata->version) {
			hprintf("root volume not found\n");
			error = EINVAL;
		}
		if (!error)
			error = hammer2_verify_volumes(volumes, rootvoldata);
	}
	kfree(voldata, M_HAMMER2);

	return error;
}

hammer2_volume_t*
hammer2_get_volume(hammer2_dev_t *hmp, hammer2_off_t offset)
{
	hammer2_volume_t *vol, *ret = NULL;
	int i;

	offset &= ~HAMMER2_OFF_MASK_RADIX;

	/* locking is unneeded until volume-add support */
	//hammer2_voldata_lock(hmp);
	/* do binary search if users really use this many supported volumes */
	for (i = 0; i < hmp->nvolumes; ++i) {
		vol = &hmp->volumes[i];
		if ((offset >= vol->offset) &&
		    (offset < vol->offset + vol->size)) {
			ret = vol;
			break;
		}
	}
	//hammer2_voldata_unlock(hmp);

	if (!ret)
		panic("no volume for offset 0x%016jx", (intmax_t)offset);

	KKASSERT(ret);
	KKASSERT(ret->dev);
	KKASSERT(ret->dev->devvp);
	KKASSERT(ret->dev->path);

	return ret;
}

/*
 * H4-deep: blockref-walk reconstruction of the v4 stripe bitmap.
 *
 * Called from vfs_mount when hammer2_raid6_bitmap_read flagged
 * stripe_bitmap_invalid (torn write, missing/corrupt header, or
 * disk 0 absent).  Walks the on-disk chain tree from hmp->vchain,
 * sets a bitmap bit for every reachable DATA/DIRENT blockref's
 * stripe slot, and on success clears stripe_bitmap_invalid so the
 * mount can proceed RW.  The new bitmap is persisted at the next
 * TXG flush via the existing hammer2_raid6_bitmap_write path.
 */
static int
hammer2_v4_record_bref(hammer2_dev_t *hmp, const hammer2_blockref_t *bref)
{
	hammer2_off_t phys_off;
	uint64_t slot;
	int byte_idx, bit_idx;

	if (bref->type != HAMMER2_BREF_TYPE_DATA &&
	    bref->type != HAMMER2_BREF_TYPE_DIRENT)
		return 0;
	if ((bref->data_off & ~HAMMER2_OFF_MASK_RADIX) == 0)
		return 0;

	phys_off = bref->data_off & HAMMER2_RAID6_PHYS_MASK;
	if (phys_off < HAMMER2_ZONE_SEG64)
		return 0;
	slot = (phys_off - HAMMER2_ZONE_SEG64) /
	    hmp->raid_config.stripe_unit;
	if (slot >= hmp->stripe_num_slots)
		return 0;
	byte_idx = (int)(slot / 8);
	bit_idx  = (int)(slot % 8);
	if (byte_idx >= (int)hmp->stripe_bitmap_size)
		return 0;

	hammer2_spin_ex(&hmp->stripe_bitmap_spin);
	hmp->stripe_bitmap[byte_idx] |= (uint8_t)(1 << bit_idx);
	if (slot >= hmp->stripe_cursor)
		hmp->stripe_cursor = slot + 1;
	hammer2_spin_unex(&hmp->stripe_bitmap_spin);
	return 0;
}

static int
hammer2_v4_walk_chain(hammer2_dev_t *hmp, hammer2_chain_t *parent)
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

		(void)hammer2_v4_record_bref(hmp, &bref);

		if (chain) {
			error = hammer2_v4_walk_chain(hmp, chain);
			if (error)
				break;
		}
	}
	if (chain) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
	}
	return error;
}

int
hammer2_v4_rebuild_stripe_bitmap(hammer2_dev_t *hmp)
{
	int error;

	KKASSERT(hmp->raid_type == HAMMER2_RAID_TYPE_RAID6);
	KKASSERT(hmp->voldata.version >= HAMMER2_VOL_VERSION_RAIDZ2);
	KKASSERT(hmp->stripe_bitmap != NULL);

	bzero(hmp->stripe_bitmap, hmp->stripe_bitmap_size);
	hmp->stripe_cursor = HAMMER2_STRIPE_V4_START;

	hammer2_chain_lock(&hmp->vchain, HAMMER2_RESOLVE_ALWAYS);
	error = hammer2_v4_walk_chain(hmp, &hmp->vchain);
	hammer2_chain_unlock(&hmp->vchain);
	return error;
}

/*
 * Stripe bitmap on-disk layout (see docs/stripe_bitmap.md):
 *
 *   page 0:                 header
 *   pages 1..bitmap_pages:  bitmap data
 *   final page:             footer
 *
 * The whole structure fits within one HAMMER2_PBUFSIZE (64 KB) block on
 * disk 0 at zone HAMMER2_ZONE_RAID6_BITMAP, since num_slots * 1 bit / 8
 * stays well under 56 KB for any practical zone size.
 */
#define HAMMER2_STRIPE_BITMAP_PAGES_MAX	\
	((HAMMER2_PBUFSIZE - 2 * HAMMER2_STRIPE_BITMAP_PAGE) / \
	 HAMMER2_STRIPE_BITMAP_PAGE)

static uint32_t
hammer2_stripe_bitmap_crc(const hammer2_stripe_bitmap_header_t *hdr,
			  const uint8_t *bitmap, size_t bitmap_pages,
			  const hammer2_stripe_bitmap_footer_t *ftr)
{
	static const uint8_t zero_crc[sizeof(hdr->crc)] = { 0 };
	const size_t hdr_crc_off = offsetof(hammer2_stripe_bitmap_header_t, crc);
	const size_t ftr_crc_off = offsetof(hammer2_stripe_bitmap_footer_t, crc);
	uint32_t c;

	/*
	 * Compute the CRC over header + bitmap + footer with the crc[]
	 * field zeroed in each.  Earlier code did this by copying the
	 * 4 KB header and footer onto the stack, zeroing the crc field
	 * in the copies, and feeding them whole — that blew the kernel
	 * stack guard page on the first mount (DragonFly per-thread
	 * stack is ~16 KB; two 4 KB local structs plus caller frames =
	 * double fault).  Feed the live buffers in three pieces each,
	 * substituting a 16-byte zero buffer where crc[] lives, with no
	 * stack copy.
	 */
	c = hammer2_icrc32(hdr, hdr_crc_off);
	c = hammer2_icrc32c(zero_crc, sizeof(zero_crc), c);
	c = hammer2_icrc32c((const uint8_t *)hdr + hdr_crc_off + sizeof(hdr->crc),
			    sizeof(*hdr) - hdr_crc_off - sizeof(hdr->crc), c);
	c = hammer2_icrc32c(bitmap, bitmap_pages * HAMMER2_STRIPE_BITMAP_PAGE,
			    c);
	c = hammer2_icrc32c(ftr, ftr_crc_off, c);
	c = hammer2_icrc32c(zero_crc, sizeof(zero_crc), c);
	c = hammer2_icrc32c((const uint8_t *)ftr + ftr_crc_off + sizeof(ftr->crc),
			    sizeof(*ftr) - ftr_crc_off - sizeof(ftr->crc), c);
	return c;
}

/*
 * Allocate the in-memory stripe bitmap for a RAID6 array.
 * Sizing assumes one zone slot per disk and the legacy
 * slot_origin = HAMMER2_ZONE_SEG64.  Bits start zero (all slots free);
 * hammer2_raid6_bitmap_read() overwrites them with the persisted copy.
 *
 * Called during mount after hmp->raid_config is populated.
 */
void
hammer2_raid6_bitmap_init(hammer2_dev_t *hmp)
{
	uint64_t stripe_unit = hmp->raid_config.stripe_unit;
	uint64_t usable = HAMMER2_ZONE_BYTES64 - HAMMER2_ZONE_SEG64;
	uint64_t max_stripes = usable / stripe_unit;
	size_t bitmap_pages = (size_t)(((max_stripes + 7) / 8 +
	    HAMMER2_STRIPE_BITMAP_PAGE - 1) / HAMMER2_STRIPE_BITMAP_PAGE);
	size_t bsize;

	if (bitmap_pages == 0)
		bitmap_pages = 1;
	KKASSERT(bitmap_pages <= HAMMER2_STRIPE_BITMAP_PAGES_MAX);
	bsize = bitmap_pages * HAMMER2_STRIPE_BITMAP_PAGE;

	hmp->stripe_bitmap = kmalloc(bsize, M_HAMMER2, M_WAITOK | M_ZERO);
	hmp->stripe_bitmap_size = bsize;
	hmp->stripe_num_slots = max_stripes;
	hmp->stripe_cursor = HAMMER2_STRIPE_V4_START;
	hmp->stripe_generation = 0;
	hmp->stripe_bitmap_invalid = 0;
	spin_init(&hmp->stripe_bitmap_spin, "h2smap");
	hmp->stripe_next_disk = 0;
}

/*
 * Read and verify the on-disk stripe bitmap from zone slot
 * HAMMER2_ZONE_RAID6_BITMAP on disk 0.  On any header/footer/CRC
 * mismatch (or read failure), mark the bitmap invalid; the caller
 * keeps the in-memory zero bitmap and logs a warning.  A blockref-walk
 * reconstruction (newplan.md §5.9, stripe_bitmap.md "mount-time
 * verify") is a Phase 2 TODO; until it lands a torn bitmap risks
 * re-allocating live slots, so the mount path treats invalid as
 * fatal for RW mounts.
 */
void
hammer2_raid6_bitmap_read(hammer2_dev_t *hmp)
{
	struct vnode *devvp;
	struct buf *bp;
	off_t pbase;
	hammer2_stripe_bitmap_header_t *hdr;
	hammer2_stripe_bitmap_footer_t *ftr;
	uint8_t *bitmap;
	size_t bitmap_pages = hmp->stripe_bitmap_size /
	    HAMMER2_STRIPE_BITMAP_PAGE;
	uint32_t hdr_crc, ftr_crc, want_crc;
	int error;

	devvp = hmp->volumes[0].dev->devvp;
	if (devvp == NULL || !hmp->volumes[0].dev->open) {
		hmp->stripe_bitmap_invalid = 1;
		return;
	}

	pbase = (off_t)HAMMER2_ZONE_RAID6_BITMAP * HAMMER2_ZONE_SEG;

	error = bread(devvp, pbase, HAMMER2_PBUFSIZE, &bp);
	if (error || bp == NULL) {
		if (bp)
			brelse(bp);
		kprintf("hammer2: stripe bitmap read I/O error %d; "
			"bitmap invalid\n", error);
		hmp->stripe_bitmap_invalid = 1;
		return;
	}

	hdr = (hammer2_stripe_bitmap_header_t *)bp->b_data;
	bitmap = (uint8_t *)bp->b_data + HAMMER2_STRIPE_BITMAP_PAGE;
	ftr = (hammer2_stripe_bitmap_footer_t *)((uint8_t *)bp->b_data +
	    HAMMER2_STRIPE_BITMAP_PAGE +
	    bitmap_pages * HAMMER2_STRIPE_BITMAP_PAGE);

	if (hdr->magic != HAMMER2_STRIPE_BITMAP_MAGIC ||
	    hdr->version != HAMMER2_STRIPE_BITMAP_VERSION ||
	    hdr->ndisks != hmp->raid_config.ndisks ||
	    hdr->stripe_unit != hmp->raid_config.stripe_unit ||
	    ftr->magic_end != HAMMER2_STRIPE_BITMAP_MAGIC_END ||
	    ftr->generation != hdr->generation) {
		kprintf("hammer2: stripe bitmap header/footer mismatch; "
			"bitmap invalid\n");
		hmp->stripe_bitmap_invalid = 1;
		brelse(bp);
		return;
	}

	bcopy(hdr->crc, &hdr_crc, sizeof(hdr_crc));
	bcopy(ftr->crc, &ftr_crc, sizeof(ftr_crc));
	want_crc = hammer2_stripe_bitmap_crc(hdr, bitmap, bitmap_pages, ftr);
	if (hdr_crc != ftr_crc || hdr_crc != want_crc) {
		kprintf("hammer2: stripe bitmap CRC mismatch "
			"(hdr %08x ftr %08x want %08x); bitmap invalid\n",
			hdr_crc, ftr_crc, want_crc);
		hmp->stripe_bitmap_invalid = 1;
		brelse(bp);
		return;
	}

	bcopy(bitmap, hmp->stripe_bitmap, hmp->stripe_bitmap_size);
	hmp->stripe_cursor = hdr->cursor;
	hmp->stripe_generation = hdr->generation;
	brelse(bp);
}

/*
 * Write the in-memory stripe bitmap to disk zone slot
 * HAMMER2_ZONE_RAID6_BITMAP on disk 0, synchronously.
 * Bumps the generation, computes a fresh CRC, and writes
 * header + bitmap + footer as one buffer.  Called from the TXG commit
 * path before the volume header write so the bitmap is durable before
 * the header that references it.
 */
void
hammer2_raid6_bitmap_write(hammer2_dev_t *hmp)
{
	struct vnode *devvp;
	struct buf *bp;
	off_t pbase;
	hammer2_stripe_bitmap_header_t *hdr;
	hammer2_stripe_bitmap_footer_t *ftr;
	uint8_t *bitmap;
	size_t bitmap_pages = hmp->stripe_bitmap_size /
	    HAMMER2_STRIPE_BITMAP_PAGE;
	uint32_t crc;

	devvp = hmp->volumes[0].dev->devvp;
	if (devvp == NULL || !hmp->volumes[0].dev->open)
		return;

	pbase = (off_t)HAMMER2_ZONE_RAID6_BITMAP * HAMMER2_ZONE_SEG;

	bp = getblk(devvp, pbase, HAMMER2_PBUFSIZE, GETBLK_KVABIO, 0);
	if (bp == NULL) {
		kprintf("hammer2_raid6_bitmap_write: getblk failed\n");
		return;
	}
	bkvasync(bp);
	bzero(bp->b_data, HAMMER2_PBUFSIZE);

	hdr = (hammer2_stripe_bitmap_header_t *)bp->b_data;
	bitmap = (uint8_t *)bp->b_data + HAMMER2_STRIPE_BITMAP_PAGE;
	ftr = (hammer2_stripe_bitmap_footer_t *)((uint8_t *)bp->b_data +
	    HAMMER2_STRIPE_BITMAP_PAGE +
	    bitmap_pages * HAMMER2_STRIPE_BITMAP_PAGE);

	hmp->stripe_generation++;
	hdr->magic = HAMMER2_STRIPE_BITMAP_MAGIC;
	hdr->version = HAMMER2_STRIPE_BITMAP_VERSION;
	hdr->ndisks = hmp->raid_config.ndisks;
	hdr->stripe_unit = hmp->raid_config.stripe_unit;
	hdr->num_slots = hmp->stripe_num_slots;
	hdr->slot_origin = HAMMER2_ZONE_SEG64;
	hdr->cursor = hmp->stripe_cursor;
	hdr->generation = hmp->stripe_generation;

	ftr->magic_end = HAMMER2_STRIPE_BITMAP_MAGIC_END;
	ftr->generation = hmp->stripe_generation;

	bcopy(hmp->stripe_bitmap, bitmap, hmp->stripe_bitmap_size);

	crc = hammer2_stripe_bitmap_crc(hdr, bitmap, bitmap_pages, ftr);
	bcopy(&crc, hdr->crc, sizeof(crc));
	bcopy(&crc, ftr->crc, sizeof(crc));

	bwrite(bp);
}

/*
 * Allocate a physical stripe slot for a RAIDZ2-native (v4) data column.
 * Sets chain->bref.data_off to the physical column offset on the chosen disk
 * and chain->bref.copyid to the disk index (0..ndisks-1).
 *
 * Returns 0 on success, ENOSPC if no free stripe slot is available.
 *
 * The caller holds the chain in the appropriate state for modification.
 * The stripe bitmap spinlock protects concurrent alloc/free.
 */
int
hammer2_raid6_stripe_alloc(hammer2_dev_t *hmp, hammer2_chain_t *chain)
{
	hammer2_raid_config_t *rc = &hmp->raid_config;
	uint64_t stripe_unit = rc->stripe_unit;
	uint64_t max_stripes = hmp->stripe_num_slots;
	uint64_t slot;
	uint64_t start;
	int wrapped = 0;
	int byte_idx, bit_idx;
	int disk_idx;
	hammer2_off_t phys_off;
	int radix;
	int n;

	/*
	 * Sequential-cursor allocator (docs/stripe_bitmap.md §Allocator):
	 * start from hmp->stripe_cursor, scan forward for the first 0 bit,
	 * wrap once on miss.  Cursor never moves backwards; on free, slots
	 * become reachable again only after the cursor wraps.
	 *
	 * Reserved-zone slots (freemap rotations at zones 21/26/31/36 and
	 * the bitmap zone 41) are skipped in line.
	 */
	hammer2_spin_ex(&hmp->stripe_bitmap_spin);

	start = hmp->stripe_cursor;
	if (start < HAMMER2_STRIPE_V4_START || start >= max_stripes)
		start = HAMMER2_STRIPE_V4_START;
	slot = start;

	for (;;) {
		uint64_t zone;
		hammer2_off_t cand_off;
		uint32_t ex;
		int in_md_zone = 0;

		if (slot >= max_stripes) {
			if (wrapped)
				break;
			wrapped = 1;
			slot = HAMMER2_STRIPE_V4_START;
			if (slot >= start)
				break;
			continue;
		}
		if (wrapped && slot >= start)
			break;

		cand_off = HAMMER2_ZONE_SEG64 + slot * stripe_unit;
		zone = cand_off / HAMMER2_ZONE_SEG64;
		if (zone >= HAMMER2_ZONE_FREEMAP_00 &&
		    zone <= HAMMER2_ZONE_FREEMAP_07 &&
		    (zone % HAMMER2_ZONE_FREEMAP_INC) ==
		     HAMMER2_ZONE_FREEMAP_00) {
			slot++;
			continue;
		}
		if (zone == HAMMER2_ZONE_RAID6_BITMAP) {
			slot++;
			continue;
		}
		/*
		 * Skip slots whose per-disk physical offset falls inside any
		 * metadata-zone extent (metadata_zone.md).  Without this,
		 * stripe data would overlap the mirrored metadata area.
		 */
		for (ex = 0; ex < hmp->md_nextents; ex++) {
			hammer2_off_t mo = hmp->md_extents[ex].md_off;
			hammer2_off_t ms = hmp->md_extents[ex].md_size;
			if (cand_off + stripe_unit > mo &&
			    cand_off < mo + ms) {
				in_md_zone = 1;
				break;
			}
		}
		if (in_md_zone) {
			slot++;
			continue;
		}

		byte_idx = (int)(slot / 8);
		bit_idx  = (int)(slot % 8);
		if (byte_idx >= (int)hmp->stripe_bitmap_size)
			break;
		if ((hmp->stripe_bitmap[byte_idx] & (1 << bit_idx)) == 0) {
			hmp->stripe_bitmap[byte_idx] |= (uint8_t)(1 << bit_idx);
			hmp->stripe_cursor = slot + 1;
			goto found;
		}
		slot++;
	}
	hammer2_spin_unex(&hmp->stripe_bitmap_spin);
	return ENOSPC;

found:
	/*
	 * Choose data disk (round-robin, skipping the P and Q disks for
	 * this stripe).  P = slot % ndisks, Q = (slot + 1) % ndisks.
	 */
	{
		int ndisks = rc->ndisks;
		int p_disk = (int)(slot % ndisks);
		int q_disk = (p_disk + 1) % ndisks;
		int candidate;
		int tries;

		/* Pick a data disk round-robin, skipping P and Q */
		for (tries = 0; tries < ndisks; tries++) {
			candidate = hmp->stripe_next_disk % ndisks;
			hmp->stripe_next_disk++;
			if (candidate != p_disk && candidate != q_disk) {
				disk_idx = candidate;
				goto have_disk;
			}
		}
		/* All disks are P or Q — can't happen for ndisks >= 4 */
		hmp->stripe_bitmap[byte_idx] &= ~(uint8_t)(1 << bit_idx);
		hammer2_spin_unex(&hmp->stripe_bitmap_spin);
		return ENOSPC;
	}
have_disk:
	hammer2_spin_unex(&hmp->stripe_bitmap_spin);
	phys_off = HAMMER2_ZONE_SEG64 + slot * stripe_unit;

	/* Compute radix (log2 of chain->bytes) */
	radix = 0;
	n = chain->bytes;
	while (n > 1) { n >>= 1; radix++; }

	/*
	 * Encode (disk_idx, phys_off) into bref.data_off so the DIO tree
	 * key is unique by construction across all disks.  Layout is
	 * documented at HAMMER2_RAID6_DISK_SHIFT in hammer2_disk.h.
	 */
	chain->bref.data_off = ((hammer2_off_t)disk_idx <<
	    HAMMER2_RAID6_DISK_SHIFT) | phys_off | (hammer2_off_t)radix;
	chain->bref.copyid   = (uint8_t)disk_idx;

	return 0;
}

/*
 * Free a physical stripe slot for a RAIDZ2-native (v4) data column.
 * Clears the stripe bitmap bit for the slot encoded in bref->data_off.
 */
void
hammer2_raid6_stripe_free(hammer2_dev_t *hmp, const hammer2_blockref_t *bref)
{
	hammer2_raid_config_t *rc = &hmp->raid_config;
	uint64_t stripe_unit = rc->stripe_unit;
	hammer2_off_t phys_off;
	uint64_t slot;
	int byte_idx, bit_idx;

	/*
	 * Decode per-disk physical offset from bref->data_off (top byte
	 * holds disk_idx; see HAMMER2_RAID6_DISK_SHIFT in hammer2_disk.h).
	 */
	phys_off = bref->data_off & HAMMER2_RAID6_PHYS_MASK;
	if (phys_off < HAMMER2_ZONE_SEG64)
		return; /* metadata block, not a stripe slot */

	slot = (phys_off - HAMMER2_ZONE_SEG64) / stripe_unit;
	byte_idx = (int)(slot / 8);
	bit_idx  = (int)(slot % 8);

	if (byte_idx >= (int)hmp->stripe_bitmap_size)
		return; /* out of range */

	hammer2_spin_ex(&hmp->stripe_bitmap_spin);
	hmp->stripe_bitmap[byte_idx] &= ~(uint8_t)(1 << bit_idx);
	hammer2_spin_unex(&hmp->stripe_bitmap_spin);
}
