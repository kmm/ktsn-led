#include "led_out.h"
#include "config.h"
#include "led_strip.h"
#include "sdkconfig.h"
#include "esp_log.h"

#define MAX_OUT CHANNELS_MAX
static const char *TAG = "led_out";

/* Per channel: the wire format it was set up with. Shared by both drivers. */
static wire_t s_wire[MAX_OUT];
static char   s_wire_str[MAX_OUT][WIRE_STR_MAX];
static uint16_t s_npix[MAX_OUT];

static void set_wire(int idx, const char *str, uint16_t npix)
{
    if (wire_parse(str ? str : WIRE_DEFAULT, &s_wire[idx], s_wire_str[idx])) wire_parse(WIRE_DEFAULT, &s_wire[idx], s_wire_str[idx]);
    s_npix[idx] = npix;
}

const char *led_out_wire(int idx) { return idx >= 0 && idx < MAX_OUT ? s_wire_str[idx] : ""; }
static uint32_t chan_bytes(int idx) { return idx >= 0 && idx < MAX_OUT ? (uint32_t)s_npix[idx] * s_wire[idx].n : 0; }

#if CONFIG_KTSN_OUTPUT_PARLIO
/* ---------------- PARLIO: every string on its own data line of one parallel DMA transfer ---------------- */
#include <string.h>
#include "driver/parlio_tx.h"
#include "esp_heap_caps.h"
#include "ws_parallel.h"

static struct { int gpio; uint16_t npix; } s_lane[MAX_OUT];
static parlio_tx_unit_handle_t s_unit;
static uint8_t *s_buf;
static size_t   s_bytes;
static unsigned s_width;

esp_err_t led_out_init(int idx, int gpio, uint16_t npix, const char *wire)
{
    if (idx < 0 || idx >= MAX_OUT) return ESP_ERR_INVALID_ARG;
    set_wire(idx, wire, npix);
    s_lane[idx].gpio = gpio;                              /* lane = channel number; the unit is built in led_out_start */
    s_lane[idx].npix = npix;
    return ESP_OK;
}

esp_err_t led_out_start(void)
{
    int lanes = 0;
    size_t maxpix = 0;                                    /* in 3-byte slots: the wire format sets bytes per pixel */
    for (int i = 0; i < MAX_OUT; i++)
        if (s_lane[i].npix) { lanes = i + 1; size_t sl = (chan_bytes(i) + 2) / 3; if (sl > maxpix) maxpix = sl; }
    if (!lanes) return ESP_OK;
    s_width = 1;
    while (s_width < (unsigned)lanes) s_width *= 2;       /* data width must be 1, 2, 4, 8... */
    s_bytes = ws_par_frame_bytes(maxpix, s_width);         /* whole 32-bit words: see ws_parallel.h */
    if (s_bytes > 0xFFFF) {                               /* PARLIO_LL_TX_MAX_BYTES_PER_FRAME on the C6 */
        ESP_LOGE(TAG, "PARLIO: %u 3-byte slots on %u lanes needs %u bytes per frame, max 65535 (fewer pixels or channels)",
                 (unsigned)maxpix, s_width, (unsigned)s_bytes);
        return ESP_ERR_INVALID_SIZE;
    }
    s_buf = heap_caps_calloc(1, s_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_buf) return ESP_ERR_NO_MEM;

    parlio_tx_unit_config_t c = {
        .clk_src = PARLIO_CLK_SRC_DEFAULT,                /* 240 MHz PLL / 72 = exactly 300 ns per sample */
        .clk_in_gpio_num = -1,
        .output_clk_freq_hz = WS_SAMPLE_HZ,
        .data_width = s_width,
        .clk_out_gpio_num = -1,
        .valid_gpio_num = -1,
        .trans_queue_depth = 1,
        .max_transfer_size = s_bytes,
        .shift_edge = PARLIO_SHIFT_EDGE_POS,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,      /* matches ws_par_encode's packing */
    };
    for (int i = 0; i < PARLIO_TX_UNIT_MAX_DATA_WIDTH; i++)
        c.data_gpio_nums[i] = (i < MAX_OUT && s_lane[i].npix) ? (gpio_num_t)s_lane[i].gpio : (gpio_num_t)-1;
    esp_err_t r = parlio_new_tx_unit(&c, &s_unit);
    if (r == ESP_OK) r = parlio_tx_unit_enable(s_unit);
    if (r != ESP_OK) {
        if (s_unit) { parlio_del_tx_unit(s_unit); s_unit = NULL; }
        heap_caps_free(s_buf); s_buf = NULL;
        return r;
    }
    ESP_LOGI(TAG, "PARLIO: %d lane(s), width %u, %u px max, %u-byte frame", lanes, s_width, (unsigned)maxpix, (unsigned)s_bytes);
    return ESP_OK;
}

void led_out_write(int idx, const pixel_t *px, uint16_t n, const out_map_t *m)
{
    if (!s_unit || idx < 0 || idx >= MAX_OUT || !s_lane[idx].npix) return;
    if (n > s_lane[idx].npix) n = s_lane[idx].npix;
    const wire_t *w = &s_wire[idx];
    uint8_t b[WIRE_MAX];
    for (uint16_t i = 0; i < n; i++) {
        wire_encode_mapped(w, m, px[i], i, b);
        ws_par_encode(s_buf, s_width, (unsigned)idx, (size_t)i * w->n, b, w->n);
    }
}

void led_out_set_parallel(bool on) { (void)on; }
uint32_t led_out_send_us(int idx) { return chan_bytes(idx) ? chan_bytes(idx) * 12u + 100u : 0; }   /* 8 bits x 5 x 300 ns */
bool led_out_concurrent(void) { return true; }
const char *led_out_driver(void) { return "parlio"; }

void led_out_flush(void)
{
    if (!s_unit) return;
    parlio_transmit_config_t tc = { .idle_value = 0 };    /* lines rest low between frames: that is the WS281x reset */
    if (parlio_tx_unit_transmit(s_unit, s_buf, s_bytes * 8, &tc) == ESP_OK)
        parlio_tx_unit_wait_all_done(s_unit, 100);
    memset(s_buf, 0, s_bytes);                            /* the encoder only writes ones */
}

#else
/* ---------------- RMT via led_strip: one RMT channel per string ---------------- */
#include "driver/rmt_tx.h"

static led_strip_handle_t s_strip[MAX_OUT];
static bool s_inflight[MAX_OUT];                /* async send started, not yet waited for */
static volatile bool s_parallel;

/* led_strip is used as a plain byte pipe: "pixels" of 3 bytes in RGB order (bytes go out as given), filled with the
 * channel's wire bytes back to back; the last one is padded with zeros, which no chip on the string receives. */
esp_err_t led_out_init(int idx, int gpio, uint16_t npix, const char *wire)
{
    if (idx < 0 || idx >= MAX_OUT) return ESP_ERR_INVALID_ARG;
    set_wire(idx, wire, npix);
    led_strip_config_t sc = {
        .strip_gpio_num = gpio,
        .max_leds = (chan_bytes(idx) + 2) / 3,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
        .led_model = LED_MODEL_WS2812,          /* 800 kHz timing; matches 12V WS2811 modules */
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rc = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = CONFIG_KTSN_RMT_MEM_SYMBOLS,   /* menuconfig KTSN LED; 0 = driver default */
        .flags.with_dma = false,                /* no RMT DMA on ESP32/C3/C6 */
    };
    return led_strip_new_rmt_device(&sc, &rc, &s_strip[idx]);
}

esp_err_t led_out_start(void) { (void)TAG; return ESP_OK; }

void led_out_write(int idx, const pixel_t *px, uint16_t n, const out_map_t *m)
{
    led_strip_handle_t h = s_strip[idx];
    if (!h) return;
    if (n > s_npix[idx]) n = s_npix[idx];
    const wire_t *w = &s_wire[idx];
    uint8_t b[WIRE_MAX], t[3];
    uint32_t slot = 0, k = 0;
    for (uint16_t i = 0; i < n; i++) {
        wire_encode_mapped(w, m, px[i], i, b);
        for (uint8_t j = 0; j < w->n; j++) {
            t[k++] = b[j];
            if (k == 3) { led_strip_set_pixel(h, slot++, t[0], t[1], t[2]); k = 0; }
        }
    }
    if (k) { while (k < 3) t[k++] = 0; led_strip_set_pixel(h, slot, t[0], t[1], t[2]); }
    if (s_parallel && led_strip_refresh_async(h) == ESP_OK) s_inflight[idx] = true;   /* waited for in led_out_flush */
    else led_strip_refresh(h);                  /* blocks until this string is sent */
}

/* Strings started in parallel finish here, whatever the setting is now (it may have changed mid-frame). */
void led_out_flush(void)
{
    for (int i = 0; i < MAX_OUT; i++)
        if (s_inflight[i]) { led_strip_refresh_async_done(s_strip[i]); s_inflight[i] = false; }
}

void led_out_set_parallel(bool on) { s_parallel = on; }
uint32_t led_out_send_us(int idx) { return chan_bytes(idx) ? (chan_bytes(idx) + 2) / 3 * 30u + 300u : 0; }   /* 3 bytes per 30 us + reset */
bool led_out_concurrent(void) { return s_parallel; }
const char *led_out_driver(void) { return "rmt"; }
#endif
