/*
 * ctl_rgberry.c - ALSA external control plugin "rgberry"
 *                 (libasound_module_ctl_rgberry.so)
 *
 * Exposes a "PCM" mixer control (volume 0..100 % + switch) that alsamixer,
 * amixer, EmulationStation, RePlay... can use.  The value lives in the shared
 * segment; the rgberry-audio daemon applies it to the audio and saves it.
 *
 *   ctl.rgberry { type rgberry }
 */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/timerfd.h>
#include <alsa/asoundlib.h>
#include <alsa/control_external.h>
#include "rgb_shm.h"

enum { KEY_VOLUME, KEY_SWITCH, NUM_KEYS };
static const char *const names[NUM_KEYS] = { "PCM Playback Volume", "PCM Playback Switch" };

typedef struct {
    snd_ctl_ext_t ext;
    rgb_shm_t    *shm;
    int           tfd;
    int           subscribed;
    uint32_t      seen_serial;
    unsigned      pending;     /* bit per key: change not reported yet */
} ctl_t;

static int c_elem_count(snd_ctl_ext_t *ext) { (void)ext; return NUM_KEYS; }

static int c_elem_list(snd_ctl_ext_t *ext, unsigned int offset, snd_ctl_elem_id_t *id)
{
    (void)ext;
    if (offset >= NUM_KEYS) return -EINVAL;
    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(id, names[offset]);
    return 0;
}

static snd_ctl_ext_key_t c_find_elem(snd_ctl_ext_t *ext, const snd_ctl_elem_id_t *id)
{
    (void)ext;
    const char *n = snd_ctl_elem_id_get_name(id);
    for (int k = 0; k < NUM_KEYS; k++)
        if (!strcmp(n, names[k])) return k;
    return SND_CTL_EXT_KEY_NOT_FOUND;
}

static int c_get_attribute(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
                           int *type, unsigned int *acc, unsigned int *count)
{
    (void)ext;
    if (key >= NUM_KEYS) return -EINVAL;
    *type  = key == KEY_VOLUME ? SND_CTL_ELEM_TYPE_INTEGER : SND_CTL_ELEM_TYPE_BOOLEAN;
    *acc   = SND_CTL_EXT_ACCESS_READWRITE;
    *count = 1;
    return 0;
}

static int c_get_integer_info(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
                              long *imin, long *imax, long *istep)
{
    (void)ext;
    if (key != KEY_VOLUME) return -EINVAL;
    *imin = 0; *imax = 100; *istep = 1;
    return 0;
}

static int c_read_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value)
{
    ctl_t *c = ext->private_data;
    if (key == KEY_VOLUME) *value = atomic_load(&c->shm->volume);
    else if (key == KEY_SWITCH) *value = !atomic_load(&c->shm->mute);
    else return -EINVAL;
    return 0;
}

static int c_write_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value)
{
    ctl_t *c = ext->private_data;
    long v = *value;
    if (key == KEY_VOLUME) {
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        if (atomic_exchange(&c->shm->volume, (int32_t)v) == v) return 0;
    } else if (key == KEY_SWITCH) {
        int32_t m = v ? 0 : 1;
        if (atomic_exchange(&c->shm->mute, m) == m) return 0;
    } else return -EINVAL;
    atomic_fetch_add(&c->shm->ctl_serial, 1);
    return 1;                                   /* changed */
}

static void c_subscribe_events(snd_ctl_ext_t *ext, int subscribe)
{
    ctl_t *c = ext->private_data;
    c->subscribed = subscribe;
    c->seen_serial = atomic_load(&c->shm->ctl_serial);
}

/* Changes made by another process show up as events (polled via a timer). */
static int c_read_event(snd_ctl_ext_t *ext, snd_ctl_elem_id_t *id, unsigned int *event_mask)
{
    ctl_t *c = ext->private_data;
    uint64_t ticks;
    while (read(c->tfd, &ticks, sizeof ticks) > 0) {}
    uint32_t s = atomic_load(&c->shm->ctl_serial);
    if (s != c->seen_serial) { c->seen_serial = s; c->pending = (1u << NUM_KEYS) - 1; }
    if (!c->subscribed || !c->pending) return -EAGAIN;
    int k = __builtin_ctz(c->pending);
    c->pending &= ~(1u << k);
    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(id, names[k]);
    *event_mask = SND_CTL_EVENT_MASK_VALUE;
    return 1;
}

static void c_close(snd_ctl_ext_t *ext)
{
    ctl_t *c = ext->private_data;
    close(c->tfd);
    rgb_shm_detach(c->shm);
    free(c);
}

static const snd_ctl_ext_callback_t callback = {
    .elem_count       = c_elem_count,
    .elem_list        = c_elem_list,
    .find_elem        = c_find_elem,
    .get_attribute    = c_get_attribute,
    .get_integer_info = c_get_integer_info,
    .read_integer     = c_read_integer,
    .write_integer    = c_write_integer,
    .subscribe_events = c_subscribe_events,
    .read_event       = c_read_event,
    .close            = c_close,
};

SND_CTL_PLUGIN_DEFINE_FUNC(rgberry)
{
    (void)root;
    snd_config_iterator_t i, next;
    snd_config_for_each(i, next, conf) {
        snd_config_t *n = snd_config_iterator_entry(i);
        const char *id;
        if (snd_config_get_id(n, &id) < 0) continue;
        if (!strcmp(id, "comment") || !strcmp(id, "type") || !strcmp(id, "hint")) continue;
        SNDERR("rgberry: unknown field %s", id);
        return -EINVAL;
    }

    ctl_t *c = calloc(1, sizeof(*c));
    if (!c) return -ENOMEM;
    c->shm = rgb_shm_attach();
    if (!c->shm) { free(c); return -ENODEV; }
    c->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    struct itimerspec its = { { 0, 200000000 }, { 0, 200000000 } };
    timerfd_settime(c->tfd, 0, &its, NULL);

    c->ext.version = SND_CTL_EXT_VERSION;
    c->ext.card_idx = 0;
    snprintf(c->ext.id, sizeof c->ext.id, "rgberry");
    snprintf(c->ext.driver, sizeof c->ext.driver, "rgberry");
    snprintf(c->ext.name, sizeof c->ext.name, "RGBerry SCART");
    snprintf(c->ext.longname, sizeof c->ext.longname, "RGBerry SCART audio (RP1 PIO)");
    snprintf(c->ext.mixername, sizeof c->ext.mixername, "RGBerry");
    c->ext.poll_fd = c->tfd;
    c->ext.callback = &callback;
    c->ext.private_data = c;

    int err = snd_ctl_ext_create(&c->ext, name, mode);
    if (err < 0) { close(c->tfd); rgb_shm_detach(c->shm); free(c); return err; }
    *handlep = c->ext.handle;
    return 0;
}
SND_CTL_PLUGIN_SYMBOL(rgberry);
