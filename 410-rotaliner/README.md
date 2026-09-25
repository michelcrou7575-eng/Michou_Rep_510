# 410 Rotaliner — Tubing Seal Seam Monitor

## System context

Two machines, in sequence, on the factory floor — each getting its own
after-the-fact QC add-on since neither shipped with the QC this project
adds:

```
Paper rolls --> [ 410 TUBER ]                --> tube --> [ 510 BOTTOMER ] --> finished tube, out the factory
                 wraps layers, closes tube        forms bottoms with hot-melt glue
                 seam with white glue via
                 the Rotaliner (polyethylene
                 seal seam)
                     |
                     +-- QC add-on: this project (410 Rotaliner
                         Tubing Seal Seam Monitor, aux S7-315-2 PLC)
```

The Bottomer (510) is a *different* machine downstream of the Tuber (410)
— that's what **TGIS-510** (the ESP32/PlatformIO thermal-camera project at
the repo root) inspects: hot-melt glue application as the Bottomer forms
tube bottoms. This project (410 Rotaliner) is upstream of that, on the
Tuber, inspecting the **polyethylene seal seam** that closes the tube in
the first place, using white (cold) glue — a completely different glue
system and machine from the Bottomer's hot melt. Do not conflate the two:

| | 410 Rotaliner | TGIS-510 |
|---|---|---|
| Machine | Tuber (forms the tube, closes it with white glue) | Bottomer (forms bottoms on the tube with hot-melt glue) |
| Location | Factory floor | Home-lab / after-hours |
| Toolchain | **SIMATIC Manager / STEP 7 (classic V5.x)** — not TIA Portal | PlatformIO / Arduino |
| Purpose | Seal seam + glue/water QC the Tuber lacks | Hot-melt glue QC on tubes |

This directory holds tracking notes for the 410 Rotaliner project only —
the actual STEP 7 source (DB/FC blocks) lives in its own SIMATIC Manager
project on the factory-floor toolchain, not in this repo. This is a
changelog/reference, not a build target.

See `CHANGELOG.md` for the block-level history.

## Hardware being added

The Tuber (410) itself has no built-in QC for these — this project is an
**auxiliary PLC bolted on** to add it:

| Component | Role |
|---|---|
| Siemens S7-315-2 | Auxiliary PLC added to the Tuber. Controls/monitors: Glue InFeed, Glue Selection, Rotaliner polyethylene seal seam, and Hot Water Supply (cleaning) |
| Siemens TP177A HMI | Added to the Profibus network — status display and setpoint entry for the operator |
| Profibus | Network connecting the S7-315-2 to the TP177A |

Where the previously-mentioned ATmega2560 fits into this (a satellite
microcontroller for one of these four subsystems, presumably) is not yet
recorded — TBD.

## Blocks (as understood so far)

| Block | Role | Status |
|---|---|---|
| DB10 | HMI DB — data carried to/from the TP177A | Added |
| FC160 | HMI Process — formats/relays DB10 for the TP177A over Profibus | Added |
| FC105 | Glue Scale Logic — glue-scale setpoint vs. actual value | Referenced, not yet confirmed added |
| Banner Auto-Trim | External device/station whose data needs to reach the HMI | Not a PLC block — an external source writing into DB10 |

Confirmed data flow — FC160 does not poll; FC105 and Banner Auto-Trim write
into DB10 directly, and FC160 only formats what's already there for the
TP177A:

```
FC105 (Glue Scale Logic)  --writes Setpoint, Actual Value-->  DB10
Banner Auto-Trim          --writes (info TBD)------------->  DB10
                                                                |
                                                    FC160 (HMI Process)
                                                    formats DB10 for the TP177A
```

## Open questions (block this being made concrete)

- **FC105 interface**: what DB/tags hold Setpoint and Actual Value today,
  and what data type (INT, REAL, scaled integer)? Does FC105 already exist
  as a block, or is it planned?
- **DB10 layout**: which offsets/tags are reserved for the glue-scale
  setpoint/actual pair vs. Banner Auto-Trim data? Is DB10 already
  structured for this, or does it need new members added?
- **Banner Auto-Trim**: what is it exactly (Banner Engineering sensor,
  a trim/cutoff station, something else), and what's its physical/logical
  interface to the S7-315-2 — discrete I/O, analog, or a fieldbus/serial
  link? What specific values ("info") does it need to write into DB10?
- **Glue InFeed / Glue Selection / Hot Water Supply**: how does the
  S7-315-2 actually monitor/control each of these (I/O points, setpoints,
  what "quality control" check is being added for each)? Not yet
  described in enough detail to log block-by-block.
- **ATmega2560's role**: mentioned in an earlier summary of this project
  but not yet placed against the four subsystems above.

These need answering (from the SIMATIC Manager project / the actual
hardware) before this can move from "tracked requirement" to real block
logic — this repo doesn't have STEP 7 source to check against.
