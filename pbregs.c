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
#include <stdarg.h>
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

static double pcb_to_temp(uint16_t raw)
{
	return (double)(raw & PB_PCB_TEMP_MASK) / 128.0;
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
 */
static int mb_comm(unsigned int mb, uint32_t cmd, size_t len_bytes,
		   uint16_t *buf)
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

	ret = mb_xfer(mb, PB_CMD_ENDCMD, 0, NULL);
	if (ret)
		return ret;

	mb_send_end(mb);
	return 0;
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
	int ret;

	for (mb = 0; mb < 2; mb++) {
		memset(buf, 0, sizeof(buf));
		printf("== CMD_READROM via MB%u ==\n", mb);

		ret = mb_comm(mb, PB_CMD_READROM, PB_ROM_SIZE, buf);
		if (ret) {
			printf("  mailbox timeout after %d ms\n",
			       mb_timeout_ms);
			continue;
		}

		print_rom((const char *)buf);
	}

	return 0;
}

/* Phase 2: CMD_READMON, the card's "SMART" block. */
static int cmd_mon(void)
{
	static uint16_t buf[sizeof(struct pb_monitor) / sizeof(uint16_t)];
	struct pb_monitor m;
	unsigned int mb;
	int ret;

	for (mb = 0; mb < 2; mb++) {
		memset(buf, 0, sizeof(buf));
		printf("== CMD_READMON via MB%u ==\n", mb);

		ret = mb_comm(mb, PB_CMD_READMON, sizeof(buf), buf);
		if (ret) {
			printf("  mailbox timeout after %d ms\n",
			       mb_timeout_ms);
			continue;
		}

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

	return 0;
}

/* Phase 2: CMD_READINFO, firmware push message. */
static int cmd_info(void)
{
	uint16_t buf[(sizeof(uint32_t) * 16 + 256) / sizeof(uint16_t) + 3];
	unsigned int mb;
	int ret;

	/* struct pblaze_print_info: u16 x3 header + values, see ref header */
	for (mb = 0; mb < 2; mb++) {
		memset(buf, 0, sizeof(buf));
		printf("== CMD_READINFO via MB%u ==\n", mb);

		ret = mb_comm(mb, PB_CMD_READINFO, sizeof(buf), buf);
		if (ret) {
			printf("  mailbox timeout after %d ms\n",
			       mb_timeout_ms);
			continue;
		}

		printf("  infolen=%u paramlen=%u strlen=%u\n",
		       buf[0], buf[1], buf[2]);
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
	void *map;
	int fd;
	int opt;

	while ((opt = getopt(argc, argv, "d:t:")) != -1) {
		switch (opt) {
		case 'd':
			path = optarg;
			break;
		case 't':
			mb_timeout_ms = atoi(optarg);
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (optind < argc)
		cmd = argv[optind];

	fd = open(path, O_RDWR | O_SYNC);
	if (fd < 0)
		die("cannot open %s (root?)", path);

	map = mmap(NULL, PB_BAR_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		   fd, 0);
	if (map == MAP_FAILED)
		die("cannot mmap %s", path);
	bar = map;

	if (!strcmp(cmd, "report"))
		cmd_report();
	else if (!strcmp(cmd, "dump"))
		cmd_dump();
	else if (!strcmp(cmd, "msg"))
		cmd_msg();
	else if (!strcmp(cmd, "rom"))
		return cmd_rom();
	else if (!strcmp(cmd, "mon"))
		return cmd_mon();
	else if (!strcmp(cmd, "info"))
		return cmd_info();
	else if (!strcmp(cmd, "flowctl"))
		cmd_flowctl();
	else
		usage(argv[0]), exit(1);

	munmap(map, PB_BAR_SIZE);
	close(fd);
	return 0;
}
