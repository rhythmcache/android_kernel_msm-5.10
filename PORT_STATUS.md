# POCO X4 Pro 5G (veux/blair) Kernel 5.10 Port Status

## Current Milestone: Boot to Early Init / Userspace
- **Active Working Slot**: Slot B (`5.4.302~Vector/0b200678`)
- **Test Slot**: Slot A
- **Hardening & Tracing Baseline (Vector 5.4 vs Port 5.10)**:
  - Vector 5.4 (`/proc/config.gz`):
    - `CONFIG_CFI_CLANG`: Disabled (`# CONFIG_CFI_CLANG is not set`)
    - `CONFIG_SHADOW_CALL_STACK`: Disabled (`# CONFIG_SHADOW_CALL_STACK is not set`)
    - `CONFIG_KPROBES`: Disabled (`# CONFIG_KPROBES is not set`)
    - `CONFIG_KRETPROBES`: Disabled (`# CONFIG_KRETPROBES is not set`)
    - `CONFIG_FTRACE`: Disabled (`# CONFIG_FTRACE is not set`)
    - `CONFIG_UBSAN`: Disabled (`# CONFIG_UBSAN is not set`)
    - `CONFIG_LTO_CLANG`: Enabled (`CONFIG_LTO_CLANG=y`)
    - `CONFIG_RANDOMIZE_BASE`: Enabled (`CONFIG_RANDOMIZE_BASE=y`)
  - Port 5.10 (at `f746b6fde58d`):
    - `CONFIG_CFI_CLANG=y`
    - `CONFIG_SHADOW_CALL_STACK=y`
    - `CONFIG_KPROBES=y`
    - `CONFIG_KRETPROBES=y`
    - `CONFIG_UBSAN=y` (`CONFIG_UBSAN_TRAP=y`, `CONFIG_UBSAN_LOCAL_BOUNDS=y`)
    - `CONFIG_LTO_CLANG_THIN=y`
    - `CONFIG_RANDOMIZE_BASE=y`
- **Latest Commits Pushed**:
  - `9320790a5bbe`: `veux.config: disable USB_DUMMY_HCD and CONFIG_UBSAN matching Vector 5.4` [Boot candidate, not diagnostic: UBSAN and USB_DUMMY_HCD disabled, do not use to symbolize old crashes]
  - `9af300b6acab`: `Revert "msm-poweroff: register pshold restart handler unconditionally before SCM"`

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
- **Verified Facts & Root Cause Analysis**:
  1. Symbol Name: In the raw pstore bytes, the printed symbol text is corrupted/unreliable (showing mangled words like `exit_ucbWep_qet_-appacked_limit` and `exit_ucb_ep_ret_maxpackep_limip`). Neither matches any valid kernel symbol.
  2. Guard Instruction & Trap: In `drivers/usb/dwc3/dwc3-msm-ops.c`, `exit_usb_ep_set_maxpacket_limit` contains an array access `dwc->eps[epnum]` protected by a bounds check (`cmp w8, #0x20; b.hs +0x38`) inserted by Clang's `-fsanitize=local-bounds` (`CONFIG_UBSAN_LOCAL_BOUNDS`). The instruction at `+0x38` is `brk #0x5512` (`d42aa240`).
  3. Register State: Register `x8` held `0x0000000000000064` (100 in decimal), which exceeded the 32-element limit (`#0x20`) and directly triggered the trap.
  4. Dummy Endpoint Cast Bug: The entry hook `entry_usb_ep_set_maxpacket_limit` blindly cast `struct usb_ep *ep` passed by `dummy_hcd` (`usb_ep_set_maxpacket_limit(&ep->ep, ~0)`) into a `struct dwc3_ep`. Because `dummy_ep` has a completely different layout and size (`0x60` bytes) than `dwc3_ep`, reading `*(ep + 0xaa)` accessed memory outside `dummy_ep`, storing a bogus value into `data->xi0` that became `x8=0x64` in the exit handler.
  5. Reset Behavior: Both `psci_sys_reset_nb` (priority 129) and `qcom_wdt_core` restart handler (priority 255) were registered. GPT attributes for `boot_a` showed `tries=0` and `unbootable=1`, confirming that resets occurred and ABL performed multiple boot attempts before exhausting retries. Commit `1d69a506e223` (registering PS_HOLD unconditionally before SCM) was defensive hardening, not a confirmed single root cause for the dark screen.
  6. SCM configuration: If `qcom_scm` is not yet probed when `msm_restart_probe()` runs, the driver skips setting SCM download mode (which defaults to 0).

---

### Run 3 Setup & Hardening:
1. **Revert PS_HOLD change (`1d69a506e223` reverted)**:
   - Reverted `1d69a506e223` to keep `msm-poweroff.c` strictly identical to working VectorKernel base (plus the verified `if (!qcom_scm_is_available()) return -EPROBE_DEFER;` guard from `f746b6fde58d`).
   - PSCI warm reset remains the primary reset path. Note: the hypothesis that PS_HOLD hard reset wipes pstore is an unverified hypothesis, not an established finding, but avoiding untested changes to the reset path keeps variables controlled.
2. **Align Sanitizers with Vector 5.4**:
   - Disabled `CONFIG_USB_DUMMY_HCD`: emulated dummy USB host controller is unnecessary on real hardware.
   - Disabled `CONFIG_UBSAN` entirely (`# CONFIG_UBSAN is not set`), matching Vector 5.4 defconfig.
   - Note: In 5.10 GKI, `CONFIG_UBSAN_LOCAL_BOUNDS` has no runtime logging handler and strictly traps. Disabling UBSAN eliminates false positive trap instructions while keeping `CONFIG_CFI_CLANG=y`.
3. **CI Artifacts & Build Classification**:
   - Commit `9320790a5bbe` is designated as a **boot candidate, not diagnostic**. Because UBSAN and dummy_hcd are disabled, its vmlinux cannot be used to symbolize the Test 2 crash.
   - For file:line symbolization of the Test 2 crash, a separate unchanged build of commit `f746b6fde58d` with vmlinux/System.map uploaded must be used.
