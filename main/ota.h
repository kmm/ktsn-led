#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Firmware updates over the network (CONFIG_KTSN_OTA): the image is written to the inactive app slot while it
 * arrives, checked, then made the boot image. One update at a time. Transport-neutral; http_api.c streams into it. */

/* Start an update of `total` bytes. NULL if fine, else a reason (nothing changed). */
const char *ota_begin(size_t total);
/* Next chunk, any size. The first bytes are checked (ESP image, this chip, a KTSN LED build) before anything is written.
 * NULL if fine, else a reason: the update is aborted. */
const char *ota_write(const void *data, size_t n);
/* All bytes written: verify (checksum) and make it the boot image. NULL if fine, else a reason (update aborted). */
const char *ota_end(void);
void        ota_abort(void);
/* Version of the image being written (from its app description), valid after the first chunk. */
const char *ota_new_version(void);

/* After boot: if this image is new and waiting to prove itself (rollback), mark it good once rendering has run for
 * OTA_HEALTH_S seconds; otherwise restart, and the bootloader goes back to the previous image. */
#define OTA_HEALTH_S 30
void ota_health_check(void);
