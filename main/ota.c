#include "ota.h"
#include <stdio.h>
#include <string.h>
#include "render.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "ota";

/* The image starts with the image header, the first segment header, then the app description: checked before writing. */
#define HEAD_LEN (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

static struct {
    bool                   active;
    const esp_partition_t *part;
    esp_ota_handle_t       handle;
    size_t                 total, done;
    uint8_t                head[HEAD_LEN];
    size_t                 head_len;
    char                   version[33];
} s;

const char *ota_new_version(void) { return s.version; }

void ota_abort(void)
{
    if (!s.active) return;
    if (s.handle) esp_ota_abort(s.handle);
    s.active = false;
    s.handle = 0;
    render_set_progress(-1);
    ESP_LOGW(TAG, "update aborted");
}

const char *ota_begin(size_t total)
{
    if (s.active) return "another update is in progress";
    s.part = esp_ota_get_next_update_partition(NULL);
    if (!s.part) return "this board has no second app slot (flash it once over USB with the OTA layout: sdkconfig.flash4mb)";
    if (total < HEAD_LEN) return "that is not a firmware image (too short)";
    if (total > s.part->size) return "image too large for the app slot";
    s.total = total; s.done = 0; s.head_len = 0; s.handle = 0; s.version[0] = 0;
    s.active = true;
    render_set_progress(0);
    ESP_LOGI(TAG, "update started: %u bytes into %s", (unsigned)total, s.part->label);
    return NULL;
}

static const char *check_head(void)
{
    const esp_image_header_t *h = (const void *)s.head;
    const esp_app_desc_t *d = (const void *)(s.head + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (h->magic != ESP_IMAGE_HEADER_MAGIC || d->magic_word != ESP_APP_DESC_MAGIC_WORD) return "that is not an ESP-IDF app image (use build/ktsn-led.bin)";
    if (h->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) return "the image is for a different chip";
    const char *mine = esp_app_get_description()->project_name;    /* "ktsn-led"; "ledctl" before the rename */
    if (strncmp(d->project_name, mine, sizeof d->project_name) && strncmp(d->project_name, "ledctl", sizeof d->project_name))
        return "the image is not a KTSN LED build";
    snprintf(s.version, sizeof s.version, "%.32s", d->version);
    return NULL;
}

/* esp_ota_begin erases the slot as it goes (sequential writes), so nothing is erased before the image looks right. */
static const char *write_flash(const void *data, size_t n)
{
    if (!s.handle) {
        esp_err_t e = esp_ota_begin(s.part, OTA_WITH_SEQUENTIAL_WRITES, &s.handle);
        if (e != ESP_OK) { s.handle = 0; return esp_err_to_name(e); }
    }
    esp_err_t e = esp_ota_write(s.handle, data, n);
    return e == ESP_OK ? NULL : esp_err_to_name(e);
}

const char *ota_write(const void *data, size_t n)
{
    if (!s.active) return "no update in progress";
    if (s.done + n > s.total) { ota_abort(); return "more data than announced"; }
    const uint8_t *p = data;
    const char *why = NULL;
    if (s.head_len < HEAD_LEN) {                         /* collect and check the head, then write it in one go */
        size_t k = HEAD_LEN - s.head_len < n ? HEAD_LEN - s.head_len : n;
        memcpy(s.head + s.head_len, p, k);
        s.head_len += k; p += k; n -= k; s.done += k;
        if (s.head_len == HEAD_LEN && !(why = check_head())) why = write_flash(s.head, HEAD_LEN);
    }
    if (!why && n) { why = write_flash(p, n); s.done += n; }
    if (why) { ota_abort(); return why; }
    render_set_progress((int)((uint64_t)s.done * 1000 / s.total));
    return NULL;
}

const char *ota_end(void)
{
    if (!s.active) return "no update in progress";
    if (s.done != s.total || !s.handle) { ota_abort(); return "image incomplete"; }
    esp_err_t e = esp_ota_end(s.handle);                 /* verifies the image (segments, SHA-256) */
    s.handle = 0;
    if (e == ESP_OK) e = esp_ota_set_boot_partition(s.part);
    s.active = false;
    if (e != ESP_OK) { render_set_progress(-1); ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(e)); return e == ESP_ERR_OTA_VALIDATE_FAILED ? "image is corrupt (checksum)" : esp_err_to_name(e); }
    render_set_progress(1000);                           /* full bar until the restart */
    ESP_LOGI(TAG, "update complete: %s, boots from %s after restart", s.version, s.part->label);
    return NULL;
}

static void health_cb(void *arg)
{
    (void)arg;
    if (g_perf.frames > 0) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware is running fine: kept");
    } else {
        ESP_LOGE(TAG, "new firmware isn't rendering after %d s: restarting into the previous one", OTA_HEALTH_S);
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

void ota_health_check(void)
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) return;
    ESP_LOGI(TAG, "first boot of new firmware: keeping it if it runs for %d s", OTA_HEALTH_S);
    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = { .callback = health_cb, .name = "ota_health" };
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, (uint64_t)OTA_HEALTH_S * 1000000);
}
