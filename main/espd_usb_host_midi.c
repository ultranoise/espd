/*
 * USB-MIDI host backend — see espd_usb_host_midi.h.
 *
 * Flow:
 *   controller --bulk IN--> in_xfer_cb --[parse 4-byte USB-MIDI events]-->
 *                           espd_midi_inject_rx --> Pd ([notein]/[ctlin]/...)
 *   Pd ([noteout]/...) --> espd_midi out callback --[pack USB-MIDI event]-->
 *                          bulk OUT --> controller
 *
 * Built only when CONFIG_ESPD_USE_USB_MIDI_HOST. The VBUS hook is always
 * compiled so boards can provide a strong override.
 */

#include "espd_usb_host_midi.h"
#include "sdkconfig.h"
#include "esp_log.h"

/* Weak default: most boards have no controllable VBUS switch. A board whose
 * hardware can source 5 V to the downstream port should provide a strong
 * espd_board_usb_host_set_vbus() that drives the enable GPIO. */
__attribute__((weak)) void espd_board_usb_host_set_vbus(bool on)
{
    (void)on;
}

#if CONFIG_ESPD_USE_USB_MIDI_HOST

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "espd_midi.h"
#include <string.h>

static const char *TAG = "espd_usb_host_midi";

/* USB descriptor type / endpoint attribute constants (USB 2.0 ch.9) — used as
 * literals to avoid depending on macro spellings across IDF versions. */
#define DESC_TYPE_INTERFACE   0x04
#define DESC_TYPE_ENDPOINT    0x05
#define EP_XFER_TYPE_MASK     0x03
#define EP_XFER_TYPE_BULK     0x02
#define EP_DIR_IN             0x80
#define IFACE_CLASS_AUDIO     0x01
#define IFACE_SUBCLASS_MIDI   0x03

/* USB-MIDI 1.0 Code Index Number -> number of valid MIDI bytes in the packet. */
static const uint8_t s_cin_len[16] = {
    0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1
};

static usb_host_client_handle_t s_client;
static usb_device_handle_t      s_dev;        /* current controller, or NULL */
static uint8_t                  s_dev_addr;

static usb_transfer_t          *s_in_xfer;
static usb_transfer_t          *s_out_xfer;
static uint8_t                  s_in_ep, s_out_ep;
static uint16_t                 s_in_mps, s_out_mps;
static uint8_t                  s_iface_num;
static volatile bool            s_out_busy;

/* ─── inbound: bulk IN completion ─── */

static void in_xfer_cb(usb_transfer_t *t)
{
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        const uint8_t *p = t->data_buffer;
        int n = t->actual_num_bytes;
        for (int i = 0; i + 4 <= n; i += 4) {
            uint8_t cin = p[i] & 0x0F;
            uint8_t len = s_cin_len[cin];
            if (len)
                espd_midi_inject_rx(&p[i + 1], len);
        }
        /* keep listening */
        if (s_dev && usb_host_transfer_submit(t) != ESP_OK)
            ESP_LOGW(TAG, "IN resubmit failed");
    } else if (t->status != USB_TRANSFER_STATUS_NO_DEVICE &&
               t->status != USB_TRANSFER_STATUS_CANCELED) {
        ESP_LOGW(TAG, "IN xfer status %d", t->status);
        if (s_dev)
            (void)usb_host_transfer_submit(t);
    }
}

/* ─── outbound: Pd -> controller (registered with espd_midi_set_output) ─── */

static void out_xfer_cb(usb_transfer_t *t)
{
    (void)t;
    s_out_busy = false;
}

static void host_midi_out(const unsigned char *data, unsigned len)
{
    if (!s_dev || !s_out_ep || !s_out_xfer || len == 0)
        return;
    if (s_out_busy)            /* previous packet still in flight — drop */
        return;

    uint8_t status = data[0];
    uint8_t cin;
    if (status >= 0xF0)
        cin = (len == 1) ? 0x0F : (len == 2) ? 0x02 : 0x03;
    else if (status >= 0x80)
        cin = status >> 4;     /* channel voice: CIN == high nibble of status */
    else
        cin = 0x0F;            /* lone data byte (running status): best effort */

    uint8_t *b = s_out_xfer->data_buffer;
    b[0] = cin & 0x0F;         /* cable number 0 */
    b[1] = (len > 0) ? data[0] : 0;
    b[2] = (len > 1) ? data[1] : 0;
    b[3] = (len > 2) ? data[2] : 0;

    s_out_xfer->num_bytes        = 4;
    s_out_xfer->bEndpointAddress = s_out_ep;
    s_out_xfer->device_handle    = s_dev;
    s_out_xfer->callback         = out_xfer_cb;
    s_out_busy = true;
    if (usb_host_transfer_submit(s_out_xfer) != ESP_OK)
        s_out_busy = false;
}

/* ─── device open / close ─── */

static void close_device(void)
{
    espd_midi_set_output(NULL);
    if (s_in_xfer)  { usb_host_transfer_free(s_in_xfer);  s_in_xfer = NULL; }
    if (s_out_xfer) { usb_host_transfer_free(s_out_xfer); s_out_xfer = NULL; }
    if (s_dev) {
        (void)usb_host_interface_release(s_client, s_dev, s_iface_num);
        (void)usb_host_device_close(s_client, s_dev);
        s_dev = NULL;
    }
    s_in_ep = s_out_ep = 0;
    s_in_mps = s_out_mps = 0;
    s_out_busy = false;
}

static bool find_midi_endpoints(const usb_config_desc_t *cfg)
{
    const uint8_t *p = (const uint8_t *)cfg;
    int total = cfg->wTotalLength, off = 0;
    bool in_midi = false;

    s_in_ep = s_out_ep = 0;
    while (off + 2 <= total) {
        const usb_standard_desc_t *d = (const usb_standard_desc_t *)(p + off);
        if (d->bLength == 0)
            break;
        if (d->bDescriptorType == DESC_TYPE_INTERFACE) {
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)d;
            in_midi = (intf->bInterfaceClass == IFACE_CLASS_AUDIO &&
                       intf->bInterfaceSubClass == IFACE_SUBCLASS_MIDI);
            if (in_midi)
                s_iface_num = intf->bInterfaceNumber;
        } else if (d->bDescriptorType == DESC_TYPE_ENDPOINT && in_midi) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)d;
            if ((ep->bmAttributes & EP_XFER_TYPE_MASK) == EP_XFER_TYPE_BULK) {
                uint16_t mps = ep->wMaxPacketSize & 0x07FF;
                if (ep->bEndpointAddress & EP_DIR_IN) {
                    s_in_ep = ep->bEndpointAddress; s_in_mps = mps;
                } else {
                    s_out_ep = ep->bEndpointAddress; s_out_mps = mps;
                }
            }
        }
        off += d->bLength;
    }
    return s_in_ep != 0;
}

static void open_device(uint8_t addr)
{
    if (s_dev)                 /* already hosting one controller */
        return;
    if (usb_host_device_open(s_client, addr, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "device_open(%u) failed", addr);
        s_dev = NULL;
        return;
    }

    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(s_dev, &cfg) != ESP_OK || !cfg) {
        ESP_LOGW(TAG, "no config descriptor");
        close_device();
        return;
    }
    if (!find_midi_endpoints(cfg)) {
        ESP_LOGW(TAG, "no USB-MIDI (Audio/MIDIStreaming) interface on device");
        close_device();
        return;
    }
    if (usb_host_interface_claim(s_client, s_dev, s_iface_num, 0) != ESP_OK) {
        ESP_LOGE(TAG, "interface_claim(%u) failed", s_iface_num);
        close_device();
        return;
    }

    /* inbound transfer: one bulk-IN max-packet, resubmitted forever */
    if (usb_host_transfer_alloc(s_in_mps ? s_in_mps : 64, 0, &s_in_xfer) != ESP_OK) {
        ESP_LOGE(TAG, "IN transfer alloc failed");
        close_device();
        return;
    }
    s_in_xfer->device_handle    = s_dev;
    s_in_xfer->bEndpointAddress = s_in_ep;
    s_in_xfer->callback         = in_xfer_cb;
    s_in_xfer->num_bytes        = s_in_mps ? s_in_mps : 64;

    /* outbound transfer (optional): only if the controller exposes a bulk-OUT */
    if (s_out_ep) {
        uint16_t sz = s_out_mps < 4 ? 4 : s_out_mps;
        if (usb_host_transfer_alloc(sz, 0, &s_out_xfer) == ESP_OK)
            espd_midi_set_output(host_midi_out);
        else
            ESP_LOGW(TAG, "OUT transfer alloc failed (input still works)");
    }

    if (usb_host_transfer_submit(s_in_xfer) != ESP_OK) {
        ESP_LOGE(TAG, "IN submit failed");
        close_device();
        return;
    }
    s_dev_addr = addr;
    ESP_LOGI(TAG, "controller ready: in_ep=0x%02x (mps %u)%s",
        s_in_ep, s_in_mps, s_out_ep ? ", out_ep present" : "");
}

/* ─── client event callback (runs in client task) ─── */

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGI(TAG, "device connected (addr %u)", msg->new_dev.address);
        open_device(msg->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGI(TAG, "device disconnected");
        close_device();
        break;
    default:
        break;
    }
}

/* ─── tasks ─── */

static void usb_host_lib_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
            usb_host_device_free_all();
    }
}

static void usb_host_client_task(void *arg)
{
    (void)arg;
    usb_host_client_config_t client_cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    if (usb_host_client_register(&client_cfg, &s_client) != ESP_OK) {
        ESP_LOGE(TAG, "client_register failed");
        vTaskDelete(NULL);
        return;
    }
    while (1)
        usb_host_client_handle_events(s_client, portMAX_DELAY);
}

/* ─── public entry ─── */

esp_err_t espd_usb_host_midi_start(void)
{
    espd_board_usb_host_set_vbus(true);

    usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(err));
        espd_board_usb_host_set_vbus(false);
        return err;
    }

    if (xTaskCreatePinnedToCore(usb_host_lib_task, "usbh_lib", 4096, NULL,
            5, NULL, 0) != pdPASS ||
        xTaskCreatePinnedToCore(usb_host_client_task, "usbh_midi", 4096, NULL,
            5, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "host task create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "USB-MIDI host started — plug in a class-compliant controller");
    return ESP_OK;
}

#endif /* CONFIG_ESPD_USE_USB_MIDI_HOST */
