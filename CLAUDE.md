# TGIS-510 firmware -- working conventions

Home-lab / after-hours project. ESP32-S3 firmware for the Thermal Glue
Inspection System lives in `src/tgis510_v4_15_N.cpp`. Read this file before
making changes so standing rules and recent context carry over across
sessions.

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
  bumps the version, in lockstep, all in the same edit:
  1. Rename `src/tgis510_v4_15_N.cpp` -> `src/tgis510_v4_15_(N+1).cpp`
     (`git mv`).
  2. Update the `// Ref: TGIS-510_cpp_V4_15.N` header comment at the top of
     the file.
  3. Update `platformio.ini`'s `FW_VERSION_STRING`/`FW_FILE_STRING`
     build_flags to match.
  One version bump per logical change, even if several bumps happen back
  to back before a single GO/commit -- keeps each commit message mapped to
  exactly one version number and one rationale.
- User preference: **"Mostly Automation"** -- make the reasonable
  engineering call and proceed rather than stopping to ask, unless
  genuinely blocked on a decision only the user can make (e.g. a
  functional spec, a hardware wiring choice, an explicit rule override).

## Recent changes and why (most recent first)

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
