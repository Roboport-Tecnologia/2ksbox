/*
 * libdisc: the C API of the raw optical disc model (doc 17 §3).
 *
 * The one header for the Rust crate (libdisc/src/capi.rs implements it),
 * QEMU's cdimage block driver (libdisc/qemu/cdimage.c) and hw/ide/atapi.c
 * (patch 51). Bump LIBDISC_API_VERSION on any change; cdimage_open refuses
 * a mismatch. Every function is thread-safe on the same handle: a disc's
 * layout is immutable once opened, and the one piece of mutable state
 * (the cache of open payload handles, which a folder disc needs because it
 * names one file per shared file) is behind a lock.
 *
 * libdisc_open also takes a *directory*: it is then served as a generated
 * ISO 9660 + Joliet volume over that tree (isodir, docs/17-cdrom-implementation.md §8),
 * and `hostcd:<device>`: the disc in a host's optical drive (§9).
 */
#ifndef LIBDISC_H
#define LIBDISC_H
#include <stddef.h>
#include <stdint.h>

#define LIBDISC_API_VERSION 3

typedef struct libdisc libdisc;                /* opaque */

enum {
    LIBDISC_OK      =  0,
    LIBDISC_ERANGE  = -1,   /* LBA outside [0, lead-out)               → sense 05/21/00 */
    LIBDISC_EMEDIUM = -2,   /* L-EC failed on a cooked read            → sense 03/11/05 */
    LIBDISC_EMODE   = -3,   /* wrong sector kind for the request       → sense 05/64/00 */
    LIBDISC_EINVAL  = -4,   /* bad parameter / CDB field               → sense 05/24/00 */
    LIBDISC_EIO     = -5,   /* host file read failed                   → sense 04/xx: report and fail */
    LIBDISC_EAGAIN  = -6,   /* read_raw_nowait: a host drive is still fetching it; call again */
};

/* sector kinds, as LibdiscSectorInfo.kind */
enum { LIBDISC_KIND_AUDIO = 0, LIBDISC_KIND_MODE1 = 1, LIBDISC_KIND_MODE2F1 = 2,
       LIBDISC_KIND_MODE2F2 = 3, LIBDISC_KIND_MODE2 = 4, LIBDISC_KIND_GAP = 5 };

typedef struct LibdiscSectorInfo {
    uint8_t kind;        /* LIBDISC_KIND_* */
    uint8_t track;       /* 1..99 */
    uint8_t index;       /* 0..99 */
    uint8_t lec;         /* data kinds: 1 ok, 0 fails; audio/gap: 1 */
} LibdiscSectorInfo;

typedef struct LibdiscTrackInfo {
    uint8_t number, session, control, mode;   /* mode = LIBDISC_KIND_* of the track */
    int32_t start_lba;   /* index 1 */
    int32_t pregap_lba;  /* index 0 start, == start_lba when there is none */
    int32_t end_lba;     /* exclusive */
} LibdiscTrackInfo;

uint32_t libdisc_api_version(void);
/* 0..100: how sure libdisc is that `filename` (first `len` bytes in `head`) is an image it reads.
   .cue → 100 when the text parses as a cue sheet; .ccd → 100 on "[CloneCD]"; .iso → 0 (stays on raw) */
int      libdisc_probe(const uint8_t *head, size_t len, const char *filename);
/* opens the image and every payload file; on failure returns NULL and writes a message into err */
libdisc *libdisc_open(const char *path, char *err, size_t errlen);
void     libdisc_close(libdisc *d);

/*
 * A host's optical drive (`hostcd:`, §9), by device path with or without
 * the prefix. ready: 1 a readable disc, 0 none (or spinning up),
 * LIBDISC_EIO the drive cannot be opened or asked. eject opens the tray.
 * Neither needs an open disc; the hostcd driver's watch thread polls ready.
 */
int      libdisc_drive_ready(const char *path);
int      libdisc_drive_eject(const char *path);

uint32_t libdisc_sector_count(const libdisc *d);          /* lead-out LBA of the last session */
uint8_t  libdisc_session_count(const libdisc *d);
uint8_t  libdisc_track_count(const libdisc *d);           /* across sessions */
int      libdisc_track_info(const libdisc *d, uint8_t track, LibdiscTrackInfo *out); /* ERANGE if no such track */
int      libdisc_sector_info(const libdisc *d, uint32_t lba, LibdiscSectorInfo *out);

int      libdisc_read_cooked(const libdisc *d, uint32_t lba, uint8_t out[2048]);  /* L-EC verified user data */
int      libdisc_read_raw(const libdisc *d, uint32_t lba, uint8_t out[2352]);
/* read_raw that never waits for a host drive (hostcd:, §9): LIBDISC_EAGAIN
 * while the drive fetches the sector; CD audio calls it from QEMU's main loop */
int      libdisc_read_raw_nowait(const libdisc *d, uint32_t lba, uint8_t out[2352]);
int      libdisc_read_sub(const libdisc *d, uint32_t lba, uint8_t out[96]);      /* deinterleaved P..W */

/* MMC responders: write the reply into out (capacity cap), return its length or a LIBDISC_E* code.
   The caller truncates to the CDB's allocation length. */
int libdisc_mmc_read_toc(const libdisc *d, uint8_t format, int msf, uint8_t start, uint8_t *out, size_t cap);
int libdisc_mmc_read_subchannel(const libdisc *d, uint32_t pos_lba, int msf, int subq, uint8_t format,
                                uint8_t track, uint8_t audio_status, uint8_t *out, size_t cap);
int libdisc_mmc_read_disc_information(const libdisc *d, uint8_t *out, size_t cap);
/* READ CD (BE) / READ CD MSF (B9): bytes per sector for this CDB, or LIBDISC_EINVAL for an illegal
   combination; then one call per sector fills exactly that many bytes */
int libdisc_mmc_read_cd_length(uint8_t expected_type, uint8_t byte9, uint8_t byte10);
int libdisc_mmc_read_cd_sector(const libdisc *d, uint32_t lba, uint8_t expected_type, uint8_t byte9,
                               uint8_t byte10, uint8_t *out, size_t cap);
#endif
