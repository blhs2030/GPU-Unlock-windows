# Archived Linux build/packaging files

These files were removed from the live tree when this repository was narrowed
to the Windows port. They are kept here **for reference only** — nothing in the
build, the test suite or the Windows executable reads them.

| File | What it was |
|---|---|
| `Makefile` | The original top-level orchestrator: release tarball, Debian/RPM/Arch packages, DKMS + initramfs install targets, firmware pipeline |
| `test_packaging.py` | The original packaging test: install layout via `make install-*`, PKGBUILD/rpm spec/changelog version pins |

## Why they cannot run here

Both depend on files that no longer exist in this repository:

- `debian/` — Debian/Ubuntu packaging (`control`, `rules`, `changelog`, `dkms`, maintainer scripts)
- `packaging/` — Arch `PKGBUILD`, RPM spec, and the mkinitcpio / initramfs-tools / dracut drop-ins
- `kernel/gpu_unlock/Kbuild`, `kernel/gpu_unlock/Makefile`, `kernel/gpu_unlock/dkms.conf` — the kbuild and DKMS metadata for the Linux module
- `scratchpad/` — a local mirror of the NVIDIA 610.43.03 kernel sources, never referenced by the build

## What is still live

- `kernel/gpu_unlock/*.c|*.h` — **kept.** This is the shared unlock engine; `windows/build.ps1` compiles it directly and `windows/gpu_unlock_win.c` includes `main.c`.
- `firmware/` — **kept.** Cross-platform Python that regenerates the five `610.43.03` blobs the real unlock needs.
- `tests/test_release.py` — **kept.** Compiles the shared engine and skips when no C compiler is available.
- `tests/test_firmware_manifest.py` — **kept.** The payload-pin check salvaged from `test_packaging.py`.
- The trimmed top-level `Makefile` — **kept.** Only `test`, `inputs`, `firmware`, `assets`, `install-firmware`, `install-licenses`.

## Restoring a working Linux module

Use upstream [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock): it
still carries the kbuild files, `dkms.conf` and all three packaging trees. The
Linux documentation in [`../README.linux.md`](../README.linux.md) describes the
original behaviour and is historical for the same reason.
