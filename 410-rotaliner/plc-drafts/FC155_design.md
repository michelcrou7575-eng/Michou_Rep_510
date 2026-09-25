# FC155 (Glue Scale Logic) — draft design

**Status: draft, not verified against the real HMI DB10 layout, hardware
config, or the SIMATIC Manager project. Syntax-check/compile in SIMATIC
Manager before downloading anything to the S7-315-2.**

## Naming: FC155, not FC105

This block was originally drafted as FC105, but **FC105 is the fixed block
number Siemens uses for the standard library "SCALE" function** (raw analog
INT -> scaled engineering REAL; FC106 is its companion "UNSCALE"). To avoid
any collision with that — whether or not the standard library is even used
elsewhere in this project — this custom "Glue Scale Logic" function is
numbered **FC155** instead, and owns the full job: scale the raw reading,
validate the Setpoint, compute deviation, and decide out-of-tolerance.

The scaling math below is still done inline with plain STL arithmetic
rather than by calling anything named SCALE, so this also has no runtime
dependency on the standard library being present at all.

**Confirmed: the Glue InFeed sensor is this same weight scale.** Glue
InFeed isn't a separate subsystem needing its own block — this FC's
`GlueActualValue`/`GlueOutOfTolerance` already is the Glue InFeed QC check,
not just "Glue Scale Logic" in the abstract.

## What this FC155 draft does

1. Reads the glue scale's (= Glue InFeed's) raw analog input and converts
   it to an engineering-unit REAL (`GlueActualValue`).
2. **Clamps the operator-entered Setpoint** (`GlueSetpoint`, written by the
   TP177A's Setpoint entry field) to a safe MIN/MAX range every scan — real
   QC value, not just formatting: protects the process from a bad/typo'd
   operator entry rather than trusting whatever was last typed in.
3. Computes `GlueDeviation` = Actual − Setpoint, for trending/display.
4. Decides `GlueOutOfTolerance` (Actual outside Setpoint ± tolerance band)
   — FC155 owns this decision; FC160 only relays the bit, per the
   already-confirmed boundary between the two blocks.

## Call location: OB1, not OB35

Unlike FC160 (pure HMI formatting, fine on a relaxed OB35 cycle), FC155
computes the actual QC decision (Setpoint clamping, tolerance check) — that
should run on the main **OB1** scan, not a slower time-driven OB, so a bad
Setpoint or an out-of-tolerance condition is caught as fast as the rest of
the Tuber's control logic runs, not up to ~100ms late.

## Proposed HMI DB10 layout used by this FC155 draft

Extends the layout already proposed in `FC160_design.md` without
overlapping it (bytes 8–13 are already spoken for by FC160's
`HMI_StatusWord`/`HMI_Heartbeat`):

| Offset | Name | Type | Written by | Purpose |
|---|---|---|---|---|
| DBD 0 | GlueSetpoint | REAL | TP177A writes it, **FC155 clamps it in place** | Operator's target, range-checked every scan |
| DBD 4 | GlueActualValue | REAL | **FC155** | Scaled glue-scale reading |
| DBD 14 | GlueDeviation | REAL | **FC155** | Actual − Setpoint, for trending/display |
| DBX 8.0 | GlueOutOfTolerance | BOOL | **FC155** | Consumed by FC160 (packed into HMI_StatusWord) |

## Placeholders that need real values before this is usable

None of these are known from this repo — all flagged inline in
`FC155.awl` too:

- **Raw analog input address** (`PIW 288` is a placeholder) — depends on
  which slot the glue scale's analog input module actually occupies in the
  S7-315-2's hardware config.
- **Raw signal range** (assumed the standard S7-300 bipolar ±27648 full
  scale) — depends on the analog module type and how the glue scale's
  transmitter is wired (0–10V, 4–20mA, etc.) and configured in HW Config.
- **Engineering-unit range** (assumed a placeholder 0.0–100.0 span) — the
  glue scale's actual measurement range and units (grams/min? %? something
  else?).
- **Setpoint MIN/MAX clamp limits** (placeholder 0.0/100.0) — the real safe
  operating range for the Setpoint.
- **Tolerance band** (placeholder ±5.0 engineering units) — how far off the
  Setpoint counts as out-of-tolerance for QC purposes.

## FC155.awl

See `FC155.awl` in this folder. Same conventions as `FC160.awl`: STL
(classic AWL) meant for a SIMATIC Manager source file, and out-of-tolerance
is decided via two separate threshold comparisons each assigned to their
own temp bit, combined afterward with a plain `U`/`O` — not chained
directly off a comparison's RLO, to keep it easy to verify.
