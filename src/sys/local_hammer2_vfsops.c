/*
 * Copyright (c) 2011-2023 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com>
 * by Daniel Flores (GSOC 2013 - mentored by Matthew Dillon, compression)
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
#include <sys/nlookup.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/fcntl.h>
#include <sys/vfsops.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <sys/objcache.h>
#include <sys/proc.h>
#include <sys/lock.h>
#include <sys/file.h>

#include <sys/diskslice.h>
#include <sys/disk.h>
#include <sys/taskqueue.h>

#include "hammer2.h"
#include "hammer2_raid6.h"

#ifndef DIOCGFLUSHCAP
#define	DIOCGFLUSHCAP		_IOR('d', 137, int)	/* 0: flushes dropped */
#endif

TAILQ_HEAD(hammer2_mntlist, hammer2_dev);
static struct hammer2_mntlist hammer2_mntlist;

struct hammer2_pfslist hammer2_pfslist;
struct hammer2_pfslist hammer2_spmplist;
struct lock hammer2_mntlk;

int hammer2_supported_version = HAMMER2_VOL_VERSION_DEFAULT;
int hammer2_debug;
int hammer2_aux_flags;
int hammer2_xop_nthreads;
int hammer2_xop_sgroups;
int hammer2_xop_xgroups;
int hammer2_xop_xbase;
int hammer2_xop_mod;
long hammer2_debug_inode;
int hammer2_cluster_meta_read = 1;	/* physical read-ahead */
int hammer2_md_3copy_upgrade;		/* mount: layout 2 -> 3 copies */
int hammer2_md_read_spread = 1;		/* v3: cold metadata reads use all mirrors */
int hammer2_cluster_data_read = 4;	/* physical read-ahead */
int hammer2_cluster_write = 0;		/* physical write clustering */
int hammer2_dedup_enable = 1;
int hammer2_always_compress = 0;	/* always try to compress */
int hammer2_flush_pipe = 100;
int hammer2_dio_count;
int hammer2_dio_limit = 256;
int hammer2_bulkfree_tps = 5000;
int hammer2_spread_workers;
int hammer2_limit_saved_depth;
int hammer2_j2_allow_rollback = 0;	/* v3 quorum: allow rollback mount */
int hammer2_j2_rollback_max = 8;	/* v3 quorum: max TXG rollback */
uint32_t hammer2_inject_eio_disk_mask = 0; /* v3 EIO injection bitmask */
int hammer2_inject_eio_autofail = 0;	/* injected read EIO fails the disk */
uint32_t hammer2_inject_wio_disk_mask = 0; /* write EIO injection bitmask */
uint32_t hammer2_inject_delay_ms_disk_mask = 0; /* late-I/O injection bitmask */
int hammer2_inject_delay_ms = 3000;	/* ... by this many ms */
uint32_t hammer2_inject_hold_ms_disk_mask = 0; /* late-completion injection */
int hammer2_inject_hold_ms = 8000;	/* ... by this many ms */
int hammer2_inject_mount_fail = 0;	/* fail this mount point, once */
int hammer2_io_timeout_ms = 30000;	/* fail a member silent this long */
int hammer2_raid6_resilver_skip_unalloc = 1;	/* v3: skip unalloc slots */
int hammer2_raid6_resilver_ckpt_slots = 2048;	/* slots between cursor commits */
uint64_t hammer2_raid6_resilver_md_bytes;	/* last resilver: metadata copied */
uint64_t hammer2_raid6_resilver_resume_slot;	/* last resilver: cursor resumed at */
int hammer2_raid6_resilver_stop_slots;		/* test hook: stop after this many slots */
int hammer2_raid6_put_stall_ms;	/* test hook: stall one write before its put */
int hammer2_raid6_subcol_max = 32768;	/* v3: chains up to this size share a column (0 = off) */
int hammer2_raid6_subcol_test;	/* test hook: place chains inside their column */
int hammer2_raid6_gen_wait_ms = 5000;	/* v3: flush wait for pending columns */
long hammer2_rz_open_left;
long hammer2_rz_open_high;
long hammer2_rz_rewrite_slots;
long hammer2_rz_resize_inplace;
long hammer2_rz_cols_alloc;
long hammer2_p10_mf_sync;
long hammer2_p10_mf_maxpend;
long hammer2_p10_mf_blocks;
long hammer2_p10_mf_defers;
long hammer2_p10_dc_trigger;
long hammer2_p10_dc_stall;
long hammer2_rz_put_anomaly;
long hammer2_rz_col_released;
long hammer2_rz_gen_wait_fail;
long hammer2_rz_seal_full;
long hammer2_rz_seal_flush;
long hammer2_rz_seal_flush_short;
long hammer2_rz_pq_wait_ns;
int hammer2_async_seal = 1;		/* P3 phase 2: seal without waiting for P/Q */
int hammer2_async_seal_max = 16;	/* ... rows with parity in flight */
int hammer2_inject_pq_defer_ms = 0;	/* test: start parity writes this late */
long hammer2_rz_pq_err;
long hammer2_rz_pq_async;
long hammer2_rz_pq_bp_wait;
long hammer2_rz_pq_rd_wait;
long hammer2_rz_pq_drain_wait;
long hammer2_rz_pq_high;
long hammer2_rz_pq_hdr_viol;
long hammer2_rz_pq_wait_cnt;
long hammer2_rz_seal_ns;
long hammer2_rz_seal_cnt;
long hammer2_cw_cnt;
long hammer2_cw_zero_ns;
long hammer2_cw_lz4_cnt;
long hammer2_cw_lz4_ns;
long hammer2_cw_assign_ns;
long hammer2_cw_done_ns;
long hammer2_cw_io_ns;
long hammer2_cw_wait_ns;
long hammer2_cw_pool_jobs;
long hammer2_cw_pool_inline;
int hammer2_comp_workers = -1;		/* -1: ncpus capped at 4 (set at init) */
u_int hammer2_comp_depth = 128;	/* > hammer2_flush_pipe (100) */
u_int hammer2_comp_inflight;
long hammer2_rz_bf_rows_freed;
long hammer2_rz_bf_reclaimed;
long hammer2_rz_bf_rows_live;
long hammer2_rz_bf_cols_live;
long hammer2_rz_bf_cols_dead;
long hammer2_rz_subcol_open;
long hammer2_rz_subcol_close;
long hammer2_rz_subcol_chains;
long hammer2_rz_subcol_nofit;
long hammer2_rz_subcol_inplace;
long hammer2_rz_subcol_stale;
long hammer2_scrub_pq_rows;
long hammer2_scrub_pq_bad;
long hammer2_scrub_pq_repaired;
long hammer2_scrub_pq_unloc;
long hammer2_scrub_pq_skipped;
long hammer2_chain_allocs;
long hammer2_limit_saved_chains;
long hammer2_limit_dirty_chains;
long hammer2_limit_dirty_inodes;
long hammer2_count_modified_chains;
long hammer2_iod_file_read;
long hammer2_iod_meta_read;
long hammer2_iod_indr_read;
long hammer2_iod_fmap_read;
long hammer2_iod_volu_read;
long hammer2_iod_file_write;
long hammer2_iod_file_wembed;
long hammer2_iod_file_wzero;
long hammer2_iod_file_wdedup;
long hammer2_iod_meta_write;
long hammer2_iod_indr_write;
long hammer2_iod_fmap_write;
long hammer2_iod_volu_write;
static long hammer2_iod_inode_creates;
static long hammer2_iod_inode_deletes;

long hammer2_process_icrc32;
long hammer2_process_xxhash64;

MALLOC_DECLARE(M_HAMMER2_CBUFFER);
MALLOC_DEFINE(M_HAMMER2_CBUFFER, "HAMMER2-compbuffer",
		"Buffer used for compression.");

MALLOC_DECLARE(M_HAMMER2_DEBUFFER);
MALLOC_DEFINE(M_HAMMER2_DEBUFFER, "HAMMER2-decompbuffer",
		"Buffer used for decompression.");

SYSCTL_NODE(_vfs, OID_AUTO, hammer2, CTLFLAG_RW, 0, "HAMMER2 filesystem");

SYSCTL_INT(_vfs_hammer2, OID_AUTO, supported_version, CTLFLAG_RD,
	   &hammer2_supported_version, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, aux_flags, CTLFLAG_RW,
	   &hammer2_aux_flags, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, debug, CTLFLAG_RW,
	   &hammer2_debug, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, debug_inode, CTLFLAG_RW,
	   &hammer2_debug_inode, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, spread_workers, CTLFLAG_RW,
	   &hammer2_spread_workers, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, cluster_meta_read, CTLFLAG_RW,
	   &hammer2_cluster_meta_read, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, md_3copy_upgrade, CTLFLAG_RW,
	   &hammer2_md_3copy_upgrade, 0,
	   "Mount converts a layout 2 (all-disk metadata mirror) array to 3 "
	   "copies; older kernels then refuse it");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, md_read_spread, CTLFLAG_RW,
	   &hammer2_md_read_spread, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, cluster_data_read, CTLFLAG_RW,
	   &hammer2_cluster_data_read, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, cluster_write, CTLFLAG_RW,
	   &hammer2_cluster_write, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, dedup_enable, CTLFLAG_RW,
	   &hammer2_dedup_enable, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, always_compress, CTLFLAG_RW,
	   &hammer2_always_compress, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, flush_pipe, CTLFLAG_RW,
	   &hammer2_flush_pipe, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, bulkfree_tps, CTLFLAG_RW,
	   &hammer2_bulkfree_tps, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, j2_allow_rollback, CTLFLAG_RW,
	   &hammer2_j2_allow_rollback, 0,
	   "v3 RAID6: allow mount to roll back when no majority at "
	   "the latest TXG (data between latest and majority is lost)");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, j2_rollback_max, CTLFLAG_RW,
	   &hammer2_j2_rollback_max, 0,
	   "v3 RAID6: maximum TXGs to search backwards for a "
	   "majority-supported seqno before giving up");
SYSCTL_UINT(_vfs_hammer2, OID_AUTO, inject_eio_disk_mask, CTLFLAG_RW,
	   &hammer2_inject_eio_disk_mask, 0,
	   "v3 RAID6 fault injection: bitmask of disk indices to force "
	   "EIO on (no auto-fail).  Default 0 (disabled).");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, inject_eio_autofail, CTLFLAG_RW,
	   &hammer2_inject_eio_autofail, 0,
	   "v3 RAID6 fault injection: 1 = an injected read EIO fails the "
	   "disk like a real one (persisted).  Default 0.");
SYSCTL_UINT(_vfs_hammer2, OID_AUTO, inject_wio_disk_mask, CTLFLAG_RW,
	   &hammer2_inject_wio_disk_mask, 0,
	   "v3 RAID6 fault injection: bitmask of disk indices whose writes "
	   "(data, P/Q, mirror, header) complete with EIO.  Default 0.");
SYSCTL_UINT(_vfs_hammer2, OID_AUTO, inject_delay_ms_disk_mask, CTLFLAG_RW,
	   &hammer2_inject_delay_ms_disk_mask, 0,
	   "v3 RAID6 fault injection: bitmask of disk indices whose reads "
	   "and synchronous writes are issued inject_delay_ms late.");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, inject_delay_ms, CTLFLAG_RW,
	   &hammer2_inject_delay_ms, 0,
	   "v3 RAID6 fault injection: delay in ms (default 3000).");
SYSCTL_UINT(_vfs_hammer2, OID_AUTO, inject_hold_ms_disk_mask, CTLFLAG_RW,
	   &hammer2_inject_hold_ms_disk_mask, 0,
	   "v3 RAID6 fault injection: bitmask of disk indices whose timed "
	   "bios (P/Q, header, flush, space map) complete inject_hold_ms "
	   "late, like a driver that returns a command after the waiter "
	   "gave up.");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, inject_hold_ms, CTLFLAG_RW,
	   &hammer2_inject_hold_ms, 0,
	   "v3 RAID6 fault injection: completion hold in ms (default 8000).");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, inject_mount_fail, CTLFLAG_RW,
	   &hammer2_inject_mount_fail, 0,
	   "Test only: make the Nth failure point of the next first mount "
	   "of a device return an error (once; see hammer2_mount_inject()).");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, io_timeout_ms, CTLFLAG_RW,
	   &hammer2_io_timeout_ms, 0,
	   "RAID6: a member with I/O outstanding that completes none for "
	   "this many ms is failed, and a synchronous write that does not "
	   "complete in this time is abandoned (0 = never)");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, resilver_skip_unalloc, CTLFLAG_RW,
	   &hammer2_raid6_resilver_skip_unalloc, 0,
	   "v3 RAID6: skip unallocated stripe slots during resilver "
	   "(default 1).  Set 0 to force full-iteration baseline for "
	   "regression timing.");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, resilver_ckpt_slots, CTLFLAG_RW,
	   &hammer2_raid6_resilver_ckpt_slots, 0,
	   "v3 RAID6: commit the resilver resume cursor every this many "
	   "stripe slots (0 = never)");
SYSCTL_QUAD(_vfs_hammer2, OID_AUTO, resilver_md_bytes, CTLFLAG_RD,
	   &hammer2_raid6_resilver_md_bytes, 0,
	   "v3 RAID6: bytes of metadata the last resilver copied (Phase A)");
SYSCTL_QUAD(_vfs_hammer2, OID_AUTO, resilver_resume_slot, CTLFLAG_RD,
	   &hammer2_raid6_resilver_resume_slot, 0,
	   "v3 RAID6: stripe slot the last resilver resumed at (0 = fresh)");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, resilver_stop_slots, CTLFLAG_RW,
	   &hammer2_raid6_resilver_stop_slots, 0,
	   "test hook: the resilver stops with EINTR after this many "
	   "slots (0 = off), as a crash would");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, raid6_put_stall_ms, CTLFLAG_RW,
	   &hammer2_raid6_put_stall_ms, 0,
	   "test hook: stall the next v3 data write (ms) between reserve and put");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, raid6_subcol_max, CTLFLAG_RW,
	   &hammer2_raid6_subcol_max, 0,
	   "v3 RAID6: data chains up to this many bytes (a power of two, "
	   "1024-32768) share a column; 0 = every chain takes its own column");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, raid6_subcol_test, CTLFLAG_RW,
	   &hammer2_raid6_subcol_test, 0,
	   "test hook: place new v3 data chains smaller than 64 KB at a "
	   "non-zero offset inside their private column (1 = end of the "
	   "column, 2 = varied); 0 = offset 0");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, raid6_gen_wait_ms, CTLFLAG_RW,
	   &hammer2_raid6_gen_wait_ms, 0,
	   "v3 RAID6: how long (ms, without progress) a flush waits for the "
	   "pending columns of its rows before it fails");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_open_left, CTLFLAG_RD,
	   &hammer2_rz_open_left, 0,
	   "v3 RAID6: rows still open after the last flush");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_open_high, CTLFLAG_RD,
	   &hammer2_rz_open_high, 0, "v3 RAID6: peak open rows");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_rewrite_slots, CTLFLAG_RD,
	   &hammer2_rz_rewrite_slots, 0,
	   "v3 RAID6: same-TXG rewrites that took a fresh slot");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_resize_inplace, CTLFLAG_RD,
	   &hammer2_rz_resize_inplace, 0,
	   "v3 RAID6: resizes that only changed the radix of the column");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, p10_mf_sync, CTLFLAG_RD,
	   &hammer2_p10_mf_sync, 0,
	   "mirror flushes by the sync (TXG flush passes)");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, p10_mf_maxpend, CTLFLAG_RD,
	   &hammer2_p10_mf_maxpend, 0,
	   "mirror flushes forced early by MIRROR_MAXPEND");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, p10_mf_blocks, CTLFLAG_RD,
	   &hammer2_p10_mf_blocks, 0,
	   "mirror blocks written by flushes (each to N-1 disks)");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, p10_mf_defers, CTLFLAG_RD,
	   &hammer2_p10_mf_defers, 0,
	   "metadata mirror copies queued by putblk");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, p10_dc_trigger, CTLFLAG_RD,
	   &hammer2_p10_dc_trigger, 0,
	   "syncer triggers by the dirty-chain/inode limit (half)");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, p10_dc_stall, CTLFLAG_RD,
	   &hammer2_p10_dc_stall, 0,
	   "frontend stalls at the dirty-chain/inode limit");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_cols_alloc, CTLFLAG_RD,
	   &hammer2_rz_cols_alloc, 0,
	   "v3 RAID6: columns reserved by stripe_alloc");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_put_anomaly, CTLFLAG_RD,
	   &hammer2_rz_put_anomaly, 0,
	   "v3 RAID6: second put on an arrived column");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_col_released, CTLFLAG_RD,
	   &hammer2_rz_col_released, 0,
	   "v3 RAID6: reservations released as zero columns after a failed write");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_gen_wait_fail, CTLFLAG_RD,
	   &hammer2_rz_gen_wait_fail, 0,
	   "v3 RAID6: flushes failed by a row-generation wait");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_seal_full, CTLFLAG_RD,
	   &hammer2_rz_seal_full, 0,
	   "v3 RAID6: rows sealed by the put that filled them");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_seal_flush, CTLFLAG_RD,
	   &hammer2_rz_seal_flush, 0, "v3 RAID6: rows sealed by a flush");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_bf_rows_freed, CTLFLAG_RD,
	   &hammer2_rz_bf_rows_freed, 0, "v3 RAID6: rows freed by bulkfree");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_bf_reclaimed, CTLFLAG_RD,
	   &hammer2_rz_bf_reclaimed, 0,
	   "v3 RAID6: referenced rows bulkfree found free (must be 0)");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_bf_rows_live, CTLFLAG_RD,
	   &hammer2_rz_bf_rows_live, 0,
	   "v3 RAID6: rows the last bulkfree pass found referenced");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_bf_cols_live, CTLFLAG_RD,
	   &hammer2_rz_bf_cols_live, 0,
	   "v3 RAID6: columns with a live chain, last bulkfree pass");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_bf_cols_dead, CTLFLAG_RD,
	   &hammer2_rz_bf_cols_dead, 0,
	   "v3 RAID6: dead columns held in referenced rows, last pass");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_subcol_open, CTLFLAG_RD,
	   &hammer2_rz_subcol_open, 0, "v3 RAID6: sub-columns opened");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_subcol_close, CTLFLAG_RD,
	   &hammer2_rz_subcol_close, 0,
	   "v3 RAID6: sub-columns closed (pin dropped, column put)");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_subcol_chains, CTLFLAG_RD,
	   &hammer2_rz_subcol_chains, 0,
	   "v3 RAID6: chains placed in a sub-column");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_subcol_nofit, CTLFLAG_RD,
	   &hammer2_rz_subcol_nofit, 0,
	   "v3 RAID6: sub-columns closed because a chain found no aligned range");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_subcol_inplace, CTLFLAG_RD,
	   &hammer2_rz_subcol_inplace, 0,
	   "v3 RAID6: rewrites that kept their range in an open sub-column");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_subcol_stale, CTLFLAG_RD,
	   &hammer2_rz_subcol_stale, 0,
	   "v3 RAID6: sub-column tickets released by a safety net (a leak)");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, scrub_pq_rows, CTLFLAG_RD,
	   &hammer2_scrub_pq_rows, 0,
	   "scrub parity pass: rows whose P and Q were recomputed (cumulative)");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, scrub_pq_bad, CTLFLAG_RD,
	   &hammer2_scrub_pq_bad, 0,
	   "scrub parity pass: rows where P or Q disagreed");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, scrub_pq_repaired, CTLFLAG_RD,
	   &hammer2_scrub_pq_repaired, 0,
	   "scrub parity pass: inconsistent rows rewritten");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, scrub_pq_unlocatable, CTLFLAG_RD,
	   &hammer2_scrub_pq_unloc, 0,
	   "scrub parity pass: inconsistent rows left alone (no single bad "
	   "column, checksum conflict, or repair dropped)");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, scrub_pq_skipped, CTLFLAG_RD,
	   &hammer2_scrub_pq_skipped, 0,
	   "scrub parity pass: rows skipped (open, busy, unreadable)");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, async_seal, CTLFLAG_RW,
	   &hammer2_async_seal, 0,
	   "v3 RAID6: seal rows without the writer waiting for P/Q (oct-review "
	   "P3 phase 2); 0 = the writer waits for P/Q, as before");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, async_seal_max, CTLFLAG_RW,
	   &hammer2_async_seal_max, 0,
	   "v3 RAID6: rows with parity in flight before a writer waits");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, inject_pq_defer_ms, CTLFLAG_RW,
	   &hammer2_inject_pq_defer_ms, 0,
	   "Test: with async_seal the reaper starts the P/Q/zero writes this "
	   "many ms after the row was sealed (media holds old parity)");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_pq_err, CTLFLAG_RW,
	   &hammer2_rz_pq_err, 0, "v3 RAID6: parity writes that failed (reaper)");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_pq_async, CTLFLAG_RW,
	   &hammer2_rz_pq_async, 0, "v3 RAID6: rows handed to the parity reaper");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_pq_bp_wait, CTLFLAG_RW,
	   &hammer2_rz_pq_bp_wait, 0, "v3 RAID6: writers held by async back-pressure");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_pq_rd_wait, CTLFLAG_RW,
	   &hammer2_rz_pq_rd_wait, 0, "v3 RAID6: parity reads that waited for a row in flight");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_pq_drain_wait, CTLFLAG_RW,
	   &hammer2_rz_pq_drain_wait, 0, "v3 RAID6: drains that had to wait");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_pq_high, CTLFLAG_RW,
	   &hammer2_rz_pq_high, 0, "v3 RAID6: peak rows with parity in flight");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_pq_hdr_viol, CTLFLAG_RW,
	   &hammer2_rz_pq_hdr_viol, 0,
	   "v3 RAID6: volume headers written with parity in flight (must stay 0)");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_pq_wait_ns, CTLFLAG_RW,
	   &hammer2_rz_pq_wait_ns, 0, "v3 RAID6: ns waiting for P/Q writes at seal");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_pq_wait_cnt, CTLFLAG_RW,
	   &hammer2_rz_pq_wait_cnt, 0, "v3 RAID6: seals that waited for P/Q");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_seal_ns, CTLFLAG_RW,
	   &hammer2_rz_seal_ns, 0, "v3 RAID6: ns inside row seal I/O, total");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_seal_cnt, CTLFLAG_RW,
	   &hammer2_rz_seal_cnt, 0, "v3 RAID6: rows sealed (timed)");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, cw_cnt, CTLFLAG_RW,
	   &hammer2_cw_cnt, 0, "write path: blocks through compress_and_write");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, cw_zero_ns, CTLFLAG_RW,
	   &hammer2_cw_zero_ns, 0, "write path: ns in the zero-block test");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, cw_lz4_cnt, CTLFLAG_RW,
	   &hammer2_cw_lz4_cnt, 0, "write path: compression attempts");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, cw_lz4_ns, CTLFLAG_RW,
	   &hammer2_cw_lz4_ns, 0, "write path: ns compressing");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, cw_assign_ns, CTLFLAG_RW,
	   &hammer2_cw_assign_ns, 0, "write path: ns in assign_physical");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, cw_io_ns, CTLFLAG_RW,
	   &hammer2_cw_io_ns, 0, "write path: ns filling and queueing the buffer");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, cw_wait_ns, CTLFLAG_RW,
	   &hammer2_cw_wait_ns, 0, "write path: ns the xop thread waited for a compression worker");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, cw_pool_jobs, CTLFLAG_RW,
	   &hammer2_cw_pool_jobs, 0, "write path: blocks pre-compressed by the worker pool");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, cw_pool_inline, CTLFLAG_RW,
	   &hammer2_cw_pool_inline, 0, "write path: LZ4 candidates compressed inline (pool off/full)");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, comp_workers, CTLFLAG_RW,
	   &hammer2_comp_workers, 0,
	   "LZ4 worker threads used (0 = compress inline in the xop thread; "
	   "capped at the threads started at load, min(ncpus,4))");
SYSCTL_UINT(_vfs_hammer2, OID_AUTO, comp_depth, CTLFLAG_RW,
	   &hammer2_comp_depth, 0, "max blocks with a compression job in flight");
SYSCTL_UINT(_vfs_hammer2, OID_AUTO, comp_inflight, CTLFLAG_RD,
	   &hammer2_comp_inflight, 0, "compression jobs in flight now");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, cw_done_ns, CTLFLAG_RW,
	   &hammer2_cw_done_ns, 0, "write path: ns in subcol_done/unlock (row seal)");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, rz_seal_flush_short, CTLFLAG_RD,
	   &hammer2_rz_seal_flush_short, 0,
	   "v3 RAID6: flush-sealed rows with fewer than ndata columns");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, chain_allocs, CTLFLAG_RD,
	   &hammer2_chain_allocs, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, limit_saved_chains, CTLFLAG_RW,
	   &hammer2_limit_saved_chains, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, limit_saved_depth, CTLFLAG_RW,
	   &hammer2_limit_saved_depth, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, limit_dirty_chains, CTLFLAG_RW,
	   &hammer2_limit_dirty_chains, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, limit_dirty_inodes, CTLFLAG_RW,
	   &hammer2_limit_dirty_inodes, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, count_modified_chains, CTLFLAG_RD,
	   &hammer2_count_modified_chains, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, dio_count, CTLFLAG_RD,
	   &hammer2_dio_count, 0, "");
SYSCTL_INT(_vfs_hammer2, OID_AUTO, dio_limit, CTLFLAG_RW,
	   &hammer2_dio_limit, 0, "");

SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_file_read, CTLFLAG_RD,
	   &hammer2_iod_file_read, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_meta_read, CTLFLAG_RD,
	   &hammer2_iod_meta_read, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_indr_read, CTLFLAG_RD,
	   &hammer2_iod_indr_read, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_fmap_read, CTLFLAG_RD,
	   &hammer2_iod_fmap_read, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_volu_read, CTLFLAG_RD,
	   &hammer2_iod_volu_read, 0, "");

SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_file_write, CTLFLAG_RD,
	   &hammer2_iod_file_write, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_file_wembed, CTLFLAG_RD,
	   &hammer2_iod_file_wembed, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_file_wzero, CTLFLAG_RD,
	   &hammer2_iod_file_wzero, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_file_wdedup, CTLFLAG_RD,
	   &hammer2_iod_file_wdedup, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_meta_write, CTLFLAG_RD,
	   &hammer2_iod_meta_write, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_indr_write, CTLFLAG_RD,
	   &hammer2_iod_indr_write, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_fmap_write, CTLFLAG_RD,
	   &hammer2_iod_fmap_write, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_volu_write, CTLFLAG_RD,
	   &hammer2_iod_volu_write, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_inode_creates, CTLFLAG_RD,
	   &hammer2_iod_inode_creates, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, iod_inode_deletes, CTLFLAG_RD,
	   &hammer2_iod_inode_deletes, 0, "");

SYSCTL_LONG(_vfs_hammer2, OID_AUTO, process_icrc32, CTLFLAG_RD,
	   &hammer2_process_icrc32, 0, "");
SYSCTL_LONG(_vfs_hammer2, OID_AUTO, process_xxhash64, CTLFLAG_RD,
	   &hammer2_process_xxhash64, 0, "");

static int hammer2_vfs_init(struct vfsconf *conf);
static int hammer2_vfs_uninit(struct vfsconf *vfsp);
static void hammer2_persist_task_fn(void *arg, int pending);
static struct task hammer2_persist_task;
static void hammer2_disk_gone(void *arg, struct disk *dp);
static void hammer2_disk_gone_task(void *arg, int pending);

struct hammer2_gone {
	TAILQ_ENTRY(hammer2_gone) entry;
	struct disk	*dp;
};

static TAILQ_HEAD(, hammer2_gone) hammer2_gone_list =
	TAILQ_HEAD_INITIALIZER(hammer2_gone_list);
static struct spinlock hammer2_gone_spin;
static struct task hammer2_gone_task;
static struct taskqueue *hammer2_gone_tq;
#ifdef DISK_GONE
static eventhandler_tag hammer2_gone_tag;
#endif
static int hammer2_vfs_mount(struct mount *mp, char *path, caddr_t data,
				struct ucred *cred);
static int hammer2_vfs_mount_spec(struct mount *mp, char *path, caddr_t data,
				struct ucred *cred, char *devstr);
static int hammer2_remount(hammer2_dev_t *, struct mount *, char *,
				struct ucred *);
static int hammer2_recovery(hammer2_dev_t *hmp);
static int hammer2_vfs_unmount(struct mount *mp, int mntflags);
static int hammer2_vfs_root(struct mount *mp, struct vnode **vpp);
static int hammer2_vfs_statfs(struct mount *mp, struct statfs *sbp,
				struct ucred *cred);
static int hammer2_vfs_statvfs(struct mount *mp, struct statvfs *sbp,
				struct ucred *cred);
static int hammer2_vfs_fhtovp(struct mount *mp, struct vnode *rootvp,
				struct fid *fhp, struct vnode **vpp);
static int hammer2_vfs_vptofh(struct vnode *vp, struct fid *fhp);
static int hammer2_vfs_checkexp(struct mount *mp, struct sockaddr *nam,
				int *exflagsp, struct ucred **credanonp);
static int hammer2_vfs_modifying(struct mount *mp);

static void hammer2_update_pmps(hammer2_dev_t *hmp);

static void hammer2_mount_helper(struct mount *mp, hammer2_pfs_t *pmp);
static void hammer2_unmount_helper(struct mount *mp, hammer2_pfs_t *pmp,
				hammer2_dev_t *hmp);
static int hammer2_fixup_pfses(hammer2_dev_t *hmp);

/*
 * HAMMER2 vfs operations.
 */
static struct vfsops hammer2_vfsops = {
	.vfs_flags	= 0,
	.vfs_init	= hammer2_vfs_init,
	.vfs_uninit	= hammer2_vfs_uninit,
	.vfs_sync	= hammer2_vfs_sync,
	.vfs_mount	= hammer2_vfs_mount,
	.vfs_unmount	= hammer2_vfs_unmount,
	.vfs_root	= hammer2_vfs_root,
	.vfs_statfs	= hammer2_vfs_statfs,
	.vfs_statvfs	= hammer2_vfs_statvfs,
	.vfs_vget	= hammer2_vfs_vget,
	.vfs_vptofh	= hammer2_vfs_vptofh,
	.vfs_fhtovp	= hammer2_vfs_fhtovp,
	.vfs_checkexp	= hammer2_vfs_checkexp,
	.vfs_modifying	= hammer2_vfs_modifying
};

MALLOC_DEFINE(M_HAMMER2, "HAMMER2-mount", "");

VFS_SET(hammer2_vfsops, hammer2, VFCF_MPSAFE);
MODULE_VERSION(hammer2, 1);

static
int
hammer2_vfs_init(struct vfsconf *conf)
{
	static struct objcache_malloc_args margs_read;
	static struct objcache_malloc_args margs_write;
	static struct objcache_malloc_args margs_vop;

	int error;
	int mod;

	error = 0;
	kmalloc_raise_limit(M_HAMMER2, 0);	/* unlimited */

	/*
	 * hammer2_xop_nthreads must be a multiple of ncpus,
	 * minimum 2 * ncpus.
	 */
	mod = ncpus;
	hammer2_xop_mod = mod;
	hammer2_xop_nthreads = mod * 2;
	while (hammer2_xop_nthreads / mod < HAMMER2_XOPGROUPS_MIN ||
	       hammer2_xop_nthreads < HAMMER2_XOPTHREADS_MIN)
	{
		hammer2_xop_nthreads += mod;
	}
	hammer2_xop_sgroups = hammer2_xop_nthreads / mod / 2;
	hammer2_xop_xgroups = hammer2_xop_nthreads / mod - hammer2_xop_sgroups;
	hammer2_xop_xbase = hammer2_xop_sgroups * mod;

	/*
	 * A large DIO cache is needed to retain dedup enablement masks.
	 * The bulkfree code clears related masks as part of the disk block
	 * recycling algorithm, preventing it from being used for a later
	 * dedup.
	 *
	 * NOTE: A large buffer cache can actually interfere with dedup
	 *	 operation because we dedup based on media physical buffers
	 *	 and not logical buffers.  Try to make the DIO case large
	 *	 enough to avoid this problem, but also cap it.
	 */
	hammer2_dio_limit = nbuf * 2;
	if (hammer2_dio_limit > 100000)
		hammer2_dio_limit = 100000;

	if (HAMMER2_BLOCKREF_BYTES != sizeof(struct hammer2_blockref))
		error = EINVAL;
	if (HAMMER2_INODE_BYTES != sizeof(struct hammer2_inode_data))
		error = EINVAL;
	if (HAMMER2_VOLUME_BYTES != sizeof(struct hammer2_volume_data))
		error = EINVAL;

	if (error)
		kprintf("HAMMER2 structure size mismatch; cannot continue.\n");

	margs_read.objsize = 65536;
	margs_read.mtype = M_HAMMER2_DEBUFFER;

	/*
	 * 32 KB of compressed output plus the LZ4 hash table
	 * (HAMMER2_LZ4_CTXSIZE), so compression needs no allocation.
	 */
	margs_write.objsize = 32768 + HAMMER2_LZ4_CTXSIZE;
	margs_write.mtype = M_HAMMER2_CBUFFER;

	margs_vop.objsize = sizeof(hammer2_xop_t);
	margs_vop.mtype = M_HAMMER2;

	/*
	 * Note thaht for the XOPS cache we want backing store allocations
	 * to use M_ZERO.  This is not allowed in objcache_get() (to avoid
	 * confusion), so use the backing store function that does it.  This
	 * means that initial XOPS objects are zerod but REUSED objects are
	 * not.  So we are responsible for cleaning the object up sufficiently
	 * for our needs before objcache_put()ing it back (typically just the
	 * FIFO indices).
	 */
	cache_buffer_read = objcache_create(margs_read.mtype->ks_shortdesc,
				0, 1, NULL, NULL, NULL,
				objcache_malloc_alloc,
				objcache_malloc_free,
				&margs_read);
	cache_buffer_write = objcache_create(margs_write.mtype->ks_shortdesc,
				0, 1, NULL, NULL, NULL,
				objcache_malloc_alloc,
				objcache_malloc_free,
				&margs_write);
	cache_xops = objcache_create(margs_vop.mtype->ks_shortdesc,
				0, 1, NULL, NULL, NULL,
				objcache_malloc_alloc_zero,
				objcache_malloc_free,
				&margs_vop);


	lockinit(&hammer2_mntlk, "mntlk", 0, 0);
	TAILQ_INIT(&hammer2_mntlist);
	TAILQ_INIT(&hammer2_pfslist);
	TAILQ_INIT(&hammer2_spmplist);

	hammer2_limit_dirty_chains = maxvnodes / 10;
	if (hammer2_limit_dirty_chains > HAMMER2_LIMIT_DIRTY_CHAINS)
		hammer2_limit_dirty_chains = HAMMER2_LIMIT_DIRTY_CHAINS;
	if (hammer2_limit_dirty_chains < 1000)
		hammer2_limit_dirty_chains = 1000;

	hammer2_limit_dirty_inodes = maxvnodes / 25;
	if (hammer2_limit_dirty_inodes < 100)
		hammer2_limit_dirty_inodes = 100;
	if (hammer2_limit_dirty_inodes > HAMMER2_LIMIT_DIRTY_INODES)
		hammer2_limit_dirty_inodes = HAMMER2_LIMIT_DIRTY_INODES;

	hammer2_limit_saved_chains = hammer2_limit_dirty_chains * 5;

	hammer2_raid6_init();
	hammer2_comp_init();
	TASK_INIT(&hammer2_persist_task, 0, hammer2_persist_task_fn, NULL);

	spin_init(&hammer2_gone_spin, "h2gone");
	TASK_INIT(&hammer2_gone_task, 0, hammer2_disk_gone_task, NULL);
	hammer2_gone_tq = taskqueue_create("h2gone", M_WAITOK,
					   taskqueue_thread_enqueue,
					   &hammer2_gone_tq);
	taskqueue_start_threads(&hammer2_gone_tq, 1, TDPRI_KERN_DAEMON, -1,
				"h2gone");
#ifdef DISK_GONE
	hammer2_gone_tag = EVENTHANDLER_REGISTER(disk_gone, hammer2_disk_gone,
						 NULL, EVENTHANDLER_PRI_ANY);
#endif

	return (error);
}

static
int
hammer2_vfs_uninit(struct vfsconf *vfsp __unused)
{
#ifdef DISK_GONE
	EVENTHANDLER_DEREGISTER(disk_gone, hammer2_gone_tag);
#endif
	taskqueue_drain(hammer2_gone_tq, &hammer2_gone_task);
	taskqueue_free(hammer2_gone_tq);
	taskqueue_drain(taskqueue_thread[0], &hammer2_persist_task);
	hammer2_comp_uninit();
	objcache_destroy(cache_buffer_read);
	objcache_destroy(cache_buffer_write);
	objcache_destroy(cache_xops);
	return 0;
}

/*
 * Failures marked from a context that cannot take the voldata lock
 * (hammer2_raid6_mark_failed with HAMMER2_FAIL_DEFER) are recorded here.
 */
static void
hammer2_persist_task_fn(void *arg __unused, int pending __unused)
{
	hammer2_dev_t *hmp;

	lockmgr(&hammer2_mntlk, LK_EXCLUSIVE);
	TAILQ_FOREACH(hmp, &hammer2_mntlist, mntentry)
		hammer2_raid6_persist_failed(hmp);
	lockmgr(&hammer2_mntlk, LK_RELEASE);
}

void
hammer2_raid6_persist_kick(void)
{
	taskqueue_enqueue(taskqueue_thread[0], &hammer2_persist_task);
}

/*
 * A RAID6 member whose device went away (unplugged) is failed and closed
 * right away, not when an I/O to it next errors out, which an idle array
 * may never do.  The close lets the driver free the device, so that a
 * replugged disk comes back under its old name and can be replaced onto
 * that path.
 *
 * The disk_gone handler runs on the disk thread, where the close must not
 * happen (see disk_gone_fn); a task does the work.  The task has a queue
 * of its own and never sleeps for a lock while holding hammer2_mntlk: a
 * hung member keeps its writers inside rebuild_lk (R11), and waiting for
 * it exclusive with mntlk held, on the shared thread, wedged mount,
 * unmount and every seal behind it.
 */
static void
hammer2_disk_gone(void *arg __unused, struct disk *dp)
{
	struct hammer2_gone *g;

	g = kmalloc(sizeof(*g), M_HAMMER2, M_WAITOK | M_ZERO);
	g->dp = dp;
	spin_lock(&hammer2_gone_spin);
	TAILQ_INSERT_TAIL(&hammer2_gone_list, g, entry);
	spin_unlock(&hammer2_gone_spin);
	taskqueue_enqueue(hammer2_gone_tq, &hammer2_gone_task);
}

/*
 * dp is only compared, never dereferenced: it may be destroyed by now,
 * but not while a member still has it open, so a match is the real disk.
 *
 * Fail first, at once (a shared rebuild_lk keeps a replace from changing
 * the member), so that I/O goes around the disk.  Then close it, which
 * needs rebuild_lk exclusive to wait out the writers; that is only tried,
 * and the caller tries again later.  Returns 1 if it has to.
 */
static int
hammer2_disk_gone_hmp(hammer2_dev_t *hmp, struct disk *dp)
{
	hammer2_devvp_t *dev;
	struct vnode *devvp;
	int again = 0;
	int error;
	int i;

	if (hmp->raid_type != HAMMER2_RAID_TYPE_RAID6)
		return 0;
	for (i = 0; i < hmp->nvolumes; i++) {
		dev = hmp->volumes[i].dev;
		if (dev == NULL || dev->open == 0)
			continue;
		devvp = dev->devvp;
		if (devvp == NULL || devvp->v_rdev == NULL ||
		    devvp->v_rdev->si_disk != dp)
			continue;

		/*
		 * A replace may finish while we wait for the lock, so check
		 * again that the member is still the disk that went away
		 * before failing it, or the new disk would be failed.
		 */
		if (!hmp->raid_failed[i]) {
			if (lockmgr(&hmp->rebuild_lk,
				    LK_SHARED | LK_NOWAIT) != 0) {
				again = 1;
				continue;
			}
			if (dev->devvp != devvp || !dev->open) {
				lockmgr(&hmp->rebuild_lk, LK_RELEASE);
				continue;
			}
			/* A third failure is left alone: the array is lost */
			error = hammer2_raid6_mark_failed(hmp, i,
							  HAMMER2_FAIL_DEFER);
			lockmgr(&hmp->rebuild_lk, LK_RELEASE);
			if (error)
				continue;
		}

		if (lockmgr(&hmp->rebuild_lk, LK_EXCLUSIVE | LK_NOWAIT) != 0) {
			again = 1;
			continue;
		}
		if (dev->devvp != devvp || !dev->open) {
			lockmgr(&hmp->rebuild_lk, LK_RELEASE);
			continue;
		}
		if (hammer2_pq_busy(hmp)) {
			/* parity in flight (async_seal): not with mntlk held */
			lockmgr(&hmp->rebuild_lk, LK_RELEASE);
			again = 1;
			continue;
		}
		error = hammer2_raid6_close_member_locked(hmp, i, devvp);
		lockmgr(&hmp->rebuild_lk, LK_RELEASE);
		if (error == EAGAIN) {
			/* writes to it are still in the driver */
			again = 1;
		} else if (error == EBUSY) {
			/* its writes fail, and with them the resilver */
			kprintf("hammer2: RAID6 disk %d (%s) is gone while "
				"resilvering\n", i, dev->path);
		} else if (error == 0) {
			kprintf("hammer2: RAID6 disk %d (%s) is gone; failed "
				"and closed\n", i, dev->path);
		}
	}
	return again;
}

static void
hammer2_disk_gone_task(void *arg __unused, int pending __unused)
{
	struct hammer2_gone *g;
	hammer2_dev_t *hmp;
	int again;
	int tries;

	for (;;) {
		spin_lock(&hammer2_gone_spin);
		g = TAILQ_FIRST(&hammer2_gone_list);
		if (g)
			TAILQ_REMOVE(&hammer2_gone_list, g, entry);
		spin_unlock(&hammer2_gone_spin);
		if (g == NULL)
			break;
		/* writers of a hung member may hold rebuild_lk for a while */
		for (tries = 0; tries < 3000; tries++) {
			again = 0;
			lockmgr(&hammer2_mntlk, LK_EXCLUSIVE);
			TAILQ_FOREACH(hmp, &hammer2_mntlist, mntentry)
				again |= hammer2_disk_gone_hmp(hmp, g->dp);
			lockmgr(&hammer2_mntlk, LK_RELEASE);
			if (!again)
				break;
			tsleep(g, 0, "h2gone", hz / 10 + 1);
		}
		kfree(g, M_HAMMER2);
	}
}

/*
 * Test-only: write a member index to make the handler above run for that
 * member's disk, as if the driver had called disk_gone() (the h2dev guest
 * kernel has no such event; only the Pi's does).  The disk is not really
 * gone, so the close can be undone with raid replace onto the same path.
 */
static int
sysctl_hammer2_inject_gone(SYSCTL_HANDLER_ARGS)
{
	hammer2_dev_t *hmp;
	struct vnode *devvp;
	struct disk *dp = NULL;
	int idx = -1;
	int error;

	error = sysctl_handle_int(oidp, &idx, 0, req);
	if (error || req->newptr == NULL)
		return error;
	lockmgr(&hammer2_mntlk, LK_EXCLUSIVE);
	TAILQ_FOREACH(hmp, &hammer2_mntlist, mntentry) {
		if (hmp->raid_type != HAMMER2_RAID_TYPE_RAID6 ||
		    idx < 0 || idx >= hmp->nvolumes ||
		    hmp->volumes[idx].dev == NULL ||
		    !hmp->volumes[idx].dev->open)
			continue;
		devvp = hmp->volumes[idx].dev->devvp;
		if (devvp && devvp->v_rdev)
			dp = devvp->v_rdev->si_disk;
		break;
	}
	lockmgr(&hammer2_mntlk, LK_RELEASE);
	if (dp == NULL)
		return ENOENT;
	hammer2_disk_gone(NULL, dp);
	return 0;
}
SYSCTL_PROC(_vfs_hammer2, OID_AUTO, inject_gone,
	    CTLTYPE_INT | CTLFLAG_WR, NULL, 0,
	    sysctl_hammer2_inject_gone, "I",
	    "Test only: treat the device of this RAID6 member as gone "
	    "(disk_gone), failing and closing it.");

/*
 * Test-only (group O): rows with parity in flight on the first RAID6
 * mount, and the command hook of hammer2_pq_test_cmd().
 */
static hammer2_dev_t *
hammer2_test_first_raid6(void)
{
	hammer2_dev_t *hmp;

	TAILQ_FOREACH(hmp, &hammer2_mntlist, mntentry) {
		if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6)
			return hmp;
	}
	return NULL;
}

static int
sysctl_hammer2_pq_inflight(SYSCTL_HANDLER_ARGS)
{
	hammer2_dev_t *hmp;
	int n = 0;

	lockmgr(&hammer2_mntlk, LK_SHARED);
	if ((hmp = hammer2_test_first_raid6()) != NULL)
		n = hmp->pq_count;
	lockmgr(&hammer2_mntlk, LK_RELEASE);
	return sysctl_handle_int(oidp, &n, 0, req);
}
SYSCTL_PROC(_vfs_hammer2, OID_AUTO, pq_inflight,
	    CTLTYPE_INT | CTLFLAG_RD, NULL, 0, sysctl_hammer2_pq_inflight, "I",
	    "v3 RAID6: rows with parity writes in flight (async_seal)");

static char hammer2_pq_test_res[96] = "none";

static int
sysctl_hammer2_pq_test(SYSCTL_HANDLER_ARGS)
{
	hammer2_dev_t *hmp;
	char cmd[64];
	int error;

	if (req->newptr == NULL)
		return sysctl_handle_string(oidp, hammer2_pq_test_res,
		    sizeof(hammer2_pq_test_res), req);
	bzero(cmd, sizeof(cmd));
	error = sysctl_handle_string(oidp, cmd, sizeof(cmd), req);
	if (error)
		return error;
	/* mntlk is not held across the I/O: the test keeps the mount */
	lockmgr(&hammer2_mntlk, LK_SHARED);
	hmp = hammer2_test_first_raid6();
	lockmgr(&hammer2_mntlk, LK_RELEASE);
	if (hmp == NULL)
		return ENOENT;
	return hammer2_pq_test_cmd(hmp, cmd, hammer2_pq_test_res,
	    sizeof(hammer2_pq_test_res));
}
SYSCTL_PROC(_vfs_hammer2, OID_AUTO, pq_test,
	    CTLTYPE_STRING | CTLFLAG_RW, NULL, 0, sysctl_hammer2_pq_test, "A",
	    "Test only: async_seal parity probe/read commands; reading gives "
	    "the last result");

/*
 * Core PFS allocator.  Used to allocate or reference the pmp structure
 * for PFS cluster mounts and the spmp structure for media (hmp) structures.
 * The pmp can be passed in or loaded by this function using the chain and
 * inode data.
 *
 * pmp->modify_tid tracks new modify_tid transaction ids for front-end
 * transactions.  Note that synchronization does not use this field.
 * (typically frontend operations and synchronization cannot run on the
 * same PFS node at the same time).
 *
 * XXX check locking
 */
hammer2_pfs_t *
hammer2_pfsalloc(hammer2_chain_t *chain,
		 const hammer2_inode_data_t *ripdata,
		 hammer2_dev_t *force_local)
{
	hammer2_pfs_t *pmp;
	hammer2_inode_t *iroot;
	int count;
	int i;
	int j;

	pmp = NULL;

	/*
	 * Locate or create the PFS based on the cluster id.  If ripdata
	 * is NULL this is a spmp which is unique and is always allocated.
	 *
	 * If the device is mounted in local mode all PFSs are considered
	 * independent and not part of any cluster (for debugging only).
	 */
	if (ripdata) {
		TAILQ_FOREACH(pmp, &hammer2_pfslist, mntentry) {
			if (force_local != pmp->force_local)
				continue;
			if (force_local == NULL &&
			    bcmp(&pmp->pfs_clid, &ripdata->meta.pfs_clid,
				 sizeof(pmp->pfs_clid)) == 0) {
				break;
			} else if (force_local && pmp->pfs_names[0] &&
			    strcmp(pmp->pfs_names[0], (const char *)ripdata->filename) == 0) {
				break;
			}
		}
	}

	if (pmp == NULL) {
		pmp = kmalloc(sizeof(*pmp), M_HAMMER2, M_WAITOK | M_ZERO);
		pmp->force_local = force_local;
		hammer2_trans_manage_init(pmp);
		kmalloc_create_obj(&pmp->minode, "HAMMER2-inodes",
				   sizeof(struct hammer2_inode));
		lockinit(&pmp->lock, "pfslk", 0, 0);
		hammer2_spin_init(&pmp->blockset_spin, "h2blkset");
		hammer2_inum_hash_init(pmp);
		hammer2_spin_init(&pmp->xop_spin, "h2xop");
		TAILQ_INIT(&pmp->syncq);
		TAILQ_INIT(&pmp->depq);
		hammer2_spin_init(&pmp->list_spin, "h2pfsalloc_list");

		/*
		 * Save the last media transaction id for the flusher.  Set
		 * initial
		 */
		if (ripdata) {
			pmp->pfs_clid = ripdata->meta.pfs_clid;
			TAILQ_INSERT_TAIL(&hammer2_pfslist, pmp, mntentry);
		} else {
			pmp->flags |= HAMMER2_PMPF_SPMP;
			TAILQ_INSERT_TAIL(&hammer2_spmplist, pmp, mntentry);
		}

		/*
		 * The synchronization thread may start too early, make
		 * sure it stays frozen until we are ready to let it go.
		 * XXX
		 */
		/*
		pmp->primary_thr.flags = HAMMER2_THREAD_FROZEN |
					 HAMMER2_THREAD_REMASTER;
		*/
	}

	/*
	 * Create the PFS's root inode and any missing XOP helper threads.
	 */
	if ((iroot = pmp->iroot) == NULL) {
		iroot = hammer2_inode_get(pmp, NULL, 1, -1);
		if (ripdata)
			iroot->meta = ripdata->meta;
		pmp->iroot = iroot;
		hammer2_inode_ref(iroot);
		hammer2_inode_unlock(iroot);
	}

	/*
	 * Stop here if no chain is passed in.
	 */
	if (chain == NULL)
		goto done;

	/*
	 * When a chain is passed in we must add it to the PFS's root
	 * inode, update pmp->pfs_types[], and update the syncronization
	 * threads.
	 *
	 * When forcing local mode, mark the PFS as a MASTER regardless.
	 *
	 * At the moment empty spots can develop due to removals or failures.
	 * Ultimately we want to re-fill these spots but doing so might
	 * confused running code. XXX
	 */
	hammer2_inode_ref(iroot);
	hammer2_mtx_ex(&iroot->lock);
	j = iroot->cluster.nchains;

	if (j == HAMMER2_MAXCLUSTER) {
		kprintf("hammer2_pfsalloc: cluster full!\n");
		/* XXX fatal error? */
	} else {
		KKASSERT(chain->pmp == NULL);
		chain->pmp = pmp;
		hammer2_chain_ref(chain);
		iroot->cluster.array[j].chain = chain;
		if (force_local)
			pmp->pfs_types[j] = HAMMER2_PFSTYPE_MASTER;
		else
			pmp->pfs_types[j] = ripdata->meta.pfs_type;
		pmp->pfs_names[j] = kstrdup((const char *)ripdata->filename, M_HAMMER2);
		pmp->pfs_hmps[j] = chain->hmp;
		hammer2_spin_ex(&pmp->blockset_spin);
		pmp->pfs_iroot_blocksets[j] = chain->data->ipdata.u.blockset;
		hammer2_spin_unex(&pmp->blockset_spin);

		/*
		 * If the PFS is already mounted we must account
		 * for the mount_count here.
		 */
		if (pmp->mp)
			++chain->hmp->mount_count;

		/*
		 * May have to fixup dirty chain tracking.  Previous
		 * pmp was NULL so nothing to undo.
		 */
		if (chain->flags & HAMMER2_CHAIN_MODIFIED)
			hammer2_pfs_memory_inc(pmp);
		++j;
	}
	iroot->cluster.nchains = j;

	/*
	 * Update nmasters from any PFS inode which is part of the cluster.
	 * It is possible that this will result in a value which is too
	 * high.  MASTER PFSs are authoritative for pfs_nmasters and will
	 * override this value later on.
	 *
	 * (This informs us of masters that might not currently be
	 *  discoverable by this mount).
	 */
	if (ripdata && pmp->pfs_nmasters < ripdata->meta.pfs_nmasters) {
		pmp->pfs_nmasters = ripdata->meta.pfs_nmasters;
	}

	/*
	 * Count visible masters.  Masters are usually added with
	 * ripdata->meta.pfs_nmasters set to 1.  This detects when there
	 * are more (XXX and must update the master inodes).
	 */
	count = 0;
	for (i = 0; i < iroot->cluster.nchains; ++i) {
		if (pmp->pfs_types[i] == HAMMER2_PFSTYPE_MASTER)
			++count;
	}
	if (pmp->pfs_nmasters < count)
		pmp->pfs_nmasters = count;

	/*
	 * Create missing synchronization and support threads.
	 *
	 * Single-node masters (including snapshots) have nothing to
	 * synchronize and do not require this thread.
	 *
	 * Multi-node masters or any number of soft masters, slaves, copy,
	 * or other PFS types need the thread.
	 *
	 * Each thread is responsible for its particular cluster index.
	 * We use independent threads so stalls or mismatches related to
	 * any given target do not affect other targets.
	 */
	for (i = 0; i < iroot->cluster.nchains; ++i) {
		/*
		 * Single-node masters (including snapshots) have nothing
		 * to synchronize and will make direct xops support calls,
		 * thus they do not require this thread.
		 *
		 * Note that there can be thousands of snapshots.  We do not
		 * want to create thousands of threads.
		 */
		if (pmp->pfs_nmasters <= 1 &&
		    pmp->pfs_types[i] == HAMMER2_PFSTYPE_MASTER) {
			continue;
		}

		/*
		 * Sync support thread
		 */
		if (pmp->sync_thrs[i].td == NULL) {
			hammer2_thr_create(&pmp->sync_thrs[i], pmp, NULL,
					   "h2nod", i, -1,
					   hammer2_primary_sync_thread);
		}
	}

	/*
	 * Create missing Xop threads
	 *
	 * NOTE: We create helper threads for all mounted PFSs or any
	 *	 PFSs with 2+ nodes (so the sync thread can update them,
	 *	 even if not mounted).
	 */
	if (pmp->mp || iroot->cluster.nchains >= 2)
		hammer2_xop_helper_create(pmp);

	hammer2_mtx_unlock(&iroot->lock);
	hammer2_inode_drop(iroot);
done:
	return pmp;
}

/*
 * Deallocate an element of a probed PFS.  If destroying and this is a
 * MASTER, adjust nmasters.
 *
 * This function does not physically destroy the PFS element in its device
 * under the super-root  (see hammer2_ioctl_pfs_delete()).
 */
void
hammer2_pfsdealloc(hammer2_pfs_t *pmp, int clindex, int destroying)
{
	hammer2_inode_t *iroot;
	hammer2_chain_t *chain;
	int j;

	/*
	 * Cleanup our reference on iroot.  iroot is (should) not be needed
	 * by the flush code.
	 */
	iroot = pmp->iroot;
	if (iroot) {
		/*
		 * Stop synchronizing
		 *
		 * XXX flush after acquiring the iroot lock.
		 * XXX clean out the cluster index from all inode structures.
		 */
		hammer2_thr_delete(&pmp->sync_thrs[clindex]);

		/*
		 * Remove the cluster index from the group.  If destroying
		 * the PFS and this is a master, adjust pfs_nmasters.
		 */
		hammer2_mtx_ex(&iroot->lock);
		chain = iroot->cluster.array[clindex].chain;
		iroot->cluster.array[clindex].chain = NULL;

		switch(pmp->pfs_types[clindex]) {
		case HAMMER2_PFSTYPE_MASTER:
			if (destroying && pmp->pfs_nmasters > 0)
				--pmp->pfs_nmasters;
			/* XXX adjust ripdata->meta.pfs_nmasters */
			break;
		default:
			break;
		}
		pmp->pfs_types[clindex] = HAMMER2_PFSTYPE_NONE;

		hammer2_mtx_unlock(&iroot->lock);

		/*
		 * Release the chain.
		 */
		if (chain) {
			atomic_set_int(&chain->flags, HAMMER2_CHAIN_RELEASE);
			hammer2_chain_drop(chain);
		}

		/*
		 * Terminate all XOP threads for the cluster index.
		 */
		if (pmp->xop_groups) {
			for (j = 0; j < hammer2_xop_nthreads; ++j) {
				hammer2_thr_delete(
					&pmp->xop_groups[j].thrs[clindex]);
			}
		}
	}
}

/*
 * Destroy a PFS, typically only occurs after the last mount on a device
 * has gone away.
 */
static void
hammer2_pfsfree(hammer2_pfs_t *pmp)
{
	hammer2_inode_t *iroot;
	hammer2_chain_t *chain;
	int chains_still_present = 0;
	int i;
	int j;

	/*
	 * Cleanup our reference on iroot.  iroot is (should) not be needed
	 * by the flush code.
	 */
	if (pmp->flags & HAMMER2_PMPF_SPMP)
		TAILQ_REMOVE(&hammer2_spmplist, pmp, mntentry);
	else
		TAILQ_REMOVE(&hammer2_pfslist, pmp, mntentry);

	/*
	 * Clean up iroot
	 */
	iroot = pmp->iroot;
	if (iroot) {
		for (i = 0; i < iroot->cluster.nchains; ++i) {
			hammer2_thr_delete(&pmp->sync_thrs[i]);
			if (pmp->xop_groups) {
				for (j = 0; j < hammer2_xop_nthreads; ++j)
					hammer2_thr_delete(
						&pmp->xop_groups[j].thrs[i]);
			}
			chain = iroot->cluster.array[i].chain;
			if (chain && !RB_EMPTY(&chain->core.rbtree)) {
				kprintf("hammer2: Warning pmp %p still "
					"has active chains\n", pmp);
				chains_still_present = 1;
			}
		}
		KASSERT(iroot->refs == 1,
			("PMP->IROOT %p REFS WRONG %d", iroot, iroot->refs));

		/* ref for iroot */
		hammer2_inode_drop(iroot);
		pmp->iroot = NULL;
	}

	/*
	 * Free remaining pmp resources
	 */
	if (chains_still_present) {
		kprintf("hammer2: cannot free pmp %p, still in use\n", pmp);
	} else {
		/* the xop threads are gone; their group array is not */
		if (pmp->xop_groups) {
			kfree(pmp->xop_groups, M_HAMMER2);
			pmp->xop_groups = NULL;
		}
		kmalloc_destroy_obj(&pmp->minode);
		kfree(pmp, M_HAMMER2);
	}
}

/*
 * Remove all references to hmp from the pfs list.  Any PFS which becomes
 * empty is terminated and freed.
 *
 * XXX inefficient.
 */
static void
hammer2_pfsfree_scan(hammer2_dev_t *hmp, int which)
{
	hammer2_pfs_t *pmp;
	hammer2_inode_t *iroot;
	hammer2_chain_t *rchain;
	int i;
	int j;
	struct hammer2_pfslist *wlist;

	if (which == 0)
		wlist = &hammer2_pfslist;
	else
		wlist = &hammer2_spmplist;
again:
	TAILQ_FOREACH(pmp, wlist, mntentry) {
		if ((iroot = pmp->iroot) == NULL)
			continue;

		/*
		 * Determine if this PFS is affected.  If it is we must
		 * freeze all management threads and lock its iroot.
		 *
		 * Freezing a management thread forces it idle, operations
		 * in-progress will be aborted and it will have to start
		 * over again when unfrozen, or exit if told to exit.
		 */
		for (i = 0; i < HAMMER2_MAXCLUSTER; ++i) {
			if (pmp->pfs_hmps[i] == hmp)
				break;
		}
		if (i == HAMMER2_MAXCLUSTER)
			continue;

		hammer2_vfs_sync_pmp(pmp, MNT_WAIT);

		/*
		 * Make sure all synchronization threads are locked
		 * down.
		 */
		for (i = 0; i < HAMMER2_MAXCLUSTER; ++i) {
			if (pmp->pfs_hmps[i] == NULL)
				continue;
			hammer2_thr_freeze_async(&pmp->sync_thrs[i]);
			if (pmp->xop_groups) {
				for (j = 0; j < hammer2_xop_nthreads; ++j) {
					hammer2_thr_freeze_async(
						&pmp->xop_groups[j].thrs[i]);
				}
			}
		}
		for (i = 0; i < HAMMER2_MAXCLUSTER; ++i) {
			if (pmp->pfs_hmps[i] == NULL)
				continue;
			hammer2_thr_freeze(&pmp->sync_thrs[i]);
			if (pmp->xop_groups) {
				for (j = 0; j < hammer2_xop_nthreads; ++j) {
					hammer2_thr_freeze(
						&pmp->xop_groups[j].thrs[i]);
				}
			}
		}

		/*
		 * Lock the inode and clean out matching chains.
		 * Note that we cannot use hammer2_inode_lock_*()
		 * here because that would attempt to validate the
		 * cluster that we are in the middle of ripping
		 * apart.
		 *
		 * WARNING! We are working directly on the inodes
		 *	    embedded cluster.
		 */
		hammer2_mtx_ex(&iroot->lock);

		/*
		 * Remove the chain from matching elements of the PFS.
		 */
		for (i = 0; i < HAMMER2_MAXCLUSTER; ++i) {
			if (pmp->pfs_hmps[i] != hmp)
				continue;
			hammer2_thr_delete(&pmp->sync_thrs[i]);
			if (pmp->xop_groups) {
				for (j = 0; j < hammer2_xop_nthreads; ++j) {
					hammer2_thr_delete(
						&pmp->xop_groups[j].thrs[i]);
				}
			}
			rchain = iroot->cluster.array[i].chain;
			iroot->cluster.array[i].chain = NULL;
			pmp->pfs_types[i] = HAMMER2_PFSTYPE_NONE;
			if (pmp->pfs_names[i]) {
				kfree(pmp->pfs_names[i], M_HAMMER2);
				pmp->pfs_names[i] = NULL;
			}
			if (rchain) {
				hammer2_chain_drop(rchain);
				/* focus hint */
				if (iroot->cluster.focus == rchain)
					iroot->cluster.focus = NULL;
			}
			pmp->pfs_hmps[i] = NULL;
		}
		hammer2_mtx_unlock(&iroot->lock);

		/*
		 * Cleanup trailing chains.  Gaps may remain.
		 */
		for (i = HAMMER2_MAXCLUSTER - 1; i >= 0; --i) {
			if (pmp->pfs_hmps[i])
				break;
		}
		iroot->cluster.nchains = i + 1;

		/*
		 * If the PMP has no elements remaining we can destroy it.
		 * (this will transition management threads from frozen->exit).
		 */
		if (iroot->cluster.nchains == 0) {
			/*
			 * If this was the hmp's spmp, we need to clean
			 * a little more stuff out.
			 */
			if (hmp->spmp == pmp) {
				hmp->spmp = NULL;
				hmp->vchain.pmp = NULL;
				hmp->fchain.pmp = NULL;
			}

			/*
			 * Free the pmp and restart the loop
			 */
			KKASSERT(TAILQ_EMPTY(&pmp->syncq));
			KKASSERT(TAILQ_EMPTY(&pmp->depq));
			hammer2_pfsfree(pmp);
			goto again;
		}

		/*
		 * If elements still remain we need to set the REMASTER
		 * flag and unfreeze it.
		 */
		for (i = 0; i < HAMMER2_MAXCLUSTER; ++i) {
			if (pmp->pfs_hmps[i] == NULL)
				continue;
			hammer2_thr_remaster(&pmp->sync_thrs[i]);
			hammer2_thr_unfreeze(&pmp->sync_thrs[i]);
			if (pmp->xop_groups) {
				for (j = 0; j < hammer2_xop_nthreads; ++j) {
					hammer2_thr_remaster(
						&pmp->xop_groups[j].thrs[i]);
					hammer2_thr_unfreeze(
						&pmp->xop_groups[j].thrs[i]);
				}
			}
		}
	}
}

/*
 * Warn about members whose cache flushes do not reach the media.
 *
 * Every volume header write is preceded by a BUF_CMD_FLUSH that makes
 * the blocks it references durable.  A disk driver that drops flushes
 * (da with DA_Q_NO_SYNC_CACHE, e.g. a USB bridge that rejects
 * SYNCHRONIZE CACHE) completes them without effect, so a power loss can
 * leave a durable header pointing at blocks still in the drive cache.
 * da learns that from the first rejected flush, so send one to each
 * member before asking with DIOCGFLUSHCAP.  Drivers without the ioctl
 * are assumed to flush.
 */
static void
hammer2_check_flush_capability(hammer2_devvp_list_t *devvpl)
{
	hammer2_devvp_t *e;
	struct buf *bp;
	int cap;
	int bad = 0;

	TAILQ_FOREACH(e, devvpl, entry) {
		if (!e->open || e->devvp == NULL)
			continue;
		bp = getpbuf(NULL);
		bp->b_bio1.bio_offset = 0;
		bp->b_bufsize = 0;
		bp->b_bcount = 0;
		bp->b_cmd = BUF_CMD_FLUSH;
		bp->b_bio1.bio_done = biodone_sync;
		bp->b_bio1.bio_flags |= BIO_SYNC;
		vn_strategy(e->devvp, &bp->b_bio1);
		if (biowait(&bp->b_bio1, "h2fcap"))
			kprintf("hammer2: %s: cache flush failed\n", e->path);
		relpbuf(bp, NULL);

		if (VOP_IOCTL(e->devvp, DIOCGFLUSHCAP, (void *)&cap, 0,
			      curthread->td_ucred, NULL) == 0 && cap == 0) {
			kprintf("hammer2: WARNING: %s cannot flush its write "
				"cache\n", e->path);
			++bad;
		}
	}
	if (bad) {
		kprintf("hammer2: WARNING: %d device(s) drop cache flushes; "
			"a power loss can lose or corrupt recently written "
			"data on them\n", bad);
	}
}

/*
 * Mount or remount HAMMER2 fileystem from physical media
 *
 *	mountroot
 *		mp		mount point structure
 *		path		NULL
 *		data		<unused>
 *		cred		<unused>
 *
 *	mount
 *		mp		mount point structure
 *		path		path to mount point
 *		data		pointer to argument structure in user space
 *			volume	volume path (device@LABEL form)
 *			hflags	user mount flags
 *		cred		user credentials
 *
 * RETURNS:	0	Success
 *		!0	error number
 */
static
int
hammer2_vfs_mount(struct mount *mp, char *path, caddr_t data,
		  struct ucred *cred)
{
	char *devstr;
	int error;

	/*
	 * A multi-disk spec can be longer than MNAMELEN (four
	 * /dev/serno paths are), so it gets a buffer of its own.
	 */
	devstr = kmalloc(MAXPATHLEN, M_HAMMER2, M_WAITOK | M_ZERO);
	error = hammer2_vfs_mount_spec(mp, path, data, cred, devstr);
	kfree(devstr, M_HAMMER2);

	return error;
}

/*
 * Test only: vfs.hammer2.inject_mount_fail=N makes failure point N of
 * the first mount of a device fail once, so the unwind of each
 * refusal can be tested.  The points, in mount order:
 *	1 hammer2_init_devvp	2 hammer2_open_devvp	3 init_volumes
 *	4 sm_init, nothing allocated	5 sm_init, allocated
 *	6 space map rebuild	7 super-root lookup	8 super-root check
 *	9 PFS label lookup	10 PFS acquire
 */
static int
hammer2_mount_inject(int point)
{
	if (hammer2_inject_mount_fail != point)
		return 0;
	hammer2_inject_mount_fail = 0;
	kprintf("hammer2_mount: injected failure at point %d\n", point);
	return 1;
}

/*
 * f_mntfromname is MNAMELEN; a spec that does not fit is shown as
 * its first device, the count of the others, and the label.
 */
static void
hammer2_set_mntfromname(struct mount *mp, const char *devstr,
			const char *label)
{
	char *name = mp->mnt_stat.f_mntfromname;
	const char *p;
	int others = 0;
	int n;

	for (p = devstr; *p; ++p) {
		if (*p == ':')
			++others;
	}
	p = strchr(devstr, ':');
	n = p ? (int)(p - devstr) : (int)strlen(devstr);
	ksnprintf(name, MNAMELEN, "%.*s:+%d@%s", n, devstr, others, label);
	if (strlen(name) == MNAMELEN - 1)
		ksnprintf(name, MNAMELEN, "%.*s", n, devstr);
}

static int
hammer2_vfs_mount_spec(struct mount *mp, char *path, caddr_t data,
		       struct ucred *cred, char *devstr)
{
	struct hammer2_mount_info info;
	hammer2_pfs_t *pmp;
	hammer2_pfs_t *spmp;
	hammer2_dev_t *hmp, *hmp_tmp;
	hammer2_dev_t *force_local;
	hammer2_key_t key_next;
	hammer2_key_t key_dummy;
	hammer2_key_t lhc;
	hammer2_chain_t *parent;
	hammer2_chain_t *chain;
	const hammer2_inode_data_t *ripdata;
	hammer2_devvp_list_t devvpl;
	hammer2_devvp_t *e, *e_tmp;
	struct file *fp;
	size_t size;
	size_t done;
	char *label;
	int ronly = ((mp->mnt_flag & MNT_RDONLY) != 0);
	int error;
	int i;

	hmp = NULL;
	pmp = NULL;
	label = NULL;
	bzero(&info, sizeof(info));

	if (path) {
		/*
		 * Non-root mount or updating a mount
		 */
		error = copyin(data, &info, sizeof(info));
		if (error)
			return (error);
	}

	if (mp->mnt_flag & MNT_UPDATE) {
		/*
		 * Update mount.  Note that pmp->iroot->cluster is
		 * an inode-embedded cluster and thus cannot be
		 * directly locked.
		 *
		 * XXX HAMMER2 needs to implement NFS export via
		 *     mountctl.
		 */
		hammer2_cluster_t *cluster;

		error = 0;
		pmp = MPTOPMP(mp);
		pmp->hflags = info.hflags;
		cluster = &pmp->iroot->cluster;
		for (i = 0; i < cluster->nchains; ++i) {
			if (cluster->array[i].chain == NULL)
				continue;
			hmp = cluster->array[i].chain->hmp;
			error = hammer2_remount(hmp, mp, path, cred);
			if (error)
				break;
		}

		return error;
	}

	if (path == NULL) {
		/*
		 * Root mount
		 */
		info.cluster_fd = -1;
		ksnprintf(devstr, MAXPATHLEN, "%s",
			  mp->mnt_stat.f_mntfromname);
		done = strlen(devstr) + 1;
		kprintf("hammer2_mount: root devstr=\"%s\"\n", devstr);
	} else {
		error = copyinstr(info.volume, devstr, MAXPATHLEN - 1, &done);
		if (error)
			return (error);
		kprintf("hammer2_mount: devstr=\"%s\"\n", devstr);
	}

	/*
	 * Extract device and label, automatically mount @BOOT, @ROOT, or @DATA
	 * if no label specified, based on the partition id.  Error out if no
	 * label or device (with partition id) is specified.  This is strictly
	 * a convenience to match the default label created by newfs_hammer2,
	 * our preference is that a label always be specified.
	 *
	 * NOTE: We allow 'mount @LABEL <blah>'... that is, a mount command
	 *	 that does not specify a device, as long as some H2 label
	 *	 has already been mounted from that device.  This makes
	 *	 mounting snapshots a lot easier.
	 */
	label = strchr(devstr, '@');
	if (label && ((label + 1) - devstr) > done) {
		kprintf("hammer2_mount: bad label %s/%zd\n", devstr, done);
		return (EINVAL);
	}
	if (label == NULL || label[1] == 0) {
		char slice;

		if (label == NULL)
			label = devstr + strlen(devstr);
		else
			*label = '\0';		/* clean up trailing @ */

		slice = label[-1];
		switch(slice) {
		case 'a':
			label = "BOOT";
			break;
		case 'd':
			label = "ROOT";
			break;
		default:
			label = "DATA";
			break;
		}
	} else {
		*label = '\0';
		label++;
	}

	kprintf("hammer2_mount: device=\"%s\" label=\"%s\" rdonly=%d\n",
		devstr, label, ronly);

	/*
	 * Initialize all device vnodes.
	 */
	TAILQ_INIT(&devvpl);
	error = hammer2_init_devvp(devstr, path == NULL, &devvpl);
	if (error == 0 && hammer2_mount_inject(1))
		error = EIO;
	if (error) {
		kprintf("hammer2: failed to initialize devvp in %s\n", devstr);
		hammer2_cleanup_devvp(&devvpl);
		return error;
	}

	/*
	 * Determine if the device has already been mounted.  After this
	 * check hmp will be non-NULL if we are doing the second or more
	 * hammer2 mounts from the same device.
	 */
	lockmgr(&hammer2_mntlk, LK_EXCLUSIVE);
	if (!TAILQ_EMPTY(&devvpl)) {
		/*
		 * Match the device.  Due to the way devfs works,
		 * we may not be able to directly match the vnode pointer,
		 * so also check to see if the underlying device matches.
		 */
		TAILQ_FOREACH(hmp_tmp, &hammer2_mntlist, mntentry) {
			TAILQ_FOREACH(e_tmp, &hmp_tmp->devvpl, entry) {
				int devvp_found = 0;
				/*
				 * An absent disk of a degraded mount has a
				 * dummy vnode that no device can match.
				 */
				if (e_tmp->devvp == NULL ||
				    e_tmp->devvp->v_rdev == NULL)
					continue;
				TAILQ_FOREACH(e, &devvpl, entry) {
					/* Skip absent devices (dummy vnodes) */
					if (e->devvp->v_rdev == NULL)
						continue;
					if (e_tmp->devvp == e->devvp)
						devvp_found = 1;
					if (e_tmp->devvp &&
					    e_tmp->devvp->v_rdev &&
					    e_tmp->devvp->v_rdev == e->devvp->v_rdev)
						devvp_found = 1;
				}
				if (!devvp_found)
					goto next_hmp;
			}
			hmp = hmp_tmp;
			kprintf("hammer2_mount: hmp=%p matched\n", hmp);
			break;
next_hmp:
			continue;
		}

		/*
		 * If no match this may be a fresh H2 mount, make sure
		 * the device is not mounted on anything else.
		 */
		if (hmp == NULL) {
			TAILQ_FOREACH(e, &devvpl, entry) {
				struct vnode *devvp = e->devvp;
				/* Skip absent devices (dummy vnodes) */
				if (devvp->v_rdev == NULL)
					continue;
				error = vfs_mountedon(devvp);
				if (error) {
					kprintf("hammer2_mount: %s mounted %d\n",
						e->path, error);
					hammer2_cleanup_devvp(&devvpl);
					lockmgr(&hammer2_mntlk, LK_RELEASE);
					return error;
				}
			}
		}
	} else {
		/*
		 * Match the label to a pmp already probed.
		 */
		TAILQ_FOREACH(pmp, &hammer2_pfslist, mntentry) {
			for (i = 0; i < HAMMER2_MAXCLUSTER; ++i) {
				if (pmp->pfs_names[i] &&
				    strcmp(pmp->pfs_names[i], label) == 0) {
					hmp = pmp->pfs_hmps[i];
					break;
				}
			}
			if (hmp)
				break;
		}
		if (hmp == NULL) {
			kprintf("hammer2_mount: PFS label \"%s\" not found\n",
				label);
			hammer2_cleanup_devvp(&devvpl);
			lockmgr(&hammer2_mntlk, LK_RELEASE);
			return ENOENT;
		}
	}

	/*
	 * Open the device if this isn't a secondary mount and construct
	 * the H2 device mount (hmp).
	 */
	if (hmp == NULL) {
		hammer2_chain_t *schain;
		hammer2_xop_head_t xop;

		/*
		 * Now open the device
		 */
		KKASSERT(!TAILQ_EMPTY(&devvpl));
		error = hammer2_open_devvp(&devvpl, ronly);
		if (error == 0 && hammer2_mount_inject(2))
			error = EIO;
		if (error) {
			hammer2_close_devvp(NULL, &devvpl, ronly);
			hammer2_cleanup_devvp(&devvpl);
			lockmgr(&hammer2_mntlk, LK_RELEASE);
			return error;
		}

		/*
		 * Construct volumes and link with device vnodes.
		 */
		hmp = kmalloc(sizeof(*hmp), M_HAMMER2, M_WAITOK | M_ZERO);
		hmp->devvp = NULL;
		error = hammer2_init_volumes(mp, &devvpl, hmp->volumes,
					     &hmp->voldata, &hmp->volhdrno,
					     &hmp->devvp);
		if (error == 0 && hammer2_mount_inject(3))
			error = EIO;
		if (error) {
			hammer2_close_devvp(NULL, &devvpl, ronly);
			hammer2_cleanup_devvp(&devvpl);
			lockmgr(&hammer2_mntlk, LK_RELEASE);
			kfree(hmp, M_HAMMER2);
			return error;
		}
		if (!hmp->devvp) {
			kprintf("hammer2: failed to initialize root volume\n");
			hammer2_close_devvp(NULL, &devvpl, ronly);
			hammer2_cleanup_devvp(&devvpl);
			lockmgr(&hammer2_mntlk, LK_RELEASE);
			kfree(hmp, M_HAMMER2);
			return EINVAL;
		}

		if (!ronly)
			hammer2_check_flush_capability(&devvpl);

		ksnprintf(hmp->devrepname, sizeof(hmp->devrepname), "%s", devstr);
		hmp->ronly = ronly;
		hmp->hflags = info.hflags & HMNT2_DEVFLAGS;
		kmalloc_create_obj(&hmp->mchain, "HAMMER2-chains",
				   sizeof(struct hammer2_chain));
		kmalloc_create_obj(&hmp->mio, "HAMMER2-dio",
				   sizeof(struct hammer2_io));
		kmalloc_create(&hmp->mmsg, "HAMMER2-msg");
		TAILQ_INSERT_TAIL(&hammer2_mntlist, hmp, mntentry);

		/*
		 * Move devvpl entries to hmp now, so that every error
		 * exit below closes the devices through
		 * hammer2_unmount_helper().
		 */
		TAILQ_INIT(&hmp->devvpl);
		while ((e = TAILQ_FIRST(&devvpl)) != NULL) {
			TAILQ_REMOVE(&devvpl, e, entry);
			TAILQ_INSERT_TAIL(&hmp->devvpl, e, entry);
		}
		KKASSERT(TAILQ_EMPTY(&devvpl));
		KKASSERT(!TAILQ_EMPTY(&hmp->devvpl));
		hammer2_io_hash_init(hmp);
		hammer2_spin_init(&hmp->list_spin, "h2mount_list");

		lockinit(&hmp->vollk, "h2vol", 0, 0);
		lockinit(&hmp->bulklk, "h2bulk", 0, 0);
		lockinit(&hmp->bflock, "h2bflk", 0, 0);
		lockinit(&hmp->rebuild_lk, "h2rbld", 0, 0);
		hammer2_iowd_start(hmp);
		hammer2_spin_init(&hmp->mirror_spin, "h2mirror");
		lockinit(&hmp->mirror_flush_lk, "h2mflk", 0, 0);
		for (i = 0; i < HAMMER2_MIRROR_HASH; ++i)
			TAILQ_INIT(&hmp->mirror_pend[i]);

		/*
		 * vchain setup. vchain.data is embedded.
		 * vchain.refs is initialized and will never drop to 0.
		 */
		hmp->vchain.hmp = hmp;
		hmp->vchain.refs = 1;
		hmp->vchain.data = (void *)&hmp->voldata;
		hmp->vchain.bref.type = HAMMER2_BREF_TYPE_VOLUME;
		hmp->vchain.bref.data_off = 0 | HAMMER2_PBUFRADIX;
		hammer2_chain_init(&hmp->vchain);

		/*
		 * fchain setup.  fchain.data is embedded.
		 * fchain.refs is initialized and will never drop to 0.
		 *
		 * The data is not used but needs to be initialized to
		 * pass assertion muster.  We use this chain primarily
		 * as a placeholder for the freemap's top-level radix tree
		 * so it does not interfere with the volume's topology
		 * radix tree.
		 */
		hmp->fchain.hmp = hmp;
		hmp->fchain.refs = 1;
		hmp->fchain.data = (void *)&hmp->voldata.freemap_blockset;
		hmp->fchain.bref.type = HAMMER2_BREF_TYPE_FREEMAP;
		hmp->fchain.bref.data_off = 0 | HAMMER2_PBUFRADIX;
		hmp->fchain.bref.methods =
			HAMMER2_ENC_CHECK(HAMMER2_CHECK_FREEMAP) |
			HAMMER2_ENC_COMP(HAMMER2_COMP_NONE);
		hammer2_chain_init(&hmp->fchain);

		/*
		 * Initialize volume header related fields.
		 */
		KKASSERT(hmp->voldata.magic == HAMMER2_VOLUME_ID_HBO ||
			 hmp->voldata.magic == HAMMER2_VOLUME_ID_ABO);
		hmp->volsync = hmp->voldata;
		hmp->free_reserved = hmp->voldata.allocator_size / 20;
		/*
		 * Must use hmp instead of volume header for these two
		 * in order to handle volume versions transparently.
		 */
		if (hmp->voldata.version >= HAMMER2_VOL_VERSION_RAIDZ2) {
			hmp->nvolumes = hmp->voldata.nvolumes;
			hmp->total_size = hmp->voldata.raid_config.array_size;
			hmp->raid_type = hmp->voldata.raid_config.raid_type;
			bcopy(&hmp->voldata.raid_config, &hmp->raid_config,
			      sizeof(hmp->raid_config));

			/*
			 * Metadata copies: 3 on layout 3, every disk on
			 * layout 2.  A layout 2 array is converted only on
			 * request (a layout 3 array is refused by a kernel
			 * that predates it); the copies already on the
			 * other disks are then simply never read.
			 */
			if (hmp->raid_config.rz_layout == 2 &&
			    hammer2_md_3copy_upgrade &&
			    !(mp->mnt_flag & MNT_RDONLY)) {
				hmp->raid_config.rz_layout = HAMMER2_RZ_LAYOUT;
				hmp->voldata.raid_config.rz_layout =
				    HAMMER2_RZ_LAYOUT;
				hmp->volsync.raid_config.rz_layout =
				    HAMMER2_RZ_LAYOUT;
				kprintf("hammer2: metadata converted to %d "
				    "copies (layout %d)\n", HAMMER2_MD_COPIES,
				    HAMMER2_RZ_LAYOUT);
			}
			hmp->md_ncopies = hammer2_md_ncopies(
			    hmp->raid_config.ndisks, hmp->raid_config.rz_layout);

			/*
			 * Restore failed disk state from on-disk config.
			 * If a disk was marked failed before unmount, keep
			 * it failed so we don't read stale/corrupt data
			 * from it.  The admin must explicitly resilver.
			 */
			hmp->raid_nfailed = 0;
			bzero(hmp->raid_failed, sizeof(hmp->raid_failed));
			for (i = 0; i < hmp->nvolumes; i++) {
				if (hmp->raid_config.disk_state[i] ==
				    HAMMER2_RAID6_DISK_FAILED) {
					hmp->raid_failed[i] = 1;
					hmp->raid_nfailed++;
				}
			}

			/*
			 * Mark any absent disks (device unavailable at
			 * mount time) as failed, and record it: an absent
			 * disk that stayed ONLINE in the header would be
			 * trusted, stale, at the next mount.  This handles
			 * degraded mounts where a disk has been physically
			 * removed or detached.  The mount is never refused
			 * for it, so the two-disk cap does not apply.  The
			 * voldata cannot be modified yet; a task records it.
			 */
			for (i = 0; i < hmp->nvolumes; i++) {
				if (hmp->volumes[i].dev == NULL ||
				    hmp->volumes[i].dev->devvp == NULL ||
				    !hmp->volumes[i].dev->open) {
					hammer2_raid6_mark_failed(hmp, i,
					    HAMMER2_FAIL_DEFER |
					    HAMMER2_FAIL_FORCE);
				}
			}

			/*
			 * Load the metadata-zone extent table from voldata
			 * (written by newfs_hammer2 -R 6).  The stripe
			 * layout depends on it.
			 */
			if (hmp->voldata.raid_config.md_nextents > 0 &&
			    hmp->voldata.raid_config.md_nextents <=
			     HAMMER2_MD_MAX_EXTENTS) {
				hmp->md_nextents =
				    hmp->voldata.raid_config.md_nextents;
				bcopy(hmp->voldata.raid_config.md_extents,
				      hmp->md_extents,
				      sizeof(hmp->md_extents));
			}

			/*
			 * Load the space map (docs/capacity.md): the row
			 * refcount the allocator works from.  A volume made
			 * before the current stripe layout is refused.
			 *
			 * With no valid copy, rebuild it by walking the
			 * blockref tree.  If that fails, refuse RW mount;
			 * RO mount stays allowed for inspection/recovery.
			 */
			if (hammer2_mount_inject(4) ||
			    hammer2_raid6_sm_init(hmp) ||
			    hammer2_mount_inject(5)) {
				hammer2_unmount_helper(mp, NULL, hmp);
				lockmgr(&hammer2_mntlk, LK_RELEASE);
				hammer2_vfs_unmount(mp, MNT_FORCE);
				return EINVAL;
			}
			hammer2_raid6_sm_read(hmp);
			if (hammer2_mount_inject(6))
				hmp->stripe_bitmap_invalid = 2;
			if (hmp->stripe_bitmap_invalid) {
				int rerr;

				kprintf("hammer2: space map invalid; "
					"walking blockref tree to rebuild...\n");
				if (hmp->stripe_bitmap_invalid == 2)
					rerr = EIO;	/* injected */
				else
					rerr = hammer2_raid6_rebuild_stripe_bitmap(hmp);
				if (rerr == 0) {
					kprintf("hammer2: space map "
						"reconstructed from blockref "
						"walk\n");
					hmp->stripe_bitmap_invalid = 0;
				} else if (!ronly) {
					kprintf("hammer2: space map "
						"rebuild failed (err %d); "
						"refusing RW mount\n", rerr);
					hammer2_unmount_helper(mp, NULL, hmp);
					lockmgr(&hammer2_mntlk, LK_RELEASE);
					hammer2_vfs_unmount(mp, MNT_FORCE);
					return EROFS;
				}
			}
			/* 6C: in-memory open-row tracker for packing. */
			hammer2_raid6_open_rows_init(hmp);

			/*
			 * Read-path self-heal repair kthread
			 * (bitrot.md §7.4): performs the deferred
			 * on-disk repair writes queued by
			 * hammer2_chain_selfheal().
			 */
			hammer2_io_repair_start(hmp);

			/* async seal: parity reaper (idle unless async_seal) */
			hammer2_pq_start(hmp);

		} else if (hmp->voldata.version >=
			   HAMMER2_VOL_VERSION_MULTI_VOLUMES) {
			hmp->nvolumes = hmp->voldata.nvolumes;
			hmp->total_size = hmp->voldata.total_size;
		} else {
			hmp->nvolumes = 1;
			hmp->total_size = hmp->voldata.volu_size;
		}
		KKASSERT(hmp->nvolumes > 0);

		/*
		 * Log RAID6 array status at mount time.  If any disks
		 * were previously failed, release VOP_OPEN on them so
		 * they can be detached/replaced.
		 */
		if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6) {
			if (hmp->raid_nfailed == 0) {
				kprintf("hammer2: RAID6 array mounted: "
					"%d disks (%d data + P + Q), "
					"all disks online\n",
					hmp->raid_config.ndisks,
					hmp->raid_config.ndata);
			} else {
				for (i = 0; i < hmp->nvolumes; i++) {
					if (!hmp->raid_failed[i])
						continue;
					kprintf("hammer2: RAID6 disk %d "
						"(%s) marked failed; "
						"operating in degraded "
						"mode\n", i,
						hmp->volumes[i].dev ?
						    hmp->volumes[i].dev->path :
						    "(absent)");
					hammer2_raid6_close_member(hmp, i, NULL);
				}
			}
		}

		/*
		 * Really important to get these right or the flush and
		 * teardown code will get confused.
		 */
		hmp->spmp = hammer2_pfsalloc(NULL, NULL, NULL);
		spmp = hmp->spmp;
		spmp->pfs_hmps[0] = hmp;

		/*
		 * Dummy-up vchain and fchain's modify_tid.  mirror_tid
		 * is inherited from the volume header.
		 */
		hmp->vchain.bref.mirror_tid = hmp->voldata.mirror_tid;
		hmp->vchain.bref.modify_tid = hmp->vchain.bref.mirror_tid;
		hmp->vchain.pmp = spmp;
		hmp->fchain.bref.mirror_tid = hmp->voldata.freemap_tid;
		hmp->fchain.bref.modify_tid = hmp->fchain.bref.mirror_tid;
		hmp->fchain.pmp = spmp;

		/*
		 * First locate the super-root inode, which is key 0
		 * relative to the volume header's blockset.
		 *
		 * Then locate the root inode by scanning the directory keyspace
		 * represented by the label.
		 */
		parent = hammer2_chain_lookup_init(&hmp->vchain, 0);
		schain = hammer2_chain_lookup(&parent, &key_dummy,
				      HAMMER2_SROOT_KEY, HAMMER2_SROOT_KEY,
				      &error, 0);
		hammer2_chain_lookup_done(parent);
		if (schain && hammer2_mount_inject(7)) {
			hammer2_chain_unlock(schain);
			hammer2_chain_drop(schain);
			schain = NULL;
		}
		if (schain == NULL) {
			kprintf("hammer2_mount: invalid super-root\n");
			hammer2_unmount_helper(mp, NULL, hmp);
			lockmgr(&hammer2_mntlk, LK_RELEASE);
			hammer2_vfs_unmount(mp, MNT_FORCE);
			return EINVAL;
		}
		if (schain->error || hammer2_mount_inject(8)) {
			kprintf("hammer2_mount: error %s reading super-root\n",
				hammer2_error_str(schain->error));
			hammer2_chain_unlock(schain);
			hammer2_chain_drop(schain);
			schain = NULL;
			hammer2_unmount_helper(mp, NULL, hmp);
			lockmgr(&hammer2_mntlk, LK_RELEASE);
			hammer2_vfs_unmount(mp, MNT_FORCE);
			return EINVAL;
		}

		/*
		 * The super-root always uses an inode_tid of 1 when
		 * creating PFSs.
		 */
		spmp->inode_tid = 1;
		spmp->modify_tid = schain->bref.modify_tid + 1;

		/*
		 * Sanity-check schain's pmp and finish initialization.
		 * Any chain belonging to the super-root topology should
		 * have a NULL pmp (not even set to spmp).
		 */
		ripdata = &schain->data->ipdata;
		KKASSERT(schain->pmp == NULL);
		spmp->pfs_clid = ripdata->meta.pfs_clid;

		/*
		 * Replace the dummy spmp->iroot with a real one.  It's
		 * easier to just do a wholesale replacement than to try
		 * to update the chain and fixup the iroot fields.
		 *
		 * The returned inode is locked with the supplied cluster.
		 */
		hammer2_dummy_xop_from_chain(&xop, schain);
		hammer2_inode_drop(spmp->iroot);
		spmp->iroot = hammer2_inode_get(spmp, &xop, -1, -1);
		spmp->spmp_hmp = hmp;
		spmp->pfs_types[0] = ripdata->meta.pfs_type;
		spmp->ronly = ronly;
		hammer2_inode_ref(spmp->iroot);
		hammer2_inode_unlock(spmp->iroot);
		hammer2_cluster_unlock(&xop.cluster);
		hammer2_chain_drop(schain);
		/* do not call hammer2_cluster_drop() on an embedded cluster */
		schain = NULL;	/* now invalid */
		/* leave spmp->iroot with one ref */

		if (!hmp->ronly) {
			error = hammer2_recovery(hmp);
			if (error == 0)
				error |= hammer2_fixup_pfses(hmp);
			/* XXX do something with error */
		}
		hammer2_update_pmps(hmp);
		hammer2_iocom_init(hmp);
		hammer2_bulkfree_init(hmp);

		/*
		 * Ref the cluster management messaging descriptor.  The mount
		 * program deals with the other end of the communications pipe.
		 *
		 * Root mounts typically do not supply one.
		 */
		if (info.cluster_fd >= 0) {
			fp = holdfp(curthread, info.cluster_fd, -1);
			if (fp) {
				hammer2_cluster_reconnect(hmp, fp);
			} else {
				kprintf("hammer2_mount: bad cluster_fd!\n");
			}
		}
	} else {
		/* hmp->devvp_list is already constructed. */
		hammer2_cleanup_devvp(&devvpl);
		spmp = hmp->spmp;
		if (info.hflags & HMNT2_DEVFLAGS) {
			kprintf("hammer2_mount: Warning: mount flags pertaining "
				"to the whole device may only be specified "
				"on the first mount of the device: %08x\n",
				info.hflags & HMNT2_DEVFLAGS);
		}
	}

	/*
	 * Force local mount (disassociate all PFSs from their clusters).
	 * Used primarily for debugging.
	 */
	force_local = (hmp->hflags & HMNT2_LOCAL) ? hmp : NULL;

	/*
	 * Lookup the mount point under the media-localized super-root.
	 * Scanning hammer2_pfslist doesn't help us because it represents
	 * PFS cluster ids which can aggregate several named PFSs together.
	 *
	 * cluster->pmp will incorrectly point to spmp and must be fixed
	 * up later on.
	 */
	hammer2_inode_lock(spmp->iroot, 0);
	parent = hammer2_inode_chain(spmp->iroot, 0, HAMMER2_RESOLVE_ALWAYS);
	lhc = hammer2_dirhash(label, strlen(label));
	chain = hammer2_chain_lookup(&parent, &key_next,
				     lhc, lhc + HAMMER2_DIRHASH_LOMASK,
				     &error, 0);
	while (chain) {
		if (chain->bref.type == HAMMER2_BREF_TYPE_INODE &&
		    strcmp(label, (char *)chain->data->ipdata.filename) == 0) {
			break;
		}
		chain = hammer2_chain_next(&parent, chain, &key_next,
					    key_next,
					    lhc + HAMMER2_DIRHASH_LOMASK,
					    &error, 0);
	}
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
	hammer2_inode_unlock(spmp->iroot);

	if (chain && hammer2_mount_inject(9)) {
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
		chain = NULL;
		error = EIO;
	}

	/*
	 * PFS could not be found?
	 */
	if (chain == NULL) {
		hammer2_unmount_helper(mp, NULL, hmp);
		lockmgr(&hammer2_mntlk, LK_RELEASE);
		hammer2_vfs_unmount(mp, MNT_FORCE);

		if (error) {
			kprintf("hammer2_mount: PFS label I/O error\n");
			return EINVAL;
		} else {
			kprintf("hammer2_mount: PFS label \"%s\" not found\n",
				label);
			return ENOENT;
		}
	}

	/*
	 * Acquire the pmp structure (it should have already been allocated
	 * via hammer2_update_pmps()).
	 */
	if (chain->error || hammer2_mount_inject(10)) {
		kprintf("hammer2_mount: PFS label I/O error\n");
	} else {
		ripdata = &chain->data->ipdata;
		pmp = hammer2_pfsalloc(NULL, ripdata, force_local);
	}
	hammer2_chain_unlock(chain);
	hammer2_chain_drop(chain);

	/*
	 * PFS to mount must exist at this point.
	 */
	if (pmp == NULL) {
		kprintf("hammer2_mount: Failed to acquire PFS structure\n");
		hammer2_unmount_helper(mp, NULL, hmp);
		lockmgr(&hammer2_mntlk, LK_RELEASE);
		hammer2_vfs_unmount(mp, MNT_FORCE);
		return EINVAL;
	}

	/*
	 * Finish the mount
	 */
	kprintf("hammer2_mount: hmp=%p pmp=%p\n", hmp, pmp);

	/* Check if the pmp has already been mounted. */
	if (pmp->mp) {
		kprintf("hammer2_mount: PFS already mounted!\n");
		hammer2_unmount_helper(mp, NULL, hmp);
		lockmgr(&hammer2_mntlk, LK_RELEASE);
		hammer2_vfs_unmount(mp, MNT_FORCE);
		return EBUSY;
	}

	pmp->ronly = ronly;
	pmp->hflags = info.hflags;
	mp->mnt_flag |= MNT_LOCAL;
	/*
	 * RAID6: disable VFS cluster_write() in VOP_WRITE to prevent
	 * mnt_pbuf_count exhaustion.  cluster_write() acquires one pbuf
	 * per write and holds it until XOP completes.  With chain-lock
	 * contention on sequential writes, many in-flight writes pile up,
	 * exhausting mnt_pbuf_count (nswbuf_kva/10 ~50) before the
	 * hammer2_flush_pipe (100) throttle kicks in, deadlocking dd
	 * in wswbuf2 (getpbuf_kva).  With MNT_NOCLUSTERW, VOP_WRITE
	 * uses bdwrite(bp); bufdaemon flushes via bawrite (no pbuf).
	 */
	if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6)
		mp->mnt_flag |= MNT_NOCLUSTERW;
	mp->mnt_kern_flag |= MNTK_ALL_MPSAFE;   /* all entry pts are SMP */
	mp->mnt_kern_flag |= MNTK_THR_SYNC;     /* new vsyncscan semantics */

	/*
	 * required mount structure initializations
	 */
	mp->mnt_stat.f_iosize = HAMMER2_PBUFSIZE;
	mp->mnt_stat.f_bsize = HAMMER2_PBUFSIZE;

	mp->mnt_vstat.f_frsize = HAMMER2_PBUFSIZE;
	mp->mnt_vstat.f_bsize = HAMMER2_PBUFSIZE;

	/*
	 * Optional fields
	 */
	mp->mnt_iosize_max = MAXPHYS;

	/*
	 * Connect up mount pointers.
	 */
	hammer2_mount_helper(mp, pmp);
	lockmgr(&hammer2_mntlk, LK_RELEASE);

	/*
	 * Finish setup
	 */
	vfs_getnewfsid(mp);
	vfs_add_vnodeops(mp, &hammer2_vnode_vops, &mp->mnt_vn_norm_ops);
	vfs_add_vnodeops(mp, &hammer2_spec_vops, &mp->mnt_vn_spec_ops);
	vfs_add_vnodeops(mp, &hammer2_fifo_vops, &mp->mnt_vn_fifo_ops);

	if (path && done > MNAMELEN) {
		bzero(mp->mnt_stat.f_mntfromname, MNAMELEN);
		hammer2_set_mntfromname(mp, devstr, label);
	} else if (path) {
		copyinstr(info.volume, mp->mnt_stat.f_mntfromname,
			  MNAMELEN - 1, &size);
		bzero(mp->mnt_stat.f_mntfromname + size, MNAMELEN - size);
	} /* else root mount, already in there */

	bzero(mp->mnt_stat.f_mntonname, sizeof(mp->mnt_stat.f_mntonname));
	if (path) {
		copyinstr(path, mp->mnt_stat.f_mntonname,
			  sizeof(mp->mnt_stat.f_mntonname) - 1,
			  &size);
	} else {
		/* root mount */
		mp->mnt_stat.f_mntonname[0] = '/';
	}

	/*
	 * Initial statfs to prime mnt_stat.
	 */
	hammer2_vfs_statfs(mp, &mp->mnt_stat, cred);

	return 0;
}

/*
 * Scan PFSs under the super-root and create hammer2_pfs structures.
 */
static
void
hammer2_update_pmps(hammer2_dev_t *hmp)
{
	const hammer2_inode_data_t *ripdata;
	hammer2_chain_t *parent;
	hammer2_chain_t *chain;
	hammer2_dev_t *force_local;
	hammer2_pfs_t *spmp;
	hammer2_key_t key_next;
	int error;

	/*
	 * Force local mount (disassociate all PFSs from their clusters).
	 * Used primarily for debugging.
	 */
	force_local = (hmp->hflags & HMNT2_LOCAL) ? hmp : NULL;

	/*
	 * Lookup mount point under the media-localized super-root.
	 *
	 * cluster->pmp will incorrectly point to spmp and must be fixed
	 * up later on.
	 */
	spmp = hmp->spmp;
	hammer2_inode_lock(spmp->iroot, 0);
	parent = hammer2_inode_chain(spmp->iroot, 0, HAMMER2_RESOLVE_ALWAYS);
	chain = hammer2_chain_lookup(&parent, &key_next,
					 HAMMER2_KEY_MIN, HAMMER2_KEY_MAX,
					 &error, 0);
	while (chain) {
		if (chain->error) {
			kprintf("I/O error scanning PFS labels\n");
		} else if (chain->bref.type != HAMMER2_BREF_TYPE_INODE) {
			kprintf("Non inode chain type %d under super-root\n",
				chain->bref.type);
		} else {
			ripdata = &chain->data->ipdata;
			hammer2_pfsalloc(chain, ripdata, force_local);
		}
		chain = hammer2_chain_next(&parent, chain, &key_next,
					   key_next, HAMMER2_KEY_MAX,
					   &error, 0);
	}
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
	hammer2_inode_unlock(spmp->iroot);
}

static
int
hammer2_remount(hammer2_dev_t *hmp, struct mount *mp, char *path __unused,
		struct ucred *cred)
{
	hammer2_pfs_t *pmp;
	hammer2_volume_t *vol;
	struct vnode *devvp;
	int i, ronly_save, error, result = 0;
	int recovered = 0;

	pmp = MPTOPMP(mp);
	ronly_save = pmp->ronly;
	if (pmp->ronly == 1 && (mp->mnt_kern_flag & MNTK_WANTRDWR))
		pmp->ronly = 0;
	else if (pmp->ronly == 0 && (mp->mnt_flag & MNT_RDONLY))
		pmp->ronly = 1;

	if (!(hmp->ronly && (mp->mnt_kern_flag & MNTK_WANTRDWR)))
		return 0;

	for (i = 0; i < hmp->nvolumes; ++i) {
		vol = &hmp->volumes[i];
		/* RAID6: a failed or absent disk is not reopened */
		if (hmp->raid_type == HAMMER2_RAID_TYPE_RAID6 &&
		    !vol->dev->open)
			continue;
		devvp = vol->dev->devvp;
		KKASSERT(devvp);
		vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
		VOP_OPEN(devvp, FREAD | FWRITE, FSCRED, NULL);
		vn_unlock(devvp);
		error = 0;
		/*
		 * Run recovery once.  RAID6 has no root volume to wait
		 * for (disk 0 may be failed), so the first live disk
		 * does.
		 */
		if (!recovered &&
		    (vol->id == HAMMER2_ROOT_VOLUME ||
		     hmp->raid_type == HAMMER2_RAID_TYPE_RAID6)) {
			recovered = 1;
			error = hammer2_recovery(hmp);
			if (error == 0)
				error |= hammer2_fixup_pfses(hmp);
		}
		vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
		if (error == 0) {
			VOP_CLOSE(devvp, FREAD, NULL);
		} else {
			VOP_CLOSE(devvp, FREAD | FWRITE, NULL);
		}
		vn_unlock(devvp);
		result |= error;
	}
	if (result == 0) {
		kprintf("hammer2: enable read/write\n");
		hmp->ronly = 0;
		hmp->spmp->ronly = 0; /* never used */
	} else {
		pmp->ronly = ronly_save;
	}

	return result;
}

/*
 * See hammer2_vfs_unmount().  raid_abort is cleared again once the
 * ioctls have drained: the device may stay mounted through another PFS.
 */
static void
hammer2_vfs_unmount_raid_abort(hammer2_pfs_t *pmp)
{
	hammer2_dev_t *hmp;
	int i;

	for (i = 0; i < HAMMER2_MAXCLUSTER; ++i) {
		hmp = pmp->pfs_hmps[i];
		if (hmp == NULL || hmp->raid_ioctls == 0)
			continue;
		kprintf("hammer2: forced unmount: stopping resilver/scrub\n");
		hmp->raid_abort = 1;
		while (hmp->raid_ioctls) {
			tsleep(__DEVOLATILE(void *, &hmp->raid_ioctls), 0,
			       "h2rabt", hz / 10);
		}
		hmp->raid_abort = 0;
	}
}

static
int
hammer2_vfs_unmount(struct mount *mp, int mntflags)
{
	hammer2_pfs_t *pmp;
	int flags;
	int error = 0;

	pmp = MPTOPMP(mp);

	if (pmp == NULL)
		return(0);

	/*
	 * A forced unmount (e.g. at shutdown) does not wait for ioctls in
	 * progress.  Stop a running resilver or scrub and wait for its
	 * ioctl to return before the vnodes and devices go away under it.
	 * A non-forced unmount fails with EBUSY on the ioctl's open vnode
	 * instead.  Done before hammer2_mntlk: the resilver syncs the pmp.
	 */
	if (mntflags & MNT_FORCE)
		hammer2_vfs_unmount_raid_abort(pmp);

	lockmgr(&hammer2_mntlk, LK_EXCLUSIVE);

	/*
	 * If mount initialization proceeded far enough we must flush
	 * its vnodes and sync the underlying mount points.  Three syncs
	 * are required to fully flush the filesystem (freemap updates lag
	 * by one flush, and one extra for safety).
	 */
	if (mntflags & MNT_FORCE)
		flags = FORCECLOSE;
	else
		flags = 0;
	if (pmp->iroot) {
		/*
		 * Pre-flush sync: clean all dirty page cache buffers before
		 * vflush calls vinvalbuf.  Without this, async XOP strategy
		 * writes may not have called biodone by the time vinvalbuf
		 * inspects buffer state, causing "busy buffer problem" for
		 * large files (e.g. 100 MB write then immediate umount).
		 */
		hammer2_vfs_sync(mp, MNT_WAIT);
		error = vflush(mp, 0, flags);
		if (error)
			goto failed;
		/*
		 * Three syncs are required to fully flush the filesystem
		 * (freemap updates lag by one flush, and one extra for
		 * safety).  Run these after vflush so any dirty chains
		 * created by VOP_FSYNC during vflush are also captured.
		 */
		hammer2_vfs_sync(mp, MNT_WAIT);
		hammer2_vfs_sync(mp, MNT_WAIT);
		hammer2_vfs_sync(mp, MNT_WAIT);
	}

	/*
	 * Cleanup the frontend support XOPS threads
	 */
	hammer2_xop_helper_cleanup(pmp);

	if (pmp->mp)
		hammer2_unmount_helper(mp, pmp, NULL);

	error = 0;
failed:
	lockmgr(&hammer2_mntlk, LK_RELEASE);

	return (error);
}

/*
 * Mount helper, hook the system mount into our PFS.
 * The mount lock is held.
 *
 * We must bump the mount_count on related devices for any
 * mounted PFSs.
 */
static
void
hammer2_mount_helper(struct mount *mp, hammer2_pfs_t *pmp)
{
	hammer2_cluster_t *cluster;
	hammer2_chain_t *rchain;
	int i;

	mp->mnt_data = (qaddr_t)pmp;
	pmp->mp = mp;

	/*
	 * After pmp->mp is set we have to adjust hmp->mount_count.
	 */
	cluster = &pmp->iroot->cluster;
	for (i = 0; i < cluster->nchains; ++i) {
		rchain = cluster->array[i].chain;
		if (rchain == NULL)
			continue;
		++rchain->hmp->mount_count;
	}

	/*
	 * Create missing Xop threads
	 */
	hammer2_xop_helper_create(pmp);
}

/*
 * Unmount helper, unhook the system mount from our PFS.
 * The mount lock is held.
 *
 * If hmp is supplied a mount responsible for being the first to open
 * the block device failed and the block device and all PFSs using the
 * block device must be cleaned up.
 *
 * If pmp is supplied multiple devices might be backing the PFS and each
 * must be disconnected.  This might not be the last PFS using some of the
 * underlying devices.  Also, we have to adjust our hmp->mount_count
 * accounting for the devices backing the pmp which is now undergoing an
 * unmount.
 */
static
void
hammer2_unmount_helper(struct mount *mp, hammer2_pfs_t *pmp, hammer2_dev_t *hmp)
{
	hammer2_cluster_t *cluster;
	hammer2_chain_t *rchain;
	int i;

	/*
	 * If no device supplied this is a high-level unmount and we have to
	 * to disconnect the mount, adjust mount_count, and locate devices
	 * that might now have no mounts.
	 */
	if (pmp) {
		KKASSERT(hmp == NULL);
		KKASSERT(MPTOPMP(mp) == pmp);
		pmp->mp = NULL;
		mp->mnt_data = NULL;

		/*
		 * After pmp->mp is cleared we have to account for
		 * mount_count.
		 */
		cluster = &pmp->iroot->cluster;
		for (i = 0; i < cluster->nchains; ++i) {
			rchain = cluster->array[i].chain;
			if (rchain == NULL)
				continue;
			--rchain->hmp->mount_count;
			/* scrapping hmp now may invalidate the pmp */
		}
again:
		TAILQ_FOREACH(hmp, &hammer2_mntlist, mntentry) {
			if (hmp->mount_count == 0) {
				hammer2_unmount_helper(NULL, NULL, hmp);
				goto again;
			}
		}
		return;
	}

	/*
	 * Try to terminate the block device.  We can't terminate it if
	 * there are still PFSs referencing it.
	 */
	if (hmp->mount_count)
		return;

	/*
	 * Decomission the network before we start messing with the
	 * device and PFS.
	 */
	hammer2_iocom_uninit(hmp);

	hammer2_bulkfree_uninit(hmp);
	hammer2_iowd_stop(hmp);

	/*
	 * Drain and stop the self-heal repair kthread while all devvps
	 * are still open (queued repair writes must land before the
	 * final flush below).
	 */
	hammer2_pq_stop(hmp);
	hammer2_io_repair_stop(hmp);

	/* a failure queued by a read completion must reach the final flush */
	hammer2_raid6_persist_failed(hmp);

	hammer2_pfsfree_scan(hmp, 0);

	/*
	 * Cycle the volume data lock as a safety (probably not needed any
	 * more).  To ensure everything is out we need to flush at least
	 * three times.  (1) The running of the sideq can dirty the
	 * filesystem, (2) A normal flush can dirty the freemap, and
	 * (3) ensure that the freemap is fully synchronized.
	 *
	 * The next mount's recovery scan can clean everything up but we want
	 * to leave the filesystem in a 100% clean state on a normal unmount.
	 */
#if 0
	hammer2_voldata_lock(hmp);
	hammer2_voldata_unlock(hmp);
#endif

	/*
	 * Flush whatever is left.  Unmounted but modified PFS's might still
	 * have some dirty chains on them.
	 */
	hammer2_chain_lock(&hmp->vchain, HAMMER2_RESOLVE_ALWAYS);
	hammer2_chain_lock(&hmp->fchain, HAMMER2_RESOLVE_ALWAYS);

	if (hmp->fchain.flags & HAMMER2_CHAIN_FLUSH_MASK) {
		hammer2_voldata_modify(hmp);
		hammer2_flush(&hmp->fchain, HAMMER2_FLUSH_TOP |
					    HAMMER2_FLUSH_ALL);
	}
	hammer2_chain_unlock(&hmp->fchain);

	if (hmp->vchain.flags & HAMMER2_CHAIN_FLUSH_MASK) {
		hammer2_flush(&hmp->vchain, HAMMER2_FLUSH_TOP |
					    HAMMER2_FLUSH_ALL);
	}
	hammer2_chain_unlock(&hmp->vchain);

	if ((hmp->vchain.flags | hmp->fchain.flags) &
	    HAMMER2_CHAIN_FLUSH_MASK) {
		kprintf("hammer2_unmount: chains left over after final sync\n");
		kprintf("    vchain %08x\n", hmp->vchain.flags);
		kprintf("    fchain %08x\n", hmp->fchain.flags);

		if (hammer2_debug & 0x0010)
			Debugger("entered debugger");
	}

	hammer2_pfsfree_scan(hmp, 1);

	KKASSERT(hmp->spmp == NULL);

	/* mirror copies queued by the final flushes, while the disks are open */
	if (hammer2_raid6_v3(hmp) && !hmp->ronly)
		hammer2_io_metadata_mirror_flush(hmp);

	/*
	 * Finish up with the device vnode
	 */
	if (!TAILQ_EMPTY(&hmp->devvpl)) {
		hammer2_close_devvp(hmp, &hmp->devvpl, hmp->ronly);
		hammer2_cleanup_devvp(&hmp->devvpl);
	}
	KKASSERT(TAILQ_EMPTY(&hmp->devvpl));

	/*
	 * Clear vchain/fchain flags that might prevent final cleanup
	 * of these chains.
	 */
	if (hmp->vchain.flags & HAMMER2_CHAIN_MODIFIED) {
		atomic_add_long(&hammer2_count_modified_chains, -1);
		atomic_clear_int(&hmp->vchain.flags, HAMMER2_CHAIN_MODIFIED);
		hammer2_pfs_memory_wakeup(hmp->vchain.pmp, -1);
	}
	if (hmp->vchain.flags & HAMMER2_CHAIN_UPDATE) {
		atomic_clear_int(&hmp->vchain.flags, HAMMER2_CHAIN_UPDATE);
	}

	if (hmp->fchain.flags & HAMMER2_CHAIN_MODIFIED) {
		atomic_add_long(&hammer2_count_modified_chains, -1);
		atomic_clear_int(&hmp->fchain.flags, HAMMER2_CHAIN_MODIFIED);
		hammer2_pfs_memory_wakeup(hmp->fchain.pmp, -1);
	}
	if (hmp->fchain.flags & HAMMER2_CHAIN_UPDATE) {
		atomic_clear_int(&hmp->fchain.flags, HAMMER2_CHAIN_UPDATE);
	}

#if 0 /* kept for future debug */
	{
		int dumpcnt;

		dumpcnt = 50;
		hammer2_dump_chain(&hmp->vchain, 0, 0, &dumpcnt, 'v', (u_int)-1);
		dumpcnt = 50;
		hammer2_dump_chain(&hmp->fchain, 0, 0, &dumpcnt, 'f', (u_int)-1);
	}
#endif

	/*
	 * Final drop of embedded freemap root chain to
	 * clean up fchain.core (fchain structure is not
	 * flagged ALLOCATED so it is cleaned out and then
	 * left to rot).
	 */
	hammer2_chain_drop(&hmp->fchain);

	/*
	 * Final drop of embedded volume root chain to clean
	 * up vchain.core (vchain structure is not flagged
	 * ALLOCATED so it is cleaned out and then left to
	 * rot).
	 */
	hammer2_chain_drop(&hmp->vchain);

	hammer2_io_hash_cleanup_all(hmp);
	if (hmp->iofree_count) {
		kprintf("io_cleanup: %d I/O's left hanging\n",
			hmp->iofree_count);
	}

	TAILQ_REMOVE(&hammer2_mntlist, hmp, mntentry);
	kmalloc_destroy_obj(&hmp->mchain);
	kmalloc_destroy_obj(&hmp->mio);
	kmalloc_destroy(&hmp->mmsg);

	if (hmp->stripe_bitmap)
		spin_uninit(&hmp->stripe_bitmap_spin);
	hammer2_raid6_sm_destroy(hmp);
	hammer2_raid6_open_rows_free(hmp);
	spin_uninit(&hmp->mirror_spin);

	kfree(hmp, M_HAMMER2);
}

int
hammer2_vfs_vget(struct mount *mp, struct vnode *dvp,
		 ino_t ino, struct vnode **vpp)
{
	hammer2_xop_lookup_t *xop;
	hammer2_pfs_t *pmp;
	hammer2_inode_t *ip;
	hammer2_tid_t inum;
	int error;

	inum = (hammer2_tid_t)ino & HAMMER2_DIRHASH_USERMSK;

	error = 0;
	pmp = MPTOPMP(mp);

	/*
	 * Easy if we already have it cached
	 */
	ip = hammer2_inode_lookup(pmp, inum);
	if (ip) {
		hammer2_inode_lock(ip, HAMMER2_RESOLVE_SHARED);
		*vpp = hammer2_igetv(ip, &error);
		hammer2_inode_unlock(ip);
		hammer2_inode_drop(ip);		/* from lookup */

		return error;
	}

	/*
	 * Otherwise we have to find the inode
	 */
	xop = hammer2_xop_alloc(pmp->iroot, 0);
	xop->lhc = inum;
	hammer2_xop_start(&xop->head, &hammer2_lookup_desc);
	error = hammer2_xop_collect(&xop->head, 0);

	if (error == 0)
		ip = hammer2_inode_get(pmp, &xop->head, -1, -1);
	hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);

	if (ip) {
		*vpp = hammer2_igetv(ip, &error);
		hammer2_inode_unlock(ip);
	} else {
		*vpp = NULL;
		error = ENOENT;
	}
	return (error);
}

static
int
hammer2_vfs_root(struct mount *mp, struct vnode **vpp)
{
	hammer2_pfs_t *pmp;
	struct vnode *vp;
	int error;

	pmp = MPTOPMP(mp);
	if (pmp->iroot == NULL) {
		kprintf("hammer2 (%s): no root inode\n",
			mp->mnt_stat.f_mntfromname);
		*vpp = NULL;
		return EINVAL;
	}

	error = 0;
	hammer2_inode_lock(pmp->iroot, HAMMER2_RESOLVE_SHARED);

	while (pmp->inode_tid == 0) {
		hammer2_xop_ipcluster_t *xop;
		const hammer2_inode_meta_t *meta;

		xop = hammer2_xop_alloc(pmp->iroot, HAMMER2_XOP_MODIFYING);
		hammer2_xop_start(&xop->head, &hammer2_ipcluster_desc);
		error = hammer2_xop_collect(&xop->head, 0);

		if (error == 0) {
			meta = &hammer2_xop_gdata(&xop->head)->ipdata.meta;
			pmp->iroot->meta = *meta;
			pmp->inode_tid = meta->pfs_inum + 1;
			hammer2_xop_pdata(&xop->head);
			/* meta invalid */

			if (pmp->inode_tid < HAMMER2_INODE_START)
				pmp->inode_tid = HAMMER2_INODE_START;
			pmp->modify_tid =
				xop->head.cluster.focus->bref.modify_tid + 1;
#if 0
			kprintf("PFS: Starting inode %jd\n",
				(intmax_t)pmp->inode_tid);
			kprintf("PMP focus good set nextino=%ld mod=%016jx\n",
				pmp->inode_tid, pmp->modify_tid);
#endif
			wakeup(&pmp->iroot);

			hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);

			/*
			 * Prime the mount info.
			 */
			hammer2_vfs_statfs(mp, &mp->mnt_stat, NULL);
			break;
		}

		/*
		 * Loop, try again
		 */
		hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);
		hammer2_inode_unlock(pmp->iroot);
		error = tsleep(&pmp->iroot, PCATCH, "h2root", hz);
		hammer2_inode_lock(pmp->iroot, HAMMER2_RESOLVE_SHARED);
		if (error == EINTR)
			break;
	}

	if (error) {
		hammer2_inode_unlock(pmp->iroot);
		*vpp = NULL;
	} else {
		vp = hammer2_igetv(pmp->iroot, &error);
		hammer2_inode_unlock(pmp->iroot);
		*vpp = vp;
	}

	return (error);
}

/*
 * Filesystem status
 *
 * XXX incorporate ipdata->meta.inode_quota and data_quota
 */
static
int
hammer2_vfs_statfs(struct mount *mp, struct statfs *sbp, struct ucred *cred)
{
	hammer2_pfs_t *pmp;
	hammer2_dev_t *hmp;
	hammer2_blockref_t bref;
	struct statfs tmp;
	hammer2_off_t space_total;
	hammer2_off_t space_free;
	hammer2_off_t space_rsv;
	int i;

	/*
	 * NOTE: iroot might not have validated the cluster yet.
	 */
	pmp = MPTOPMP(mp);

	bzero(&tmp, sizeof(tmp));

	for (i = 0; i < pmp->iroot->cluster.nchains; ++i) {
		hmp = pmp->pfs_hmps[i];
		if (hmp == NULL)
			continue;
		if (pmp->iroot->cluster.array[i].chain)
			bref = pmp->iroot->cluster.array[i].chain->bref;
		else
			bzero(&bref, sizeof(bref));

		tmp.f_files = bref.embed.stats.inode_count;
		tmp.f_ffree = 0;
		if (hammer2_raid6_v3(hmp)) {
			hammer2_raid6_space(hmp, &space_total, &space_free);
			space_rsv = space_total / 20;
		} else {
			space_total = hmp->voldata.allocator_size;
			space_free = hmp->voldata.allocator_free;
			space_rsv = hmp->free_reserved;
		}
		tmp.f_blocks = space_total / mp->mnt_vstat.f_bsize;
		tmp.f_bfree = space_free / mp->mnt_vstat.f_bsize;
		tmp.f_bavail = tmp.f_bfree;

		if (cred && cred->cr_uid != 0) {
			uint64_t adj;

			/* 5% */
			adj = space_rsv / mp->mnt_vstat.f_bsize;
			tmp.f_blocks -= adj;
			tmp.f_bfree -= adj;
			tmp.f_bavail -= adj;
		}

		mp->mnt_stat.f_blocks = tmp.f_blocks;
		mp->mnt_stat.f_bfree = tmp.f_bfree;
		mp->mnt_stat.f_bavail = tmp.f_bavail;
		mp->mnt_stat.f_files = tmp.f_files;
		mp->mnt_stat.f_ffree = tmp.f_ffree;

		*sbp = mp->mnt_stat;
	}
	return (0);
}

static
int
hammer2_vfs_statvfs(struct mount *mp, struct statvfs *sbp, struct ucred *cred)
{
	hammer2_pfs_t *pmp;
	hammer2_dev_t *hmp;
	hammer2_blockref_t bref;
	struct statvfs tmp;
	hammer2_off_t space_total;
	hammer2_off_t space_free;
	hammer2_off_t space_rsv;
	int i;

	/*
	 * NOTE: iroot might not have validated the cluster yet.
	 */
	pmp = MPTOPMP(mp);
	bzero(&tmp, sizeof(tmp));

	for (i = 0; i < pmp->iroot->cluster.nchains; ++i) {
		hmp = pmp->pfs_hmps[i];
		if (hmp == NULL)
			continue;
		if (pmp->iroot->cluster.array[i].chain)
			bref = pmp->iroot->cluster.array[i].chain->bref;
		else
			bzero(&bref, sizeof(bref));

		tmp.f_files = bref.embed.stats.inode_count;
		tmp.f_ffree = 0;
		if (hammer2_raid6_v3(hmp)) {
			hammer2_raid6_space(hmp, &space_total, &space_free);
			space_rsv = space_total / 20;
		} else {
			space_total = hmp->voldata.allocator_size;
			space_free = hmp->voldata.allocator_free;
			space_rsv = hmp->free_reserved;
		}
		tmp.f_blocks = space_total / mp->mnt_vstat.f_bsize;
		tmp.f_bfree = space_free / mp->mnt_vstat.f_bsize;
		tmp.f_bavail = tmp.f_bfree;

		if (cred && cred->cr_uid != 0) {
			uint64_t adj;

			/* 5% */
			adj = space_rsv / mp->mnt_vstat.f_bsize;
			tmp.f_blocks -= adj;
			tmp.f_bfree -= adj;
			tmp.f_bavail -= adj;
		}

		mp->mnt_vstat.f_blocks = tmp.f_blocks;
		mp->mnt_vstat.f_bfree = tmp.f_bfree;
		mp->mnt_vstat.f_bavail = tmp.f_bavail;
		mp->mnt_vstat.f_files = tmp.f_files;
		mp->mnt_vstat.f_ffree = tmp.f_ffree;

		*sbp = mp->mnt_vstat;
	}
	return (0);
}

/*
 * Mount-time recovery (RW mounts)
 *
 * Updates to the free block table are allowed to lag flushes by one
 * transaction.  In case of a crash, then on a fresh mount we must do an
 * incremental scan of the last committed transaction id and make sure that
 * all related blocks have been marked allocated.
 */
struct hammer2_recovery_elm {
	TAILQ_ENTRY(hammer2_recovery_elm) entry;
	hammer2_chain_t *chain;
	hammer2_tid_t sync_tid;
};

TAILQ_HEAD(hammer2_recovery_list, hammer2_recovery_elm);

struct hammer2_recovery_info {
	struct hammer2_recovery_list list;
	hammer2_tid_t	mtid;
	int	depth;
};

static int hammer2_recovery_scan(hammer2_dev_t *hmp,
			hammer2_chain_t *parent,
			struct hammer2_recovery_info *info,
			hammer2_tid_t sync_tid);

#define HAMMER2_RECOVERY_MAXDEPTH	10

static
int
hammer2_recovery(hammer2_dev_t *hmp)
{
	struct hammer2_recovery_info info;
	struct hammer2_recovery_elm *elm;
	hammer2_chain_t *parent;
	hammer2_tid_t sync_tid;
	hammer2_tid_t mirror_tid;
	int error;

	hammer2_trans_init(hmp->spmp, 0);

	sync_tid = hmp->voldata.freemap_tid;
	mirror_tid = hmp->voldata.mirror_tid;

	kprintf("hammer2_mount: \"%s\": ", hmp->devrepname);
	if (sync_tid >= mirror_tid) {
		kprintf("no recovery needed\n");
	} else {
		kprintf("freemap recovery %016jx-%016jx\n",
			sync_tid + 1, mirror_tid);
	}

	TAILQ_INIT(&info.list);
	info.depth = 0;
	parent = hammer2_chain_lookup_init(&hmp->vchain, 0);
	error = hammer2_recovery_scan(hmp, parent, &info, sync_tid);
	hammer2_chain_lookup_done(parent);

	while ((elm = TAILQ_FIRST(&info.list)) != NULL) {
		TAILQ_REMOVE(&info.list, elm, entry);
		parent = elm->chain;
		sync_tid = elm->sync_tid;
		kfree(elm, M_HAMMER2);

		hammer2_chain_lock(parent, HAMMER2_RESOLVE_ALWAYS);
		error |= hammer2_recovery_scan(hmp, parent, &info,
					      hmp->voldata.freemap_tid);
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);	/* drop elm->chain ref */
	}

	hammer2_trans_done(hmp->spmp, 0);

	return error;
}

static
int
hammer2_recovery_scan(hammer2_dev_t *hmp, hammer2_chain_t *parent,
		      struct hammer2_recovery_info *info,
		      hammer2_tid_t sync_tid)
{
	const hammer2_inode_data_t *ripdata;
	hammer2_chain_t *chain;
	hammer2_blockref_t bref;
	int tmp_error;
	int rup_error;
	int error;
	int first;

	/*
	 * Adjust freemap to ensure that the block(s) are marked allocated.
	 */
	if (parent->bref.type != HAMMER2_BREF_TYPE_VOLUME) {
		hammer2_freemap_adjust(hmp, &parent->bref,
				       HAMMER2_FREEMAP_DORECOVER);
	}

	/*
	 * Check type for recursive scan
	 */
	switch(parent->bref.type) {
	case HAMMER2_BREF_TYPE_VOLUME:
		/* data already instantiated */
		break;
	case HAMMER2_BREF_TYPE_INODE:
		/*
		 * Must instantiate data for DIRECTDATA test and also
		 * for recursion.
		 */
		hammer2_chain_lock(parent, HAMMER2_RESOLVE_ALWAYS);
		ripdata = &parent->data->ipdata;
		if (ripdata->meta.op_flags & HAMMER2_OPFLAG_DIRECTDATA) {
			/* not applicable to recovery scan */
			hammer2_chain_unlock(parent);
			return 0;
		}
		hammer2_chain_unlock(parent);
		break;
	case HAMMER2_BREF_TYPE_INDIRECT:
		/*
		 * Must instantiate data for recursion
		 */
		hammer2_chain_lock(parent, HAMMER2_RESOLVE_ALWAYS);
		hammer2_chain_unlock(parent);
		break;
	case HAMMER2_BREF_TYPE_DIRENT:
	case HAMMER2_BREF_TYPE_DATA:
	case HAMMER2_BREF_TYPE_FREEMAP:
	case HAMMER2_BREF_TYPE_FREEMAP_NODE:
	case HAMMER2_BREF_TYPE_FREEMAP_LEAF:
		/* not applicable to recovery scan */
		return 0;
		break;
	default:
		return HAMMER2_ERROR_BADBREF;
	}

	/*
	 * Defer operation if depth limit reached.
	 */
	if (info->depth >= HAMMER2_RECOVERY_MAXDEPTH) {
		struct hammer2_recovery_elm *elm;

		elm = kmalloc(sizeof(*elm), M_HAMMER2, M_ZERO | M_WAITOK);
		elm->chain = parent;
		elm->sync_tid = sync_tid;
		hammer2_chain_ref(parent);
		TAILQ_INSERT_TAIL(&info->list, elm, entry);
		/* unlocked by caller */

		return(0);
	}


	/*
	 * Recursive scan of the last flushed transaction only.  We are
	 * doing this without pmp assignments so don't leave the chains
	 * hanging around after we are done with them.
	 *
	 * error	Cumulative error this level only
	 * rup_error	Cumulative error for recursion
	 * tmp_error	Specific non-cumulative recursion error
	 */
	chain = NULL;
	first = 1;
	rup_error = 0;
	error = 0;

	for (;;) {
		error |= hammer2_chain_scan(parent, &chain, &bref,
					    &first,
					    HAMMER2_LOOKUP_NODATA);

		/*
		 * Problem during scan or EOF
		 */
		if (error)
			break;

		/*
		 * If this is a leaf
		 */
		if (chain == NULL) {
			if (bref.mirror_tid > sync_tid) {
				hammer2_freemap_adjust(hmp, &bref,
						     HAMMER2_FREEMAP_DORECOVER);
			}
			continue;
		}

		/*
		 * This may or may not be a recursive node.
		 */
		atomic_set_int(&chain->flags, HAMMER2_CHAIN_RELEASE);
		if (bref.mirror_tid > sync_tid) {
			++info->depth;
			tmp_error = hammer2_recovery_scan(hmp, chain,
							   info, sync_tid);
			--info->depth;
		} else {
			tmp_error = 0;
		}

		/*
		 * Flush the recovery at the PFS boundary to stage it for
		 * the final flush of the super-root topology.
		 */
		if (tmp_error == 0 &&
		    (bref.flags & HAMMER2_BREF_FLAG_PFSROOT) &&
		    (chain->flags & HAMMER2_CHAIN_ONFLUSH)) {
			hammer2_flush(chain, HAMMER2_FLUSH_TOP |
					     HAMMER2_FLUSH_ALL);
		}
		rup_error |= tmp_error;
	}
	return ((error | rup_error) & ~HAMMER2_ERROR_EOF);
}

/*
 * This fixes up an error introduced in earlier H2 implementations where
 * moving a PFS inode into an indirect block wound up causing the
 * HAMMER2_BREF_FLAG_PFSROOT flag in the bref to get cleared.
 */
static
int
hammer2_fixup_pfses(hammer2_dev_t *hmp)
{
	const hammer2_inode_data_t *ripdata;
	hammer2_chain_t *parent;
	hammer2_chain_t *chain;
	hammer2_key_t key_next;
	hammer2_pfs_t *spmp;
	int error;

	error = 0;

	/*
	 * Lookup mount point under the media-localized super-root.
	 *
	 * cluster->pmp will incorrectly point to spmp and must be fixed
	 * up later on.
	 */
	spmp = hmp->spmp;
	hammer2_inode_lock(spmp->iroot, 0);
	parent = hammer2_inode_chain(spmp->iroot, 0, HAMMER2_RESOLVE_ALWAYS);
	chain = hammer2_chain_lookup(&parent, &key_next,
					 HAMMER2_KEY_MIN, HAMMER2_KEY_MAX,
					 &error, 0);
	while (chain) {
		if (chain->bref.type != HAMMER2_BREF_TYPE_INODE)
			continue;
		if (chain->error) {
			kprintf("I/O error scanning PFS labels\n");
			error |= chain->error;
		} else if ((chain->bref.flags &
			    HAMMER2_BREF_FLAG_PFSROOT) == 0) {
			int error2;

			ripdata = &chain->data->ipdata;
			hammer2_trans_init(hmp->spmp, 0);
			error2 = hammer2_chain_modify(chain,
						      chain->bref.modify_tid,
						      0, 0);
			if (error2 == 0) {
				kprintf("hammer2: Correct mis-flagged PFS %s\n",
					ripdata->filename);
				chain->bref.flags |= HAMMER2_BREF_FLAG_PFSROOT;
			} else {
				error |= error2;
			}
			hammer2_flush(chain, HAMMER2_FLUSH_TOP |
					     HAMMER2_FLUSH_ALL);
			hammer2_trans_done(hmp->spmp, 0);
		}
		chain = hammer2_chain_next(&parent, chain, &key_next,
					   key_next, HAMMER2_KEY_MAX,
					   &error, 0);
	}
	if (parent) {
		hammer2_chain_unlock(parent);
		hammer2_chain_drop(parent);
	}
	hammer2_inode_unlock(spmp->iroot);

	return error;
}

/*
 * Sync a mount point; this is called periodically on a per-mount basis from
 * the filesystem syncer, and whenever a user issues a sync.
 */
int
hammer2_vfs_sync(struct mount *mp, int waitfor)
{
	int error;

	error = hammer2_vfs_sync_pmp(MPTOPMP(mp), waitfor);

	return error;
}

/*
 * Because frontend operations lock vnodes before we get a chance to
 * lock the related inode, we can't just acquire a vnode lock without
 * risking a deadlock.  The frontend may be holding a vnode lock while
 * also blocked on our SYNCQ flag while trying to get the inode lock.
 *
 * To deal with this situation we can check the vnode lock situation
 * after locking the inode and perform a work-around.
 */
int
hammer2_vfs_sync_pmp(hammer2_pfs_t *pmp, int waitfor)
{
	hammer2_inode_t *ip;
	hammer2_depend_t *depend;
	hammer2_depend_t *depend_next;
	struct vnode *vp;
	uint32_t pass2;
	int error;
	int wakecount;
	int dorestart;

	/*
	 * Move all inodes on sideq to syncq.  This will clear sideq.
	 * This should represent all flushable inodes.  These inodes
	 * will already have refs due to being on syncq or sideq.  We
	 * must do this all at once with the spinlock held to ensure that
	 * all inode dependencies are part of the same flush.
	 *
	 * We should be able to do this asynchronously from frontend
	 * operations because we will be locking the inodes later on
	 * to actually flush them, and that will partition any frontend
	 * op using the same inode.  Either it has already locked the
	 * inode and we will block, or it has not yet locked the inode
	 * and it will block until we are finished flushing that inode.
	 *
	 * When restarting, only move the inodes flagged as PASS2 from
	 * SIDEQ to SYNCQ.  PASS2 propagation by inode_lock4() and
	 * inode_depend() are atomic with the spin-lock.
	 */
	hammer2_trans_init(pmp, HAMMER2_TRANS_ISFLUSH);
#ifdef HAMMER2_DEBUG_SYNC
	kprintf("FILESYSTEM SYNC BOUNDARY\n");
#endif
	dorestart = 0;

	/*
	 * Move inodes from depq to syncq, releasing the related
	 * depend structures.
	 */
restart:
#ifdef HAMMER2_DEBUG_SYNC
	kprintf("FILESYSTEM SYNC RESTART (%d)\n", dorestart);
#endif
	hammer2_trans_setflags(pmp, 0/*HAMMER2_TRANS_COPYQ*/);
	hammer2_trans_clearflags(pmp, HAMMER2_TRANS_RESCAN);

	/*
	 * Move inodes from depq to syncq.  When restarting, only depq's
	 * marked pass2 are moved.
	 */
	hammer2_spin_ex(&pmp->list_spin);
	depend_next = TAILQ_FIRST(&pmp->depq);
	wakecount = 0;

	while ((depend = depend_next) != NULL) {
		depend_next = TAILQ_NEXT(depend, entry);
		if (dorestart && depend->pass2 == 0)
			continue;
		TAILQ_FOREACH(ip, &depend->sideq, entry) {
			KKASSERT(ip->flags & HAMMER2_INODE_SIDEQ);
			atomic_set_int(&ip->flags, HAMMER2_INODE_SYNCQ);
			atomic_clear_int(&ip->flags, HAMMER2_INODE_SIDEQ);
			ip->depend = NULL;
		}

		/*
		 * NOTE: pmp->sideq_count includes both sideq and syncq
		 */
		TAILQ_CONCAT(&pmp->syncq, &depend->sideq, entry);

		depend->count = 0;
		depend->pass2 = 0;
		TAILQ_REMOVE(&pmp->depq, depend, entry);
	}

	hammer2_spin_unex(&pmp->list_spin);
	hammer2_trans_clearflags(pmp, /*HAMMER2_TRANS_COPYQ |*/
				      HAMMER2_TRANS_WAITING);
	dorestart = 0;

	/*
	 * sideq_count may have dropped enough to allow us to unstall
	 * the frontend.
	 */
	hammer2_pfs_memory_wakeup(pmp, 0);

	/*
	 * Now run through all inodes on syncq.
	 *
	 * Flush transactions only interlock with other flush transactions.
	 * Any conflicting frontend operations will block on the inode, but
	 * may hold a vnode lock while doing so.
	 */
	hammer2_spin_ex(&pmp->list_spin);
	while ((ip = TAILQ_FIRST(&pmp->syncq)) != NULL) {
		/*
		 * Remove the inode from the SYNCQ, transfer the syncq ref
		 * to us.  We must clear SYNCQ to allow any potential
		 * front-end deadlock to proceed.  We must set PASS2 so
		 * the dependency code knows what to do.
		 */
		pass2 = ip->flags;
		cpu_ccfence();
		if (atomic_cmpset_int(&ip->flags,
			      pass2,
			      (pass2 & ~(HAMMER2_INODE_SYNCQ |
					 HAMMER2_INODE_SYNCQ_WAKEUP)) |
			      HAMMER2_INODE_SYNCQ_PASS2) == 0)
		{
			continue;
		}
		TAILQ_REMOVE(&pmp->syncq, ip, entry);
		--pmp->sideq_count;
		hammer2_spin_unex(&pmp->list_spin);

		/*
		 * Tickle anyone waiting on ip->flags or the hysteresis
		 * on the dirty inode count.
		 */
		if (pass2 & HAMMER2_INODE_SYNCQ_WAKEUP)
			wakeup(&ip->flags);
		if (++wakecount >= hammer2_limit_dirty_inodes / 20 + 1) {
			wakecount = 0;
			hammer2_pfs_memory_wakeup(pmp, 0);
		}

		/*
		 * Relock the inode, and we inherit a ref from the above.
		 * We will check for a race after we acquire the vnode.
		 */
		hammer2_mtx_ex(&ip->lock);

		/*
		 * We need the vp in order to vfsync() dirty buffers, so if
		 * one isn't attached we can skip it.
		 *
		 * Ordering the inode lock and then the vnode lock has the
		 * potential to deadlock.  If we had left SYNCQ set that could
		 * also deadlock us against the frontend even if we don't hold
		 * any locks, but the latter is not a problem now since we
		 * cleared it.  igetv will temporarily release the inode lock
		 * in a safe manner to work-around the deadlock.
		 *
		 * Unfortunately it is still possible to deadlock when the
		 * frontend obtains multiple inode locks, because all the
		 * related vnodes are already locked (nor can the vnode locks
		 * be released and reacquired without messing up RECLAIM and
		 * INACTIVE sequencing).
		 *
		 * The solution for now is to move the vp back onto SIDEQ
		 * and set dorestart, which will restart the flush after we
		 * exhaust the current SYNCQ.  Note that additional
		 * dependencies may build up, so we definitely need to move
		 * the whole SIDEQ back to SYNCQ when we restart.
		 */
		vp = ip->vp;
		if (vp) {
			if (vget(vp, LK_EXCLUSIVE|LK_NOWAIT)) {
				/*
				 * Failed to get the vnode, requeue the inode
				 * (PASS2 is already set so it will be found
				 * again on the restart).  Then unlock.
				 */
				vp = NULL;
				dorestart |= 1;
#ifdef HAMMER2_DEBUG_SYNC
				kprintf("inum %ld (sync delayed by vnode)\n",
					(long)ip->meta.inum);
#endif
				hammer2_inode_delayed_sideq(ip);

				hammer2_mtx_unlock(&ip->lock);
				hammer2_inode_drop(ip);

				/*
				 * If PASS2 was previously set we might
				 * be looping too hard, ask for a delay
				 * along with the restart.
				 */
				if (pass2 & HAMMER2_INODE_SYNCQ_PASS2)
					dorestart |= 2;
				hammer2_spin_ex(&pmp->list_spin);
				continue;
			}
		} else {
			vp = NULL;
		}

		/*
		 * If the inode wound up on a SIDEQ again it will already be
		 * prepped for another PASS2.  In this situation if we flush
		 * it now we will just wind up flushing it again in the same
		 * syncer run, so we might as well not flush it now.
		 */
		if (ip->flags & HAMMER2_INODE_SIDEQ) {
			hammer2_mtx_unlock(&ip->lock);
			hammer2_inode_drop(ip);
			if (vp)
				vput(vp);
			dorestart |= 1;
			hammer2_spin_ex(&pmp->list_spin);
			continue;
		}

		/*
		 * Ok we have the inode exclusively locked and if vp is
		 * not NULL that will also be exclusively locked.  Do the
		 * meat of the flush.
		 *
		 * vp token needed for v_rbdirty_tree check / vclrisdirty
		 * sequencing.  Though we hold the vnode exclusively so
		 * we shouldn't need to hold the token also in this case.
		 */
		if (vp) {
			vfsync(vp, MNT_WAIT, 1, NULL, NULL);
			bio_track_wait(&vp->v_track_write, 0, 0); /* XXX */
		}

		/*
		 * If the inode has not yet been inserted into the tree
		 * we must do so.  Then sync and flush it.  The flush should
		 * update the parent.
		 */
		if (ip->flags & HAMMER2_INODE_DELETING) {
#ifdef HAMMER2_DEBUG_SYNC
			kprintf("inum %ld destroy\n", (long)ip->meta.inum);
#endif
			hammer2_inode_chain_des(ip);
			atomic_add_long(&hammer2_iod_inode_deletes, 1);
		} else if (ip->flags & HAMMER2_INODE_CREATING) {
#ifdef HAMMER2_DEBUG_SYNC
			kprintf("inum %ld insert\n", (long)ip->meta.inum);
#endif
			hammer2_inode_chain_ins(ip);
			atomic_add_long(&hammer2_iod_inode_creates, 1);
		}
#ifdef HAMMER2_DEBUG_SYNC
		kprintf("inum %ld chain-sync\n", (long)ip->meta.inum);
#endif

		/*
		 * Because I kinda messed up the design and index the inodes
		 * under the root inode, along side the directory entries,
		 * we can't flush the inode index under the iroot until the
		 * end.  If we do it now we might miss effects created by
		 * other inodes on the SYNCQ.
		 *
		 * Do a normal (non-FSSYNC) flush instead, which allows the
		 * vnode code to work the same.  We don't want to force iroot
		 * back onto the SIDEQ, and we also don't want the flush code
		 * to update pfs_iroot_blocksets until the final flush later.
		 *
		 * XXX at the moment this will likely result in a double-flush
		 * of the iroot chain.
		 */
		hammer2_inode_chain_sync(ip);
		if (ip == pmp->iroot) {
			hammer2_inode_chain_flush(ip, HAMMER2_XOP_INODE_STOP);
		} else {
			hammer2_inode_chain_flush(ip, HAMMER2_XOP_INODE_STOP |
						      HAMMER2_XOP_FSSYNC);
		}
		if (vp) {
			lwkt_gettoken(&vp->v_token);
			if ((ip->flags & (HAMMER2_INODE_MODIFIED |
					  HAMMER2_INODE_RESIZED |
					  HAMMER2_INODE_DIRTYDATA)) == 0 &&
			    RB_EMPTY(&vp->v_rbdirty_tree) &&
			    !bio_track_active(&vp->v_track_write))
			{
				vclrisdirty(vp);
			} else {
				hammer2_inode_delayed_sideq(ip);
			}
			lwkt_reltoken(&vp->v_token);
			vput(vp);
			vp = NULL;	/* safety */
		}
		atomic_clear_int(&ip->flags, HAMMER2_INODE_SYNCQ_PASS2);
		hammer2_inode_unlock(ip);	/* unlock+drop */
		/* ip pointer invalid */

		/*
		 * If the inode got dirted after we dropped our locks,
		 * it will have already been moved back to the SIDEQ.
		 */
		hammer2_spin_ex(&pmp->list_spin);
	}
	hammer2_spin_unex(&pmp->list_spin);
	hammer2_pfs_memory_wakeup(pmp, 0);

	if (dorestart || (pmp->trans.flags & HAMMER2_TRANS_RESCAN)) {
		/*
		 * bit 2 is set if something above thinks we might be
		 * looping too hard, try to unclog the frontend
		 * dependency and wait a bit before restarting.
		 *
		 * NOTE: The frontend could be stuck in h2memw, though
		 *	 it isn't supposed to be holding vnode locks
		 *	 in that case.
		 */
		if (dorestart & 2) {
			wakeup(&pmp->inmem_dirty_chains);
			tsleep(&dorestart, 0, "h2syndel", 2);
		}
#ifdef HAMMER2_DEBUG_SYNC
		kprintf("FILESYSTEM SYNC STAGE 1 RESTART\n");
		/*tsleep(&dorestart, 0, "h2STG1-R", hz*20);*/
#endif
		dorestart = 1;
		goto restart;
	}
#ifdef HAMMER2_DEBUG_SYNC
	kprintf("FILESYSTEM SYNC STAGE 2 BEGIN\n");
	/*tsleep(&dorestart, 0, "h2STG2", hz*20);*/
#endif

	/*
	 * We have to flush the PFS root last, even if it does not appear to
	 * be dirty, because all the inodes in the PFS are indexed under it.
	 * The normal flushing of iroot above would only occur if directory
	 * entries under the root were changed.
	 *
	 * Specifying VOLHDR will cause an additionl flush of hmp->spmp
	 * for the media making up the cluster.
	 */
	if ((ip = pmp->iroot) != NULL) {
		hammer2_inode_ref(ip);
		hammer2_mtx_ex(&ip->lock);
		hammer2_inode_chain_sync(ip);
		hammer2_inode_chain_flush(ip, HAMMER2_XOP_INODE_STOP |
					      HAMMER2_XOP_FSSYNC |
					      HAMMER2_XOP_VOLHDR);
		hammer2_inode_unlock(ip);	/* unlock+drop */
	}
#ifdef HAMMER2_DEBUG_SYNC
	kprintf("FILESYSTEM SYNC STAGE 2 DONE\n");
#endif

	/*
	 * device bioq sync
	 */
	hammer2_bioq_sync(pmp);

	error = 0;	/* XXX */
	hammer2_trans_done(pmp, HAMMER2_TRANS_ISFLUSH);

	return (error);
}

static
int
hammer2_vfs_vptofh(struct vnode *vp, struct fid *fhp)
{
	hammer2_inode_t *ip;

	KKASSERT(MAXFIDSZ >= 16);
	ip = VTOI(vp);
	fhp->fid_len = offsetof(struct fid, fid_data[16]);
	fhp->fid_ext = 0;
	((hammer2_tid_t *)fhp->fid_data)[0] = ip->meta.inum;
	((hammer2_tid_t *)fhp->fid_data)[1] = 0;

	return 0;
}

static
int
hammer2_vfs_fhtovp(struct mount *mp, struct vnode *rootvp,
	       struct fid *fhp, struct vnode **vpp)
{
	hammer2_tid_t inum;
	int error;

	inum = ((hammer2_tid_t *)fhp->fid_data)[0] & HAMMER2_DIRHASH_USERMSK;
	if (vpp) {
		if (inum == 1)
			error = hammer2_vfs_root(mp, vpp);
		else
			error = hammer2_vfs_vget(mp, NULL, inum, vpp);
	} else {
		error = 0;
	}
	return error;
}

static
int
hammer2_vfs_checkexp(struct mount *mp, struct sockaddr *nam,
		 int *exflagsp, struct ucred **credanonp)
{
	hammer2_pfs_t *pmp;
	struct netcred *np;
	int error;

	pmp = MPTOPMP(mp);
	np = vfs_export_lookup(mp, &pmp->export, nam);
	if (np) {
		*exflagsp = np->netc_exflags;
		*credanonp = &np->netc_anon;
		error = 0;
	} else {
		error = EACCES;
	}
	return error;
}

/*
 * This handles hysteresis on regular file flushes.  Because the BIOs are
 * routed to a thread it is possible for an excessive number to build up
 * and cause long front-end stalls long before the runningbuffspace limit
 * is hit, so we implement hammer2_flush_pipe to control the
 * hysteresis.
 *
 * This is a particular problem when compression is used.
 */
void
hammer2_lwinprog_ref(hammer2_pfs_t *pmp)
{
	atomic_add_int(&pmp->count_lwinprog, 1);
}

void
hammer2_lwinprog_drop(hammer2_pfs_t *pmp)
{
	int lwinprog;

	lwinprog = atomic_fetchadd_int(&pmp->count_lwinprog, -1);
	if ((lwinprog & HAMMER2_LWINPROG_WAITING) &&
	    (lwinprog & HAMMER2_LWINPROG_MASK) <= hammer2_flush_pipe * 2 / 3) {
		atomic_clear_int(&pmp->count_lwinprog,
				 HAMMER2_LWINPROG_WAITING);
		wakeup(&pmp->count_lwinprog);
	}
	if ((lwinprog & HAMMER2_LWINPROG_WAITING0) &&
	    (lwinprog & HAMMER2_LWINPROG_MASK) <= 0) {
		atomic_clear_int(&pmp->count_lwinprog,
				 HAMMER2_LWINPROG_WAITING0);
		wakeup(&pmp->count_lwinprog);
	}
}

void
hammer2_lwinprog_wait(hammer2_pfs_t *pmp, int flush_pipe)
{
	int lwinprog;
	int lwflag = (flush_pipe) ? HAMMER2_LWINPROG_WAITING :
				    HAMMER2_LWINPROG_WAITING0;

	for (;;) {
		lwinprog = pmp->count_lwinprog;
		cpu_ccfence();
		if ((lwinprog & HAMMER2_LWINPROG_MASK) <= flush_pipe)
			break;
		tsleep_interlock(&pmp->count_lwinprog, 0);
		atomic_set_int(&pmp->count_lwinprog, lwflag);
		lwinprog = pmp->count_lwinprog;
		if ((lwinprog & HAMMER2_LWINPROG_MASK) <= flush_pipe)
			break;
		tsleep(&pmp->count_lwinprog, PINTERLOCKED, "h2wpipe", hz);
	}
}

/*
 * It is possible for an excessive number of dirty chains or dirty inodes
 * to build up.  When this occurs we start an asynchronous filesystem sync.
 * If the level continues to build up, we stall, waiting for it to drop,
 * with some hysteresis.
 *
 * This relies on the kernel calling hammer2_vfs_modifying() prior to
 * obtaining any vnode locks before making a modifying VOP call.
 */
static int
hammer2_vfs_modifying(struct mount *mp)
{
	if (mp->mnt_flag & MNT_RDONLY)
		return EROFS;
	hammer2_pfs_memory_wait(MPTOPMP(mp));

	return 0;
}

/*
 * Initiate an asynchronous filesystem sync and, with hysteresis,
 * stall if the internal data structure count becomes too bloated.
 */
void
hammer2_pfs_memory_wait(hammer2_pfs_t *pmp)
{
	uint32_t waiting;
	int pcatch;
	int error;
	int started;

	if (pmp == NULL || pmp->mp == NULL)
		return;

	started = 0;

	for (;;) {
		waiting = pmp->inmem_dirty_chains & HAMMER2_DIRTYCHAIN_MASK;
		cpu_ccfence();

		/*
		 * Start the syncer running at 1/2 the limit to try
		 * to avoid sleeping.
		 */
		if (waiting > hammer2_limit_dirty_chains / 2 ||
		    pmp->sideq_count > hammer2_limit_dirty_inodes / 2)
		{
			atomic_add_long(&hammer2_p10_dc_trigger, 1);
			trigger_syncer(pmp->mp);
		}

		/*
		 * Stall at the limit waiting for the counts to drop.
		 * This code will typically be woken up once the count
		 * drops below 3/4 the limit, or in one second.
		 */
		if (waiting < hammer2_limit_dirty_chains &&
		    pmp->sideq_count < hammer2_limit_dirty_inodes)
		{
			break;
		}

		if (started == 0) {
			atomic_add_long(&hammer2_p10_dc_stall, 1);
			trigger_syncer_start(pmp->mp);
			started = 1;
		}

		/*
		 * Interlocked re-test, sleep, and retry.
		 */
		pcatch = curthread->td_proc ? PCATCH : 0;
		tsleep_interlock(&pmp->inmem_dirty_chains, pcatch);

		atomic_set_int(&pmp->inmem_dirty_chains,
			       HAMMER2_DIRTYCHAIN_WAITING);

		if (waiting < hammer2_limit_dirty_chains &&
		    pmp->sideq_count < hammer2_limit_dirty_inodes) {
			break;
		}
		error = tsleep(&pmp->inmem_dirty_chains,
			       PINTERLOCKED | pcatch,
			       "h2memw", hz);
		if (error == ERESTART)
			break;
	}
	if (started)
		trigger_syncer_stop(pmp->mp);
}

/*
 * Wake up any stalled frontend ops waiting, with hysteresis, using
 * 2/3 of the limit.
 */
void
hammer2_pfs_memory_wakeup(hammer2_pfs_t *pmp, int count)
{
	uint32_t waiting;

	if (pmp) {
		waiting = atomic_fetchadd_int(&pmp->inmem_dirty_chains, count);
		/* don't need --waiting to test flag */

		if ((waiting & HAMMER2_DIRTYCHAIN_WAITING) &&
		    (pmp->inmem_dirty_chains & HAMMER2_DIRTYCHAIN_MASK) <=
		    hammer2_limit_dirty_chains * 2 / 3 &&
		    pmp->sideq_count <= hammer2_limit_dirty_inodes * 2 / 3) {
			atomic_clear_int(&pmp->inmem_dirty_chains,
					 HAMMER2_DIRTYCHAIN_WAITING);
			wakeup(&pmp->inmem_dirty_chains);
		}
	}
}

void
hammer2_pfs_memory_inc(hammer2_pfs_t *pmp)
{
	if (pmp) {
		atomic_add_int(&pmp->inmem_dirty_chains, 1);
	}
}

/*
 * Volume header data locks
 */
void
hammer2_voldata_lock(hammer2_dev_t *hmp)
{
	lockmgr(&hmp->vollk, LK_EXCLUSIVE);
}

void
hammer2_voldata_unlock(hammer2_dev_t *hmp)
{
	lockmgr(&hmp->vollk, LK_RELEASE);
}

/*
 * Caller indicates that the volume header is being modified.  Flag
 * the related chain and adjust its transaction id.
 *
 * The transaction id is set to voldata.mirror_tid + 1, similar to
 * what hammer2_chain_modify() does.  Be very careful here, volume
 * data can be updated independently of the rest of the filesystem.
 */
void
hammer2_voldata_modify(hammer2_dev_t *hmp)
{
	if ((hmp->vchain.flags & HAMMER2_CHAIN_MODIFIED) == 0) {
		atomic_add_long(&hammer2_count_modified_chains, 1);
		atomic_set_int(&hmp->vchain.flags, HAMMER2_CHAIN_MODIFIED);
		hammer2_pfs_memory_inc(hmp->vchain.pmp);
		hmp->vchain.bref.mirror_tid = hmp->voldata.mirror_tid + 1;
	}
}

/*
 * Returns 0 if the filesystem has tons of free space
 * Returns 1 if the filesystem has less than 10% remaining
 * Returns 2 if the filesystem has less than 2%/5% (user/root) remaining.
 */
int
hammer2_vfs_enospace(hammer2_inode_t *ip, off_t bytes, struct ucred *cred)
{
	hammer2_pfs_t *pmp;
	hammer2_dev_t *hmp;
	hammer2_off_t free_reserved;
	hammer2_off_t free_nominal;
	int i;

	pmp = ip->pmp;

	if (pmp->free_ticks == 0 || pmp->free_ticks != ticks) {
		free_reserved = HAMMER2_SEGSIZE;
		free_nominal = 0x7FFFFFFFFFFFFFFFLLU;
		for (i = 0; i < pmp->iroot->cluster.nchains; ++i) {
			hmp = pmp->pfs_hmps[i];
			if (hmp == NULL)
				continue;
			if (pmp->pfs_types[i] != HAMMER2_PFSTYPE_MASTER &&
			    pmp->pfs_types[i] != HAMMER2_PFSTYPE_SOFT_MASTER)
				continue;

			if (hammer2_raid6_v3(hmp)) {
				/*
				 * On v3 allocator_free is the metadata zone
				 * over the raw size of every disk, it says
				 * nothing about the data rows.  Use the free
				 * data rows against 5% of the data space (as
				 * statfs does).  A flush that cannot get a
				 * metadata block is as bad as a data write
				 * that finds no row, so a metadata zone
				 * under its own 5% reads as full.
				 */
				hammer2_off_t dt, df, mt, mf;

				hammer2_raid6_space_split(hmp, &dt, &df,
							  &mt, &mf);
				if (mf < mt / 20)
					df = 0;
				if (free_nominal > df)
					free_nominal = df;
				if (free_reserved < dt / 20)
					free_reserved = dt / 20;
				continue;
			}
			if (free_nominal > hmp->voldata.allocator_free)
				free_nominal = hmp->voldata.allocator_free;
			if (free_reserved < hmp->free_reserved)
				free_reserved = hmp->free_reserved;
		}

		/*
		 * SMP races ok
		 */
		pmp->free_reserved = free_reserved;
		pmp->free_nominal = free_nominal;
		pmp->free_ticks = ticks;
	} else {
		free_reserved = pmp->free_reserved;
		free_nominal = pmp->free_nominal;
	}
	if (cred && cred->cr_uid != 0) {
		if ((int64_t)(free_nominal - bytes) <
		    (int64_t)free_reserved) {
			return 2;
		}
	} else {
		if ((int64_t)(free_nominal - bytes) <
		    (int64_t)free_reserved / 2) {
			return 2;
		}
	}
	if ((int64_t)(free_nominal - bytes) < (int64_t)free_reserved * 2)
		return 1;
	return 0;
}
