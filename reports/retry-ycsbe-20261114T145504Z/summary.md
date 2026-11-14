# Follow-up adaptive retry / boundary-only probe

36 valid trials; no execution errors; no retry-exhausted logical transactions.
1s warmup + 2s measurement; one trial per case, full optional metrics enabled.
Source differs from the preceding paired campaign; do not merge their measurements into one final curve.

| Algorithm | Policy | Theta | TPS | Exhausted | Unfinished |
|---|---|---:|---:|---:|---:|
| exact-range | no-wait | 0.0 | 179138 | 0 | 32 |
| exact-range | no-wait | 1.5 | 45988.3 | 0 | 32 |
| exact-range | wait-die | 0.0 | 202268 | 0 | 31 |
| exact-range | wait-die | 1.5 | 49627.6 | 0 | 32 |
| exact-range | wound-wait | 0.0 | 224460 | 0 | 32 |
| exact-range | wound-wait | 1.5 | 28886.5 | 0 | 32 |
| gap-lock | no-wait | 0.0 | 37817.8 | 0 | 32 |
| gap-lock | no-wait | 1.5 | 8413.03 | 0 | 32 |
| gap-lock | wait-die | 0.0 | 41381.7 | 0 | 31 |
| gap-lock | wait-die | 1.5 | 3252.06 | 0 | 32 |
| gap-lock | wound-wait | 0.0 | 38754.3 | 0 | 32 |
| gap-lock | wound-wait | 1.5 | 5197.67 | 0 | 32 |
| hierarchical | no-wait | 0.0 | 240359 | 0 | 31 |
| hierarchical | no-wait | 1.5 | 71017.9 | 0 | 32 |
| hierarchical | wait-die | 0.0 | 207339 | 0 | 28 |
| hierarchical | wait-die | 1.5 | 102826 | 0 | 31 |
| hierarchical | wound-wait | 0.0 | 219758 | 0 | 31 |
| hierarchical | wound-wait | 1.5 | 29064.9 | 0 | 31 |
| next-key | no-wait | 0.0 | 51894.7 | 0 | 32 |
| next-key | no-wait | 1.5 | 12723.3 | 0 | 31 |
| next-key | wait-die | 0.0 | 55376.4 | 0 | 31 |
| next-key | wait-die | 1.5 | 4566.7 | 0 | 32 |
| next-key | wound-wait | 0.0 | 52739.7 | 0 | 30 |
| next-key | wound-wait | 1.5 | 7398.47 | 0 | 32 |
| prior-key | no-wait | 0.0 | 56831.5 | 0 | 32 |
| prior-key | no-wait | 1.5 | 12706.4 | 0 | 32 |
| prior-key | wait-die | 0.0 | 55140.5 | 0 | 32 |
| prior-key | wait-die | 1.5 | 4678.24 | 0 | 32 |
| prior-key | wound-wait | 0.0 | 52951.9 | 0 | 32 |
| prior-key | wound-wait | 1.5 | 7465.85 | 0 | 32 |
| record-2pl | no-wait | 0.0 | 122816 | 0 | 30 |
| record-2pl | no-wait | 1.5 | 18257.1 | 0 | 32 |
| record-2pl | wait-die | 0.0 | 121151 | 0 | 31 |
| record-2pl | wait-die | 1.5 | 8295.55 | 0 | 31 |
| record-2pl | wound-wait | 0.0 | 116460 | 0 | 31 |
| record-2pl | wound-wait | 1.5 | 12120.5 | 0 | 32 |
