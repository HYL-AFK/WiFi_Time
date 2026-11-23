# WiTime Scheduled Weather, Time Sync, And Status LED Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the approved weather schedule, four-hour SNTP calibration, bidirectional 15-second seconds-column animation, and the bottom-right status LED behaviors without changing the existing display layout.

**Architecture:** Keep `app_weather` as the owner of weather-window timing and maintain separate active/pending snapshots. Keep `app_time` as the owner of SNTP cycles, reinitializing the existing SNTP client only when a four-hour cycle starts. Keep all RGB output in the existing display task; other modules post semantic events and never touch WS2812 directly.

**Tech Stack:** ESP-IDF 5.3.4, FreeRTOS tasks/event groups, C static verification through `tests/verify_witime.ps1`, WS2812 16x16 matrix.

---

### Task 1: Add failing static assertions for the approved behavior

**Files:**
- Modify: `tests/verify_witime.ps1`

- [ ] **Step 1: Add assertions for weather windows and pending publication**

Assert that the weather source contains explicit 20/30-minute scheduling, separate pending state, refresh-time publication, and failure invalidation.

- [ ] **Step 2: Add assertions for four-hour SNTP cycles**

Assert a four-hour period, SNTP deinitialization/reinitialization for a new cycle, and failure continuation without replacing the current time snapshot.

- [ ] **Step 3: Add assertions for seconds parity and status-lamp events**

Assert odd/even 15-second branches, bottom-to-top clearing, Bluetooth success notification, weather-refresh success notification, and the status colors/brightness animation.

- [ ] **Step 4: Run the static test and verify it fails for missing implementation**

Run: `powershell -ExecutionPolicy Bypass -File tests\verify_witime.ps1`

Expected: `FAIL` naming the first new missing behavior.

### Task 2: Implement scheduled weather acquisition and publication

**Files:**
- Modify: `main/APP/app_weather.c`
- Modify: `main/common/weather_snapshot.h`
- Modify: `main/common/weather_snapshot.c`
- Modify: `main/APP/app_display.c`

- [ ] **Step 1: Add explicit active/pending weather state**

Keep the current snapshot as the displayed result. During each scheduled window, write successful network results to a pending snapshot. At the refresh boundary, atomically publish the pending result; when the window has no success, publish an invalid network-error snapshot so the icon placeholder and temperature red cross are rendered.

- [ ] **Step 2: Preserve RTC fallback until a scheduled weather failure is published**

Add a failure-latched field to the snapshot. The RTC temperature setter must not overwrite a published network failure; a successful network result clears the latch. Existing RTC fallback before the first weather result remains available.

- [ ] **Step 3: Anchor the schedule at weather-task startup**

Perform the existing immediate network-triggered request for the initial display. After startup, open acquisition windows at 20-30, 50-60, 80-90 minutes and every 30 minutes thereafter. A successful request during a window is cached only; the display changes at 30, 60, 90 minutes. Network arrival during an active window may trigger one retry if the window has not succeeded.

- [ ] **Step 4: Drive cyan fetching status and refresh-success blink events**

Notify the display task while an HTTP request is active. At each scheduled refresh, notify a two-blink cyan success only when a pending result was published; failures clear the pending result and leave the red-cross fault state active.

### Task 3: Add four-hour SNTP calibration cycles

**Files:**
- Modify: `main/APP/app_time.c`

- [ ] **Step 1: Start the first SNTP cycle on the existing network event**

Retain the existing primary/fallback servers and 3000 ms fallback logging. Encapsulate initialization in a helper that deinitializes the old client before starting a new cycle.

- [ ] **Step 2: Schedule the next cycle four hours after a successful or terminally failed attempt**

Use a monotonic timer. On due time, start one new network sync attempt. If disconnected or the attempt fails, keep the old system/RTC-backed time and schedule the next attempt without changing the time snapshot.

- [ ] **Step 3: Preserve existing successful SNTP-to-DS3231 writeback**

Only call `apply_sntp_time()` after `esp_netif_sntp_sync_wait()` succeeds; the four-hour loop must not write an invalid time.

### Task 4: Implement seconds-column parity animation and bottom-right status lamp

**Files:**
- Modify: `main/APP/app_display.c`
- Modify: `main/APP/app_display.h`
- Modify: `main/main.c`
- Modify: `main/middle/ble_provision.c`

- [ ] **Step 1: Replace the seconds renderer with odd/even 15-second phases**

Odd phases light from the bottom upward and retain all previously lit pixels. Even phases turn pixels off from the bottom upward. The bottom-right status pixel at `(15,15)` is excluded from seconds rendering.

- [ ] **Step 2: Add status-state precedence and animation**

Render `(15,15)` as: blue breathing while BLE is connected, cyan during weather HTTP, green while STA is online, pink in ordinary display, and red on a fault. A two-blink overlay has priority for exactly two on/off cycles, then returns to the current base state. BLE connection success and scheduled weather publication trigger their respective blink colors.

- [ ] **Step 3: Allow only the status pixel to be non-grayscale**

Retain the frame-integrity check for all other pixels, but explicitly allow the single colored status pixel so RGB status output is transmitted.

- [ ] **Step 4: Wire status events from BLE, Wi-Fi, and weather**

Call the new display APIs from BLE connect/disconnect callbacks, Wi-Fi state changes, and weather request/publication boundaries. Do not let those callbacks write WS2812 data directly.

### Task 5: Verify behavior without building firmware

**Files:**
- Modify: `tests/verify_witime.ps1`

- [ ] **Step 1: Run static verification**

Run: `powershell -ExecutionPolicy Bypass -File tests\verify_witime.ps1`

Expected: `PASS: WiTime static verification`.

- [ ] **Step 2: Run whitespace/diff checks**

Run: `git diff --check -- main/APP/app_weather.c main/APP/app_time.c main/APP/app_display.c main/APP/app_display.h main/common/weather_snapshot.c main/common/weather_snapshot.h main/main.c main/middle/ble_provision.c tests/verify_witime.ps1`

Expected: exit code 0.

- [ ] **Step 3: Do not run ESP-IDF build or flash commands**

Report static verification only; firmware compilation and device flashing remain explicitly out of scope for this request.

