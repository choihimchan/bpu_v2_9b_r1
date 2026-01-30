# BPU v2.9b-r1 — Runtime Statistics

This document describes runtime counters exposed by  
**BPU (Batch Processing Unit)** to make scheduling behavior
**observable and explainable under pressure**.

All counters are emitted during execution and referenced in:
- `docs/log_samples.md`
- `docs/diagram.md`

---

## 1. Purpose of Runtime Statistics

BPU does not treat data loss or delay as silent failures.

Instead, every meaningful scheduling decision is reflected in
explicit counters so that:

- Backpressure can be detected
- Budget exhaustion is visible
- Degradation is intentional and explainable

These statistics are a core part of BPU’s design.

---

## 2. Transmission Counters

### `tx_flush`
- **Meaning**: Number of successful TX flush operations
- **Increments when**:
  - TX buffer is available
  - Budget allows job transmission

This counter indicates forward progress.

---

### `skipTX`
- **Meaning**: TX attempt skipped due to backpressure
- **Increments when**:
  - UART TX buffer is not writable

A rising `skipTX` indicates output congestion.

---

## 3. Degradation Counters

### `drop_low_pri`
- **Meaning**: Low-priority jobs dropped intentionally
- **Increments when**:
  - Bytes-per-tick budget is exceeded
  - Job priority allows degradation

This counter confirms graceful degradation under pressure.

---

## 4. Queue Observation Counters

### `queue_depth`
- **Meaning**: Current number of jobs in the queue
- **Observed at runtime**

### `queue_depth_max`
- **Meaning**: Maximum queue depth observed
- **Used to evaluate worst-case pressure**

Queue depth metrics help identify sustained overload conditions.

---

## 5. Counter Summary

| Counter           | Meaning                              | Trigger condition                    |
|-------------------|--------------------------------------|--------------------------------------|
| tx_flush          | Successful TX flush                  | TX available + budget sufficient     |
| skipTX            | TX skipped due to backpressure       | UART TX buffer blocked               |
| drop_low_pri      | Low-priority job dropped             | Budget exceeded                      |
| queue_depth       | Current queue size                   | Runtime observation                  |
| queue_depth_max   | Maximum observed queue size          | Sustained pressure                   |

---

## Notes

- Counters are monotonic unless explicitly reset
- Interpretation examples are provided in `docs/log_samples.md`
- Each counter maps directly to a control path in `docs/diagram.md`
