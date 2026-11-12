#include "firmware.h"
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_ota_ops.h"

static const char *state_name(esp_ota_img_states_t s)
{
    switch (s) {
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    default:                         return "";
    }
}

void fw_info(fw_info_t *f)
{
    memset(f, 0, sizeof *f);
    const esp_app_desc_t *d = esp_app_get_description();
    f->version = d->version; f->date = d->date; f->time = d->time; f->idf = d->idf_ver;
    const esp_partition_t *run = esp_ota_get_running_partition(), *boot = esp_ota_get_boot_partition();
    f->slot = run ? run->label : "?";
    esp_ota_img_states_t st;
    f->state = run && esp_ota_get_state_partition(run, &st) == ESP_OK ? state_name(st) : "";
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    if (other) {
        f->has_other = true;
        f->other_slot = other->label;
        esp_app_desc_t od;
        if (esp_ota_get_partition_description(other, &od) == ESP_OK) {
            f->other_valid = true;
            snprintf(f->other_version, sizeof f->other_version, "%.32s", od.version);
            snprintf(f->other_date, sizeof f->other_date, "%.15s", od.date);
            snprintf(f->other_time, sizeof f->other_time, "%.15s", od.time);
        }
    }
    f->next_boot = boot && run && boot != run ? boot->label : NULL;
}
