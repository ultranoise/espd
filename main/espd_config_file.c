/*
 * Unified config.txt parser — single pass, all keys into g_espd_cfg.
 */

#include "espd_config_file.h"
#include "espd.h"
#include "espd_storage.h"
#include "espd_runtime_config.h"

#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "espd_config";

espd_config_t g_espd_cfg;

static char *cfg_trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

static int cfg_parse_pin_list(const char *v, int *out, int max)
{
    int n = 0;
    const char *p = v;
    if (!v || !*v)
        return 0;
    while (*p && n < max)
    {
        char *end;
        long pin = strtol(p, &end, 10);
        if (p == (const char *)end)
            break;
        out[n++] = (int)pin;
        p = (const char *)end;
        while (*p == ',' || *p == ' ' || *p == '\t')
            p++;
    }
    return n;
}

void espd_config_load(void)
{
    const char *config_path = espd_storage_config_path();
    FILE *f;
    char line[256];

    /* Defaults: -1 means "not set in config.txt, use compiled default". */
    memset(&g_espd_cfg, 0, sizeof(g_espd_cfg));
    g_espd_cfg.usb_role            = ESPD_USB_ROLE_NOMIDI;
    g_espd_cfg.audio_dma_desc_num  = -1;
    g_espd_cfg.audio_dma_frame_num = -1;
    g_espd_cfg.audio_sample_rate   = -1;
    g_espd_cfg.ain_task_period_ms  = -1;
    g_espd_cfg.ain_deadband        = -1;
    g_espd_cfg.ain_report_every    = -1;
    g_espd_cfg.aout_pwm_freq_hz   = -1;
    g_espd_cfg.din_active_low      = -1;
    g_espd_cfg.din_task_period_ms  = -1;
    g_espd_cfg.touch_task_period_ms = -1;
    g_espd_cfg.touch_report_every  = -1;

    if (!config_path)
    {
        ESP_LOGI(TAG, "no config.txt found");
        return;
    }
    f = fopen(config_path, "r");
    if (!f)
    {
        ESP_LOGW(TAG, "cannot read %s", config_path);
        return;
    }

    while (fgets(line, sizeof(line), f))
    {
        char *eq, *k, *v;
        char *comment = strchr(line, '#');
        if (comment)
            *comment = '\0';
        k = cfg_trim(line);
        if (*k == '\0')
            continue;
        eq = strchr(k, '=');
        if (!eq)
            continue;
        *eq++ = '\0';
        v = cfg_trim(eq);
        k = cfg_trim(k);

        /* USB MIDI role: nomidi (default) | device | host */
        if (!strcmp(k, "usb_midi_role"))
        {
            if (!strcasecmp(v, "host"))
                g_espd_cfg.usb_role = ESPD_USB_ROLE_HOST;
            else if (!strcasecmp(v, "device"))
                g_espd_cfg.usb_role = ESPD_USB_ROLE_DEVICE;
            else
                g_espd_cfg.usb_role = ESPD_USB_ROLE_NOMIDI;
        }

        /* WiFi */
        else if (!strcmp(k, "wifi_ssid"))
        {
            snprintf(g_espd_cfg.wifi_ssid, sizeof(g_espd_cfg.wifi_ssid), "%s", v);
            g_espd_cfg.wifi_have_ssid = (g_espd_cfg.wifi_ssid[0] != '\0');
        }
        else if (!strcmp(k, "wifi_password"))
            snprintf(g_espd_cfg.wifi_password, sizeof(g_espd_cfg.wifi_password), "%s", v);

        /* Audio */
        else if (!strcmp(k, "audio_dma_desc_num"))
        {
            int n = atoi(v);
            if (n >= 2 && n <= 16)
                g_espd_cfg.audio_dma_desc_num = n;
        }
        else if (!strcmp(k, "audio_dma_frame_num"))
        {
            int n = atoi(v);
            if (n >= 8 && n <= 1024)
                g_espd_cfg.audio_dma_frame_num = n;
        }
        else if (!strcmp(k, "audio_sample_rate"))
        {
            int n = atoi(v);
            if (n >= ESPD_AUDIO_SAMPLE_RATE_MIN && n <= ESPD_AUDIO_SAMPLE_RATE_MAX)
                g_espd_cfg.audio_sample_rate = n;
            else
                ESP_LOGW(TAG, "ignoring audio_sample_rate=%s (range %d..%d)",
                    v, ESPD_AUDIO_SAMPLE_RATE_MIN, ESPD_AUDIO_SAMPLE_RATE_MAX);
        }

        /* AIN */
        else if (!strcmp(k, "ain_pins"))
        {
            g_espd_cfg.ain_have_pins = true;
            g_espd_cfg.ain_n = cfg_parse_pin_list(v, g_espd_cfg.ain_pins, ESPD_CFG_MAX_PINS);
        }
        else if (!strcmp(k, "ain_task_period_ms"))
        {
            int t = atoi(v);
            if (t >= 1 && t <= 500)
                g_espd_cfg.ain_task_period_ms = t;
        }
        else if (!strcmp(k, "ain_deadband"))
        {
            int d = atoi(v);
            if (d >= 1 && d <= 2047)
                g_espd_cfg.ain_deadband = d;
        }
        else if (!strcmp(k, "ain_report_every_n_blocks"))
        {
            int r = atoi(v);
            if (r >= 1 && r <= 64)
                g_espd_cfg.ain_report_every = r;
        }

        /* AOUT */
        else if (!strcmp(k, "aout_pins"))
        {
            g_espd_cfg.aout_have_pins = true;
            g_espd_cfg.aout_n = cfg_parse_pin_list(v, g_espd_cfg.aout_pins, ESPD_CFG_MAX_PINS);
        }
        else if (!strcmp(k, "aout_pwm_freq_hz"))
        {
            int hz = atoi(v);
            if (hz >= 100 && hz <= 40000000)
                g_espd_cfg.aout_pwm_freq_hz = hz;
        }

        /* DIN */
        else if (!strcmp(k, "din_pins"))
        {
            g_espd_cfg.din_have_pins = true;
            g_espd_cfg.din_n = cfg_parse_pin_list(v, g_espd_cfg.din_pins, ESPD_CFG_MAX_PINS);
        }
        else if (!strcmp(k, "din_active_low"))
            g_espd_cfg.din_active_low = (atoi(v) != 0);
        else if (!strcmp(k, "din_task_period_ms"))
        {
            int t = atoi(v);
            if (t >= 1 && t <= 500)
                g_espd_cfg.din_task_period_ms = t;
        }

        /* DOUT */
        else if (!strcmp(k, "dout_pins"))
        {
            g_espd_cfg.dout_have_pins = true;
            g_espd_cfg.dout_n = cfg_parse_pin_list(v, g_espd_cfg.dout_pins, ESPD_CFG_MAX_PINS);
        }

        /* Touch */
        else if (!strcmp(k, "touch_pins"))
        {
            g_espd_cfg.touch_have_pins = true;
            g_espd_cfg.touch_n = cfg_parse_pin_list(v, g_espd_cfg.touch_pins, ESPD_CFG_MAX_PINS);
        }
        else if (!strcmp(k, "touch_task_period_ms"))
        {
            int t = atoi(v);
            if (t >= 1 && t <= 500)
                g_espd_cfg.touch_task_period_ms = t;
        }
        else if (!strcmp(k, "touch_report_every_n_blocks"))
        {
            int r = atoi(v);
            if (r >= 1 && r <= 64)
                g_espd_cfg.touch_report_every = r;
        }
    }
    fclose(f);

    ESP_LOGI(TAG, "loaded %s", config_path);

    /* Apply audio overrides to runtime config immediately. */
    if (g_espd_cfg.audio_dma_desc_num >= 0)
        espd_audio_set_dma_desc_num(g_espd_cfg.audio_dma_desc_num);
    if (g_espd_cfg.audio_dma_frame_num >= 0)
        espd_audio_set_dma_frame_num(g_espd_cfg.audio_dma_frame_num);
    if (g_espd_cfg.audio_sample_rate >= 0)
        espd_audio_set_sample_rate(g_espd_cfg.audio_sample_rate);

    /* Apply WiFi credentials to globals. */
#ifdef ESPD_USE_WIFI
    if (g_espd_cfg.wifi_have_ssid)
    {
        snprintf(espd_wifi_ssid, sizeof(espd_wifi_ssid), "%s", g_espd_cfg.wifi_ssid);
        snprintf(espd_wifi_password, sizeof(espd_wifi_password), "%s", g_espd_cfg.wifi_password);
    }
    else
    {
        espd_wifi_ssid[0] = '\0';
        espd_wifi_password[0] = '\0';
    }
    ESP_LOGI(TAG, "wifi: ssid=%s sta=%s",
        espd_wifi_ssid[0] ? espd_wifi_ssid : "(none)",
        g_espd_cfg.wifi_have_ssid ? "on" : "off");
#endif
}
