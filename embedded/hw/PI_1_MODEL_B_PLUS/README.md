# HW test: Raspberry Pi 1 Model B+ V1.2 — first full-Linux, native-gcc PASS

Status: **PASS** (2026-07-27, tree `055eb70ac8a6`, clean)

Every prior target was a bare-metal MCU cross-compiled from macOS through
a PlatformIO toolchain. This one is independent in every dimension that
matters:

- **Native compilation** — the distro's own `gcc (Raspbian 14.2.0-19+rpi1)`
  running ON the target compiled the sources there. No cross toolchain, no
  PlatformIO, no Arduino core — a compiler binary nothing else in the
  matrix shares. The build is also `-Wall -Wextra -Werror`-clean on
  gcc 14.2.
- **Full OS** — Linux 6.18 (Raspbian trixie, armv6l): the host-build path
  (`embedded/harness/host_main.c`, stdout) on 2014 silicon.
- **32-bit userland ⇒ portable backend** — armv6 gcc has no `__int128`,
  so the Pi exercises fp_math.h's portable two-limb backend while the
  local reference uses the native `__int128` backend: a genuine
  cross-backend AND cross-compiler compare.
- **ARMv6 + hard-float ABI** — ARM1176JZF-S with VFPv2, armhf userland;
  the FPU is real and the ABI would happily use it. The hash agreeing is
  the integer-only discipline holding on yet another compiler/ABI cell.
- **Legacy Linux target**: 700 MHz single-core Raspberry Pi 1 B+ from
  2014, 512 MB RAM.

| | hash | runtime (informational) |
|---|---|---|
| host reference (arm64 macOS, `__int128` backend) | `37d6a1208d99182f` | — |
| **Pi 1 B+ ARM1176 @ 700 MHz, native gcc -O2** | **`37d6a1208d99182f`** | 71 ms |

Raw transcript: `results/2026-07-27-pi1-bplus.txt` — includes remote
uname, device-tree model string, gcc version, and SHA-256 of the exact
sources compiled, verified identical to the local tree's copies.

## Procedure

`harness/run_pi.sh [host]` is the one-command runner:

1. Build the host reference locally and extract the expected hash (same
   contract as the MCU targets' `prepare.sh` — no committed pin).
2. `scp` the six sources to `pi@<host>:/home/pi/astronav-profile/`.
3. Build there with `gcc -std=c99 -O2 -Wall -Wextra -Werror`, run, and
   capture a provenance-stamped transcript.
4. Compare hashes and synthesize the standard record line; fail-closed
   (non-zero exit unless PASS).

The compile takes a while on this CPU — be patient before declaring a
hang. SSH auth is whatever the operator has set up (key, agent, or an
interactive password prompt); credentials are deliberately not recorded
here.

## Hardware / provisioning

- Board: Raspberry Pi 1 Model B+ Rev 1.2 (2014), BCM2835
- CPU: ARM1176JZF-S @ 700 MHz (ARMv6, VFPv2), 512 MB RAM
- OS: Raspberry Pi OS Lite (trixie, armhf, 2026-06 image) on SD card,
  headless: SSH enabled via boot-partition `ssh` marker file, login
  seeded via `userconf.txt`. The image ships gcc preinstalled, so the
  board never needed internet access.
- Network: wired Ethernet, DHCP — the IP can move between boots; mDNS
  name `raspberrypi.local` is the stable handle (pass the IP to
  `run_pi.sh` if multicast is filtered on your switch).
