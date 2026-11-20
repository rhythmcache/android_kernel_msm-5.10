# POCO X4 Pro 5G (veux/blair) Kernel 5.10 Port Status

## Current Milestone: Boot to Early Init / Userspace
- **Active Working Slot**: Slot B (`5.4.302~Vector/0b200678`)
- **Test Slot**: Slot A
- **Latest Commits Pushed**:
  - `1d69a506e223`: `msm-poweroff: register pshold restart handler unconditionally before SCM`
  - `b8808d8b19ea`: `veux.config: disable USB_DUMMY_HCD and UBSAN_TRAP, upload vmlinux/System.map`

## IMPORTANT TESTING & HARDWARE RULES
- **Forced Reboots**: Must ALWAYS use a **Power-only long press** (~10-15s). Do NOT press any volume buttons. (Fastboot mode was entered previously after repeated failed boots; the exact cause is unverified, but keeping hands off volume keys ensures recovery/bootloader combos are never triggered).
- **Panic Timeout**: Reboots after panics happen automatically within ~5 seconds. Do not touch the device buttons for at least 3 minutes after reboot.

---

## Test Runs & Oops Analysis

### Run 1: Test Build `69bb2b2d358b`
- **Result**: Boot failure / hang on Poco logo.
- **Log Captured**: `/sdcard/kernel-port/port-logs/fail1-20261120_192954/console-ramoops-0`
- **Execution Reach**: Reached $t = 0.144458\text{ s}$ (`Linux version 5.10.209`).
- **Crash Site**: `qcom_scm_io_writel+0x78/0x110` <- `qcom_scm_set_download_mode+0xdc/0x178` <- `msm_restart_probe+0x2d0/0x524`
- **Root Cause**: `msm-poweroff` probe invoked uninitialized `__scm` pointer at pure_initcall.

### Run 2: Test Build `f746b6fde58d`
- **Result**: Stuck on Poco logo, then dark screen. Fastboot mode entered after user pressed Power + Vol Down.
- **Log Captured**: `/sdcard/kernel-port/port-logs/fail2-20261120_211216/console-ramoops-0`
- **Execution Reach**: Reached $t = 0.858830\text{ s}$ (`Linux version 5.10.272-gf746b6fde58d`). All early subsystems, SMP, clocks, regulators, pinctrl, and early drivers probed.
- **Crash Site**:
  ```
  [ 0.849419] Unexpected kernel BRK exception at EL1
  [ 0.849508] Internal error: BRK handler: f2005512 [#1] PREEMPT SMP
  [ 0.849552] pc : exit_usb_ep_get_maxpacket_limit+0x38/0x3c
  [ 0.849857] Code: b802e1b9 2a1b03e0 d50323bf d65f03c0 (d42aa240)
  [ 0.854332] Kernel panic - not syncing: BRK handler: Fatal exception
  [ 0.858830] Rebooting in 5 seconds..
  ```
- **Root Cause**:
  1. Opcode `d42aa240` is `brk #0x5512` = Clang UBSAN trap (`0x5500 + kind 0x12`), triggered by `CONFIG_UBSAN_TRAP=y` during `dummy_hcd` initialization.
  2. Dark screen / no reboot: `msm-restart` probe was deferred waiting for `qcom_scm`. When panic hit at $t = 0.85\text{ s}$, no restart handler was registered on the notifier chain. `machine_restart()` printed "Reboot failed -- System halted" and entered an infinite loop with interrupts disabled.
  3. Fastboot entry: Caused by Power + Vol Down button combination during recovery attempts.

---

### Fixes for Run 3:
1. **msm-poweroff (`1d69a506e223`)**:
   - Mapped `pshold-base` and registered `pm_power_off` / `restart_nb` (priority 200) unconditionally at the very start of probe, with no probe deferral.
   - SCM download mode configuration is now guarded by `if (qcom_scm_is_available())` rather than aborting probe.
2. **Configuration (`b8808d8b19ea`)**:
   - Disabled virtual `CONFIG_USB_DUMMY_HCD`.
   - Disabled `CONFIG_UBSAN_TRAP` so UBSAN errors log warnings instead of triggering fatal `BRK` panics.
   - Kept `CONFIG_PANIC_TIMEOUT=5`.
3. **CI Artifacts**:
   - Updated GitHub Actions workflow to upload `vmlinux` and `System.map` alongside `Image` and DTBs.
