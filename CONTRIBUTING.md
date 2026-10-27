# Contributing to gpu-unlock for Windows

Thanks for helping. This port has a small, sharp surface — a user-mode CLI plus a kernel driver — so changes need to respect a few hard rules. Read this before opening a PR.

## Ground rules

1. **License:** every new file starts with `/* SPDX-License-Identifier: GPL-2.0-only */` (Python: `# SPDX-License-Identifier: GPL-2.0-only`). Contributions are accepted under GPL-2.0-only.
2. **Do not break the shared engine.** `kernel/gpu_unlock/*.c` is compiled by both the Linux module and this port. Changes there must keep both building and passing `tests/`.
3. **Policy stays in user mode.** The driver performs mechanical operations only. Never move target selection, asset parsing, or unlock decisions into `gpuunlock.sys`.
4. **No vendored binaries.** Do not commit NVIDIA firmware, generated `.bin` blobs, `build/`, certificates, or signed drivers.

## Architecture map

| Layer | Files | Rule |
|---|---|---|
| CLI | `gpu_unlock_win.c` | Argument parsing, UAC, service lifecycle, confirmation UX |
| Service layer | `hal.c`, `hal.h` | Linux-shaped API; dispatch between real and simulated backends |
| Real backend | `backend_real.c` | Forwards to `gpuunlock.sys` via IOCTL; mirrors DMA buffers |
| Win32 backend | `backend_win32.c` | SetupAPI enumeration, `DICS_DISABLE` / `DICS_ENABLE` |
| Simulated backend | `backend_sim.c` | In-process GA104 register model; no hardware |
| Assets | `assets_builtin.c` | Synthetic blobs; real-blob loader with path handling |
| Driver | `driver/gpuunlock_drv.c` | Mechanical PCI config / BAR / MMIO / DMA / FLR / D0 |
| ABI | `driver/gpuunlock_ioctl.h` | Versioned IOCTL contract |
| Shim | `include/linux/*.h` | Linux API subset for the reused engine |

## Build and test

```powershell
# user-mode executable
powershell -ExecutionPolicy Bypass -File windows\build.ps1

# kernel driver (requires WDK)
powershell -ExecutionPolicy Bypass -File windows\build_driver.ps1

# unit tests
python -m unittest discover -s tests -v
```

The C component tests (`test_release.py`) compile `kernel/gpu_unlock/` and therefore need `cc` on `PATH` — gcc or clang, which on Windows means MinGW/MSYS2. Without a compiler they skip themselves; CI and Linux reviewers are expected to run them for real.

A PR is expected to:

- build cleanly with MSVC at `/W3` (no new warnings),
- pass the unit tests,
- pass `gpu_unlock.exe verify` and `gpu_unlock.exe verify --resume` on the simulated card,
- not regress the shared engine in `kernel/gpu_unlock/` — the Windows executable is compiled from it, and the C component tests are the guard when a compiler is available.

### Testing on real hardware (optional but valuable)

Only if you accept the risk on a spare card:

```powershell
gpu_unlock.exe list
gpu_unlock.exe real --probe
gpu_unlock.exe real --pre-flr      # or --cold after a pre-boot disable
```

Include in the PR: chip, board model, VBIOS version, NVIDIA driver version, `list` output, `--probe` dump, and the read-back result.

## Coding style

- **C11**, kernel/Linux conventions: tabs for indentation, `snake_case`, `u8/u16/u32/u64` from `compat.h`, explicit brace style as in existing files.
- Keep the diff focused. Do not reformat unrelated code.
- Prefer fixed-size buffers and explicit lengths. Check every length and offset before use.
- Avoid C++ constructs, VLAs, and compiler-specific extensions beyond what MSVC already accepts.
- Errors: return negative `errno` values on the user-mode side; NTSTATUS on the driver side. Do not mix them.
- Comments explain *why*, especially where a register offset or magic value is non-obvious.

## IOCTL ABI changes

- Bump `GU_DRIVER_VERSION` in `gpuunlock_ioctl.h`.
- Keep existing IOCTL numbers and struct layouts stable, or version them.
- Validate `size`, `offset`, `bus/device/function`, `bar`, `handle` and `length` in the driver. Reject on overflow.
- Never accept a raw physical address or arbitrary kernel pointer from user mode.

## Commit and pull requests

- One logical change per commit. Describe the *what* and *why* in the message.
- Reference the issue or compatibility report when applicable.
- Update `README.md` and `README.zh-CN.md` when behavior, flags, or requirements change.
- Do not `--amend` a force-push over review history; add commits.
- CI or a maintainer will run the build and tests; fix failures before requesting re-review.

## Compatibility reports

A good report is the most useful contribution you can make. Use this template:

```
Chip:            GA104
Card:            RTX 3070 (Palit GamingPro)
PCI ID:          10de:2484
VBIOS:           <version>
NVIDIA driver:   610.43.03
Command:         gpu_unlock.exe real --pre-flr
list output:     <paste>
--probe output:  <paste>
read-back:       PASS / FAIL
BF16 GEMM before/after: <numbers, if measured>
Notes:           <screen blank duration, re-enable result, stability>
```

## Security

Do not open a public issue for a security problem. Follow [`SECURITY.md`](SECURITY.md).

## What not to commit

- NVIDIA firmware or any generated `*.bin`
- `build/` outputs (`*.obj`, `*.exe`, `*.sys`)
- Test-signing certificates or private keys
- Local machine paths, logs containing hardware serial numbers
- Large binaries, archives, or cached NVIDIA sources

## Questions

Open a discussion or an issue with the `question` label. For hardware-specific behavior, include the output of `gpu_unlock.exe list` and `gpu_unlock.exe real --probe`.

## License

By contributing, you agree that your contributions are licensed under GPL-2.0-only, consistent with [`../licenses/GPL-2.0.txt`](licenses/GPL-2.0.txt).
