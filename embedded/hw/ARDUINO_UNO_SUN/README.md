# HW test: Arduino Uno — SUN profile slice

Status: **PASS** (2026-07-27, tree `055eb70ac8a6`, clean) —
hash `f6d799f2b7e424a7` == host reference, ~9.1 s per run,
24,342 B flash (75.5%), 784 B static RAM.

Same physical card, process, and config as `../ARDUINO_UNO/` (see that
README for hardware details and the 8-bit significance); this folder's
`prepare.sh` copies `profile_sun.c` — the Sun almanac slice: inertial +
earth-fixed direction, distance/SD/HP, GHA Aries over 32 epochs.

Raw capture: `results/2026-07-27-uno-sun.txt`.
