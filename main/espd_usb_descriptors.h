/*
 * Custom composite USB descriptor (CDC [+ MSC] + MIDI).
 *
 * esp_tinyusb's auto descriptor builder does not support the MIDI class, so when
 * USB MIDI is enabled we hand-build the whole configuration descriptor and feed
 * it to the stack via tinyusb_config_t.descriptor. When MIDI is disabled the
 * default auto-generated descriptor is used instead (this file compiles to
 * nothing).
 */
#pragma once

#include "sdkconfig.h"

#if CONFIG_ESPD_USE_USB_OTG && CONFIG_ESPD_USE_USB_MIDI

#include "tinyusb.h"

/* Fill cfg->descriptor with the espd composite (CDC [+MSC] + MIDI) descriptors. */
void espd_usb_apply_midi_descriptor(tinyusb_config_t *cfg);

#endif /* CONFIG_ESPD_USE_USB_OTG && CONFIG_ESPD_USE_USB_MIDI */
