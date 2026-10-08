# TGIS-510 firmware -- working conventions

Home-lab / after-hours project. ESP32-S3 firmware for the Thermal Glue
Inspection System lives in `src/tgis510_vX_YY_ZZ.cpp` (currently
`tgis510_v5_02_01.cpp`, i.e. V5.02.01 -- see "V5.00.00" below for how we
got here from the V4.15.x line, and "Branch consolidation (V5.02.01)"
below for how this repo moved from `claude/cpp-file-sharing-t8h5g4` to
`Main`). Read this file before making changes so standing rules and
recent context carry over across sessions.

**`Main` is the working branch as of this consolidation** --
`claude/cpp-file-sharing-t8h5g4` (V4.15.56 through V5.01.12) is now
historical only, kept for its commit history, not developed on further.

## Standing rules

- **Wait for "GO" to commit.** Never run `git commit`/`git push` until the
  user says the literal word "GO" in a message -- then do it yourself,
  directly, in this session. Editing files locally is fine any time;
  committing/pushing is not, no matter how small the change or how many
  stop-hook reminders fire about uncommitted changes. If a stop hook
  complains about uncommitted changes, that's expected while waiting for
  GO -- explain that plainly, don't commit to silence it.
  **Exception: `.md` files** (this file included) don't need a "GO" --
  commit and push those immediately after editing them. The GO/GOS gate is
  only for firmware (`.cpp`/`platformio.ini`) changes.
- **"GOS" means show, don't do.** When the user says the literal word
  "GOS" instead of "GO", don't edit the .cpp file yourself -- instead
  explain how to make the C++ modification: which function/lines to
  change, the exact code, and why, so the user can apply it themselves
  (e.g. in their own editor) rather than have this session write the file.
  This is about the firmware edit itself, not the git commit -- committing
  still only happens on a later "GO".
- **Version Adjust when you modify.** Every content change to the firmware
  bumps the version, in lockstep, all in the same edit. Current scheme
  (since V5.00.00): `VX.YY.ZZ` <-> `src/tgis510_vX_YY_ZZ.cpp`, bump `ZZ`
  for a normal change (`ZZ` wraps to `YY+1.00` at 99, bump `YY`/`X` only
  for something you'd actually call a minor/major bump). Before that, the
  scheme was `V4.15.N` <-> `src/tgis510_v4_15_N.cpp`, bumping `N`.
  1. Rename `src/tgis510_v...cpp` to the new version's filename (`git mv`,
     or a plain rename if the source came from outside this repo).
  2. Update the `// Ref: TGIS-510_cpp_V...` header comment at the top of
     the file to match.
  3. Update `platformio.ini`'s `FW_VERSION_STRING`/`FW_FILE_STRING`
     build_flags to match.
  One version bump per logical change, even if several bumps happen back
  to back before a single GO/commit -- keeps each commit message mapped to
  exactly one version number and one rationale.
- User preference: **"Mostly Automation"** -- make the reasonable
  engineering call and proceed rather than stopping to ask, unless
  genuinely blocked on a decision only the user can make (e.g. a
  functional spec, a hardware wiring choice, an explicit rule override).

- **Branch consolidation (V5.02.01)** -- The user pushed independent
  hand-edits (same pattern as V5.00.00/V5.01.00/V5.01.01 before) to a
  brand-new `Main` branch rather than this repo's working branch at the
  time (`claude/cpp-file-sharing-t8h5g4`), then asked to keep `Main` and
  consolidate everything onto it. Confirmed before moving anything: the
  firmware content on `Main` (`TGIS-510_cpp_V5.02.00`/`.01`, see next
  entry) descends from this project's own `V5.01.12`, not a separate
  line -- diff against our last `V5.01.12` was ~600 lines out of ~4300,
  concentrated in a few features, not a rewrite.
  What moved: `CLAUDE.md` (this file), the real CX-Designer `Symbol
  Table`, root `README.md`, 6 bench-test screenshots, the vendored
  `lib/Adafruit_MLX90640/` fork, and the `reference/
  HotMelt_MLX90640_80032_9_8_1.cpp` foundation file -- all copied over
  from `claude/cpp-file-sharing-t8h5g4` as-is. `Main` already had its own
  copies of the MLX90640 fork and the HotMelt reference file nested under
  `510 Quality Control System/` (a separate, self-contained PlatformIO
  subproject bundled into this repo) -- the MLX90640 fork is byte-identical
  so no conflict; the HotMelt reference file differs (an older/different
  snapshot) so both were kept rather than one overwriting the other.
  Two real fixes, not just file moves:
  1. `TGIS-510_cpp_V5.02.00` was sitting at the repo root with no `.cpp`
     extension and no `src/` folder -- PlatformIO's default `src_dir`
     only compiles recognized extensions under `src/`, so this file was
     not buildable as committed, independent of any `platformio.ini`
     question. Renamed to `src/tgis510_v5_02_01.cpp` (matching this
     file's own `// Ref: TGIS-510_cpp_V5.02.01` header, and this
     project's established `VX.YY.ZZ` <-> `src/tgis510_vX_YY_ZZ.cpp`
     convention) -- a correctness fix, not a style preference.
  2. `Main` had no root-level `platformio.ini` at all (only the nested
     subproject's own, under `510 Quality Control System/`), so there
     was no way to build the root-level firmware either way. Added one,
     based on `claude/cpp-file-sharing-t8h5g4`'s own `platformio.ini`
     specifically to carry forward its confirmed flash/PSRAM boot-fix
     (ESP32-S3FH4R2 is 4MB flash/2MB Quad PSRAM, not the devkit default
     8MB/Octal -- a real boot-time crash, not a placeholder) which the
     nested subproject's `platformio.ini` does NOT have.
  `TGIS_510_MAIN.cpp` at the repo root is a stale `V5.01.00` snapshot
  (predates `V5.02.00` by several real versions) -- confirmed superseded,
  left in place untouched rather than deleted.
- **V5.02.00/V5.02.01** -- The user's own edits on `Main`, reviewed and
  documented here after the fact (same as V5.00.00/V5.01.00/V5.01.01).
  Real changes on top of `V5.01.12`:
  - `FlagRelayTx`/`FlagRelayRx` decoupled from hardware: `service()` now
    takes a caller-supplied `now` (millisecond clock) instead of calling
    `millis()`/`digitalWrite()` directly, writing to an internal
    `wireState[3]` array instead -- portable/testable, same reasoning
    `Gray3`/`Gray3Transceiver` were split for. The index bits crossing
    the wire are now actually Gray-coded (`Gray3::ENCODE`/`DECODE`),
    where V5.01.06-09 sent raw binary.
  - New `runFlagRelaySelfTest()`: a pure-software loopback -- two
    independent TX/RX pairs simulate the ESP and PLC sides entirely in
    memory, verifying the full protocol logic without any hardware,
    printed once at boot (`[FLAG-RELAY] software loopback self-test:
    PASS/FAIL`).
  - `applyFlagRelayWireState()`/`serviceFlagRelayTransport()`: finally
    calls `flagRelayTx.service()` from `loop()`, resolving V5.01.07's
    "needs a 3rd free pin" blocker by reusing `OPTO_1`/`OPTO_2`/
    `ESP_OPTO_3` while `digitalCommsTestMode` is active (that mode
    already suspends `PlcComms::setStatus()` on those same pins, so
    there's no second writer) -- and loops the transmitted wire state
    into a local `flagRelayRx` for a live digital-loopback test. Correctly
    carries forward V5.01.11's `OPTO_2` polarity fix
    (`!flagRelayTx.wireState[1]`, `OPTO_1`/`ESP_OPTO_3` not inverted).
  - Three new diagnostics-display toggles on previously-unused lowercase
    keys (`'c'`/`'x'`/`'z'` -- distinct from the uppercase `'C'`/`'X'`/
    `'Z'` already in use, so zero collision): `'z'` gates all of
    `printDiagnostics()` (on-demand and the periodic auto-print) on/off;
    `'c'` toggles the general diagnostics block; `'x'` toggles the
    Digital Comms block (`Send_0-7`/`Receive_0-7`/Test Function status).
  - Dropped `PlcComms::testOutputState[3..7]` and their `'E'`/`'O'`/`'Q'`/
    `'T'`/`'U'` serial commands (the V5.01.03 Send_3-7 placeholders) --
    superseded now that `FlagRelayTx` has a real transport and a passing
    self-test for the same 8-flag need.
  - New PC-key path: raw serial `'1'`-`'8'` now also call
    `flagRelayTx.setFlag()` directly, but only while `digitalCommsTestMode`
    is active. **Open question, not yet resolved**: this is a second,
    independent way to reach the same 8 flags alongside V5.01.09's
    HMI-panel path (the numbered IO block, gated by `testFunctionMode`,
    freed off the PC keyboard entirely in V5.01.10) -- gated by a
    different flag (`digitalCommsTestMode` vs `testFunctionMode`), so
    not broken, but worth deciding whether both paths should keep
    existing side by side.
  - PLC side: `FC140` ("GREY_CODE_COMMS") calls `FB160`/"GREYCODE"
    (instance `DB160`, symbolic name `"COMMS RX"`) -- the STL equivalent
    of `FlagRelayTx`/`FlagRelayRx` that was offered but never written
    earlier in this project's history, now Gray-coded on both sides to
    match the ESP rework above. **Open concern, not yet verified**:
    `FB160` uses `JC`/`JCN`/`JU` jumps to labels (`TACT`, `ECYC`, `TCPH`,
    `TS00`, ...) throughout -- this project already found once that
    `LABEL` does not compile on the user's actual S7 toolchain and
    deliberately moved to a structured, flag-gated style instead (see
    the FB100/Gray-decode work); this file may hit the same wall and
    should be test-compiled before being trusted. Also unverified from
    the file alone: `FC140`'s call references the instance DB as
    `"GREYCODE DB"`, but `DB160`'s own symbolic name here is
    `"COMMS RX"` -- that binding lives in the project's own Symbol
    Table, not in these files.
- **V5.01.12** -- Answers the "remove Toggle on 1-A buttons" question from
  V5.01.09/.10: the user clarified they want the numbered IO block
  (`'1'`-`'A'`, IO1-7/OPTO_1-3) to be **momentary** -- `kDiagLampAutoReset[5..14]`
  flipped from `false` to `true`, so each one's `$B(555-564)` lamp now
  flashes ~1s to confirm the press then auto-resets (`activateDiagLamp()`),
  same as the 17 one-shot diag buttons, instead of toggling and staying
  latched to mirror the output. Lamp-only change -- `toggleMcpOutput()`/
  `PlcComms::toggleTestBit()`/`flagRelayTx.setFlag()` (while Test Function
  is ON) are untouched, so the real LED/OPTO pin/flag still toggles and
  holds exactly as before; only the on-screen confirmation is momentary
  now. Explicitly kept toggled (latched, `kDiagLampAutoReset=false`),
  per the user's own list: `$B65`/`$B565` (KEYENCE_TRIG, a persistent
  bench-test-pulse mode since V5.01.01) and `$B77`/`$B577`
  (MLX_LIVE_TOGGLE, a persistent streaming mode) -- both already were,
  no change needed, just confirmed unaffected. `$B79`/`$B579` (Test
  Function, V5.01.09) isn't part of this array at all (standalone, its
  own `testFuncLampState`) -- also confirmed unaffected, stays toggled.
- **V5.01.11** -- V5.01.08 was wrong to assume `OPTO_1` and `OPTO_2`
  share one polarity just because they're the same 24V/220ohm MCP-driven
  circuit type -- the user confirmed `OPTO_2` is inverted after that fix
  (which had stripped the `!` from both). Real hardware: `OPTO_1` is
  active-high, `OPTO_2` is active-low. `PlcComms::setStatus()`/
  `toggleTestBit()` now write `OPTO_1` directly and `!OPTO_2` --
  `ESP_OPTO_3` (never inverted) is unaffected.
- **V5.01.10** -- Freed all 28 diag-button serial commands off the PC
  keyboard, per the user's explicit request after they questioned why
  `'S'` was needed when the HMI's own `ST-BY` button already does the
  same thing (it does -- `applyDiagButtonUpdate()` calls
  `handleSerialCommand(kDiagCommandChars[i])` for every diag button, so
  the two were always the same code, just two entry points). `loop()`'s
  raw `Serial.available()` read now checks a new `isSerialOnlyCommand()`
  before forwarding a keystroke to `handleSerialCommand()` -- anything
  not on that 9-command list (`'Z'`/`'0'`/`'#'`/`'E'`/`'O'`/`'Q'`/`'T'`/
  `'U'`/`'Y'`) gets a `"[SERIAL] ignored -- use the HMI TEST screen"`
  message instead of dispatching, leaving those 28 letters/digits
  reachable from the HMI panel only. Deliberately did NOT free the 9:
  each has zero HMI/panel equivalent at all (`'Z'`/`'0'`/`'Y'` are bench/
  calibration tools that never got a button; `'#'`/`'E'`/`'O'`/`'Q'`/
  `'T'`/`'U'` are the FlagRelayTx/Send_3-7 bench-test commands from
  V5.01.03/V5.01.07) -- blocking those too would make them permanently
  unreachable, not just redundant, so they still answer the USB serial
  monitor directly. `handleSerialCommand()` itself, and
  `applyDiagButtonUpdate()`'s internal call into it, are unchanged --
  this only gates the raw-keystroke entry point in `loop()`. Also
  resolves the keyspace pressure that forced `'#'` into a multi-char
  prefix back in V5.01.07, if more serial-only bench commands are ever
  needed.
- **V5.01.09** -- New `$B79`/lamp `$B579` HMI button, "Test Function" --
  user-added (not yet in the committed Symbol Table), same +500 lamp-offset
  pattern as the 28 diag buttons but standalone (its address doesn't fall
  in `kDiagButtonNames[28]`'s contiguous block), same precedent as
  `BUTTON_ACK_ADDR`/`BUTTON_FAIL_ADDR`. From the user's own request: since
  the HMI TEST screen already has a numbered IO block (buttons 1-10,
  serial `'1'`-`'A'`), no need for separate `FlagRelayTx` bench-test
  buttons -- just give that block a second mode. New persistent
  `testFunctionMode` flag, flipped by `applyTestFuncButtonUpdate()` (gated
  behind the TEST screen's own Enable toggle, same reasoning as the 28
  diag buttons) and mirrored to the `$B579` lamp (latched, not
  auto-reset -- it's a mode, not a one-shot). The old separate `case '1'`-
  `'7'`/`'8'`/`'9'`/`'A'` blocks in `handleSerialCommand()` are now one
  combined block branching on `testFunctionMode`: OFF (default) is
  unchanged (IO1-7 MCP LED test + OPTO_1/2/3 raw TO_PLC_COMM bit test); ON
  repurposes `'1'`-`'8'` as COMMS Test 1-8 (`flagRelayTx.setFlag()` for
  flags 0-7 -- an exact fit, resolving V5.01.07's "flag index 7 has no
  free button" gap), `'9'`/`'A'` spare. Supersedes the earlier "do both
  actions on the same button" idea floated for this problem -- once the
  user pushed back on "you lose the LED-wiring check" as a false
  dichotomy (correctly: `toggleMcpOutput()` and `flagRelayTx.setFlag()`
  touch disjoint state, so nothing stopped both firing from one press),
  the user's own follow-up request replaced that with this cleaner
  mode-switch design instead, which also fixes the button-count mismatch
  that the "both at once" idea left open. `printDiagnostics()` ('D') now
  also prints the current Test Function mode and what buttons 1-A do in
  it.
- **V5.01.08** -- Fixed `PlcComms::setStatus()`/`toggleTestBit()` driving
  `OPTO_1` (and `OPTO_2`, same 24V/220ohm MCP-driven circuit, same code
  pattern -- fixed together as one root cause, though the user's own
  report named only `OPTO_1`) inverted (`!testOutputState[...]`) while
  `ESP_OPTO_3` was not -- confirmed a real bug per the user's report,
  following up on V5.01.07-era's "why does OPTO_1 turn on after reset"
  question that first surfaced the inconsistency. Both now write
  `testOutputState[...]` directly, matching `ESP_OPTO_3`'s polarity.
- **V5.01.07** -- `'#'` serial command: a minimal bench-test parser for
  `FlagRelayTx::setFlag()`, from a real bug the user hit trying to bench
  test V5.01.06 -- they typed `"setFlag(3, true)"` straight into the
  serial terminal, expecting it to invoke the new API, and instead got
  `[IO-TEST] MCP output #3 (pin 10) -> HIGH`: the `'3'` in that string
  hit the *existing* `case '3'` (IO3 toggle), because
  `handleSerialCommand()` dispatches one character at a time with no
  function-call parser at all. Real, demonstrated risk, not
  hypothetical: typing free text into the serial terminal right now
  silently triggers whatever existing single-char command matches each
  character in it.
  Fix: `'#'` + index digit (0-7) + value digit (0/1), e.g. `"#31"` =
  `setFlag(3, true)`, spanning multiple `handleSerialCommand()` calls
  (one per keystroke) via a small persistent state machine
  (`FlagTestState`), non-blocking. `'#'` as the prefix resolves the
  "every A-Z/0-9 slot is taken" problem flagged in V5.01.06's own
  bench-test-procedure discussion: confirmed safe by reading the switch's
  `default: break;` -- it silently ignores anything without a case, and
  nothing in the switch matches a non-alphanumeric character, so a
  symbol prefix has zero collision risk against the other 36 commands,
  no need to sacrifice an existing letter.
  New global `FlagRelayTx flagRelayTx;` instance -- but deliberately
  *not* calling `.service()` from `loop()` yet, since that needs real
  GPIO numbers to actually transmit and the 3rd free pin needed for the
  bench loopback test (only `GPIO10`/`GPIO11` are verified free from
  this file's own `Pins` comments) still isn't confirmed. `'#NV'` lets
  `setFlag()`/the pending-queue logic (`desired[]`/`committed[]`/
  `pending[]`) be exercised and observed right now regardless, printed
  back immediately on each command -- unblocks bench testing the queuing
  layer without needing the pin question settled first.
- **V5.01.06** -- New `FlagRelayTx`/`FlagRelayRx` structs -- a genuinely
  different protocol from `Gray3`, not an extension of it, for a
  genuinely different requirement the user clarified: 8 independent
  flags, any subset toggleable at any time, each side's array mirrored
  bit-for-bit on the other (not "exactly one active state out of 8",
  which is what `Gray3` actually solves). Worked through why `Gray3`
  can't safely carry this before building anything: a 3-bit Gray cube has
  degree 3 -- each of its 8 states has only 3 single-bit-away neighbors,
  not 7 -- so the single-bit-transition guarantee only holds for values
  stepping through the ring in its own fixed order (true for `PlcStatus`,
  verified against its real transition paths in V5.01.02), not for
  flags that can change in arbitrary order. Confirmed with the user
  (asked "what system is best", given two real options with their
  tradeoffs: reserve 1 of 8 codes as an IDLE marker for a simpler
  single-frame scheme at the cost of only 7 usable flags, or a 2-frame
  scheme keeping all 8) -- picked the 2-frame option ("Option 2") since
  the user explicitly wanted all 8, not 7.
  Protocol: every flag change is one 2-frame event. Wire 2 is a parity
  bit flipping on every frame (both halves of every event) -- the
  self-strobe, same principle as `Gray3`, extended across 2 frames.
  Frame A carries `index[1:0]` on wires 0-1; Frame B carries `index[2]`
  on wire 0 and the flag's new VALUE on wire 1 -- an authoritative SET,
  not a blind toggle, chosen specifically so a lost event for one index
  self-heals the next time that same flag genuinely changes, rather than
  leaving the two sides permanently desynced with no way to tell which
  bit is wrong (flagged as a real risk of a naive toggle-only version
  before deciding against it). Framing is by strict A-then-B protocol
  alternation, not a self-describing tag bit (there wasn't a spare bit
  for one without sacrificing the value bit) -- cold start syncs to
  whatever frame arrives first as "A", same precedent as FB100's "accept
  the first sample outright, no fault possible yet" from the Gray-decode
  PLC-side work.
  Separate from `Gray3Transceiver` (not built on top of it) since the
  framing is fundamentally different; same non-blocking,
  `millis()`-based, caller-owned-struct, portable/pin-agnostic style.
  Still not wired to real pins or to `PlcComms`/`FROM_PLC_COMM` -- same
  as `Gray3`, that attribution is still the user's own call once this is
  bench-tested. PLC-side STL equivalent described in chat, not committed
  to this repo (separate S7 program, same as `FC56`/`FB100`).
- **V5.01.05** -- New `Gray3Transceiver` struct: the hardware-facing
  settling-delay wrapper around `Gray3::encode()` from a follow-up
  question about the user's own "2-frame transceiver"/`PIN_TX_D0`/
  `PIN_TX_D1`/`PIN_TX_STR`/`q_Tx_STR` description, worked through rather
  than taken at face value since parts of it didn't add up: "packing 8
  discrete booleans into a 3-bit integer (0-7)" is exactly `Gray3`'s
  existing one-hot encode (no redesign needed there), and a literal 2
  data wires + 1 dedicated strobe wire is mathematically impossible to
  rule in -- 2 bits only carry 4 values, not the 8 needed, and there's no
  spare 4th wire anyway (confirmed stuck at 3 each side). So `PIN_TX_STR`
  is read as the *software-side* "data is now settled" event, not a
  physical pin -- consistent with "no wiring" being the explicit reason
  `Gray3` stays pure/pin-free (V5.01.04): the settle timer can't live
  inside a portable, pin-agnostic codec, so it lives in this new,
  separate, caller-owned struct instead (not a namespace singleton, so
  more than one independent Gray3 link could run at once without sharing
  state). `beginTransmit()` writes the encoded pattern to 3
  caller-supplied pins and starts a `millis()`-based settle timer;
  `settled()` polls it. Deliberately non-blocking (no `delay()`, despite
  the user's literal "`delay(5)`" suggestion) to match this file's own
  standing non-blocking design elsewhere -- flagged as a conscious
  deviation, not silently substituted, since a real blocking
  `delay(SETTLE_MS)` would also work fine if a given call site truly has
  no other timing to protect. Still not wired to real pins -- same as
  `Gray3` itself, that's still the user's own call to make once tested.
- **V5.01.04** -- New `Gray3` namespace: a generic, standalone 3-bit
  Gray-code encoder/decoder (`encode()`/`decode()`), deliberately
  decoupled from `PlcComms`/`PlcStatus`/any physical pin -- the user
  wants to attribute `In0-7`/`Out0-7` to real signals themselves, only
  after testing the codec's reliability in isolation. Same verified table
  (`0,4,6,2,3,7,5,1` / `0,7,3,4,1,6,2,5`) as `PlcComms::GRAY3_ENCODE`
  (V5.01.02), factored out rather than duplicated with new values, so
  this and the PLC-side equivalent (`FC56 "DIGITAL COMMS"`, rewritten the
  same session to the same generic shape: `In0-7` one-hot -> `TxBit0-2`
  Gray-coded; `RxBit0-2` Gray-coded -> `Out0-7` one-hot, no `DB56`/FM350
  wiring baked in) are guaranteed to decode what the other encodes.
  `encode()`'s documented precondition is exactly one of `in[]` true
  (one-hot) -- flagged in both the C++ comment and FC56's STL header that
  the two sides currently handle a violation of that differently (C++
  picks the lowest-index true entry and ignores the rest; the STL version
  ORs together every simultaneously-true input's pattern instead), since
  nothing should rely on either behavior rather than just keeping inputs
  one-hot. Not wired into anything yet -- no caller, no version bump to
  `PlcComms`/`setStatus()`, by design, until it's been tested standalone.
- **V5.01.03** -- Bench-only "digital comms test mode", from a bring-up
  question about how to test the 3-bit comms link from both sides. The
  user built a PLC-side `DATA_BLOCK "DIGITAL COMMS"` (DB56) with 8
  `Send_0-7` / 8 `Receive_0-7` BOOLs for 1:1 bench comparison, then asked
  for the ESP-side equivalent "while stopping the actual one" (the real
  PLC_STATUS auto-output), then separately asked about pausing most ESP
  functions while the test runs. Both landed together:
  - `PlcComms::testOutputState[3]` grew to `[8]` (`Send_0-7`, matching
    DB56's naming). `toggleTestBit()` extended to bits 0-7: bits 0-2 still
    drive the real pins (unchanged -- `'8'`/`'9'`/`'A'`), bits 3-7 are
    array-only placeholders (new `'E'`/`'O'`/`'Q'`/`'T'`/`'U'` serial
    commands, no `$B` address, no physical pin -- confirmed staying at 3
    wires each side, so these are just ready for if that ever changes).
  - New `digitalCommsTestMode` flag, toggled by `'0'` (serial-only, no
    `$B` address, like `'Z'`), enterable only from `Standby`/`FaultStop`
    (same precedent as `scanMcpStatusLeds()`) since it suspends real
    subsystems: the tube-inspection pipeline (encoder/presence/position
    tracking, Keyence triggering, MLX90640 capture/processing, Word-Lamp
    matrix pacing/telemetry) and the `PLC_STATUS` auto-output, so manual
    `Send_0-7` commands have `TO_PLC_COMM` to themselves without
    production logic fighting them on the next `loop()` iteration --
    exactly the class of two-writers-one-array bug V4.15.71 already had to
    fix once, avoided here by construction instead of by caution.
    Deliberately NOT suspended: `esp_task_wdt_reset()`, the HMI header
    status bits ($B0/$B1/$B80 -- an operator watching the HMI should still
    see the ESP as alive, not faulty, during a bench test), HMI
    button/screen polling, and `ns12.service()` (the NS12 transport pump
    itself, since HMI polling still needs it running).
  - `printDiagnostics()` ('D') now prints `Send_0-7`/`Receive_0-7` by the
    same names as DB56, for direct side-by-side comparison with a PLC
    variable-table watch during bring-up. `Receive_0-2` mirror
    `plcAcknowledge`/`plcMachineRunning`/`tubeIsBad`; `Receive_3-7` are
    placeholder `false` (no physical input pin yet, same as `Send_3-7`).
- **V5.01.02** -- Gray-coded `TO_PLC_COMM` (`PLC_STATUS`, `OPTO_1`/
  `OPTO_2`/`ESP_OPTO_3`) instead of raw binary, from a design discussion
  about getting more usable signal out of a 3-wire link that's staying at
  3 wires each side (confirmed -- no more physical PLC I/O headroom on
  either end for now). `GRAY3_ENCODE[8]` in `PlcComms` maps each
  `PlcStatus` value to a pattern where every actually-possible transition
  changes exactly one physical line, verified against this file's own two
  transition paths rather than assumed: the diag `'P'` button
  (`$B66`/`handleSerialCommand()`) cycles STOP->ALARM->WARNING->READY->STOP
  one step at a time (Gray-ring-adjacent by construction), and production
  logic (`loop()`) jumps directly between STOP and READY skipping
  ALARM/WARNING -- checked separately, and it happens to also be single-bit
  with this table. Ring positions 4-7 are spare, unused, for a future
  `PlcStatus` value if one's ever added. Payoff: the PLC no longer needs a
  dedicated "data valid" strobe bit to safely sample `PLC_STATUS` -- any
  detected line change on OPTO_1/OPTO_2/ESP_OPTO_3 is inherently the
  strobe, and a decoded pattern more than 1 bit away from the last one is
  detectable on the PLC side as a missed update/line glitch rather than a
  real state, which the previous raw-binary encoding couldn't tell apart
  from a legitimate value. Only `setStatus()` changed -- `toggleTestBit()`
  (the `'8'`/`'9'`/`'A'` bench-test overrides, `$B62-$B64`) deliberately
  stays raw/per-pin, since it exists to flip each physical line in
  isolation for wiring verification; patterns produced while using it
  aren't meant to be valid Gray/PLC_STATUS codes.
  Considered and rejected applying the same treatment to `FROM_PLC_COMM`
  (`ACKNOWLEDGE`/`MACHINE_RUNNING`/`tubeIsBad`) even though it's confirmed
  not yet wired on the PLC side (so no compatibility concern either way):
  Gray coding's single-bit guarantee only holds for one sender stepping a
  single value through defined, sequenced states. Those three are
  independent facts that can each change on their own schedule (the PLC
  could raise `RUNNING` and `ACKNOWLEDGE` in the same scan cycle, with no
  "adjacent step" relationship between them) -- no bit assignment fixes
  that, since the problem isn't the encoding, it's that there's no single
  state machine to encode. Left as three independently-debounced flags,
  which `servicePlcControl()` already handles correctly.
  The PLC side isn't implemented here (separate S7 program) -- it needs a
  matching Gray-decode + Hamming-distance-1 validity check before this
  buys anything; until then the PLC would need to keep decoding these 3
  bits as if they were still raw binary, which no longer matches what the
  ESP drives.
- **V5.01.01** -- Ported the `KeyenceTrigger` upgrade from the user's
  `TGIS-510_cpp_V5.01.00` (same source as V5.00.00: their own edits,
  pushed to `claude/510-bottomer-hot-melt-monitor-3rq1ao` by mistake).
  `fire()` is now a thin wrapper over a new `fireFor(widthUs)`, which
  takes a custom pulse width and returns whether it actually fired (false
  if already pending) -- production triggering
  (`serviceTubePositionTracking()`) still calls plain `fire()` and is
  unaffected. Repurposed `case 'K'` (`$B65`, the KEYENCE_TRIG diag
  button): it no longer fires one manual 500us pulse -- it now toggles a
  periodic bench-test pulse (100ms wide, every 500ms) via the new
  `serviceKeyenceTriggerTest()`, long enough to see/hear/scope without a
  real tube pass. Flipped `kDiagLampAutoReset[15]` (KEYENCE_TRIG) from
  `true` to `false` to match -- it's a persistent toggle now, not a
  one-shot confirm-and-reset action (also fixed that table's stale
  `OPTO_1/2/3` comment labels for indices 12-15, which actually cover
  IO8/IO9/YEL_LED_TEST/KEYENCE_TRIG). The version number was the user's
  explicit call (V5.01.01), not derived from V5.00.01 + V5.01.00 by the
  usual bump rule.
- **V5.00.01** -- First step toward the V5.00.00 header brainstorm's
  velocity-dependent processing mode: that's blocked on a real
  `ENCODER_COUNTS_PER_MM` (still `1.0f`, a placeholder), which needs an
  actual bench/field measurement, not a guessed constant. Added
  `EncoderTracker::resetTotal()` and a new calibration-only serial
  command `'Z'` (not a `$B` diag button -- no panel address) that zeroes
  the running encoder total. Procedure: send `'Z'`, feed a tube of known
  length through the presence sensor, send `'D'`, divide the raw encoder
  count by that length in mm to get the real `ENCODER_COUNTS_PER_MM`.
- **V5.00.00** -- New reference baseline, replacing the V4.15.x line. The
  user took V4.15.73, kept working on it in their own editor (Allman
  braces throughout, not this file's K&R style) on a different branch
  (`claude/510-bottomer-hot-melt-monitor-3rq1ao`, file
  `TGIS-510_cpp_V4_15.74`, pushed there by mistake), and asked for it to
  be merged in here and renamed V5.00.00 -- a deliberate major bump, not
  a continuation of V4.15.x's patch counter. Confirmed before merging:
  it already contains everything through V4.15.73 verbatim (comments
  match word-for-word), plus real new work on top:
  - `setButtonStatusLed()` no longer drives the physical MCP LED at all
    for the 5 SETUP-group buttons -- they now drive *only* the `$B4x` HMI
    lamp. Resolves the pin-sharing caveat flagged in V4.15.71 (IO1-IO5
    bench tests could desync the enclosure LED from the Enable state) by
    removing the shared write entirely, consistent with "all 7 MCP LEDs
    are bench-test-only."
  - New `refreshFunctionButtonLamps()`: re-sends all 5 button lamps after
    every toggle, so a stale bit from a screen change or mutual-exclusion
    reset can't linger.
  - New `clearFunctionButtonStates()`, wired into `$W50` (current screen
    number) change detection: switching HMI screens now disarms all 5
    Enable toggles and refreshes their lamps. `$W50` was tracked since
    early in this file's history but never acted on until now.
  - Extensive new header brainstorming on a velocity-dependent processing
    mode: raw-delta below 60 m/min (today's only path), calibrated
    Celsius from 60-200 m/min (`mlx.getFrame()` vs `getRawFrame()`), with
    Strip1/Strip2 reframed as Outer/Inner. Not yet implemented --
    `ENCODER_COUNTS_PER_MM` is still a placeholder, so there's no real
    speed estimate to switch on yet, and Celsius mode needs its own
    thresholds (today's raw-delta thresholds don't carry over). Read the
    file's own header for the full brainstorm before starting this.
  Kept as-is (not reformatted to this file's usual K&R/blank-line style)
  to avoid introducing bugs while adopting 3556 lines of unreviewed
  content sight-unseen beyond the diff above -- reformat later if wanted.
- **V4.15.73** -- Added trailing `// ledPin -> ON/OFF` comments on the
  actual `digitalWrite`/`mcp.digitalWrite` calls for the test LEDs
  (`scanMcpStatusLeds()`'s sweep loop, `toggleMcpOutput()`), per the
  user's clarification of the earlier address-comment request.
- **V4.15.72** -- All 7 MCP LEDs are bench-test-only per the user (no
  fixed meaning), so added `case 'Y'` to `handleSerialCommand()`: runs
  `scanMcpStatusLeds()` (previously boot-only) on demand for
  troubleshooting. Gated to `Standby`/`FaultStop` only -- it blocks
  ~2.8s on `delay()`, which would eat real-time budget mid-tube-pass.
  Note in code: it leaves each LED LOW when done, which can desync the
  physical LED from whatever a SETUP-group Enable toggle last set it to
  (they share `kMcpOutputPins[0..4]`) -- re-press that screen's Enable
  button to resync. Also added a `$B<addr> / lamp $B<addr>` comment on
  every `case` in `handleSerialCommand()` per the user's request, and
  fixed a stale comment above `scanMcpStatusLeds()` that still described
  the TEST/DIAG one-shot side effects removed in V4.15.69.
- **V4.15.71** -- **Root-cause fix** for the "$B43 not following $B33" /
  "IO4 affects TEST" reports: the 5 SETUP-group screen-Enable buttons and
  the 9 numbered IO-test toggles ('1'-'9' on the TEST screen) were both
  reading/writing `mcpOutputState[0..4]` -- same array, overlapping
  indices, two unrelated features. Pressing IO1-IO5 (`toggleMcpOutput()`)
  silently flipped the exact slots `setButtonStatusLed()`/
  `applyButtonBitUpdate()`/`BUTTON_TEST_INDEX`'s gate/`applyAckButtonUpdate()`
  read as "is SETUP/ALARM_LOG/TREND_FULL/TEST/DIAG armed", corrupting the
  screen-enable state and desyncing the `$B4x` lamps from it. Added a
  separate `buttonEnableState[NS12::BUTTON_COUNT]` array for the
  SETUP-group only; `mcpOutputState[9]` is now exclusively the numbered
  IO-test toggles' state. The physical LED confirmation still shares
  `kMcpOutputPins[0..4]` with those same IO tests (same enclosure LEDs) --
  that pin-sharing is unchanged and intentional, only the logical state
  that drives the `$B4x` lamp and the enable gate is now independent of
  it. Also dropped `toggleButtonStatusLed()`'s stale `!mcpOk` early-return,
  which had been silently undoing V4.15.66's "send the lamp even if MCP
  is down" fix.
- **V4.15.70** -- `printDiagnostics()` now prints each SETUP-group
  button's current Enable state by name (`mcpOutputState[0..4]`,
  `$B30-34`), to help debug a reported problem with those toggles from
  the terminal.
- **V4.15.69** -- Three fixes from real hardware testing:
  - Added the `$B38` FAIL button (top-right, distinct from Acknowledge):
    only clears `tubeIsBad`/`$B2`, leaves `FaultStop` and the screen
    Enable toggles alone. No dedicated lamp (not in the Symbol Table).
    Caveat noted in code: `tubeIsBad` is level-driven from the PLC's own
    bit 2 every poll, not edge-triggered, so if the PLC hasn't also
    dropped its signal, the next poll re-asserts it -- this is a local
    display clear, not a guarantee.
  - Removed `handleHmiButtonPress()`'s old `pushTestPattern()`/
    `printDiagnostics()` side effects on the `TEST`/`DIAG` SETUP-group
    buttons -- now that those buttons are screen Enable toggles (not
    one-shot actions), firing the old action on every arm/disarm
    double-triggered what's already reachable from the TEST screen's own
    `TEST_PATTERN`/`DIAGNOSTICS` grid buttons.
  - `sendWB()` now retries once on a partial write and logs
    addr/count/bytes-sent on failure, same fix as `sendWM()` got in
    V4.15.59 -- a dropped WB is exactly what leaves a button/lamp LED
    stuck showing the wrong state.
- **V4.15.68** -- Resolves V4.15.67's open `$B2` question: the PLC now
  relays Keyence's pass/fail result on `FROM_PLC_COMM` bit 2
  (`Pins::ESP_INPUT_3`), per the user's explicit call. Renamed
  `plcControlBit2` -> `tubeIsBad` throughout; `servicePlcControl()`'s
  existing debounce for that bit now also mirrors it straight to `$B2`
  ("this Tube is BAD") on change. Polarity (HIGH = bad/fail) is this
  file's assumption, flagged as unconfirmed against the S7 program, same
  as the other FROM_PLC_COMM/TO_PLC_COMM bit-to-pin choices.
- **V4.15.67** -- Wired 3 of the 4 header status bits the operator
  specified: `$B0` ("ESP Loop() is running") blinks every 500ms to prove
  liveness; `$B1` ("ESP Faulty") mirrors `state == FaultStop`; `$B80`
  ("Overall Alarm... if healthy = OFF, no latch") is a non-latching OR of
  every subsystem failure the ESP currently tracks (`FaultStop`, `!mcpOk`,
  `!mlxInitialized`) -- NS12/PLC comms don't have a live-health tracker
  yet, so they aren't in that OR until they do. `$B2` ("this Tube is BAD")
  was left unwired at the time (see V4.15.68 above for how it resolved).
- **V4.15.66** -- User uploaded the real CX-Designer Symbol Table
  (`Symbol Table`, project 510_HotMel_20260902_1) -- confirms every
  address this file already assumed ($B30-34/40-44 SETUP-group buttons +
  lamps, $B50-77/550-577 diag buttons + lamps, $W50/700-827/828-830) is
  exactly right, and reveals several previously-unwired ones:
  - `$B39`/`$B49` = Acknowledge button/lamp -- added as a standalone,
    independently-polled momentary button (not part of the SETUP-group's
    mutually-exclusive bank). Per the operator's spec, ACK now clears
    `FaultStop` AND disarms every screen's Enable toggle back to
    read-only (a full reset-to-safe-state), with its lamp flashing 1s to
    confirm receipt (same pattern as the 17 momentary diag-button lamps).
  - `$B40-44` "SETUP/ALARM LOG/TREND FULL/TEST/DIAG Button LAMP" -- these
    addresses were already declared (`LAMP_SETUP_ADDR` etc.) but never
    used. `setButtonStatusLed()` now also sends a WB to the matching
    on-screen lamp bit, in addition to the physical MCP LED it already
    drove -- both update together now.
  - **Still unwired, needs a spec before implementing**: `$B38` "Fail
    Button" (distinct from Acknowledge -- what should it do?), and
    `$B0`/`$B1`/`$B2`/`$B80` "Power/Fault/Fail/Alarm Status" (read as
    outputs the ESP should drive to the header status icon on every
    screen, but which bit means what isn't specified yet).
- **V4.15.65** -- Per the operator's spec: of the 28 TEST-screen buttons,
  the first 17 in panel order (state buttons + all named actions except
  `MLX_LIVE_TOGGLE`) are one-shot commands where a persistent lamp would
  misleadingly suggest an ongoing state -- their lamp now lights on press
  purely to confirm the ESP saw it, then auto-resets ~1s later
  (`kDiagLampAutoReset[]`, `activateDiagLamp()`, `serviceDiagLampAutoReset()`).
  The other 11 (numbered IO block + `MLX_LIVE_TOGGLE`) address real
  persistent outputs, so those still toggle and stay latched as before.
- **V4.15.64** -- Every non-MAIN HMI screen has its own "Enable" button
  (same name as the screen, e.g. `TEST`, `SETUP`, `DIAG`) whose latched
  state (already tracked by `toggleButtonStatusLed()`/`mcpOutputState[]`
  since V4.15.61) doubles as that screen's arm/disarm switch: off, the
  screen is read-only ("just a screen display"); on, its own controls
  respond. Added `BUTTON_TEST_INDEX` and gated `applyDiagButtonUpdate()`
  (the 28 TEST-screen buttons) behind `mcpOutputState[BUTTON_TEST_INDEX]`
  -- a press is ignored and logged while TEST isn't enabled. SETUP/
  ALARM_LOG/TREND_FULL/DIAG have no controls of their own yet, so nothing
  to gate there today; the same pattern (index into `mcpOutputState[]`)
  applies once they do.
- **V4.15.63** -- Panel button 10 (was labeled "OPTO3") no longer toggles
  `ESP_OPTO_3` directly -- that pin is now a live `TO_PLC_COMM` bit, not a
  free test output. Renamed `kDiagButtonNames[14]` to `"YEL_LED_TEST"` and
  `case 'A'` in `handleSerialCommand()` now toggles `McpPin::ELED_Y`
  instead (via the existing `toggleMcpOutput(6)`). Note: this duplicates
  IO7 (`'7'`), which already toggles the same LED.
- **V4.15.62** -- User added a status/lamp bit per diag button on the HMI
  side, `$B550-$B577`, offset +500 from each button's own `$B(50-77)`
  address -- gives the 28 diag buttons (which have no physical LED, unlike
  the 5 SETUP-group buttons) a lamp on the panel itself. Added
  `NS12::DIAG_LAMP_BASE_ADDR` (computed as `DIAG_BUTTON_BASE_ADDR + 500`,
  not hardcoded, so it tracks the base address if that ever moves),
  `diagLampState[]`, and `toggleDiagLamp()` -- flips the lamp once per
  rising edge of the button's momentary press, independent per button (no
  mutual exclusion, since these are 28 separate one-shot actions, not a
  mode selector like the SETUP-group bank). Wired into
  `applyDiagButtonUpdate()` alongside the existing `handleSerialCommand()`
  call.
- **V4.15.61** -- `ELED_Y` moved from GPB7 (15) to GPB3 (11) to match the
  user's confirmed pin layout, freeing GPB7. Added `toggleButtonStatusLed()`:
  the 5 SETUP-group HMI buttons ($B30-$B34) report a *momentary* "button is
  pressed" bit, not a latching switch -- mirroring the LED straight to that
  bit meant it only stayed lit while a finger was on the touchscreen.
  `applyButtonBitUpdate()` now toggles the LED once per rising edge instead,
  reusing the existing `mcpOutputState[]` array rather than adding new
  state. Built generically off `NS12::BUTTON_COUNT`/`kButtonAddrs`/
  `kMcpOutputPins` so any future button added to those arrays gets the same
  latching-LED behavior for free, no new code needed.
- **V4.15.59** -- `sendWM()` now retries once immediately on a partial
  `Serial2.write()`; if still short, logs `addr`/`count`/bytes-sent so a
  recurring blank column on the HMI Word-Lamp matrix can be matched to a
  specific dropped write instead of just the aggregate `wmFailures` count.
  Added `matrixPushCounter`, pushed to `$W830` (`bandAddr+2`, computed off
  the band address rather than hardcoded so it stays clear of the matrix's
  own pixel range in experimental 32x24 mode too) -- increments once per
  fully-completed matrix push, so a blank column can be diagnosed from the
  panel side: if the counter keeps incrementing but a column stays blank,
  the ESP genuinely finished sending and the fault is on the PT, not a
  dropped ESP write.
- **V4.15.57** -- Debounced `servicePlcControl()`'s 3 `FROM_PLC_COMM` bits
  (ACKNOWLEDGE, MACHINE_RUNNING, bit 2): a bit is only accepted once it
  reads the same on two consecutive ~20ms polls. Guards against relay
  chatter/line noise, and against the PLC's own multi-bit output codes not
  switching atomically (e.g. `ALARM(001)->WARNING(010)` flips 2 bits at
  once -- a poll landing mid-transition could otherwise sample an invalid
  intermediate code).
- **V4.15.56** -- Bundled: (1) decluttered `printDiagnostics()` per
  explicit user line-by-line review; (2) added `scanMcpStatusLeds()`, a
  boot-time sweep of all 7 MCP-driven status LEDs; (3) reworked ESP/PLC
  comms into a full 3-bit byte each way -- `TO_PLC_COMM` (`ESP_OPTO_3`/
  `OPTO_1`/`OPTO_2`) and `FROM_PLC_COMM` (`ESP_INPUT_3`/`INPUT_1`/
  `INPUT_2`). Keyence Result now wires directly to a PLC input instead of
  through the ESP32 (its only prior use was a diagnostic log line, nothing
  time-sensitive), freeing `ESP_INPUT_3`/GPIO6 for `FROM_PLC_COMM` bit 2 --
  removed the now-dead `keyenceResultIsr()`/`handleKeyenceResult()` path.
  `FROM_PLC_COMM` bit 2's *role* (bit 2 of the byte) is fixed; its
  *behavior* (what firmware should do with its value) is still open --
  tracked/logged only, nothing acts on it yet; (4) stripped the in-file
  historical FIELD UPDATE/CONFIRMED/CORRECTED/etc. chronicle and shortened
  inter-function comments per explicit user request (an informed,
  deliberate override of this project's earlier "never edit FIELD UPDATE
  retroactively" convention -- the history still exists in git log, just
  not in the file); added a blank line before `if`/`for`/`while` except
  right after `{`, `else`, or a short guard clause.

## Architecture notes worth knowing

- The real CX-Designer address map is checked into the repo root as
  `Symbol Table` (project 510_HotMel_20260902_1) -- the ground truth for
  every `$B`/`$W` address, confirmed against this file in V4.15.66. Check
  it before guessing at an address for anything new.
- NS12 (Omron NS12-TS00B-V2 HMI) talks over RS232 (`Serial2`, 38400 baud),
  entirely separate from I2C. I2C only carries the MLX90640 camera and the
  MCP23017 I/O expander (address `0x20`, A0/A1/A2 grounded; RESET pin must
  be hardware-tied high, floating/low there is the classic "MCP23017
  doesn't respond" cause).
- `McpPin` values are the Adafruit_MCP23X17 library's linear GPIO index
  (`GPA0-7` = 0-7, `GPB0-7` = 8-15), not the DIP package pin number.
- The Word-Lamp matrix push (`sendWM()`, column-paced) is fire-and-forget,
  no ack from the panel -- see V4.15.59 above for how a dropped/blank
  column gets diagnosed.
- `$W828`/`$W829` carry the band-max readout (top/bottom half max temp x10);
  `$W830` is `matrixPushCounter` (see V4.15.59); `$W100-$W108` are the
  telemetry block, all 9 words already assigned -- no free slots in either
  range.
- `$B50-$B77` are the 28 diag buttons (momentary press bits); `$B550-$B577`
  are their matching lamp bits, offset +500, user-added on the panel (see
  V4.15.62). `$B30-$B34` are the 5 SETUP-group buttons, also momentary --
  those drive physical MCP LEDs instead (see V4.15.61), not HMI lamp bits.
- 6 HMI screens exist: MAIN, TREND (+TREND FULL), SETUP, DIAG (read-only
  telemetry, no buttons), TEST (all 28 diag/test buttons), ALARM LOG
  (read-only log, no buttons). Every screen except MAIN has an Enable
  button matching its own name, whose latched state gates that screen's
  own controls (see V4.15.64) -- currently only TEST has controls to gate.
  The panel's numbered IO block runs 1-10, not 1-9: button 10 replaced the
  old standalone "OPTO3" test (see V4.15.63).
- NS12 baud rate is confirmed `38400` (matches `NS12::BAUD` in this file)
  -- a different branch's claim of `9600` was checked and doesn't apply
  here. The `claude/510-bottomer-hot-melt-monitor-3rq1ao` branch's PLC/S7
  FC-block design content (FC105/FC155/FC156/FC160, DB10, design-notes)
  belongs to the separate 410 Rotaliner project, not TGIS-510 -- nothing
  from that branch needs to be preserved in this repo.
- `Main` (this branch) now carries this project's own PLC-side STL blocks
  at the repo root -- `FC56`/`FC60`/`FC61`/`FC140`, `DB56`/`DB60`/`DB61`/
  `DB160`, `FB 100`/`FB160` -- confirmed genuinely TGIS-510's own (Gray3/
  FlagRelay comms, digital I/O mapping), distinct from the 410 Rotaliner
  FC105/FC155/FC156/FC160/DB10 set on the other branch mentioned above;
  the block-number ranges don't overlap. See "Branch consolidation
  (V5.02.01)" above for how this repo arrived at `Main` as the working
  branch, and "V5.02.00/V5.02.01" for open concerns on `FB160` and
  `FC140`'s instance-DB binding specifically.

## Reference diagnostics baseline

A known-healthy `printDiagnostics()` ('D') snapshot, captured 2026-09-30
on real hardware, for comparing against when something looks off:

```
---- DIAGNOSTICS ----
State              : Standby
Measured FPS       : 15.59
Min/Max/Avg raw delta : -99 / 25 / -0
Good/Failed frames : 6847 / 0
Encoder count (raw/mm) : 3690182 / 3690182.0
Capture state      : LATCHED
Strip1 present/maxT/hotPx : 1 / 66.2 / 1
Strip2 present/maxT/hotPx : 0 / 17.7 / 0
NS12 WM attempts/failures/oversized : 6571 / 0 / 0
NS12 RM attempts/success/writeFail/timeout/parseErr : 154 / 154 / 0 / 0 / 0
NS12 RB attempts/success/writeFail/timeout/parseErr : 308 / 308 / 0 / 0 / 0
NS12 WB attempts/failures : 876 / 0
NS12 SB notify count/rejected : 12 / 0
PLC_STATUS (commanded) : 3 (0=STOP/1=ALARM/2=WARNING/3=READY)
PLC_CONTROL ACKNOWLEDGE/MACHINE_RUNNING/tubeIsBad : 1 / 1 / 0
Screen Enable toggles ($B30-34)   : TREND FULL=off  SETUP=off  DIAG=off  TEST=ON  ALARM LOG=off
HotMelt Start/End position (mm) : 0.0 / 0.0 (from HMI)
Current HMI screen : 4
Tube length          : not yet learned (no tube has cleared the sensor yet)
Free heap          : 315.1 kB
```

Notes on this snapshot:
- NS12 link fully healthy: zero failures/timeouts/parse errors across
  WM/RM/RB/WB/SB.
- `Encoder count (raw/mm)` shows the same number twice -- `ENCODER_COUNTS_PER_MM`
  was still the `1.0f` placeholder when this was taken (pre-V5.00.01
  calibration). A post-calibration baseline should show these two numbers
  genuinely differ.
- `TEST` was the only armed screen Enable toggle, consistent with bench
  testing on the TEST screen.
- Strip1 (Outer) detected glue on this pass (`1/66.2/1`), Strip2 (Inner)
  didn't (`0/17.7/0`) -- not itself evidence of a bug, just worth noting
  if it recurs unexpectedly.
