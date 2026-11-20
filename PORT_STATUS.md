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
- **Result**: Stuck on Poco logo, then dark screen. Fastboot mode entered after repeated failed boot attempts (exact cause of fastboot entry unverified).
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
- **Analysis**:
  1. Opcode `d42aa240` is `brk #0x5512` = Clang UBSAN trap (`0x5500 + kind 0x12`), triggered by `CONFIG_UBSAN_TRAP=y` during `dummy_hcd` initialization.
  2. Reset Behavior: Both `psci_sys_reset_nb` (priority 129) and `qcom_wdt_core` restart handler (priority 255) were registered. Furthermore, GPT attributes for `boot_a` showed `tries=0` and `unbootable=1`, confirming that resets did occur and ABL performed multiple boot attempts before exhausting retries. Commit `1d69a506e223` (registering PS_HOLD unconditionally before SCM) is defensive hardening to guarantee PS_HOLD availability at any stage, not a confirmed single root cause for the dark screen.
  3. SCM configuration: If `qcom_scm` is not yet probed when `msm_restart_probe()` runs, the driver skips setting SCM download mode (which defaults to 0).

---

### Run 3 Setup & Hardening:
1. **Revert PS_HOLD change (`1d69a506e223` reverted)**:
   - Reverted `1d69a506e223` to keep `msm-poweroff.c` strictly identical to working VectorKernel base (plus the verified `if (!qcom_scm_is_available()) return -EPROBE_DEFER;` guard from `f746b6fde58d`).
   - PSCI warm reset remains the primary reset path (preserving pstore/ramoops across reboots), backed by `qcom_soc_wdt` (priority 255).
2. **Align Sanitizers with Vector 5.4**:
   - Disabled `CONFIG_USB_DUMMY_HCD`: emulated dummy USB host controller is unnecessary on real hardware.
   - Disabled `CONFIG_UBSAN` entirely (`# CONFIG_UBSAN is not set`), matching Vector 5.4 defconfig.
   - Note: In 5.10 GKI, `CONFIG_UBSAN_LOCAL_BOUNDS` has no runtime logging handler and strictly traps. Disabling UBSAN eliminates false positive trap instructions while keeping `CONFIG_CFI_CLANG=y`.
3. **CI Artifacts**:
   - `vmlinux` and `System.map` are uploaded as build artifacts for full offline symbolization.
