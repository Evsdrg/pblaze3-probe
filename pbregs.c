// SPDX-License-Identifier: GPL-2.0
/*
 * pbregs - Memblaze PBlaze3 (FPGA prototype SSD) BAR0 register probe
 *
 * Userspace reconnaissance tool for a PBlaze3 card whose NAND flash has
 * been removed. It talks to the Xilinx FPGA through BAR0 via mmap of the
 * PCI sysfs resource file.
 *
 * Layer 1 (pure registers, no firmware involvement):
 *   - FPGA version / bitstream compile date & time
 *   - mailbox read registers: firmware lifecycle stage (ST_STAGE),
 *     echoed command code and status flags
 *   - XADC die temperature / voltages (channel-select read)
 *   - PCB temperature
 *   - firmware debug message buffer
 *
 * Layer 2 (mailbox commands, answered by the on-card firmware):
 *   - CMD_READROM  : 4KB ROM key/value identity text
 *   - CMD_READMON  : struct pblaze_monitor (the card's "SMART")
 *
 * The mailbox handshake reproduces pblaze_send_cmd_imm() /
 * pblaze_comm_imm() from the 2016 vendor driver (ref/pblaze_pcie.c),
 * with bounded timeouts instead of the original 5-minute spin.
 *
 * Copyright (C) 2026 pblaze3-probe authors
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/pci_regs.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#define PB_DEFAULT_RESOURCE	"/sys/bus/pci/devices/0000:01:00.0/resource0"
#define PB_BAR_SIZE		0x10000

/* Register map, from ref/driver/pblaze_pcie.h */
#define PB_REG_VERSION		0x000
#define PB_REG_WRITE_MB0	0x010
#define PB_REG_READ_MB0		0x014
#define PB_REG_WRITE_MB1	0x018
#define PB_REG_READ_MB1		0x01C
#define PB_REG_XADC		0x020
#define PB_REG_FPGA_COMPILE_DATE 0x030
#define PB_REG_FPGA_COMPILE_TIME 0x034
#define PB_REG_PCB_TEMPERATURE	0x038
#define PB_REG_ISRMASK		0x040
#define PB_REG_ISRDELAY		0x044
#define PB_REG_ISRINFO		0x048
#define PB_REG_FLOW_CTRL_ADDR	0x060
#define PB_REG_FLOW_CTRL_DATA	0x064
#define PB_DEBUG_MSG_ADDR	0x400
#define PB_DEBUG_MSG_LENGTH	0xc00

/* Read-mailbox status fields */
#define PB_ST_DATA_MASK		0x0000ffffu
#define PB_ST_CMD_SHIFT		16u
#define PB_ST_MSG_SHIFT		24u
#define PB_ST_ALERT_SHIFT	27u
#define PB_ST_STAGE_SHIFT	28u
#define PB_ST_WAIT		(1u << 30)
#define PB_ST_INTERRUPT		(1u << 31)

enum pb_stage {
	PB_STAGE_INIT = 0,
	PB_STAGE_READY = 1,
	PB_STAGE_RUN = 2,
};

/* Mailbox commands */
#define PB_CMD_LOCKROM		0x02
#define PB_CMD_BADTETRIS_INFO	0x03
#define PB_CMD_READINFO		0x05
#define PB_CMD_READMON		0x06
#define PB_CMD_READROM		0x07
#define PB_CMD_ENDCMD		0x21
#define PB_CMD_DATA1		0x22
#define PB_CMD_DATA2		0x23
#define PB_CMD_EXITINT		0x25
#define PB_CMD_CLEARWAIT	0x09
#define PB_CMD_CLEARINT		0x0a
#define PB_CMD_CLEARALERT	0x1c

/* Firmware status codes echoed by CMD_ENDCMD */
#define PB_CMD_ECHO_SUCCEED		0
#define PB_CMD_ECHO_FAILED		1
#define PB_CMD_ECHO_INVALID_PARMA	2
#define PB_CMD_ECHO_INVALID_DEVICE	3
#define PB_CMD_ECHO_PROBE_NO_FINISH	4
#define PB_CMD_ECHO_MALLOC_FAILED	5

#define PB_IS_MAJORCMD(cmd)	((cmd) >= 0x01 && (cmd) <= 0x20)

/* XADC channels */
#define PB_XADC_WRITE_SHIFT	16u
#define PB_XADC_WRITE_MASK	0x007f0000u
#define PB_XADC_READ_MASK	0xffffu

#define PB_XADC_TEMP		0x00
#define PB_XADC_VCCINT		0x01
#define PB_XADC_VCCAUX		0x02
#define PB_XADC_VFLASH_IO	0x03
#define PB_XADC_VFLASH_CORE	0x10
#define PB_XADC_VDDR3		0x11
#define PB_XADC_MAX_TEMP	0x20
#define PB_XADC_MAX_VCCINT	0x21
#define PB_XADC_MAX_VCCAUX	0x22
#define PB_XADC_MIN_TEMP	0x24
#define PB_XADC_MIN_VCCINT	0x25
#define PB_XADC_MIN_VCCAUX	0x26

#define PB_PCB_TEMP_MASK	0x1fff

#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))

/* Flow-control init, values from the vendor driver */
#define PB_INT_INTERVAL		0x0c000100u
#define PB_COEFF_C_ADDR		0x1000008u
#define PB_WRITE_FC_EN_ADDR	0x100000fu

#define PB_ROM_SIZE		4096
/* struct pblaze_print_info on the wire: 3 u16 + 80 u32 (ref header) */
#define PB_PRINT_INFO_SIZE	328

/* The card's health block, ref/driver/pblaze_hal.h struct pblaze_monitor */
struct pb_monitor {
	uint64_t logical_read;
	uint64_t flash_read;
	uint64_t logical_write;
	uint64_t flash_write;
	uint32_t lsa_limit_in_tetris;
	uint32_t dead_block;
	uint16_t dead_die;
	uint16_t init_count;
	uint16_t start_count;
	uint16_t over_heating;
	uint16_t tetris_size_in_system;
	int16_t boardtemp_max;
	int16_t boardtemp_min;
};

static volatile uint32_t *bar;
static int mb_timeout_ms = 200;

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "pbregs: ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	if (errno)
		fprintf(stderr, ": %s", strerror(errno));
	fputc('\n', stderr);
	exit(1);
}

/*
 * Derive the PCI config file path from a resource file path, so the
 * tool can read live configuration space without hardcoding a BDF.
 */
static int pb_config_path(const char *resource, char *out, size_t outlen)
{
	char *name;

	if (strlen(resource) >= outlen)
		return -ENAMETOOLONG;
	strcpy(out, resource);
	name = strrchr(out, '/');
	if (!name || strcmp(name + 1, "resource0"))
		return -EINVAL;
	if ((size_t)(name - out) + sizeof("/config") > outlen)
		return -ENAMETOOLONG;
	strcpy(name + 1, "config");
	return 0;
}

static int pb_read_config(const char *resource, unsigned int off, void *buf,
			  size_t len)
{
	char path[PATH_MAX];
	ssize_t count;
	int fd;
	int ret;

	ret = pb_config_path(resource, path, sizeof(path));
	if (ret)
		return ret;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	count = pread(fd, buf, len, off);
	ret = (count == (ssize_t)len) ? 0 : -EIO;
	close(fd);
	return ret;
}

static int pci_preflight(const char *resource)
{
	unsigned char config[64];
	unsigned int vendor, device, command;
	int ret;

	ret = pb_read_config(resource, 0, config, sizeof(config));
	if (ret)
		return ret;

	vendor = config[PCI_VENDOR_ID] | config[PCI_VENDOR_ID + 1] << 8;
	device = config[PCI_DEVICE_ID] | config[PCI_DEVICE_ID + 1] << 8;
	command = config[PCI_COMMAND] | config[PCI_COMMAND + 1] << 8;
	if (vendor == 0xffff) {
		fprintf(stderr, "pbregs: live PCI config is inaccessible; "
			"refusing BAR access\n");
		return -ENODEV;
	}
	if (vendor != 0x1c5f || device != 0x0530)
		return -ENODEV;
	if (!(command & PCI_COMMAND_MEMORY)) {
		fprintf(stderr, "pbregs: PCI memory decoding is disabled "
			"(command=0x%04x); refusing BAR access\n", command);
		return -EACCES;
	}
	return 0;
}

/*
 * Negotiated link width and generation, from the PCIe capability in
 * live config space. Mirrors pblaze_pcie_get_link_status().
 */
static int pb_link_status(const char *resource, int *width, int *gen)
{
	unsigned char hdr[64], cap[4];
	unsigned int off;
	int i;
	int ret;

	ret = pb_read_config(resource, 0, hdr, sizeof(hdr));
	if (ret)
		return ret;

	off = hdr[PCI_CAPABILITY_LIST] & 0xfc;
	for (i = 0; i < 48 && off >= 0x40; i++) {
		unsigned char lnk[2];
		unsigned int val;

		ret = pb_read_config(resource, off, cap, sizeof(cap));
		if (ret)
			return ret;

		if (cap[PCI_CAP_LIST_ID] == PCI_CAP_ID_EXP) {
			ret = pb_read_config(resource, off + PCI_EXP_LNKSTA,
					     lnk, sizeof(lnk));
			if (ret)
				return ret;
			val = lnk[0] | lnk[1] << 8;
			*width = (val & PCI_EXP_LNKSTA_NLW) >> 4;
			switch (val & PCI_EXP_LNKSTA_CLS) {
			case PCI_EXP_LNKSTA_CLS_2_5GB:
				*gen = 1;
				break;
			case PCI_EXP_LNKSTA_CLS_5_0GB:
				*gen = 2;
				break;
			case PCI_EXP_LNKSTA_CLS_8_0GB:
				*gen = 3;
				break;
			default:
				*gen = 0;
				break;
			}
			return 0;
		}

		off = cap[PCI_CAP_LIST_NEXT] & 0xfc;
	}

	return -ENOENT;
}

static uint32_t rd(unsigned int off)
{
	return bar[off / 4];
}

static void wr(unsigned int off, uint32_t val)
{
	bar[off / 4] = val;
}

static void msleep(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}

static double xadc_to_temp(uint16_t raw)
{
	return (double)(raw >> 4) * (503.975 / 4096.0) - 273.15;
}

static double xadc_to_volt(uint16_t raw)
{
	return (double)(raw >> 4) * (3.0 / 4096.0);
}

/*
 * PCB temperature. The vendor driver reads the 13-bit sensor field and
 * shifts it left by 3 (pblaze_read_pcb_temperature), then the tool
 * divides by 128 (GetOnboardTemperature); the net scale is raw / 16.
 */
static double pcb_to_temp(uint16_t raw)
{
	return (double)(raw & PB_PCB_TEMP_MASK) / 16.0;
}

/*
 * XADC channel-select read. The channel number is written to bits
 * [22:16]; the FPGA echoes it back once the sample is latched, and the
 * 16-bit conversion result sits in bits [15:0]. This mirrors
 * pblaze_read_xadc_value() in ref/driver/pblaze_raid.c.
 */
static int xadc_read(uint8_t ch, uint16_t *out)
{
	unsigned int i;

	wr(PB_REG_XADC, (uint32_t)ch << PB_XADC_WRITE_SHIFT);

	for (i = 0; i < 1000; i++) {
		uint32_t val = rd(PB_REG_XADC);

		if (((val & PB_XADC_WRITE_MASK) >> PB_XADC_WRITE_SHIFT) == ch) {
			*out = (uint16_t)(val & PB_XADC_READ_MASK);
			return 0;
		}
		msleep(1);
	}

	return -ETIMEDOUT;
}

static const char *stage_name(uint32_t stage)
{
	switch (stage) {
	case PB_STAGE_INIT:
		return "INIT";
	case PB_STAGE_READY:
		return "READY";
	case PB_STAGE_RUN:
		return "RUN";
	default:
		return "RESERVED";
	}
}

static void decode_mailbox(const char *name, uint32_t val)
{
	printf("  %s raw=0x%08" PRIx32
	       " data=0x%04" PRIx32 " cmd=0x%02" PRIx32
	       " msg=%" PRIu32 " alert=%" PRIu32
	       " stage=%s wait=%" PRIu32 " intr=%" PRIu32 "\n",
	       name, val, val & PB_ST_DATA_MASK,
	       (val >> PB_ST_CMD_SHIFT) & 0xffu,
	       (val >> PB_ST_MSG_SHIFT) & 7u,
	       (val >> PB_ST_ALERT_SHIFT) & 3u,
	       stage_name((val >> PB_ST_STAGE_SHIFT) & 3u),
	       (val & PB_ST_WAIT) ? 1 : 0,
	       (val & PB_ST_INTERRUPT) ? 1 : 0);
}

/*
 * One mailbox transaction word, reproducing pblaze_send_cmd_imm():
 * write (cmd<<16)|data (with the interrupt bit set for major commands),
 * then poll the read mailbox until the FPGA echoes the command code.
 * Returns the 16-bit data field, or -ETIMEDOUT.
 */
static int mb_xfer(unsigned int mb, uint32_t cmd, uint16_t data_in,
		   uint16_t *data_out)
{
	unsigned int woff = mb ? PB_REG_WRITE_MB1 : PB_REG_WRITE_MB0;
	unsigned int roff = mb ? PB_REG_READ_MB1 : PB_REG_READ_MB0;
	uint32_t value = (cmd << 16) | data_in;
	uint64_t deadline;

	if (PB_IS_MAJORCMD(cmd))
		value |= PB_ST_INTERRUPT;

	wr(woff, value);

	deadline = now_us() + (uint64_t)mb_timeout_ms * 1000;
	for (;;) {
		uint32_t val = rd(roff);

		if (((val >> PB_ST_CMD_SHIFT) & 0xffu) == (cmd & 0xffu)) {
			if (data_out)
				*data_out = (uint16_t)(val & PB_ST_DATA_MASK);
			return 0;
		}

		if (now_us() >= deadline)
			return -ETIMEDOUT;
	}
}

static void mb_send_end(unsigned int mb)
{
	wr(mb ? PB_REG_WRITE_MB1 : PB_REG_WRITE_MB0, PB_CMD_EXITINT << 16);
}

/*
 * Multi-word mailbox command, reproducing pblaze_comm_imm(): major
 * command first, then len 16-bit words carried by alternating
 * CMD_DATA1/CMD_DATA2, then CMD_ENDCMD and CMD_EXITINT.
 *
 * The data field echoed by CMD_ENDCMD carries the firmware's status
 * code for the command (CMD_ECHO_* in the vendor driver); it is
 * returned through @status when non-NULL.
 */
static int mb_comm(unsigned int mb, uint32_t cmd, size_t len_bytes,
		   uint16_t *buf, uint16_t *status)
{
	size_t words = len_bytes / sizeof(uint16_t);
	size_t i;
	int ret;

	ret = mb_xfer(mb, cmd, 0, NULL);
	if (ret)
		return ret;

	for (i = 0; i < words; i++) {
		uint32_t data_cmd = (i & 1) ? PB_CMD_DATA2 : PB_CMD_DATA1;

		ret = mb_xfer(mb, data_cmd, 0, &buf[i]);
		if (ret)
			return ret;
	}

	ret = mb_xfer(mb, PB_CMD_ENDCMD, 0, status);
	if (ret)
		return ret;

	mb_send_end(mb);
	return 0;
}

static const char *cmd_status_name(uint16_t status)
{
	switch (status) {
	case PB_CMD_ECHO_SUCCEED:
		return "succeed";
	case PB_CMD_ECHO_FAILED:
		return "failed";
	case PB_CMD_ECHO_INVALID_PARMA:
		return "invalid parameter";
	case PB_CMD_ECHO_INVALID_DEVICE:
		return "invalid device";
	case PB_CMD_ECHO_PROBE_NO_FINISH:
		return "probe not finished";
	case PB_CMD_ECHO_MALLOC_FAILED:
		return "malloc failed";
	default:
		return "unknown";
	}
}

/* Apply the vendor flow-control init sequence, ref pblaze_pcie.c:1235 */
static void flow_control_init(void)
{
	wr(PB_REG_ISRDELAY, PB_INT_INTERVAL);
	msleep(1);
	wr(PB_REG_FLOW_CTRL_DATA, PB_INT_INTERVAL & 0x3fffu);
	msleep(1);
	wr(PB_REG_FLOW_CTRL_ADDR, PB_COEFF_C_ADDR);
	msleep(1);
	wr(PB_REG_FLOW_CTRL_DATA, 1);
	msleep(1);
	wr(PB_REG_FLOW_CTRL_ADDR, PB_WRITE_FC_EN_ADDR);
	msleep(1);
}

static void print_rom(const char *rom)
{
	const char *p = rom;
	const char *end = rom + PB_ROM_SIZE;
	int is_key = 1;

	while (p < end && *p) {
		size_t len = strnlen(p, (size_t)(end - p));

		printf("  %s%.*s\n", is_key ? "" : "      ",
		       (int)len, p);
		is_key = !is_key;
		p += len + 1;
	}
}

static void print_monitor(const struct pb_monitor *m)
{
	printf("  logical_read      %" PRIu64 "\n", m->logical_read);
	printf("  flash_read        %" PRIu64 "\n", m->flash_read);
	printf("  logical_write     %" PRIu64 "\n", m->logical_write);
	printf("  flash_write       %" PRIu64 "\n", m->flash_write);
	printf("  lsa_limit_in_tetris %" PRIu32 "\n", m->lsa_limit_in_tetris);
	printf("  dead_block        %" PRIu32 "\n", m->dead_block);
	printf("  dead_die          %" PRIu16 "\n", m->dead_die);
	printf("  init_count        %" PRIu16 "\n", m->init_count);
	printf("  start_count       %" PRIu16 "\n", m->start_count);
	printf("  over_heating      %" PRIu16 "\n", m->over_heating);
	printf("  tetris_size_in_system %" PRIu16 "\n",
	       m->tetris_size_in_system);
	printf("  boardtemp_max     %.1f C\n",
	       (double)m->boardtemp_max / 128.0);
	printf("  boardtemp_min     %.1f C\n",
	       (double)m->boardtemp_min / 128.0);
}

static int u64_from_words(const uint16_t *w)
{
	return (int)((uint32_t)w[0] | ((uint32_t)w[1] << 16));
}

static uint64_t u64_from_words64(const uint16_t *w)
{
	return (uint64_t)(uint32_t)w[0] |
	       ((uint64_t)(uint32_t)w[1] << 16) |
	       ((uint64_t)(uint32_t)w[2] << 32) |
	       ((uint64_t)(uint32_t)w[3] << 48);
}

/*
 * Look up a key in the ROM key/value text, mirroring pb_get_str_by_key():
 * entries are NUL-terminated key and value strings in sequence.
 */
static const char *rom_str(const char *rom, const char *key)
{
	const char *p = rom;
	const char *end = rom + PB_ROM_SIZE;

	while (p < end && *p) {
		size_t klen = strnlen(p, (size_t)(end - p));

		if (klen == strlen(key) && !memcmp(p, key, klen) &&
		    p + klen + 1 < end)
			return p + klen + 1;
		p += klen + 1;
		if (p >= end)
			break;
		p += strnlen(p, (size_t)(end - p)) + 1;
	}

	return NULL;
}

static long long rom_int(const char *rom, const char *key)
{
	const char *v = rom_str(rom, key);

	return v ? strtoll(v, NULL, 10) : 0;
}

static void print_size(const char *name, unsigned long long bytes)
{
	static const char *const unit[] = { "B", "KB", "MB", "GB", "TB" };
	double val = (double)bytes;
	int i = 0;

	while (val >= 1024.0 && i < (int)ARRAY_SIZE(unit) - 1) {
		val /= 1024.0;
		i++;
	}
	printf("%s=%.2f%s\n", name, val, unit[i]);
}

/*
 * nvidia-smi style summary. Groups A (ROM identity), B (sensors) and
 * C (link/status) are always printed. Group D (health/performance) is
 * printed only when the firmware has reached RUN; otherwise it is
 * reported as unavailable rather than as zeroes.
 */
static int cmd_status(const char *path)
{
	static uint16_t rombuf[PB_ROM_SIZE / sizeof(uint16_t)];
	static uint16_t monbuf[sizeof(struct pb_monitor) / sizeof(uint16_t)];
	const char *rom = (const char *)rombuf;
	struct pb_monitor m;
	uint16_t status;
	uint32_t mb0, mb1, stage;
	int width = 0, gen = 0;
	bool rom_ok = false;
	bool mon_ok = false;

	mb0 = rd(PB_REG_READ_MB0);
	mb1 = rd(PB_REG_READ_MB1);
	stage = (mb1 >> PB_ST_STAGE_SHIFT) & 3u;
	if (stage == PB_STAGE_INIT && ((mb0 >> PB_ST_STAGE_SHIFT) & 3u) != PB_STAGE_INIT)
		stage = (mb0 >> PB_ST_STAGE_SHIFT) & 3u;

	/* Group A: identity from ROM (MB1 answers on this card) */
	memset(rombuf, 0, sizeof(rombuf));
	if (mb_comm(1, PB_CMD_READROM, PB_ROM_SIZE, rombuf, &status) == 0 &&
	    status == PB_CMD_ECHO_SUCCEED)
		rom_ok = true;
	else {
		memset(rombuf, 0, sizeof(rombuf));
		if (mb_comm(0, PB_CMD_READROM, PB_ROM_SIZE, rombuf, &status) == 0 &&
		    status == PB_CMD_ECHO_SUCCEED)
			rom_ok = true;
	}

	printf("===== identity =====\n");
	if (rom_ok) {
		const char *model = rom_str(rom, "Model:");

		printf("Model=%s\n", model ? model : "?");
		printf("SerialNumber=%s\n",
		       rom_str(rom, "Serial Number:") ?: "?");
		printf("FirmwareVersion=%s\n",
		       rom_str(rom, "Firmware Version:") ?: "?");
		print_size("FirmwareSize",
			   (unsigned long long)rom_int(rom, "Firmware Size:"));
		printf("FlashCell=%s\n", rom_str(rom, "Flash Cell:") ?: "?");
		printf("FlashType=%s\n", rom_str(rom, "Flash Type:") ?: "?");
		printf("MotherCardPN=%s\n",
		       rom_str(rom, "Mother Card PN:") ?: "?");
		if (model && strlen(model) > 3) {
			if (model[3] == 'H')
				printf("FormFactor=Half-length,full-height\n");
			else if (model[3] == 'L')
				printf("FormFactor=Half-length,half-height\n");
		}
	} else {
		printf("Model=unavailable (ROM read failed)\n");
	}
	printf("DeviceIndex=%lld\n", rom_int(rom, "DeviceIndex:"));

	/* Group B: sensors (XADC + PCB) */
	printf("===== sensors =====\n");
	{
		static const struct {
			uint8_t ch;
			const char *name;
			bool is_temp;
		} tbl[] = {
			{ PB_XADC_TEMP,		"CoreTemperature",	true },
			{ PB_XADC_MAX_TEMP,	"MaxCoreTemperature",	true },
			{ PB_XADC_MIN_TEMP,	"MinCoreTemperature",	true },
			{ PB_XADC_VCCINT,	"VCore",		false },
			{ PB_XADC_VCCAUX,	"VPeripheral",		false },
			{ PB_XADC_VDDR3,	"VDDR",			false },
			{ PB_XADC_VFLASH_IO,	"VFlash",		false },
			{ PB_XADC_VFLASH_CORE,	"VFlashCore",		false },
		};
		size_t i;

		for (i = 0; i < ARRAY_SIZE(tbl); i++) {
			uint16_t raw;

			if (xadc_read(tbl[i].ch, &raw)) {
				printf("%s=unavailable\n", tbl[i].name);
				continue;
			}
			if (tbl[i].is_temp)
				printf("%s=%.1fC\n", tbl[i].name,
				       xadc_to_temp(raw));
			else
				printf("%s=%.2fV\n", tbl[i].name,
				       xadc_to_volt(raw));
		}
		printf("BoardTemperature=%.1fC\n",
		       pcb_to_temp((uint16_t)rd(PB_REG_PCB_TEMPERATURE)));
	}

	/* Group C: link and status */
	printf("===== status =====\n");
	if (pb_link_status(path, &width, &gen) == 0)
		printf("PCIeLink=%d(v%d)\n", width, gen);
	else
		printf("PCIeLink=unavailable\n");
	printf("FirmwareStage=%s\n", stage_name(stage));
	printf("Status=%s\n", stage == PB_STAGE_RUN ? "Online" : "Offline");

	/* Group D: health/performance, only meaningful once running */
	printf("===== health =====\n");
	memset(monbuf, 0, sizeof(monbuf));
	if (mb_comm(1, PB_CMD_READMON, sizeof(monbuf), monbuf, &status) == 0 &&
	    status == PB_CMD_ECHO_SUCCEED)
		mon_ok = true;
	else {
		memset(monbuf, 0, sizeof(monbuf));
		if (mb_comm(0, PB_CMD_READMON, sizeof(monbuf), monbuf, &status) == 0 &&
		    status == PB_CMD_ECHO_SUCCEED)
			mon_ok = true;
	}

	if (!mon_ok) {
		printf("DeviceHealth=unavailable (READMON failed)\n");
		return 1;
	}

	m.logical_read = u64_from_words64(monbuf + 0);
	m.flash_read = u64_from_words64(monbuf + 4);
	m.logical_write = u64_from_words64(monbuf + 8);
	m.flash_write = u64_from_words64(monbuf + 12);
	m.dead_block = (uint32_t)u64_from_words(monbuf + 18);
	m.dead_die = monbuf[20];
	m.init_count = monbuf[21];
	m.start_count = monbuf[22];
	m.over_heating = monbuf[23];
	m.tetris_size_in_system = monbuf[24];

	if (stage != PB_STAGE_RUN) {
		printf("DeviceHealth=unavailable (firmware stage %s, "
		       "not RUN)\n", stage_name(stage));
		printf("DeviceDeadBlocks=%u\n", m.dead_block);
		printf("DeviceDeadDies=%u\n", m.dead_die);
		printf("InitializationCounts=%u\n", m.init_count);
		printf("StartupCounts=%u\n", m.start_count);
		printf("DeviceRemainLife=unavailable\n");
		printf("AlreadyReadCapacity=unavailable\n");
		printf("AlreadyWriteCapacity=unavailable\n");
		printf("WriteAmplification=unavailable\n");
		return 0;
	}

	print_size("AlreadyReadCapacity", m.logical_read * 4096ull);
	print_size("AlreadyWriteCapacity", m.logical_write * 4096ull);
	printf("WriteAmplification=%.2f\n",
	       m.logical_write ? (double)m.flash_write / m.logical_write : 0.0);
	printf("DeviceDeadBlocks=%u\n", m.dead_block);
	printf("DeviceDeadDies=%u\n", m.dead_die);
	printf("InitializationCounts=%u\n", m.init_count);
	printf("StartupCounts=%u\n", m.start_count);
	printf("ReadOnly=%s\n", m.dead_die ? "Yes" : "No");

	return 0;
}

static void cmd_report(void)
{
	static const struct {
		uint8_t ch;
		const char *name;
		double scale;
	} xadc_table[] = {
		{ PB_XADC_TEMP,		"core temp",	0 },
		{ PB_XADC_MAX_TEMP,	"core temp max", 0 },
		{ PB_XADC_MIN_TEMP,	"core temp min", 0 },
		{ PB_XADC_VCCINT,	"VCCINT",	1 },
		{ PB_XADC_MAX_VCCINT,	"VCCINT max",	1 },
		{ PB_XADC_MIN_VCCINT,	"VCCINT min",	1 },
		{ PB_XADC_VCCAUX,	"VCCAUX",	1 },
		{ PB_XADC_MAX_VCCAUX,	"VCCAUX max",	1 },
		{ PB_XADC_MIN_VCCAUX,	"VCCAUX min",	1 },
		{ PB_XADC_VDDR3,	"VDDR3",	1 },
		{ PB_XADC_VFLASH_IO,	"Vflash IO",	1.2 },
		{ PB_XADC_VFLASH_CORE,	"Vflash core",	2.2 },
	};
	size_t i;

	printf("== identity ==\n");
	printf("  fpga_version      0x%08" PRIx32 "\n", rd(PB_REG_VERSION));
	printf("  compile_date      0x%08" PRIx32 "\n",
	       rd(PB_REG_FPGA_COMPILE_DATE));
	printf("  compile_time      0x%08" PRIx32 "\n",
	       rd(PB_REG_FPGA_COMPILE_TIME));

	printf("== firmware lifecycle (read mailboxes, passive) ==\n");
	decode_mailbox("MB0", rd(PB_REG_READ_MB0));
	decode_mailbox("MB1", rd(PB_REG_READ_MB1));

	printf("== interrupt / flow-control regs ==\n");
	printf("  isrmask=0x%08" PRIx32 " isrdelay=0x%08" PRIx32
	       " isrinfo=0x%08" PRIx32 "\n",
	       rd(PB_REG_ISRMASK), rd(PB_REG_ISRDELAY), rd(PB_REG_ISRINFO));
	printf("  fc_addr=0x%08" PRIx32 " fc_data=0x%08" PRIx32 "\n",
	       rd(PB_REG_FLOW_CTRL_ADDR), rd(PB_REG_FLOW_CTRL_DATA));

	printf("== XADC (channel-select read) ==\n");
	for (i = 0; i < ARRAY_SIZE(xadc_table); i++) {
		uint16_t raw;
		int ret = xadc_read(xadc_table[i].ch, &raw);

		if (ret) {
			printf("  %-14s ch 0x%02x  TIMEOUT\n",
			       xadc_table[i].name, xadc_table[i].ch);
			continue;
		}

		if (xadc_table[i].scale == 0)
			printf("  %-14s ch 0x%02x  raw 0x%04x  %6.2f C\n",
			       xadc_table[i].name, xadc_table[i].ch, raw,
			       xadc_to_temp(raw));
		else
			printf("  %-14s ch 0x%02x  raw 0x%04x  %6.3f V\n",
			       xadc_table[i].name, xadc_table[i].ch, raw,
			       xadc_to_volt(raw) * xadc_table[i].scale);
	}

	printf("== PCB temperature ==\n");
	printf("  pcb_temp          raw 0x%04x  %.1f C\n",
	       (uint16_t)rd(PB_REG_PCB_TEMPERATURE),
	       pcb_to_temp((uint16_t)rd(PB_REG_PCB_TEMPERATURE)));
}

static void cmd_dump(void)
{
	unsigned int off;

	for (off = 0; off < 0x100; off += 16) {
		unsigned int w;

		printf("  %04x:", off);
		for (w = 0; w < 4; w++)
			printf(" %08" PRIx32, rd(off + w * 4));
		printf("\n");
	}
}

static void cmd_msg(void)
{
	unsigned int off;

	printf("== firmware debug message buffer (0x%x .. 0x%x) ==\n",
	       PB_DEBUG_MSG_ADDR,
	       PB_DEBUG_MSG_ADDR + PB_DEBUG_MSG_LENGTH);

	for (off = 0; off < PB_DEBUG_MSG_LENGTH; off += 4) {
		uint32_t val = rd(PB_DEBUG_MSG_ADDR + off);
		int b;

		for (b = 0; b < 4; b++) {
			char c = (char)(val >> (8 * b));

			if (c == '\n') {
				putchar('\n');
			} else if (c >= 0x20 && c < 0x7f) {
				putchar(c);
			} else if (c == '\0') {
				putchar('.');
			} else {
				putchar('?');
			}
		}
	}
	putchar('\n');
}

/* Phase 2: CMD_READROM on both mailboxes. */
static int cmd_rom(void)
{
	static uint16_t buf[PB_ROM_SIZE / sizeof(uint16_t)];
	unsigned int mb;
	bool any_ok = false;
	int ret;

	for (mb = 0; mb < 2; mb++) {
		uint16_t status;

		memset(buf, 0, sizeof(buf));
		printf("== CMD_READROM via MB%u ==\n", mb);

		ret = mb_comm(mb, PB_CMD_READROM, PB_ROM_SIZE, buf, &status);
		if (ret) {
			printf("  mailbox timeout after %d ms\n",
			       mb_timeout_ms);
			continue;
		}
		if (status != PB_CMD_ECHO_SUCCEED) {
			printf("  firmware status %u (%s)\n", status,
			       cmd_status_name(status));
			continue;
		}

		any_ok = true;
		print_rom((const char *)buf);
	}

	return any_ok ? 0 : 1;
}

/* Phase 2: CMD_READMON, the card's "SMART" block. */
static int cmd_mon(void)
{
	static uint16_t buf[sizeof(struct pb_monitor) / sizeof(uint16_t)];
	struct pb_monitor m;
	unsigned int mb;
	bool any_ok = false;
	int ret;

	for (mb = 0; mb < 2; mb++) {
		uint16_t status;

		memset(buf, 0, sizeof(buf));
		printf("== CMD_READMON via MB%u ==\n", mb);

		ret = mb_comm(mb, PB_CMD_READMON, sizeof(buf), buf, &status);
		if (ret) {
			printf("  mailbox timeout after %d ms\n",
			       mb_timeout_ms);
			continue;
		}
		if (status != PB_CMD_ECHO_SUCCEED) {
			printf("  firmware status %u (%s)\n", status,
			       cmd_status_name(status));
			continue;
		}

		any_ok = true;
		m.logical_read = u64_from_words64(buf + 0);
		m.flash_read = u64_from_words64(buf + 4);
		m.logical_write = u64_from_words64(buf + 8);
		m.flash_write = u64_from_words64(buf + 12);
		m.lsa_limit_in_tetris = (uint32_t)u64_from_words(buf + 16);
		m.dead_block = (uint32_t)u64_from_words(buf + 18);
		m.dead_die = buf[20];
		m.init_count = buf[21];
		m.start_count = buf[22];
		m.over_heating = buf[23];
		m.tetris_size_in_system = buf[24];
		m.boardtemp_max = (int16_t)buf[25];
		m.boardtemp_min = (int16_t)buf[26];

		print_monitor(&m);
	}

	return any_ok ? 0 : 1;
}

/* Phase 2: CMD_READINFO, firmware push message. */
static int cmd_info(void)
{
	/*
	 * struct pblaze_print_info is 328 bytes: 3 u16 header fields
	 * followed by a u32 value[] array (see ref header). The array
	 * itself is 16 + 256/4 = 80 u32 = 320 bytes, and the u32 forces
	 * 4-byte alignment after the 6-byte header, so the trailing u16
	 * in the wire stream is padding.
	 */
	uint16_t buf[PB_PRINT_INFO_SIZE / sizeof(uint16_t)];
	unsigned int mb;
	bool any_ok = false;
	int ret;

	for (mb = 0; mb < 2; mb++) {
		uint16_t status;

		memset(buf, 0, sizeof(buf));
		printf("== CMD_READINFO via MB%u ==\n", mb);

		ret = mb_comm(mb, PB_CMD_READINFO, sizeof(buf), buf, &status);
		if (ret) {
			printf("  mailbox timeout after %d ms\n",
			       mb_timeout_ms);
			continue;
		}
		if (status != PB_CMD_ECHO_SUCCEED) {
			printf("  firmware status %u (%s)\n", status,
			       cmd_status_name(status));
			continue;
		}

		any_ok = true;
		printf("  infolen=%u paramlen=%u strlen=%u\n",
		       buf[0], buf[1], buf[2]);
	}
	return any_ok ? 0 : 1;
}

/*
 * Single 32-bit MMIO read, nothing else. Used to test whether the card
 * survives plain memory reads toward its BAR.
 */
static int cmd_peek(unsigned int off)
{
	uint32_t value = rd(off);

	printf("peek @0x%03x = 0x%08" PRIx32 "\n", off, value);
	if (value == UINT32_MAX) {
		fprintf(stderr, "pbregs: MMIO returned all ones; "
			"device response is unavailable or invalid\n");
		return 1;
	}
	return 0;
}

static void cmd_flowctl(void)
{
	printf("applying vendor flow-control init sequence...\n");
	flow_control_init();
	printf("done\n");
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options] [command]\n"
		"commands:\n"
		"  report   identity + firmware stage + XADC + PCB temp (default)\n"
		"  status   nvidia-smi style summary (identity/sensors/status/health)\n"
		"  peek [HEX] single read-only 32-bit register read (default offset 0)\n"
		"  dump     hexdump of BAR0 first 0x100 bytes\n"
		"  msg      firmware debug message buffer\n"
		"  rom      mailbox CMD_READROM (phase 2)\n"
		"  mon      mailbox CMD_READMON (phase 2)\n"
		"  info     mailbox CMD_READINFO (phase 2)\n"
		"  flowctl  apply vendor flow-control init (phase 2 fallback)\n"
		"options:\n"
		"  -d PATH  PCI sysfs resource file (default %s)\n"
		"  -t MS    mailbox poll timeout in ms (default %d)\n",
		prog, PB_DEFAULT_RESOURCE, mb_timeout_ms);
}

int main(int argc, char **argv)
{
	const char *path = PB_DEFAULT_RESOURCE;
	const char *cmd = "report";
	unsigned int offset = PB_REG_VERSION;
	bool readonly;
	void *map;
	int status = 0;
	int ret;
	int fd;
	int opt;

	while ((opt = getopt(argc, argv, "d:t:")) != -1) {
		switch (opt) {
		case 'd':
			path = optarg;
			break;
		case 't': {
			char *end;
			long timeout;

			errno = 0;
			timeout = strtol(optarg, &end, 10);
			if (errno || end == optarg || *end ||
			    timeout < 1 || timeout > 10000) {
				fprintf(stderr, "pbregs: timeout must be 1..10000 ms\n");
				return 1;
			}
			mb_timeout_ms = (int)timeout;
			break;
		}
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (optind < argc)
		cmd = argv[optind++];
	if (strcmp(cmd, "peek") && strcmp(cmd, "report") &&
	    strcmp(cmd, "status") && strcmp(cmd, "dump") &&
	    strcmp(cmd, "msg") && strcmp(cmd, "rom") &&
	    strcmp(cmd, "mon") && strcmp(cmd, "info") &&
	    strcmp(cmd, "flowctl")) {
		usage(argv[0]);
		return 1;
	}
	if (!strcmp(cmd, "peek") && optind < argc) {
		char *end;
		unsigned long parsed;

		errno = 0;
		parsed = strtoul(argv[optind++], &end, 16);
		if (errno || end == argv[optind - 1] || *end ||
		    parsed > PB_BAR_SIZE - sizeof(uint32_t) || (parsed & 3)) {
			fprintf(stderr, "pbregs: offset must be aligned and within BAR0\n");
			return 1;
		}
		offset = (unsigned int)parsed;
	}
	if (optind != argc) {
		usage(argv[0]);
		return 1;
	}

	ret = pci_preflight(path);
	if (ret) {
		fprintf(stderr, "pbregs: PCI preflight failed: %s\n",
			strerror(-ret));
		return 1;
	}
	readonly = !strcmp(cmd, "peek") || !strcmp(cmd, "dump") ||
		   !strcmp(cmd, "msg");
	fd = open(path, (readonly ? O_RDONLY : O_RDWR) | O_SYNC | O_CLOEXEC);
	if (fd < 0)
		die("cannot open %s (root?)", path);

	map = mmap(NULL, PB_BAR_SIZE,
		   readonly ? PROT_READ : PROT_READ | PROT_WRITE,
		   MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		die("cannot mmap %s", path);
	bar = map;

	if (strcmp(cmd, "peek") && rd(PB_REG_VERSION) == UINT32_MAX) {
		fprintf(stderr, "pbregs: version register returned all ones; "
			"refusing further register access\n");
		status = 1;
		goto out;
	}
	if (!strcmp(cmd, "report"))
		cmd_report();
	else if (!strcmp(cmd, "status"))
		status = cmd_status(path);
	else if (!strcmp(cmd, "dump"))
		cmd_dump();
	else if (!strcmp(cmd, "msg"))
		cmd_msg();
	else if (!strcmp(cmd, "rom"))
		status = cmd_rom();
	else if (!strcmp(cmd, "mon"))
		status = cmd_mon();
	else if (!strcmp(cmd, "info"))
		status = cmd_info();
	else if (!strcmp(cmd, "flowctl"))
		cmd_flowctl();
	else
		status = cmd_peek(offset);
out:
	munmap(map, PB_BAR_SIZE);
	close(fd);
	return status;
}
