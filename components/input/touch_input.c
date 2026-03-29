/*
 * Touch input backend — GT911 capacitive touch controller
 *
 * Polls GT911 over I2C at ~50ms intervals.
 * Detects tap (touch down + up within 300ms) and long-press (held >= 500ms).
 * Posts INPUT_EVENT_TAP / INPUT_EVENT_LONG_PRESS with x,y coordinates.
 */

#if defined(CONFIG_INPUT_TOUCH) || defined(CONFIG_INPUT_AUTO)

#include "input_hal_internal.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include <string.h>

static const char *TAG = "touch_input";

/* GT911 I2C address (ADDR pin low = 0x5D, high = 0x14; default board = 0x14) */
#define GT911_ADDR          0x14

/* GT911 registers */
#define GT911_STATUS_REG    0x814E
#define GT911_POINT0_REG    0x8150

/* Timing thresholds (ms) */
#define TAP_MAX_MS          300
#define LONG_PRESS_MS       500

/* Poll interval (ms) */
#define POLL_INTERVAL_MS    50

/* Waveshare Touch-LCD-7 IO expander addresses and reset values */
#define IOEXP_ADDR_24       0x24
#define IOEXP_ADDR_38       0x38
#define IOEXP_24_INIT       0x01
#define IOEXP_38_ASSERT     0x2C   /* RST asserted */
#define IOEXP_38_DEASSERT   0x2E   /* RST released */
#define RESET_ASSERT_US     (100 * 1000)
#define RESET_GPIO_LOW_US   (100 * 1000)
#define RESET_HOLD_US       (200 * 1000)

/* Touch state machine states */
typedef enum {
    STATE_IDLE,
    STATE_TOUCHING,
    STATE_WAITING_LIFT,
} touch_state_t;

static i2c_master_dev_handle_t s_gt911_dev = NULL;

/* ------------------------------------------------------------------ */
/* GT911 register helpers                                               */
/* ------------------------------------------------------------------ */

static esp_err_t gt911_read_reg(i2c_master_dev_handle_t dev,
                                uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit_receive(dev, addr, 2, buf, len, 100);
}

static esp_err_t gt911_write_reg(i2c_master_dev_handle_t dev,
                                 uint16_t reg, uint8_t val)
{
    uint8_t buf[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val };
    return i2c_master_transmit(dev, buf, 3, 100);
}

/* ------------------------------------------------------------------ */
/* Poll task                                                            */
/* ------------------------------------------------------------------ */

static void touch_poll_task(void *arg)
{
    touch_state_t state       = STATE_IDLE;
    int64_t       touch_start = 0;
    uint16_t      touch_x     = 0;
    uint16_t      touch_y     = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));

        /* Read status register */
        uint8_t status = 0;
        if (gt911_read_reg(s_gt911_dev, GT911_STATUS_REG, &status, 1) != ESP_OK) {
            continue;
        }

        bool buffer_ready = (status & 0x80) != 0;
        int  npoints      = (int)(status & 0x0F);

        /* Always clear status after reading so GT911 prepares next sample */
        gt911_write_reg(s_gt911_dev, GT911_STATUS_REG, 0x00);

        int64_t now_ms = (int64_t)(esp_timer_get_time() / 1000);

        switch (state) {
        case STATE_IDLE:
            if (buffer_ready && npoints > 0) {
                /* Read first touch point (8 bytes at GT911_POINT0_REG) */
                uint8_t pt[8] = {0};
                if (gt911_read_reg(s_gt911_dev, GT911_POINT0_REG, pt, 8) == ESP_OK) {
                    touch_x     = (uint16_t)(((uint16_t)pt[3] << 8) | pt[2]);
                    touch_y     = (uint16_t)(((uint16_t)pt[5] << 8) | pt[4]);
                    touch_start = now_ms;
                    state       = STATE_TOUCHING;
                    ESP_LOGD(TAG, "touch down x=%u y=%u", touch_x, touch_y);
                }
            }
            break;

        case STATE_TOUCHING: {
            int64_t elapsed = now_ms - touch_start;

            if (elapsed >= LONG_PRESS_MS) {
                /* Long-press threshold reached */
                input_event_t ev = {
                    .type = INPUT_EVENT_LONG_PRESS,
                    .len  = 0,
                    .x    = touch_x,
                    .y    = touch_y,
                };
                ESP_LOGD(TAG, "long-press x=%u y=%u", touch_x, touch_y);
                input_hal_post_event(&ev);
                state = STATE_WAITING_LIFT;
            } else if (buffer_ready && npoints == 0 && elapsed < TAP_MAX_MS) {
                /* Finger lifted within tap window (< 300ms) */
                input_event_t ev = {
                    .type = INPUT_EVENT_TAP,
                    .len  = 0,
                    .x    = touch_x,
                    .y    = touch_y,
                };
                ESP_LOGD(TAG, "tap x=%u y=%u elapsed=%lldms", touch_x, touch_y, elapsed);
                input_hal_post_event(&ev);
                state = STATE_IDLE;
            } else if (buffer_ready && npoints == 0) {
                /* Lift between 300ms and 500ms — no event, return to idle */
                ESP_LOGD(TAG, "lift in dead zone (%lldms), no event", elapsed);
                state = STATE_IDLE;
            } else if (!buffer_ready) {
                /*
                 * No new data from GT911 yet — could mean finger is still
                 * down (no fresh sample) or was lifted between polls.
                 * Only treat as lift if enough silence has passed beyond tap
                 * window with no active touch.  Continue waiting.
                 */
                (void)elapsed;
            }
            break;
        }

        case STATE_WAITING_LIFT:
            /* Wait until GT911 reports zero touch points */
            if (buffer_ready && npoints == 0) {
                ESP_LOGD(TAG, "finger lifted after long-press");
                state = STATE_IDLE;
            }
            break;

        default:
            state = STATE_IDLE;
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Waveshare hardware reset sequence                                    */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_INPUT_TOUCH_WAVESHARE_RESET
static esp_err_t gt911_waveshare_reset(i2c_master_bus_handle_t bus)
{
    esp_err_t ret;

    /* Configure GPIO as output, drive HIGH before sequence */
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_INPUT_TOUCH_RESET_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&io_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    gpio_set_level(CONFIG_INPUT_TOUCH_RESET_GPIO, 1);

    /* IO expander 0x24 — write 0x01 */
    i2c_master_dev_handle_t iox24 = NULL;
    i2c_device_config_t cfg24 = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = IOEXP_ADDR_24,
        .scl_speed_hz    = 400000,
    };
    ret = i2c_master_bus_add_device(bus, &cfg24, &iox24);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add ioexp 0x24 failed: %s", esp_err_to_name(ret));
        return ret;
    }
    uint8_t cmd = IOEXP_24_INIT;
    ret = i2c_master_transmit(iox24, &cmd, 1, 100);
    i2c_master_bus_rm_device(iox24);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ioexp 0x24 write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* IO expander 0x38 — assert reset (0x2C) */
    i2c_master_dev_handle_t iox38 = NULL;
    i2c_device_config_t cfg38 = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = IOEXP_ADDR_38,
        .scl_speed_hz    = 400000,
    };
    ret = i2c_master_bus_add_device(bus, &cfg38, &iox38);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add ioexp 0x38 failed: %s", esp_err_to_name(ret));
        return ret;
    }
    cmd = IOEXP_38_ASSERT;
    ret = i2c_master_transmit(iox38, &cmd, 1, 100);
    if (ret != ESP_OK) {
        i2c_master_bus_rm_device(iox38);
        ESP_LOGE(TAG, "ioexp 0x38 assert failed: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_rom_delay_us(RESET_ASSERT_US);

    /* GPIO LOW */
    gpio_set_level(CONFIG_INPUT_TOUCH_RESET_GPIO, 0);
    esp_rom_delay_us(RESET_GPIO_LOW_US);

    /* IO expander 0x38 — deassert reset (0x2E) */
    cmd = IOEXP_38_DEASSERT;
    ret = i2c_master_transmit(iox38, &cmd, 1, 100);
    i2c_master_bus_rm_device(iox38);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ioexp 0x38 deassert failed: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_rom_delay_us(RESET_HOLD_US);

    ESP_LOGI(TAG, "GT911 hardware reset complete");
    return ESP_OK;
}
#endif /* CONFIG_INPUT_TOUCH_WAVESHARE_RESET */

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */

esp_err_t touch_input_backend_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port            = CONFIG_INPUT_TOUCH_I2C_PORT,
        .sda_io_num          = CONFIG_INPUT_TOUCH_SDA_PIN,
        .scl_io_num          = CONFIG_INPUT_TOUCH_SCL_PIN,
        .clk_source          = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt   = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus_handle = NULL;
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
        return ret;
    }

#ifdef CONFIG_INPUT_TOUCH_WAVESHARE_RESET
    ret = gt911_waveshare_reset(bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GT911 reset failed: %s", esp_err_to_name(ret));
        i2c_del_master_bus(bus_handle);
        return ret;
    }
#endif

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = GT911_ADDR,
        .scl_speed_hz    = 400000,
    };

    ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &s_gt911_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(ret));
        i2c_del_master_bus(bus_handle);
        return ret;
    }

    BaseType_t xret = xTaskCreatePinnedToCore(
        touch_poll_task,
        "touch_poll",
        3072,
        NULL,
        4,
        NULL,
        0   /* core 0 */
    );
    if (xret != pdPASS) {
        ESP_LOGE(TAG, "failed to create touch_poll_task");
        i2c_master_bus_rm_device(s_gt911_dev);
        i2c_del_master_bus(bus_handle);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "GT911 touch backend initialised (I2C port %d, SDA=%d, SCL=%d)",
             CONFIG_INPUT_TOUCH_I2C_PORT,
             CONFIG_INPUT_TOUCH_SDA_PIN,
             CONFIG_INPUT_TOUCH_SCL_PIN);
    return ESP_OK;
}

#endif /* CONFIG_INPUT_TOUCH || CONFIG_INPUT_AUTO */

/* ------------------------------------------------------------------ */
/* No-op stub when touch is disabled                                    */
/* ------------------------------------------------------------------ */

#if !defined(CONFIG_INPUT_TOUCH) && !defined(CONFIG_INPUT_AUTO)
#include "esp_err.h"
esp_err_t touch_input_backend_init(void) { return ESP_OK; }
#endif
