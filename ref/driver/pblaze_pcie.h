#ifndef _PBLAZE_PCIE_H_
#define _PBLAZE_PCIE_H_

#include "pblaze_hal.h"
#include "pblaze_raid.h"

#define PCIE_DRV_NAME			"memcon"

#define PB_PCI_LINK_GEN_1	(1)
#define PB_PCI_LINK_GEN_2	(2)
#define PB_PCI_LINK_GEN_3	(3)

#define PBLAZE_PCIE_VENDOR_ID		0x1c5f
#define PBLAZE_PCIE_DEVICE_ID		0x0530

#ifndef PCI_EXP_LNKSTA_NLW
#define PCI_EXP_LNKSTA_NLW		0x03f0
#endif

#define PCIE_LINK_STATUS_ADDRESS	0x12
#define PCIE_LINK_STATUS_SHIFT		4
#define PCIE_LINK_STATUS_MASK		((1u << 5) - 1)

#define PERF_BH_ACCPACKET		1u
#define CHANNEL_SHIFT			8
#define CHANNEL_SIZE			(1 << CHANNEL_SHIFT)
#define EMBEDED_CPU_SHIFT		1
#define EMBEDED_CPU_SIZE		(1 << EMBEDED_CPU_SHIFT)
#define PERCPU_ACTIVE_SIZE		127

/* Flow Control Registers */
#define	FLOW_CONTROL_ADDR		0x060
#define	FLOW_CONTROL_DATA 		0x064

#define COEFF_C_ADDR			0x1000008
#define WRITE_FC_EN_ADDR		0x100000f

/* Memory Registers */
#define VERSION_ADDR			0x0

#define REG_WRITE_MB0			0x010
#define REG_READ_MB0			0x014
#define REG_WRITE_MB1			0x018
#define REG_READ_MB1			0x01C

#define ISRMASK_ADDR			0x040
#define ISRDELAY_ADDR			0x044
#define ISRINFO_ADDR			0x048

#define REG_FPGA_RELEASE		0x000
#define REG_FPGA_COMPLILE_DATE		0x030
#define REG_FPGA_COMPLILE_TIME		0x034

#define REG_XADC			0x20
#define XADC_WRITE_SHIFT		16
#define XADC_WRITE_MASK			0x007f0000
#define XADC_READ_MASK			0xffff

#define XADC_TEMP_CHN			0x00
#define XADC_INT_CHN			0x01	/* Vcore */
#define XADC_VCCAUX_CHN			0x02	/* peripheral */
#define XADC_MAX_TEMP_CHN		0x20
#define XADC_MAX_INT_CHN		0x21
#define XADC_MAX_VCCAUX_CHN		0x22
#define XADC_MIN_TEMP_CHN		0x24
#define XADC_MIN_INT_CHN		0x25
#define XADC_MIN_VCCAUX_CHN		0x26

#define XADC_VDDR3_CHN			0x11
#define XADC_VFLASH_IO_CHN		0x03
#define XADC_VFLASH_CORE_CHN		0x10

#define REG_PCB_TEMPERATURE		0x38
#define PCB_TEMPERATURE_MASK		0x1fff
#define REG_DMA				0x1000

#define PBLAZE_INT_MUST_DISABLE	(1)

/* Interrupt Vector */
#define INT_EVENT_SHIFT			30
#define INT_EVENT_MASK			3

/* DMA */
#define INT_IDX0_SHIFT			0
#define INT_IDX0_VALID_SHIFT		12
#define INT_IDX1_SHIFT			13
#define INT_IDX1_VALID_SHIFT		25
#define INT_EXIST			28
#define IDX_FIFO_EMPTY			29

#define INT_IDX2_SHIFT			32
#define INT_IDX2_VALID_SHIFT		44
#define INT_IDX3_SHIFT			45
#define INT_IDX3_VALID_SHIFT		57

/* Status Register */
#define ST_DATA_SHIFT			0	/* 0-15 */
#define ST_DATA_MASK			((1 << 16) - 1)
#define ST_CMD_SHIFT			16	/* 16-23 */
#define ST_CMD_MASK			(((1 << 8) - 1) << ST_CMD_SHIFT)
#define ST_MSG_SHIFT			24	/* 24-26 */
#define ST_MSG_MASK			(((1 << 3) - 1) << ST_MSG_SHIFT)
#define ST_ALERT_CHANGE_SHIFT		27	/* 27-28 */
#define ST_ALERT_CHANGE_MASK		(((1 << 1) - 1) << ST_ALERT_CHANGE_SHIFT)
#define ST_STAGE_SHIFT			28	/* 28-29 */
#define ST_STAGE_MASK			(((1 << 2) - 1) << ST_STAGE_SHIFT)
#define ST_WAIT_SHIFT			30
#define ST_WAIT_MASK			(1 << ST_WAIT_SHIFT)
#define ST_INTERRUPT_SHIFT		31
#define ST_INTERRUPT_MASK		(1 << ST_INTERRUPT_SHIFT)

/* Command Register */
#define CMD_MASK			0x7fff
#define CMD_SHIFT			16

#define CMD_ENDCMD			0x21
#define CMD_DATA1			0x22
#define CMD_DATA2			0x23
#define CMD_TOGGLEDEFAULT		0x24
#define CMD_EXITINT			0x25

#define CMD_REINIT			0x14
#define CMD_BADTETRIS_INFO		0x3
#define CMD_READINFO			0x5
#define CMD_READMON			0x6
#define CMD_READROM			0x7
#define CMD_LOCKROM			0x2
#define CMD_WRITEROM			0x11
#define CMD_WRITEFIRMWARE		0x18
#define CMD_CLEARWAIT			0x9
#define CMD_CLEARINT			0xa
#define CMD_BAECON			0x1b
#define CMD_CLEARALERT			0x1c

#define MIN_MAJORCMD			0x1
#define MAX_MAJORCMD			0x20
#define CMD_TODEVMASK			0x10
#define IS_CMDTODEV(cmd)		((cmd & CMD_TODEVMASK) != 0)
#define IS_MAJORCMD(cmd)		(cmd >= MIN_MAJORCMD && cmd <= MAX_MAJORCMD)

#define CMD_ECHO_SUCCEED		0
#define CMD_ECHO_FAILED			1
#define CMD_ECHO_INVALID_PARMA		2
#define CMD_ECHO_INVALID_DEVICE		3
#define CMD_ECHO_PROBE_NO_FINISH		4
#define CMD_ECHO_MALLOC_FAILED		5

#define DATA_MASK			0xffff
#define DATA_SHIFT			0u

#define MSG_NULL			0
#define MSG_PRINT			1
#define MSG_READYWAIT			2
#define MSG_BADTETRIS			3

#define MEMDISK_MAJORS			32
#define MEMDISK_MINORS			32

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 18)
#define IRQF_SHARED			0x00000080
#endif

#define PKT_NULL			((u32)-1)

#define NR_SENTS			64
#define NR_RECVDS			64

#define SIGNATUREWIDTH			8
#define MAGICNUMBER			0x32132325

#define NR_LUN_PER_TETRIS		7

#define DMA_PROCESS_INIT		0x0
#define DMA_PROCESS_TRIM		0x0
#define DMA_PROCESS_ENTRY		0x100

#define DEV_FAULT			0
#define DEV_RDONLY			1
#define DEV_SHUTDOWN			2

/* The position bit in u32 to mark tasklet already enter */
#define INTR_LOCK_BIT			20
#define INTR_LOCK_MASK			(0x1 << INTR_LOCK_BIT)

/* The debug message register */
#define	DEBUG_MSG_LENGTH	(0xc00)
#define	DEBUG_MSG_ADDR		(0x400)

/* The ioreq max time value */
#define	PB_IOREQ_TIMEOUT	(HZ * 60)

enum {
	PB_INTX_MODE,
	PB_MSI_MODE,
	PB_MSIX_MODE,
	PB_MSIX_UNSUPPORT_MODE,
	PB_UNKNOWN_MODE,
	PB_INTR_MODE_ITEMS
};

union pblaze_pcie_op {
	struct {
		u32 is_cmd:1;
		u32 msi_vec:7;
		u32 nr_ents:8;
		u32 trim_idx:12;
		u32 reserved:2;
		u32 is_write:1;
		u32 is_trim:1;
		u32 lba;
	} cmd;
	struct {
		u32 sba_low;
		u32 sba_high:16;
		u32 idx:12;
		u32 reserved2:4;
	} entry;
};

struct pblaze_dma_packet {
	dma_addr_t addr;
	size_t size;
	struct io_request *ioreq;
	u32 next;
};

struct pblaze_recycle_desc {
	u16 rfifo[INTERRUPT_QUEUE_SIZE];
	volatile u32 push_pos;
	volatile u32 pop_pos;
};

struct pblaze_dma_packet_pool {
	struct pblaze_dma_packet pool[INTERRUPT_QUEUE_SIZE];

	/* Multi tasklet will access recycle_fifo parallel */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 34)
	struct pblaze_recycle_desc *recycle_percpu;
#else
	struct pblaze_recycle_desc __percpu *recycle_percpu;
#endif

	/* 1 : used; 0 : free */
	volatile u8 packets_stat[INTERRUPT_QUEUE_SIZE];
	volatile u32 free_pkts_head;
	volatile u32 flying_packets;

#ifdef PBLAZE_COUNTER
	volatile u32 ops_cnt[NR_OPS_CNT_ITEMS];
	volatile u32 pkts_stat[NR_PKT_STAT_ITEMS];
#endif
};

struct pblaze_comm_dev {
	struct list_head cmd_list;	/* ioctl cmd list */
	spinlock_t cmd_list_lock;	/* protecting cmd_list */
	volatile u32 status;
	u32 instruction_index;
	u8 *read_reg;
	u8 *write_reg;
	struct pblaze_monitor monitor;	/* For temp read buffer */
	struct pblaze_pcie *owner;
};

struct pblaze_cmd {
	/* Link into the pblaze_comm_dev->cmd_list */
	struct list_head node;
	struct pblaze_ioctl_request *ioctl_req;
	u16 *data;
};

struct pblaze_print_info {
	u16 infolen;
	u16 paramlen;
	u16 strlen;
	u32 value[16 + 256 / sizeof(u32)];
};

struct pblaze_bad_tetris_info {
	u16 id;
	u16 err_block_cnt;
};

struct pblaze_ioctl_request {
	/* record the right pending number for different ioctl cmd */
	u32 major_cmd;
	u32 len;
	bool is_write;

	volatile u32 pending_dev;
	int request_status;
	struct completion ioctl_comp;
};

struct pblaze_pcie;

struct pblaze_tasklet_entry {
	u8 id;
	struct pblaze_pcie *pcie;
	struct tasklet_struct pcie_tasklet;
};

struct pblaze_pcie {
	struct pci_dev *dev;
	struct task_struct *submit_thread;
	struct semaphore submit_sem;

	u64 ioreq_list_length;
	struct list_head ioreq_list;	/* every pcie has a io_request list */
	spinlock_t ioreq_list_lock;	/* protecting ioreq_list */

	u8 *regs;			/* ioremap_nocache */
	union {
		struct pblaze_comm_dev pcd[COMM_DEV_START + COMM_DEV_COUNT];
		struct {
			struct pblaze_comm_dev pcd_master;
			struct pblaze_comm_dev pcd_slave;
		};
	};

	struct pblaze_dma_packet_pool pool;

	/* The current pcie dma handle context */
	volatile u32 level;		/* Make DMA handle serial */
	u32 dma_process;		/* INIT,TRIM,ENTRY */
	u32 packet_chain_first;
	u32 packet_chain_cur;
	u32 cur_dma_entries;
	dma_addr_t *cur_dma_addr;

	volatile u32 intr_stat;		/* Interrupt status of cpu devs */
	volatile u32 intr_mask;		/* Interupt mask (0 or 0xffffffff) */

	void *secure_trim_addr;
	dma_addr_t secure_trim_dma_addr;

	/* For unsecure trim */
	struct io_request dummy_ioreq[0];

	/* Prevent a process call oneself */
	volatile u32 task_hash[8];

	u32 c2s_values[COMM_DEV_START + COMM_DEV_COUNT];
	u32 link_width;
	u32 link_gen;
	struct pblaze_init_monitor init_monitor; /* For temp read buffer */

	u32 sub_dev_index;
	u32 quota;			/* quota in one cycle */
	s32 *addr_tbl;			/* mapping addr table, max range:8T */

	volatile bool is_stop;
	volatile bool is_stoped;
	struct kref refcn;
	struct pblaze_raid *raid;
#ifdef PBLAZE_COUNTER
	volatile u32 cur_cnt;
	volatile u32 max_cnt;
#endif
	volatile u32 empty_cnt;

	s32 interrupt_mode;
	struct msix_entry msix_entries[MSIX_TABLE_SIZE];
	volatile u32 tasklet_iter;
	u32 tasklet_num;
	struct pblaze_tasklet_entry tasklet_entries[0];
	/* New member must not be placed here */
	u32 interrupt_flag;
};

void pblaze_comm_noimm(struct pblaze_cmd *ioctl_cmd, struct pblaze_pcie *pcie,
		       u32 pcd_idx);
int pblaze_comm_imm(u32 major_cmd, u32 len, u16 *data, bool is_write,
		     struct pblaze_comm_dev *pcd);

void pblaze_pcie_issue_ioreq(struct pblaze_pcie *dev, bool is_work_thread);
bool pblaze_pcie_is_recycle_available(struct pblaze_recycle_desc *recycle_desc);
void pblaze_pcie_recycle_pkts(struct pblaze_pcie *pcie, struct pblaze_recycle_desc *recycle_desc);
void pblaze_pcie_check_timeout(struct pblaze_pcie *pcie);
void pblaze_generate_bug_report(struct pblaze_raid *raid);
void pblaze_trigger_pcie_interrupt(struct pblaze_pcie *pcie);
#ifdef PBLAZE_COUNTER
static inline void pblaze_pcie_update_sent_pkts(struct pblaze_dma_packet_pool *pool)
{
	pb_atomic_dec(&pool->pkts_stat[NR_PKT_ALLOCED]);
	pb_atomic_inc(&pool->pkts_stat[NR_PKT_SENT]);
}

static inline void pblaze_pcie_update_recvd_pkts(struct pblaze_dma_packet_pool *pool, u32 recycle_id)
{
	u32 tmp = recycle_id;

	do {
		pb_atomic_dec(&pool->pkts_stat[NR_PKT_SENT]);
		pb_atomic_inc(&pool->pkts_stat[NR_PKT_RECVD]);

		tmp = pool->pool[tmp].next;
		if (tmp == PKT_NULL) {
			DPRINTK(ERR, "Packet %u recvd linked to PKT_NULL\n", recycle_id);
			return;
		}
	} while (tmp != recycle_id);
}

static inline void pblaze_pcie_update_dma_cmd(struct pblaze_dma_packet_pool *pool, union pblaze_pcie_op *op)
{
	if (op->cmd.is_cmd) {
		pb_atomic_inc(&pool->ops_cnt[NR_DMA_CMDS]);
		if (op->cmd.is_write)
			pb_atomic_inc(&pool->ops_cnt[NR_DMA_WRITE]);
		else
			pb_atomic_inc(&pool->ops_cnt[NR_DMA_READ]);
	} else {
		pb_atomic_inc(&pool->ops_cnt[NR_DMA_ENTRYS]);
	}

	pb_atomic_inc(&pool->ops_cnt[NR_DMA_OPS]);
}

#define pblaze_pcie_stat_set(ptr, value)	(*ptr = value)
#define pblaze_pcie_stat_inc(ptr)		pb_atomic_inc(ptr)
#define pblaze_pcie_stat_dec(ptr)		pb_atomic_dec(ptr)
#define pblaze_pcie_stat_add(ptr, value)	pb_atomic_add(ptr, value)
#define pblaze_pcie_stat_sub(ptr, value)	pb_atomic_sub(ptr, value)
#define pblaze_pcie_stat_sent(pool)		pblaze_pcie_update_sent_pkts(pool)
#define pblaze_pcie_stat_recvd(pool, id)	pblaze_pcie_update_recvd_pkts(pool, id)
#define pblaze_pcie_stat_dma(pool, op)		pblaze_pcie_update_dma_cmd(pool, op)
#else
#define pblaze_pcie_stat_set(ptr, value)
#define pblaze_pcie_stat_inc(ptr)
#define pblaze_pcie_stat_dec(ptr)
#define pblaze_pcie_stat_add(ptr, value)
#define pblaze_pcie_stat_sub(ptr, value)
#define pblaze_pcie_stat_sent(pool)
#define pblaze_pcie_stat_recvd(pool, id)
#define pblaze_pcie_stat_dma(pool, op)
#endif

#endif
