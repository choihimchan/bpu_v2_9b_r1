#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// FreeRTOS task primitives
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ESP-IDF UART and timing
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_timer.h"

// Pull BPU declarations without compiling implementation
#define BPU_ESPIDF_DECLARE_ONLY 1
#include "bpu_espidf.c"
#undef BPU_ESPIDF_DECLARE_ONLY

// Example debug switch
#ifndef BPU_EXAMPLE_DEBUG
#define BPU_EXAMPLE_DEBUG 0
#endif

// Example-local return codes
typedef enum { EX_RC_OK = 0, EX_RC_ERR = 1 } ExRc;

// UART TX context for BPU IO callbacks
typedef struct {
    uart_port_t uart;
    uint16_t min_free;
    uint16_t chunk_max;
} UartOutCtx;

// UART port selection
static const uart_port_t LOG_UART = UART_NUM_0;
static const uart_port_t OUT_UART = UART_NUM_1;

// UART baud rates
static const int LOG_BAUD = 115200;
static const int OUT_BAUD = 921600;

// UART pin mapping
static const int OUT_TX_PIN = 17;
static const int OUT_RX_PIN = 16;

// Timing periods
static const uint32_t TICK_MS = 20;

static const uint32_t SENSOR_MS = 80;
static const uint32_t HB_MS = 200;
static const uint32_t TELEM_MS = 1000;

// TX pacing and backpressure thresholds
static const uint16_t TX_BUDGET_BYTES = 200;
static const uint16_t OUT_MIN_FREE = 96;
static const uint16_t TX_CHUNK_MAX = 128;

// Coalescing/aging thresholds
static const uint16_t COALESCE_WINDOW_MS = 20;
static const uint16_t AGED_MS = 200;

// UART driver buffer sizes
static const int LOG_RX_BUF = 256;
static const int LOG_TX_BUF = 512;

static const int OUT_RX_BUF = 256;
static const int OUT_TX_BUF = 2048;

// Static task storage (no heap)
static StaticTask_t g_task_tcb;
static StackType_t g_task_stack[4096 / sizeof(StackType_t)];

// Logging helpers
static int log_write(const uint8_t *p, size_t n);
static int log_str(const char *s);
static int log_nl(void);
static int log_u32_dec(uint32_t v);
static int log_u32_hex(uint32_t v);

// BPU IO callbacks for output UART
static int out_tx_free(void *ctx, size_t *free_out);
static int out_tx_write_some(void *ctx, const uint8_t *p, size_t len, size_t *wrote_out);
static int out_time_us(void *ctx, uint32_t *us_out);

// UART initialization
static int uart_init_ports(void);
// Demo task running BPU tick loop
static void bpu_demo_task(void *arg);

// Write raw bytes to log UART
static int log_write(const uint8_t *p, size_t n)
{
    int rc;
    int w;

    rc = EX_RC_OK;

    if (p == NULL) {
        rc = EX_RC_ERR;
    } else {
        if (n != 0U) {
            w = uart_write_bytes(LOG_UART, (const char *)p, n);
            if (w < 0) {
                rc = EX_RC_ERR;
            }
        }
    }

    return rc;
}

// Write zero-terminated string
static int log_str(const char *s)
{
    int rc;
    size_t i;

    rc = EX_RC_OK;

    if (s == NULL) {
        rc = EX_RC_ERR;
    } else {
        i = 0U;
        while (s[i] != 0) {
            (void)log_write((const uint8_t *)&s[i], 1U);
            i++;
        }
    }

    return rc;
}

// Write newline
static int log_nl(void)
{
    int rc;
    uint8_t nl;

    rc = EX_RC_OK;
    nl = (uint8_t)'\n';

    if (log_write(&nl, 1U) != EX_RC_OK) {
        rc = EX_RC_ERR;
    }

    return rc;
}

// Write u32 as decimal
static int log_u32_dec(uint32_t v)
{
    int rc;
    char buf[11];
    uint32_t x;
    size_t i;
    size_t j;

    rc = EX_RC_OK;
    x = v;
    i = 0U;

    if (x == 0U) {
        buf[i] = '0';
        i++;
    } else {
        while (x != 0U && i < sizeof(buf)) {
            buf[i] = (char)('0' + (char)(x % 10U));
            i++;
            x /= 10U;
        }
    }

    j = 0U;
    while (j < i / 2U) {
        char t;

        t = buf[j];
        buf[j] = buf[i - 1U - j];
        buf[i - 1U - j] = t;

        j++;
    }

    j = 0U;
    while (j < i) {
        (void)log_write((const uint8_t *)&buf[j], 1U);
        j++;
    }

    return rc;
}

// Write u32 as hex
static int log_u32_hex(uint32_t v)
{
    int rc;
    uint32_t x;
    uint32_t i;

    rc = EX_RC_OK;
    x = v;
    i = 0U;

    (void)log_str("0x");

    while (i < 8U) {
        uint32_t shift;
        uint32_t nib;
        uint8_t c;

        shift = 28U - (i * 4U);
        nib = (x >> shift) & 0x0FU;

        if (nib < 10U) {
            c = (uint8_t)('0' + (uint8_t)nib);
        } else {
            c = (uint8_t)('A' + (uint8_t)(nib - 10U));
        }

        (void)log_write(&c, 1U);
        i++;
    }

    return rc;
}

// Query free space for TX backpressure
static int out_tx_free(void *ctx, size_t *free_out)
{
    int rc;
    esp_err_t err;
    size_t f;
    UartOutCtx *c;

    rc = BPU_RC_OK;

    if (ctx == NULL) {
        rc = BPU_RC_ERR;
    } else {
        if (free_out == NULL) {
            rc = BPU_RC_ERR;
        } else {
            c = (UartOutCtx *)ctx;

            f = 0U;
            err = uart_get_tx_buffer_free_size(c->uart, &f);
            if (err != ESP_OK) {
                rc = BPU_RC_ERR;
            } else {
                *free_out = f;
            }
        }
    }

    return rc;
}

// Write as much as UART accepts now
static int out_tx_write_some(void *ctx, const uint8_t *p, size_t len, size_t *wrote_out)
{
    int rc;
    UartOutCtx *c;
    size_t free_sz;
    size_t usable;
    size_t want;
    int w;

    rc = BPU_RC_OK;

    if (wrote_out != NULL) {
        *wrote_out = 0U;
    }

    if (ctx == NULL) {
        rc = BPU_RC_ERR;
    } else {
        if (p == NULL) {
            rc = BPU_RC_ERR;
        } else {
            if (wrote_out == NULL) {
                rc = BPU_RC_ERR;
            } else {
                c = (UartOutCtx *)ctx;

                free_sz = 0U;
                if (out_tx_free(ctx, &free_sz) != BPU_RC_OK) {
                    rc = BPU_RC_ERR;
                } else {
                    if (free_sz <= (size_t)c->min_free) {
                        *wrote_out = 0U;
                        rc = BPU_RC_OK;
                    } else {
                        usable = free_sz - (size_t)c->min_free;

                        want = len;
                        if (want > usable) {
                            want = usable;
                        }

                        if (c->chunk_max != 0U) {
                            if (want > (size_t)c->chunk_max) {
                                want = (size_t)c->chunk_max;
                            }
                        }

                        if (want == 0U) {
                            *wrote_out = 0U;
                            rc = BPU_RC_OK;
                        } else {
                            w = uart_write_bytes(c->uart, (const char *)p, want);
                            if (w < 0) {
                                rc = BPU_RC_ERR;
                            } else {
                                if ((size_t)w > want) {
                                    w = (int)want;
                                }
                                *wrote_out = (size_t)w;
                                rc = BPU_RC_OK;
                            }
                        }
                    }
                }
            }
        }
    }

    return rc;
}

// Provide time source for profiling
static int out_time_us(void *ctx, uint32_t *us_out)
{
    int rc;
    uint64_t t;

    rc = BPU_RC_OK;
    (void)ctx;

    if (us_out == NULL) {
        rc = BPU_RC_ERR;
    } else {
        t = (uint64_t)esp_timer_get_time();
        *us_out = (uint32_t)(t & 0xFFFFFFFFULL);
    }

    return rc;
}

// Configure and install UART drivers
static int uart_init_ports(void)
{
    int rc;
    esp_err_t err;
    uart_config_t cfg;

    rc = EX_RC_OK;

    cfg.baud_rate = LOG_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
#if defined(UART_SCLK_DEFAULT)
    cfg.source_clk = UART_SCLK_DEFAULT;
#endif

    err = uart_param_config(LOG_UART, &cfg);
    if (err != ESP_OK) {
        rc = EX_RC_ERR;
    } else {
        err = uart_driver_install(LOG_UART, LOG_RX_BUF, LOG_TX_BUF, 0, NULL, 0);
        if (err != ESP_OK) {
            rc = EX_RC_ERR;
        } else {
            cfg.baud_rate = OUT_BAUD;

            err = uart_param_config(OUT_UART, &cfg);
            if (err != ESP_OK) {
                rc = EX_RC_ERR;
            } else {
                err = uart_set_pin(OUT_UART, OUT_TX_PIN, OUT_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
                if (err != ESP_OK) {
                    rc = EX_RC_ERR;
                } else {
                    err = uart_driver_install(OUT_UART, OUT_RX_BUF, OUT_TX_BUF, 0, NULL, 0);
                    if (err != ESP_OK) {
                        rc = EX_RC_ERR;
                    }
                }
            }
        }
    }

    return rc;
}

// Periodically push events and call bpu_tick
static void bpu_demo_task(void *arg)
{
    Bpu bpu;
    BpuIo io;
    BpuConfig cfg;
    UartOutCtx out_ctx;

    uint32_t next_sensor;
    uint32_t next_hb;
    uint32_t next_telem;

    TickType_t last_wake;
    TickType_t period_ticks;

    (void)arg;

    out_ctx.uart = OUT_UART;
    out_ctx.min_free = OUT_MIN_FREE;
    out_ctx.chunk_max = TX_CHUNK_MAX;

    io.ctx = &out_ctx;
    io.tx_free = out_tx_free;
    io.tx_write_some = out_tx_write_some;
    io.time_us = out_time_us;

    cfg.tx_budget_bytes = TX_BUDGET_BYTES;
    cfg.tx_min_free = OUT_MIN_FREE;
    cfg.tx_chunk_max = TX_CHUNK_MAX;
    cfg.coalesce_window_ms = COALESCE_WINDOW_MS;
    cfg.aged_ms = AGED_MS;
    cfg.enable_degrade = 1U;

    (void)bpu_init(&bpu, &io, &cfg);

    next_sensor = 10U;
    next_hb = 50U;
    next_telem = 200U;

    last_wake = xTaskGetTickCount();
    period_ticks = pdMS_TO_TICKS(TICK_MS);

    while (1) {
        uint32_t now_ms;

        now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

        if ((int32_t)(now_ms - next_sensor) >= 0) {
            uint8_t payload[2];
            uint16_t v;

            next_sensor = now_ms + SENSOR_MS;
            v = (uint16_t)((now_ms / 10U) & 0xFFFFU);

            payload[0] = (uint8_t)(v & 0xFFU);
            payload[1] = (uint8_t)((v >> 8) & 0xFFU);

            (void)bpu_push_event(&bpu, BPU_EVT_SENSOR, payload, 2U, now_ms);
        }

        if ((int32_t)(now_ms - next_hb) >= 0) {
            uint8_t payload[1];

            next_hb = now_ms + HB_MS;
            payload[0] = 0x01U;

            (void)bpu_push_event(&bpu, BPU_EVT_HB, payload, 1U, now_ms);
        }

        if ((int32_t)(now_ms - next_telem) >= 0) {
            uint8_t payload[4];

            next_telem = now_ms + TELEM_MS;

            payload[0] = (uint8_t)(now_ms & 0xFFU);
            payload[1] = (uint8_t)((now_ms >> 8) & 0xFFU);
            payload[2] = (uint8_t)((now_ms >> 16) & 0xFFU);
            payload[3] = (uint8_t)((now_ms >> 24) & 0xFFU);

            (void)bpu_push_event(&bpu, BPU_EVT_TELEM, payload, 4U, now_ms);
        }

        (void)bpu_tick(&bpu, now_ms);

        vTaskDelayUntil(&last_wake, period_ticks);
    }
}

// ESP-IDF entry point
void app_main(void)
{
    int rc;
    TaskHandle_t h;

    rc = uart_init_ports();

    if (rc == EX_RC_OK) {
        h = xTaskCreateStatic(
            bpu_demo_task,
            "bpu_demo",
            (uint32_t)(sizeof(g_task_stack) / sizeof(StackType_t)),
            NULL,
            tskIDLE_PRIORITY + 1,
            g_task_stack,
            &g_task_tcb
        );
        (void)h;
    }
}
