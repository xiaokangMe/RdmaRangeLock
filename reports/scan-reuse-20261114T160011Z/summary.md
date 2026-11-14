# Reusing held classic scan protection

Paired binaries; alternating old/new order; 2 trial(s)/configuration; optional metrics disabled.

| Algorithm | Policy | Theta | Old median TPS | New median TPS | Ratio |
|---|---|---:|---:|---:|---:|
| next-key | no-wait | 0 | 52,682.6 | 54,851.2 | 1.041x |
| next-key | no-wait | 1.5 | 14,064.3 | 12,693.5 | 0.903x |
| next-key | wait-die | 0 | 56,279.8 | 55,070.4 | 0.979x |
| next-key | wait-die | 1.5 | 4,542.5 | 4,467.8 | 0.984x |
| next-key | wound-wait | 0 | 55,551.7 | 57,138.8 | 1.029x |
| next-key | wound-wait | 1.5 | 7,268.8 | 7,191.6 | 0.989x |
| gap-lock | no-wait | 0 | 41,974.7 | 37,939.5 | 0.904x |
| gap-lock | no-wait | 1.5 | 8,687.5 | 8,459.5 | 0.974x |
| gap-lock | wait-die | 0 | 38,788.7 | 41,413.9 | 1.068x |
| gap-lock | wait-die | 1.5 | 3,138.4 | 3,104.9 | 0.989x |
| gap-lock | wound-wait | 0 | 40,008.6 | 38,716.1 | 0.968x |
| gap-lock | wound-wait | 1.5 | 5,094.6 | 5,205.8 | 1.022x |
| prior-key | no-wait | 0 | 53,990.1 | 54,411.9 | 1.008x |
| prior-key | no-wait | 1.5 | 13,817.0 | 13,154.3 | 0.952x |
| prior-key | wait-die | 0 | 55,248.2 | 56,758.2 | 1.027x |
| prior-key | wait-die | 1.5 | 4,487.1 | 4,388.6 | 0.978x |
| prior-key | wound-wait | 0 | 55,817.2 | 53,522.7 | 0.959x |
| prior-key | wound-wait | 1.5 | 7,235.1 | 7,230.3 | 0.999x |
