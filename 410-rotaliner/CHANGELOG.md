# 410 Rotaliner — Block Changelog

Tracks changes made in the SIMATIC Manager / STEP 7 (classic V5.x) project
for the 410 Rotaliner Tubing Seal Seam Monitor — **not TIA Portal**. The
actual block source lives in that SIMATIC Manager project on the
factory-floor toolchain, not in this repo — entries here are a reference
log only.

## 2026-09-25

- **DB10 (HMI DB)** added — data block carrying the operator-HMI-facing
  values for this line.
- **FC160** added — function block. Confirmed purpose: **HMI Process** —
  the block that formats/relays DB10 for the operator HMI.
- **FC105 (Glue Scale Logic)** — needs to work with FC160: writes
  **Setpoint** and **Actual Value** data into DB10 (glue-scale target vs.
  measured reading, surfaced to the operator via FC160).
- **Banner Auto-Trim** needs to relay its info through the same path —
  writes into DB10 as well, so FC160 is the common HMI-facing formatter for
  glue-scale data (via FC105) and Banner Auto-Trim data alike, not just one
  or the other.
- **Confirmed: FC160 does not poll FC105 or Banner Auto-Trim.** Both write
  into DB10 directly; FC160 only formats whatever is already there for the
  HMI.

See `README.md` for the block interface table, data flow, and remaining
open questions (DB10 layout, FC105 tag addresses, Banner Auto-Trim's actual
interface) blocking an implementable version of this.
