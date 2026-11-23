/*
 * cdimage: a QEMU format block driver over libdisc (doc 17 §5.1).
 *
 * The block layer sees the cooked 2048-byte view of the disc (L-EC
 * verified user data of the data sectors), so qemu-img, SeaBIOS' El Torito
 * boot, blockdev-change-medium and eject keep working; cdimage_disc()
 * hands the raw model to hw/ide/atapi.c for everything an ATAPI drive
 * answers from the disc itself (raw sectors, TOC, subchannel, CD-DA).
 *
 * Probing: libdisc scores .cue and .ccd files 100 and everything else 0,
 * so a plain .iso stays on the raw driver, byte for byte as before.
 * Overlaid into qemu/block/ by scripts/prepare-qemu.sh from libdisc/qemu/;
 * built when meson found liblibdisc.a (-Dlibdisc_dir, patch 50).
 *
 * The same file also registers `isodir`, which serves a host *directory*
 * as a generated ISO 9660 + Joliet volume (M5g, docs/17-cdrom-implementation.md §8).
 * It is a protocol driver, `isodir:/path/to/folder`, because a directory
 * can be neither probed nor opened as a `file` child, the shape vvfat's
 * `fat:` prefix has for the same reason. Everything else is shared with
 * cdimage: the same state, the same reads, the same disc handle reaching
 * hw/ide/atapi.c, so a folder is a disc in the drive like any other.
 *
 * And `hostcd`, the disc in a host's own optical drive (track M5h, doc 17
 * §9): `hostcd:/dev/sr0`. libdisc builds the model from the drive's TOC and
 * reads every sector from the drive; this driver adds what a removable
 * medium needs. It opens with no disc in the drive, and a thread watches
 * the drive (TEST UNIT READY once a second): a disc taken out is the
 * guest's tray opening, a disc put in is opened off the main loop and
 * reaches the guest as a medium change, all through the BlockBackend's
 * change_media callback, as `blockdev-change-medium` would. The guest's
 * eject opens the real tray.
 *
 * Copyright (c) 2026 the win98-xp-virt authors. GPL-2.0-only, like QEMU.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "block/block-io.h"
#include "block/block_int.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qemu/option.h"
#include "qemu/cutils.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "qobject/qdict.h"
#include "block/cdimage.h"
#include "block/libdisc.h"

#define CDIMAGE_SECTOR 2048
#define ISODIR_PREFIX  "isodir:"
#define HOSTCD_PREFIX  "hostcd:"
#define HOSTCD_POLL_MS 1000

/*
 * hostcd's watch: a thread that polls the drive and hands each change to
 * the main loop through `bh`. `pending` (NULL: the disc went) is the
 * thread's to fill and the bottom half's to take, under `lock`.
 */
typedef struct HostcdWatch {
    QemuThread thread;
    QemuSemaphore stop;     /* posted once, at close */
    QemuMutex lock;
    QEMUBH *bh;
    char *dev;              /* "hostcd:/dev/sr0", what libdisc_open takes */
    bool present;           /* the thread's view: a disc is open */
    bool failed;            /* the disc in the drive did not open; said once */
    bool pending_set;
    libdisc *pending;
} HostcdWatch;

typedef struct BDRVCdimageState {
    libdisc *disc;
    uint32_t sectors;       /* lead-out LBA of the last session */
    char *path;             /* the host file or directory `disc` was opened from */
    HostcdWatch *watch;     /* hostcd only */
} BDRVCdimageState;

static BlockDriver bdrv_cdimage;
static BlockDriver bdrv_isodir;
static BlockDriver bdrv_hostcd;

/*
 * Open the medium at `path` (an image file, or a directory for isodir)
 * and take its size. Shared by both drivers: everything that differs is
 * how they got hold of the path.
 */
static int cdimage_open_disc(BlockDriverState *bs, const char *path, Error **errp)
{
    BDRVCdimageState *s = bs->opaque;
    char err[512];

    if (libdisc_api_version() != LIBDISC_API_VERSION) {
        error_setg(errp, "libdisc API version %u, cdimage was built for %u",
                   libdisc_api_version(), LIBDISC_API_VERSION);
        return -EINVAL;
    }
    err[0] = 0;
    s->disc = libdisc_open(path, err, sizeof(err));
    if (!s->disc) {
        error_setg(errp, "%s: %s", bs->drv->format_name,
                   err[0] ? err : "cannot open the medium");
        return -EINVAL;
    }
    s->sectors = libdisc_sector_count(s->disc);
    if (s->sectors == 0) {
        libdisc_close(s->disc);
        s->disc = NULL;
        error_setg(errp, "%s: %s: empty disc", bs->drv->format_name, path);
        return -EINVAL;
    }
    s->path = g_strdup(path);
    bs->total_sectors = (int64_t)s->sectors * CDIMAGE_SECTOR / BDRV_SECTOR_SIZE;
    return 0;
}

static int cdimage_probe(const uint8_t *buf, int buf_size, const char *filename)
{
    return libdisc_probe(buf, buf_size, filename);
}

static int cdimage_open(BlockDriverState *bs, QDict *options, int flags,
                        Error **errp)
{
    BDRVCdimageState *s = bs->opaque;
    char err[512];
    const char *path;
    int ret;

    GLOBAL_STATE_CODE();

    /* read-only medium: refuse a read-write open unless auto-read-only */
    bdrv_graph_rdlock_main_loop();
    ret = bdrv_apply_auto_read_only(bs, "cdimage discs are read-only", errp);
    bdrv_graph_rdunlock_main_loop();
    if (ret < 0) {
        return ret;
    }

    /* the .cue / .ccd itself becomes bs->file: tiny, read-only, harmless */
    ret = bdrv_open_file_child(NULL, options, "file", bs, errp);
    if (ret < 0) {
        return ret;
    }

    GRAPH_RDLOCK_GUARD_MAINLOOP();

    if (libdisc_api_version() != LIBDISC_API_VERSION) {
        error_setg(errp, "libdisc API version %u, cdimage was built for %u",
                   libdisc_api_version(), LIBDISC_API_VERSION);
        return -EINVAL;
    }

    path = bs->file->bs->exact_filename;
    if (!path[0]) {
        path = bs->file->bs->filename;
    }
    if (!path[0]) {
        path = bs->filename;
    }
    return cdimage_open_disc(bs, path, errp);
}

/*
 * isodir:/path/to/folder -> options["dir"]. The prefix is the whole
 * syntax: a shared folder has no options to parse, and anything after
 * the prefix is a path, colons and all.
 */
static void isodir_parse_filename(const char *filename, QDict *options,
                                  Error **errp)
{
    const char *dir;

    if (!strstart(filename, ISODIR_PREFIX, &dir) || !dir[0]) {
        error_setg(errp, "isodir: expected " ISODIR_PREFIX "<directory>");
        return;
    }
    qdict_put_str(options, "dir", dir);
}

static QemuOptsList isodir_runtime_opts = {
    .name = "isodir",
    .head = QTAILQ_HEAD_INITIALIZER(isodir_runtime_opts.head),
    .desc = {
        {
            .name = "dir",
            .type = QEMU_OPT_STRING,
            .help = "Host directory to serve as an ISO 9660 disc",
        },
        { /* end of list */ }
    },
};

static int isodir_open(BlockDriverState *bs, QDict *options, int flags,
                       Error **errp)
{
    QemuOpts *opts;
    const char *dir;
    int ret;

    GLOBAL_STATE_CODE();

    /* A folder is a disc: read-only, whatever the caller asked for. */
    bdrv_graph_rdlock_main_loop();
    ret = bdrv_apply_auto_read_only(bs, "isodir discs are read-only", errp);
    bdrv_graph_rdunlock_main_loop();
    if (ret < 0) {
        return ret;
    }

    opts = qemu_opts_create(&isodir_runtime_opts, NULL, 0, &error_abort);
    if (!qemu_opts_absorb_qdict(opts, options, errp)) {
        qemu_opts_del(opts);
        return -EINVAL;
    }
    dir = qemu_opt_get(opts, "dir");
    if (!dir) {
        error_setg(errp, "isodir needs a 'dir' option (or an "
                   ISODIR_PREFIX "<directory> filename)");
        qemu_opts_del(opts);
        return -EINVAL;
    }
    /*
     * No bdrv_open_file_child: there is no file to open, and the block
     * layer would not know what to do with a directory if there were.
     * That also means this node has no children, so no bdrv_child_perm.
     */
    ret = cdimage_open_disc(bs, dir, errp);
    qemu_opts_del(opts);
    return ret;
}

static void cdimage_refresh_limits(BlockDriverState *bs, Error **errp)
{
    bs->bl.request_alignment = CDIMAGE_SECTOR; /* whole sectors only */
    /* a host drive's disc comes and goes, and its size with it */
    bs->bl.has_variable_length = bs->drv == &bdrv_hostcd;
}

/*
 * hostcd:/dev/sr0 -> options["dev"]. As with isodir, the prefix is the
 * whole syntax.
 */
static void hostcd_parse_filename(const char *filename, QDict *options,
                                  Error **errp)
{
    const char *dev;

    if (!strstart(filename, HOSTCD_PREFIX, &dev) || !dev[0]) {
        error_setg(errp, "hostcd: expected " HOSTCD_PREFIX "<device>");
        return;
    }
    qdict_put_str(options, "dev", dev);
}

static QemuOptsList hostcd_runtime_opts = {
    .name = "hostcd",
    .head = QTAILQ_HEAD_INITIALIZER(hostcd_runtime_opts.head),
    .desc = {
        {
            .name = "dev",
            .type = QEMU_OPT_STRING,
            .help = "Host optical drive (/dev/sr0)",
        },
        { /* end of list */ }
    },
};

/*
 * Tell whoever holds the medium that it changed. A protocol node reached
 * through its prefix has a raw format node above it, whose child class has
 * no change_media, so walk up to the BlockBackend (the drive).
 */
static void GRAPH_RDLOCK hostcd_notify(BlockDriverState *bs, bool load)
{
    BdrvChild *c;

    QLIST_FOREACH(c, &bs->parents, next_parent) {
        if (c->klass->change_media) {
            c->klass->change_media(c, load);
        } else if (c->klass->parent_is_bds) {
            hostcd_notify(c->opaque, load);
        }
    }
}

static void hostcd_set_disc(BlockDriverState *bs, libdisc *d)
{
    BDRVCdimageState *s = bs->opaque;

    s->disc = d;
    s->sectors = d ? libdisc_sector_count(d) : 0;
    bs->total_sectors = (int64_t)s->sectors * CDIMAGE_SECTOR / BDRV_SECTOR_SIZE;
}

/* Main loop, BQL: apply what the watch saw. */
static void hostcd_change_bh(void *opaque)
{
    BlockDriverState *bs = opaque;
    BDRVCdimageState *s = bs->opaque;
    HostcdWatch *w = s->watch;
    libdisc *d;

    qemu_mutex_lock(&w->lock);
    if (!w->pending_set) {
        qemu_mutex_unlock(&w->lock);
        return;
    }
    d = w->pending;
    w->pending = NULL;
    w->pending_set = false;
    qemu_mutex_unlock(&w->lock);

    GRAPH_RDLOCK_GUARD_MAINLOOP();
    /*
     * Reads are synchronous on this (the main) thread and atapi.c asks
     * cdimage_disc() on every command, so nothing holds the old handle.
     * A swap is out then in: the guest sees the tray open, then a new
     * medium (NOT READY, then UNIT ATTENTION), as on a real drive.
     */
    if (s->disc) {
        libdisc *old = s->disc;

        hostcd_set_disc(bs, NULL);
        hostcd_notify(bs, false);
        libdisc_close(old);
        info_report("hostcd: %s: the disc was taken out", s->path);
    }
    if (d) {
        hostcd_set_disc(bs, d);
        hostcd_notify(bs, true);
        info_report("hostcd: %s: a disc was put in (%u sectors)", s->path,
                    s->sectors);
    }
}

static void *hostcd_watch_thread(void *opaque)
{
    HostcdWatch *w = opaque;

    /* qemu_sem_timedwait is -1 on a timeout: poll until stop is posted */
    while (qemu_sem_timedwait(&w->stop, HOSTCD_POLL_MS) < 0) {
        bool ready = libdisc_drive_ready(w->dev) == 1;
        libdisc *d = NULL;

        if (!ready) {
            w->failed = false;
        }
        if (ready == w->present) {
            continue;
        }
        if (ready) {
            char err[512];

            if (w->failed) {
                continue;
            }
            err[0] = 0;
            d = libdisc_open(w->dev, err, sizeof(err));
            if (!d) {
                /* a blank or unreadable disc: say so once, until it comes out */
                warn_report("hostcd: %s", err[0] ? err : "cannot open the disc");
                w->failed = true;
                continue;
            }
        }
        w->present = ready;
        qemu_mutex_lock(&w->lock);
        if (w->pending) {
            libdisc_close(w->pending);
        }
        w->pending = d;
        w->pending_set = true;
        qemu_mutex_unlock(&w->lock);
        qemu_bh_schedule(w->bh);
    }
    return NULL;
}

static int hostcd_open(BlockDriverState *bs, QDict *options, int flags,
                       Error **errp)
{
    BDRVCdimageState *s = bs->opaque;
    HostcdWatch *w;
    QemuOpts *opts;
    const char *dev;
    char err[512];
    int ret;

    GLOBAL_STATE_CODE();

    bdrv_graph_rdlock_main_loop();
    ret = bdrv_apply_auto_read_only(bs, "hostcd discs are read-only", errp);
    bdrv_graph_rdunlock_main_loop();
    if (ret < 0) {
        return ret;
    }
    if (libdisc_api_version() != LIBDISC_API_VERSION) {
        error_setg(errp, "libdisc API version %u, cdimage was built for %u",
                   libdisc_api_version(), LIBDISC_API_VERSION);
        return -EINVAL;
    }

    opts = qemu_opts_create(&hostcd_runtime_opts, NULL, 0, &error_abort);
    if (!qemu_opts_absorb_qdict(opts, options, errp)) {
        qemu_opts_del(opts);
        return -EINVAL;
    }
    dev = qemu_opt_get(opts, "dev");
    if (!dev) {
        error_setg(errp, "hostcd needs a 'dev' option (or an "
                   HOSTCD_PREFIX "<device> filename)");
        qemu_opts_del(opts);
        return -EINVAL;
    }

    w = g_new0(HostcdWatch, 1);
    w->dev = g_strconcat(HOSTCD_PREFIX, dev, NULL);
    s->path = g_strdup(dev);
    qemu_opts_del(opts);

    /* A drive that cannot be asked at all is an error; an empty one is not. */
    ret = libdisc_drive_ready(w->dev);
    if (ret < 0) {
        error_setg(errp, "hostcd: cannot use the drive %s (no such drive, or "
                   "no permission to open it)", s->path);
        g_free(w->dev);
        g_free(w);
        g_free(s->path);
        s->path = NULL;
        return -EINVAL;
    }
    if (ret == 1) {
        err[0] = 0;
        hostcd_set_disc(bs, libdisc_open(w->dev, err, sizeof(err)));
        if (!s->disc) {
            warn_report("hostcd: %s", err[0] ? err : "cannot open the disc");
            w->failed = true;
        }
    }
    w->present = s->disc != NULL;
    qemu_sem_init(&w->stop, 0);
    qemu_mutex_init(&w->lock);
    w->bh = qemu_bh_new(hostcd_change_bh, bs);
    s->watch = w;
    qemu_thread_create(&w->thread, "hostcd", hostcd_watch_thread, w,
                       QEMU_THREAD_JOINABLE);
    return 0;
}

static bool coroutine_fn GRAPH_RDLOCK
hostcd_co_is_inserted(BlockDriverState *bs)
{
    return ((BDRVCdimageState *)bs->opaque)->disc != NULL;
}

static int64_t coroutine_fn GRAPH_RDLOCK
hostcd_co_getlength(BlockDriverState *bs)
{
    return (int64_t)((BDRVCdimageState *)bs->opaque)->sectors * CDIMAGE_SECTOR;
}

/* The guest's eject (START STOP UNIT, LoEj) opens the real tray. */
static void coroutine_fn GRAPH_RDLOCK
hostcd_co_eject(BlockDriverState *bs, bool eject_flag)
{
    BDRVCdimageState *s = bs->opaque;

    if (eject_flag && libdisc_drive_eject(s->watch->dev) != LIBDISC_OK) {
        warn_report("hostcd: %s: the tray did not open", s->path);
    }
}

static int coroutine_fn GRAPH_RDLOCK
cdimage_co_preadv(BlockDriverState *bs, int64_t offset, int64_t bytes,
                  QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    BDRVCdimageState *s = bs->opaque;
    uint8_t buf[CDIMAGE_SECTOR];
    int64_t done = 0;

    assert(QEMU_IS_ALIGNED(offset, CDIMAGE_SECTOR));
    assert(QEMU_IS_ALIGNED(bytes, CDIMAGE_SECTOR));

    while (done < bytes) {
        uint64_t lba = (offset + done) / CDIMAGE_SECTOR;
        int rc;

        if (!s->disc) {
            return -ENOMEDIUM;  /* hostcd with the drive empty */
        }
        if (lba >= s->sectors) {
            return -EIO;
        }
        /*
         * A data sector whose EDC/ECC does not verify, or an audio / gap
         * sector, is an I/O error here exactly as a drive answers a
         * READ(10) of it; the ATAPI path reports the precise sense itself.
         */
        rc = libdisc_read_cooked(s->disc, (uint32_t)lba, buf);
        if (rc != LIBDISC_OK) {
            return -EIO;
        }
        qemu_iovec_from_buf(qiov, done, buf, CDIMAGE_SECTOR);
        done += CDIMAGE_SECTOR;
    }
    return 0;
}

static void cdimage_close(BlockDriverState *bs)
{
    BDRVCdimageState *s = bs->opaque;
    HostcdWatch *w = s->watch;

    if (w) {
        qemu_sem_post(&w->stop);
        qemu_thread_join(&w->thread);
        qemu_bh_delete(w->bh);
        if (w->pending) {
            libdisc_close(w->pending);
        }
        qemu_mutex_destroy(&w->lock);
        qemu_sem_destroy(&w->stop);
        g_free(w->dev);
        g_free(w);
        s->watch = NULL;
    }

    libdisc_close(s->disc);
    s->disc = NULL;
    g_free(s->path);
    s->path = NULL;
}

/* The cdimage or isodir node under `bs`, or NULL when the medium is not ours. */
static BlockDriverState *cdimage_node(BlockDriverState *bs)
{
    if (!bs) {
        return NULL;
    }
    bs = bdrv_skip_filters(bs);
    /*
     * A protocol driver reached through its filename prefix can end up
     * with a format node above it, so walk down through those too: a
     * disc handle that is not found here is not an error anywhere, it
     * silently drops the drive back to QEMU's stock ATAPI answers.
     */
    while (bs && bs->drv && bs->drv->is_format && bs->drv != &bdrv_cdimage && bs->file) {
        bs = bdrv_skip_filters(bs->file->bs);
    }
    if (!bs || (bs->drv != &bdrv_cdimage && bs->drv != &bdrv_isodir &&
                bs->drv != &bdrv_hostcd)) {
        return NULL;
    }
    return bs;
}

libdisc *cdimage_disc(BlockDriverState *bs)
{
    GRAPH_RDLOCK_GUARD_MAINLOOP();

    bs = cdimage_node(bs);
    return bs ? ((BDRVCdimageState *)bs->opaque)->disc : NULL;
}

const char *cdimage_medium_path(BlockDriverState *bs)
{
    BlockDriverState *ours;

    GRAPH_RDLOCK_GUARD_MAINLOOP();

    if (!bs) {
        return NULL;
    }
    ours = cdimage_node(bs);
    if (ours) {
        return ((BDRVCdimageState *)ours->opaque)->path;
    }
    /*
     * Not ours (a plain ISO the raw driver took): the file at the bottom
     * of the chain is the medium. A protocol node has no `file`.
     */
    bs = bdrv_skip_filters(bs);
    while (bs && bs->file) {
        bs = bdrv_skip_filters(bs->file->bs);
    }
    if (!bs) {
        return NULL;
    }
    return bs->exact_filename[0] ? bs->exact_filename : bs->filename;
}

static BlockDriver bdrv_cdimage = {
    .format_name         = "cdimage",
    .instance_size       = sizeof(BDRVCdimageState),
    .bdrv_probe          = cdimage_probe,
    .bdrv_open           = cdimage_open,
    .bdrv_child_perm     = bdrv_default_perms,
    .bdrv_refresh_limits = cdimage_refresh_limits,
    .bdrv_co_preadv      = cdimage_co_preadv,
    .bdrv_close          = cdimage_close,
    .is_format           = true,
};

static const char *const isodir_strong_runtime_opts[] = {
    "dir",
    NULL
};

static BlockDriver bdrv_isodir = {
    .format_name         = "isodir",
    .protocol_name       = "isodir",
    .instance_size       = sizeof(BDRVCdimageState),
    .bdrv_parse_filename = isodir_parse_filename,
    .bdrv_open           = isodir_open,
    .bdrv_refresh_limits = cdimage_refresh_limits,
    .bdrv_co_preadv      = cdimage_co_preadv,
    .bdrv_close          = cdimage_close,
    .strong_runtime_opts = isodir_strong_runtime_opts,
};

static const char *const hostcd_strong_runtime_opts[] = {
    "dev",
    NULL
};

static BlockDriver bdrv_hostcd = {
    .format_name         = "hostcd",
    .protocol_name       = "hostcd",
    .instance_size       = sizeof(BDRVCdimageState),
    .bdrv_parse_filename = hostcd_parse_filename,
    .bdrv_open           = hostcd_open,
    .bdrv_refresh_limits = cdimage_refresh_limits,
    .bdrv_co_preadv      = cdimage_co_preadv,
    .bdrv_co_getlength   = hostcd_co_getlength,
    .bdrv_co_is_inserted = hostcd_co_is_inserted,
    .bdrv_co_eject       = hostcd_co_eject,
    .bdrv_close          = cdimage_close,
    .strong_runtime_opts = hostcd_strong_runtime_opts,
};

static void bdrv_cdimage_init(void)
{
    bdrv_register(&bdrv_cdimage);
    bdrv_register(&bdrv_isodir);
    bdrv_register(&bdrv_hostcd);
}

block_init(bdrv_cdimage_init);
