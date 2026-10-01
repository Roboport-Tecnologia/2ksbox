/*
 * tpm-libtpms: a TPM 2.0 inside QEMU's own process (track M20).
 *
 *   -tpmdev libtpms,id=tpm0,state=<file> -device tpm-crb,tpmdev=tpm0
 *
 * QEMU's own TPM backends are a host TPM (passthrough) and swtpm in a
 * process of its own over a socket (emulator). This one links libtpms,
 * the TPM swtpm runs, and calls it directly: no second process to start,
 * supervise or fit into a sandbox (ADR-002). Windows 11 wants a TPM 2.0
 * to install, and BitLocker and Hello want one to keep working.
 *
 * Overlaid into backends/tpm/ by scripts/prepare-qemu.sh, built by patch
 * 75 when configure finds libtpms (`--enable-libtpms`).
 *
 * State. The TPM's permanent state (its seeds, NV indices, persistent
 * keys, the orderly-shutdown data) is one blob, libtpms's "permall",
 * kept in `state=` and replaced atomically each time libtpms stores it.
 * A missing file is a TPM that was never made: libtpms manufactures one
 * on its first start and stores it. The other names libtpms may store
 * live in memory for the process's life. A snapshot carries the
 * permanent and volatile state (vmstate below), and loading one writes
 * its permanent state back to the file, so the TPM always matches the
 * disk it booted with.
 *
 * libtpms is one TPM per process (its state is global), and so is QEMU
 * ("Only one TPM is allowed", system/tpm.c); create() checks it anyway,
 * since the callbacks below reach the TPM through one static pointer.
 *
 * Not here: an endorsement key certificate. Nothing provisions one, as
 * with swtpm without swtpm_setup; Windows makes its own EK and reports
 * the TPM ready (M20 step 1), but attestation to Microsoft will not work.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/lockable.h"
#include "sysemu/runstate.h"
#include "sysemu/tpm_backend.h"
#include "sysemu/tpm_util.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/clone-visitor.h"
#include "qapi/qapi-visit-tpm.h"
#include "qom/object.h"

#include <libtpms/tpm_library.h>
#include <libtpms/tpm_error.h>
#include <libtpms/tpm_tis.h>
#include <libtpms/tpm_nvfilename.h>

/*
 * Not tpm_int.h: its TPM 1.2 error codes are libtpms's tpm_error.h's
 * again, defined a second time.
 */
#define TPM_RESP_HDR_SIZE 10   /* tag, size, return code */

#define TYPE_TPM_LIBTPMS "tpm-libtpms"
OBJECT_DECLARE_SIMPLE_TYPE(TPMLibtpms, TPM_LIBTPMS)

struct TPMLibtpms {
    TPMBackend parent;

    TPMLibtpmsOptions *options;
    QemuMutex mutex;          /* every libtpms call but the cancel */
    bool running;             /* between TPMLIB_MainInit and _Terminate */
    size_t buffersize;        /* asked for while an incoming state waited */

    /* TPMLIB_Process's response buffer, libtpms's own (malloc/realloc) */
    unsigned char *rbuf;
    uint32_t rbuf_size;

    /* names other than "permall", for this process's life */
    GHashTable *other_blobs;

    /* the snapshot's blobs, filled by pre_save, read by post_load */
    uint32_t permanent_len;
    uint8_t *permanent;
    uint32_t volatil_len;
    uint8_t *volatil;
};

/* libtpms's callbacks carry no context: this is the one TPM. */
static TPMLibtpms *the_tpm;
static uint8_t cur_locty;

/* -- libtpms's callbacks ------------------------------------------------ */

static TPM_RESULT tpm_libtpms_nvram_init(void)
{
    return TPM_SUCCESS;
}

static TPM_RESULT tpm_libtpms_nvram_loaddata(unsigned char **data,
                                             uint32_t *length,
                                             uint32_t tpm_number,
                                             const char *name)
{
    TPMLibtpms *t = the_tpm;
    g_autofree gchar *contents = NULL;
    gsize len = 0;
    GBytes *b;

    *data = NULL;
    *length = 0;

    if (strcmp(name, TPM_PERMANENT_ALL_NAME) == 0) {
        g_autoptr(GError) gerr = NULL;

        if (!g_file_get_contents(t->options->state, &contents, &len, &gerr)) {
            if (g_error_matches(gerr, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
                return TPM_RETRY;   /* never made: libtpms manufactures one */
            }
            error_report("tpm-libtpms: %s", gerr->message);
            return TPM_FAIL;
        }
    } else {
        b = g_hash_table_lookup(t->other_blobs, name);
        if (!b) {
            return TPM_RETRY;
        }
        contents = g_memdup2(g_bytes_get_data(b, &len), g_bytes_get_size(b));
    }

    /* libtpms frees what it loads with free() */
    *data = malloc(len ? len : 1);
    if (!*data) {
        return TPM_SIZE;
    }
    memcpy(*data, contents, len);
    *length = len;
    return TPM_SUCCESS;
}

static TPM_RESULT tpm_libtpms_nvram_storedata(const unsigned char *data,
                                              uint32_t length,
                                              uint32_t tpm_number,
                                              const char *name)
{
    TPMLibtpms *t = the_tpm;

    if (strcmp(name, TPM_PERMANENT_ALL_NAME) == 0) {
        g_autoptr(GError) gerr = NULL;

        /* a temporary file renamed over the old one: never half written */
        if (!g_file_set_contents(t->options->state, (const gchar *)data,
                                 length, &gerr)) {
            error_report("tpm-libtpms: %s", gerr->message);
            return TPM_FAIL;
        }
        return TPM_SUCCESS;
    }

    g_hash_table_insert(t->other_blobs, g_strdup(name),
                        g_bytes_new(data, length));
    return TPM_SUCCESS;
}

static TPM_RESULT tpm_libtpms_nvram_deletename(uint32_t tpm_number,
                                               const char *name,
                                               TPM_BOOL mustExist)
{
    TPMLibtpms *t = the_tpm;
    bool existed;

    if (strcmp(name, TPM_PERMANENT_ALL_NAME) == 0) {
        existed = unlink(t->options->state) == 0;
    } else {
        existed = g_hash_table_remove(t->other_blobs, name);
    }
    return (mustExist && !existed) ? TPM_FAIL : TPM_SUCCESS;
}

static TPM_RESULT tpm_libtpms_io_init(void)
{
    return TPM_SUCCESS;
}

static TPM_RESULT tpm_libtpms_io_getlocality(TPM_MODIFIER_INDICATOR *locty,
                                             uint32_t tpm_number)
{
    *locty = cur_locty;
    return TPM_SUCCESS;
}

static TPM_RESULT tpm_libtpms_io_getphysicalpresence(TPM_BOOL *pp,
                                                     uint32_t tpm_number)
{
    *pp = FALSE;
    return TPM_SUCCESS;
}

static struct libtpms_callbacks tpm_libtpms_callbacks = {
    .sizeOfStruct = sizeof(struct libtpms_callbacks),
    .tpm_nvram_init = tpm_libtpms_nvram_init,
    .tpm_nvram_loaddata = tpm_libtpms_nvram_loaddata,
    .tpm_nvram_storedata = tpm_libtpms_nvram_storedata,
    .tpm_nvram_deletename = tpm_libtpms_nvram_deletename,
    .tpm_io_init = tpm_libtpms_io_init,
    .tpm_io_getlocality = tpm_libtpms_io_getlocality,
    .tpm_io_getphysicalpresence = tpm_libtpms_io_getphysicalpresence,
};

/* -- the TPM's power ---------------------------------------------------- */

/* Called with the mutex held. */
static void tpm_libtpms_stop(TPMLibtpms *t)
{
    if (t->running) {
        TPMLIB_Terminate();
        t->running = false;
    }
}

/* Called with the mutex held. A restart is a power cycle, as swtpm's INIT. */
static int tpm_libtpms_start(TPMLibtpms *t, size_t buffersize)
{
    TPM_RESULT res;

    tpm_libtpms_stop(t);
    if (buffersize) {
        TPMLIB_SetBufferSize(buffersize, NULL, NULL);
    }
    res = TPMLIB_MainInit();
    if (res != TPM_SUCCESS) {
        error_report("tpm-libtpms: TPMLIB_MainInit failed: 0x%x", res);
        return -1;
    }
    t->running = true;
    return 0;
}

/* -- TPMBackendClass ---------------------------------------------------- */

static int tpm_libtpms_startup_tpm(TPMBackend *tb, size_t buffersize)
{
    TPMLibtpms *t = TPM_LIBTPMS(tb);

    QEMU_LOCK_GUARD(&t->mutex);
    /* an incoming state starts the TPM in post_load */
    if (runstate_check(RUN_STATE_INMIGRATE)) {
        t->buffersize = buffersize;
        return 0;
    }
    return tpm_libtpms_start(t, buffersize);
}

static void tpm_libtpms_handle_request(TPMBackend *tb, TPMBackendCmd *cmd,
                                       Error **errp)
{
    TPMLibtpms *t = TPM_LIBTPMS(tb);
    g_autofree unsigned char *in = g_memdup2(cmd->in, cmd->in_len);
    uint32_t resp_size = 0;
    TPM_RESULT res;

    QEMU_LOCK_GUARD(&t->mutex);
    cmd->selftest_done = false;
    if (!t->running) {
        error_setg(errp, "tpm-libtpms: a command to a TPM that is not running");
        tpm_util_write_fatal_error_response(cmd->out, cmd->out_len);
        return;
    }
    cur_locty = cmd->locty;
    res = TPMLIB_Process(&t->rbuf, &resp_size, &t->rbuf_size, in, cmd->in_len);
    if (res != TPM_SUCCESS || resp_size < TPM_RESP_HDR_SIZE ||
        resp_size > cmd->out_len) {
        error_setg(errp, "tpm-libtpms: TPMLIB_Process: 0x%x, %u bytes for %u",
                   res, resp_size, cmd->out_len);
        tpm_util_write_fatal_error_response(cmd->out, cmd->out_len);
        return;
    }
    memcpy(cmd->out, t->rbuf, resp_size);
    cmd->selftest_done = tpm_util_is_selftest(cmd->in, cmd->in_len) &&
                         tpm_cmd_get_errcode(cmd->out) == 0;
}

static void tpm_libtpms_cancel_cmd(TPMBackend *tb)
{
    /* no lock: the command it cancels holds it */
    TPMLIB_CancelCommand();
}

static bool tpm_libtpms_get_tpm_established_flag(TPMBackend *tb)
{
    TPMLibtpms *t = TPM_LIBTPMS(tb);
    TPM_BOOL established = FALSE;

    QEMU_LOCK_GUARD(&t->mutex);
    if (t->running) {
        TPM_IO_TpmEstablished_Get(&established);
    }
    return established;
}

static int tpm_libtpms_reset_tpm_established_flag(TPMBackend *tb,
                                                  uint8_t locty)
{
    TPMLibtpms *t = TPM_LIBTPMS(tb);

    QEMU_LOCK_GUARD(&t->mutex);
    if (!t->running) {
        return -1;
    }
    /* only localities 3 and 4 may; libtpms checks through the callback */
    cur_locty = locty;
    return TPM_IO_TpmEstablished_Reset() == TPM_SUCCESS ? 0 : -1;
}

static TPMVersion tpm_libtpms_get_tpm_version(TPMBackend *tb)
{
    return TPM_VERSION_2_0;
}

static size_t tpm_libtpms_get_buffer_size(TPMBackend *tb)
{
    TPMLibtpms *t = TPM_LIBTPMS(tb);

    QEMU_LOCK_GUARD(&t->mutex);
    return TPMLIB_SetBufferSize(0, NULL, NULL);   /* 0: ask */
}

static TpmTypeOptions *tpm_libtpms_get_tpm_options(TPMBackend *tb)
{
    TPMLibtpms *t = TPM_LIBTPMS(tb);
    TpmTypeOptions *options = g_new0(TpmTypeOptions, 1);

    options->type = TPM_TYPE_LIBTPMS;
    options->u.libtpms.data = QAPI_CLONE(TPMLibtpmsOptions, t->options);
    return options;
}

static TPMBackend *tpm_libtpms_create(QemuOpts *opts)
{
    const char *state = qemu_opt_get(opts, "state");
    g_autofree char *dir = NULL;
    TPMLibtpms *t;
    TPM_RESULT res;

    if (the_tpm) {
        error_report("tpm-libtpms: libtpms is one TPM per process, and "
                     "'%s' has it", the_tpm->parent.id);
        return NULL;
    }
    if (!state || !*state) {
        error_report("tpm-libtpms: missing state=<file>");
        return NULL;
    }
    dir = g_path_get_dirname(state);
    if (!g_file_test(dir, G_FILE_TEST_IS_DIR)) {
        error_report("tpm-libtpms: no directory %s for state=%s", dir, state);
        return NULL;
    }

    res = TPMLIB_ChooseTPMVersion(TPMLIB_TPM_VERSION_2);
    if (res == TPM_SUCCESS) {
        res = TPMLIB_RegisterCallbacks(&tpm_libtpms_callbacks);
    }
    if (res != TPM_SUCCESS) {
        error_report("tpm-libtpms: libtpms refused its setup: 0x%x", res);
        return NULL;
    }

    t = TPM_LIBTPMS(object_new(TYPE_TPM_LIBTPMS));
    t->options->state = g_strdup(state);
    the_tpm = t;
    return TPM_BACKEND(t);
}

static const QemuOptDesc tpm_libtpms_cmdline_opts[] = {
    {
        .name = "type",
        .type = QEMU_OPT_STRING,
        .help = "Type of TPM backend",
    },
    {
        .name = "state",
        .type = QEMU_OPT_STRING,
        .help = "File holding the TPM's permanent state",
    },
    { /* end of list */ },
};

/* -- snapshots and migration -------------------------------------------- */

/* libtpms's buffer (malloc) into one vmstate owns (g_malloc). */
static void tpm_libtpms_take(uint8_t **dst, uint32_t *dst_len,
                             unsigned char *src, uint32_t len)
{
    g_free(*dst);
    *dst = g_memdup2(src, len);
    *dst_len = len;
    free(src);
}

static int tpm_libtpms_pre_save(void *opaque)
{
    TPMLibtpms *t = TPM_LIBTPMS(opaque);
    unsigned char *buf = NULL;
    uint32_t len = 0;
    TPM_RESULT res;

    tpm_backend_finish_sync(TPM_BACKEND(t));

    QEMU_LOCK_GUARD(&t->mutex);
    res = TPMLIB_GetState(TPMLIB_STATE_PERMANENT, &buf, &len);
    if (res != TPM_SUCCESS) {
        error_report("tpm-libtpms: no permanent state to save: 0x%x", res);
        return -EIO;
    }
    tpm_libtpms_take(&t->permanent, &t->permanent_len, buf, len);

    buf = NULL;
    len = 0;
    if (t->running) {
        res = TPMLIB_GetState(TPMLIB_STATE_VOLATILE, &buf, &len);
        if (res != TPM_SUCCESS) {
            error_report("tpm-libtpms: no volatile state to save: 0x%x", res);
            return -EIO;
        }
    }
    tpm_libtpms_take(&t->volatil, &t->volatil_len, buf, len);
    return 0;
}

static int tpm_libtpms_post_load(void *opaque, int version_id)
{
    TPMLibtpms *t = TPM_LIBTPMS(opaque);
    g_autoptr(GError) gerr = NULL;
    TPM_RESULT res;

    QEMU_LOCK_GUARD(&t->mutex);
    tpm_libtpms_stop(t);

    /* the permanent state first: the volatile one is checked against it */
    res = TPMLIB_SetState(TPMLIB_STATE_PERMANENT, t->permanent,
                          t->permanent_len);
    if (res == TPM_SUCCESS && t->volatil_len) {
        res = TPMLIB_SetState(TPMLIB_STATE_VOLATILE, t->volatil,
                              t->volatil_len);
    }
    if (res != TPM_SUCCESS) {
        error_report("tpm-libtpms: the snapshot's TPM state was refused: 0x%x",
                     res);
        return -EINVAL;
    }

    /* the file follows the snapshot, as the disk does */
    if (!g_file_set_contents(t->options->state, (const gchar *)t->permanent,
                             t->permanent_len, &gerr)) {
        error_report("tpm-libtpms: %s", gerr->message);
        return -EIO;
    }

    return tpm_libtpms_start(t, t->buffersize) < 0 ? -EIO : 0;
}

static const VMStateDescription vmstate_tpm_libtpms = {
    .name = "tpm-libtpms",
    .version_id = 0,
    .pre_save = tpm_libtpms_pre_save,
    .post_load = tpm_libtpms_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(permanent_len, TPMLibtpms),
        VMSTATE_VBUFFER_ALLOC_UINT32(permanent, TPMLibtpms, 0, 0,
                                     permanent_len),
        VMSTATE_UINT32(volatil_len, TPMLibtpms),
        VMSTATE_VBUFFER_ALLOC_UINT32(volatil, TPMLibtpms, 0, 0,
                                     volatil_len),
        VMSTATE_END_OF_LIST()
    }
};

/* -- the object --------------------------------------------------------- */

static void tpm_libtpms_inst_init(Object *obj)
{
    TPMLibtpms *t = TPM_LIBTPMS(obj);

    t->options = g_new0(TPMLibtpmsOptions, 1);
    t->other_blobs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           (GDestroyNotify)g_bytes_unref);
    qemu_mutex_init(&t->mutex);
    vmstate_register_any(NULL, &vmstate_tpm_libtpms, obj);
}

static void tpm_libtpms_inst_finalize(Object *obj)
{
    TPMLibtpms *t = TPM_LIBTPMS(obj);

    vmstate_unregister(NULL, &vmstate_tpm_libtpms, obj);
    qemu_mutex_lock(&t->mutex);
    tpm_libtpms_stop(t);
    qemu_mutex_unlock(&t->mutex);
    free(t->rbuf);
    g_free(t->permanent);
    g_free(t->volatil);
    g_hash_table_destroy(t->other_blobs);
    qapi_free_TPMLibtpmsOptions(t->options);
    qemu_mutex_destroy(&t->mutex);
    if (the_tpm == t) {
        the_tpm = NULL;
    }
}

static void tpm_libtpms_class_init(ObjectClass *klass, void *data)
{
    TPMBackendClass *tbc = TPM_BACKEND_CLASS(klass);

    tbc->type = TPM_TYPE_LIBTPMS;
    tbc->opts = tpm_libtpms_cmdline_opts;
    tbc->desc = "TPM 2.0 in this process (libtpms)";
    tbc->create = tpm_libtpms_create;
    tbc->startup_tpm = tpm_libtpms_startup_tpm;
    tbc->cancel_cmd = tpm_libtpms_cancel_cmd;
    tbc->get_tpm_established_flag = tpm_libtpms_get_tpm_established_flag;
    tbc->reset_tpm_established_flag = tpm_libtpms_reset_tpm_established_flag;
    tbc->get_tpm_version = tpm_libtpms_get_tpm_version;
    tbc->get_buffer_size = tpm_libtpms_get_buffer_size;
    tbc->get_tpm_options = tpm_libtpms_get_tpm_options;
    tbc->handle_request = tpm_libtpms_handle_request;
}

static const TypeInfo tpm_libtpms_info = {
    .name = TYPE_TPM_LIBTPMS,
    .parent = TYPE_TPM_BACKEND,
    .instance_size = sizeof(TPMLibtpms),
    .class_init = tpm_libtpms_class_init,
    .instance_init = tpm_libtpms_inst_init,
    .instance_finalize = tpm_libtpms_inst_finalize,
};

static void tpm_libtpms_register(void)
{
    type_register_static(&tpm_libtpms_info);
}

type_init(tpm_libtpms_register)
