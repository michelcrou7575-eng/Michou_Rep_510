# 410 Rotaliner — Tubing Seal Seam Monitor

**Separate project from TGIS-510** (the ESP32/PlatformIO thermal glue
inspection system in `src/` at the repo root). Do not conflate the two:

| | 410 Rotaliner | TGIS-510 |
|---|---|---|
| Location | Factory floor | Home-lab / after-hours |
| Controller | Siemens S7-300 + ATmega2560 | ESP32-S3 |
| Toolchain | **SIMATIC Manager / STEP 7 (classic V5.x)** — not TIA Portal | PlatformIO / Arduino |
| Purpose | Tubing seal seam monitoring | Hot-melt glue QC on tubes |

This directory holds tracking notes for the 410 Rotaliner project only —
the actual STEP 7 source (DB/FC blocks, ATmega2560 firmware) lives in its
own project in SIMATIC Manager on the factory-floor toolchain, not in this
repo. This is a changelog/reference, not a build target.

See `CHANGELOG.md` for the block-level history.

## Blocks (as understood so far)

| Block | Role | Status |
|---|---|---|
| DB10 | HMI DB — data carried to/from the operator HMI | Added |
| FC160 | HMI Process — drives the HMI, reads/writes DB10 | Added |
| FC105 | Glue Scale Logic — glue-scale setpoint vs. actual value | Referenced, not yet confirmed added |
| Banner Auto-Trim | External device/station whose data needs to reach the HMI | Not a PLC block — an external source relaying through FC160 |

Intended data flow, as described so far:

```
FC105 (Glue Scale Logic)  --Setpoint, Actual Value-->  FC160 (HMI Process)  -->  DB10  -->  HMI
Banner Auto-Trim          --(info TBD)------------->  FC160 (HMI Process)  -->  DB10  -->  HMI
```

FC160 is meant to be the common HMI-facing path for both glue-scale data and
Banner Auto-Trim data, not a path dedicated to just one of them.

**Confirmed: FC160 does not poll.** FC105 (and Banner Auto-Trim) write into
DB10 directly; FC160's job is only to format/relay whatever is already
sitting in DB10 for the HMI. Revised data flow:

```
FC105 (Glue Scale Logic)  --writes Setpoint, Actual Value-->  DB10
Banner Auto-Trim          --writes (info TBD)------------->  DB10
                                                                |
                                                    FC160 (HMI Process)
                                                    formats DB10 for the HMI
```

### Open questions (block this being made concrete)

- **FC105 interface**: what DB/tags hold Setpoint and Actual Value today,
  and what data type (INT, REAL, scaled integer)? Does FC105 already exist
  as a block, or is it planned?
- **DB10 layout**: which offsets/tags are reserved for the glue-scale
  setpoint/actual pair vs. Banner Auto-Trim data? Is DB10 already
  structured for this, or does it need new members added?
- **Banner Auto-Trim**: what is it exactly (Banner Engineering sensor,
  a trim/cutoff station, something else), and what's its physical/logical
  interface to the S7-300 — discrete I/O, analog, or a fieldbus/serial
  link? What specific values ("info") does it need to write into DB10?

These need answering (from the SIMATIC Manager project / the actual
hardware) before this can move from "tracked requirement" to real block
logic — this repo doesn't have STEP 7 source to check against.
