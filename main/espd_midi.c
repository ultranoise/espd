/*
 * Pd platform MIDI backend on TinyUSB.
 *
 * Pd's device-independent MIDI core (pd/src/s_midi.c) calls the platform hooks
 * defined here. We bridge them to TinyUSB's MIDI device class:
 *
 *   Pd  --[noteout/ctlout/...]-->  outmidi_* --> queue --> sys_putmidi* --> tud_midi_stream_write
 *   USB --> tud_midi_rx_cb --> ring --> sys_poll_midi --> sys_midibytein --> [notein/ctlin/...] in Pd
 *
 * Threading: tud_midi_rx_cb runs in the TinyUSB task; it only feeds a
 * FreeRTOS StreamBuffer. The Pd (audio) task drains that StreamBuffer in
 * sys_poll_midi and writes outbound MIDI with tud_midi_stream_write (guarded by
 * tud_midi_mounted). The StreamBuffer makes RX cross-core safe; MIDI rates are
 * low so the single direct TX call is fine.
 *
 * Compiled on every target so Pd's MIDI objects always exist. The USB parts are
 * gated by CONFIG_ESPD_USE_USB_OTG && CONFIG_ESPD_USE_USB_MIDI; without them the
 * backend is an inert stub (objects present but silent).
 */

#include "sdkconfig.h"
#include "espd_midi.h"
#include "../pd/src/m_pd.h"
#include "../pd/src/s_stuff.h"

#include <string.h>

#define ESPD_MIDI_USB \
    (CONFIG_ESPD_USE_USB_OTG && CONFIG_ESPD_USE_USB_MIDI)

#if (CONFIG_ESPD_USE_USB_OTG && CONFIG_ESPD_USE_USB_MIDI && !CONFIG_TINYUSB_MIDI_COUNT)
#error "ESPD_USE_USB_MIDI requires CONFIG_TINYUSB_MIDI_COUNT >= 1 (set it in the board's sdkconfig.defaults)."
#endif

#if ESPD_MIDI_USB
#include "tusb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"

static const char *TAG = "espd_midi";

#define ESPD_MIDI_RX_BUF   2048
static StreamBufferHandle_t s_rx_stream;
#endif /* ESPD_MIDI_USB */

static int s_midi_open;

/* When set, Pd's outbound MIDI is routed here (USB-MIDI host) instead of the
 * TinyUSB device class. NULL = default device path. */
static espd_midi_out_fn s_out_fn;

/* ─── scheduler shims required by s_midi.c (espd has its own scheduler) ─── */
int sys_schedadvance = 0;
int sched_get_using_audio(void) { return SCHED_AUDIO_NONE; }

/* ─── number of bytes in a channel/system voice message given its status ─── */
#if ESPD_MIDI_USB
static int espd_midi_msglen(int status)
{
    if (status >= 0xF0) {            /* system common / realtime */
        switch (status) {
        case 0xF1:                  /* MTC quarter frame */
        case 0xF3: return 2;        /* song select */
        case 0xF2: return 3;        /* song position pointer */
        default:   return 1;        /* tune request, realtime, etc. */
        }
    }
    switch (status & 0xF0) {
    case 0xC0:                      /* program change */
    case 0xD0: return 2;            /* channel pressure */
    default:   return 3;            /* note on/off, CC, poly AT, pitch bend */
    }
}
#endif

/* ─── TinyUSB inbound callback (runs in the TinyUSB task) ─── */
#if ESPD_MIDI_USB
void tud_midi_rx_cb(uint8_t itf)
{
    (void)itf;
    uint8_t buf[64];
    uint32_t n;
    if (!s_rx_stream)
        return;
    while ((n = tud_midi_stream_read(buf, sizeof(buf))) > 0)
        (void)xStreamBufferSend(s_rx_stream, buf, n, 0);
}
#endif

/* ─── Pd platform MIDI backend hooks (called from pd/src/s_midi.c) ─── */

void sys_do_open_midi(int nmidiindev, int *midiindev,
    int nmidioutdev, int *midioutdev)
{
    (void)midiindev;
    (void)midioutdev;
    s_midi_open = (nmidiindev > 0 || nmidioutdev > 0);
}

void sys_close_midi(void)
{
    s_midi_open = 0;
}

void sys_putmidibyte(int portno, int byte)
{
    (void)portno;
#if ESPD_MIDI_USB
    if (!s_midi_open)
        return;
    uint8_t b = (uint8_t)byte;
    if (s_out_fn) {                 /* host mode: forward to controller */
        s_out_fn(&b, 1);
        return;
    }
    if (!tud_midi_mounted())
        return;
    (void)tud_midi_stream_write(0, &b, 1);
#else
    (void)byte;
#endif
}

void sys_putmidimess(int portno, int a, int b, int c)
{
    (void)portno;
#if ESPD_MIDI_USB
    if (!s_midi_open)
        return;
    uint8_t msg[3] = { (uint8_t)a, (uint8_t)b, (uint8_t)c };
    uint32_t len = (uint32_t)espd_midi_msglen(a);
    if (s_out_fn) {                 /* host mode: forward to controller */
        s_out_fn(msg, len);
        return;
    }
    if (!tud_midi_mounted())
        return;
    (void)tud_midi_stream_write(0, msg, len);
#else
    (void)a; (void)b; (void)c;
#endif
}

void sys_poll_midi(void)
{
#if ESPD_MIDI_USB
    uint8_t buf[64];
    size_t n;
    if (!s_rx_stream)
        return;
    while ((n = xStreamBufferReceive(s_rx_stream, buf, sizeof(buf), 0)) > 0)
        for (size_t i = 0; i < n; i++)
            sys_midibytein(0, buf[i]);   /* port 0 -> Pd MIDI parser */
#endif
}

void midi_getdevs(char *indevlist, int *nindevs,
    char *outdevlist, int *noutdevs, int maxndevs, int devdescsize)
{
    const char *name = "ESPD USB MIDI";
    if (maxndevs < 1 || devdescsize < 1) {
        *nindevs = *noutdevs = 0;
        return;
    }
    strncpy(indevlist, name, devdescsize - 1);
    indevlist[devdescsize - 1] = '\0';
    strncpy(outdevlist, name, devdescsize - 1);
    outdevlist[devdescsize - 1] = '\0';
    *nindevs = 1;
    *noutdevs = 1;
}

/* ─── generic source/sink shared with the USB-MIDI host backend ─── */

void espd_midi_inject_rx(const unsigned char *data, unsigned len)
{
#if ESPD_MIDI_USB
    if (s_rx_stream && data && len)
        (void)xStreamBufferSend(s_rx_stream, data, len, 0);
#else
    (void)data; (void)len;
#endif
}

void espd_midi_set_output(espd_midi_out_fn fn)
{
    s_out_fn = fn;
}

/* ─── espd lifecycle ─── */

void espd_midi_init(void)
{
#if ESPD_MIDI_USB
    if (!s_rx_stream) {
        s_rx_stream = xStreamBufferCreate(ESPD_MIDI_RX_BUF, 1);
        if (!s_rx_stream)
            ESP_LOGE(TAG, "RX stream buffer alloc failed");
    }
#endif
    sys_initmidiqueue();
    {
        int indev = 0, outdev = 0;   /* single device, index 0 */
        sys_open_midi(1, &indev, 1, &outdev, 1);
    }
#if ESPD_MIDI_USB
    ESP_LOGI(TAG, "USB MIDI port ready (native Pd MIDI objects active)");
#endif
}

void espd_midi_poll(void)
{
    sys_pollmidiqueue();   /* polls input (sys_poll_midi) + flushes output queue */
}
