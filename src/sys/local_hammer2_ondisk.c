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

#include <vm/vm_object.h>

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

/*
 * Append a placeholder entry for an absent device of a multi-device
 * mount.  It gets a dummy vnode so the buffer cache has a valid anchor
 * for degraded I/O paths (getblk/brelse); dead vnode ops make any
 * accidental I/O return an error.
 */
static hammer2_devvp_t *
hammer2_add_absent_devvp(hammer2_devvp_list_t *devvpl, const char *path)
{
	hammer2_devvp_t *e;
	struct vnode *dummy_vp;

	if (getspecialvnode(VT_NON, NULL, &dead_vnode_vops_p, &dummy_vp,
			    0, 0)) {
		hprintf("cannot allocate dummy vnode for %s\n", path);
		return NULL;
	}
	dummy_vp->v_type = VCHR;
	/*
	 * getblk() needs a VM object, which a real device vnode gets at
	 * open.  Buffers here are only reconstruction scratch and are
	 * never dirtied (putblk brelse's them).
	 */
	vinitvmio(dummy_vp, IDX_TO_OFF(INT_MAX), PAGE_SIZE, -1);
	vx_unlock(dummy_vp);
	e = kmalloc(sizeof(*e), M_HAMMER2, M_WAITOK | M_ZERO);
	e->devvp = dummy_vp;
	e->path = kstrdup(path, M_HAMMER2);
	e->open = 0;
	TAILQ_INSERT_TAIL(devvpl, e, entry);

	return e;
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
				hprintf("device %s not found (%d), "
					"will attempt degraded mount\n",
					path, error);
				if (hammer2_add_absent_devvp(devvpl, path) == NULL)
					break;
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
hammer2_verify_volumes_common(const hammer2_volume_t *volumes,
			      const hammer2_volume_data_t *rootvoldata)
{
	const hammer2_volume_t *vol;
	struct partinfo part;
	const char *path;
	char buf[64];
	int i;
	uuid_t uuid;

	/*
	 * check volume header.  A v3 RAID6 array adopts the header with
	 * the newest rz_txg_seq, which belongs to whichever disk that is
	 * (disk 0 may be failed or absent), so it has no root volume id.
	 */
	if (rootvoldata->volu_id != HAMMER2_ROOT_VOLUME &&
	    !(rootvoldata->version >= HAMMER2_VOL_VERSION_RAIDZ2 &&
	      rootvoldata->raid_config.raid_type == HAMMER2_RAID_TYPE_RAID6)) {
		hprintf("volume id %d must be %d\n", rootvoldata->volu_id,
			HAMMER2_ROOT_VOLUME);
		return EINVAL;
	}
	uuid = rootvoldata->fstype;
	snprintf_uuid(buf, sizeof(buf), &uuid); /* takes non-const uuid */
	if (strcmp(buf, HAMMER2_UUID_STRING)) {
		hprintf("volume fstype uuid %s must be %s\n", buf,
			HAMMER2_UUID_STRING);
		return EINVAL;
	}

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
		if (vol->size == 0) {
			hprintf("%s has size of 0\n", path);
			return EINVAL;
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
	vol = &volumes[HAMMER2_ROOT_VOLUME];
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
	if (rootvoldata->volu_id >= rc->ndisks) {
		hprintf("root volume id %d exceeds ndisks %d\n",
			rootvoldata->volu_id, rc->ndisks);
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

	error = hammer2_verify_volumes_common(volumes, rootvoldata);
	if (error)
		return error;

	if (rootvoldata->version >= HAMMER2_VOL_VERSION_RAIDZ2)
		return hammer2_verify_volumes_3(volumes, rootvoldata);
	else if (rootvoldata->version >= HAMMER2_VOL_VERSION_MULTI_VOLUMES)
		return hammer2_verify_volumes_2(volumes, rootvoldata);
	else
		return hammer2_verify_volumes_1(volumes, rootvoldata);
}

/*
 * Per-zone summary of one disk's volume headers, kept so the v3 quorum
 * can count a disk for every TXG seq it holds, not just its newest.
 */
typedef struct hammer2_volzone {
	int		valid;
	int		volu_id;
	uint64_t	seq;		/* rz_txg_seq */
	uint64_t	mirror_tid;
	hammer2_off_t	volu_size;
	int		raid6;		/* v3 RAID6 header */
	uuid_t		fsid;
} hammer2_volzone_t;

/*
 * Read and verify volume header copy 'i' of a device into *voldata.
 * Returns 0 if it is valid, else -1.
 */
static int
hammer2_read_volume_zone(struct vnode *devvp, const char *path, int i,
			 hammer2_volume_data_t *voldata)
{
	hammer2_volume_data_t *vd;
	struct buf *bp = NULL;
	hammer2_crc32_t crc0, crc1;

	if (bread(devvp, i * HAMMER2_ZONE_BYTES64, HAMMER2_VOLUME_BYTES,
		  &bp)) {
		brelse(bp);
		return -1;
	}

	vd = (struct hammer2_volume_data *)bp->b_data;
	/* verify volume header magic */
	if ((vd->magic != HAMMER2_VOLUME_ID_HBO) &&
	    (vd->magic != HAMMER2_VOLUME_ID_ABO)) {
		hprintf("%s #%d: bad magic\n", path, i);
		goto bad;
	}

	if (vd->magic == HAMMER2_VOLUME_ID_ABO) {
		/* XXX: Reversed-endianness filesystem */
		hprintf("%s #%d: reverse-endian filesystem detected\n",
			path, i);
		goto bad;
	}

	/* verify volume header CRC's */
	crc0 = vd->icrc_sects[HAMMER2_VOL_ICRC_SECT0];
	crc1 = hammer2_icrc32(bp->b_data + HAMMER2_VOLUME_ICRC0_OFF,
			      HAMMER2_VOLUME_ICRC0_SIZE);
	if (crc0 != crc1) {
		hprintf("%s #%d: volume header crc mismatch sect0 %08x/%08x\n",
			path, i, crc0, crc1);
		goto bad;
	}
	crc0 = vd->icrc_sects[HAMMER2_VOL_ICRC_SECT1];
	crc1 = hammer2_icrc32(bp->b_data + HAMMER2_VOLUME_ICRC1_OFF,
			      HAMMER2_VOLUME_ICRC1_SIZE);
	if (crc0 != crc1) {
		hprintf("%s #%d: volume header crc mismatch sect1 %08x/%08x\n",
			path, i, crc0, crc1);
		goto bad;
	}
	crc0 = vd->icrc_volheader;
	crc1 = hammer2_icrc32(bp->b_data + HAMMER2_VOLUME_ICRCVH_OFF,
			      HAMMER2_VOLUME_ICRCVH_SIZE);
	if (crc0 != crc1) {
		hprintf("%s #%d: volume header crc mismatch vh %08x/%08x\n",
			path, i, crc0, crc1);
		goto bad;
	}

	*voldata = *vd;
	brelse(bp);
	return 0;
bad:
	brelse(bp);
	return -1;
}

/*
 * Returns zone# of returned volume header or < 0 on failure.
 *
 * There are up to 4 copies of the volume header (syncs iterate between
 * them so there is no single master).  The newest valid copy is
 * returned: by rz_txg_seq on a v3 RAID6 volume (mirror_tid also moves
 * with writes that do not commit a header), else by mirror_tid.  If
 * zones is not NULL it receives a summary of every copy.
 */
static int
hammer2_read_volume_header(struct vnode *devvp, const char *path,
			   hammer2_volume_data_t *voldata,
			   hammer2_volzone_t *zones)
{
	hammer2_volume_data_t *vd;
	hammer2_volzone_t zs[HAMMER2_NUM_VOLHDRS];
	int zone = -1;
	int ref = -1;
	int better;
	int i, j, n;

	/*
	 * Pass 1: read and summarize every copy.
	 */
	bzero(zs, sizeof(zs));
	vd = kmalloc(sizeof(*vd), M_HAMMER2, M_WAITOK);
	for (i = 0; i < HAMMER2_NUM_VOLHDRS; ++i) {
		if (hammer2_read_volume_zone(devvp, path, i, vd) < 0)
			continue;
		zs[i].valid = 1;
		zs[i].volu_id = vd->volu_id;
		zs[i].volu_size = vd->volu_size;
		zs[i].seq = vd->raid_config.rz_txg_seq;
		zs[i].mirror_tid = vd->mirror_tid;
		zs[i].fsid = vd->fsid;
		zs[i].raid6 = (vd->version >= HAMMER2_VOL_VERSION_RAIDZ2 &&
			       vd->raid_config.raid_type ==
				HAMMER2_RAID_TYPE_RAID6);
	}
	kfree(vd, M_HAMMER2);

	/*
	 * All header copies of a device must belong to one filesystem.
	 * A stale copy from an earlier filesystem on the same device (a
	 * smaller newfs, or a disk reused from a larger one) carries a
	 * different fsid and may even have a newer sequence.  newfs always
	 * writes zone 0, so its fsid is the reference.  If zone 0 is not
	 * valid, take the fsid most copies agree on, the one holding the
	 * lowest zone on a tie.
	 */
	if (zs[0].valid) {
		ref = 0;
	} else {
		int best = 0;

		for (i = 1; i < HAMMER2_NUM_VOLHDRS; ++i) {
			if (!zs[i].valid)
				continue;
			n = 0;
			for (j = 1; j < HAMMER2_NUM_VOLHDRS; ++j) {
				if (zs[j].valid &&
				    bcmp(&zs[j].fsid, &zs[i].fsid,
					 sizeof(zs[i].fsid)) == 0)
					++n;
			}
			if (n > best) {
				best = n;
				ref = i;
			}
		}
	}
	for (i = 0; i < HAMMER2_NUM_VOLHDRS; ++i) {
		if (!zs[i].valid || i == ref)
			continue;
		if (bcmp(&zs[i].fsid, &zs[ref].fsid, sizeof(zs[i].fsid)) ||
		    zs[i].volu_id != zs[ref].volu_id) {
			hprintf("%s #%d: header belongs to another "
				"filesystem (fsid/volume id differs from "
				"#%d); ignored\n", path, i, ref);
			zs[i].valid = 0;
		}
	}

	/*
	 * Pass 2: pick the newest remaining copy.
	 */
	for (i = 0; i < HAMMER2_NUM_VOLHDRS; ++i) {
		if (!zs[i].valid)
			continue;
		if (zone == -1) {
			better = 1;
		} else if (zs[zone].raid6 && zs[i].raid6) {
			better = zs[zone].seq < zs[i].seq;
		} else {
			better = zs[zone].mirror_tid < zs[i].mirror_tid;
		}
		if (better)
			zone = i;
	}

	/*
	 * A copy beyond the end of the volume the newest header describes
	 * cannot have been written by that filesystem.
	 */
	if (zone != -1) {
		for (i = 0; i < HAMMER2_NUM_VOLHDRS; ++i) {
			if (!zs[i].valid || i == zone)
				continue;
			if ((hammer2_off_t)i * HAMMER2_ZONE_BYTES64 >=
			    zs[zone].volu_size) {
				hprintf("%s #%d: header lies beyond volume "
					"size 0x%jx; ignored\n", path, i,
					(uintmax_t)zs[zone].volu_size);
				zs[i].valid = 0;
			}
		}
	}

	if (zones)
		bcopy(zs, zones, sizeof(zs));
	if (zone == -1) {
		hprintf("%s has no valid volume headers\n", path);
		return -EINVAL;
	}

	vd = kmalloc(sizeof(*vd), M_HAMMER2, M_WAITOK);
	if (hammer2_read_volume_zone(devvp, path, zone, vd) < 0) {
		kfree(vd, M_HAMMER2);
		hprintf("%s #%d: header unreadable on second read\n",
			path, zone);
		return -EINVAL;
	}
	*voldata = *vd;
	kfree(vd, M_HAMMER2);
	return zone;
}

/*
 * Find a valid header zone of volume 'id' carrying v3 seq 'seq' and the
 * array's fsid.  Returns the zone number or -1.
 */
static int
hammer2_volzone_find(const hammer2_volzone_t *zones, int id,
		     const uuid_t *fsid, uint64_t seq)
{
	int i;

	for (i = 0; i < HAMMER2_NUM_VOLHDRS; ++i) {
		if (zones[i].valid && zones[i].volu_id == id &&
		    zones[i].seq == seq &&
		    bcmp(&zones[i].fsid, fsid, sizeof(*fsid)) == 0)
			return i;
	}
	return -1;
}

static void
hammer2_print_uuid_mismatch(uuid_t *uuid1, uuid_t *uuid2, const char *id)
{
	char buf1[64], buf2[64];

	snprintf_uuid(buf1, sizeof(buf1), uuid1);
	snprintf_uuid(buf2, sizeof(buf2), uuid2);

	hprintf("volume %s uuid mismatch %s vs %s\n", id, buf1, buf2);
}

int
hammer2_init_volumes(struct mount *mp, hammer2_devvp_list_t *devvpl,
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
	 * J2-full: per-disk v3 sequence numbers captured during the
	 * read loop; resolved into a majority-supported target seqno
	 * after the loop completes.  Indexed by voldata->volu_id.
	 */
	uint64_t disk_seqs[HAMMER2_MAX_VOLUMES];
	int disk_seen[HAMMER2_MAX_VOLUMES];
	hammer2_volzone_t zi[HAMMER2_NUM_VOLHDRS];
	hammer2_volzone_t (*zones)[HAMMER2_NUM_VOLHDRS];

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
	zones = kmalloc(sizeof(*zones) * HAMMER2_MAX_VOLUMES, M_HAMMER2,
			M_WAITOK | M_ZERO);
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
		bzero(zi, sizeof(zi));
		error = hammer2_read_volume_header(devvp, path, voldata, zi);
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
		 * v3 RAIDZ2-native: every disk carries the same array UUID
		 * (mkfs writes voldata.fsid identically into every disk).
		 * Per-disk rz_array_uuid is generated at format time and
		 * must match what we already verified via voldata.fsid above.
		 * rz_disk_id must match volu_id; rz_ndisks must agree with
		 * raid_config.ndisks.
		 *
		 * Full majority-quorum + multi-TXG rollback (volhdr_quorum.md)
		 * is deferred; today we log per-disk rz_txg_seq and any
		 * mismatched UUIDs.  Mount-time selection of the
		 * highest-seqno root voldata is a follow-up.
		 */
		if (voldata->version >= HAMMER2_VOL_VERSION_RAIDZ2 &&
		    voldata->raid_config.raid_type ==
		     HAMMER2_RAID_TYPE_RAID6) {
			const hammer2_raid_config_t *vrc =
			    &voldata->raid_config;
			if (bcmp(vrc->rz_array_uuid, &voldata->fsid,
				 sizeof(vrc->rz_array_uuid)) != 0) {
				hprintf("%s: rz_array_uuid does not match "
					"voldata.fsid; refusing\n", path);
				error = ENXIO;
				goto done;
			}
			if (vrc->rz_disk_id != voldata->volu_id) {
				hprintf("%s: rz_disk_id %u != volu_id %u\n",
					path, vrc->rz_disk_id,
					voldata->volu_id);
				error = EINVAL;
				goto done;
			}
			if (vrc->rz_ndisks != vrc->ndisks) {
				hprintf("%s: rz_ndisks %u != ndisks %u\n",
					path, vrc->rz_ndisks, vrc->ndisks);
				error = EINVAL;
				goto done;
			}
			disk_seqs[voldata->volu_id] = vrc->rz_txg_seq;
			disk_seen[voldata->volu_id] = 1;
			bcopy(zi, zones[voldata->volu_id], sizeof(zi));
			hprintf("%s: v3 RAID6 disk %u/%u txg_seq %ju\n",
				path, vrc->rz_disk_id, vrc->rz_ndisks,
				(uintmax_t)vrc->rz_txg_seq);
		}

		/* all per-volume tests passed */
		vol->dev = e;
		vol->id = voldata->volu_id;
		vol->offset = voldata->volu_loff[vol->id];
		vol->size = voldata->volu_size;
		/*
		 * Select the disk whose voldata becomes rootvoldata.
		 *
		 * J2-full: under v3 RAID6, prefer whichever disk has the
		 * highest rz_txg_seq so the in-memory state reflects the
		 * most-recently committed TXG.  Otherwise (v1/v2/v3) keep
		 * the legacy "first ROOT_VOLUME wins, else first present
		 * disk" behavior.
		 */
		if (voldata->version >= HAMMER2_VOL_VERSION_RAIDZ2 &&
		    voldata->raid_config.raid_type ==
		     HAMMER2_RAID_TYPE_RAID6) {
			/*
			 * On a tie prefer the lowest disk id, so the
			 * choice does not depend on the order of the
			 * mount spec.  Older newfs_hammer2 wrote the
			 * sroot blockset only into disk 0's header.
			 */
			if (*rootvoldevvp == NULL ||
			    voldata->raid_config.rz_txg_seq >
			     rootvoldata->raid_config.rz_txg_seq ||
			    (voldata->raid_config.rz_txg_seq ==
			      rootvoldata->raid_config.rz_txg_seq &&
			     voldata->volu_id < rootvoldata->volu_id)) {
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
	 * v3 RAID6 quorum on the TXG seq (docs/volhdr_quorum.md).
	 *
	 * The headers of a TXG are written one disk after another, so a
	 * crash can leave the disks at T+1 and T.  A disk that reached T+1
	 * still holds its T header in another zone, so a disk votes for
	 * every seq that one of its valid zones carries.  The mount takes
	 * the highest seq a majority supports.  Stepping back one TXG is
	 * the normal outcome of such a crash and needs no authorization;
	 * deeper rollbacks need vfs.hammer2.j2_allow_rollback.
	 *
	 * A disk without a zone at the chosen seq is behind: its columns
	 * for that TXG may be missing.  It is marked FAILED in the header
	 * we mount with (the next commit persists that) until it is
	 * resilvered.  A disk with a newer zone (an orphan T+1 header after
	 * a rollback) is fine: only the chosen zone is ever used, and the
	 * next commit writes its volume header into the zone after the
	 * chosen one on every online disk, which is where the orphan is.
	 */
	if (!error &&
	    rootvoldata->version >= HAMMER2_VOL_VERSION_RAIDZ2 &&
	    rootvoldata->raid_config.raid_type ==
	     HAMMER2_RAID_TYPE_RAID6) {
		hammer2_raid_config_t *rrc = &rootvoldata->raid_config;
		uint32_t ndisks = rrc->ndisks;
		uint32_t electorate = 0;
		uint32_t majority;
		uint32_t nsupport;
		uint32_t nfailed;
		uint64_t target = rrc->rz_txg_seq;	/* newest seen */
		uint64_t chosen = target;
		uint64_t lowest = (uint64_t)-1;
		uint32_t seen_count = 0;
		uint32_t rollback_max = (uint32_t)
		    (hammer2_j2_rollback_max > 0 ?
		     hammer2_j2_rollback_max : 8);
		int j;

		/*
		 * Disks the newest header marks FAILED stopped receiving
		 * header writes when they failed, so their seqs lag by
		 * design.  They do not vote.  The rest must reach a
		 * majority of themselves and at least ndisks - 2 (the
		 * disks needed to read the data at all).
		 */
		for (j = 0; j < (int)ndisks && j < HAMMER2_MAX_VOLUMES; j++) {
			if (rrc->disk_state[j] == HAMMER2_RAID6_DISK_FAILED)
				disk_seen[j] = 0;
			else
				electorate++;
		}
		majority = electorate / 2 + 1;
		if (majority < ndisks - 2)
			majority = ndisks - 2;

		for (j = 0; j < HAMMER2_MAX_VOLUMES; j++) {
			if (!disk_seen[j])
				continue;
			seen_count++;
			if (disk_seqs[j] < lowest)
				lowest = disk_seqs[j];
		}

		for (;;) {
			nsupport = 0;
			for (j = 0; j < HAMMER2_MAX_VOLUMES; j++) {
				if (disk_seen[j] &&
				    hammer2_volzone_find(zones[j], j, &fsid,
							 chosen) >= 0)
					nsupport++;
			}
			if (nsupport >= majority)
				break;
			if (chosen == 0 || target - chosen >= rollback_max) {
				hprintf("v3 quorum: no majority within "
					"%u-TXG rollback limit "
					"(newest seq %ju, lowest %ju, "
					"%u disks seen of %u, need %u)\n",
					rollback_max,
					(uintmax_t)target,
					(uintmax_t)lowest,
					seen_count, ndisks, majority);
				error = ENXIO;
				goto done;
			}
			chosen--;
		}

		if (chosen != target) {
			int zn = -1;

			if (target - chosen > 1 && !hammer2_j2_allow_rollback) {
				hprintf("v3 quorum: would roll back "
					"%ju -> %ju (%u/%u disks support "
					"it); refusing.  Set "
					"vfs.hammer2.j2_allow_rollback=1 to "
					"authorize and accept the loss of "
					"writes between those TXGs.\n",
					(uintmax_t)target, (uintmax_t)chosen,
					nsupport, ndisks);
				error = ENXIO;
				goto done;
			}
			hprintf("v3 quorum: rolling back %ju -> %ju "
				"(%u/%u disks support seq %ju)\n",
				(uintmax_t)target, (uintmax_t)chosen,
				nsupport, ndisks, (uintmax_t)chosen);

			/*
			 * Mount with the header of the chosen seq, read
			 * from the lowest supporting disk.  The extent
			 * table, disk_state etc. all come from that copy.
			 */
			for (j = 0; j < HAMMER2_MAX_VOLUMES; j++) {
				if (!disk_seen[j])
					continue;
				zn = hammer2_volzone_find(zones[j], j, &fsid,
							  chosen);
				if (zn >= 0)
					break;
			}
			KKASSERT(j < HAMMER2_MAX_VOLUMES);
			e = volumes[j].dev;
			if (hammer2_read_volume_zone(e->devvp, e->path, zn,
						     rootvoldata) < 0) {
				hprintf("v3 quorum: cannot re-read header "
					"%d of %s\n", zn, e->path);
				error = ENXIO;
				goto done;
			}
			*rootvolzone = zn;
			*rootvoldevvp = e->devvp;
		}

		/*
		 * Disks that do not support the chosen seq are behind and
		 * need a resilver.  Count them with the disks already
		 * failed and the ones absent; more than two is more than
		 * RAID6 can carry.
		 */
		nfailed = 0;
		for (j = 0; j < (int)ndisks && j < HAMMER2_MAX_VOLUMES; j++) {
			if (rrc->disk_state[j] == HAMMER2_RAID6_DISK_FAILED) {
				nfailed++;
				continue;
			}
			for (i = 0; i < HAMMER2_NUM_VOLHDRS; i++) {
				if (zones[j][i].valid)
					break;
			}
			if (i == HAMMER2_NUM_VOLHDRS) {	/* absent */
				nfailed++;
				continue;
			}
			if (hammer2_volzone_find(zones[j], j, &fsid,
						 chosen) >= 0)
				continue;
			hprintf("v3 quorum: disk %d newest seq %ju is below "
				"%ju; marking it failed (needs resilver)\n",
				j, (uintmax_t)disk_seqs[j],
				(uintmax_t)chosen);
			rrc->disk_state[j] = HAMMER2_RAID6_DISK_FAILED;
			rrc->flags |= HAMMER2_RAID6_FLAG_DEGRADED;
			nfailed++;
		}
		if (nfailed > 2) {
			hprintf("v3 quorum: %u of %u disks failed, absent or "
				"behind seq %ju; more than RAID6 can "
				"recover\n", nfailed, ndisks,
				(uintmax_t)chosen);
			error = ENXIO;
			goto done;
		}
	}

	/*
	 * For RAID6 degraded mounts: assign unavailable device entries
	 * to uninitialized volume slots so vol->dev is non-NULL for all
	 * volumes.  This allows code that accesses vol->dev->path or
	 * vol->dev->open to work without NULL checks everywhere.
	 */
	if (!error && rootvoldata->version >= HAMMER2_VOL_VERSION_RAIDZ2) {
		hammer2_raid_config_t *rc = &rootvoldata->raid_config;
		int slot;

		e = TAILQ_FIRST(devvpl);
		for (slot = 0; slot < rc->ndisks; slot++) {
			vol = &volumes[slot];
			if (vol->id != -1)
				continue;
			/*
			 * Find the next unavailable entry.  A disk left
			 * out of the mount spec has none; give it a
			 * placeholder so its slot still maps its offsets.
			 */
			while (e != NULL && e->devvp != NULL && e->open)
				e = TAILQ_NEXT(e, entry);
			if (e == NULL) {
				hprintf("RAID6 disk %d not in the mount "
					"spec\n", slot);
				e = hammer2_add_absent_devvp(devvpl,
							     "(missing)");
				if (e == NULL) {
					error = ENOMEM;
					break;
				}
			}
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
	kfree(zones, M_HAMMER2);

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
 * Mark the space map page holding `slot` stale in both copies.  Caller
 * holds stripe_bitmap_spin.
 */
static __inline void
hammer2_raid6_sm_touch(hammer2_dev_t *hmp, uint64_t slot)
{
	hmp->sm_dirty[slot >> HAMMER2_SM_PAGE_RADIX] = 3;
}

/*
 * Blockref-walk reconstruction of the v3 space map.
 *
 * Called from vfs_mount when hammer2_raid6_sm_read found no valid
 * copy.  Walks the on-disk chain tree from hmp->vchain and counts every
 * reachable DATA/DIRENT blockref in its row.  Both copies are then
 * rewritten whole by the next two flushes.
 */
/*
 * Per-bref accounting at walker time.  Each live DATA/DIRENT bref
 * contributes one column to its row, so refcount = count of live
 * brefs sharing the slot.  Bitmap bit is the union (set iff any
 * bref).  6C packing depends on this — under-counting would let the
 * allocator reuse a slot that still has live cols.
 */
/*
 * The space map slot of a v3 DATA/DIRENT blockref.  Returns 0 for any
 * other blockref.
 */
static int
hammer2_raid6_bref_slot(hammer2_dev_t *hmp, const hammer2_blockref_t *bref,
			uint64_t *slotp)
{
	hammer2_off_t phys_off;
	uint64_t slot;

	if (bref->type != HAMMER2_BREF_TYPE_DATA &&
	    bref->type != HAMMER2_BREF_TYPE_DIRENT)
		return 0;
	phys_off = bref->data_off & ~HAMMER2_OFF_MASK_RADIX;
	if (phys_off < HAMMER2_ZONE_SEG64)
		return 0;
	slot = (phys_off - HAMMER2_ZONE_SEG64) /
	    hmp->raid_config.stripe_unit;
	if (slot >= hmp->stripe_num_slots)
		return 0;
	*slotp = slot;
	return 1;
}

static int
hammer2_raid6_record_bref(hammer2_dev_t *hmp, const hammer2_blockref_t *bref)
{
	uint64_t slot;

	if (!hammer2_raid6_bref_slot(hmp, bref, &slot))
		return 0;

	hammer2_spin_ex(&hmp->stripe_bitmap_spin);
	hmp->stripe_bitmap[slot / 8] |= (uint8_t)(1 << (slot % 8));
	if (hmp->stripe_row_refcount[slot] < 0xff)
		hmp->stripe_row_refcount[slot]++;
	if (slot >= hmp->stripe_cursor)
		hmp->stripe_cursor = slot + 1;
	hammer2_spin_unex(&hmp->stripe_bitmap_spin);
	return 0;
}

static int
hammer2_raid6_walk_chain(hammer2_dev_t *hmp, hammer2_chain_t *parent)
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

		(void)hammer2_raid6_record_bref(hmp, &bref);

		if (chain) {
			error = hammer2_raid6_walk_chain(hmp, chain);
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
hammer2_raid6_rebuild_stripe_bitmap(hammer2_dev_t *hmp)
{
	int error;

	KKASSERT(hammer2_raid6_v3(hmp));
	KKASSERT(hmp->stripe_bitmap != NULL);

	bzero(hmp->stripe_bitmap, hmp->stripe_bitmap_size);
	bzero(hmp->stripe_row_refcount,
	      (size_t)hmp->sm_npages * HAMMER2_SM_PAGE);
	hmp->stripe_cursor = 0;

	hammer2_chain_lock(&hmp->vchain, HAMMER2_RESOLVE_ALWAYS);
	error = hammer2_raid6_walk_chain(hmp, &hmp->vchain);
	hammer2_chain_unlock(&hmp->vchain);
	hammer2_raid6_sm_dirty_all(hmp);
	return error;
}

/*
 * Check the stripe layout recorded in the volume header and allocate
 * the in-memory space map.  Returns EINVAL for a v3 volume made before
 * the layout (docs/capacity.md); it has to be re-made.
 */
int
hammer2_raid6_sm_init(hammer2_dev_t *hmp)
{
	hammer2_raid_config_t *rc = &hmp->raid_config;
	uint64_t nslots = rc->rz_num_slots;
	uint64_t npages;

	if (rc->rz_layout != HAMMER2_RZ_LAYOUT) {
		kprintf("hammer2: RAID6 v3 volume has stripe layout %u, "
			"this kernel needs %u; re-run newfs_hammer2\n",
			rc->rz_layout, HAMMER2_RZ_LAYOUT);
		return EINVAL;
	}
	npages = (nslots + HAMMER2_SM_PAGE_SLOTS - 1) / HAMMER2_SM_PAGE_SLOTS;
	if (nslots == 0 || npages != rc->rz_sm_pages ||
	    npages > HAMMER2_SM_MAX_PAGES ||
	    rc->rz_sm_copy != (npages + 1) * HAMMER2_SM_PAGE ||
	    hmp->md_nextents == 0) {
		kprintf("hammer2: RAID6 v3 space map geometry invalid "
			"(slots %ju pages %u copy %ju extents %u)\n",
			(uintmax_t)nslots, rc->rz_sm_pages,
			(uintmax_t)rc->rz_sm_copy, hmp->md_nextents);
		return EINVAL;
	}

	hmp->stripe_num_slots = nslots;
	hmp->sm_npages = (uint32_t)npages;
	hmp->stripe_bitmap_size = (size_t)((nslots + 7) / 8);
	hmp->stripe_bitmap = kmalloc(hmp->stripe_bitmap_size, M_HAMMER2,
				     M_WAITOK | M_ZERO);
	hmp->stripe_row_refcount = kmalloc((size_t)npages * HAMMER2_SM_PAGE,
					   M_HAMMER2, M_WAITOK | M_ZERO);
	hmp->sm_dirty = kmalloc((size_t)npages, M_HAMMER2, M_WAITOK | M_ZERO);
	hmp->sm_crc[0] = kmalloc((size_t)npages * sizeof(uint32_t),
				 M_HAMMER2, M_WAITOK | M_ZERO);
	hmp->sm_crc[1] = kmalloc((size_t)npages * sizeof(uint32_t),
				 M_HAMMER2, M_WAITOK | M_ZERO);
	hmp->stripe_bf_seen = kmalloc(hmp->stripe_bitmap_size, M_HAMMER2,
				      M_WAITOK | M_ZERO);
	hmp->stripe_bf_new = kmalloc(hmp->stripe_bitmap_size, M_HAMMER2,
				     M_WAITOK | M_ZERO);
	hmp->stripe_bf_staged = kmalloc(hmp->stripe_bitmap_size, M_HAMMER2,
					M_WAITOK | M_ZERO);
	hmp->stripe_open = kmalloc(hmp->stripe_bitmap_size, M_HAMMER2,
				   M_WAITOK | M_ZERO);
	hmp->stripe_bf_active = 0;
	hmp->stripe_bf_complete = 0;
	hmp->stripe_cursor = 0;
	hmp->stripe_generation = hmp->voldata.raid_config.rz_sm_gen;
	hmp->stripe_bitmap_invalid = 0;
	spin_init(&hmp->stripe_bitmap_spin, "h2smap");
	hmp->stripe_next_disk = 0;
	return 0;
}

void
hammer2_raid6_sm_destroy(hammer2_dev_t *hmp)
{
	uint8_t **bfp[] = { &hmp->stripe_bf_seen, &hmp->stripe_bf_new,
			    &hmp->stripe_bf_staged, &hmp->stripe_open };
	int c;

	for (c = 0; c < 4; c++) {
		if (*bfp[c]) {
			kfree(*bfp[c], M_HAMMER2);
			*bfp[c] = NULL;
		}
	}
	if (hmp->stripe_bitmap) {
		kfree(hmp->stripe_bitmap, M_HAMMER2);
		hmp->stripe_bitmap = NULL;
	}
	if (hmp->stripe_row_refcount) {
		kfree(hmp->stripe_row_refcount, M_HAMMER2);
		hmp->stripe_row_refcount = NULL;
	}
	if (hmp->sm_dirty) {
		kfree(hmp->sm_dirty, M_HAMMER2);
		hmp->sm_dirty = NULL;
	}
	for (c = 0; c < 2; c++) {
		if (hmp->sm_crc[c]) {
			kfree(hmp->sm_crc[c], M_HAMMER2);
			hmp->sm_crc[c] = NULL;
		}
	}
}

/*
 * Make the next two flushes write both copies whole: after a walk
 * rebuild, and when a disk comes back whose copies are stale.
 */
void
hammer2_raid6_sm_dirty_all(hammer2_dev_t *hmp)
{
	if (hmp->sm_dirty == NULL)
		return;
	hammer2_spin_ex(&hmp->stripe_bitmap_spin);
	memset(hmp->sm_dirty, 3, hmp->sm_npages);
	hammer2_spin_unex(&hmp->stripe_bitmap_spin);
}

/*
 * Return the device for disk i when it can hold a current space map
 * copy: present, open and not failed.  A failed disk stops receiving
 * copies, so its copies are stale and must not be loaded.
 */
static struct vnode *
hammer2_raid6_sm_devvp(hammer2_dev_t *hmp, int i)
{
	hammer2_devvp_t *e = hmp->volumes[i].dev;

	if (e == NULL || e->devvp == NULL || !e->open)
		return NULL;
	if (hmp->raid_failed[i])
		return NULL;
	return e->devvp;
}

static uint32_t
hammer2_raid6_sm_hdr_crc(const hammer2_sm_header_t *hdr)
{
	uint32_t c;

	c = hammer2_icrc32(hdr, offsetof(hammer2_sm_header_t, hdr_crc));
	return hammer2_icrc32c((const char *)hdr + HAMMER2_SM_CRC_OFF,
			       hdr->npages * sizeof(uint32_t), c);
}

/*
 * Load the space map copy committed by the volume header: copy
 * rz_sm_gen & 1, generation rz_sm_gen.  The CRC table comes from the
 * first disk with a valid header; each page is taken from the first
 * disk whose copy matches its CRC, so one stale or damaged disk does
 * not cost the whole map.  If any page is missing everywhere the map
 * is marked invalid and the mount path rebuilds it with a walk.
 */
void
hammer2_raid6_sm_read(hammer2_dev_t *hmp)
{
	hammer2_raid_config_t *rc = &hmp->raid_config;
	uint64_t gen = hmp->voldata.raid_config.rz_sm_gen;
	int c = (int)(gen & 1);
	hammer2_off_t base = rc->rz_sm_off + c * rc->rz_sm_copy;
	uint32_t npages = hmp->sm_npages;
	uint32_t *crcs = hmp->sm_crc[c];
	hammer2_sm_header_t *hdr;
	struct vnode *devvp;
	struct buf *bp;
	uint64_t cursor = 0;
	uint64_t slot;
	uint32_t p;
	int found = 0;
	int i;

	for (i = 0; i < hmp->nvolumes && !found; i++) {
		devvp = hammer2_raid6_sm_devvp(hmp, i);
		if (devvp == NULL)
			continue;
		bp = NULL;
		if (bread(devvp, base, HAMMER2_SM_PAGE, &bp)) {
			if (bp)
				brelse(bp);
			continue;
		}
		hdr = (hammer2_sm_header_t *)bp->b_data;
		if (hdr->magic == HAMMER2_SM_MAGIC &&
		    hdr->version == HAMMER2_SM_VERSION &&
		    hdr->ndisks == rc->ndisks &&
		    hdr->stripe_unit == rc->stripe_unit &&
		    hdr->num_slots == hmp->stripe_num_slots &&
		    hdr->npages == npages &&
		    hdr->generation == gen &&
		    hdr->copy == (uint32_t)c &&
		    hdr->hdr_crc == hammer2_raid6_sm_hdr_crc(hdr)) {
			bcopy((char *)bp->b_data + HAMMER2_SM_CRC_OFF, crcs,
			      npages * sizeof(uint32_t));
			cursor = hdr->cursor;
			found = 1;
		}
		brelse(bp);
	}
	if (!found) {
		kprintf("hammer2: no valid space map header for "
			"generation %ju\n", (uintmax_t)gen);
		goto invalid;
	}

	for (p = 0; p < npages; p++) {
		uint8_t *dst = hmp->stripe_row_refcount +
			       (size_t)p * HAMMER2_SM_PAGE;
		int ok = 0;

		for (i = 0; i < hmp->nvolumes && !ok; i++) {
			devvp = hammer2_raid6_sm_devvp(hmp, i);
			if (devvp == NULL)
				continue;
			bp = NULL;
			if (bread(devvp, base + (1 + (off_t)p) * HAMMER2_SM_PAGE,
				  HAMMER2_SM_PAGE, &bp) == 0 &&
			    hammer2_icrc32(bp->b_data, HAMMER2_SM_PAGE) ==
			    crcs[p]) {
				bcopy(bp->b_data, dst, HAMMER2_SM_PAGE);
				ok = 1;
			}
			if (bp)
				brelse(bp);
		}
		if (!ok) {
			kprintf("hammer2: space map page %u of generation "
				"%ju bad on every disk\n", p, (uintmax_t)gen);
			goto invalid;
		}
	}

	for (slot = 0; slot < hmp->stripe_num_slots; slot++) {
		if (hmp->stripe_row_refcount[slot])
			hmp->stripe_bitmap[slot / 8] |=
			    (uint8_t)(1 << (slot % 8));
	}
	hmp->stripe_cursor = cursor;

	/*
	 * Nothing says what the other copy holds; the next flush, which
	 * writes it, rewrites it whole.
	 */
	memset(hmp->sm_dirty, 1 << (c ^ 1), npages);
	return;

invalid:
	bzero(hmp->stripe_row_refcount, (size_t)npages * HAMMER2_SM_PAGE);
	hmp->stripe_bitmap_invalid = 1;
}

/*
 * Write space map generation stripe_generation + 1 to its copy on every
 * live disk, synchronously.  Called by the flush before the volume
 * headers; the cache flush that precedes each volume header write makes
 * the copy durable before the header that names it.  The copy is the
 * one the on-disk headers do not name, so a crash part way leaves the
 * committed copy alone.
 *
 * Only pages changed since the copy was last written are written.  A
 * page that fails on any disk stays dirty.  Returns the new generation
 * in *genp; the caller records it in the volume header and commits it
 * to stripe_generation once the headers are written.  Fails with EIO
 * when no live disk took the whole copy.
 */
int
hammer2_raid6_sm_write(hammer2_dev_t *hmp, uint64_t *genp)
{
	hammer2_raid_config_t *rc = &hmp->raid_config;
	uint64_t gen = hmp->stripe_generation + 1;
	int c = (int)(gen & 1);
	uint8_t bit = (uint8_t)(1 << c);
	hammer2_off_t base = rc->rz_sm_off + c * rc->rz_sm_copy;
	uint32_t npages = hmp->sm_npages;
	int derr[HAMMER2_MAX_VOLUMES];
	hammer2_sm_header_t *hdr;
	struct vnode *devvp;
	struct buf *bp;
	char *pbuf;
	uint32_t p;
	int perr;
	int i;

	bzero(derr, sizeof(derr));
	pbuf = kmalloc(HAMMER2_SM_PAGE, M_HAMMER2, M_WAITOK);

	/*
	 * Shared rebuild_lk, as for any writer to a member: a disk that
	 * goes away is failed and its buffers thrown out under the
	 * exclusive lock, and a buffer getblk() makes here meanwhile
	 * panics vinvalbuf().
	 */
	lockmgr(&hmp->rebuild_lk, LK_SHARED);

	for (p = 0; p < npages; p++) {
		if ((hmp->sm_dirty[p] & bit) == 0)
			continue;
		hammer2_spin_ex(&hmp->stripe_bitmap_spin);
		bcopy(hmp->stripe_row_refcount + (size_t)p * HAMMER2_SM_PAGE,
		      pbuf, HAMMER2_SM_PAGE);
		hmp->sm_dirty[p] &= ~bit;
		hammer2_spin_unex(&hmp->stripe_bitmap_spin);
		hmp->sm_crc[c][p] = hammer2_icrc32(pbuf, HAMMER2_SM_PAGE);

		perr = 0;
		for (i = 0; i < hmp->nvolumes; i++) {
			devvp = hammer2_raid6_sm_devvp(hmp, i);
			if (devvp == NULL)
				continue;
			bp = getblk(devvp,
				    base + (1 + (off_t)p) * HAMMER2_SM_PAGE,
				    HAMMER2_SM_PAGE, GETBLK_KVABIO, 0);
			bkvasync(bp);
			bcopy(pbuf, bp->b_data, HAMMER2_SM_PAGE);
			if (bwrite(bp)) {
				derr[i] = 1;
				perr = 1;
			}
		}
		if (perr) {
			hammer2_spin_ex(&hmp->stripe_bitmap_spin);
			hmp->sm_dirty[p] |= bit;
			hammer2_spin_unex(&hmp->stripe_bitmap_spin);
		}
	}

	bzero(pbuf, HAMMER2_SM_PAGE);
	hdr = (hammer2_sm_header_t *)pbuf;
	hdr->magic = HAMMER2_SM_MAGIC;
	hdr->version = HAMMER2_SM_VERSION;
	hdr->ndisks = rc->ndisks;
	hdr->stripe_unit = rc->stripe_unit;
	hdr->num_slots = hmp->stripe_num_slots;
	hdr->generation = gen;
	hdr->cursor = hmp->stripe_cursor;
	hdr->npages = npages;
	hdr->copy = (uint32_t)c;
	bcopy(hmp->sm_crc[c], pbuf + HAMMER2_SM_CRC_OFF,
	      npages * sizeof(uint32_t));
	hdr->hdr_crc = hammer2_raid6_sm_hdr_crc(hdr);

	perr = EIO;
	for (i = 0; i < hmp->nvolumes; i++) {
		devvp = hammer2_raid6_sm_devvp(hmp, i);
		if (devvp == NULL || derr[i])
			continue;
		bp = getblk(devvp, base, HAMMER2_SM_PAGE, GETBLK_KVABIO, 0);
		bkvasync(bp);
		bcopy(pbuf, bp->b_data, HAMMER2_SM_PAGE);
		if (bwrite(bp) == 0)
			perr = 0;
	}
	lockmgr(&hmp->rebuild_lk, LK_RELEASE);
	kfree(pbuf, M_HAMMER2);

	if (perr)
		kprintf("hammer2: space map generation %ju not written "
			"on any disk\n", (uintmax_t)gen);
	*genp = gen;
	return perr;
}

/*
 * Open-row tracking — see hmp->open_rows comment in hammer2.h.
 *
 * Each entry tracks a row (stripe slot) allocated during the current
 * TXG that hasn't sealed P/Q yet.  The allocator packs additional
 * chains into existing open rows so up to ndata chains share one
 * row's parity columns.  Rows seal when full (n_alloc == ndata and
 * every alloc'd col has col_data) or at TXG flush boundary.
 *
 * Stored as a TAILQ.  Entries are kmalloc'd at register time and
 * kfree'd at seal time — no fixed cap, never force-seal an
 * incomplete row.
 */

void
hammer2_raid6_open_rows_init(hammer2_dev_t *hmp)
{
	if (hmp->open_rows_inited)
		return;
	TAILQ_INIT(&hmp->open_rows);
	hmp->open_rows_inited = 1;
	hmp->open_rows_count = 0;
	hmp->open_rows_high = 0;
}

void
hammer2_raid6_open_rows_free(hammer2_dev_t *hmp)
{
	struct hammer2_open_row *r;
	int d;

	if (!hmp->open_rows_inited)
		return;
	if (hmp->open_rows_count)
		kprintf("hammer2: v3 unmount drops %ld open rows "
			"(columns not committed)\n", hmp->open_rows_count);
	while ((r = TAILQ_FIRST(&hmp->open_rows)) != NULL) {
		TAILQ_REMOVE(&hmp->open_rows, r, entry);
		if (hmp->stripe_open)
			clrbit(hmp->stripe_open, r->row_id);
		for (d = 0; d < HAMMER2_MAX_VOLUMES; d++) {
			if (r->col_data[d]) {
				kfree(r->col_data[d], M_HAMMER2);
				r->col_data[d] = NULL;
			}
		}
		kfree(r, M_HAMMER2);
	}
	hmp->open_rows_count = 0;
	hmp->open_rows_inited = 0;
}

/*
 * Write a row: zero the data columns no chain reserved, write the
 * columns putblk dropped, then P/Q over cols[].  Called without
 * stripe_bitmap_spin; sleeps.
 */
static void
hammer2_raid6_row_io(hammer2_dev_t *hmp, hammer2_off_t phys_off,
		     uint64_t row_id, uint32_t alloc_mask, uint32_t dropped,
		     int p_disk, int q_disk, hammer2_row_col_t *cols,
		     int ncols, size_t bytes)
{
	void *zbuf = NULL;
	struct buf *zbps[HAMMER2_MAX_VOLUMES];
	int ndisks = hmp->raid_config.ndisks;
	int nz = 0;
	int zdisk[HAMMER2_MAX_VOLUMES];
	int d, i;

	/*
	 * Hold off the resilver's copy of this slot while we write it,
	 * and tell it the slot is done if a rebuild is running: this seal
	 * writes every column of the rebuilding disk.
	 */
	lockmgr(&hmp->rebuild_lk, LK_SHARED);
	if (hmp->rebuild_active && hmp->rebuild_done &&
	    row_id / 8 < hmp->stripe_bitmap_size) {
		hammer2_spin_ex(&hmp->stripe_bitmap_spin);
		hmp->rebuild_done[row_id / 8] |= (uint8_t)(1 << (row_id % 8));
		hammer2_spin_unex(&hmp->stripe_bitmap_spin);
	}

	if (ncols > 0) {
		/*
		 * Zero data-disk columns that no chain ever reserved.  P/Q
		 * are computed assuming those cols are zero, but the disk
		 * itself still holds whatever bytes a prior tenant left
		 * (or factory garbage in a never-touched region); without
		 * this zero-write, parity reconstruction over the on-disk
		 * column on a later resilver/scrub would yield wrong data
		 * for the *written* columns.  Only fires for data disks NOT
		 * in alloc_mask — disks that are reserved but whose chain
		 * has not yet putblk'd will receive their bytes through the
		 * chain's own bp.bdwrite path.  They are written in
		 * parallel with each other and with P/Q, and waited for
		 * before the seal returns.
		 */
		for (d = 0; d < ndisks; d++) {
			struct buf *zbp;
			int werr;

			if (d == p_disk || d == q_disk)
				continue;
			if (alloc_mask & (1U << d))
				continue;
			if (!hammer2_raid6_disk_writable(hmp, d))
				continue;
			if (hmp->volumes[d].dev == NULL ||
			    hmp->volumes[d].dev->devvp == NULL)
				continue;
			if (hmp->raid_failed[d]) {
				/* rebuilding disk: never wait on its bufs */
				if (zbuf == NULL)
					zbuf = kmalloc(bytes, M_HAMMER2,
						       M_WAITOK | M_ZERO);
				werr = hammer2_io_raid6_write_nowait(
				    hmp->volumes[d].dev->devvp, phys_off,
				    zbuf, (int)bytes);
				if (werr)
					hammer2_raid6_write_failed(hmp, d,
								   werr);
				continue;
			}
			zbp = getblk(hmp->volumes[d].dev->devvp, phys_off,
				     (int)bytes, GETBLK_KVABIO, 0);
			if (zbp == NULL)
				continue;
			bkvasync(zbp);
			bzero(zbp->b_data, bytes);
			/* a column left non-zero makes P/Q wrong for it */
			if (hammer2_bwrite_start(zbp)) {
				zdisk[nz] = d;
				zbps[nz++] = zbp;
			}
		}

		/*
		 * putblk skips the data column of a failed disk.  Write it
		 * here if that disk now takes writes: it is being rebuilt,
		 * or it came back online since the putblk.
		 */
		for (i = 0; i < ncols; i++) {
			int werr;

			d = cols[i].disk_idx;
			if ((dropped & (1U << d)) == 0)
				continue;
			if (!hammer2_raid6_disk_writable(hmp, d))
				continue;
			if (hmp->volumes[d].dev == NULL ||
			    hmp->volumes[d].dev->devvp == NULL)
				continue;
			werr = hammer2_io_raid6_write_nowait(
			    hmp->volumes[d].dev->devvp, phys_off,
			    cols[i].data, (int)bytes);
			if (werr)
				hammer2_raid6_write_failed(hmp, d, werr);
		}

		/*
		 * write_row auto-fails a member whose P/Q write fails; an
		 * error back means redundancy is exhausted, which
		 * auto_fail_disk has already reported.
		 */
		(void)hammer2_io_raid6_write_row(hmp, phys_off, cols,
						 ncols, bytes);

		for (i = 0; i < nz; i++) {
			int werr = hammer2_bwrite_wait(zbps[i]);

			if (werr)
				hammer2_raid6_write_failed(hmp, zdisk[i],
							   werr);
		}
	}
	lockmgr(&hmp->rebuild_lk, LK_RELEASE);
	if (zbuf)
		kfree(zbuf, M_HAMMER2);
}

/*
 * Seal a row: build cols[] from in-memory col_data, write it, free
 * col data, remove the entry from the open_rows TAILQ and free the
 * entry itself.  Caller holds stripe_bitmap_spin and has checked that
 * every reserved column has its data (or that the row is being
 * dropped); this function releases the spin around the (sleeping)
 * I/O and re-acquires it before returning.
 */
static void
hammer2_raid6_seal_row_locked_to_unlocked(hammer2_dev_t *hmp,
					  struct hammer2_open_row *r)
{
	hammer2_row_col_t cols[HAMMER2_MAX_VOLUMES];
	void *to_free[HAMMER2_MAX_VOLUMES];
	int ncols = 0;
	int d, i;

	for (d = 0; d < hmp->raid_config.ndisks; d++) {
		if ((r->alloc_mask & (1U << d)) == 0)
			continue;
		if (r->col_data[d] == NULL)
			continue;
		cols[ncols].disk_idx = d;
		cols[ncols].data = r->col_data[d];
		cols[ncols].bytes = r->bytes;
		to_free[ncols] = r->col_data[d];
		r->col_data[d] = NULL;
		ncols++;
	}
	TAILQ_REMOVE(&hmp->open_rows, r, entry);
	hmp->open_rows_count--;
	clrbit(hmp->stripe_open, r->row_id);
	hammer2_spin_unex(&hmp->stripe_bitmap_spin);

	hammer2_raid6_row_io(hmp, r->phys_off, r->row_id, r->alloc_mask,
			     r->dropped_mask, r->p_disk, r->q_disk,
			     cols, ncols, r->bytes);
	for (i = 0; i < ncols; i++)
		kfree(to_free[i], M_HAMMER2);
	kfree(r, M_HAMMER2);

	hammer2_spin_ex(&hmp->stripe_bitmap_spin);
}

/*
 * Every reserved column of the row has its data.  alloc_mask is 32
 * bits: loop over the array's disks, not HAMMER2_MAX_VOLUMES, or the
 * shift wraps and a col_data[] past the array counts as missing.
 */
static int
hammer2_raid6_row_complete(hammer2_dev_t *hmp, struct hammer2_open_row *r)
{
	int d;

	for (d = 0; d < hmp->raid_config.ndisks; d++) {
		if ((r->alloc_mask & (1U << d)) && r->col_data[d] == NULL)
			return 0;
	}
	return 1;
}

/*
 * Walk open_rows for an entry with at least one free data disk that
 * isn't p_disk/q_disk/failed/already-allocated.  On success, mark the
 * disk as allocated, return phys_off + disk_idx.  Caller already holds
 * stripe_bitmap_spin.
 *
 * Returns 1 on success, 0 if no open row could absorb.  next_disk is
 * advanced on success to spread chains across disks.
 */
static int
hammer2_raid6_open_row_pack_locked(hammer2_dev_t *hmp,
				   hammer2_off_t *phys_off_out,
				   int *disk_idx_out)
{
	hammer2_raid_config_t *rc = &hmp->raid_config;
	int ndisks = rc->ndisks;
	int ndata = rc->ndata;
	struct hammer2_open_row *r;
	int t, candidate;

	if (!hmp->open_rows_inited)
		return 0;

	TAILQ_FOREACH(r, &hmp->open_rows, entry) {
		if (r->n_alloc >= ndata || r->gen != hmp->rz_gen)
			continue;
		for (t = 0; t < ndisks; t++) {
			candidate = hmp->stripe_next_disk % ndisks;
			hmp->stripe_next_disk++;
			if (candidate == r->p_disk || candidate == r->q_disk)
				continue;
			if (hmp->raid_failed[candidate])
				continue;
			if (r->alloc_mask & (1U << candidate))
				continue;
			r->alloc_mask |= (1U << candidate);
			r->n_alloc++;
			*phys_off_out = r->phys_off;
			*disk_idx_out = candidate;
			return 1;
		}
	}
	return 0;
}

/*
 * Place a freshly-allocated row into open_rows[].  Called from
 * stripe_alloc after the bitmap scan with stripe_bitmap_spin held, so
 * the caller allocates the entry `r` (zeroed) before taking the lock.
 */
static void
hammer2_raid6_open_row_register_locked(hammer2_dev_t *hmp,
				       struct hammer2_open_row *r,
				       uint64_t row_id,
				       hammer2_off_t phys_off,
				       int disk_idx)
{
	hammer2_raid_config_t *rc = &hmp->raid_config;
	int ndisks = rc->ndisks;
	int ndata = rc->ndata;
	int p_disk = (int)(row_id % ndisks);
	int q_disk = (p_disk + 1) % ndisks;

	/*
	 * TAILQ-backed tracker — never force-seal.  Each new row
	 * has its own entry; entries are freed at seal time.
	 * Memory is bounded by (in-flight rows) × (per-entry metadata
	 * + per-col_data buffer); typical TXG flushes drain everything
	 * via hammer2_raid6_seal_all_open_rows.
	 */
	r->phys_off   = phys_off;
	r->gen        = hmp->rz_gen;
	r->row_id     = row_id;
	r->p_disk     = p_disk;
	r->q_disk     = q_disk;
	r->alloc_mask = (1U << disk_idx);
	r->n_alloc    = 1;
	r->ndata      = ndata;
	r->bytes      = rc->stripe_unit;

	TAILQ_INSERT_TAIL(&hmp->open_rows, r, entry);
	setbit(hmp->stripe_open, row_id);
	hmp->open_rows_count++;
	if (hmp->open_rows_count > hmp->open_rows_high)
		hmp->open_rows_high = hmp->open_rows_count;
	if (hmp->open_rows_count > hammer2_rz_open_high)
		hammer2_rz_open_high = hmp->open_rows_count;
}

/*
 * Return 1 if the column bref names cannot take another put in place:
 * its row has sealed (no open row) or the column's data is already in
 * the row.  Return 0 if it is still pending, i.e. a put is on its way
 * and a new put coalesces into it.  A column in a sealed row sits under
 * P/Q that are already on disk, and an arrived one may be sealed by
 * another chain's put at any moment, so a chain that rewrites it in the
 * same TXG must take a fresh slot (oct-review.md §10.3).  A pending
 * column keeps its row open, so this cannot race a seal.
 */
int
hammer2_raid6_col_arrived(hammer2_dev_t *hmp, const hammer2_blockref_t *bref)
{
	struct hammer2_open_row *r;
	uint64_t slot;
	int arrived = 1;

	if (!hammer2_raid6_bref_slot(hmp, bref, &slot))
		return 0;
	hammer2_spin_ex(&hmp->stripe_bitmap_spin);
	if (isset(hmp->stripe_open, slot)) {
		TAILQ_FOREACH(r, &hmp->open_rows, entry) {
			if (r->row_id != slot)
				continue;
			arrived = (r->col_data[bref->copyid] != NULL);
			break;
		}
	}
	hammer2_spin_unex(&hmp->stripe_bitmap_spin);
	return arrived;
}

/*
 * Putblk → open_row sink.  Takes ownership of `data` (kfree'd when
 * row seals).  If every reserved col then has its data and the row is
 * full, seal it; a complete row of an older generation wakes the flush.
 */
void
hammer2_raid6_open_row_add_data(hammer2_dev_t *hmp, hammer2_off_t phys_off,
				int disk_idx, void *data, size_t bytes,
				int dropped)
{
	struct hammer2_open_row *r;
	void *to_free = NULL;
	int wake = 0;

	/*
	 * Every chain that makes it through stripe_alloc has a row
	 * registered before its bp's putblk can fire add_data, and a
	 * row leaves the list only once all its reserved cols have
	 * arrived.  A miss means a chain bypassed the allocator's
	 * register path, or putblk'd again after its row sealed, and
	 * we'd otherwise silently drop its bytes from P/Q.
	 */
	KKASSERT(hmp->open_rows_inited);

	hammer2_spin_ex(&hmp->stripe_bitmap_spin);
	TAILQ_FOREACH(r, &hmp->open_rows, entry) {
		if (r->phys_off == phys_off)
			break;
	}
	if (r == NULL) {
		hammer2_spin_unex(&hmp->stripe_bitmap_spin);
		panic("hammer2: v3 putblk for a row not open: phys %016jx "
		      "disk %d", (uintmax_t)phys_off, disk_idx);
	}
	KKASSERT(r->bytes == bytes);
	KKASSERT(r->alloc_mask & (1U << disk_idx));

	/*
	 * A column is put once.  chain_modify gives a chain whose column
	 * has arrived a fresh slot (hammer2_raid6_col_arrived), so a second
	 * put here means a dio dirtied again without a modify; the latest
	 * copy wins, which is only right while the row stays open, and it
	 * is counted (vfs.hammer2.rz_put_anomaly).  kfree the old copy
	 * after the spinlock is released: it can sleep on the kernel map.
	 */
	to_free = r->col_data[disk_idx];
	if (to_free)
		hammer2_rz_put_anomaly++;
	r->col_data[disk_idx] = data;
	if (dropped)
		r->dropped_mask |= 1U << disk_idx;
	else
		r->dropped_mask &= ~(1U << disk_idx);

	/*
	 * A full row seals here.  A row of an older generation is sealed
	 * by the flush, which is told when its last column has arrived.
	 */
	if (hammer2_raid6_row_complete(hmp, r)) {
		if (r->gen != hmp->rz_gen && hmp->rz_gen_waiting)
			wake = 1;
		if (r->n_alloc >= r->ndata)
			hammer2_raid6_seal_row_locked_to_unlocked(hmp, r);
	}
	hammer2_spin_unex(&hmp->stripe_bitmap_spin);

	if (wake)
		wakeup(&hmp->open_rows);
	if (to_free)
		kfree(to_free, M_HAMMER2);
}

/*
 * Seal the open rows at a TXG flush, before VOP_FSYNC and the volume
 * header (oct-review.md §10.2).  Called after the last chain flush has
 * returned: every chain the header can commit was allocated before this
 * point, so its row is of the current generation or an older one.
 *
 * Advance rz_gen, which closes every existing row to new reservations
 * (packing takes only rows of the current generation), then seal each
 * older row once its reserved columns have all arrived.  A column is
 * pending only between the allocation and the put of one strategy
 * write, so the wait is short; the row is sealed in full, never with a
 * column missing.
 *
 * If no column arrives for vfs.hammer2.raid6_gen_wait_ms the flush
 * fails with EAGAIN: the caller must not write the volume header.  The
 * rows stay on the list and the next flush waits for them again.
 *
 * Each seal drops the spinlock, so the list is rescanned from the head
 * after every seal.
 */
int
hammer2_raid6_seal_all_open_rows(hammer2_dev_t *hmp)
{
	struct hammer2_open_row *r;
	struct hammer2_open_row *stuck;
	int deadline;
	int d;

	if (!hmp->open_rows_inited)
		return 0;

	hammer2_spin_ex(&hmp->stripe_bitmap_spin);
	hmp->rz_gen++;
	deadline = ticks + (int)((long)hz * hammer2_raid6_gen_wait_ms / 1000);
	for (;;) {
		stuck = NULL;
		TAILQ_FOREACH(r, &hmp->open_rows, entry) {
			if (r->gen == hmp->rz_gen)
				continue;
			if (hammer2_raid6_row_complete(hmp, r))
				break;
			if (stuck == NULL)
				stuck = r;
		}
		if (r != NULL) {
			hammer2_raid6_seal_row_locked_to_unlocked(hmp, r);
			/* progress: the wait starts over */
			deadline = ticks +
			    (int)((long)hz * hammer2_raid6_gen_wait_ms / 1000);
			continue;
		}
		if (stuck == NULL)
			break;
		if ((int)(ticks - deadline) >= 0) {
			uint32_t pend = 0;
			uint64_t row_id = stuck->row_id;
			hammer2_off_t phys_off = stuck->phys_off;
			uint32_t gen = stuck->gen;
			uint32_t now = hmp->rz_gen;

			for (d = 0; d < hmp->raid_config.ndisks; d++) {
				if ((stuck->alloc_mask & (1U << d)) &&
				    stuck->col_data[d] == NULL)
					pend |= 1U << d;
			}
			hammer2_rz_gen_wait_fail++;
			hammer2_spin_unex(&hmp->stripe_bitmap_spin);
			kprintf("hammer2: v3 flush failed: row %ju phys %016jx "
				"gen %u (now %u) still waits for columns "
				"%08x after %d ms\n",
				(uintmax_t)row_id, (uintmax_t)phys_off, gen,
				now, pend, hammer2_raid6_gen_wait_ms);
			return EAGAIN;
		}
		hmp->rz_gen_waiting++;
		ssleep(&hmp->open_rows, &hmp->stripe_bitmap_spin, 0,
		       "h2rzgen", hz / 10 ? hz / 10 : 1);
		hmp->rz_gen_waiting--;
	}
	hammer2_rz_open_left = hmp->open_rows_count;
	hammer2_spin_unex(&hmp->stripe_bitmap_spin);
	return 0;
}

/*
 * Return 1 if stripe slot `slot` may hold a parity row.  Everything up
 * to the end of metadata extent 0 (space map, aux area, metadata zone),
 * the reserved segment at the start of every 2 GB zone (volume header
 * backups, freemap blocks) and any further metadata extent hold per-disk
 * or mirrored data instead (docs/capacity.md): the allocator never
 * places a row there, and the resilver must not rebuild one there from
 * parity.
 */
int
hammer2_raid6_slot_is_data(hammer2_dev_t *hmp, uint64_t slot)
{
	uint64_t stripe_unit = hmp->raid_config.stripe_unit;
	hammer2_off_t off = HAMMER2_ZONE_SEG64 + slot * stripe_unit;
	hammer2_off_t zoff = off & HAMMER2_ZONE_MASK64;
	uint32_t ex;

	if (slot >= hmp->stripe_num_slots)
		return 0;
	if (zoff < HAMMER2_ZONE_SEG64 ||
	    zoff + stripe_unit > HAMMER2_ZONE_BYTES64)
		return 0;
	if (off < hmp->md_extents[0].md_off + hmp->md_extents[0].md_size)
		return 0;
	for (ex = 1; ex < hmp->md_nextents; ex++) {
		hammer2_off_t mo = hmp->md_extents[ex].md_off;
		hammer2_off_t ms = hmp->md_extents[ex].md_size;
		if (off + stripe_unit > mo && off < mo + ms)
			return 0;
	}
	return 1;
}

/*
 * Usable size and free space of a v3 array for statfs.  The freemap only
 * covers metadata on v3 (data rows live in the stripe bitmap), and it
 * spans the raw size of every disk, so allocator_size/free mean nothing
 * to the user.  Report the rows (ndata columns each) plus the metadata
 * zone, counted once because it is mirrored.  A partly packed row counts
 * as full, so free space is never overstated.
 */
void
hammer2_raid6_space(hammer2_dev_t *hmp, hammer2_off_t *totalp,
		    hammer2_off_t *freep)
{
	hammer2_off_t row_bytes;
	hammer2_off_t md_size = 0;
	hammer2_off_t md_used;
	uint64_t slot;
	uint64_t used;
	uint32_t ex;
	size_t i;

	row_bytes = hmp->raid_config.stripe_unit * hmp->raid_config.ndata;
	for (ex = 0; ex < hmp->md_nextents; ex++)
		md_size += hmp->md_extents[ex].md_size;

	if (hmp->stripe_bitmap == NULL) {
		*totalp = md_size;
		*freep = 0;
		return;
	}
	if (hmp->stripe_data_slots == 0) {
		for (slot = 0; slot < hmp->stripe_num_slots; slot++) {
			if (hammer2_raid6_slot_is_data(hmp, slot))
				++hmp->stripe_data_slots;
		}
	}

	/*
	 * Counting is unlocked; a racing allocation only makes the figure
	 * slightly stale.  Recount at most once a second.
	 */
	if (hmp->stripe_used_ticks == 0 ||
	    (u_int)(ticks - hmp->stripe_used_ticks) >= (u_int)hz) {
		used = 0;
		for (i = 0; i < hmp->stripe_bitmap_size; i++)
			used += bitcount32(hmp->stripe_bitmap[i]);
		hmp->stripe_used_slots = used;
		hmp->stripe_used_ticks = ticks ? ticks : 1;
	}
	used = hmp->stripe_used_slots;
	if (used > hmp->stripe_data_slots)
		used = hmp->stripe_data_slots;

	/*
	 * Every freemap allocation on v3 is metadata.
	 */
	md_used = hmp->voldata.allocator_size - hmp->voldata.allocator_free;
	if (md_used > md_size)
		md_used = md_size;

	*totalp = hmp->stripe_data_slots * row_bytes + md_size;
	*freep = (hmp->stripe_data_slots - used) * row_bytes +
		 (md_size - md_used);
}

/*
 * Allocate a physical stripe slot for a RAIDZ2-native (v3) data column.
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
	uint64_t byte_idx;
	int bit_idx;
	int disk_idx;
	hammer2_off_t phys_off;
	struct hammer2_open_row *newrow;
	int radix;
	int n;

	/*
	 * The open_rows entry for a new row, allocated here because
	 * kmalloc(M_WAITOK) may sleep and the scan holds a spinlock.
	 */
	newrow = kmalloc(sizeof(*newrow), M_HAMMER2, M_WAITOK | M_ZERO);

	/*
	 * 6C packing: try to land in an existing open row first so a row
	 * can be filled with up to ndata chains before P/Q is computed.
	 * Bisect knob: hammer2_raid6_pack_open_rows=0 disables packing
	 * (single-chain-per-row preserved), leaving only the deferred-P/Q
	 * seal hook from 6C-2 — useful for diagnosing whether a failure
	 * comes from packing or from the timing change.
	 */
	hammer2_spin_ex(&hmp->stripe_bitmap_spin);

	if (hammer2_raid6_pack_open_rows) {
		hammer2_off_t pack_off;
		int pack_disk_idx;

		if (hammer2_raid6_open_row_pack_locked(hmp, &pack_off,
		    &pack_disk_idx)) {
			int prc_radix;
			int prc_n = chain->bytes;

			/*
			 * One more live chain in the row: freeing any one
			 * of them must not free the row.
			 */
			slot = (pack_off - HAMMER2_ZONE_SEG64) / stripe_unit;
			if (hmp->stripe_row_refcount[slot] < 0xff)
				hmp->stripe_row_refcount[slot]++;
			hammer2_raid6_sm_touch(hmp, slot);
			if (hmp->stripe_bf_active)
				setbit(hmp->stripe_bf_new, slot);
			hammer2_spin_unex(&hmp->stripe_bitmap_spin);
			kfree(newrow, M_HAMMER2);
			prc_radix = 0;
			while (prc_n > 1) { prc_n >>= 1; prc_radix++; }
			chain->bref.data_off =
			    pack_off | (hammer2_off_t)prc_radix;
			chain->bref.copyid   = (uint8_t)pack_disk_idx;
			return 0;
		}
	}

	start = hmp->stripe_cursor;
	if (start >= max_stripes)
		start = 0;
	slot = start;

	for (;;) {
		if (slot >= max_stripes) {
			if (wrapped)
				break;
			wrapped = 1;
			slot = 0;
			if (slot >= start)
				break;
			continue;
		}
		if (wrapped && slot >= start)
			break;

		if (!hammer2_raid6_slot_is_data(hmp, slot)) {
			slot++;
			continue;
		}

		byte_idx = slot / 8;
		bit_idx  = (int)(slot % 8);
		if ((hmp->stripe_bitmap[byte_idx] & (1 << bit_idx)) == 0) {
			hmp->stripe_bitmap[byte_idx] |= (uint8_t)(1 << bit_idx);
			hmp->stripe_row_refcount[slot] = 1;
			hammer2_raid6_sm_touch(hmp, slot);
			if (hmp->stripe_bf_active)
				setbit(hmp->stripe_bf_new, slot);
			hmp->stripe_cursor = slot + 1;
			goto found;
		}
		slot++;
	}
	hammer2_spin_unex(&hmp->stripe_bitmap_spin);
	kfree(newrow, M_HAMMER2);
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
		hmp->stripe_row_refcount[slot] = 0;
		hammer2_spin_unex(&hmp->stripe_bitmap_spin);
		kfree(newrow, M_HAMMER2);
		return ENOSPC;
	}
have_disk:
	phys_off = HAMMER2_ZONE_SEG64 + slot * stripe_unit;
	/*
	 * Register the newly-allocated row so subsequent stripe_alloc
	 * calls within the same TXG can pack into it.  Done while still
	 * holding stripe_bitmap_spin.
	 */
	hammer2_raid6_open_row_register_locked(hmp, newrow, slot, phys_off,
					       disk_idx);
	hammer2_spin_unex(&hmp->stripe_bitmap_spin);

	/* Compute radix (log2 of chain->bytes) */
	radix = 0;
	n = chain->bytes;
	while (n > 1) { n >>= 1; radix++; }

	/*
	 * v3 encoding: bref.data_off holds only per-disk phys_off | radix.
	 * Disk identity lives exclusively in bref.copyid.  The DIO cache
	 * synthesizes a (disk_idx<<56)|phys_off key at lookup time to keep
	 * per-disk entries distinct.
	 */
	chain->bref.data_off = phys_off | (hammer2_off_t)radix;
	chain->bref.copyid   = (uint8_t)disk_idx;

	return 0;
}

/*
 * v3 bulkfree (docs/capacity.md, Freeing).
 *
 * Data slots are never freed when a chain is modified or deleted: a
 * snapshot may still reference the old block, and the tree that drops
 * it may not be committed yet.  Instead the bulkfree ioctl scans a
 * snapshot of the committed topology, every PFS and snapshot included,
 * and hammer2_raid6_bf_mark() records each data slot it references.
 *
 * Flushes do not wait for frontend transactions, so a slot allocated
 * just before the pass may be in no committed tree yet.  Freeing is
 * therefore two-stage, as in the freemap: a slot that a pass finds
 * unreferenced is staged, and freed by the next pass if that one finds
 * it unreferenced too.  A slot allocated while a pass runs
 * (stripe_bf_new) is kept and unstaged.  The bulkfree ioctl syncs
 * before every pass, so the two scans see different commits.
 *
 * The staged set is in memory only; after a remount the first pass
 * stages again.
 */
void
hammer2_raid6_bf_begin(hammer2_dev_t *hmp)
{
	bzero(hmp->stripe_bf_seen, hmp->stripe_bitmap_size);
	hammer2_spin_ex(&hmp->stripe_bitmap_spin);
	bzero(hmp->stripe_bf_new, hmp->stripe_bitmap_size);
	hmp->stripe_bf_active = 1;
	hmp->stripe_bf_complete = 0;
	hammer2_spin_unex(&hmp->stripe_bitmap_spin);
}

/*
 * Scan callback: record a referenced data slot and return 1, or return
 * 0 if the blockref is not a v3 data slot.  Only the scan writes
 * stripe_bf_seen.
 */
int
hammer2_raid6_bf_mark(hammer2_dev_t *hmp, const hammer2_blockref_t *bref)
{
	uint64_t slot;

	if (!hammer2_raid6_bref_slot(hmp, bref, &slot))
		return 0;
	setbit(hmp->stripe_bf_seen, slot);
	return 1;
}

/*
 * End a pass.  If the scan covered the whole committed tree
 * (stripe_bf_complete, set by hammer2_bulkfree_pass), apply it to the
 * space map one page at a time.
 */
void
hammer2_raid6_bf_end(hammer2_dev_t *hmp)
{
	uint64_t nslots = hmp->stripe_num_slots;
	uint64_t base;
	uint64_t slot;
	uint64_t end;
	long nfreed = 0;
	long nstaged = 0;
	long nfixed = 0;

	if (hmp->stripe_bf_complete == 0) {
		hammer2_spin_ex(&hmp->stripe_bitmap_spin);
		hmp->stripe_bf_active = 0;
		hammer2_spin_unex(&hmp->stripe_bitmap_spin);
		kprintf("hammer2: v3 bulkfree: scan incomplete, "
			"space map unchanged\n");
		return;
	}

	for (base = 0; base < nslots; base += HAMMER2_SM_PAGE_SLOTS) {
		end = base + HAMMER2_SM_PAGE_SLOTS;
		if (end > nslots)
			end = nslots;
		hammer2_spin_ex(&hmp->stripe_bitmap_spin);
		for (slot = base; slot < end; slot++) {
			uint8_t *rc = &hmp->stripe_row_refcount[slot];

			/*
			 * A row that is still open has columns in flight
			 * that no committed tree references yet: neither
			 * free it nor stage it.
			 */
			if (isset(hmp->stripe_open, slot))
				continue;
			if (isset(hmp->stripe_bf_seen, slot) ||
			    isset(hmp->stripe_bf_new, slot)) {
				clrbit(hmp->stripe_bf_staged, slot);
				/*
				 * Referenced by the committed tree but
				 * free in the map: take it back.
				 */
				if (*rc == 0 &&
				    isclr(hmp->stripe_bf_new, slot) &&
				    hammer2_raid6_slot_is_data(hmp, slot)) {
					*rc = 1;
					setbit(hmp->stripe_bitmap, slot);
					hammer2_raid6_sm_touch(hmp, slot);
					++nfixed;
				}
				continue;
			}
			if (*rc == 0) {
				clrbit(hmp->stripe_bf_staged, slot);
				continue;
			}
			if (isset(hmp->stripe_bf_staged, slot)) {
				*rc = 0;
				clrbit(hmp->stripe_bitmap, slot);
				clrbit(hmp->stripe_bf_staged, slot);
				hammer2_raid6_sm_touch(hmp, slot);
				++nfreed;
			} else {
				setbit(hmp->stripe_bf_staged, slot);
				++nstaged;
			}
		}
		hammer2_spin_unex(&hmp->stripe_bitmap_spin);
	}
	hammer2_spin_ex(&hmp->stripe_bitmap_spin);
	hmp->stripe_bf_active = 0;
	hmp->stripe_used_ticks = 0;
	hammer2_spin_unex(&hmp->stripe_bitmap_spin);

	kprintf("hammer2: v3 bulkfree: %ld rows freed, %ld staged, "
		"%ld reclaimed\n", nfreed, nstaged, nfixed);
}
