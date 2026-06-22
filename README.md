# UPDI Programmer (Flipper Zero FAP)

Program modern AVR microcontrollers over the **UPDI** single-wire interface, standalone on
a Flipper Zero — no PC required. Reads the device signature, chip-erases, flashes from an
Intel HEX file on the SD card, dumps flash back to a HEX file, and reads fuses.

There is already an AVR **ISP** programmer FAP in the wild (Skorpionm's "AVR Flasher"), but
ISP is the old SPI-style interface for classic AVRs. This app fills the gap for **UPDI**,
the interface used by every modern AVR (tinyAVR 0/1/2, megaAVR 0, AVR EA).

> ⚠️ **Standard 3.3 V UPDI only. No High-Voltage UPDI.** See [Safety & limitations](#safety--limitations).

The build artifact is [`dist/updi_prog.fap`](dist/updi_prog.fap).

---

## Status

| Capability | State |
|---|---|
| Read signature → identify device | ✅ implemented, emulator-verified |
| Read fuses | ✅ |
| Chip erase (+ unlock locked devices) | ✅ |
| Flash from Intel HEX + read-back verify | ✅ |
| Verify flash against Intel HEX (reports first diff) | ✅ |
| Dump flash → Intel HEX on SD | ✅ |
| NVM P:0 (tinyAVR 0/1/2, megaAVR 0) | ✅ |
| NVM P:3 (AVR EA) | ✅ |
| EEPROM / user-row write, fuse *write* UI | ⏳ core present (`updi_nvm_write_fuse`), no menu entry yet |
| High-Voltage UPDI | ❌ impossible on Flipper (3.3 V GPIO) |

The whole UPDI protocol stack is verified on the PC against a UPDI target emulator
(116 checks, both NVM variants — see [Host tests](#host-tests)). **On-hardware testing is
still required** — use the [Manual hardware test checklist](#manual-hardware-test-checklist).

---

## Supported devices (v1)

Geometry is generated from Microchip's device packs (see
[`scripts/gen_device_table.py`](scripts/gen_device_table.py)). Unknown signatures are
rejected with a clear message rather than guessing.

- **AVR EA** (NVM P:3, 24-bit): `AVR64EA28`, `AVR64EA32`, `AVR64EA48`
- **tinyAVR 0/1/2** (NVM P:0, 16-bit): `ATtiny412`, `ATtiny416`, `ATtiny814`, `ATtiny816`,
  `ATtiny1614`, `ATtiny1616`, `ATtiny3216`, `ATtiny3226`, `ATtiny827`, `ATtiny1627`
- **megaAVR 0** (NVM P:0, 16-bit): `ATmega808`, `ATmega809`, `ATmega1608`, `ATmega1609`,
  `ATmega3208`, `ATmega3209`, `ATmega4808`, `ATmega4809`

Adding a part: drop its `<name>.py` pack into
`reference/pymcuprog/deviceinfo/devices/`, add the name to `PARTS` in the generator, and
re-run `python scripts/gen_device_table.py`. (Microchip has not yet published `avr16ea*` /
`avr32ea*` packs; only the `avr64ea*` family is available.)

---

## Wiring (resistor-bridge, v1)

This mirrors the pyupdi / SerialUPDI topology — simplest and lowest-risk, using the
Flipper's stock USART on **GPIO 13 (TX)** and **14 (RX)**.

| Flipper pin | Connection |
|---|---|
| **13** (USART TX) | → **1 kΩ** resistor → UPDI line node |
| **14** (USART RX) | → UPDI line node (directly) |
| UPDI line node | → target **UPDI** pin |
| **9** (3V3) | → target **VCC** (optional — powers the target from the Flipper) |
| **11** or **18** (GND) | → target **GND** (common ground required) |

```
 Flipper 13 (TX) ──[ 1kΩ ]──┬────────────── target UPDI
 Flipper 14 (RX) ───────────┤
                            └──[ 4.7k–10k ]── 3V3   (optional pull-up, helps at higher baud)

 Flipper  9 (3V3) ───────────────────────────  target VCC   (optional)
 Flipper 11/18 (GND) ────────────────────────  target GND   (required)
```

**Why the resistor + why echo handling matters:** TX and RX are tied onto one wire, so
every byte the Flipper sends is also received back on RX. The transport layer reads and
discards exactly those echoed bytes after each transmit, then reads the genuine target
response. This is the #1 thing people get wrong; it's built into
[`transport/transport_furi.c`](transport/transport_furi.c) from the start.

A 4.7 kΩ–10 kΩ pull-up from the UPDI node to 3.3 V can improve reliability at higher baud.

---

## Safety & limitations

- **3.3 V only.** Flipper GPIO is 3.3 V. Modern AVRs program fine at 3.3 V. **Do not** drive
  a target powered at 5 V without a level translator — power the target at 3.3 V (Flipper
  pin 9 can supply it) or use a level shifter on the UPDI line.
- **No High-Voltage UPDI.** The Flipper cannot put 12 V on a GPIO. If a target's UPDI pin
  has been fused to GPIO or RESET (`RSTPINCFG`), only a **12 V HV-UPDI programmer** can
  recover it. This app cannot, and does not pretend to.
- **Default baud 115200.** UPDI autobauds; higher speeds depend on the line's rise/fall
  times and are fiddly. A baud selector is in **Settings**.
- **HEX address assumption:** flash HEX addresses are treated as **0-based** flash offsets
  (what `avr-gcc`/XC8 emit). The app adds the device's mapped flash base internally.

---

## Pinned firmware

Built and validated against:

- **Momentum** firmware, SDK **`mntm-012`**, hardware target **f7**
- App check: **Target 7, API 87.1**

The `furi_hal_serial` API drifts across firmware versions; `transport_furi.c` was written
against the headers in this exact SDK. If you build for a different channel and the serial
API differs, that file is the only place that needs adjusting.

---

## Build

Uses [`ufbt`](https://github.com/flipperdevices/flipperzero-ufbt).

```sh
pip install ufbt

# Point ufbt at the Momentum SDK (one-time):
ufbt update --index-url=https://up.momentum-fw.dev/firmware/directory.json

# Build the .fap (run from this directory):
ufbt

# Build + install + launch on a connected Flipper:
ufbt launch
```

The result is `dist/updi_prog.fap`. To install manually, copy it to
`SD:/apps/GPIO/updi_prog.fap`.

---

## Usage

From **Apps → GPIO → UPDI Programmer**:

1. **Connect / Read Device** — activates UPDI, reads the SIB + signature, identifies the
   part, and shows flash/eeprom/fuse geometry.
2. **Flash from HEX** — pick a `.hex` file from the SD card. The app chip-erases, writes,
   and **read-back verifies**. The target is released from reset afterwards so it runs.
3. **Verify from HEX** — pick a `.hex` file and compare it against the device's current
   flash **without erasing or writing**. On a mismatch it reports the first differing byte
   (flash offset, expected HEX value vs. value on the chip). A locked device reports
   "Device locked" rather than being touched.
4. **Dump Flash** — reads the whole flash and writes `/ext/updi/<device>_dump.hex`.
5. **Chip Erase** — erases flash/EEPROM. Also unlocks a locked device.
6. **Read Fuses** — lists fuse bytes.
7. **Settings** — baud rate.
8. **About / Wiring** — on-device wiring and warnings.

---

## Architecture

The UPDI protocol is kept fully portable and **has no FURI dependencies** — it talks to
hardware only through the [`updi/updi_transport.h`](updi/updi_transport.h) seam, so it can
be audited against pymcuprog and unit-tested off-device.

```
updi_prog.c                 app entry: ViewDispatcher + SceneManager + event loop
ui/                         scenes (menu, device info, progress, result, fuses,
                            settings, about) + the background worker thread
updi/
  updi_constants.h          UPDI opcodes / ASI registers / keys (ported from pymcuprog)
  updi_transport.h          send / recv / double_break / millis seam (the porting boundary)
  updi_link.c/.h            SYNCH framing, LDCS/STCS, LDS/STS, LD/ST, REPEAT, KEY, SIB,
                            block read/write (16- and 24-bit addressing)
  updi_nvm.c/.h             progmode/keys/reset, chip erase, page erase/write, fuses
                            (NVM P:0 and P:3 via one descriptor table)
  updi_device.c/.h          signature → geometry table (generated)
  updi_session.c/.h         connect/identify, flash image write+verify, dump, fuses
transport/transport_furi.c  the ONE hardware file: furi_hal_serial + echo discard
hex/intel_hex.c/.h          Intel HEX parser (into an image) + streaming writer
host_test/                  PC unit tests + a UPDI target emulator (not built into the fap)
scripts/gen_device_table.py device table generator
```

Protocol details are ported from **pymcuprog `serialupdi`** (the canonical reference) and
cross-checked against the Microchip datasheet register maps. Each non-obvious step carries
a one-line comment citing the register or the pymcuprog function it mirrors.

---

## Host tests

The portable core (link framing, NVM choreography for both variants, Intel HEX, device
table, session image write/verify/dump) is tested on the PC against a UPDI target emulator
— no Flipper needed:

```powershell
./host_test/run.ps1      # uses clang or gcc; builds + runs, expect "0 failures"
```

This is the substitute for hardware I can run automatically; it caught real ordering bugs
during development. It does **not** replace the on-hardware checklist below.

---

## Manual hardware test checklist

Run these against a real chip (AVR64EA48 recommended, or any supported part). Each item
maps to a build-brief phase gate.

**Setup**
- [ ] Wire per [Wiring](#wiring-resistor-bridge-v1). Confirm common GND. Power target at 3.3 V.
- [ ] Pin 13 → 1 kΩ → UPDI node; pin 14 → UPDI node directly. (Optional 4.7–10 kΩ pull-up.)
- [ ] Install `updi_prog.fap`, launch it.

**Gate 1 — Transport / link (signature):**
- [ ] **Connect / Read Device** shows the correct part name and signature
      (AVR64EA48 = `1E 96 1E`), family `AVR EA`, and `NVM P:3`.
- [ ] A *wrong/disconnected* UPDI line reports **"No response (timeout)"** (not a hang).

**Gate 2 — Read & erase:**
- [ ] **Chip Erase** succeeds.
- [ ] **Dump Flash** writes `/ext/updi/<device>_dump.hex`. After a chip erase the dump is
      all `0xFF` in the program region.

**Gate 3 — Flash write + verify:**
- [ ] Build a blink HEX for the target (e.g. with `avr-gcc`/PlatformIO). Copy it to the SD.
- [ ] **Flash from HEX** completes with **"Write + read-back verify passed."**
- [ ] The target runs the new firmware (LED blinks) after flashing.
- [ ] **Dump Flash** again and `diff` the dump against the original HEX's data region — they match.

**Gate 4 — Fuses & robustness:**
- [ ] **Read Fuses** shows plausible values (e.g. `F1`/BODCFG, `F5`/SYSCFG0 non-`0xFF`).
- [ ] Flashing a known-good HEX twice in a row both verify.
- [ ] Try 230400 baud in **Settings**; if it fails, 115200 should still work (add the pull-up).

**Locked-device path (optional, destructive):**
- [ ] On a device with the UPDI/lock set, **Connect** reports "Device locked"; **Chip Erase**
      unlocks it (erases everything), after which **Connect** identifies it.

If anything fails, capture which gate and the on-screen error; the error strings map to
`UpdiStatus` in [`updi/updi_link.h`](updi/updi_link.h).

---

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| "No response (timeout)" | Wiring, no common GND, target not at 3.3 V, or wrong UPDI pin. Add the pull-up. |
| "Serial busy/failed" | Another GPIO app is holding the USART, or the CLI is attached. Close it. |
| "Unknown signature" | Part not in the device table — add its pack and regenerate. |
| "Verify mismatch" | Flaky line at high baud (drop to 115200, add pull-up), or a 5 V target. |
| "Device locked" | Use **Chip Erase** to unlock (erases all). If UPDI was fused off, you need HV-UPDI. |

---

## v2 ideas (not blocking v1)

- **Single-wire half-duplex (HDSEL):** drive UPDI on one GPIO with no external resistor and
  no echo to discard, by setting `USART_CR3.HDSEL` via the STM32 LL drivers. Cleaner, but
  requires dropping below `furi_hal_serial`. The bridge version ships first.
- EEPROM / user-row write screens and a fuse-write editor (core support already exists).

---

## References

- pymcuprog `serialupdi` — protocol ground truth:
  <https://github.com/microchip-pic-avr-tools/pymcuprog/tree/main/pymcuprog/serialupdi>
- SpenceKonde AVR-Guidance — jtag2updi.md (why UPDI works the way it does):
  <https://github.com/SpenceKonde/AVR-Guidance/blob/master/UPDI/jtag2updi.md>
- pyupdi (documents the resistor-bridge wiring): <https://github.com/mraardvark/pyupdi>
- ufbt: <https://github.com/flipperdevices/flipperzero-ufbt>

## License

MIT. Protocol logic is an independent C port of the (MIT-licensed) pymcuprog reference.
