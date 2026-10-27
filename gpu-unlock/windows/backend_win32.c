/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Read-only enumeration of real PCI display devices through SetupAPI.
 *
 * This is the part of the Linux module that is genuinely portable: listing
 * which Ampere cards are present does not need BAR or configuration-space
 * access. The unlock transaction itself stays simulated, because a Windows
 * user-mode process cannot map BAR0 or displace the bound nvlddmkm driver.
 */
#include <windows.h>
#include <setupapi.h>
#include "hal.h"

static void wide_to_ansi(const wchar_t *wide, char *out, size_t out_size)
{
	int n;

	if (!out_size)
		return;
	out[0] = '\0';
	if (!wide)
		return;
	n = WideCharToMultiByte(CP_ACP, 0, wide, -1, out, (int)out_size, NULL, NULL);
	if (n <= 0)
		out[0] = '\0';
}

static void property_string(HDEVINFO set, SP_DEVINFO_DATA *info, DWORD property,
			    char *out, size_t out_size)
{
	wchar_t buffer[512];
	DWORD size = 0;

	out[0] = '\0';
	if (!SetupDiGetDeviceRegistryPropertyW(set, info, property, NULL,
					      (PBYTE)buffer, sizeof(buffer), &size))
		return;
	wide_to_ansi(buffer, out, out_size);
}

static int property_dword(HDEVINFO set, SP_DEVINFO_DATA *info, DWORD property,
			  DWORD *out)
{
	DWORD size = 0;

	return SetupDiGetDeviceRegistryPropertyW(set, info, property, NULL,
						 (PBYTE)out, sizeof(*out), &size) &&
	       size == sizeof(*out);
}

/* Numeric location properties work in every locale and survive a DICS_DISABLE,
 * unlike the localized SPDRP_LOCATION_INFORMATION string. */
static void device_bdf(HDEVINFO set, SP_DEVINFO_DATA *info, char *out,
		       size_t out_size)
{
	char location[256];
	unsigned int bus = 0, slot = 0, func = 0;

	out[0] = '\0';
	property_string(set, info, SPDRP_LOCATION_INFORMATION, location, sizeof(location));
	if (sscanf(location, "PCI bus %u, device %u, function %u",
		   &bus, &slot, &func) != 3) {
		DWORD numeric_bus = 0, address = 0;

		if (property_dword(set, info, SPDRP_BUSNUMBER, &numeric_bus)) {
			bus = numeric_bus;
			if (property_dword(set, info, SPDRP_ADDRESS, &address)) {
				slot = (address >> 16) & 0xffff;
				func = address & 0xffff;
			}
		}
	}
	if (bus || slot || func)
		snprintf(out, out_size, "0000:%02x:%02x.%u", bus, slot, func);
	else
		snprintf(out, out_size, "n/a");
}

int gu_win32_enum_gpus(struct gu_gpu_info *out, int max)
{
	HDEVINFO set;
	SP_DEVINFO_DATA info;
	DWORD index;
	int count = 0;

	if (!out || max <= 0)
		return 0;
	set = SetupDiGetClassDevsW(NULL, L"PCI", NULL,
				   DIGCF_PRESENT | DIGCF_ALLCLASSES);
	if (set == INVALID_HANDLE_VALUE)
		return 0;
	info.cbSize = sizeof(info);
	for (index = 0; count < max && SetupDiEnumDeviceInfo(set, index, &info);
	     index++) {
		char hardware[512];
		char description[256];
		char service[128];
		unsigned int vendor = 0, device = 0;
		struct gu_gpu_info *gpu = &out[count];

		property_string(set, &info, SPDRP_HARDWAREID, hardware, sizeof(hardware));
		if (sscanf(hardware, "PCI\\VEN_%4x&DEV_%4x", &vendor, &device) != 2)
			continue;
		if (vendor != PCI_VENDOR_ID_NVIDIA)
			continue;
		property_string(set, &info, SPDRP_DEVICEDESC, description, sizeof(description));
		property_string(set, &info, SPDRP_SERVICE, service, sizeof(service));
		device_bdf(set, &info, gpu->bdf, sizeof(gpu->bdf));
		gpu->vendor = (unsigned short)vendor;
		gpu->device = (unsigned short)device;
		snprintf(gpu->name, sizeof(gpu->name), "%s", description[0] ? description : "NVIDIA PCI device");
		snprintf(gpu->service, sizeof(gpu->service), "%s", service);
		count++;
	}
	SetupDiDestroyDeviceInfoList(set);
	return count;
}

/* ------------------------------------------------- device state change */
/* DICS_DISABLE unloads the bound nvlddmkm and leaves the function in D3hot;
 * the real backend brings PMCSR back to D0 before touching the BAR. */

static int find_device(const char *bdf, HDEVINFO *out_set,
		       SP_DEVINFO_DATA *out_info)
{
	HDEVINFO set;
	SP_DEVINFO_DATA info;
	DWORD index;
	char candidate[24];

	if (!bdf)
		return 0;
	/* DIGCF_ALLCLASSES without DIGCF_PRESENT also finds disabled functions,
	 * which is what re-enabling after an aborted unlock needs. */
	set = SetupDiGetClassDevsW(NULL, L"PCI", NULL, DIGCF_ALLCLASSES);
	if (set == INVALID_HANDLE_VALUE)
		return 0;
	info.cbSize = sizeof(info);
	for (index = 0; SetupDiEnumDeviceInfo(set, index, &info); index++) {
		device_bdf(set, &info, candidate, sizeof(candidate));
		if (!_stricmp(candidate, bdf)) {
			*out_set = set;
			*out_info = info;
			return 1;
		}
	}
	SetupDiDestroyDeviceInfoList(set);
	return 0;
}

static int change_device_state(const char *bdf, DWORD state)
{
	HDEVINFO set;
	SP_DEVINFO_DATA info;
	SP_PROPCHANGE_PARAMS params;

	if (!find_device(bdf, &set, &info))
		return -1;
	memset(&params, 0, sizeof(params));
	params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
	params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
	params.StateChange = state;
	params.Scope = DICS_FLAG_GLOBAL;
	params.HwProfile = 0;
	if (!SetupDiSetClassInstallParamsW(set, &info,
					   (SP_CLASSINSTALL_HEADER *)&params,
					   sizeof(params)) ||
	    !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &info)) {
		SetupDiDestroyDeviceInfoList(set);
		return -1;
	}
	SetupDiDestroyDeviceInfoList(set);
	return 0;
}

int gu_win32_disable_device(const char *bdf)
{
	return change_device_state(bdf, DICS_DISABLE);
}

int gu_win32_enable_device(const char *bdf)
{
	return change_device_state(bdf, DICS_ENABLE);
}
