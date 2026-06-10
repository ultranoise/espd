/*
 * USB-MIDI *host* backend.
 *
 * Lets the board act as a USB host for a class-compliant MIDI controller
 * (e.g. a KORG nanoKONTROL) plugged into the OTG port, forwarding the
 * controller's MIDI into Pd's native MIDI objects via espd_midi, and Pd's
 * outgoing MIDI back to the controller.
 *
 * This is the mutually-exclusive counterpart to the TinyUSB *device* stack
 * (CDC + MSC + MIDI). The role is selected at boot from config.txt
 * (usb_midi_role = device|host); only one may run because the ESP32-S3 has a single
 * USB-OTG PHY. While hosting, USB serial monitoring is unavailable.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* Enable/disable 5 V VBUS on the downstream (host) port. Weak no-op default;
 * override in a board component if the hardware has a VBUS load switch/boost.
 * Without real VBUS the attached controller cannot power on. */
void espd_board_usb_host_set_vbus(bool on);

#if CONFIG_ESPD_USE_USB_MIDI_HOST
/* Install the USB Host stack, enable VBUS, and begin forwarding a connected
 * USB-MIDI controller to/from Pd. Spawns its own daemon + client tasks and
 * returns once the host stack is running. Call instead of the device stack
 * when config.txt selects usb_midi_role=host. */
esp_err_t espd_usb_host_midi_start(void);
#endif
