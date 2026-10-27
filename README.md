# gpu-unlock for Windows

> Unlock the full FP/BF16 instruction issue rate of NVIDIA Ampere GeForce cards (GA102 / GA103 / GA104 / GA106) on Windows — without patching the NVIDIA driver.

![License: GPL-2.0-only](https://img.shields.io/badge/license-GPL--2.0--only-blue.svg)

![Platform: Windows 10/11 x64](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-0078D6.svg)

**English** · [简体中文](README.zh-CN.md)

## Preview

> Placeholder — replace `docs/screenshot.svg` with a real screenshot (`docs/screenshot.png`) before publishing.

![gpu-unlock for Windows console and driver](docs/screenshot.svg)

---

`gpu-unlock for Windows` is the Windows port of the [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock) Linux kernel module. It applies the same **CMP unlocker V67/PLM** technique from user mode: it writes the GPU speed selectors (SS0 `0x88888888`, which selects the full BF16 rate, and the 4-bit SS1 `0x8` IMLA4), performs a PCIe FLR, and lets the unmodified NVIDIA driver bind the card afterwards.

The result is **up to ~2× the FP/BF16 FMA-into-FP32 issue rate** on supported cards, with no driver patching and no UEFI application.

> **Looking for the original Linux kernel module?** Its unlock engine is still here and is exactly what this port compiles — `kernel/gpu_unlock/` is shared code. The Linux-side scaffolding (kbuild, DKMS, `debian/`, `packaging/`) has been removed, so the module no longer builds on Linux. The archived Linux documentation is in [`docs/README.linux.md`](docs/README.linux.md), and copies of the removed build files are in [`docs/linux-legacy/`](docs/linux-legacy/). The rest of this file covers the Windows port only.

> **This is a hardware modification tool, not a software crack.** It writes undocumented GPU registers. Read [Safety, risk and warranty](#safety-risk-and-warranty) before running it on a machine you depend on.

---

## Table of contents

- [Preview](#preview)
- [How to run](#how-to-run)
- [How it works](#how-it-works)
- [Features](#features)
- [Requirements](#requirements)
- [Building](#building)
- [Deployment layout](#deployment-layout)
- [Usage](#usage)
- [Command-line reference](#command-line-reference)
- [Supported GPUs](#supported-gpus)
- [Firmware](#firmware)
- [Architecture](#architecture)
- [Safety, risk and warranty](#safety-risk-and-warranty)
- [Troubleshooting / FAQ](#troubleshooting--faq)
- [Known limitations](#known-limitations)
- [Contributing](#contributing)
- [License and attribution](#license-and-attribution)

---

## How to run

`gpu_unlock.exe` is a **portable console program**. There is no installer, no service to register up front and no configuration file: you put two binaries and one firmware folder in the same directory and run the executable.

### What you need in one folder

```
gpu_unlock.exe          <- built from windows/
gpuunlock.sys           <- built from windows/driver/
gpu-unlock\610.43.03\   <- the five blobs from firmware/
```

See [Deployment layout](#deployment-layout) for the exact tree and where each piece comes from.

### The four steps

**1. Build the binaries** — `windows\build.ps1` and `windows\build_driver.ps1`. Details in [Building](#building).

**2. Sign the driver once, then reboot** — elevated, because the kernel will not load an unsigned test driver:

```powershell
powershell -ExecutionPolicy Bypass -File windows\sign_driver.ps1 -EnableTestSigning
```

**3. Drop the firmware in place** — `gpu-unlock\610.43.03\` next to `gpu_unlock.exe`. The tool refuses a real unlock without it.

**4. Run it** — double-click for the one-click path, or drive it from a console:

```powershell
gpu_unlock.exe list           # which NVIDIA functions exist, and is mine supported?
gpu_unlock.exe verify         # run the whole transaction on the built-in simulator
gpu_unlock.exe real --probe   # read-only look at the real card (needs admin + driver)
gpu_unlock.exe real           # the actual unlock: blanks the screen, asks you to type YES
```

The console form is the recommended one: it is the only way to pass `--bdf`, `--pre-flr`, `--cold` or `--assets` when the simple path is refused.

### What double-clicking does

Launching `gpu_unlock.exe` with **no arguments** is exactly `gpu_unlock.exe real`, with one addition: when started from Explorer it keeps the window open (`Press any key to exit...`) so you can read the result. Concretely it will:

1. show a UAC prompt and relaunch itself elevated (refusing it exits with code 1),
2. create and start the kernel service `GpuUnlock` pointing at `gpuunlock.sys` beside the exe,
3. pick the **first supported** NVIDIA function and print its BDF,
4. print a warning that the display adapter will be disabled and ask you to type `YES`,
5. disable the adapter (the screen goes dark), run the unlock through the driver, then re-enable it,
6. print `unlock read-back: PASS` or `FAIL`.

It cannot choose a card, skip the confirmation or clear a stale GSP — use the console form for that.

### What each command requires

| Command | Administrator | `gpuunlock.sys` | Real `610.43.03` firmware |
|---|---|---|---|
| `list` | no | no | no |
| `verify` | no | no | no — `assets_builtin.c` synthesizes blobs |
| `real --probe` | **yes** | yes | no — it only reads registers |
| `real` (and double-click) | **yes** | yes | **yes** |
| `version`, `help` | no | no | no |

`verify` and `list` are safe anywhere: neither loads the driver, touches the GPU or needs elevation.

### Uninstalling / cleaning up

Nothing is written to Program Files and no autostart entry is created. To remove the tool: delete the folder, then delete the kernel service registration that the first real run created (elevated):

```powershell
sc.exe stop GpuUnlock
sc.exe delete GpuUnlock
```

Optionally undo test signing with `bcdedit /set testsigning off` and reboot.

---

## How it works

The Windows build keeps the original kernel unlock engine **unchanged** and re-hosts it:

1. **Enumerate** — `backend_win32.c` lists NVIDIA PCI functions through SetupAPI and picks the first supported device, or the one given by `--bdf`.
2. **Elevate & load the driver** — `gpu_unlock.exe` requests UAC, registers and starts `gpuunlock.sys` as a demand-start kernel service (`GpuUnlock`, device link `\\.\GpuUnlock`).
3. **Disable the adapter** — `DICS_DISABLE` unloads `nvlddmkm` and frees the card. The screen goes blank on a display card.
4. **Run the original engine** — the same sources the Linux module uses (`main.c`, `unlock_610.c`, `prep_610.c`, `layout_610.c`, `vbios_610.c`, `gfw_eval.c`) are compiled into the executable against a Linux-API shim (`windows/include/linux/`). Linux module registration macros are neutralized, so `gpu_unlock_init()` / `gpu_unlock_exit()` run as ordinary functions.
5. **Mechanical I/O via the driver** — `gpuunlock.sys` exposes only what user mode cannot do itself: PCI config read/write, BAR mapping, physically contiguous DMA, PCIe FLR and a D0 transition. **Unlock policy stays in user mode.**
6. **Unlock sequence** — load the V67/PLM firmware, run FWSEC-FRTS (raise WPR2), start the Booter, write SS0/SS1, issue FLR.
7. **Verify** — the tool reads the selectors back. `SS0 == 0x88888888 && SS1 == 0x8 && GSP halted && FLR seen` is reported as `PASS`.
8. **Re-enable** — `DICS_ENABLE` lets PnP bind `nvlddmkm` again. The card runs at the unlocked rate for the rest of the power cycle.

A **simulated GA104 backend** (`backend_sim.c`, RTX 3070 `10de:2484`, BDF `0000:21:00.0`) is built in. It is an in-process register model — no real PCI config space or BAR is touched — and exists so the whole transaction can be tested on any machine.

---

## Features

- **No driver patching.** Survives NVIDIA driver updates; nothing is modified on disk in the driver.
- **Pre-driver timing.** Unlock runs after the adapter is disabled and before `nvlddmkm` re-binds.
- **Same engine as the Linux module.** Byte-for-byte reuse of the validated unlock logic.
- **Minimal kernel surface.** The driver only performs mechanical PCI/DMA/FLR operations; all policy is user mode.
- **Built-in simulator.** `verify` exercises the full path with synthetic firmware on any GPU (or none).
- **Read-only probe.** `real --probe` dumps PMC/GFW/PLM/SS0/SS1/WPR2 without writing anything.
- **Explicit confirmation.** Real unlocks blank the screen and ask you to type `YES`.
- **Fail-safe reporting.** Unlock success is decided by register read-back, not by exit code alone.
- **Test-signed, open source.** GPL-2.0-only, auditable source, no binary blobs beyond NVIDIA firmware.

---

## Requirements

### Runtime

| Requirement   | Detail                                                             |
| ------------- | ------------------------------------------------------------------ |
| OS            | Windows 10 / 11, x64                                               |
| Privileges    | Administrator for `real` and the double-click path (UAC prompt). `list`, `verify`, `version` and `help` need no elevation |
| Driver        | `gpuunlock.sys`, **self-signed**, test signing enabled, registered as the kernel service `GpuUnlock` |
| Secure Boot   | **Off.** `bcdedit /set testsigning on` is rejected while Secure Boot policy is active, so the driver can never load |
| Firmware      | The five real `610.43.03` blobs in `gpu-unlock\610.43.03\` next to the exe — needed only for a real unlock |
| GPU           | A supported Ampere GeForce (see [Supported GPUs](#supported-gpus)) |
| NVIDIA driver | Installed (`nvlddmkm`) for real use; not required for `verify`     |
| Runtime DLLs  | None to install: the executable statically links the C runtime and imports only in-box Windows libraries (`KERNEL32`, `ADVAPI32`, `SETUPAPI`, `SHELL32`), so no VC++ redistributable is required |

### Build

| Tool                                                           | Purpose                                                                                         |
| -------------------------------------------------------------- | ----------------------------------------------------------------------------------------------- |
| Visual Studio 2019/2022 — Build Tools or full IDE, with the **C/C++ x64** component (`Microsoft.VisualStudio.Component.VC.Tools.x86.x64`) | `cl.exe` / `link.exe` for both the exe and the driver |
| Windows 10/11 SDK (UCRT headers such as `ucrt\stdio.h`, plus `signtool.exe`) | User-mode compile, and signing the driver |
| Windows Driver Kit (WDK, `Microsoft.WindowsWDK`)               | `km\ntddk.h`, `ntoskrnl.lib`, `hal.lib`, `wdmsec.lib`, `BufferOverflowK.lib`                     |
| PowerShell 5.1+                                                | Build and signing scripts; they locate MSVC/SDK/WDK themselves, so no Developer Command Prompt is needed |
| Python 3.11+                                                   | Firmware pipeline (`hashlib.file_digest`) and the test suite; optional if the blobs already exist |

> `windows/build.ps1` finds MSVC through `vswhere.exe` and needs the `VC.Tools.x86.x64` component plus a Windows 10 SDK with `ucrt\stdio.h`. `windows/build_driver.ps1` instead needs the WDK's `km\ntddk.h` and the kernel libs under `Windows Kits\10\Lib\<ver>\km\x64`.

---

## Building

Run from the repository root in PowerShell.

### 1. User-mode executable

```powershell
powershell -ExecutionPolicy Bypass -File windows\build.ps1
```

Produces `build\gpu_unlock.exe` (objects in `build\obj\`).

### 2. Kernel driver

```powershell
powershell -ExecutionPolicy Bypass -File windows\build_driver.ps1
```

Produces `build\gpuunlock.sys` (object in `build\drvobj\`).

### 3. Sign and enable test signing (elevated)

```powershell
powershell -ExecutionPolicy Bypass -File windows\sign_driver.ps1 -EnableTestSigning
```

> **Both build scripts pass `/pathmap`**, so `__FILE__` and the object records are
> rewritten to `C:\gpu-unlock-src\...` instead of the builder's real profile path.
> A binary built from this repository therefore does not disclose the build
> account. `/pathmap` requires `/experimental:deterministic`; MSVC silently
> ignores it otherwise, which is how a `C:\Users\<account>\...` string usually
> ends up in a shipped EXE.

This creates/reuses a self-signed code-signing certificate, trusts it for the current user, signs `gpuunlock.sys`, verifies the signature, and turns on `testsigning`.

> **Reboot after enabling test signing.** The driver cannot load until test signing is active.

To sign without enabling test signing, drop `-EnableTestSigning`.

### 4. Firmware blobs

The real unlock needs the five `610.43.03` blobs. Generate them with the shared Python pipeline (canonical target is `make firmware` on Linux/WSL; on Windows run the scripts directly):

```powershell
python firmware\fetch_artifacts.py <cache-dir>
python firmware\generate_v67.py <nvidia-utils-package> <assets-dir>
python firmware\extract_boot_assets.py <nvidia-source-dir> <assets-dir>
```

The scripts verify every input against its pinned SHA-256 in `firmware/artifacts-610.43.03.json` before writing. Copy the outputs into the deployment folder (see below).

### 5. Tests (optional)

```powershell
python -m unittest discover -s tests -v
```

`test_firmware_manifest.py` checks the generated V67 payload against its pinned SHA-256 and always runs. `test_release.py` compiles the shared C engine, so it needs `cc` on `PATH` (gcc or clang — on Windows that means MinGW/MSYS2); when no compiler is available it **skips itself** instead of failing. Those are the tests that catch regressions in `kernel/gpu_unlock/`, the code the executable is built from.

---

## Deployment layout

There is no installer. Deploy as a portable folder; the executable resolves both the driver and the firmware relative to its own path:

```
gpu_unlock.exe
gpuunlock.sys
gpu-unlock\
└── 610.43.03\
    ├── booter_load_ga102.bin
    ├── v67-ga102-610.43.03.bin
    ├── gsp_ga10x.bin
    ├── gsp_boot_ga102.bin
    └── gsp_boot_desc_ga102.bin
```

- `gpuunlock.sys` must sit **next to** `gpu_unlock.exe` (or pass `--driver PATH`).
- The firmware directory is resolved as `<exe-dir>\gpu-unlock\610.43.03` (or pass `--assets DIR`).
- Without the firmware directory, `list`, `verify` and `real --probe` still work; a real unlock refuses with `real firmware is required; place the 610.43.03 blobs ...`.
- The two build scripts only write into `build\`; assembling this folder is a manual copy. Nothing is installed system-wide.

---

## Usage

> Run `gpu_unlock.exe list` first, then `verify`, then `real --probe`, and only then a real unlock.

### 1. Enumerate real GPUs

```powershell
gpu_unlock.exe list
```

Prints each NVIDIA PCI function with its BDF, vendor:device, `supported` / `ignored` status, bound service, and the simulated target.

### 2. Self-test with the simulator

```powershell
gpu_unlock.exe verify
gpu_unlock.exe verify --resume
```

Runs the full transaction against the simulated GA104. `--resume` also simulates a suspend/resume cycle. No hardware, no driver needed.

### 3. Read-only probe of real hardware

```powershell
gpu_unlock.exe real --probe
gpu_unlock.exe real --probe --bdf 0000:01:00.0
```

Loads the driver and dumps `PMC_BOOT_0`, `GFW_CPUCTL`, `GFW_BOOT`, `GFW_PLM`, `SS0`, `SS1`, `WPR2` lo/hi, FB size and VGA. **Writes nothing.** Needs elevation and `gpuunlock.sys`, but not the firmware. If WPR2 is up, the card was already initialised since power-on and the cold path will refuse it.

### 4. Real unlock

```powershell
# Interactive: disables the adapter, asks you to type YES
gpu_unlock.exe real

# Non-interactive, specific card
gpu_unlock.exe real --bdf 0000:01:00.0 --yes

# Card already initialised by the driver: clear GSP first
gpu_unlock.exe real --pre-flr

# Adapter was disabled before boot and WPR2 is down
gpu_unlock.exe real --cold
```

A successful run ends with:

```
unlock read-back: PASS
```

### 5. One-click (double-click) path

Launching `gpu_unlock.exe` with **no arguments** requests elevation, installs/loads the driver if missing, unlocks the first supported card, and pauses before exit so the result stays visible.

It is exactly `gpu_unlock.exe real` plus the pause, so it cannot select a card, suppress the `YES` prompt or clear a stale GSP. Use the console form when you need any of those. Step-by-step behaviour is in [How to run](#how-to-run).

> Double-clicking is convenient but opaque. Prefer the CLI so you can see `--probe` state and choose `--pre-flr` / `--cold` explicitly.

### 6. Version and help

```powershell
gpu_unlock.exe version
gpu_unlock.exe help
```

---

## Command-line reference

```
gpu_unlock.exe [command] [options]
```

### Commands

| Command                                                                                            | Description                                                              |
| -------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------ |
| *(none)*                                                                                           | One-click: elevate → ensure driver → unlock first supported card → pause |
| `list`                                                                                             | Enumerate NVIDIA PCI devices via SetupAPI                                |
| `verify [--resume]`                                                                                | Run the transaction on the simulated GA104                               |
| `real [--probe] [--bdf BDF] [--assets DIR] [--driver PATH] [--yes] [--quiet] [--cold] [--pre-flr]` | Unlock real hardware                                                     |
| `version`                                                                                          | Print version                                                            |
| `help`, `--help`, `-h`                                                                             | Show usage                                                               |


### Options

| Option | Applies to | Meaning |
|---|---|---|
| `--bdf BDF` | `real` | PCI function to target, e.g. `0000:01:00.0`. Full PCI name, case-insensitive, independent of GPU numbering |
| `--assets DIR` | `real`, `verify` | Load real `610.43.03` blobs from `DIR` instead of the default `gpu-unlock\610.43.03` |
| `--driver PATH` | `real` | Install/start the given driver instead of `gpuunlock.sys` next to the exe |
| `--yes` | `real` | Skip the interactive `YES` confirmation. Implied by `--cold`, which has no adapter to disable and therefore no prompt |
| `--quiet` | `real`, `verify` | Suppress informational log lines |
| `--cold` | `real` | Do not disable the adapter: it was disabled before boot, so WPR2 is down and the cold path applies. Also implies `--yes` |
| `--pre-flr` | `real` | Reset an already-initialised GSP with a FLR before unlocking |
| `--probe` | `real` | Read-only register dump; never writes. Still needs elevation, because it opens the driver |
| `--resume` | `verify` | Also simulate post-resume unlocking |

### Exit status

| Code | Meaning |
|---|---|
| `0` | Success (for `real`: read-back `PASS`) |
| `1` | Failure (driver error, no supported card, unlock read-back `FAIL`) |
| `2` | Bad command line |

---

## Supported GPUs

The engine accepts the following NVIDIA PCI device IDs (`supported_ids[]` in `kernel/gpu_unlock/main.c`). It excludes audio functions and the professional (GL) parts, which already run BF16 at full rate; GA103 and GA106 also exclude mobile parts where noted.

| Chip | Accepted device IDs |
|---|---|
| **GA102** | `2200` `2203` `2204` `2205` `2206` `2207` `2208` `220a` `220d` `2216` `222b` `222f` |
| **GA104** | `2480` `2482` `2483` `2484` `2486` `2487` `2488` `2489` `248a` `248c` `248d` `248e` `249c` `249d` `249f` `24a0` `24a4` `24ac` `24ad` `24af` `24bf` `24c0` `24c7` `24c8` `24c9` `24dc` `24dd` `24df` `24e0` |
| **GA103** | `2414` (untested) |
| **GA106** | `2501` `2503` `2504` `2505` `2507` `2508` `2509` `252f` `2544` |
| **GA106M** | `2520` `2560` |

**Accepted ≠ validated.** Community-reported results:

| Chip | Card | PCI ID | Result | BF16 GEMM gain |
|---|---|---|---|---|
| GA102 | RTX 3090 Ti | `10de:2203` | Unlocked | +68–80% |
| GA102 | RTX 3090 | `10de:2204` | Unlocked | +25–67% |
| GA102 | RTX 3080 Ti 20 GB (OEM) | `10de:2205` | Completes, **BF16 stays at half rate** | — |
| GA102 | RTX 3080 10 GB | `10de:2206` | Unlocked | +70–73% |
| GA102 | RTX 3080 20 GB | `10de:2206` | Unlocked | +56–59% |
| GA104 | RTX 3070 | `10de:2484` | Unlocked | +73–81% |
| GA104 | RTX 3070 LHR | `10de:2488` | Unlocked | +77–84% |
| GA104 | RTX 3070 Ti | `10de:2482` | **Not tested with this module** (worked from a UEFI port) | — |
| GA103 | any | `10de:2414` | **No reports** | — |
| GA106 | RTX 3060 12 GB | `10de:2504` | Unlocked | ≈ +80% |
| GA106M | RTX 3060 Laptop 6 GB | `10de:2560` | Unlocked | +56–60% |

**Not supported:** RTX 40/50 series (Ada/Blackwell), Turing (RTX 20 series), and professional RTX A / Quadro parts.

**About the 3070:** the simulated backend models an RTX 3070 (GA104, `10de:2484`, BDF `0000:21:00.0`) because that is the board whose cold-state registers were captured. It is a **test fixture**, not a statement that the tool only supports the 3070. `list` always prints a fixed "simulated target" line; it does not describe your GPU.

---

## Firmware

The real unlock loads five blobs from `gpu-unlock\610.43.03\`:

| File | Role |
|---|---|
| `booter_load_ga102.bin` | SEC2 Booter image |
| `v67-ga102-610.43.03.bin` | V67/PLM patched firmware image |
| `gsp_ga10x.bin` | GSP firmware (ELF with `.fwimage` / `.fwsignature_ga10x`) |
| `gsp_boot_ga102.bin` | GSP boot image |
| `gsp_boot_desc_ga102.bin` | GSP boot descriptor |

They are derived from pinned NVIDIA inputs whose SHA-256 is recorded in `firmware/artifacts-610.43.03.json`. Generation is version-pinned to `610.43.03`; a different driver version requires re-deriving the layout.

For `verify`, the executable does **not** need these files: `assets_builtin.c` synthesizes blobs with the same wire geometry, containing no NVIDIA firmware.

---

## Architecture

```
                 ┌─────────────────────────────┐
                 │        gpu_unlock.exe       │  user mode
                 │  CLI · HAL · backends       │
                 │  ┌───────────┐ ┌──────────┐ │
                 │  │ simulation│ │  real    │ │
                 │  │ GA104/3070│ │ via IOCTL│ │
                 │  └───────────┘ └────┬─────┘ │
                 │  kernel engine (main.c,     │
                 │  unlock/prep/layout/vbios/  │
                 │  gfw_eval) — unchanged      │
                 └──────────────┬──────────────┘
                                │ DeviceIoControl
                 ┌──────────────▼──────────────┐
                 │        gpuunlock.sys        │  kernel mode
                 │  config R/W · BAR map       │
                 │  MMIO R/W · DMA · FLR · D0  │
                 └──────────────┬──────────────┘
                                │
                       ┌────────▼────────┐
                       │   NVIDIA GPU    │
                       └─────────────────┘
```

| Path | Responsibility |
|---|---|
| `windows/gpu_unlock_win.c` | CLI, UAC, service management, device enable/disable |
| `windows/hal.c`, `windows/hal.h` | Linux-kernel-shaped service layer; dispatches sim vs. real |
| `windows/backend_win32.c` | SetupAPI enumeration, `DICS_DISABLE` / `DICS_ENABLE` |
| `windows/backend_real.c` | Forwards PCI/MMIO/DMA/FLR to the driver |
| `windows/backend_sim.c` | In-process GA104 register model |
| `windows/assets_builtin.c` | Synthetic assets; real-blob loader |
| `windows/driver/gpuunlock_drv.c` | WDM driver; mechanical operations only |
| `windows/driver/gpuunlock_ioctl.h` | Shared IOCTL ABI |
| `windows/include/linux/*.h` | Linux API shims for the reused engine |
| `kernel/gpu_unlock/*.c` | Shared unlock engine (also used by the Linux module) |

---

## Safety, risk and warranty

This tool **writes undocumented GPU registers and resets the PCIe function**. Treat it as a hardware modification.

- **The screen goes blank** while the display adapter is disabled. It is re-enabled afterwards, but if re-enable fails you must use Device Manager.
- **The unlock lasts only for the current power cycle.** A power loss resets SS0/SS1. The Windows build does **not** auto-run at boot or after resume; re-run it when needed.
- **Unsigned test driver.** Enabling `testsigning` lowers the machine's code-integrity posture. Disable it with `bcdedit /set testsigning off` when done.
- **If the firmware never stops**, the module keeps the DMA buffers that firmware may still use until the next power-off. **Power off before retrying.**
- **Warranty and stability.** Register-level modification may void the warranty and can leave the card in a bad state. Validate on a spare/secondary card first, never on your only display adapter.
- **No persistence guarantee.** The tool does not survive sleep/hibernate in the way the Linux initramfs module does.

---

## Troubleshooting / FAQ

**`cannot start the gpuunlock driver (Win32 error 577)` / `ERROR_INVALID_IMAGE_HASH`**
The driver is not signed, or test signing is off. Run `windows\sign_driver.ps1 -EnableTestSigning` and **reboot**.

**`bcdedit` refuses to set `testsigning`**
Secure Boot is on. Windows will not accept test-signing policy while Secure Boot is enforced, so the self-signed driver can never load. Disable Secure Boot in the UEFI firmware setup first, then re-run the signing script and reboot.

**`cannot start the gpuunlock driver (Win32 error 5)` / access denied**
Not elevated. Run from an Administrator console.

**`gpuunlock.sys was not found next to gpu_unlock.exe` (error 2)**
Place `gpuunlock.sys` beside `gpu_unlock.exe`, or pass `--driver PATH`.

**`real firmware is required; place the 610.43.03 blobs ...`**
The `gpu-unlock\610.43.03` folder is missing or incomplete. Generate the five blobs and copy them next to the exe.

**"cold path will refuse it" / WPR2 is up**
The card was already initialised by the driver since power-on. Use `--pre-flr`, or disable the adapter in Device Manager **before boot** and use `--cold`.

**`unlock read-back: FAIL`**
The transaction ran but SS0/SS1 did not read back as expected. Reboot, then retry with `--pre-flr`. On some boards (e.g. 3080 Ti 20 GB) the unlock completes without a BF16 gain.

**Why does `verify` say RTX 3070?**
`verify` uses the built-in simulated GA104. It is a fixture, not your hardware.

**Does it work on RTX 40/50 or Turing?**
No. Only the GA102/GA103/GA104/GA106 IDs listed above.

**Can I use a display card?**
Yes. It blanks the screen during the unlock and re-enables the adapter afterwards. Prefer a secondary card if you can.

**Does it patch the NVIDIA driver?**
No. Nothing on disk is modified; the unlock happens before `nvlddmkm` binds and is not driver-version-specific in the patching sense.

**Does the unlock persist across reboot?**
No. Power loss clears the selectors. Re-run the tool after a cold boot.

**Multiple GPUs?**
One card per invocation. Auto-selects the first supported card, or pass `--bdf` and run once per card.

**Is my warranty affected?**
Possibly. This is an unsupported hardware modification; proceed at your own risk.

**How do I remove the tool completely?**
Delete the folder, then remove the kernel service the first real run registered (elevated): `sc.exe stop GpuUnlock` and `sc.exe delete GpuUnlock`. Deleting the folder alone leaves the service pointing at a missing file. Undo test signing with `bcdedit /set testsigning off` if you no longer need it.

---

## Known limitations

- **Test-signed driver only** — not WHQL-signed; requires `testsigning`.
- **No boot-time or auto-resume hook** — unlike the Linux initramfs module, the Windows build is invoked manually.
- **Cold-path constraint** — a card initialised since power-on is refused unless you use `--pre-flr` or `--cold` after a pre-boot disable.
- **Display adapter downtime** — the screen blanks while the adapter is disabled.
- **One card per run** — no single-command multi-card sweep.
- **Version-pinned firmware** — offsets and blobs target `610.43.03`; other versions need re-derivation.
- **Varying efficacy** — some accepted IDs (GA103, 3080 Ti 20 GB) are untested or show no gain; power limits strongly affect results.
- **Laptop GPUs** — GA106M is accepted, but mobile behaviour may differ from desktop reports.
- **The Linux module no longer builds** — `kernel/gpu_unlock/` is the shared unlock engine and is what this port compiles, but the kbuild files, `dkms.conf` and all three packaging trees were removed. [`docs/README.linux.md`](docs/README.linux.md) is archived documentation, and [`docs/linux-legacy/`](docs/linux-legacy/) holds the removed build files. Use upstream [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock) if you need a working Linux module.

---

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the full guide. In short:

1. **Build and test locally** before opening a PR:
   ```powershell
   powershell -ExecutionPolicy Bypass -File windows\build.ps1
   powershell -ExecutionPolicy Bypass -File windows\build_driver.ps1
   python -m unittest discover -s tests -v
   ```
2. **Match the existing style** — kernel/Linux conventions, tabs, `SPDX-License-Identifier: GPL-2.0-only` on every new file.
3. **Keep policy in user mode.** New hardware access goes through the IOCTL ABI; do not add unlock logic to the driver.
4. **Do not commit** NVIDIA firmware, generated `.bin` blobs, build outputs (`build/`), certificates, or signed binaries.
5. **Compatibility reports** are valuable — include chip, board, VBIOS, driver version, `list` output, `--probe` dump, and read-back result.
6. **Security issues** — follow [`SECURITY.md`](SECURITY.md); do not open a public issue.

---

## License and attribution

- **Source code:** GPL-2.0-only. See [`licenses/GPL-2.0.txt`](licenses/GPL-2.0.txt).
- **Generated firmware** derives from the pinned CMP reference; NVIDIA firmware remains under its own terms. See [`firmware/NOTICE.md`](firmware/NOTICE.md) and [`licenses/`](licenses/).
- **Upstream:** [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock) — the original Linux kernel module and the V67/PLM technique.
- **Third-party notices:** [`licenses/CMP40HX-MIT.txt`](licenses/CMP40HX-MIT.txt), [`licenses/NVIDIA-MIT.txt`](licenses/NVIDIA-MIT.txt).

> NVIDIA, GeForce, RTX and related marks are trademarks of NVIDIA Corporation. This project is not affiliated with or endorsed by NVIDIA.
