#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Include guard
#ifndef BPU_ESPIDF_C_API_INCLUDED
#define BPU_ESPIDF_C_API_INCLUDED 1

// Return codes
typedef enum { BPU_RC_OK = 0, BPU_RC_ERR = 1 } BpuRc;

// Event kinds produced by producers
typedef enum { BPU_EVT_CMD = 1, BPU_EVT_SENSOR = 2, BPU_EVT_HB = 3, BPU_EVT_TELEM = 4 } BpuEvtType;
// Job kinds consumed by worker logic
typedef enum { BPU_JOB_CMD = 1, BPU_JOB_SENSOR = 2, BPU_JOB_HB = 3, BPU_JOB_TELEM = 4 } BpuJobType;

// Merge policy for queueing
typedef enum { BPU_MERGE_NONE = 0, BPU_MERGE_LAST = 1 } BpuMergePolicy;

// Event record (fixed payload)
typedef struct {
    uint8_t type;
    uint8_t flags;
    uint16_t len;
    uint32_t t_ms;
    uint8_t payload[16];
} BpuEvent;

// Job record (fixed payload)
typedef struct {
    uint8_t type;
    uint8_t flags;
    uint16_t len;
    uint32_t t_ms;
    uint8_t payload[32];
} BpuJob;

// Debug/telemetry counters
typedef struct {
    uint32_t tick;
    uint32_t ev_in;
    uint32_t ev_out;
    uint32_t ev_merge;
    uint32_t ev_drop;
    uint32_t job_in;
    uint32_t job_out;
    uint32_t job_merge;
    uint32_t job_drop;
    uint32_t tx_frame_sent;
    uint32_t tx_frame_partial;
    uint32_t tx_bytes;
    uint32_t tx_skip_budget;
    uint32_t tx_skip_backpressure;
    uint32_t flush_try;
    uint32_t flush_ok;
    uint32_t pick_sensor;
    uint32_t pick_hb;
    uint32_t pick_telem;
    uint32_t pick_aged;
    uint32_t aged_hit_sensor;
    uint32_t aged_hit_hb;
    uint32_t aged_hit_telem;
    uint32_t degrade_drop;
    uint32_t degrade_requeue;
    uint32_t pending_active;
    uint32_t pending_len;
    uint32_t pending_pos;
    uint32_t dirty_mask_lo;
    uint32_t dirty_mask_hi;
    uint32_t work_us_last;
    uint32_t work_us_max;
} BpuStats;

// IO callbacks provided by platform
typedef struct {
    void *ctx;
    int (*tx_free)(void *ctx, size_t *free_out);
    int (*tx_write_some)(void *ctx, const uint8_t *p, size_t len, size_t *wrote_out);
    int (*time_us)(void *ctx, uint32_t *us_out);
} BpuIo;

// Runtime configuration knobs
typedef struct {
    uint16_t tx_budget_bytes;
    uint16_t tx_min_free;
    uint16_t tx_chunk_max;
    uint16_t coalesce_window_ms;
    uint16_t aged_ms;
    uint8_t enable_degrade;
} BpuConfig;

// Small ring buffer for events
typedef struct {
    BpuEvent buf[8];
    uint16_t head;
    uint16_t tail;
    uint16_t count;
} BpuEvRing;

// Small ring buffer for jobs
typedef struct {
    BpuJob buf[4];
    uint16_t head;
    uint16_t tail;
    uint16_t count;
} BpuJobRing;

// Main BPU state (no heap)
typedef struct {
    BpuIo io;
    BpuConfig cfg;
    BpuStats st;
    BpuEvRing evq;
    BpuJobRing jobq;
    uint8_t pending_buf[4 + 64 + 2 + 16 + 1];
    uint16_t pending_len;
    uint16_t pending_pos;
    uint8_t pending_have;
    uint8_t seq;
    uint32_t init_magic;
} Bpu;

// Public API
int bpu_init(Bpu *bpu, const BpuIo *io, const BpuConfig *cfg);
int bpu_push_event(Bpu *bpu, uint8_t evt_type, const uint8_t *payload, uint16_t len, uint32_t now_ms);
int bpu_tick(Bpu *bpu, uint32_t now_ms);
int bpu_tick_ex(Bpu *bpu, uint32_t now_ms, uint32_t now_us);
int bpu_get_stats(const Bpu *bpu, BpuStats *out);

// End of public header section
#endif

// Implementation section (compiled unless DECLARE_ONLY)
#if !defined(BPU_ESPIDF_DECLARE_ONLY)

// CRC16-CCITT for framing
static uint16_t bpu_crc16_ccitt(const uint8_t *data, size_t len);
static size_t bpu_cobs_encode(const uint8_t *input, size_t length, uint8_t *output, size_t out_max);

// Internal helper declarations
static BpuMergePolicy bpu_policy_for(uint8_t type);
static uint8_t bpu_job_for_evt(uint8_t evt_type);

// Event ring helpers
static int bpu_evr_push(BpuEvRing *r, const BpuEvent *v);
static int bpu_evr_pop(BpuEvRing *r, BpuEvent *out);
static BpuEvent *bpu_evr_at(BpuEvRing *r, uint16_t i);

// Job ring helpers
static int bpu_jor_push(BpuJobRing *r, const BpuJob *v);
static int bpu_jor_pop(BpuJobRing *r, BpuJob *out);
static BpuJob *bpu_jor_at(BpuJobRing *r, uint16_t i);

// Coalescing queue helpers
static int bpu_evq_push_coalesce(Bpu *bpu, const BpuEvent *e);
static int bpu_evq_pop(Bpu *bpu, BpuEvent *out);

static int bpu_jobq_push_coalesce(Bpu *bpu, const BpuJob *j);
static int bpu_jobq_pop(Bpu *bpu, BpuJob *out);

// Dirty-bit tracking
static uint64_t bpu_bit64(uint8_t n);
static uint64_t bpu_dirty_mask(const Bpu *bpu);

// Framing and TX helpers
static int bpu_build_frame(Bpu *bpu, uint8_t type, const uint8_t *payload, uint8_t len);
static int bpu_send_pending(Bpu *bpu, uint16_t *budget_left, bool *progress_out);
static int bpu_schedule_from_events(Bpu *bpu, uint32_t now_ms);
static int bpu_flush_jobs(Bpu *bpu, uint32_t now_ms, uint16_t *budget_left);

// Timing helpers
static int bpu_try_time_us(Bpu *bpu, uint32_t *us_out);

// Compute CRC16 over raw bytes
static uint16_t bpu_crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc;
    size_t i;

    crc = 0xFFFFU;
    i = 0U;

    while (i < len) {
        int b;

        crc ^= (uint16_t)data[i] << 8;

        b = 0;
        while (b < 8) {
            if ((crc & 0x8000U) != 0U) {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            } else {
                crc = (uint16_t)(crc << 1);
            }
            b++;
        }

        i++;
    }

    return crc;
}

// Encode payload using COBS
static size_t bpu_cobs_encode(const uint8_t *input, size_t length, uint8_t *output, size_t out_max)
{
    size_t out_len;
    size_t read_index;
    size_t write_index;
    size_t code_index;
    uint8_t code;
    int rc;

    out_len = 0U;
    rc = BPU_RC_OK;

    if (input == NULL) {
        rc = BPU_RC_ERR;
    } else {
        if (output == NULL) {
            rc = BPU_RC_ERR;
        } else {
            if (out_max == 0U) {
                rc = BPU_RC_ERR;
            }
        }
    }

    if (rc == BPU_RC_OK) {
        read_index = 0U;
        write_index = 1U;
        code_index = 0U;
        code = 1U;

        while (read_index < length && rc == BPU_RC_OK) {
            if (write_index >= out_max) {
                rc = BPU_RC_ERR;
            } else {
                if (input[read_index] == 0U) {
                    output[code_index] = code;
                    code = 1U;
                    code_index = write_index;
                    write_index++;
                    read_index++;
                } else {
                    output[write_index] = input[read_index];
                    write_index++;
                    read_index++;
                    code++;
                    if (code == 0xFFU) {
                        if (write_index >= out_max) {
                            rc = BPU_RC_ERR;
                        } else {
                            output[code_index] = code;
                            code = 1U;
                            code_index = write_index;
                            write_index++;
                        }
                    }
                }
            }
        }

        if (rc == BPU_RC_OK) {
            if (code_index >= out_max) {
                rc = BPU_RC_ERR;
            } else {
                output[code_index] = code;
                out_len = write_index;
            }
        }
    }

    return out_len;
}

static BpuMergePolicy bpu_policy_for(uint8_t type)
{
    BpuMergePolicy p;

    p = BPU_MERGE_NONE;

    if (type == BPU_EVT_SENSOR) {
        p = BPU_MERGE_LAST;
    } else {
        if (type == BPU_EVT_HB) {
            p = BPU_MERGE_LAST;
        } else {
            if (type == BPU_EVT_TELEM) {
                p = BPU_MERGE_LAST;
            } else {
                p = BPU_MERGE_NONE;
            }
        }
    }

    return p;
}

// Map event type to job type
static uint8_t bpu_job_for_evt(uint8_t evt_type)
{
    uint8_t j;

    j = 0U;

    if (evt_type == BPU_EVT_CMD) {
        j = BPU_JOB_CMD;
    } else {
        if (evt_type == BPU_EVT_SENSOR) {
            j = BPU_JOB_SENSOR;
        } else {
            if (evt_type == BPU_EVT_HB) {
                j = BPU_JOB_HB;
            } else {
                if (evt_type == BPU_EVT_TELEM) {
                    j = BPU_JOB_TELEM;
                } else {
                    j = 0U;
                }
            }
        }
    }

    return j;
}

// Push event into ring buffer
static int bpu_evr_push(BpuEvRing *r, const BpuEvent *v)
{
    int rc;

    rc = BPU_RC_OK;

    if (r == NULL) {
        rc = BPU_RC_ERR;
    } else {
        if (v == NULL) {
            rc = BPU_RC_ERR;
        } else {
            if (r->count >= 8U) {
                rc = BPU_RC_ERR;
            } else {
                r->buf[r->head] = *v;
                r->head = (uint16_t)((r->head + 1U) % 8U);
                r->count++;
            }
        }
    }

    return rc;
}

// Pop event from ring buffer
static int bpu_evr_pop(BpuEvRing *r, BpuEvent *out)
{
    int rc;

    rc = BPU_RC_OK;

    if (r == NULL) {
        rc = BPU_RC_ERR;
    } else {
        if (out == NULL) {
            rc = BPU_RC_ERR;
        } else {
            if (r->count == 0U) {
                rc = BPU_RC_ERR;
            } else {
                *out = r->buf[r->tail];
                r->tail = (uint16_t)((r->tail + 1U) % 8U);
                r->count--;
            }
        }
    }

    return rc;
}

static BpuEvent *bpu_evr_at(BpuEvRing *r, uint16_t i)
{
    BpuEvent *p;
    uint16_t idx;

    p = NULL;

    if (r != NULL) {
        idx = (uint16_t)((r->tail + i) % 8U);
        p = &r->buf[idx];
    }

    return p;
}

// Push job into ring buffer
static int bpu_jor_push(BpuJobRing *r, const BpuJob *v)
{
    int rc;

    rc = BPU_RC_OK;

    if (r == NULL) {
        rc = BPU_RC_ERR;
    } else {
        if (v == NULL) {
            rc = BPU_RC_ERR;
        } else {
            if (r->count >= 4U) {
                rc = BPU_RC_ERR;
            } else {
                r->buf[r->head] = *v;
                r->head = (uint16_t)((r->head + 1U) % 4U);
                r->count++;
            }
        }
    }

    return rc;
}

// Pop job from ring buffer
static int bpu_jor_pop(BpuJobRing *r, BpuJob *out)
{
    int rc;

    rc = BPU_RC_OK;

    if (r == NULL) {
        rc = BPU_RC_ERR;
    } else {
        if (out == NULL) {
            rc = BPU_RC_ERR;
        } else {
            if (r->count == 0U) {
                rc = BPU_RC_ERR;
            } else {
                *out = r->buf[r->tail];
                r->tail = (uint16_t)((r->tail + 1U) % 4U);
                r->count--;
            }
        }
    }

    return rc;
}

static BpuJob *bpu_jor_at(BpuJobRing *r, uint16_t i)
{
    BpuJob *p;
    uint16_t idx;

    p = NULL;

    if (r != NULL) {
        idx = (uint16_t)((r->tail + i) % 4U);
        p = &r->buf[idx];
    }

    return p;
}

// Push event with optional coalescing
static int bpu_evq_push_coalesce(Bpu *bpu, const BpuEvent *e)
{
    int rc;

    rc = BPU_RC_OK;

    if (bpu == NULL) {
        rc = BPU_RC_ERR;
    } else {
        if (e == NULL) {
            rc = BPU_RC_ERR;
        } else {
            bpu->st.ev_in++;

            if (bpu->cfg.coalesce_window_ms > 0U && bpu_policy_for(e->type) == BPU_MERGE_LAST && bpu->evq.count != 0U) {
                uint16_t i;
                bool merged;

                i = 0U;
                merged = false;

                while (i < bpu->evq.count) {
                    BpuEvent *ex;

                    ex = bpu_evr_at(&bpu->evq, i);

                    if (ex != NULL) {
                        if (ex->type == e->type) {
                            if ((uint32_t)(e->t_ms - ex->t_ms) <= (uint32_t)bpu->cfg.coalesce_window_ms) {
                                *ex = *e;
                                bpu->st.ev_merge++;
                                merged = true;
                            }
                        }
                    }

                    i++;
                }

                if (!merged) {
                    if (bpu_evr_push(&bpu->evq, e) != BPU_RC_OK) {
                        bpu->st.ev_drop++;
                        rc = BPU_RC_ERR;
                    }
                }
            } else {
                if (bpu_evr_push(&bpu->evq, e) != BPU_RC_OK) {
                    bpu->st.ev_drop++;
                    rc = BPU_RC_ERR;
                }
            }
        }
    }

    return rc;
}

// Pop next event respecting policy
static int bpu_evq_pop(Bpu *bpu, BpuEvent *out)
{
    int rc;

    rc = BPU_RC_OK;

    if (bpu == NULL) {
        rc = BPU_RC_ERR;
    } else {
        if (bpu_evr_pop(&bpu->evq, out) != BPU_RC_OK) {
            rc = BPU_RC_ERR;
        } else {
            bpu->st.ev_out++;
        }
    }

    return rc;
}

// Push job with optional coalescing
static int bpu_jobq_push_coalesce(Bpu *bpu, const BpuJob *j)
{
    int rc;

    rc = BPU_RC_OK;

    if (bpu == NULL) {
        rc = BPU_RC_ERR;
    } else {
        if (j == NULL) {
            rc = BPU_RC_ERR;
        } else {
            bpu->st.job_in++;

            if (bpu->jobq.count != 0U) {
                uint16_t i;
                bool merged;

                i = 0U;
                merged = false;

                while (i < bpu->jobq.count) {
                    BpuJob *ex;

                    ex = bpu_jor_at(&bpu->jobq, i);

                    if (ex != NULL) {
                        if (ex->type == j->type) {
                            *ex = *j;
                            bpu->st.job_merge++;
                            merged = true;
                        }
                    }

                    i++;
                }

                if (!merged) {
                    if (bpu_jor_push(&bpu->jobq, j) != BPU_RC_OK) {
                        bpu->st.job_drop++;
                        rc = BPU_RC_ERR;
                    }
                }
            } else {
                if (bpu_jor_push(&bpu->jobq, j) != BPU_RC_OK) {
                    bpu->st.job_drop++;
                    rc = BPU_RC_ERR;
                }
            }
        }
    }

    return rc;
}

// Pop next job respecting policy
static int bpu_jobq_pop(Bpu *bpu, BpuJob *out)
{
    int rc;

    rc = BPU_RC_OK;

    if (bpu == NULL) {
        rc = BPU_RC_ERR;
    } else {
        if (bpu_jor_pop(&bpu->jobq, out) != BPU_RC_OK) {
            rc = BPU_RC_ERR;
        } else {
            bpu->st.job_out++;
        }
    }

    return rc;
}

// Build 64-bit bit mask
static uint64_t bpu_bit64(uint8_t n)
{
    uint64_t r;

    r = 0ULL;

    if (n < 64U) {
        r = 1ULL << n;
    }

    return r;
}

// Current dirty/coalesce bitmap
static uint64_t bpu_dirty_mask(const Bpu *bpu)
{
    uint64_t m;
    uint16_t i;

    m = 0ULL;

    if (bpu != NULL) {
        i = 0U;
        while (i < bpu->jobq.count) {
            uint16_t idx;
            const BpuJob *j;

            idx = (uint16_t)((bpu->jobq.tail + i) % 4U);
            j = &bpu->jobq.buf[idx];

            if (j->type >= 1U && j->type <= 63U) {
                m |= bpu_bit64(j->type);
            }

            i++;
        }

        if (bpu->pending_have != 0U) {
            m |= bpu_bit64(63U);
        }
    }

    return m;
}

// Read microsecond clock if available
static int bpu_try_time_us(Bpu *bpu, uint32_t *us_out)
{
    int rc;

    rc = BPU_RC_ERR;

    if (us_out != NULL) {
        *us_out = 0U;
    }

    if (bpu != NULL && us_out != NULL) {
        if (bpu->io.time_us != NULL) {
            if (bpu->io.time_us(bpu->io.ctx, us_out) == BPU_RC_OK) {
                rc = BPU_RC_OK;
            }
        }
    }

    return rc;
}

// Build framed packet into pending buffer
static int bpu_build_frame(Bpu *bpu, uint8_t type, const uint8_t *payload, uint8_t len)
{
    int rc;
    uint8_t decoded[4 + 64 + 2];
    size_t decoded_len;
    size_t enc_len;
    uint16_t crc;
    uint8_t i;

    rc = BPU_RC_OK;

    if (bpu == NULL) {
        rc = BPU_RC_ERR;
    } else {
        if (payload == NULL) {
            rc = BPU_RC_ERR;
        }
    }

    if (rc == BPU_RC_OK) {
        if (len > 64U) {
            len = 64U;
        }

        decoded[0] = 0xB2U;
        decoded[1] = type;
        decoded[2] = bpu->seq;
        decoded[3] = len;

        bpu->seq++;

        i = 0U;
        while (i < len) {
            decoded[4U + i] = payload[i];
            i++;
        }

        crc = bpu_crc16_ccitt(&decoded[1], (size_t)(3U + (uint32_t)len));
        decoded[4U + len + 0U] = (uint8_t)(crc & 0xFFU);
        decoded[4U + len + 1U] = (uint8_t)((crc >> 8) & 0xFFU);

        decoded_len = (size_t)(4U + (uint32_t)len + 2U);

        enc_len = bpu_cobs_encode(decoded, decoded_len, bpu->pending_buf, sizeof(bpu->pending_buf));
        if (enc_len == 0U) {
            rc = BPU_RC_ERR;
        } else {
            if (enc_len + 1U > sizeof(bpu->pending_buf)) {
                rc = BPU_RC_ERR;
            } else {
                bpu->pending_buf[enc_len] = 0x00U;
                bpu->pending_len = (uint16_t)(enc_len + 1U);
                bpu->pending_pos = 0U;
                bpu->pending_have = 1U;
            }
        }
    }

    return rc;
}

// Drain pending buffer to IO under budget
static int bpu_send_pending(Bpu *bpu, uint16_t *budget_left, bool *progress_out)
{
    int rc;
    bool progress;
    bool done;

    rc = BPU_RC_OK;
    progress = false;
    done = false;

    if (progress_out != NULL) {
        *progress_out = false;
    }

    if (bpu == NULL) {
        rc = BPU_RC_ERR;
    } else {
        if (budget_left == NULL) {
            rc = BPU_RC_ERR;
        } else {
            if (bpu->pending_have == 0U) {
                bpu->pending_len = 0U;
                bpu->pending_pos = 0U;
            } else {
                if (bpu->io.tx_write_some == NULL) {
                    rc = BPU_RC_ERR;
                } else {
                    while (!done && bpu->pending_pos < bpu->pending_len && rc == BPU_RC_OK) {
                        size_t want;
                        size_t wrote;
                        uint16_t budget;
                        uint16_t chunk_cap;

                        want = 0U;
                        if (*budget_left != 0U) {
                            want = (size_t)(bpu->pending_len - bpu->pending_pos);
                        }

                        budget = *budget_left;

                        if (want > (size_t)budget) {
                            want = (size_t)budget;
                        }

                        chunk_cap = bpu->cfg.tx_chunk_max;
                        if (chunk_cap != 0U) {
                            if (want > (size_t)chunk_cap) {
                                want = (size_t)chunk_cap;
                            }
                        }

                        wrote = 0U;

                        if (want == 0U) {
                            done = true;
                        } else {
                            if (bpu->io.tx_write_some(bpu->io.ctx, &bpu->pending_buf[bpu->pending_pos], want, &wrote) != BPU_RC_OK) {
                                rc = BPU_RC_ERR;
                            } else {
                                if (wrote == 0U) {
                                    bpu->st.tx_skip_backpressure++;
                                    done = true;
                                } else {
                                    bpu->pending_pos = (uint16_t)(bpu->pending_pos + (uint16_t)wrote);
                                    *budget_left = (uint16_t)(*budget_left - (uint16_t)wrote);
                                    bpu->st.tx_bytes += (uint32_t)wrote;
                                    progress = true;
                                }
                            }
                        }
                    }

                    if (rc == BPU_RC_OK) {
                        if (bpu->pending_pos >= bpu->pending_len) {
                            bpu->pending_len = 0U;
                            bpu->pending_pos = 0U;
                            bpu->pending_have = 0U;

                            bpu->st.tx_frame_sent++;
                            bpu->st.pending_active = 0U;
                            bpu->st.pending_len = 0U;
                            bpu->st.pending_pos = 0U;
                        } else {
                            if (progress) {
                                bpu->st.tx_frame_partial++;
                            }

                            bpu->st.pending_active = 1U;
                            bpu->st.pending_len = (uint32_t)bpu->pending_len;
                            bpu->st.pending_pos = (uint32_t)bpu->pending_pos;
                        }
                    }
                }
            }
        }
    }

    if (progress_out != NULL) {
        *progress_out = progress;
    }

    return rc;
}

// Convert queued events into jobs
static int bpu_schedule_from_events(Bpu *bpu, uint32_t now_ms)
{
    int rc;
    bool done;

    rc = BPU_RC_OK;
    done = false;

    if (bpu == NULL) {
        rc = BPU_RC_ERR;
    } else {
        while (!done) {
            BpuEvent e;

            if (bpu_evq_pop(bpu, &e) != BPU_RC_OK) {
                done = true;
            } else {
                bool aged;
                BpuJob j;
                uint8_t tag;
                uint16_t copy_n;
                uint16_t i;

                aged = false;

                if ((uint32_t)(now_ms - e.t_ms) >= (uint32_t)bpu->cfg.aged_ms) {
                    aged = true;
                }

                if (aged) {
                    bpu->st.pick_aged++;

                    if (e.type == BPU_EVT_SENSOR) {
                        bpu->st.aged_hit_sensor++;
                    } else {
                        if (e.type == BPU_EVT_HB) {
                            bpu->st.aged_hit_hb++;
                        } else {
                            if (e.type == BPU_EVT_TELEM) {
                                bpu->st.aged_hit_telem++;
                            }
                        }
                    }
                }

                j.type = bpu_job_for_evt(e.type);
                j.flags = e.flags;
                j.t_ms = now_ms;

                tag = 0U;
                if (e.type == BPU_EVT_SENSOR) {
                    tag = 0x01U;
                } else {
                    if (e.type == BPU_EVT_HB) {
                        tag = 0x02U;
                    } else {
                        if (e.type == BPU_EVT_TELEM) {
                            tag = 0x03U;
                        } else {
                            if (e.type == BPU_EVT_CMD) {
                                tag = 0x04U;
                            } else {
                                tag = 0x00U;
                            }
                        }
                    }
                }

                j.payload[0] = tag;
                j.payload[1] = (uint8_t)e.len;

                copy_n = e.len;
                if (copy_n > (uint16_t)(sizeof(j.payload) - 2U)) {
                    copy_n = (uint16_t)(sizeof(j.payload) - 2U);
                }

                i = 0U;
                while (i < copy_n) {
                    j.payload[2U + i] = e.payload[i];
                    i++;
                }

                j.len = (uint16_t)(2U + copy_n);

                if (bpu_jobq_push_coalesce(bpu, &j) != BPU_RC_OK) {
                    rc = BPU_RC_ERR;
                }
            }
        }
    }

    return rc;
}

// Serialize and send queued jobs
static int bpu_flush_jobs(Bpu *bpu, uint32_t now_ms, uint16_t *budget_left)
{
    int rc;
    bool done;

    rc = BPU_RC_OK;
    done = false;

    if (bpu == NULL) {
        rc = BPU_RC_ERR;
    } else {
        if (budget_left == NULL) {
            rc = BPU_RC_ERR;
        } else {
            while (!done) {
                if (*budget_left == 0U) {
                    done = true;
                } else {
                    if (bpu->pending_have != 0U) {
                        bool progress;

                        progress = false;

                        if (bpu_send_pending(bpu, budget_left, &progress) != BPU_RC_OK) {
                            rc = BPU_RC_ERR;
                            done = true;
                        } else {
                            if (!progress) {
                                done = true;
                            }
                        }
                    } else {
                        if (bpu->jobq.count == 0U) {
                            done = true;
                        } else {
                            BpuJob j;
                            size_t decoded_len;
                            size_t worst_overhead;
                            size_t worst_on_wire;
                            uint8_t wire_len;
                            size_t free_sz;
                            int have_free;

                            bpu->st.flush_try++;

                            if (bpu_jobq_pop(bpu, &j) != BPU_RC_OK) {
                                done = true;
                            } else {
                                decoded_len = 4U + (size_t)j.len + 2U;
                                worst_overhead = (decoded_len / 254U) + 2U;
                                worst_on_wire = decoded_len + worst_overhead + 1U;

                                if (worst_on_wire > (size_t)(*budget_left)) {
                                    bpu->st.tx_skip_budget++;

                                    if (bpu->cfg.enable_degrade != 0U) {
                                        if (j.type == BPU_JOB_TELEM) {
                                            bpu->st.degrade_drop++;
                                        } else {
                                            (void)bpu_jobq_push_coalesce(bpu, &j);
                                            bpu->st.degrade_requeue++;
                                        }
                                    } else {
                                        (void)bpu_jobq_push_coalesce(bpu, &j);
                                    }

                                    done = true;
                                } else {
                                    free_sz = 0U;
                                    have_free = BPU_RC_ERR;

                                    if (bpu->io.tx_free != NULL) {
                                        have_free = bpu->io.tx_free(bpu->io.ctx, &free_sz);
                                    }

                                    if (have_free != BPU_RC_OK) {
                                        (void)bpu_jobq_push_coalesce(bpu, &j);
                                        bpu->st.degrade_requeue++;
                                        done = true;
                                    } else {
                                        if (free_sz < (size_t)bpu->cfg.tx_min_free) {
                                            (void)bpu_jobq_push_coalesce(bpu, &j);
                                            bpu->st.degrade_requeue++;
                                            bpu->st.tx_skip_backpressure++;
                                            done = true;
                                        } else {
                                            wire_len = 0U;
                                            if (j.len > 255U) {
                                                wire_len = 255U;
                                            } else {
                                                wire_len = (uint8_t)j.len;
                                            }

                                            if (bpu_build_frame(bpu, j.type, j.payload, wire_len) != BPU_RC_OK) {
                                                (void)bpu_jobq_push_coalesce(bpu, &j);
                                                bpu->st.degrade_requeue++;
                                                done = true;
                                            } else {
                                                bool progress;
                                                uint16_t before;

                                                before = *budget_left;
                                                progress = false;

                                                if (bpu_send_pending(bpu, budget_left, &progress) != BPU_RC_OK) {
                                                    (void)bpu_jobq_push_coalesce(bpu, &j);
                                                    bpu->pending_len = 0U;
                                                    bpu->pending_pos = 0U;
                                                    bpu->pending_have = 0U;
                                                    bpu->st.degrade_requeue++;
                                                    done = true;
                                                } else {
                                                    if (!progress) {
                                                        (void)bpu_jobq_push_coalesce(bpu, &j);
                                                        bpu->pending_len = 0U;
                                                        bpu->pending_pos = 0U;
                                                        bpu->pending_have = 0U;
                                                        bpu->st.degrade_requeue++;
                                                        bpu->st.tx_skip_backpressure++;
                                                        done = true;
                                                    } else {
                                                        bpu->st.flush_ok++;

                                                        if (before == *budget_left) {
                                                            done = true;
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    (void)now_ms;

    return rc;
}

// Initialize BPU state and defaults
int bpu_init(Bpu *bpu, const BpuIo *io, const BpuConfig *cfg)
{
    int rc;

    rc = BPU_RC_OK;

    if (bpu == NULL) {
        rc = BPU_RC_ERR;
    } else {
        if (io == NULL) {
            rc = BPU_RC_ERR;
        } else {
            if (cfg == NULL) {
                rc = BPU_RC_ERR;
            } else {
                if (io->tx_free == NULL) {
                    rc = BPU_RC_ERR;
                } else {
                    if (io->tx_write_some == NULL) {
                        rc = BPU_RC_ERR;
                    }
                }
            }
        }
    }

    if (rc == BPU_RC_OK) {
        bpu->io = *io;
        bpu->cfg = *cfg;

        bpu->evq.head = 0U;
        bpu->evq.tail = 0U;
        bpu->evq.count = 0U;

        bpu->jobq.head = 0U;
        bpu->jobq.tail = 0U;
        bpu->jobq.count = 0U;

        bpu->pending_len = 0U;
        bpu->pending_pos = 0U;
        bpu->pending_have = 0U;

        bpu->st.tick = 0U;
        bpu->st.ev_in = 0U;
        bpu->st.ev_out = 0U;
        bpu->st.ev_merge = 0U;
        bpu->st.ev_drop = 0U;
        bpu->st.job_in = 0U;
        bpu->st.job_out = 0U;
        bpu->st.job_merge = 0U;
        bpu->st.job_drop = 0U;
        bpu->st.tx_frame_sent = 0U;
        bpu->st.tx_frame_partial = 0U;
        bpu->st.tx_bytes = 0U;
        bpu->st.tx_skip_budget = 0U;
        bpu->st.tx_skip_backpressure = 0U;
        bpu->st.flush_try = 0U;
        bpu->st.flush_ok = 0U;
        bpu->st.pick_sensor = 0U;
        bpu->st.pick_hb = 0U;
        bpu->st.pick_telem = 0U;
        bpu->st.pick_aged = 0U;
        bpu->st.aged_hit_sensor = 0U;
        bpu->st.aged_hit_hb = 0U;
        bpu->st.aged_hit_telem = 0U;
        bpu->st.degrade_drop = 0U;
        bpu->st.degrade_requeue = 0U;
        bpu->st.pending_active = 0U;
        bpu->st.pending_len = 0U;
        bpu->st.pending_pos = 0U;
        bpu->st.dirty_mask_lo = 0U;
        bpu->st.dirty_mask_hi = 0U;
        bpu->st.work_us_last = 0U;
        bpu->st.work_us_max = 0U;

        bpu->seq = 0U;
        bpu->init_magic = 0x42505531U;
    }

    return rc;
}

// Add new event into queue
int bpu_push_event(Bpu *bpu, uint8_t evt_type, const uint8_t *payload, uint16_t len, uint32_t now_ms)
{
    int rc;
    BpuEvent e;
    uint16_t i;

    rc = BPU_RC_OK;

    if (bpu == NULL) {
        rc = BPU_RC_ERR;
    } else {
        if (payload == NULL) {
            rc = BPU_RC_ERR;
        } else {
            if (bpu->init_magic != 0x42505531U) {
                rc = BPU_RC_ERR;
            }
        }
    }

    if (rc == BPU_RC_OK) {
        if (evt_type == BPU_EVT_SENSOR) {
            bpu->st.pick_sensor++;
        } else {
            if (evt_type == BPU_EVT_HB) {
                bpu->st.pick_hb++;
            } else {
                if (evt_type == BPU_EVT_TELEM) {
                    bpu->st.pick_telem++;
                }
            }
        }

        if (len > (uint16_t)sizeof(e.payload)) {
            len = (uint16_t)sizeof(e.payload);
        }

        e.type = evt_type;
        e.flags = 0U;
        e.len = len;
        e.t_ms = now_ms;

        i = 0U;
        while (i < len) {
            e.payload[i] = payload[i];
            i++;
        }

        if (bpu_evq_push_coalesce(bpu, &e) != BPU_RC_OK) {
            rc = BPU_RC_ERR;
        }
    }

    return rc;
}

// Run one scheduling/flush cycle
int bpu_tick(Bpu *bpu, uint32_t now_ms)
{
    return bpu_tick_ex(bpu, now_ms, 0U);
}

// Tick with explicit microsecond time
int bpu_tick_ex(Bpu *bpu, uint32_t now_ms, uint32_t now_us)
{
    int rc;
    uint16_t budget;
    uint32_t t0;
    uint32_t t1;
    uint64_t dirty;
    bool have_t0;
    bool have_t1;

    rc = BPU_RC_OK;

    t0 = 0U;
    t1 = 0U;
    have_t0 = false;
    have_t1 = false;

    if (bpu == NULL) {
        rc = BPU_RC_ERR;
    } else {
        if (bpu->init_magic != 0x42505531U) {
            rc = BPU_RC_ERR;
        }
    }

    if (rc == BPU_RC_OK) {
        if (now_us != 0U) {
            t0 = now_us;
            have_t0 = true;
        } else {
            if (bpu_try_time_us(bpu, &t0) == BPU_RC_OK) {
                have_t0 = true;
            }
        }

        budget = bpu->cfg.tx_budget_bytes;

        if (bpu->pending_have != 0U) {
            bool progress;

            progress = false;
            if (bpu_send_pending(bpu, &budget, &progress) != BPU_RC_OK) {
                rc = BPU_RC_ERR;
            }
        }

        if (rc == BPU_RC_OK) {
            (void)bpu_schedule_from_events(bpu, now_ms);
            (void)bpu_flush_jobs(bpu, now_ms, &budget);
        }

        bpu->st.tick++;

        dirty = bpu_dirty_mask(bpu);
        bpu->st.dirty_mask_lo = (uint32_t)(dirty & 0xFFFFFFFFULL);
        bpu->st.dirty_mask_hi = (uint32_t)((dirty >> 32) & 0xFFFFFFFFULL);

        if (now_us != 0U) {
            t1 = now_us;
            have_t1 = true;
        } else {
            if (bpu_try_time_us(bpu, &t1) == BPU_RC_OK) {
                have_t1 = true;
            }
        }

        if (have_t0 && have_t1) {
            if (t1 >= t0) {
                bpu->st.work_us_last = (uint32_t)(t1 - t0);
            } else {
                bpu->st.work_us_last = 0U;
            }

            if (bpu->st.work_us_last > bpu->st.work_us_max) {
                bpu->st.work_us_max = bpu->st.work_us_last;
            }
        }
    }

    return rc;
}

// Copy stats snapshot
int bpu_get_stats(const Bpu *bpu, BpuStats *out)
{
    int rc;

    rc = BPU_RC_OK;

    if (bpu == NULL) {
        rc = BPU_RC_ERR;
    } else {
        if (out == NULL) {
            rc = BPU_RC_ERR;
        } else {
            if (bpu->init_magic != 0x42505531U) {
                rc = BPU_RC_ERR;
            }
        }
    }

    if (rc == BPU_RC_OK) {
        *out = bpu->st;
    }

    return rc;
}

#endif
