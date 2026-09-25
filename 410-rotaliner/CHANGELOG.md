# 410 Rotaliner — Block Changelog

Tracks changes made in the TIA Portal (STEP 7) project for the 410
Rotaliner Tubing Seal Seam Monitor. The actual block source lives in the
TIA Portal project on the factory-floor toolchain, not in this repo —
entries here are a reference log only.

## 2026-09-25

- **DB10 (HMI DB)** added — data block carrying the operator-HMI-facing
  values for this line.
- **FC160** added — function block. Confirmed purpose: **HMI Process** —
  the block that drives/services the operator HMI, reading from and writing
  to DB10.
- **FC105 (Glue Scale Logic)** — needs to work with FC160: exchanges
  **Setpoint** and **Actual Value** data with it (glue-scale target vs.
  measured reading, surfaced to the operator through FC160/DB10).
- **Banner Auto-Trim** needs to relay its info through FC160 as well —
  i.e. FC160 is meant to be the common HMI-facing path for glue-scale data
  (via FC105) and Banner Auto-Trim data alike, not just one or the other.

See `README.md` for the block interface table and open questions blocking
an actual implementation of the FC105/FC160/Banner Auto-Trim wiring.
