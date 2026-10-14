// SPDX-License-Identifier: GPL-2.0
/*
 * pblaze_probe - Memblaze PBlaze3 prototype card (FPGA, no NAND) probe driver
 *
 * This is a diagnostic driver for a Memblaze PBlaze3 card whose NAND
 * flash array has been removed. It does not implement any storage path;
 * it only exposes the FPGA register file and the mailbox commands that
 * reach the on-card firmware:
 *
 *   /sys/kernel/debug/pblaze/<bdf>/regs     key register dump
 *   /sys/kernel/debug/pblaze/<bdf>/stage    firmware lifecycle stage
 *   /sys/kernel/debug/pblaze/<bdf>/xadc     die temperature and voltages
 *   /sys/kernel/debug/pblaze/<bdf>/pcb_temp board temperature
 *   /sys/kernel/debug/pblaze/<bdf>/msg      firmware debug message buffer
 *   /sys/kernel/debug/pblaze/<bdf>/rom      4KB ROM identity key/values
 *   /sys/kernel/debug/pblaze/<bdf>/monitor  health counters ("SMART")
 *
 * The register map, the mailbox handshake and the XADC read sequence
 * follow the 2016 vendor driver (see PROTOCOL.md); timeouts are bounded
 * instead of the original unbounded spins. Interrupts and DMA are not
 * used: every mailbox transaction is polled.
 */

#include <linux/debugfs.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#define PB_PCI_VENDOR_ID	0x1c5f
#define PB_PCI_DEVICE_ID	0x0530

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

#define PB_CMD_READMON		0x06
#define PB_CMD_READROM		0x07
#define PB_CMD_ENDCMD		0x21
#define PB_CMD_DATA1		0x22
#define PB_CMD_DATA2		0x23
#define PB_CMD_EXITINT		0x25

#define PB_IS_MAJORCMD(cmd)	((cmd) >= 0x01 && (cmd) <= 0x20)

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

#define PB_ROM_SIZE		4096
#define PB_MB_TIMEOUT_US	200000
#define PB_XADC_TIMEOUT_US	100000

struct pblaze_monitor {
	u64 logical_read;
	u64 flash_read;
	u64 logical_write;
	u64 flash_write;
	u32 lsa_limit_in_tetris;
	u32 dead_block;
	u16 dead_die;
	u16 init_count;
	u16 start_count;
	u16 over_heating;
	u16 tetris_size_in_system;
	s16 boardtemp_max;
	s16 boardtemp_min;
};

struct pblaze_probe {
	struct pci_dev *pdev;
	void __iomem *regs;
	struct dentry *debugfs;
};

static struct dentry *pblaze_probe_root;

static u32 pb_read(const struct pblaze_probe *pb, unsigned int off)
{
	return readl(pb->regs + off);
}

static void pb_write(const struct pblaze_probe *pb, unsigned int off, u32 val)
{
	writel(val, pb->regs + off);
}

static const char *pb_stage_name(u32 stage)
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

static int pb_xadc_read(const struct pblaze_probe *pb, u8 ch, u16 *out)
{
	u32 val;
	int ret;

	pb_write(pb, PB_REG_XADC, (u32)ch << PB_XADC_WRITE_SHIFT);

	ret = readl_poll_timeout(pb->regs + PB_REG_XADC, val,
				 ((val & PB_XADC_WRITE_MASK) >>
				  PB_XADC_WRITE_SHIFT) == ch,
				 0, PB_XADC_TIMEOUT_US);
	if (ret)
		return ret;

	*out = val & PB_XADC_READ_MASK;
	return 0;
}

static int pb_mb_xfer(const struct pblaze_probe *pb, unsigned int mb,
		      u32 cmd, u16 data_in, u16 *data_out)
{
	unsigned int woff = mb ? PB_REG_WRITE_MB1 : PB_REG_WRITE_MB0;
	unsigned int roff = mb ? PB_REG_READ_MB1 : PB_REG_READ_MB0;
	u32 value = (cmd << 16) | data_in;
	u32 val;
	int ret;

	if (PB_IS_MAJORCMD(cmd))
		value |= PB_ST_INTERRUPT;

	pb_write(pb, woff, value);

	ret = readl_poll_timeout(pb->regs + roff, val,
				 ((val >> PB_ST_CMD_SHIFT) & 0xffu) ==
				 (cmd & 0xffu),
				 0, PB_MB_TIMEOUT_US);
	if (ret)
		return ret;

	if (data_out)
		*data_out = val & PB_ST_DATA_MASK;
	return 0;
}

static void pb_mb_send_end(const struct pblaze_probe *pb, unsigned int mb)
{
	pb_write(pb, mb ? PB_REG_WRITE_MB1 : PB_REG_WRITE_MB0,
		 PB_CMD_EXITINT << 16);
}

/*
 * Multi-word mailbox command, following pblaze_comm_imm(): major
 * command, then len 16-bit words over alternating CMD_DATA1/2, then
 * CMD_ENDCMD and CMD_EXITINT.
 */
static int pb_mb_comm(const struct pblaze_probe *pb, unsigned int mb,
		      u32 cmd, size_t len, u16 *buf)
{
	size_t words = len / sizeof(*buf);
	size_t i;
	int ret;

	ret = pb_mb_xfer(pb, mb, cmd, 0, NULL);
	if (ret)
		return ret;

	for (i = 0; i < words; i++) {
		u32 data_cmd = (i & 1) ? PB_CMD_DATA2 : PB_CMD_DATA1;

		ret = pb_mb_xfer(pb, mb, data_cmd, 0, &buf[i]);
		if (ret)
			return ret;
	}

	ret = pb_mb_xfer(pb, mb, PB_CMD_ENDCMD, 0, NULL);
	if (ret)
		return ret;

	pb_mb_send_end(pb, mb);
	return 0;
}

/*
 * The ROM is a sequence of NUL-terminated key/value string pairs, as
 * consumed by pb_get_str_by_key() in the vendor driver.
 */
static const char *pb_rom_lookup(const char *rom, size_t len,
				 const char *key)
{
	const char *p = rom;
	const char *end = rom + len;

	while (p < end && *p) {
		size_t klen = strnlen(p, end - p);

		if (!strcmp(p, key) && p + klen + 1 < end)
			return p + klen + 1;
		p += klen + 1;
		if (p >= end)
			break;
		p += strnlen(p, end - p) + 1;
	}

	return NULL;
}

static void pb_seq_regline(struct seq_file *s, const char *name,
			   unsigned int off)
{
	seq_printf(s, "%-22s 0x%03x  0x%08x\n", name, off,
		   pb_read(s->private, off));
}

static int pb_regs_show(struct seq_file *s, void *ignored)
{
	pb_seq_regline(s, "version", PB_REG_VERSION);
	pb_seq_regline(s, "compile_date", PB_REG_FPGA_COMPILE_DATE);
	pb_seq_regline(s, "compile_time", PB_REG_FPGA_COMPILE_TIME);
	pb_seq_regline(s, "read_mb0", PB_REG_READ_MB0);
	pb_seq_regline(s, "read_mb1", PB_REG_READ_MB1);
	pb_seq_regline(s, "isr_mask", PB_REG_ISRMASK);
	pb_seq_regline(s, "isr_delay", PB_REG_ISRDELAY);
	pb_seq_regline(s, "isr_info", PB_REG_ISRINFO);
	pb_seq_regline(s, "flow_ctrl_addr", PB_REG_FLOW_CTRL_ADDR);
	pb_seq_regline(s, "flow_ctrl_data", PB_REG_FLOW_CTRL_DATA);

	return 0;
}

static int pb_stage_show(struct seq_file *s, void *ignored)
{
	u32 mb0 = pb_read(s->private, PB_REG_READ_MB0);
	u32 mb1 = pb_read(s->private, PB_REG_READ_MB1);

	seq_printf(s, "MB0 stage %s cmd 0x%02x data 0x%04x msg %u wait %u\n",
		   pb_stage_name((mb0 >> PB_ST_STAGE_SHIFT) & 3u),
		   (mb0 >> PB_ST_CMD_SHIFT) & 0xffu, mb0 & PB_ST_DATA_MASK,
		   (mb0 >> PB_ST_MSG_SHIFT) & 7u,
		   !!(mb0 & PB_ST_WAIT));
	seq_printf(s, "MB1 stage %s cmd 0x%02x data 0x%04x msg %u wait %u\n",
		   pb_stage_name((mb1 >> PB_ST_STAGE_SHIFT) & 3u),
		   (mb1 >> PB_ST_CMD_SHIFT) & 0xffu, mb1 & PB_ST_DATA_MASK,
		   (mb1 >> PB_ST_MSG_SHIFT) & 7u,
		   !!(mb1 & PB_ST_WAIT));

	return 0;
}

/*
 * XADC voltage scale factors, in tenths (scale_num / 10): plain rails
 * are 1.0, VFlash IO is divided by 1.2 and VFlash core by 2.2 in the
 * vendor tool (GetVoltage plus its caller).
 */
static int pb_xadc_show(struct seq_file *s, void *ignored)
{
	static const struct {
		u8 ch;
		const char *name;
		bool is_temp;
		int scale_tenths;
	} table[] = {
		{ PB_XADC_TEMP,		"core_temp_c",	true,	10 },
		{ PB_XADC_MAX_TEMP,	"core_temp_max_c", true,	10 },
		{ PB_XADC_MIN_TEMP,	"core_temp_min_c", true,	10 },
		{ PB_XADC_VCCINT,	"vccint_v",	false,	10 },
		{ PB_XADC_MAX_VCCINT,	"vccint_max_v",	false,	10 },
		{ PB_XADC_MIN_VCCINT,	"vccint_min_v",	false,	10 },
		{ PB_XADC_VCCAUX,	"vccaux_v",	false,	10 },
		{ PB_XADC_MAX_VCCAUX,	"vccaux_max_v",	false,	10 },
		{ PB_XADC_MIN_VCCAUX,	"vccaux_min_v",	false,	10 },
		{ PB_XADC_VDDR3,	"vddr3_v",	false,	10 },
		{ PB_XADC_VFLASH_IO,	"vflash_io_v",	false,	12 },
		{ PB_XADC_VFLASH_CORE,	"vflash_core_v", false,	22 },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(table); i++) {
		u16 raw;
		int ret = pb_xadc_read(s->private, table[i].ch, &raw);

		if (ret) {
			seq_printf(s, "%-16s ch 0x%02x timeout\n",
				   table[i].name, table[i].ch);
			continue;
		}

		if (table[i].is_temp) {
			s64 mc = div64_s64((s64)(raw >> 4) * 503975000, 4096) -
				 273150000;
			s64 whole = div64_s64(mc, 1000000);
			s64 frac = div64_s64(mc % 1000000, 1000);

			if (frac < 0)
				frac = -frac;

			seq_printf(s, "%-16s ch 0x%02x raw 0x%04x %lld.%03lld C\n",
				   table[i].name, table[i].ch, raw,
				   whole, frac);
		} else {
			u64 uv = (u64)(raw >> 4) * 3000000 / 4096 *
				 table[i].scale_tenths / 10;

			seq_printf(s, "%-16s ch 0x%02x raw 0x%04x %llu.%06llu V\n",
				   table[i].name, table[i].ch, raw,
				   uv / 1000000, uv % 1000000);
		}
	}

	return 0;
}

static int pb_pcb_temp_show(struct seq_file *s, void *ignored)
{
	u16 raw = pb_read(s->private, PB_REG_PCB_TEMPERATURE) &
		  PB_PCB_TEMP_MASK;

	/* The vendor net scale is raw / 16 (see pbregs pcb_to_temp) */
	seq_printf(s, "%d.%d C\n", raw / 16, (raw % 16) * 10 / 16);

	return 0;
}

static int pb_msg_show(struct seq_file *s, void *ignored)
{
	struct pblaze_probe *pb = s->private;
	unsigned int off;

	for (off = 0; off < PB_DEBUG_MSG_LENGTH; off++) {
		char c = readb(pb->regs + PB_DEBUG_MSG_ADDR + off);

		if (c == '\0')
			continue;
		seq_putc(s, c >= 0x20 && c < 0x7f ? c : '.');
	}
	seq_putc(s, '\n');

	return 0;
}

static int pb_rom_show(struct seq_file *s, void *ignored)
{
	struct pblaze_probe *pb = s->private;
	static DEFINE_MUTEX(rom_lock);
	u16 *buf;
	const char *rom;
	unsigned int mb;
	int ret = 0;

	mutex_lock(&rom_lock);
	buf = kmalloc(PB_ROM_SIZE, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	rom = (const char *)buf;

	for (mb = 0; mb < 2 && !ret; mb++) {
		ret = pb_mb_comm(pb, mb, PB_CMD_READROM, PB_ROM_SIZE, buf);
		if (ret)
			break;

		seq_printf(s, "# via MB%u\n", mb);
		seq_write(s, rom, PB_ROM_SIZE);
		seq_putc(s, '\n');
	}

	if (ret)
		seq_printf(s, "# mailbox timeout (MB%u)\n", mb);
	else
		seq_printf(s, "# serial: %s\n",
			   pb_rom_lookup(rom, PB_ROM_SIZE, "Serial Number:"));

	kfree(buf);
out_unlock:
	mutex_unlock(&rom_lock);
	return ret;
}

static int pb_monitor_show(struct seq_file *s, void *ignored)
{
	struct pblaze_probe *pb = s->private;
	static DEFINE_MUTEX(mon_lock);
	struct pblaze_monitor m = { 0 };
	u16 *buf;
	unsigned int mb;
	int ret = 0;

	mutex_lock(&mon_lock);
	buf = kmalloc(sizeof(m), GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	for (mb = 0; mb < 2 && !ret; mb++) {
		ret = pb_mb_comm(pb, mb, PB_CMD_READMON, sizeof(m), buf);
		if (ret)
			break;

		m.logical_read = get_unaligned_le64((u8 *)buf);
		m.flash_read = get_unaligned_le64((u8 *)buf + 8);
		m.logical_write = get_unaligned_le64((u8 *)buf + 16);
		m.flash_write = get_unaligned_le64((u8 *)buf + 24);
		m.lsa_limit_in_tetris = get_unaligned_le32((u8 *)buf + 32);
		m.dead_block = get_unaligned_le32((u8 *)buf + 36);
		m.dead_die = get_unaligned_le16((u8 *)buf + 40);
		m.init_count = get_unaligned_le16((u8 *)buf + 42);
		m.start_count = get_unaligned_le16((u8 *)buf + 44);
		m.over_heating = get_unaligned_le16((u8 *)buf + 46);
		m.tetris_size_in_system = get_unaligned_le16((u8 *)buf + 48);
		m.boardtemp_max = get_unaligned_le16((u8 *)buf + 50);
		m.boardtemp_min = get_unaligned_le16((u8 *)buf + 52);

		seq_printf(s, "# via MB%u\n", mb);
		seq_printf(s, "logical_read\t%llu\n", m.logical_read);
		seq_printf(s, "flash_read\t%llu\n", m.flash_read);
		seq_printf(s, "logical_write\t%llu\n", m.logical_write);
		seq_printf(s, "flash_write\t%llu\n", m.flash_write);
		seq_printf(s, "lsa_limit_in_tetris\t%u\n",
			   m.lsa_limit_in_tetris);
		seq_printf(s, "dead_block\t%u\n", m.dead_block);
		seq_printf(s, "dead_die\t%u\n", m.dead_die);
		seq_printf(s, "init_count\t%u\n", m.init_count);
		seq_printf(s, "start_count\t%u\n", m.start_count);
		seq_printf(s, "over_heating\t%u\n", m.over_heating);
		seq_printf(s, "tetris_size_in_system\t%u\n",
			   m.tetris_size_in_system);
		seq_printf(s, "boardtemp_max\t%d\n",
			   (int)m.boardtemp_max * 10 / 128);
		seq_printf(s, "boardtemp_min\t%d\n",
			   (int)m.boardtemp_min * 10 / 128);
	}

	if (ret)
		seq_printf(s, "# mailbox timeout (MB%u): firmware unreachable\n",
			   mb);

	kfree(buf);
out_unlock:
	mutex_unlock(&mon_lock);
	return ret;
}

static int pb_single_open(struct inode *inode, struct file *file,
			  int (*show)(struct seq_file *, void *))
{
	return single_open(file, show, inode->i_private);
}

static int pb_regs_open(struct inode *inode, struct file *file)
{
	return pb_single_open(inode, file, pb_regs_show);
}

static int pb_stage_open(struct inode *inode, struct file *file)
{
	return pb_single_open(inode, file, pb_stage_show);
}

static int pb_xadc_open(struct inode *inode, struct file *file)
{
	return pb_single_open(inode, file, pb_xadc_show);
}

static int pb_pcb_temp_open(struct inode *inode, struct file *file)
{
	return pb_single_open(inode, file, pb_pcb_temp_show);
}

static int pb_msg_open(struct inode *inode, struct file *file)
{
	return pb_single_open(inode, file, pb_msg_show);
}

static int pb_rom_open(struct inode *inode, struct file *file)
{
	return pb_single_open(inode, file, pb_rom_show);
}

static int pb_monitor_open(struct inode *inode, struct file *file)
{
	return pb_single_open(inode, file, pb_monitor_show);
}

static const struct file_operations pb_regs_fops = {
	.owner		= THIS_MODULE,
	.open		= pb_regs_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static const struct file_operations pb_stage_fops = {
	.owner		= THIS_MODULE,
	.open		= pb_stage_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static const struct file_operations pb_xadc_fops = {
	.owner		= THIS_MODULE,
	.open		= pb_xadc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static const struct file_operations pb_pcb_temp_fops = {
	.owner		= THIS_MODULE,
	.open		= pb_pcb_temp_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static const struct file_operations pb_msg_fops = {
	.owner		= THIS_MODULE,
	.open		= pb_msg_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static const struct file_operations pb_rom_fops = {
	.owner		= THIS_MODULE,
	.open		= pb_rom_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static const struct file_operations pb_monitor_fops = {
	.owner		= THIS_MODULE,
	.open		= pb_monitor_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int pblaze_probe_probe(struct pci_dev *pdev,
			      const struct pci_device_id *id)
{
	struct pblaze_probe *pb;
	struct dentry *dir;
	int ret;

	pb = devm_kzalloc(&pdev->dev, sizeof(*pb), GFP_KERNEL);
	if (!pb)
		return -ENOMEM;
	pb->pdev = pdev;
	pci_set_drvdata(pdev, pb);

	ret = pcim_enable_device(pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot enable PCI device\n");

	pb->regs = pcim_iomap(pdev, 0, 0);
	if (!pb->regs)
		return dev_err_probe(&pdev->dev, -ENOMEM,
				     "cannot map BAR0\n");

	dir = debugfs_create_dir(dev_name(&pdev->dev), pblaze_probe_root);
	if (IS_ERR(dir))
		return dev_err_probe(&pdev->dev, PTR_ERR(dir),
				     "cannot create debugfs dir\n");
	pb->debugfs = dir;

	debugfs_create_file("regs", 0400, dir, pb, &pb_regs_fops);
	debugfs_create_file("stage", 0400, dir, pb, &pb_stage_fops);
	debugfs_create_file("xadc", 0400, dir, pb, &pb_xadc_fops);
	debugfs_create_file("pcb_temp", 0400, dir, pb, &pb_pcb_temp_fops);
	debugfs_create_file("msg", 0400, dir, pb, &pb_msg_fops);
	debugfs_create_file("rom", 0400, dir, pb, &pb_rom_fops);
	debugfs_create_file("monitor", 0400, dir, pb, &pb_monitor_fops);

	dev_info(&pdev->dev, "PBlaze3 probe driver attached, fpga version 0x%08x\n",
		 pb_read(pb, PB_REG_VERSION));

	return 0;
}

static void pblaze_probe_remove(struct pci_dev *pdev)
{
	struct pblaze_probe *pb = pci_get_drvdata(pdev);

	debugfs_remove_recursive(pb->debugfs);
}

static const struct pci_device_id pblaze_probe_ids[] = {
	{ PCI_DEVICE(PB_PCI_VENDOR_ID, PB_PCI_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, pblaze_probe_ids);

static struct pci_driver pblaze_probe_driver = {
	.name		= "pblaze_probe",
	.id_table	= pblaze_probe_ids,
	.probe		= pblaze_probe_probe,
	.remove		= pblaze_probe_remove,
};

module_pci_driver(pblaze_probe_driver);

MODULE_DESCRIPTION("Memblaze PBlaze3 no-NAND probe driver");
MODULE_AUTHOR("pblaze3-probe authors");
MODULE_LICENSE("GPL");
