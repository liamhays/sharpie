/*
 * Copyright (c) 2022 Geoff Simmons <geoff@simmons.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * See LICENSE
 */

/* Cribbed generously from TinyUSB and the Pico SDK */
// cribbed further from https://gitlab.com/slimhazard/rp2040-rnd/

#include "tusb.h"
#include "pico/unique_id.h"

/* Hijacking the Raspberry Pi PID. */
#define USB_VID (0x2E8A)

#define USB_PID (0xa1b1)
#define USB_BCD (0x0200)

#define USBD_DESC_LEN     (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)
#define USBD_MAX_POWER_MA (50)

#define USBD_ITF_VENDOR (0)
#define USBD_N_ITFS  (1)

#define USBD_VENDOR_EP_OUT          (0x01)
#define USBD_VENDOR_EP_IN           (0x81)
#define USBD_VENDOR_CMD_MAX_SIZE    (8)
#define USBD_VENDOR_IN_OUT_MAX_SIZE (64)

#define USBD_STR_0       (0x00)
#define USBD_STR_LANG    (0x00)
#define USBD_STR_MANUF   (0x01)
#define USBD_STR_PRODUCT (0x02)
#define USBD_STR_SERIAL  (0x03)
#define USBD_STR_VENDOR  (0x04)

#define DESC_STR_MAX (32)

/* Device descriptor */
static const tusb_desc_device_t desc_device = {
  .bLength		= sizeof(tusb_desc_device_t),
  .bDescriptorType	= TUSB_DESC_DEVICE,
  .bcdUSB		= USB_BCD,

  .bDeviceClass		= TUSB_CLASS_VENDOR_SPECIFIC,
  .bDeviceSubClass	= 0,
  .bDeviceProtocol	= 0,
  .bMaxPacketSize0	= CFG_TUD_ENDPOINT0_SIZE,

  .idVendor		= USB_VID,
  .idProduct		= USB_PID,
  .bcdDevice		= 0x0100,
  .iManufacturer	= USBD_STR_MANUF,
  .iProduct		= USBD_STR_PRODUCT,
  .iSerialNumber	= USBD_STR_SERIAL,
  .bNumConfigurations	= 1,
};

/* Configuration Descriptor */
static const uint8_t desc_cfg[] = {
  /*
   * Config number, interface count, string index, total length,
   * attribute, power in mA
   */
  TUD_CONFIG_DESCRIPTOR(1, USBD_N_ITFS, USBD_STR_0, USBD_DESC_LEN, 0x00,
			USBD_MAX_POWER_MA),

  /*
   * VENDOR: Interface number, string index, EP Out & IN address, EP
   * size
   */
  TUD_VENDOR_DESCRIPTOR(USBD_ITF_VENDOR, USBD_STR_VENDOR,
			USBD_VENDOR_EP_OUT, USBD_VENDOR_EP_IN,
			USBD_VENDOR_IN_OUT_MAX_SIZE),
};

/* String Descriptors */
#define ID_LEN (PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1)
static char id[ID_LEN] = { 0 };

static const char * desc_string[] = {
  [USBD_STR_MANUF]	= "chungus",
  [USBD_STR_PRODUCT]	= "sharpie",
  [USBD_STR_SERIAL]	= id,
  [USBD_STR_VENDOR]	= "yo mama",
};

/* For GET DEVICE DESCRIPTOR */
const uint8_t *
tud_descriptor_device_cb(void)
{
  return (const uint8_t *)&desc_device;
}

/* For GET CONFIGURATION DESCRIPTOR */
const uint8_t *
tud_descriptor_configuration_cb(uint8_t index)
{
  (void) index;
  return desc_cfg;
}

/*
 * For GET STRING DESCRIPTOR.
 * Return a string with a two-byte header, followed by the string as UTF-16.
 */
const uint16_t *
tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
  static uint16_t str[DESC_STR_MAX];
  const char *s;
  uint8_t n;
  (void) langid;

  if (id[0] == '\0')
    pico_get_unique_board_id_string(id, ID_LEN);

  if (index >= count_of(desc_string))
    return NULL;
  if (index == USBD_STR_LANG) {
    str[1] = 0x0409; // US English
    n = 1;
  }
  else {
    s = desc_string[index];
    for (n = 0; s[n] && n < DESC_STR_MAX; n++)
      str[n + 1] = s[n];
  }

  /* Header bytes: length including header, string type */
  str[0] = (TUSB_DESC_STRING << 8 ) | (2 * n + 2);
  return str;
}
