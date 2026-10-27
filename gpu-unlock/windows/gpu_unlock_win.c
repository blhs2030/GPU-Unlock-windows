/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Windows console front end for the gpu-unlock kernel module.
 *
 * The unmodified kernel module source is included directly so the CLI can
 * drive its real entry points and target list. `verify` runs the simulated
 * GA104 backend, `real` (and the no-argument double-click path) runs the same
 * transaction against real hardware through gpuunlock.sys.
 */
#include <windows.h>
#include <winsvc.h>
#include <shellapi.h>
#include <wchar.h>
#include <conio.h>
#include "hal.h"
#include "../kernel/gpu_unlock/main.c"

#define GU_DRIVER_SERVICE L"GpuUnlock"
#define GU_DRIVER_FILE L"gpuunlock.sys"
#define GU_DRIVER_IMAGE_LINK L"\\\\.\\GpuUnlock"

static int supported(unsigned short device)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(supported_ids); i++)
		if (supported_ids[i] == device)
			return 1;
	return 0;
}

/* Double-clicking in Explorer gives the process its own console; a shell
 * keeps the console attached to cmd/PowerShell. Only pause in the former. */
static int interactive_launch(void)
{
	DWORD processes[2];
	DWORD mode;
	HANDLE input = GetStdHandle(STD_INPUT_HANDLE);

	if (!input || input == INVALID_HANDLE_VALUE || !GetConsoleMode(input, &mode))
		return 0;
	return GetConsoleProcessList(processes, (DWORD)ARRAY_SIZE(processes)) == 1;
}

static void pause_before_exit(void)
{
	printf("\nPress any key to exit...\n");
	fflush(stdout);
	_getch();
}

/* ------------------------------------------------------- real hardware */

static int is_elevated(void)
{
	HANDLE token = NULL;
	TOKEN_ELEVATION elevation;
	DWORD size = 0;
	int elevated = 0;

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return 0;
	if (GetTokenInformation(token, TokenElevation, &elevation,
				sizeof(elevation), &size))
		elevated = elevation.TokenIsElevated != 0;
	CloseHandle(token);
	return elevated;
}

/* Relaunch the same command line through the UAC prompt. */
static int relaunch_elevated(void)
{
	wchar_t exe[MAX_PATH];
	wchar_t args[2048];
	LPWSTR *argv;
	int argc = 0, i, length = 0;

	if (!GetModuleFileNameW(NULL, exe, (DWORD)ARRAY_SIZE(exe)))
		return 1;
	argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv)
		return 1;
	args[0] = L'\0';
	for (i = 1; i < argc; i++) {
		int written = swprintf(args + length,
				       ARRAY_SIZE(args) - (size_t)length,
				       L"\"%s\" ", argv[i]);

		if (written < 0)
			break;
		length += written;
	}
	LocalFree(argv);
	if ((INT_PTR)ShellExecuteW(NULL, L"runas", exe, length ? args : NULL,
				   NULL, SW_SHOWNORMAL) <= 32) {
		fprintf(stderr, "gpu_unlock: elevation was refused or failed; "
			"run this command from an administrator console\n");
		return 1;
	}
	return 0;
}

static int driver_path(wchar_t *out, size_t count)
{
	wchar_t *slash;

	if (!GetModuleFileNameW(NULL, out, (DWORD)count))
		return -1;
	slash = wcsrchr(out, L'\\');
	if (!slash ||
	    (size_t)(slash - out) + 1 + wcslen(GU_DRIVER_FILE) + 1 > count)
		return -1;
	wcscpy(slash + 1, GU_DRIVER_FILE);
	return 0;
}

/* Create (or reuse) and start the kernel service. Returns 0 or a Win32 error. */
static DWORD ensure_driver(const wchar_t *path)
{
	SC_HANDLE manager, service;
	DWORD error = 0;

	manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
	if (!manager)
		return GetLastError();
	service = CreateServiceW(manager, GU_DRIVER_SERVICE, GU_DRIVER_SERVICE,
				 SERVICE_START | SERVICE_QUERY_STATUS,
				 SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
				 SERVICE_ERROR_NORMAL, path, NULL, NULL, NULL,
				 NULL, NULL);
	if (!service) {
		error = GetLastError();
		if (error != ERROR_SERVICE_EXISTS) {
			CloseServiceHandle(manager);
			return error;
		}
		service = OpenServiceW(manager, GU_DRIVER_SERVICE,
				       SERVICE_START | SERVICE_QUERY_STATUS);
		if (!service) {
			error = GetLastError();
			CloseServiceHandle(manager);
			return error;
		}
	}
	if (!StartServiceW(service, 0, NULL)) {
		error = GetLastError();
		if (error == ERROR_SERVICE_ALREADY_RUNNING)
			error = 0;
	}
	CloseServiceHandle(service);
	CloseServiceHandle(manager);
	return error;
}

static void explain_service_error(DWORD error)
{
	fprintf(stderr, "gpu_unlock: cannot start the gpuunlock driver (Win32 error %lu)\n",
		(unsigned long)error);
	if (error == ERROR_INVALID_IMAGE_HASH || error == 577)
		fprintf(stderr, "  The driver is not signed. Enable test signing "
			"(bcdedit /set testsigning on), reboot, and sign it first.\n");
	else if (error == ERROR_ACCESS_DENIED)
		fprintf(stderr, "  Run this command elevated.\n");
	else if (error == 2)
		fprintf(stderr, "  gpuunlock.sys was not found next to gpu_unlock.exe.\n");
}

/* Select the first supported NVIDIA function SetupAPI reports. */
static int choose_bdf(char *out, size_t size)
{
	struct gu_gpu_info gpus[64];
	int count, i, found = 0;

	gu_hal_init();
	count = gu_win32_enum_gpus(gpus, (int)ARRAY_SIZE(gpus));
	printf("Real NVIDIA PCI devices:\n");
	for (i = 0; i < count; i++) {
		int usable = supported(gpus[i].device) &&
			     strcmp(gpus[i].bdf, "n/a") != 0;

		printf("  %-13s %04x:%04x  %-9s %s\n", gpus[i].bdf,
		       gpus[i].vendor, gpus[i].device,
		       usable ? "unlockable" : "ignored", gpus[i].name);
		if (usable && !found) {
			snprintf(out, size, "%s", gpus[i].bdf);
			found = 1;
		}
	}
	return found ? 0 : -1;
}

/* The real backend must never program synthetic firmware into hardware. */
static int real_assets_dir(char *out, size_t size)
{
	wchar_t exe[MAX_PATH], dir[MAX_PATH];
	wchar_t *slash;
	int n;

	if (!GetModuleFileNameW(NULL, exe, (DWORD)ARRAY_SIZE(exe)))
		return -1;
	slash = wcsrchr(exe, L'\\');
	if (!slash)
		return -1;
	*slash = L'\0';
	if (swprintf(dir, ARRAY_SIZE(dir), L"%s\\gpu-unlock\\610.43.03", exe) < 0)
		return -1;
	if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES)
		return -1;
	n = WideCharToMultiByte(CP_ACP, 0, dir, -1, out, (int)size, NULL, NULL);
	return n > 0 ? 0 : -1;
}

static int real_probe(const char *bdf)
{
	u8 *bar;
	u16 command = 0;
	u32 wpr_lo, wpr_hi;
	int ret = 1;

	gu_hal_set_real(1);
	if (gu_real_open(bdf)) {
		fprintf(stderr, "gpu_unlock: cannot open %s through gpuunlock.sys\n", bdf);
		gu_hal_set_real(0);
		return 1;
	}
	gu_real_read_config_word(NULL, PCI_COMMAND, &command);
	printf("config space: vendor=%04x device=%04x command=%04x\n",
	       gu_real_vendor(), gu_real_device(), command);
	bar = gu_real_iomap(NULL, 0, 0);
	if (!bar) {
		fprintf(stderr, "gpu_unlock: BAR0 could not be mapped\n");
	} else {
		wpr_lo = gu_real_readl(bar + 0x1fa824);
		wpr_hi = gu_real_readl(bar + 0x1fa828);
		printf("BAR0 mapped; read-only samples:\n"
		       "  PMC_BOOT_0   (+0x000000) = %08x\n"
		       "  GFW_CPUCTL   (+0x110100) = %08x\n"
		       "  GFW_BOOT     (+0x118234) = %08x\n"
		       "  GFW_PLM      (+0x823804) = %08x\n"
		       "  SS0          (+0x82381c) = %08x\n"
		       "  SS1          (+0x823820) = %08x\n"
		       "  WPR2 lo/hi   (+0x1fa824) = %08x:%08x\n"
		       "  FB size MiB  (+0x1183a4) = %08x\n"
		       "  VGA          (+0x625f04) = %08x\n",
		       gu_real_readl(bar + 0x000000),
		       gu_real_readl(bar + 0x110100),
		       gu_real_readl(bar + 0x118234),
		       gu_real_readl(bar + 0x823804),
		       gu_real_readl(bar + 0x82381c),
		       gu_real_readl(bar + 0x823820),
		       wpr_lo, wpr_hi,
		       gu_real_readl(bar + 0x1183a4),
		       gu_real_readl(bar + 0x625f04));
		if (wpr_hi != 0xffffffffu && (wpr_hi >> 4))
			printf("state: GSP/WPR2 is up (a driver has already used this card "
			       "since power on); the cold path will refuse it\n");
		else
			printf("state: WPR2 is down (card has not been initialised since "
			       "power on)\n");
		gu_real_iounmap(NULL, bar);
		ret = 0;
	}
	gu_real_close();
	gu_hal_set_real(0);
	return ret;
}

static int confirm_unlock(void)
{
	char line[16];

	printf("This disables the display adapter (the screen goes dark) and writes "
	       "GPU registers.\nType YES to continue: ");
	fflush(stdout);
	if (!fgets(line, sizeof(line), stdin))
		return 0;
	return !strcmp(line, "YES\n") || !strcmp(line, "YES\r\n") ||
	       !strcmp(line, "YES");
}

/* Disable the adapter, run the unmodified kernel unlock, then enable it again.
 * `cold` means the adapter was already left disabled across a reboot, so the
 * driver never initialised the GSP and WPR2 is down as the kernel module
 * expects. `pre_flr` clears an already-initialised GSP before the transaction.
 */
static int real_unlock(const char *bdf, int assume_yes, int cold, int pre_flr)
{
	int need_enable = cold, unlocked = 0, ret = 1;

	if (cold) {
		printf("%s is assumed disabled since boot (cold card)\n", bdf);
		assume_yes = 1;
	} else {
		if (!assume_yes && !confirm_unlock())
			return 1;
		printf("disabling %s (nvlddmkm will unload)\n", bdf);
		fflush(stdout);
		if (gu_win32_disable_device(bdf)) {
			fprintf(stderr, "gpu_unlock: cannot disable %s\n", bdf);
			return 1;
		}
		need_enable = 1;
		Sleep(1500);
	}

	gu_hal_set_real(1);
	if (gu_real_open(bdf)) {
		fprintf(stderr, "gpu_unlock: the driver cannot reach %s after disable\n", bdf);
		goto close_real;
	}
	if (pre_flr) {
		printf("pre-FLR to clear the initialised GSP state\n");
		fflush(stdout);
		if (gu_real_pre_flr()) {
			fprintf(stderr, "gpu_unlock: pre-FLR failed\n");
			goto close_real;
		}
	}
	targets = "all";
	resume = false;
	printf("running the unlock transaction\n");
	gpu_unlock_init();
	unlocked = gu_real_unlocked();
	gpu_unlock_exit();

close_real:
	gu_real_close();
	gu_hal_set_real(0);

	if (need_enable) {
		if (gu_win32_enable_device(bdf))
			fprintf(stderr, "gpu_unlock: WARNING: could not re-enable %s; "
				"use Device Manager to enable it\n", bdf);
		else
			printf("adapter re-enabled\n");
	}
	printf("unlock read-back: %s\n", unlocked ? "PASS" : "FAIL");
	ret = unlocked ? 0 : 1;
	return ret;
}

static int command_real(int probe, const char *bdf_opt, const char *assets,
			const wchar_t *driver, int assume_yes, int quiet,
			int cold, int pre_flr)
{
	char bdf[24];
	char default_assets[MAX_PATH];
	wchar_t default_driver[MAX_PATH];
	DWORD error;

	if (!is_elevated())
		return relaunch_elevated();
	gu_hal_init();
	gu_hal_set_quiet(quiet);

	if (!driver) {
		if (driver_path(default_driver, ARRAY_SIZE(default_driver)))
			driver = GU_DRIVER_FILE;
		else
			driver = default_driver;
	}
	error = ensure_driver(driver);
	if (error) {
		explain_service_error(error);
		return 1;
	}

	if (bdf_opt) {
		snprintf(bdf, sizeof(bdf), "%s", bdf_opt);
	} else if (choose_bdf(bdf, sizeof(bdf))) {
		fprintf(stderr, "gpu_unlock: no supported NVIDIA card was found; "
			"use --bdf to select a function\n");
		return 1;
	}
	printf("target: %s\n", bdf);

	if (probe)
		return real_probe(bdf);

	if (!assets) {
		if (real_assets_dir(default_assets, sizeof(default_assets))) {
			fprintf(stderr, "gpu_unlock: real firmware is required; place the "
				"610.43.03 blobs in gpu-unlock\\610.43.03 next to "
				"gpu_unlock.exe or pass --assets DIR\n");
			return 1;
		}
		assets = default_assets;
	}
	if (gu_assets_set_dir(assets)) {
		fprintf(stderr, "gpu_unlock: cannot use assets directory \"%s\"\n",
			assets);
		return 1;
	}
	return real_unlock(bdf, assume_yes, cold, pre_flr);
}

static void usage(void)
{
	printf("gpu-unlock for Windows (user-mode build)\n"
	       "\n"
	       "Usage:\n"
	       "  gpu_unlock.exe                         one-click: unlock the real card\n"
	       "                                         (UAC prompt, then type YES)\n"
	       "  gpu_unlock.exe list                    enumerate real NVIDIA PCI devices\n"
	       "  gpu_unlock.exe verify [--resume]       run the unlock on the simulated GA104\n"
	       "  gpu_unlock.exe real --probe [--bdf BDF]  read-only check through the driver\n"
	       "  gpu_unlock.exe real [--bdf BDF] [--assets DIR] [--yes]\n"
	       "                                         unlock the real card (elevated)\n"
	       "  gpu_unlock.exe real --cold [--bdf BDF] [--assets DIR]\n"
	       "                                         unlock a card disabled since boot\n"
	       "  gpu_unlock.exe version                 print version\n"
	       "  gpu_unlock.exe help                    this text\n"
	       "\n"
	       "Options:\n"
	       "  --assets DIR   load real 610.43.03 firmware blobs from DIR\n"
	       "  --bdf BDF      PCI function to use, e.g. 0000:01:00.0\n"
	       "  --driver PATH  gpuunlock.sys to install (default: next to the exe)\n"
	       "  --yes          skip the interactive confirmation\n"
	       "  --quiet        suppress informational log lines\n"
	       "  --cold         do not disable the adapter: it was disabled before boot,\n"
	       "                 so WPR2 is down and the kernel module's cold path applies\n"
	       "  --pre-flr      reset an already-initialised GSP before unlocking\n"
	       "\n"
	       "The no-argument run and `real` need gpuunlock.sys, test signing and real\n"
	       "610.43.03 firmware.\n"
	       "It disables the adapter (screen goes dark) and re-enables it afterwards.\n"
	       "A card the Windows driver has already initialised refuses the cold path;\n"
	       "use --cold after a reboot with the adapter left disabled, or --pre-flr.\n"
	       "The simulated card is a GA104 RTX 3070 (10de:2484).\n");
}

static int command_list(void)
{
	struct gu_gpu_info gpus[64];
	int count, i;

	gu_hal_init();
	count = gu_win32_enum_gpus(gpus, (int)ARRAY_SIZE(gpus));
	printf("Real NVIDIA PCI devices reported by SetupAPI: %d\n", count);
	for (i = 0; i < count; i++) {
		printf("  %-13s %04x:%04x  %-9s %s\n",
		       gpus[i].bdf, gpus[i].vendor, gpus[i].device,
		       supported(gpus[i].device) ? "supported" : "ignored",
		       gpus[i].name);
		if (gpus[i].service[0])
			printf("                bound service: %s\n", gpus[i].service);
	}
	printf("\nSimulated target: 0000:21:00.0 10de:2484  RTX 3070 (GA104) "
	       "- in-process register model, not real hardware\n");
	return 0;
}

static int command_verify(int resume_test, const char *assets, int quiet)
{
	int cold_ok, resume_ok = 1;

	gu_hal_init();
	gu_hal_set_quiet(quiet);
	gu_sim_init();
	if (assets) {
		int ret = gu_assets_set_dir(assets);

		if (ret) {
			fprintf(stderr, "gpu_unlock: cannot use assets directory \"%s\" (%d)\n",
				assets, ret);
			gu_sim_shutdown();
			return 1;
		}
	}
	targets = "all";
	resume = true;
	gpu_unlock_init();
	cold_ok = gu_sim_unlocked();
	printf("cold unlock (before the driver binds): %s\n", cold_ok ? "PASS" : "FAIL");
	if (resume_test) {
		gu_sim_prepare_resume();
		win_pm_resume();
		resume_ok = gu_sim_unlocked();
		printf("post-resume unlock (driver suspended): %s\n",
		       resume_ok ? "PASS" : "FAIL");
	}
	gpu_unlock_exit();
	gu_sim_shutdown();
	gu_assets_free();
	return cold_ok && resume_ok ? 0 : 1;
}

/* No arguments: the double-click path. Ask for UAC, install the driver if it is
 * missing, then unlock the real card and keep the console open so the result
 * stays readable. */
static int command_default(void)
{
	int ret;

	if (!is_elevated()) {
		int refused = relaunch_elevated();

		if (refused && interactive_launch())
			pause_before_exit();
		return refused;
	}
	printf("gpu-unlock one-click unlock (real hardware through gpuunlock.sys)\n\n");
	ret = command_real(0, NULL, NULL, NULL, 0, 0, 0, 0);
	if (interactive_launch())
		pause_before_exit();
	return ret;
}

int main(int argc, char **argv)
{
	const char *mode;
	const char *assets = NULL;
	const char *bdf = NULL;
	const char *driver_opt = NULL;
	wchar_t driver_wide[MAX_PATH];
	wchar_t *driver = NULL;
	int resume_test = 0, quiet = 0, probe = 0, assume_yes = 0;
	int cold = 0, pre_flr = 0;
	int i;

	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	if (argc < 2)
		return command_default();
	mode = argv[1];
	for (i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--resume")) {
			resume_test = 1;
		} else if (!strcmp(argv[i], "--quiet")) {
			quiet = 1;
		} else if (!strcmp(argv[i], "--assets") && i + 1 < argc) {
			assets = argv[++i];
		} else if (!strcmp(argv[i], "--bdf") && i + 1 < argc) {
			bdf = argv[++i];
		} else if (!strcmp(argv[i], "--driver") && i + 1 < argc) {
			driver_opt = argv[++i];
		} else if (!strcmp(argv[i], "--probe")) {
			probe = 1;
		} else if (!strcmp(argv[i], "--yes")) {
			assume_yes = 1;
		} else if (!strcmp(argv[i], "--cold")) {
			cold = 1;
		} else if (!strcmp(argv[i], "--pre-flr")) {
			pre_flr = 1;
		} else {
			fprintf(stderr, "gpu_unlock: unknown option \"%s\"\n", argv[i]);
			usage();
			return 2;
		}
	}
	if (!strcmp(mode, "list"))
		return command_list();
	if (!strcmp(mode, "verify"))
		return command_verify(resume_test, assets, quiet);
	if (!strcmp(mode, "real")) {
		if (driver_opt) {
			if (MultiByteToWideChar(CP_ACP, 0, driver_opt, -1, driver_wide,
						(int)ARRAY_SIZE(driver_wide)) <= 0) {
				fprintf(stderr, "gpu_unlock: invalid --driver path\n");
				return 2;
			}
			driver = driver_wide;
		}
		return command_real(probe, bdf, assets, driver, assume_yes, quiet,
				    cold, pre_flr);
	}
	if (!strcmp(mode, "version")) {
		printf("gpu_unlock 1.0.0 (Windows user-mode, real GA10x backend via gpuunlock.sys)\n");
		return 0;
	}
	if (!strcmp(mode, "help") || !strcmp(mode, "--help") || !strcmp(mode, "-h")) {
		usage();
		return 0;
	}
	fprintf(stderr, "gpu_unlock: unknown command \"%s\"\n", mode);
	usage();
	return 2;
}
