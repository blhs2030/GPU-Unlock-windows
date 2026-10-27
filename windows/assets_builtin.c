/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Built-in synthetic assets for the Windows user-mode build.
 *
 * These are generated locally and contain no NVIDIA firmware. They have the
 * exact wire geometry the unmodified kernel code validates, so the whole
 * unlock transaction can run without the proprietary 610.43.03 package.
 * `--assets DIR` loads the real blobs instead; lookup matches the final path
 * component, so both a firmware root and the 610.43.03 directory work.
 */
#include <windows.h>
#include "hal.h"

#define BOOTER_FILE "booter_load_ga102.bin"
#define V67_FILE "v67-ga102-610.43.03.bin"
#define GSP_FILE "gsp_ga10x.bin"
#define BOOT_FILE "gsp_boot_ga102.bin"
#define BOOT_DESC_FILE "gsp_boot_desc_ga102.bin"

#define BOOTER_TOTAL (72u + 60416u + 768u)
#define V67_SIZE 0xfa00u
#define GSP_IMAGE_SIZE 4097u
#define GSP_SIG_SIZE 4096u
#define GSP_FILE_SIZE (64u + GSP_IMAGE_SIZE + GSP_SIG_SIZE + 40u + 7u + 4u * 64u)
#define BOOT_TOTAL 24576u
#define DESC_TOTAL 84u

static char *asset_dir;

struct file_asset {
	struct file_asset *next;
	char *name;
	u8 *data;
	size_t size;
};

static struct file_asset *file_assets;

static u8 booter_blob[BOOTER_TOTAL];
static u8 v67_blob[V67_SIZE];
static u8 gsp_blob[GSP_FILE_SIZE];
static u8 boot_blob[BOOT_TOTAL];
static u8 desc_blob[DESC_TOTAL];

static int booter_ready, v67_ready, gsp_ready, boot_ready, desc_ready;

static void put_u16(u8 *p, u16 value)
{
	p[0] = (u8)value;
	p[1] = (u8)(value >> 8);
}

static void put_u32(u8 *p, u32 value)
{
	p[0] = (u8)value;
	p[1] = (u8)(value >> 8);
	p[2] = (u8)(value >> 16);
	p[3] = (u8)(value >> 24);
}

static void put_u64(u8 *p, u64 value)
{
	unsigned int i;

	for (i = 0; i < 8; i++)
		p[i] = (u8)(value >> (8 * i));
}

/* ------------------------------------------------------------- booter */

static void build_booter(void)
{
	u8 *p = booter_blob;
	u32 header[14];

	memset(booter_blob, 0, sizeof(booter_blob));
	put_u32(p, 0x314c5442u);	/* "BTL1" */
	put_u32(p + 4, 1);
	header[0] = 0;			/* os_code_off */
	header[1] = 256;		/* os_code_size */
	header[2] = 35328;		/* os_data_off */
	header[3] = 25088;		/* os_data_size */
	header[4] = 1;			/* num_apps */
	header[5] = 256;		/* app_code_off */
	header[6] = 35072;		/* app_code_size */
	header[7] = 256;		/* app_data_off */
	header[8] = 0;			/* app_data_size */
	header[9] = 35344;		/* patch_loc */
	header[10] = 3;			/* ucode_id */
	header[11] = 1;			/* engine_id */
	header[12] = 2;			/* num_sigs */
	header[13] = 768;		/* sig_bytes */
	for (unsigned int i = 0; i < 14; i++)
		put_u32(p + 8 + i * 4, header[i]);
	put_u32(p + 64, 60416);		/* image_size */
	put_u32(p + 68, 768);		/* sig_bytes after the header */
	memset(p + 72, 0x90, 60416);
	memset(p + 72 + 60416, 0x11, 384);
	memset(p + 72 + 60416 + 384, 0x22, 384);
	booter_ready = 1;
}

/* ---------------------------------------------------------------- v67 */

static void build_v67(void)
{
	static const struct {
		u32 offset, value;
	} patches[] = {
		{ 0x1100, 0x00000007 }, { 0xF948, 0xFFFFFFFF },
		{ 0xF950, 0x00000D44 }, { 0xF960, 0x00823804 },
		{ 0xF968, 0x00001FCE }, { 0xF974, 0x00000000 },
		{ 0xF97C, 0x00001101 }, { 0xF980, 0x000084C8 },
		{ 0xF984, 0x00008E18 }, { 0xF98C, 0x000084C8 },
		{ 0xF990, 0x00000000 }, { 0xF998, 0x00001FCE },
		{ 0xF9A4, 0x0000FFBC }, { 0xF9AC, 0x00005789 },
		{ 0xF9BC, 0x00000D44 }, { 0xF9CC, 0x00000003 },
		{ 0xF9D4, 0x00001FCE }, { 0xF9E8, 0x00000D52 },
		{ 0xF9EC, 0x000081EE },
	};
	unsigned int i;

	for (i = 0; i < V67_SIZE / 4; i++)
		put_u32(v67_blob + i * 4, 0x0000019Cu);
	for (i = 0; i < sizeof(patches) / sizeof(patches[0]); i++)
		put_u32(v67_blob + patches[i].offset, patches[i].value);
	v67_ready = 1;
}

/* ---------------------------------------------------------------- gsp */

/* A minimal ELF64 with .fwimage and .fwsignature_ga10x, parsed by
 * gpu_unlock_elf_section() exactly like the real gsp_ga10x.bin. */
static void build_gsp(void)
{
	u8 *elf = gsp_blob;
	unsigned int fw_off = 64;
	unsigned int sig_off = 64 + GSP_IMAGE_SIZE;
	unsigned int shstr_off = sig_off + GSP_SIG_SIZE;
	static const char names[] = "\0.fwimage\0.fwsignature_ga10x\0.shstrtab\0";
	unsigned int shoff = (shstr_off + sizeof(names) - 1 + 7) & ~7u;

	memset(gsp_blob, 0, sizeof(gsp_blob));
	elf[0] = 0x7f;
	elf[1] = 'E';
	elf[2] = 'L';
	elf[3] = 'F';
	elf[4] = 2;			/* ELFCLASS64 */
	elf[5] = 1;			/* ELFDATA2LSB */
	elf[6] = 1;			/* EV_CURRENT */
	put_u16(elf + 16, 1);		/* ET_REL */
	put_u32(elf + 20, 1);		/* e_version */
	put_u64(elf + 40, shoff);
	put_u16(elf + 52, 64);		/* e_ehsize */
	put_u16(elf + 58, 64);		/* e_shentsize */
	put_u16(elf + 60, 4);		/* e_shnum */
	put_u16(elf + 62, 3);		/* e_shstrndx */
	memset(elf + fw_off, 0xa5, GSP_IMAGE_SIZE);
	memset(elf + sig_off, 0x5a, GSP_SIG_SIZE);
	memcpy(elf + shstr_off, names, sizeof(names) - 1);
	/* section 1: .fwimage */
	put_u32(elf + shoff + 64, 1);
	put_u32(elf + shoff + 64 + 4, 1);	/* SHT_PROGBITS */
	put_u64(elf + shoff + 64 + 24, fw_off);
	put_u64(elf + shoff + 64 + 32, GSP_IMAGE_SIZE);
	/* section 2: .fwsignature_ga10x */
	put_u32(elf + shoff + 128, 10);
	put_u32(elf + shoff + 128 + 4, 1);
	put_u64(elf + shoff + 128 + 24, sig_off);
	put_u64(elf + shoff + 128 + 32, GSP_SIG_SIZE);
	/* section 3: .shstrtab */
	put_u32(elf + shoff + 192, 30);
	put_u32(elf + shoff + 192 + 4, 3);	/* SHT_STRTAB */
	put_u64(elf + shoff + 192 + 24, shstr_off);
	put_u64(elf + shoff + 192 + 32, sizeof(names) - 1);
	put_u64(elf + 40, shoff);
	gsp_ready = 1;
}

/* --------------------------------------------------------------- boot */

static void build_boot(void)
{
	memset(boot_blob, 0, sizeof(boot_blob));
	memset(boot_blob + 0x100, 0xc3, 0x100);
	memset(boot_blob + 0x200, 0x5a, 0x40);
	memset(boot_blob + 0x300, 0xa5, 0x100);
	boot_ready = 1;
}

static void build_boot_desc(void)
{
	u8 *p = desc_blob;

	memset(desc_blob, 0, sizeof(desc_blob));
	put_u32(p + 0, 5);		/* version 5 */
	put_u32(p + 4, 0);		/* 1: code range offset */
	put_u32(p + 8, 0x100);
	put_u32(p + 12, 0x100);		/* 3: data range offset */
	put_u32(p + 16, 0x100);
	put_u32(p + 32, 0x200);		/* 8: manifest offset */
	put_u32(p + 36, 0x40);
	put_u32(p + 40, 0x300);		/* 10: image data offset */
	put_u32(p + 44, 0x100);
	put_u32(p + 48, 0x100);		/* 12: entry offset */
	put_u32(p + 52, 0x100);
	put_u32(p + 56, 1);
	put_u32(p + 76, BOOT_TOTAL);
	put_u32(p + 80, 0);
	desc_ready = 1;
}

/* ------------------------------------------------------------- VBIOS */

/* One MiB, laid out like a board dump: PCIR header, BIT token table pointing
 * at the FWSEC ucode table, and a V3 FWSEC descriptor with a 1 MiB FRTS
 * mapper command. Validated by gpu_unlock_vbios_fwsec(). */
void gu_asset_vbios(u8 *rom, size_t size)
{
	unsigned int sum = 0, i;
	u8 *image;
	u8 *dmem;

	if (!rom || size < 0x1000)
		return;
	memset(rom, 0, size);
	rom[0] = 0x55;
	rom[1] = 0xaa;
	rom[0x18] = 32;			/* PCIR offset */
	memcpy(rom + 32, "PCIR", 4);
	rom[42] = 24;			/* NPDE offset (no NPDE signature) */
	rom[48] = 8;			/* image size in 512-byte units */
	rom[53] = 0x80;			/* last image */

	/* BIT token table at 64. */
	rom[64] = 0xff;
	rom[65] = 0xb8;
	memcpy(rom + 66, "BIT", 3);
	rom[72] = 12;			/* table size */
	rom[73] = 8;			/* token stride */
	rom[74] = 1;			/* token count */
	for (i = 64; i < 75; i++)
		sum += rom[i];
	rom[75] = (u8)(0u - sum);
	rom[76] = 0x70;
	rom[77] = 2;
	rom[78] = 4;			/* token length */
	put_u32(rom + 80, 100);		/* pointer to FWSEC table */
	put_u32(rom + 100, 128);	/* FWSEC table offset */

	/* FWSEC ucode table: version 1, one 6-byte entry, entry 0 is 0x85. */
	rom[128] = 1;
	rom[129] = 6;
	rom[130] = 6;
	rom[131] = 1;
	rom[134] = 0x85;
	put_u32(rom + 136, 256);	/* descriptor offset */

	/* Descriptor: version 5 (bits 15:8 = 3), size 812 (44 + 2 * 384). */
	put_u32(rom + 256, (812u << 16) | 0x301u);
	put_u32(rom + 260, 1536);	/* stored image bytes */
	put_u32(rom + 264, 512);	/* PKC offset in DMEM */
	put_u32(rom + 268, 128);	/* interface offset in DMEM */
	put_u32(rom + 272, 0);		/* IMEM PA */
	put_u32(rom + 276, 256);	/* IMEM size */
	put_u32(rom + 280, 0);		/* IMEM VA */
	put_u32(rom + 284, 0);		/* DMEM PA */
	put_u32(rom + 288, 1024);	/* DMEM size */
	put_u16(rom + 292, 0x400);	/* GSP engine mask */
	rom[294] = 9;			/* ucode id (FWSEC) */
	rom[295] = 2;			/* signature count */
	put_u16(rom + 296, 3);		/* signature versions 0 and 1 */

	/* FWSEC payload: image_off = 256 + 812 = 1068, DMEM at +256. */
	image = rom + 1068;
	dmem = image + 256;
	/* interface: version 1, header 4, stride 8, two entries */
	dmem[128] = 1;
	dmem[129] = 4;
	dmem[130] = 8;
	dmem[131] = 2;
	put_u32(dmem + 132, 4);		/* command mapper entry */
	put_u32(dmem + 136, 0x100);	/* mapper DMEM offset */
	put_u32(dmem + 140, 1);		/* unrelated entry */
	put_u32(dmem + 144, 0);
	/* V3 mapper at DMEM + 0x100. */
	put_u16(dmem + 256 + 4, 3);
	put_u16(dmem + 256 + 6, 64);
	put_u32(dmem + 256 + 8, 0x300);
	put_u32(dmem + 256 + 12, 64);
	put_u32(dmem + 256 + 44, 0x15);
}

/* --------------------------------------------------------------- look */

static const char *base_name(const char *name)
{
	const char *base = name;
	const char *p;

	for (p = name; *p; p++)
		if (*p == '/' || *p == '\\')
			base = p + 1;
	return base;
}

static int read_whole_file(const char *path, u8 **data, size_t *size)
{
	FILE *file;
	long length;
	u8 *buffer;

	file = fopen(path, "rb");
	if (!file)
		return -ENOENT;
	if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 ||
	    fseek(file, 0, SEEK_SET)) {
		fclose(file);
		return -EIO;
	}
	buffer = malloc((size_t)length ? (size_t)length : 1);
	if (!buffer) {
		fclose(file);
		return -ENOMEM;
	}
	if (length && fread(buffer, 1, (size_t)length, file) != (size_t)length) {
		free(buffer);
		fclose(file);
		return -EIO;
	}
	fclose(file);
	*data = buffer;
	*size = (size_t)length;
	return 0;
}

static struct file_asset *cache_find(const char *base)
{
	struct file_asset *entry;

	for (entry = file_assets; entry; entry = entry->next)
		if (!strcmp(entry->name, base))
			return entry;
	return NULL;
}

static int lookup_file(const char *name, const u8 **data, size_t *size)
{
	const char *base = base_name(name);
	struct file_asset *entry;
	char path[1024];
	char relative[512];
	size_t i;
	int ret;

	if (!asset_dir)
		return -ENOENT;
	entry = cache_find(base);
	if (!entry) {
		entry = calloc(1, sizeof(*entry));
		if (!entry)
			return -ENOMEM;
		entry->name = malloc(strlen(base) + 1);
		if (!entry->name) {
			free(entry);
			return -ENOMEM;
		}
		strcpy(entry->name, base);
		ret = -ENOENT;
		/* The firmware path may be rooted at the asset directory. */
		if (strlen(name) + strlen(asset_dir) + 2 < sizeof(path)) {
			for (i = 0; name[i] && i + 1 < sizeof(relative); i++)
				relative[i] = name[i] == '/' ? '\\' : name[i];
			relative[i] = '\0';
			snprintf(path, sizeof(path), "%s\\%s", asset_dir, relative);
			ret = read_whole_file(path, &entry->data, &entry->size);
		}
		/* Also accept the 610.43.03 directory itself. */
		if (ret && strlen(base) + strlen(asset_dir) + 2 < sizeof(path)) {
			snprintf(path, sizeof(path), "%s\\%s", asset_dir, base);
			ret = read_whole_file(path, &entry->data, &entry->size);
		}
		if (ret) {
			free(entry->name);
			free(entry);
			return ret;
		}
		entry->next = file_assets;
		file_assets = entry;
	}
	*data = entry->data;
	*size = entry->size;
	return 0;
}

int gu_asset_lookup(const char *name, const u8 **data, size_t *size)
{
	const char *base;

	if (!name || !data || !size)
		return -EINVAL;
	if (asset_dir) {
		int ret = lookup_file(name, data, size);

		if (!ret)
			return 0;
		if (ret != -ENOENT)
			return ret;
	}
	base = base_name(name);
	if (!strcmp(base, BOOTER_FILE)) {
		if (!booter_ready)
			build_booter();
		*data = booter_blob;
		*size = sizeof(booter_blob);
		return 0;
	}
	if (!strcmp(base, V67_FILE)) {
		if (!v67_ready)
			build_v67();
		*data = v67_blob;
		*size = sizeof(v67_blob);
		return 0;
	}
	if (!strcmp(base, GSP_FILE)) {
		if (!gsp_ready)
			build_gsp();
		*data = gsp_blob;
		*size = GSP_FILE_SIZE;
		return 0;
	}
	if (!strcmp(base, BOOT_FILE)) {
		if (!boot_ready)
			build_boot();
		*data = boot_blob;
		*size = sizeof(boot_blob);
		return 0;
	}
	if (!strcmp(base, BOOT_DESC_FILE)) {
		if (!desc_ready)
			build_boot_desc();
		*data = desc_blob;
		*size = sizeof(desc_blob);
		return 0;
	}
	return -ENOENT;
}

int gu_assets_set_dir(const char *dir)
{
	char *copy = NULL;

	if (dir) {
		copy = malloc(strlen(dir) + 1);
		if (!copy)
			return -ENOMEM;
		strcpy(copy, dir);
	}
	free(asset_dir);
	asset_dir = copy;
	return 0;
}

void gu_assets_free(void)
{
	struct file_asset *entry, *next;

	for (entry = file_assets; entry; entry = next) {
		next = entry->next;
		free(entry->name);
		free(entry->data);
		free(entry);
	}
	file_assets = NULL;
	free(asset_dir);
	asset_dir = NULL;
}
