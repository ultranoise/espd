/*
 * USB OTG subsystem: TinyUSB driver, MSC (mass storage), CDC (serial), dev-sync.
 * Extracted from espd.c — preserves MSC sync mechanism and early VFS mount.
 */

#include "espd_usb.h"
#include "espd.h"
#include "espd_storage.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_flash.h"
/* Full esp_flash_t definition (size field): grow the default chip's recorded
 * size from the build placeholder to the detected physical size so the whole
 * chip is addressable (see espd_usb_register_dynamic_storage). */
#include "esp_flash_chips/esp_flash_types.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdint.h>
#include "driver/uart.h"

#if CONFIG_ESPD_USE_USB_OTG
#include "tinyusb.h"
#include "tinyusb_msc.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "tinyusb_default_config.h"
#include "espd_usb_descriptors.h"
#endif
#if (CONFIG_ESPD_USE_USB_OTG && CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED) || (CONFIG_ESPD_DEV_SERIAL_SYNC && CONFIG_USJ_ENABLE_USB_SERIAL_JTAG && CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED)
#include "driver/usb_serial_jtag.h"
#endif
#if CONFIG_ESPD_DEV_CDC_SYNC
#include "espd_dev.h"
#endif
#if CONFIG_FATFS_USE_LABEL
#include "ff.h"
#endif

static const char *TAG = "espd_usb";

#if CONFIG_ESPD_USE_USB_OTG && CONFIG_ESPD_USE_USB_MSC
static tinyusb_msc_storage_handle_t msc_handle = NULL;
static volatile bool s_msc_should_exit_drive_mode = false;
static bool s_msc_disabled_after_eject = false;
#endif
static wl_handle_t wl_handle = WL_INVALID_HANDLE;
static bool s_flash_vfs_early;

/* ─── Early VFS mount (plain FAT, no TinyUSB — for config.txt before USB) ─── */

esp_err_t espd_usb_mount_flash_early_vfs(void)
{
    esp_vfs_fat_mount_config_t mount_cfg;

    if (s_flash_vfs_early)
        return ESP_OK;

    mount_cfg = (esp_vfs_fat_mount_config_t){
        .max_files = 16,
        .format_if_mount_failed = true,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        ESPD_STORAGE_MOUNT, "storage", &mount_cfg, &wl_handle);
    if (err != ESP_OK)
        return err;
    s_flash_vfs_early = true;
    return ESP_OK;
}

static esp_err_t espd_usb_unmount_flash_early_vfs(void)
{
    if (!s_flash_vfs_early)
        return ESP_OK;
    esp_err_t err = esp_vfs_fat_spiflash_unmount_rw_wl(ESPD_STORAGE_MOUNT, wl_handle);
    s_flash_vfs_early = false;
    wl_handle = WL_INVALID_HANDLE;
    return err;
}
/* ─── Dynamic storage partition ─── */

/* Register a FAT "storage" partition spanning all flash after the last partition
 * in the table. The base table ships NO storage partition, so one firmware fills
 * whatever flash the chip actually has — espd stays flash-size-agnostic. Runs once
 * at boot, before /storage is first looked up (early VFS / MSC). Requires
 * CONFIG_ESPTOOLPY_FLASHSIZE_DETECT so the flash driver reports the real size. */
esp_err_t espd_usb_register_dynamic_storage(void)
{
    if (esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
            ESP_PARTITION_SUBTYPE_DATA_FAT, "storage"))
        return ESP_OK;   /* already present */

    /* True physical chip size via live SFDP detection — NOT esp_flash_get_size(),
     * which returns the *configured* size baked from the bootloader header (a fixed
     * build placeholder). The header is only patched to the real size at flash time
     * if the flashing tool honors HEADER_FLASHSIZE_UPDATE + detect; the web flasher
     * (esptool.js) does not, so we must detect at runtime to be flasher-independent. */
    uint32_t flash_size = 0;
    esp_err_t err = esp_flash_get_physical_size(NULL, &flash_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "flash size query failed: %s", esp_err_to_name(err));
        return err;
    }

    /* esp_partition_register_external() bounds-checks the region against the default
     * chip's configured ->size (still the placeholder). Grow it to the detected
     * physical size so the check passes and FAT/wear-levelling I/O can reach the
     * whole chip. Only ever grows toward the real size; a no-op on a smaller chip. */
    if (esp_flash_default_chip && esp_flash_default_chip->size < flash_size)
        esp_flash_default_chip->size = flash_size;

    uint32_t used_end = 0;
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it) {
        const esp_partition_t *p = esp_partition_get(it);
        uint32_t end = p->address + p->size;
        if (end > used_end)
            used_end = end;
        it = esp_partition_next(it);
    }

    if (used_end == 0 || used_end >= flash_size) {
        ESP_LOGW(TAG, "no free flash for /storage (used=0x%x flash=0x%x)",
            (unsigned)used_end, (unsigned)flash_size);
        return ESP_OK;
    }

    const esp_partition_t *part = NULL;
    err = esp_partition_register_external(NULL, used_end, flash_size - used_end,
        "storage", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, &part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register /storage failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "/storage: %u KiB at 0x%x (flash %u KiB)",
        (unsigned)((flash_size - used_end) / 1024), (unsigned)used_end,
        (unsigned)(flash_size / 1024));
    return ESP_OK;
}

/* ─── FAT volume label ─── */

static void espd_usb_apply_msc_volume_label_when_ready(void)
{
#if CONFIG_FATFS_USE_LABEL
    FRESULT res;
    const char *label = "ESPD";
    res = f_setlabel(label);
    if (res == FR_OK)
        ESP_LOGI(TAG, "MSC volume label set to '%s'", label);
    else
        ESP_LOGW(TAG, "Failed to set MSC volume label (res=%d)", res);
#endif
}

/* ─── CDC write (serialized for esp_log + protocol replies) ─── */

void espd_serial_sync_write(const void *data, size_t len)
{
    if (!data || len == 0)
        return;

#if CONFIG_ESPD_DEV_SERIAL_SYNC
#if CONFIG_USJ_ENABLE_USB_SERIAL_JTAG && CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
    usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(100));
#else
    uart_write_bytes(UART_NUM_0, data, len);
#endif
#elif CONFIG_ESPD_DEV_CDC_SYNC
    if (!tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0) || !tud_mounted())
        return;
    {
        size_t off = 0;
        while (off < len) {
            size_t w = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,
                    (const uint8_t *)data + off, len - off);
            if (w == 0)
                break;   /* endpoint busy — drop remainder */
            off += w;
            if (off < len)
                tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
        }
        (void)tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    }
#endif
}

int espd_serial_sync_log(const char *fmt, va_list args)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n > 0) {
        size_t w = (size_t)n;
        if (w >= sizeof(buf))
            w = sizeof(buf) - 1;
        espd_serial_sync_write(buf, w);
    }
    return n;
}

/* ─── TinyUSB MSC storage ─── */

#if CONFIG_ESPD_USE_USB_OTG && CONFIG_ESPD_USE_USB_MSC
static void espd_usb_msc_event_callback(tinyusb_msc_storage_handle_t handle,
                                          tinyusb_msc_event_t *event, void *arg)
{
    (void)handle;
    (void)arg;

    switch (event->id) {
    case TINYUSB_MSC_EVENT_MOUNT_START:
        ESP_LOGI(TAG, "MSC mount start (mount_point=%d)", event->mount_point);
        break;
    case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
        ESP_LOGI(TAG, "MSC mount complete (mount_point=%d)", event->mount_point);
        if (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
            s_msc_should_exit_drive_mode = true;
        }
        break;
    case TINYUSB_MSC_EVENT_MOUNT_FAILED:
        ESP_LOGW(TAG, "MSC mount failed (mount_point=%d)", event->mount_point);
        break;
    case TINYUSB_MSC_EVENT_FORMAT_REQUIRED:
        ESP_LOGW(TAG, "MSC format required");
        break;
    case TINYUSB_MSC_EVENT_FORMAT_FAILED:
        ESP_LOGW(TAG, "MSC format failed");
        break;
    default:
        break;
    }
}
#endif

#if CONFIG_ESPD_USE_USB_OTG

#define ESPD_USB_TASK_CORE          0
#define ESPD_USB_INIT_TASK_PRIO     2
#define ESPD_USB_DEVICE_TASK_PRIO   4

#if CONFIG_ESPD_USE_USB_MSC
static esp_err_t espd_usb_msc_driver_ensure(void)
{
    tinyusb_msc_driver_config_t msc_drv_cfg = {
        .user_flags.auto_mount_off = (esp_reset_reason() != ESP_RST_POWERON),
        .callback = espd_usb_msc_event_callback,
        .callback_arg = NULL,
    };
    esp_err_t err = tinyusb_msc_install_driver(&msc_drv_cfg);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE)
        return ESP_OK;
    ESP_LOGW(TAG, "MSC driver install: %s", esp_err_to_name(err));
    return err;
}

static esp_err_t espd_usb_mount_storage_app(void)
{
    const esp_partition_t *data_partition;
    esp_err_t err;

    if (msc_handle != NULL)
        return ESP_OK;

    if (s_flash_vfs_early) {
        err = espd_usb_unmount_flash_early_vfs();
        if (err != ESP_OK)
            return err;
    }

    err = espd_usb_msc_driver_ensure();
    if (err != ESP_OK)
        return err;

    data_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (!data_partition) {
        ESP_LOGE(TAG, "'storage' partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    if (wl_handle == WL_INVALID_HANDLE) {
        err = wl_mount(data_partition, &wl_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wear levelling failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    {
        tinyusb_msc_storage_config_t msc_storage_cfg = {
            .medium.wl_handle = wl_handle,
            .fat_fs = {
                .base_path = ESPD_STORAGE_MOUNT,
                .config = {
                    .max_files = 64,
                    .format_if_mount_failed = true,
                    .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
                },
                .do_not_format = false,
                .format_flags = FM_FAT,
            },
            .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
        };
        err = tinyusb_msc_new_storage_spiflash(&msc_storage_cfg, &msc_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "MSC storage failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "/storage mounted (APP)");
    espd_usb_apply_msc_volume_label_when_ready();
    return ESP_OK;
}

bool espd_usb_msc_storage_present(void)
{
    return msc_handle != NULL;
}

bool espd_usb_msc_host_mounted(void)
{
    tinyusb_msc_mount_point_t mp;
    if (!msc_handle)
        return false;
    if (tinyusb_msc_get_storage_mount_point(msc_handle, &mp) != ESP_OK)
        return false;
    return mp == TINYUSB_MSC_STORAGE_MOUNT_USB;
}

esp_err_t espd_usb_expose_msc_to_host(void)
{
    if (!msc_handle)
        return ESP_ERR_INVALID_STATE;
    if (espd_usb_msc_host_mounted())
        return ESP_OK;
    if (!tud_mounted())
        return ESP_OK;   /* no host — keep VFS available to app */
#if CONFIG_ESPD_DEV_CDC_SYNC
    if (tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0)) {
        (void)tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(100));
    }
#endif
    vTaskDelay(pdMS_TO_TICKS(10));
    return tinyusb_msc_set_storage_mount_point(msc_handle,
        TINYUSB_MSC_STORAGE_MOUNT_USB);
}

esp_err_t espd_usb_ensure_msc_app_mount(void)
{
    if (!msc_handle)
        return ESP_ERR_INVALID_STATE;
    if (!espd_usb_msc_host_mounted())
        return ESP_OK;
    return tinyusb_msc_set_storage_mount_point(msc_handle,
        TINYUSB_MSC_STORAGE_MOUNT_APP);
}
#endif /* CONFIG_ESPD_USE_USB_MSC */

#if CONFIG_ESPD_USE_USB_OTG && CONFIG_ESPD_USE_USB_MSC
esp_err_t espd_usb_msc_unmount_storage(void)
{
    if (!msc_handle) {
        return ESP_OK; /* Already disabled */
    }

    ESP_LOGI(TAG, "Unmounting MSC storage");
    esp_err_t err = tinyusb_msc_delete_storage(msc_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "MSC storage unmounted");
        msc_handle = NULL;
    } else {
        ESP_LOGW(TAG, "MSC storage unmounting failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t espd_usb_msc_reinstall_driver_with_auto_mount_off(void)
{
    /* Uninstall MSC driver */
    esp_err_t err = tinyusb_msc_uninstall_driver();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "MSC driver uninstalled");
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "MSC driver not installed");
    } else {
        ESP_LOGW(TAG, "MSC driver uninstall failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Reinstall MSC driver with auto_mount_off=1 to prevent auto-remount on USB reconnect */
    tinyusb_msc_driver_config_t msc_drv_cfg = {
        .user_flags.auto_mount_off = 1,
        .callback = NULL,
        .callback_arg = NULL,
    };
    err = tinyusb_msc_install_driver(&msc_drv_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "MSC driver reinstalled with auto_mount_off=1");
    } else {
        ESP_LOGW(TAG, "MSC driver reinstall failed: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t espd_usb_msc_remount_vfs(void)
{
    /* Remount storage using direct VFS */
    esp_err_t err = espd_usb_mount_flash_early_vfs();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Storage remounted via direct VFS");
        s_msc_disabled_after_eject = true;
    } else {
        ESP_LOGE(TAG, "Failed to remount storage via direct VFS: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t espd_usb_msc_disable_and_remount_vfs(void)
{
    esp_err_t err = espd_usb_msc_unmount_storage();
    if (err != ESP_OK) {
        return err;
    }

    err = espd_usb_msc_reinstall_driver_with_auto_mount_off();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MSC driver reinstall failed, continuing anyway");
    }

    err = espd_usb_msc_remount_vfs();
    return err;
}

void espd_usb_msc_disable_after_eject()
{
    s_msc_disabled_after_eject = true;
}
#endif /* CONFIG_ESPD_USE_USB_MSC */

/* ─── USJ teardown and TinyUSB boot ─── */

static void espd_usb_release_usj_for_otg(void)
{
#if CONFIG_ESPD_DEV_SERIAL_SYNC && CONFIG_USJ_ENABLE_USB_SERIAL_JTAG && CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
    fflush(stdout);
    fflush(stderr);
    (void)usb_serial_jtag_driver_uninstall();
#endif
}

#if CONFIG_ESPD_USE_USB_MSC
bool espd_usb_wait_for_host(uint32_t timeout_ticks)
{
    TickType_t start = xTaskGetTickCount();
    while (!tud_mounted()) {
        if ((TickType_t)(xTaskGetTickCount() - start) >= (TickType_t)timeout_ticks)
            return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

void espd_usb_drive_mode_wait(void)
{
    (void)espd_usb_expose_msc_to_host();
    ESP_LOGI(TAG, "USB drive mode -- eject to start audio");

    s_msc_should_exit_drive_mode = false;
    while (!s_msc_should_exit_drive_mode) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "USB drive ejected by host");
}
#endif

static bool usb_init_on_core0(void)
{
    esp_err_t err;
    tinyusb_config_t tusb_cfg;

    espd_usb_release_usj_for_otg();

#if CONFIG_ESPD_USE_USB_MSC
    if (espd_usb_msc_driver_ensure() != ESP_OK)
        ESP_LOGW(TAG, "MSC driver pre-install failed");
#endif

    tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.task = TINYUSB_TASK_CUSTOM(
        TINYUSB_DEFAULT_TASK_SIZE, ESPD_USB_DEVICE_TASK_PRIO, ESPD_USB_TASK_CORE);
#if CONFIG_ESPD_USE_USB_MIDI
    /* esp_tinyusb's auto descriptor builder cannot add the MIDI class, so supply
     * a hand-built composite (CDC [+MSC] + MIDI) descriptor. */
    espd_usb_apply_midi_descriptor(&tusb_cfg);
#endif
    err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "TinyUSB install failed: %s", esp_err_to_name(err));
        return false;
    }

#if CONFIG_ESPD_DEV_CDC_SYNC
    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = espd_dev_cdc_rx_cb,
    };
#else
    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
    };
#endif
    err = tinyusb_cdcacm_init(&acm_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CDC init: %s", esp_err_to_name(err));
        return false;
    }

#if CONFIG_ESPD_DEV_CDC_SYNC
    espd_dev_init();
    esp_log_set_vprintf(espd_serial_sync_log);
#elif CONFIG_ESPD_USB_CONSOLE_CDC
    err = tinyusb_console_init(TINYUSB_CDC_ACM_0);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "CDC console: %s", esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "logs on CDC (cu.usbmodem*)");
#elif CONFIG_ESPD_USE_USB_MSC
    ESP_LOGW(TAG, "MSC only — enable OTG CDC for serial logs");
#endif

#if CONFIG_ESPD_USE_USB_MSC
    if (msc_handle == NULL && !s_msc_disabled_after_eject) {
        err = espd_usb_mount_storage_app();
        if (err != ESP_OK)
            ESP_LOGW(TAG, "/storage mount failed: %s", esp_err_to_name(err));
        else
            espd_storage_resolve_paths();
    }
#endif
    ESP_LOGI(TAG, "ready (cu.usbmodem%s1)", CONFIG_TINYUSB_DESC_SERIAL_STRING);
    return true;
}

#define ESPD_USB_BOOT_STACK        10240
#define ESPD_USB_BOOT_TIMEOUT_MS   15000

static void usb_boot_task(void *arg)
{
    TaskHandle_t waiter = (TaskHandle_t)arg;
    bool ok = usb_init_on_core0();

    if (waiter)
        xTaskNotify(waiter, ok ? 1 : 0, eSetValueWithOverwrite);
    vTaskDelete(NULL);
}

bool espd_usb_start_after_wifi(void)
{
    static bool started;
    TaskHandle_t waiter;
    uint32_t note = 0;

    if (started)
        return true;
    started = true;

    waiter = xTaskGetCurrentTaskHandle();
    if (xTaskCreatePinnedToCore(usb_boot_task, "usb_otg", ESPD_USB_BOOT_STACK, waiter,
            6, NULL, ESPD_USB_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return false;
    }
    if (xTaskNotifyWait(0, UINT32_MAX, &note,
            pdMS_TO_TICKS(ESPD_USB_BOOT_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "init timeout (%d ms)", ESPD_USB_BOOT_TIMEOUT_MS);
        return false;
    }
    return note != 0;
}
#endif /* CONFIG_ESPD_USE_USB_OTG */
