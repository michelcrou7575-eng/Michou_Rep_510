# TGIS-510 — Thermal Glue Inspection System

Home-lab / after-hours project. **Separate codebase and separate GitHub
repository from the 410 Rotaliner Tubing Seal Seam Monitor** (factory
floor, auxiliary Siemens S7-315-2 + SIMATIC Manager — tracked in
`michelcrou7575-eng/Michou_Rep_410`, not here) — do not conflate the two
firmware/PLC projects, even though the machines are related: the 410 Tuber
(which 410 Rotaliner adds QC to) feeds tubes into the 510 Bottomer (which
this TGIS-510 project inspects), two sequential stations on the same line.

Industrial QC system detecting hot-melt glue application on tubes moving at
high speed. Confirms glue presence, temperature, and quantity across both
glue strips per tube pass, and pushes a stable QC-confirmation image to an
operator HMI (Omron NS12).

## Status

`src/HotMelt_MLX90640_80032_9_8_1.cpp` is the **real, currently-running
firmware** — uploaded directly from the PlatformIO project, not written by
Claude. It's the actual source of truth for this project. It's a
**foundation build**: MLX90640 bring-up, system-state manager, MCP23017
machine I/O, and NS12 Memory Link telemetry/matrix output are working.
Encoder, tube presence sensor, Keyence integration, and machine RUN/STOP
handshaking are explicitly the next stages, per the file's own header — none
of that exists in this build yet.

`design-notes/tgis510_v4_14_0_speculative.cpp.txt` is a **discarded
speculative draft** written earlier in this session from a project handoff
description, before the real source was available. It invented an entire
encoder/Keyence/position-projection layer that doesn't exist in the real
firmware, and got the NS12 protocol framing and baud rate wrong (see below).
It is **not part of the build** (renamed `.txt` so PlatformIO won't compile
it) and is kept only in case any of its ideas are useful once encoder/Keyence
work actually starts. Don't treat anything in it as fact about the real
system.

## Hardware

| Component | Role |
|---|---|
| Waveshare ESP32-S3-Zero-M (ESP32-S3FH4R2) | Main controller |
| MLX90640 32×24 thermal camera | Glue strip presence/temp/quantity (I2C 800kHz) |
| Omron NS12-TS00B-V2 HMI | Operator display, Memory Link protocol, RS-232 via HIN232CP |
| MCP23017 I/O expander | Machine I/O (stop/interlock), shares I2C bus |
| Onboard WS2812 RGB LED | Status indication |

Keyence vision sensor and ZATOR LMZ02 encoder are planned for later stages
and are not wired into this build yet.

## Build

PlatformIO, Arduino framework — see `platformio.ini`. The PlatformIO
project root is this folder (`510 Quality Control System/`), not the repo
root — `cd` in here first.

```
cd "510 Quality Control System"
pio run
pio run -t upload
pio device monitor
```

The MLX90640 driver is a **patched local copy** in
`lib/Adafruit_MLX90640_QC510/` (adds `getRawFrame()` for raw-ADC diagnostic
dumps — see that file's header comment) — do not add the public
`adafruit/Adafruit MLX90640` library to `lib_deps`, PlatformIO would then
have two conflicting copies.

Board is currently a generic `esp32-s3-devkitc-1` definition as a
placeholder for the actual Waveshare ESP32-S3-Zero-M board.

## Confirmed values (from the real running firmware — supersede anything
## said elsewhere, including the speculative draft)

- **NS12 baud is 9600, not 38400.** The file's own header states 38400
  showed ~15% read timeouts on this bench setup; 9600 gave zero
  timeouts/errors. (An earlier project summary claimed 38400 was the
  confirmed source of truth — that's now known to be wrong, or at least
  superseded.)
- **NS12 Memory Link frame format** is not a fixed-width, checksummed
  frame. It's `ESC 'W' 'M' '0' <4-hex addr><2-dec count><comma-separated
  hex words, zero-suppressed> CR` for writes, and `ESC 'R' 'M' '0' <4-hex
  addr><2-dec count> CR` for reads — `*S='0'` explicitly selects
  checksum-off, so there is no FCS byte at all.
- ESC=0x1B is used for every command on this unit, confirmed for both reads
  and writes (contradicts the manual's documented 0x1C for WM/WD).
- I2C at 800kHz (1MHz silently broke MCP23017 enumeration).
- MLX90640 at 32Hz nominal refresh, ~7.8–8.7 measured FPS.
- 16×8 Word Lamp matrix at `$W700`, column-major stride 8 — full 32×24
  caused the NS12 to miss RM read requests entirely, so the matrix stays
  16×8 in this build (see the open issue below — column-pacing alone did
  **not** fix that class of problem).
- `MATRIX_TEMP_MAX_C` is still 40.0°C (bench value) and
  `CAPTURE_TRIGGER_TEMP_C` is 180.0°C — both explicitly flagged in-code as
  needing retuning before running against real hot melt/production
  temperatures. Note the trigger constant is currently set well *above*
  the matrix max, which looks backwards; check this against the file's own
  commented-out `90.0f` alternative before trusting it.

## Open issue: RM reads are currently failing 100% on real hardware

The diagnostic snapshot pasted when this file was added showed:

```
NS12 read requests  : 628
NS12 reads OK       : 0
NS12 timeouts       : 0
NS12 parse errors   : 628
NS12 resync discards: 0
```

Every single read request got a prompt, correctly-framed-enough reply (no
timeouts, no resync discards — meaning the parser's very first byte was
already ESC as expected) that still failed to validate as a real RM
response. This is with column-paced 16×8 writes already in place (one
8-word column every 25ms), so **column-pacing by itself did not fix the
read-starvation problem** the 32×24 mode was reverted over — worth knowing
before re-attempting a wider matrix.

**Hypothesis worth checking first** (matches the "0 resync discards, 0
timeouts" signature): the ESP32 RX line may be picking up an **echo of its
own outgoing WM writes** rather than genuine RM replies from the PT — either
real electrical crosstalk on the HIN232CP wiring, or the PT itself echoing
received commands. Every line starts with the required ESC byte (so it's
never treated as noise) but `parseReadResponse()` immediately fails the
`response[1]=='R' && response[2]=='M'` check if what actually arrived was
the echo of a prior `ESC 'W' 'M' ...` write — which fits perfectly, since
WM matrix writes fire far more often (every 25ms) than RM read requests
(every 500ms), so whatever's sitting in the RX FIFO right before a read poll
starts is very likely to be a recent WM write's echo.

`NS12_DEBUG_RAW_RX` (currently `0` in `src/HotMelt_MLX90640_80032_9_8_1.cpp`)
is already built for exactly this — flip it to `1` and confirm whether the
bytes captured right after an RM request are literally identical to the
last WM command sent. If they match, it's the echo/crosstalk theory; if they
genuinely start with `ESC R M` but still fail to parse, the field-offset
guess (`candidateFieldOffsets = {3, 4}`) or the response's actual layout is
wrong and needs a real byte-for-byte capture to pin down.

## Repo layout

- `src/HotMelt_MLX90640_80032_9_8_1.cpp` — the real firmware, build target.
- `lib/Adafruit_MLX90640_QC510/` — patched local MLX90640 library (adds
  `getRawFrame()`).
- `design-notes/tgis510_v4_14_0_speculative.cpp.txt` — discarded
  speculative draft, not built, kept for reference only (see Status above).

Everything for this project lives under `510 Quality Control System/` at
the repo root — there is no unrelated content alongside it.

## Known limitation

Single-tube-in-flight only, once tube tracking exists — not yet applicable
since there's no encoder/presence-sensor integration in this build.
