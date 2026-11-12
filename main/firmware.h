#pragma once
#include <stdbool.h>

/* What is running and what is in the other app slot: for `version` and /api/status, and for testing updates and
 * rollback (a freshly updated image is "pending" until ota.c keeps it; a reset before then boots the other slot). */
typedef struct {
    const char *version, *date, *time, *idf;
    const char *slot;            /* running partition label: "factory", "ota_0", "ota_1" */
    const char *state;           /* "valid", "pending" (new, not confirmed yet), "new", "invalid", "aborted", or "" (no OTA) */
    bool        has_other;       /* an OTA layout: there is another app slot */
    const char *other_slot;
    bool        other_valid;     /* the other slot holds an app image */
    char        other_version[33], other_date[16], other_time[16];
    const char *next_boot;       /* boot partition label if it isn't the running one, else NULL */
} fw_info_t;

void fw_info(fw_info_t *f);
