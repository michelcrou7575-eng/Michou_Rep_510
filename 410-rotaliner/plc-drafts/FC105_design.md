# FC105 (Glue Scale Logic) — draft design

**Status: draft, not verified against the real HMI DB10 layout, hardware
config, or the SIMATIC Manager project. Syntax-check/compile in SIMATIC
Manager before downloading anything to the S7-315-2.**

## A naming ambiguity worth resolving first

**FC105 is the fixed block number Siemens uses for the standard library
"SCALE" function** (raw analog INT -> scaled engineering REAL; FC106 is its
companion "UNSCALE"). Given this block is named "Glue Scale Logic," there
are two real possibilities:

1. **FC105 already is the standard SCALE block**, just used to convert the
   glue scale's raw analog reading into an engineering value — in which
   case it has no room for anything beyond that (SCALE's parameters are
   fixed: IN, HI_LIM, LO_LIM, BIPOLAR, RET_VAL, OUT). Setpoint comparison
   and tolerance/QC logic would need to live in a *different* block.
2. **FC105 is a custom, purpose-built function** that happens to share that
   number (e.g. because the standard library isn't used in this project, or
   it was renumbered) — in which case it can own the full job: scale the
   raw reading, validate the Setpoint, compute deviation, and decide
   out-of-tolerance.

**This draft assumes (2)** — a self-contained custom FC that owns the whole
glue-scale QC job — because that's what closes the loop with the FC160
draft already in this folder (which assumes FC105 writes GlueSetpoint,
GlueActualValue, and GlueOutOfTolerance into HMI DB10). If it turns out
(1) is actually the case, tell me and this splits into two blocks: the
standard SCALE call for raw conversion, plus a new custom FC (different
number) for the Setpoint/tolerance logic.

To keep this workable regardless of which turns out to be true, **the
scaling math below is done inline with plain STL arithmetic rather than by
calling anything named SCALE** — no dependency on, or collision with,
whatever FC105 actually is in the standard library sense.

## What this FC105 draft does

1. Reads the glue scale's raw analog input and converts it to an
   engineering-unit REAL (`GlueActualValue`).
2. **Clamps the operator-entered Setpoint** (`GlueSetpoint`, written by the
   TP177A's Setpoint entry field) to a safe MIN/MAX range every scan — real
   QC value, not just formatting: protects the process from a bad/typo'd
   operator entry rather than trusting whatever was last typed in.
3. Computes `GlueDeviation` = Actual − Setpoint, for trending/display.
4. Decides `GlueOutOfTolerance` (Actual outside Setpoint ± tolerance band)
   — FC105 owns this decision; FC160 only relays the bit, per the
   already-confirmed boundary between the two blocks.

## Call location: OB1, not OB35

Unlike FC160 (pure HMI formatting, fine on a relaxed OB35 cycle), FC105
computes the actual QC decision (Setpoint clamping, tolerance check) — that
should run on the main **OB1** scan, not a slower time-driven OB, so a bad
Setpoint or an out-of-tolerance condition is caught as fast as the rest of
the Tuber's control logic runs, not up to ~100ms late.

## Proposed HMI DB10 layout used by this FC105 draft

Extends the layout already proposed in `FC160_design.md` without
overlapping it (bytes 8–13 are already spoken for by FC160's
`HMI_StatusWord`/`HMI_Heartbeat`):

| Offset | Name | Type | Written by | Purpose |
|---|---|---|---|---|
| DBD 0 | GlueSetpoint | REAL | TP177A writes it, **FC105 clamps it in place** | Operator's target, range-checked every scan |
| DBD 4 | GlueActualValue | REAL | **FC105** | Scaled glue-scale reading |
| DBD 14 | GlueDeviation | REAL | **FC105** | Actual − Setpoint, for trending/display |
| DBX 8.0 | GlueOutOfTolerance | BOOL | **FC105** | Consumed by FC160 (packed into HMI_StatusWord) |

## Placeholders that need real values before this is usable

None of these are known from this repo — all flagged inline in
`FC105.awl` too:

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

## FC105.awl

See `FC105.awl` in this folder. Same conventions as `FC160.awl`: STL
(classic AWL) meant for a SIMATIC Manager source file, and out-of-tolerance
is decided via two separate threshold comparisons each assigned to their
own temp bit, combined afterward with a plain `U`/`O` — not chained
directly off a comparison's RLO, to keep it easy to verify.
