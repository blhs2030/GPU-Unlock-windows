# Security Policy

`gpu-unlock for Windows` ships a kernel driver (`gpuunlock.sys`) that exposes low-level PCI, MMIO, DMA and FLR primitives to a user-mode process. That makes its attack surface worth treating seriously. This document explains what is in scope and how to report a problem responsibly.

## Supported versions

| Version | Supported |
|---|---|
| `1.0.x` | Yes |
| Older / unversioned builds | No |

Only the current `windows/` tree is maintained. Rebuild from `HEAD` before reporting; do not test against a stale binary.

## Reporting a vulnerability

**Do not open a public issue for a security problem.**

Use GitHub's private vulnerability reporting:

1. Open the repository's **Security** tab.
2. Choose **Report a vulnerability**.
3. Include: affected component, version/commit, reproduction steps, impact, and any PoC.

If private reporting is unavailable, contact the maintainer through the repository's listed contact channel and clearly mark the message as a security report. Please do not attach proprietary firmware or NVIDIA binaries.

### What to expect

| Stage | Target |
|---|---|
| Acknowledgement | within 72 hours |
| Initial assessment | within 7 days |
| Fix or mitigation plan | within 90 days, coordinated with you |
| Public disclosure | after a fix ships, by mutual agreement |

We will credit reporters unless you ask to remain anonymous.

## Scope

In scope:

- `windows/driver/gpuunlock_drv.c` — WDM driver behavior.
- `windows/driver/gpuunlock_ioctl.h` — IOCTL ABI (validation, integer overflow, size handling).
- `windows/gpu_unlock_win.c` — UAC flow, service creation, device enable/disable, asset path handling.
- `windows/backend_real.c`, `backend_win32.c`, `backend_sim.c`, `assets_builtin.c`, `hal.c` — input validation, bounds, memory safety.
- `windows/build.ps1`, `build_driver.ps1`, `sign_driver.ps1` — command injection, unsafe path handling, certificate handling.
- `firmware/*.py` — download verification, archive extraction, path traversal, SHA-256 enforcement.

Out of scope (by design, not vulnerabilities):

- Requiring Administrator rights to load the driver or touch PCI config space.
- Requiring `testsigning` and a self-signed certificate. This is the documented deployment model, not a bypass.
- Physical access, DMA attacks, or an attacker who can already write kernel memory.
- Bricking or instability caused by deliberately misusing the unlock on unsupported hardware.
- Missing WHQL/Microsoft signature. The project is test-signed only.
- Vulnerabilities in NVIDIA's closed driver, firmware, or WDK/SDK themselves (report those upstream).

## Threat model and design intent

- The driver is **mechanical only**. It must never contain unlock policy, target selection, or asset parsing.
- Every IOCTL input is treated as untrusted: validate size, offset, BDF, BAR index, DMA handle and length; reject on overflow.
- The user-mode process is the trust boundary for policy. A local administrator is already trusted; the goal is to avoid turning a bug into arbitrary kernel read/write for a *less* privileged caller.
- Firmware and assets are verified against pinned SHA-256 before use. Paths are resolved relative to the executable, never from untrusted input without validation.
- Test signing is a usability trade-off, not a security boundary. Do not describe it as one.

## Hardening expectations for contributors

- Keep `/GS-` and `BufferOverflowK.lib` behavior intact; do not weaken compiler mitigations without justification.
- Do not add IOCTLs that accept raw physical addresses or arbitrary kernel pointers.
- Bump `GU_DRIVER_VERSION` and keep the IOCTL ABI backward-compatible, or version it explicitly.
- New parsing code needs bounds checks and unit tests. Prefer fixed-size buffers and explicit length fields.

## Safe harbor

We will not pursue legal action against researchers who:

- Act in good faith and follow this policy,
- Avoid privacy violations, data destruction, and service disruption,
- Give us reasonable time to fix the issue before disclosure,
- Do not exploit the finding beyond what is needed to demonstrate it.

## Related documents

- [`README.md`](README.md) — usage, risks and warranty.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — build, test and review expectations.
- [`../licenses/GPL-2.0.txt`](licenses/GPL-2.0.txt) — license.
