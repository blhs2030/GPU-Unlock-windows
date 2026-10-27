# SPDX-License-Identifier: GPL-2.0-only
#
# Linux-side helpers: the shared firmware pipeline and the Python test suite.
#
# The distribution packaging and kbuild install path was removed from this
# repository together with `debian/`, `packaging/` and
# `kernel/gpu_unlock/{Kbuild,Makefile,dkms.conf}`. The targets that drove it
# (`dist`, `deb`, `rpm`, `arch`, `install`, `install-dkms`,
# `install-mkinitcpio`, `install-initramfs-tools`, `install-dracut`) are gone.
# The original file is archived in `docs/linux-legacy/Makefile` if you need to
# restore Linux packaging from upstream.
VERSION    := $(shell cat VERSION)
NV         := 610.43.03
PYTHON     ?= python3
DESTDIR    ?=
CACHE      ?= $(HOME)/.cache/gpu-unlock
INPUTS     ?= $(CACHE)/inputs
ASSETS     ?= $(CACHE)/assets
FWDIR      ?= /usr/lib/firmware/gpu-unlock/$(NV)
LICENSEDIR ?= /usr/share/licenses
PKG        ?= gpu-unlock

FIRMWARE_FILES := booter_load_ga102.bin gsp_ga10x.bin gsp_boot_ga102.bin \
                  gsp_boot_desc_ga102.bin v67-ga102-$(NV).bin
LICENSES       := licenses/GPL-2.0.txt licenses/CMP40HX-MIT.txt licenses/NVIDIA-MIT.txt
NV_SOURCE      := NVIDIA-kernel-module-source-$(NV)
NV_UTILS       := nvidia-utils-$(NV)-1-x86_64_v3.pkg.tar.zst

.PHONY: all test inputs firmware assets install-firmware install-licenses

all:

# The C component tests need a C compiler and skip themselves when none is on
# PATH; the firmware-manifest test always runs.
test:
	GPU_UNLOCK_ASSETS=$(ASSETS) $(PYTHON) -m unittest discover -s tests -v

# Download and SHA-256 check the pinned NVIDIA inputs.
inputs:
	$(PYTHON) firmware/fetch_artifacts.py $(INPUTS)

# Generate firmware from $(INPUTS) into $(ASSETS). The generators re-check
# every input against its pinned hash before writing anything.
firmware:
	rm -rf $(ASSETS).src
	mkdir -p $(ASSETS).src
	tar -xf $(INPUTS)/$(NV_SOURCE).tar.xz -C $(ASSETS).src
	$(PYTHON) firmware/generate_v67.py "$$(realpath $(INPUTS)/$(NV_UTILS))" $(ASSETS)
	$(PYTHON) firmware/extract_boot_assets.py $(ASSETS).src/$(NV_SOURCE) $(ASSETS)

assets: inputs
	$(MAKE) firmware

# Stage the blobs next to an install; the Windows build copies them into
# `gpu-unlock\610.43.03\` beside the executable instead.
install-firmware:
	install -Dm644 -t $(DESTDIR)$(FWDIR) $(addprefix $(ASSETS)/,$(FIRMWARE_FILES))
	install -Dm644 $(ASSETS)/NVIDIA-LICENSE $(DESTDIR)$(LICENSEDIR)/gpu-unlock-firmware/NVIDIA-LICENSE
	install -Dm644 firmware/NOTICE.md $(DESTDIR)$(LICENSEDIR)/gpu-unlock-firmware/NOTICE.md

install-licenses:
	install -Dm644 -t $(DESTDIR)$(LICENSEDIR)/$(PKG) $(LICENSES)
