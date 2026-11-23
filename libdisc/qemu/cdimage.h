/*
 * cdimage: the libdisc handle behind a BlockDriverState (doc 17 §5.1).
 *
 * The only thing hw/ide/atapi.c includes besides libdisc.h. Overlaid into
 * qemu/include/block/ by scripts/prepare-qemu.sh from libdisc/qemu/.
 */
#ifndef BLOCK_CDIMAGE_H
#define BLOCK_CDIMAGE_H

#include "block/libdisc.h"

typedef struct BlockDriverState BlockDriverState;

/*
 * The disc model of the medium in `bs` (filters skipped), or NULL when the
 * medium is not a cdimage (a plain ISO on the raw driver, no medium). Call
 * it on every command from under the BQL and never cache the result: a
 * medium change replaces the BlockDriverState underneath.
 */
libdisc *cdimage_disc(BlockDriverState *bs);

/*
 * The host path the medium in `bs` was opened from: a cdimage's image
 * file, an isodir's directory (no `isodir:` prefix), a hostcd's device
 * (no `hostcd:` prefix), or the file under a
 * medium that is not ours. NULL with no medium. Same rules as above: from
 * under the BQL, never cached. The disc shelf (patch 52) compares it with
 * its entries to say which disc is in the drive.
 */
const char *cdimage_medium_path(BlockDriverState *bs);

#endif
