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

### Run 3: Test Build `015d22e5b04b`
- **Result**: Boot reached userspace `/init` at $t = 0.89\text{s}$! Reboots occurred cleanly after 10s timeout, safely completing 7 automated retry cycles and falling back to Slot B.
- **Log Captured**: `/sdcard/kernel-port/logs/015d22e5b04b/console-ramoops-0`
- **Execution Reach**:
  - Reached userspace `/init` at $t = 0.890461\text{s}$.
  - Switched root to `/first_stage_ramdisk` at $t = 0.900384\text{s}$.
  - First-stage init waited 10,000 ms for storage block devices (`boot_a`, `dtbo_a`, `super`, `vbmeta_a`), timed out, and panicked with `exitcode=0x00007f00`.
- **Root Cause**:
  - `4804000.ufshc` deferred probe with `-517` (`-EPROBE_DEFER`) waiting for `ufsphy`.
  - The UFS PHY node in `blair-qrd.dtsi` requested `compatible = "qcom,ufs-phy-qmp-v4-yupik"`.
  - The driver for `qcom,ufs-phy-qmp-v4-yupik` was present in Vector 5.4 but completely missing in 5.10 `drivers/phy/qualcomm/`.

---

### Run 4 (Commit `a0b8ac72dd52`):
1. **Ported Yupik UFS PHY Driver**:
   - Added `phy-qcom-ufs-qmp-v4-yupik.c` and `phy-qcom-ufs-qmp-v4-yupik.h` to `drivers/phy/qualcomm/`.
   - Updated `drivers/phy/qualcomm/Makefile` under `CONFIG_PHY_QCOM_UFS_V4`.
2. **Diagnostic & Devlink Cmdline**:
   - Updated `CONFIG_CMDLINE` in `veux.config` with `fw_devlink=permissive deferred_probe_timeout=5 loglevel=8`.
3. **CI Verification**:
   - Verified `phy-qcom-ufs-qmp-v4-yupik.o` compiled into Image and `qcom,ufs-phy-qmp-v4-yupik` string is present in Image binary.
   - Identified that `qcom,rpm-smd-regulator` and `qcom,qsmmu-v500` were `=m` in `holi_GKI.config` and not yet built-in.

---

### Run 5 Setup (Storage & Transport Built-in Alignment):
1. **SMD-RPM Transport & Regulators Built-in**:
   - Built-in `CONFIG_MSM_RPM_SMD=y`, `CONFIG_REGULATOR_RPM_SMD=y` (powers UFS supplies L4A/L7A).
   - Built-in `CONFIG_RPMSG_QCOM_GLINK=y`, `CONFIG_RPMSG_QCOM_GLINK_RPM=y`, `CONFIG_RPMSG_QCOM_GLINK_SMEM=y`, `CONFIG_QCOM_GLINK=y`.
   - Built-in `CONFIG_QCOM_CLK_SMD_RPM=y`, `CONFIG_SPMI_PMIC_CLKDIV=y`, `CONFIG_MSM_CLK_AOP_QMP=y`.
   - Built-in `CONFIG_REGULATOR_QCOM_PM8008=y`, `CONFIG_MFD_I2C_PMIC=y`.
2. **SMMU / IOMMU Built-in**:
   - Built-in `CONFIG_ARM_SMMU=y`, `CONFIG_QTI_IOMMU_SUPPORT=y` (resolves `ufshc@4804000` SMMU dependency).
3. **Sleep & Keypad Helpers**:
   - Built-in `CONFIG_QCOM_SMP2P_SLEEPSTATE=y`, `CONFIG_QCOM_SUBSYSTEM_SLEEP_STATS=y`, `CONFIG_KEYBOARD_GPIO=y`.
4. **Cmdline & Buffer Protection**:
   - Removed `initcall_debug` to prevent 256KB ramoops console ring buffer rollover during the 10s init wait.
   - Kept `fw_devlink=permissive deferred_probe_timeout=5 loglevel=8 ignore_loglevel panic=5 qcom_scm.download_mode=0`.

---

### Run 6 (QSEECOM, SHMBRIDGE & Watchdog Updates):
1. **Probe Diagnostics & SHMBRIDGE Bridge Tracing**:
   - Added detailed `pr_err` diagnostics across `qseecom.c` for SHMBRIDGE memory handles (`qsecom.align`, `qsecom_ta.align`).
   - Converted inline early keymaster64 lookup to delayed work (15s after boot) to prevent stalling probe.
2. **Watchdog Adjustment**:
   - Maintained 45s stage-1 / 90s stage-2 timeout for userspace boot.

---

### Run 7 (MSM ION Port & Keymaster Resolution):
1. **Root Cause Analysis**:
   - `vendor.keymaster-4-1` calls `libQSEEComAPI.so`, which opens `/dev/ion` and allocates buffers using `ION_QSECOM_TA_HEAP_ID` (`0x2`) and `ION_QSECOM_HEAP_ID` (`0x80`).
   - In 5.10 GKI, Qualcomm's `msm_ion.c` driver was missing; only generic system heap (id 0) was registered. Allocations failed with `-ENODEV`, leading to keymaster SIGABRT.
2. **Implementation (Option a)**:
   - **Commit 1 (`ad1c6c0fea3d`)**: Added `include/linux/msm_ion.h`, embedded `struct msm_iommu_data iommu_data;` into `struct ion_buffer`.
   - **Commit 2 (`3cee65811ab1`)**: Ported MSM ION heap driver files from Vector 5.4 (`msm_ion.c`, `msm_ion_dma_buf.c`, `ion_msm_system_heap.c`, `ion_system_secure_heap.c`, `ion_carveout_heap.c`, `ion_cma_heap.c`, `ion_secure_util.c`, `ion_msm_page_pool.c`). Adapted `msm_ion_dma_buf.c` to 5.10 DMA-BUF architecture.
   - **Commit 3**: Added diagnostics for `ion_open`, `ion_alloc_fd`, and heap matching in `ion.c` / `ion_buffer.c`. Updated `veux.config` (`CONFIG_ION_MSM_HEAPS=y`, `CONFIG_QCOM_LAZY_MAPPING=y`, `CONFIG_ION_POOL_AUTO_REFILL=y`, `CONFIG_ION_POOL_FILL_MARK=100`, `# CONFIG_ION_CMA_HEAP is not set`). Removed dead `=m` from `holi_GKI.config`.

---

### Slot A Debug Guards (TEMPORARY - FOR DEBUGGING ONLY):
> [!WARNING]
> These guards exist solely for capturing kernel/userspace logs on Slot A bringup failures without entering Recovery or triggering persistent ABL boot loops. **REMOVE BEFORE FINAL PRODUCTION RELEASE.**

1. **Read-Only Protection for `misc` Partition (`block/partitions/core.c`, `block/ioctl.c`)**:
   - In `add_partition()`, when the partition name from GPT matches `"misc"`, `p->policy` is set to `1` (`bdev_read_only`).
   - In `blkdev_roset()`, any attempt to clear read-only state via `ioctl(BLKROSET)` on `"misc"` is rejected with `-EPERM`.
   - **Purpose**: Prevents Android userspace (`UserDataPreparer` / `RecoverySystem`) from writing `"boot-recovery"` into the Bootloader Control Block (BCB) on `/dev/block/by-name/misc` upon `init_user0` failure.
   - **Scope**: Matches only the `"misc"` GPT partition name. Does not affect any other partition (`boot`, `userdata`, `metadata`, etc.).

2. **Reboot Command Interception (`drivers/power/reset/msm-poweroff.c`)**:
   - In `msm_restart_prepare(const char *cmd)`, any reboot command containing `"recovery"`, `"bootloader"`, or `"fastboot"` (via `strstr`, covering `"reboot,recovery"`, `"userrequested,recovery"`, `"reboot,bootloader"`, `"userrequested,bootloader"`, `"userrequested,fastboot"`, fastbootd) is intercepted and remapped to `0x77665501` (`PON_RESTART_REASON_UNKNOWN` / normal warm reboot), accompanied by a `pr_err` log.
   - **Purpose**: Prevents Qualcomm ABL from redirecting the reboot into Recovery mode or Fastboot mode when userspace calls `PowerManager.reboot()`.
   - **Crucial Side Effect**: This guard removes the bootloader reboot path entirely for Slot A test builds (`adb reboot bootloader` will not work on Slot A, but reboots normally). This is debug-only and must never reach the production kernel.
   - Together with Guard 1, ABL treats failures as normal boot attempts, decrements `triesRemaining`, and falls back cleanly to Slot B, leaving the 5.10 console ramoops intact.

---

### Mandatory Safe Slot A Test Procedure:
> [!IMPORTANT]
> Follow this procedure strictly on every Slot A test run to prevent accidental mode steering and ensure fallback:
> 1. **Power-Only Normal Boot**: To boot Slot A normally, use a **short Power press**. Do NOT hold Power for 10–12 seconds (a 10–12s hold is a PMIC forced reset that consumes a boot retry without clean userspace teardown). **NEVER hold Volume Up or Volume Down.** (Holding Volume Up commands ABL in hardware to enter Recovery; holding Volume Down commands ABL to enter Fastboot).
> 2. **During a Loop, Do Not Touch Phone for 5 Minutes**: Do not interrupt the boot loop manually. Allow the phone to execute its boot attempts and wait for ABL to automatically decrement the retry counter and fall back to Slot B.
> 3. **If it ends in Fastboot**: This happens if ABL exhausts all retry counters across slots or if Volume Down was triggered. If stranded in fastboot, run `fastboot set_active b` from a host / connected phone (e.g. via Dioxamine) to return to Slot B.
> 4. **Post-Run Log Harvest Routine**: Immediately after booting Slot B:
>    - Harvest `/sys/fs/pstore/*` to `/sdcard/kernel-port/logs/`.
>    - Report `vold`, `keystore2`, `keymaster`, `gatekeeper`, and `ion_alloc` lines around `init_user0`.
>    - Verify slot flags (`IBootControl/default 4`, `sgdisk`) and check `misc` SHA256 against clean backup.

> [!NOTE]
> **What this guard CANNOT cover**:
> - Hardware key presses during power-on (ABL evaluates Volume Up / Volume Down in hardware before loading the kernel).
> - ABL entering Fastboot on its own when all boot attempts across all bootable slots are fully exhausted.
> - "ABL only inspects IMEM, misc, and GPT" is the expected behavior based on CAF source code, not verified in proprietary ABL binary. If ABL still enters recovery or fastboot during a test, record exactly what happened.


