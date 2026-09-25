# 410 Rotaliner — Block Changelog

Tracks changes made in the SIMATIC Manager / STEP 7 (classic V5.x) project
for the 410 Rotaliner Tubing Seal Seam Monitor — **not TIA Portal**. The
actual block source lives in that SIMATIC Manager project on the
factory-floor toolchain, not in this repo — entries here are a reference
log only.

## 2026-09-25

- **HMI DB10** added — data block carrying the operator-HMI-facing
  values for this line.
- **FC160** added — function block. Confirmed purpose: **HMI Process** —
  the block that formats/relays HMI DB10 for the operator HMI.
- **FC155 (Glue Scale Logic)** — needs to work with FC160: writes
  **Setpoint** and **Actual Value** data into HMI DB10 (glue-scale target vs.
  measured reading, surfaced to the operator via FC160).
- **Banner Auto-Trim** needs to relay its info through the same path —
  writes into HMI DB10 as well, so FC160 is the common HMI-facing formatter for
  glue-scale data (via FC155) and Banner Auto-Trim data alike, not just one
  or the other.
- **Confirmed: FC160 does not poll FC155 or Banner Auto-Trim.** Both write
  into HMI DB10 directly; FC160 only formats whatever is already there for
  the HMI.
- **System context recorded**: the 410 Tuber (paper rolls -> layered,
  glue-sealed tube via the Rotaliner polyethylene seal seam) feeds the 510
  Bottomer (forms bottoms with hot-melt glue, inspected separately by
  TGIS-510) — two different machines in sequence, each with its own
  after-the-fact QC add-on.
- Confirmed hardware: an auxiliary **Siemens S7-315-2** PLC is being added
  to the Tuber to monitor/control Glue InFeed, Glue Selection, the
  Rotaliner polyethylene seal seam, and Hot Water Supply (cleaning) — QC
  the original machine lacks. A **Siemens TP177A** HMI is being added to
  the Profibus network for status display and setpoint entry, driven by
  FC160/HMI DB10.
- **ATmega2560 role confirmed**: it's an "intelligent" sensor on the
  Rotaliner seal seam, linked to the S7-315-2 over RS232 through a
  **Siemens CP340** point-to-point communication processor module — not
  direct wired I/O.
- **Draft FC160 (HMI Process)** added: `plc-drafts/FC160.awl` +
  `FC160_design.md`. Stateless formatter called from OB35, packs
  status/fault bits already in HMI DB10 into one status word plus a
  heartbeat counter — does not compute anything itself.
- **Draft FC155 (Glue Scale Logic)** added: `plc-drafts/FC155.awl` +
  `FC155_design.md`. Called from OB1 (it makes the actual QC decision, so
  it can't run on a slower cycle than FC160). Scales the glue scale's raw
  analog reading, clamps the operator's Setpoint to a safe range, computes
  Deviation, and decides GlueOutOfTolerance — FC160 only relays that bit.
  Originally drafted as FC105, renumbered to **FC155** to avoid colliding
  with Siemens' fixed standard-library SCALE block number (FC105/FC106 =
  SCALE/UNSCALE).
- **Draft FC156 (Banner Auto-Trim Interface)** added: `plc-drafts/FC156.awl`
  + `FC156_design.md`. Much more speculative than FC155/FC160 — assumes the
  smallest possible interface (one discrete fault contact) purely as a
  placeholder, since Banner Auto-Trim's real interface type (discrete,
  analog, or serial/fieldbus) isn't known yet. Relays into the
  `BannerAutoTrimFault` bit FC160's draft already reserves.
- **Confirmed: Glue InFeed's sensor is the weight scale** — the same
  physical sensor FC155 (Glue Scale Logic) already reads. Not a separate
  subsystem: FC155's GlueActualValue/GlueOutOfTolerance already is the
  Glue InFeed QC check, so no new block was needed for it.

See `README.md` for the block interface table, data flow, and remaining
open questions (HMI DB10 layout, FC155 tag addresses, Banner Auto-Trim's
real interface, ATmega2560/CP340 protocol) blocking an implementable
version of this.
