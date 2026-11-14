# Reusing held classic scan protection

Paired binaries; alternating old/new order; 1 trial(s)/configuration; optional metrics disabled.

| Algorithm | Policy | Theta | Old median TPS | New median TPS | Ratio |
|---|---|---:|---:|---:|---:|
| next-key | no-wait | 0 | 56,921.2 | 54,198.8 | 0.952x |
| next-key | no-wait | 1.5 | 14,177.6 | 15,601.4 | 1.100x |
| next-key | wait-die | 0 | 54,849.0 | 53,731.6 | 0.980x |
| next-key | wait-die | 1.5 | 4,531.0 | 5,507.2 | 1.215x |
| next-key | wound-wait | 0 | 53,579.9 | 55,357.0 | 1.033x |
| next-key | wound-wait | 1.5 | 7,161.6 | 9,875.3 | 1.379x |
| gap-lock | no-wait | 0 | 40,298.1 | 38,700.9 | 0.960x |
| gap-lock | no-wait | 1.5 | 8,276.4 | 11,657.4 | 1.409x |
| gap-lock | wait-die | 0 | 42,817.9 | 37,935.3 | 0.886x |
| gap-lock | wait-die | 1.5 | 3,089.3 | 4,062.2 | 1.315x |
| gap-lock | wound-wait | 0 | 37,462.7 | 39,064.8 | 1.043x |
| gap-lock | wound-wait | 1.5 | 5,142.0 | 7,084.4 | 1.378x |
| prior-key | no-wait | 0 | 52,197.6 | 58,285.5 | 1.117x |
| prior-key | no-wait | 1.5 | 13,791.1 | 16,585.6 | 1.203x |
| prior-key | wait-die | 0 | 54,199.5 | 58,386.5 | 1.077x |
| prior-key | wait-die | 1.5 | 4,504.8 | 5,549.6 | 1.232x |
| prior-key | wound-wait | 0 | 58,094.0 | 54,567.4 | 0.939x |
| prior-key | wound-wait | 1.5 | 7,260.0 | 9,673.1 | 1.332x |

The console denominator was accidentally fixed at 72; metadata correctly records 36 trials (one round). All 36 raw results are retained.
