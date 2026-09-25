# FC160 (HMI Process) — draft design

**Status: draft, not verified against the real HMI DB10 layout or SIMATIC
Manager project. Syntax-check/compile in SIMATIC Manager before downloading
anything to the S7-315-2.** This repo has no access to the actual STEP 7
source, so everything below is a proposal to react to, not a known-good
implementation.

## Scope, per what's confirmed so far

FC160 is the HMI Process block: it **formats/relays HMI DB10 for the TP177A**.
It does **not** poll FC105 or Banner Auto-Trim, and it should **not**
compute glue-scale tolerance/alarm logic — that's FC105's job ("Glue Scale
Logic"). Blurring that boundary is the main design mistake to avoid here:
if FC160 starts computing things, HMI DB10 stops being a clean record of
"what FC105/Banner Auto-Trim decided" and becomes two blocks fighting over
the same data.

Given that, FC160's actual job is narrower than "process" suggests:

1. Pack whatever status/fault bits FC105 and Banner Auto-Trim already wrote
   into HMI DB10 into one compact **HMI status word**, for a single word-lamp /
   multi-state screen object rather than wiring N separate bit lamps.
2. Drive a **heartbeat counter** so the TP177A can show a live
   "PLC communication OK" indicator (increments every FC160 call; the HMI
   watches for it changing, not for a specific value).
3. Convert/scale values only if the TP177A's screen objects need a
   different representation than what FC105 wrote (see open question
   below) — e.g. a REAL engineering value where the panel's tag is
   configured as a scaled INT for a bargraph.

## Call location: OB35, not OB1

Recommend calling FC160 from **OB35** (default 100ms cycle) rather than
OB1. Reasons:
- HMI refresh has no business competing for time in the main scan-cycle
  logic that actually runs the Tuber's glue/water systems.
- A steady, bounded refresh rate is what an operator display wants anyway
  — no benefit to formatting HMI data every OB1 scan if OB1 is running
  faster than the TP177A's own Profibus poll rate.
- If OB35 isn't already configured/enabled in the hardware station, that's
  a one-time addition in SIMATIC Manager's HW Config.

## Proposed HMI DB10 additions

HMI DB10 already exists ("Added" per the changelog) with whatever FC105 needs
for Setpoint/Actual Value. The offsets below are a **proposal for what
FC160 reads and writes** — reconcile against HMI DB10's real declaration in
SIMATIC Manager before using these addresses for anything:

| Offset | Name | Type | Written by | Purpose |
|---|---|---|---|---|
| DBD 0 | GlueSetpoint | REAL | FC105 | Glue-scale target |
| DBD 4 | GlueActualValue | REAL | FC105 | Glue-scale measured reading |
| DBX 8.0 | GlueOutOfTolerance | BOOL | FC105 | Actual vs. Setpoint alarm (FC105's decision, not FC160's) |
| DBX 8.1 | BannerAutoTrimFault | BOOL | Banner Auto-Trim interface | Placeholder — real bit(s) TBD |
| DBW 10 | HMI_StatusWord | WORD | **FC160** | Packed status bits for one HMI screen object |
| DBW 12 | HMI_Heartbeat | WORD | **FC160** | Free-running counter, "PLC alive" on the TP177A |

## Open question this can't resolve without the real project

**Does the TP177A's tag configuration point directly at FC105's raw
addresses (DBD0/DBD4/DBX8.x), or at a separate HMI-only region that FC160
populates?** The draft below assumes the latter (a small dedicated
HMI_StatusWord/HMI_Heartbeat area) because it's the more common,
screen-design-friendly pattern — the HMI configuration in ProTool/WinCC
flexible can then stay stable even if FC105's internal layout changes
later. If your TP177A screens already read HMI DB10's raw addresses directly,
FC160 may only need the heartbeat — tell me and I'll trim this down.

## FC160.awl

See `FC160.awl` in this folder — plain STL (classic AWL), meant to be
pasted into a SIMATIC Manager source file, syntax-checked, and compiled
into FC160. It intentionally avoids chaining comparison instructions
directly with boolean logic (a spot where STL syntax gets easy to get
subtly wrong) in favor of individual bit assignments combined afterward —
slightly more verbose, easier to verify by eye and in the compiler.
