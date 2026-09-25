# 410 Rotaliner — Tubing Seal Seam Monitor

**Separate project from TGIS-510** (the ESP32/PlatformIO thermal glue
inspection system in `src/` at the repo root). Do not conflate the two:

| | 410 Rotaliner | TGIS-510 |
|---|---|---|
| Location | Factory floor | Home-lab / after-hours |
| Controller | Siemens S7-300 + ATmega2560 | ESP32-S3 |
| Toolchain | TIA Portal / STEP 7 | PlatformIO / Arduino |
| Purpose | Tubing seal seam monitoring | Hot-melt glue QC on tubes |

This directory holds tracking notes for the 410 Rotaliner project only —
the actual TIA Portal source (DB/FC blocks, ATmega2560 firmware) lives in
its own project on the factory-floor toolchain, not in this repo. This is
a changelog/reference, not a build target.

See `CHANGELOG.md` for the block-level history.
