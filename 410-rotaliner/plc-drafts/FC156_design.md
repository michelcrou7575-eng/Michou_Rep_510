# FC156 (Banner Auto-Trim Interface) — draft design

**Status: draft, minimal skeleton. Much less grounded than FC155/FC160 —
those could lean on well-known PLC idioms (analog scaling, bit packing)
regardless of unknowns. This one can't: it's a genuine guess at Banner
Auto-Trim's interface type, not just its addresses. Confirm the interface
type before trusting anything beyond the shape of this block.**

## What's actually known vs. guessed

Known (from earlier confirmations): Banner Auto-Trim needs to relay
info through the same HMI DB10 -> FC160 path as the glue scale, and
FC160's draft already reserves `DBX 8.1` (`BannerAutoTrimFault`) for it.

Everything else is unknown:
- Is Banner Auto-Trim's interface to the S7-315-2 discrete I/O
  (a fault/ready contact), analog, or serial/fieldbus?
- Is it a Banner Engineering sensor product, or a trim/cutoff station
  under a "Banner Auto-Trim" name, or something else entirely?
- Does it need to relay only a fault bit, or also measured values
  (length trimmed, position, speed — unknown)?

## This draft's assumption (the minimal, most-guessable case)

Assumes the **simplest possible interface**: a single discrete fault/ready
contact wired to one digital input on the S7-315-2, polarity unconfirmed.
This is deliberately the smallest possible guess — a placeholder skeleton
to extend once the real interface is known, not a considered design like
FC155's analog scaling (which rests on well-established PLC practice
regardless of the specific numbers involved).

**If Banner Auto-Trim actually needs analog or serial data relayed, this
draft is close to useless as-is** — it would need a completely different
shape (an analog scaling network like FC155's, or a serial parser like the
ATmega2560/CP340 link will eventually need). Worth confirming before
extending this further.

## Proposed layout addition

| Offset | Name | Type | Written by | Purpose |
|---|---|---|---|---|
| I 4.0 (placeholder) | BannerAutoTrim_FaultInput | BOOL | Banner Auto-Trim (hardwired) | Raw discrete input — guessed address and polarity |
| DBX 8.1 (HMI DB10) | BannerAutoTrimFault | BOOL | **FC156** | Already reserved by FC160's draft |

## Call location

OB1 alongside FC155 — if this turns out to be a QC-relevant fault (stops
production, not just cosmetic), it belongs on the main scan like FC155,
not FC160's relaxed OB35 cycle.

## FC156.awl

See `FC156.awl` — three lines of actual logic; the value here is mostly
in the placeholders and open questions it makes explicit, not the logic
itself.
