# BPU Runtime Statistics

These counters provide observability into scheduler behavior.

## Counters

- `tx_flush`
  - Number of successful TX flush operations

- `skipTX`
  - Incremented when TX is blocked

- `drop_low_pri`
  - Low priority jobs dropped due to budget pressure

- `queue_depth_max`
  - Maximum observed queue depth

These stats are referenced in runtime logs and diagrams.

## Counter summary

| Counter        | Meaning                          | Trigger condition              |
|----------------|----------------------------------|--------------------------------|
| tx_flush       | Successful TX flush              | TX available + budget ok       |
| skipTX         | TX skipped due to backpressure   | UART TX buffer blocked         |
| drop_low_pri   | Low priority job dropped         | Budget exceeded                |
| queue_depth    | Current queue size               | Runtime observation            |

