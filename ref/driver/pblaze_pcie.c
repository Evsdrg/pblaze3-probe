#include <linux/vmalloc.h>

#include "pblaze_pcie.h"
#include "pblaze_raid.h"

const char *driver_version = "1.1.0.0 (Compiled on "__DATE__" "__TIME__")";
int pblaze_int_interval = 0x0c000100;
int pblaze_msi_disable = 0;
int pblaze_msi_tbl_lock = 0;

module_param_named(int_interval, pblaze_int_interval, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(int_interval, "Set the interrupt delay and write delay");

module_param_named(msi_disable, pblaze_msi_disable, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(msi_disable, "Disable the msi feature");

module_param_named(msi_tbl_lock, pblaze_msi_tbl_lock, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(msi_tbl_lock, "Lock the msi table");

static void pblaze_collect_system_info(struct pblaze_raid *raid)
{
	schedule_work(&raid->bug_report_work);
}

/* 
static void pblaze_collect_firmware_message(struct pblaze_pcie *pcie)
{
	u32 value = 0;
	int i = 0;

	printk("Collecting firmware message start:\n");
	for (i = 0; i < (DEBUG_MSG_LENGTH / sizeof(u32)); i++) {
		value = readl(pcie->regs + DEBUG_MSG_ADDR + i * sizeof(u32));
		printk("0x%x ", value);
	}
	printk("\nCollecting firmware message end\n");
}
*/

static inline int is_ioreq_timeout(struct io_request *ioreq)
{
	BUG_ON(ioreq == NULL);
	return time_after_eq(jiffies, ioreq->submit_jiffies + PB_IOREQ_TIMEOUT);
}

static inline void pblaze_display_timeout_ioreq(struct io_request *ioreq)
{
	BUG_ON(ioreq == NULL);
	DPRINTK(ERR, "Timeout io_requst 0x%p, bio 0x%p, is_dre %d, "
		"ioreq_len %llu,ioreq_start %llu, "
		"submit jiffies %lu, issue jiffies %lu\n",
		ioreq, ioreq->bio, ioreq->is_dre,
		ioreq->ioreq_len, ioreq->ioreq_start, 
		ioreq->submit_jiffies, ioreq->issue_jiffies);
}

void pblaze_pcie_check_timeout(struct pblaze_pcie *pcie)
{
	u32 dev_index = pcie->sub_dev_index;
	struct io_request *ioreq = NULL;

	DPRINTK(ERR, "Check timeout ioreq on pcie %d, current jiffies %lu\n", 
		dev_index, jiffies);

	spin_lock_bh(&pcie->ioreq_list_lock);
	list_for_each_entry(ioreq, &pcie->ioreq_list, privs[dev_index].node) {
		if (is_ioreq_timeout(ioreq)) {
			pblaze_display_timeout_ioreq(ioreq);
		}
	}
	spin_unlock_bh(&pcie->ioreq_list_lock);
}

void pblaze_generate_bug_report(struct pblaze_raid *raid)
{
	struct pblaze_pcie *pcie = NULL;
	int i = 0;
	
	for (i = 0; i < raid->nr_dev; i++) {
		pcie = raid->pcie_devs[i];
		pblaze_dump_pcie_fpga_counter(pcie);
		/* pblaze_collect_firmware_message(pcie); */
	}

	pblaze_collect_system_info(raid);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 38)
static struct pci_device_id pblaze_pcie_tbl[] __initdata = {
	{ PBLAZE_PCIE_VENDOR_ID, PBLAZE_PCIE_DEVICE_ID,
	PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{ 0, }
};
#else
static struct pci_device_id pblaze_pcie_tbl[] = {
	{ PBLAZE_PCIE_VENDOR_ID, PBLAZE_PCIE_DEVICE_ID,
	PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{ 0, }
};
#endif

static void release_pblaze_pcie(struct kref *refcn)
{
	struct pblaze_pcie *pcie;

	DPRINTK(INFO, "release pcie device\n");

	pcie = container_of(refcn, struct pblaze_pcie, refcn);
	vfree(pcie);
}

static void pblaze_pcie_enable_intx(struct pblaze_pcie *pcie)
{
	pcie->intr_mask = 0xffffffff;
	writel(1, pcie->regs + ISRMASK_ADDR);
}

static void pblaze_pcie_disable_intx(struct pblaze_pcie *pcie)
{
	writel(0, pcie->regs + ISRMASK_ADDR);
	readl(pcie->regs + ISRMASK_ADDR);
	readl(pcie->regs + ISRMASK_ADDR);
	pcie->intr_mask = 0;
}

static inline void pblaze_send_cmd_end(struct pblaze_comm_dev *pcd)
{
	writel((CMD_EXITINT << CMD_SHIFT), pcd->write_reg);
}

inline u32 pblaze_transfer_cur_cmd(struct pblaze_cmd *ioctl_cmd,
				   u32 instruction_index, u16 *data_in, u16 **data_out)
{
	u32 cur_cmd;

	*data_out = NULL;

	if (instruction_index == 1) {
		cur_cmd = ioctl_cmd->ioctl_req->major_cmd;
	} else if (instruction_index == ioctl_cmd->ioctl_req->len + 2) {
		cur_cmd = CMD_ENDCMD;
	} else {
		cur_cmd = (instruction_index & 1) ? CMD_DATA1 : CMD_DATA2;

		if (ioctl_cmd->ioctl_req->is_write) {
			*data_in = ioctl_cmd->data[instruction_index - 2];
		} else {
			*data_out = ioctl_cmd->data + (instruction_index - 2);
		}
	}

	return cur_cmd;
}

static int pblaze_send_cmd_noimm(struct pblaze_comm_dev *pcd)
{
	struct pblaze_cmd *ioctl_cmd = NULL;
	u32 cur_cmd;
	u32 last_cmd;
	u16 data_in;
	u16 *data_out;
	u32 reg_read;
	u32 isr_status;
	int retry;

	data_in = 0u;
	data_out = NULL;

	/* Cuz of the multi tasklet, we must protect here */
	spin_lock(&pcd->cmd_list_lock);
	if (!list_empty(&pcd->cmd_list))
		ioctl_cmd = list_first_entry(&pcd->cmd_list, struct pblaze_cmd, node);
	spin_unlock(&pcd->cmd_list_lock);

	while (ioctl_cmd != NULL) {
		retry = 40;

		/* Must check it cuz this flow may break when retry is 0 */
		if (pcd->instruction_index == 0) {
			++pcd->instruction_index;
			cur_cmd = pblaze_transfer_cur_cmd(ioctl_cmd, pcd->instruction_index, &data_in, &data_out);
			isr_status = IS_MAJORCMD(cur_cmd) ? ST_INTERRUPT_MASK : 0u;
			writel(isr_status | (cur_cmd << CMD_SHIFT) | (((u32)data_in) << DATA_SHIFT), pcd->write_reg);
		}

		cur_cmd = pblaze_transfer_cur_cmd(ioctl_cmd, pcd->instruction_index, &data_in, &data_out);
		while (true) {
			reg_read = readl(pcd->read_reg);
			last_cmd = (reg_read & ST_CMD_MASK) >> ST_CMD_SHIFT;

			if (last_cmd != cur_cmd) {
				if (--retry == 0) {
					return -ETIMEDOUT;
				}
				continue;
			}

			retry = 40;

			if (data_out != NULL) {
				*data_out = (reg_read & ST_DATA_MASK) >> ST_DATA_SHIFT;
			}

			if (++pcd->instruction_index == ioctl_cmd->ioctl_req->len + 3) {
				pblaze_send_cmd_end(pcd);
				pcd->instruction_index = 0;
				spin_lock(&pcd->cmd_list_lock);
				list_del(&ioctl_cmd->node);
				spin_unlock(&pcd->cmd_list_lock);

				if (ioctl_cmd->ioctl_req->request_status == CMD_ECHO_SUCCEED) {
					ioctl_cmd->ioctl_req->request_status = (reg_read & ST_DATA_MASK) >> ST_DATA_SHIFT;
				}

				/* Multi-tasklet will access pending_dev parallel */
				if (pb_atomic_dec(&ioctl_cmd->ioctl_req->pending_dev) == 0) {
					complete(&ioctl_cmd->ioctl_req->ioctl_comp);
				}

				spin_lock(&pcd->cmd_list_lock);
				if (!list_empty(&pcd->cmd_list))
					ioctl_cmd = list_first_entry(&pcd->cmd_list, struct pblaze_cmd, node);
				else
					ioctl_cmd = NULL;
				spin_unlock(&pcd->cmd_list_lock);

				break;
			}

			cur_cmd = pblaze_transfer_cur_cmd(ioctl_cmd, pcd->instruction_index, &data_in, &data_out);
			isr_status = IS_MAJORCMD(cur_cmd) ? ST_INTERRUPT_MASK : 0u;
			writel(isr_status | (cur_cmd << CMD_SHIFT) | (((u32)data_in) << DATA_SHIFT), pcd->write_reg);
		}
	}

	return 0;
}

void pblaze_comm_noimm(struct pblaze_cmd *ioctl_cmd, struct pblaze_pcie *pcie,
		       u32 pcd_idx)
{
	struct pblaze_comm_dev *pcd;

	pcd = &pcie->pcd[pcd_idx];

	/* link task to pcd linked list */
	spin_lock_bh(&pcd->cmd_list_lock);
	list_add_tail(&ioctl_cmd->node, &pcd->cmd_list);
	spin_unlock_bh(&pcd->cmd_list_lock);

	/* trigger interrupt tasklet */
	pb_atomic_or(&pcie->intr_stat, 1u << pcd_idx);

	if (pcie->interrupt_mode == PB_INTX_MODE)
		pblaze_pcie_disable_intx(pcie);

	tasklet_schedule(&pcie->tasklet_entries[0].pcie_tasklet);
}

inline u32 pblaze_send_cmd_imm(u32 cmd, u16 data, struct pblaze_comm_dev *pcd)
{
	u32 read_value;
	u32 value;
	unsigned long start_timestamp = jiffies;

	cmd &= CMD_MASK;
	value = IS_MAJORCMD(cmd) ? ST_INTERRUPT_MASK : 0u;
	value |= (cmd << CMD_SHIFT) | (((u32)data & DATA_MASK) << DATA_SHIFT);

	do {
		writel(value, pcd->write_reg);
		read_value = readl(pcd->read_reg);
#ifdef ASSERTCHECK
		if (read_value == 0xffffffff) {
			DPRINTK(DEBUG, "%08x\n", readl(pcd->read_reg));
		}
#endif
		if (time_after_eq(jiffies, start_timestamp + HZ * 5 * 60)) {
			DPRINTK(ERR, "cmd 0x%x, write_value 0x%x, read_value 0x%x\n",
					cmd, value, read_value);
			pblaze_dump_pcie_fpga_counter(pcd->owner);
			return -1;
		}

	} while (((read_value & ST_CMD_MASK) >> ST_CMD_SHIFT) != cmd);

	return (read_value & ST_DATA_MASK) >> ST_DATA_SHIFT;
}

int pblaze_comm_imm(u32 major_cmd, u32 len, u16 *data, bool is_write,
			    struct pblaze_comm_dev *pcd)
{
	u32 cmd, stat;

	stat = pblaze_send_cmd_imm(major_cmd, 0, pcd);
	if (stat == -1) {
		DPRINTK(ERR, "pblaze send command to frimware failed!\n");
		return stat;
	} 

	DPRINTK(INFO, "cmd %x\n", major_cmd);

	len = len / sizeof(u16);
	cmd = CMD_DATA1;

	while (len-- != 0) {
		if (is_write) {
			stat = pblaze_send_cmd_imm(cmd, *data, pcd);
			if (stat == -1) {
				DPRINTK(ERR, "pblaze send command to frimware failed!\n");
				return stat;
			}
		} else {
			stat = pblaze_send_cmd_imm(cmd, 0, pcd);
			if (stat == -1) {
				DPRINTK(ERR, "pblaze send command to frimware failed!\n");
				return stat;
			}
			*data = stat;
		}
		cmd = CMD_DATA1 + CMD_DATA2 - cmd;

		DPRINTK(INFO, "cmd %x\n", cmd);

		++data;
	}

	stat = pblaze_send_cmd_imm(CMD_ENDCMD, 0, pcd);
	if (stat == -1) {
		DPRINTK(ERR, "pblaze send command to frimware failed!\n");
		return stat;
	}

	DPRINTK(INFO, "cmd %x\n", CMD_ENDCMD);

	pblaze_send_cmd_end(pcd);
	return 0;
}

static void pblaze_clear_wait(struct pblaze_comm_dev *pcd)
{
	DPRINTK(DEBUG, "clear wait++\n");
	pblaze_comm_imm(CMD_CLEARWAIT, 0, NULL, false, pcd);
	DPRINTK(DEBUG, "clear wait--\n");
}

static void pblaze_clear_intr(struct pblaze_comm_dev *pcd)
{
	DPRINTK(DEBUG, "clear interrupt++\n");
	pblaze_comm_imm(CMD_CLEARINT, 0, NULL, false, pcd);
	DPRINTK(DEBUG, "clear interrupt--\n");
}

static void pblaze_clear_alert(struct pblaze_comm_dev *pcd)
{
	DPRINTK(DEBUG, "clear alert++\n");
	pblaze_comm_imm(CMD_CLEARALERT, 0, NULL, false, pcd);
	DPRINTK(DEBUG, "clear alert--\n");
}

static void pblaze_pcie_recv_monitor(struct pblaze_pcie *pcie,
				     struct pblaze_monitor *monitor)
{
	pblaze_comm_imm(CMD_READMON, sizeof(struct pblaze_monitor),
			(u16 *)monitor, false, &pcie->pcd_slave);
}

static void pblaze_read_info(struct pblaze_pcie *pcie,
			     struct pblaze_comm_dev *pcd, u32 index)
{
	char format[256 + 32];
	char *tags[] = {"PCIe=%u TM SN=%s ", "PCIe=%u TS SN=%s "};
	const char *sn;
	struct pblaze_print_info print_info;

	pblaze_comm_imm(CMD_READINFO, sizeof(struct pblaze_print_info),
			(u16 *)&print_info, false, pcd);
	strcpy(format, tags[index]);
	strncat(format, (char *)(print_info.value + print_info.paramlen),
		print_info.strlen);
	sn = pb_get_str_by_key(pcie->raid->init_monitor.rom, "Serial Number:");

	printk(format, pcie->sub_dev_index, sn,
	       print_info.value[0], print_info.value[1], print_info.value[2],
	       print_info.value[3], print_info.value[4], print_info.value[5],
	       print_info.value[6], print_info.value[7], print_info.value[8],
	       print_info.value[9], print_info.value[10], print_info.value[11],
	       print_info.value[12], print_info.value[13],
	       print_info.value[14], print_info.value[15]);

	set_bit(PB_NOTICE_PRINT, &pcie->raid->error_code);
}

static void pblaze_read_bad_tetris(struct pblaze_pcie *pcie,
				   struct pblaze_comm_dev *pcd, u32 index)
{
	struct pblaze_bad_tetris_info bad_tetris_info;

#ifdef RDONLY_CHECK
	set_bit(DEV_RDONLY, &pcie->raid->flags);
#endif
	set_bit(PB_CRITICAL_RDONLY, &pcie->raid->error_code);

	pblaze_comm_imm(CMD_BADTETRIS_INFO,
			sizeof(struct pblaze_bad_tetris_info),
			(u16 *)&bad_tetris_info, false, pcd);

	DPRINTK(ALERT, "Raid:%s, pcie:%u, cpu:%u, becomes read only \
	       since tetris%u contains %u bad blocks\n",
	       pcie->raid->gendisk->disk_name, pcie->sub_dev_index, index,
	       bad_tetris_info.id, bad_tetris_info.err_block_cnt);
}

static void pblaze_pcd_handle(struct pblaze_pcie *pcie, u32 index)
{
	int ret;
	u32 value, new_status;
	struct pblaze_comm_dev *pcd;

	pcd = pcie->pcd + index;
	value = readl(pcd->read_reg);

	/* Update device status change event */
	new_status = (value & ST_STAGE_MASK) >> ST_STAGE_SHIFT;

	if ((pcd->status != new_status) &&
	    (pcd->status != ST_STAGE_MID || new_status != ST_STAGE_READY)) {
		DPRINTK(INFO, "pcie:%u, pcd:%u change status from %u to %u\n",
			pcie->sub_dev_index, index, pcd->status, new_status);
		pcd->status = new_status;
		pcie->raid->new_status = new_status;

		DPRINTK(NOTICE, "nr_dev:%u nr_dev_max:%u\n", 
				pcie->raid->nr_dev, pcie->raid->nr_dev_max);
		if (pcie->raid->nr_dev == pcie->raid->nr_dev_max) {
			/* nr_dev and status should be all updated done */
			mb();
			schedule_work(&pcie->raid->stat_work);
		}
	}

	/* Handle device cmd first */
	ret = pblaze_send_cmd_noimm(pcd);
	if (ret) {
		DPRINTK(INFO, "Device send cmd sequence timeout\n");
	} else if (value & ST_WAIT_MASK) {
		switch ((value & ST_MSG_MASK) >> ST_MSG_SHIFT) {
		case MSG_READYWAIT:
			DPRINTK(DEBUG, "msg ready wait\n");
			break;
		case MSG_PRINT:
			DPRINTK(DEBUG, "msg print\n");
			pblaze_read_info(pcie, pcd, index);
			break;
		case MSG_BADTETRIS:
			DPRINTK(DEBUG, "msg bad tetris\n");
			pblaze_read_bad_tetris(pcie, pcd, index);
			break;
		default:
			DPRINTK(DEBUG, "msg unknown:%x\n",
				(value & ST_MSG_MASK) >> ST_MSG_SHIFT);
			break;
		}

		/*
		 * If pcd clear wait when ready, set status to ST_STAGE_MID.
		 * during this MID-RUN period, reinit op should wait it to run.
		 */
		if (pcd->status == ST_STAGE_READY) {
			if (pblaze_test_unload(pcie->raid) == false) {
				pblaze_clear_wait(pcd);
				pcd->status = ST_STAGE_MID;
			}
		} else {
			/* In this case, raid's status will change to READY */
			pblaze_clear_wait(pcd);
		}
	} else if (value & ST_ALERT_CHANGE_MASK) {
		struct pblaze_monitor monitor;

		pblaze_pcie_recv_monitor(pcie, &monitor);
		if (monitor.over_heating)
			printk("Pcie:%d core temperature alert occur\n", pcie->sub_dev_index);
		else
			printk("Pcie:%d core temperature alert clear\n", pcie->sub_dev_index);
		pblaze_clear_alert(pcd);
	} else {
		if (pcie->interrupt_mode == PB_INTX_MODE) 
			pblaze_clear_intr(pcd);
		else if (value & ST_INTERRUPT_MASK)
			pblaze_clear_intr(pcd);
	}

	/* For proc information */
	pcie->c2s_values[index] = readl(pcd->read_reg);
}

int pblaze_pcie_init_dma_pool(struct pblaze_dma_packet_pool *pool)
{
	int ret = 0;
	u32 i;

	pool->flying_packets = 0;
	pool->free_pkts_head = 0;
	for (i = 0; i != INTERRUPT_QUEUE_SIZE - 1; ++i)
		pool->pool[i].next = i + 1;
	pool->pool[INTERRUPT_QUEUE_SIZE - 1].next = PKT_NULL;

	pool->recycle_percpu = alloc_percpu(struct pblaze_recycle_desc);
	if (pool->recycle_percpu == NULL) {
		DPRINTK(ERR, "Alloc recycle_percpu error\n");
		ret = -ENOMEM;
	}

	return ret;
}

static struct io_request *pblaze_pcie_get_ioreq(struct pblaze_pcie *pcie,
						u32 dev_index)
{
	struct io_request *ioreq_head = NULL;

	spin_lock_bh(&pcie->ioreq_list_lock);
	if (!list_empty(&pcie->ioreq_list))
		ioreq_head = list_first_entry(&pcie->ioreq_list, struct io_request, privs[dev_index].node);
	spin_unlock_bh(&pcie->ioreq_list_lock);

	return ioreq_head;
}

static void pblaze_pcie_del_ioreq(struct pblaze_pcie *pcie,
				  struct io_request *ioreq, u32 dev_index)
{
	spin_lock_bh(&pcie->ioreq_list_lock);
	list_del(&ioreq->privs[dev_index].node);
	pcie->ioreq_list_length--;
	spin_unlock_bh(&pcie->ioreq_list_lock);

	pblaze_pcie_stat_inc(&pcie->raid->submit_cnt);
}

u32 pblaze_pcie_get_free_pkt(struct pblaze_dma_packet_pool *pool)
{
	u32 head_id;
	u32 new_head_id;

	head_id = pool->free_pkts_head;
	if (likely(head_id != PKT_NULL)) {
		pb_atomic_inc(&pool->flying_packets);
		new_head_id = pool->pool[head_id].next;
		while (pb_atomic_cmpxchg(&pool->free_pkts_head, head_id, new_head_id) != head_id) {
			/*
			 * No need to check head_id==PKT_NULL again
			 * since GetFreePacket is single thread accessible.
			 */
			head_id = pool->free_pkts_head;
			new_head_id = pool->pool[head_id].next;
		}

		pblaze_pcie_stat_dec(&pool->pkts_stat[NR_PKT_FREE]);
		pblaze_pcie_stat_inc(&pool->pkts_stat[NR_PKT_ALLOCED]);
	}

	return head_id;
}

void pblaze_pcie_set_free_pkt(struct pblaze_dma_packet_pool *pool, u32 new_head_id)
{
	u32 head_id;
	struct pblaze_dma_packet *p = &pool->pool[new_head_id];

	pb_atomic_dec(&pool->flying_packets);
	do {
		head_id = pool->free_pkts_head;
		p->next = head_id;
	} while (pb_atomic_cmpxchg(&pool->free_pkts_head, head_id, new_head_id) != head_id);

	pblaze_pcie_stat_dec(&pool->pkts_stat[NR_PKT_ALLOCED]);
	pblaze_pcie_stat_inc(&pool->pkts_stat[NR_PKT_FREE]);
}

bool pblaze_pcie_set_free_pkt_range(struct pblaze_dma_packet_pool *pool,
				    u32 new_head_id, u32 *tail_id, int size)
{
	u32 head_id;

	pb_atomic_sub(&pool->flying_packets, (u32)size);
	do {
		head_id = pool->free_pkts_head;
		*tail_id = head_id;
	} while (pb_atomic_cmpxchg(&pool->free_pkts_head, head_id, new_head_id) != head_id);

	pblaze_pcie_stat_add(&pool->pkts_stat[NR_PKT_FREE], size);
	pblaze_pcie_stat_sub(&pool->pkts_stat[NR_PKT_RECVD], size);

	return head_id == PKT_NULL;
}

static inline void __pblaze_pcie_issue_ioreq(struct pblaze_pcie *pcie,
					     unsigned long timeout)
{
	u32 pkt_free_id;
	u32 dev_index;
	union pblaze_pcie_op op;
	struct io_request *ioreq_head;
	struct io_request_priv *priv;
	struct pblaze_dma_packet *pkt_free;
	dma_addr_t dma_addr;

	dev_index = pcie->sub_dev_index;
	pkt_free_id = PKT_NULL;

	while ((ioreq_head = pblaze_pcie_get_ioreq(pcie, dev_index)) != NULL
		&& time_before(jiffies, timeout)) {
		memset(&op, 0, sizeof(union pblaze_pcie_op));
		priv = &(ioreq_head->privs[dev_index]);

		if (pcie->dma_process == DMA_PROCESS_ENTRY) {
			pkt_free_id = pcie->packet_chain_first;
		} else if (pkt_free_id == PKT_NULL) {
			pkt_free_id = pblaze_pcie_get_free_pkt(&pcie->pool);
			if (pkt_free_id == PKT_NULL) {
				DPRINTK(INFO, "Issue ioreq: get null packet\n");
				return;
			}
		}

		pkt_free = pcie->pool.pool + pkt_free_id;

		if (pcie->dma_process == DMA_PROCESS_INIT) {
			if (unlikely(pb_is_unsecure_trim_bio(ioreq_head->bio))) {
				op.cmd.is_cmd = 1;
				op.cmd.is_trim = 1;
				op.cmd.nr_ents = 1;
				op.cmd.trim_idx = pkt_free_id;
				op.cmd.lba = priv->start;
				pkt_free->ioreq = pcie->dummy_ioreq;
				pkt_free->addr = (dma_addr_t)0;
				pkt_free->next = pkt_free_id;

				pblaze_pcie_del_ioreq(pcie, ioreq_head, dev_index);
				pblaze_raid_complete_io(pcie->raid, ioreq_head);

				pcie->pool.packets_stat[pkt_free_id] = 1;
				pkt_free_id = PKT_NULL;

				pblaze_pcie_stat_sent(&pcie->pool);
			} else {
				if (priv->big_dma_tbl)
					pcie->cur_dma_addr = priv->big_dma_tbl;
				else
					pcie->cur_dma_addr = priv->small_dma_tbl;

				pcie->cur_dma_entries = priv->len;
				op.cmd.is_cmd = 1;
				op.cmd.is_write = pb_is_write_bio(ioreq_head->bio);
				op.cmd.nr_ents = pcie->cur_dma_entries;
				op.cmd.lba = priv->start;
				pcie->packet_chain_first = pkt_free_id;
				pcie->packet_chain_cur = pkt_free_id;

				if (pcie->cur_dma_entries > 1)
					DPRINTK(DEBUG,  "cmd %d lba %d entry_count:%d\n",
						op.cmd.is_write, op.cmd.lba, pcie->cur_dma_entries);

				/* Get pkt_chain_first directly but not get new id next time */
				pcie->dma_process = DMA_PROCESS_ENTRY;
				pkt_free_id = PKT_NULL;
			}
			ioreq_head->issue_jiffies = jiffies;
		} else {
			if (pb_is_secure_trim_bio(ioreq_head->bio)) {
				dma_addr = pcie->secure_trim_dma_addr;
				pkt_free->addr = (dma_addr_t)0;
			} else {
				dma_addr = *pcie->cur_dma_addr;
				pkt_free->addr = dma_addr;
				pkt_free->size = PB_BLOCK_SIZE;
				++pcie->cur_dma_addr;
			}

			/*
			 * Hardware needs sba to be dw aligned.
			 * dma_map_page() doesn't ensure this. We achieved this
			 * by imposing constraints on the incoming requests: if
			 * the bio is 4K aligned, then there's no problem at
			 * all, otherwise, the generic block layer will convert
			 * it to be 8bytes aligned since we had called the
			 * blk_queue_dma_alignment(q,0x7) early in the probing
			 * process.
			 * And the lowest bit to be 0 also indicates
			 * hardware that it's an entry rather than a command.
			 */
			op.entry.sba_low = (u32)dma_addr;
			op.entry.sba_high = (u32)(dma_addr >> 32);
			op.entry.idx = pkt_free_id;

			pcie->pool.packets_stat[pkt_free_id] = 1;

			pkt_free->ioreq = ioreq_head;
			pkt_free->next = pcie->packet_chain_cur;
			pcie->packet_chain_cur = pkt_free_id;

			/* Update proc info */
			pblaze_pcie_stat_sent(&pcie->pool);

			if (--pcie->cur_dma_entries == 0) {
				/* Create the circular linked list */
				pcie->pool.pool[pcie->packet_chain_first].next = pkt_free_id;
				pblaze_pcie_del_ioreq(pcie, ioreq_head, dev_index);
				pcie->dma_process = DMA_PROCESS_INIT;
			} else {
				pcie->dma_process++;
			}

			pkt_free_id = PKT_NULL;
		}


#ifdef __i386__
		writel(*(u32 *)&op, pcie->regs + REG_DMA);
		writel(*((u32 *)&op + 1), pcie->regs + REG_DMA);
#else
		writeq(*(u64 *)&op, pcie->regs + REG_DMA);
#endif

		pblaze_pcie_stat_dma(&pcie->pool, &op);
	}

	if (pkt_free_id != PKT_NULL)
		pblaze_pcie_set_free_pkt(&pcie->pool, pkt_free_id);
}

void pblaze_pcie_issue_ioreq(struct pblaze_pcie *pcie, bool is_work_thread)
{
	unsigned long timeout;

	if (likely(pb_atomic_inc(&pcie->level) == 1)) {
		timeout = jiffies + (is_work_thread ? (HZ / 10) : (HZ / 100));

		do {
			pb_atomic_xchg(&pcie->level, 1);
			__pblaze_pcie_issue_ioreq(pcie, timeout);

			if (likely(time_after_eq(jiffies, timeout))) {
				pb_atomic_xchg(&pcie->level, 0);
				up(&pcie->submit_sem);
				return;
			}
		} while (pb_atomic_dec(&pcie->level) > 0);
	}
}

static int pblaze_pcie_issue_ioreq_slow(void *arg)
{
	u32 remain_sched_period = SCHEDULE_PERIOD;
	struct pblaze_pcie *pcie = (struct pblaze_pcie *)arg;

	DPRINTK(INFO, "++\n");
	while (pcie->is_stop == false) {
		if (down_interruptible(&pcie->submit_sem))
			break;

		/* May not schedule out actually, but it is beneficial to performance */
		if (remain_sched_period-- == 0) {
			remain_sched_period = SCHEDULE_PERIOD;
			schedule();
		}

		pblaze_pcie_issue_ioreq(pcie, true);
	}

	mb();
	pcie->is_stoped = true;

	DPRINTK(INFO, "--\n");
	return 0;
}

bool pblaze_pcie_is_recycle_available(struct pblaze_recycle_desc *recycle_desc)
{
	return (recycle_desc->push_pos - recycle_desc->pop_pos) > PERF_BH_ACCPACKET;
}

u32 pblaze_pcie_pop_recycle_pkt(struct pblaze_recycle_desc *recycle_desc)
{
	u32 pkt_id;
	u32 pop = recycle_desc->pop_pos;
	u32 pop_old;

	do {
		if (pop == recycle_desc->push_pos)
			return PKT_NULL;

		pop_old = pop;
		pkt_id = recycle_desc->rfifo[pop & INTERRUPT_QUEUE_MASK];
	} while ((pop = pb_atomic_cmpxchg(&recycle_desc->pop_pos, pop_old, 1 + pop_old)) != pop_old);

	return pkt_id;
}

void pblaze_pcie_recycle_pkts(struct pblaze_pcie *pcie, struct pblaze_recycle_desc *recycle_desc)
{
	u32 chain_len = 0;
	int pkt_recycle_id;
	u32 pkt_id = PKT_NULL;
	u32 *pkt_next_id = &pkt_id;
	u32 pkt_tmp_id;
	struct pblaze_dma_packet *head;
	struct pblaze_dma_packet *iter;
	struct io_request *ioreq;
	struct device *dev = pcie->raid->pcie_child_dev;

	while (true) {
		pkt_recycle_id = pblaze_pcie_pop_recycle_pkt(recycle_desc);
		if (pkt_recycle_id == PKT_NULL)
			break;
		if (!pcie->pool.packets_stat[pkt_recycle_id]) {
			DPRINTK(ALERT, "Packet %d to be recycled isn't used\n", pkt_recycle_id);

			set_bit(DEV_FAULT, &pcie->raid->flags);
			set_bit(PB_FATAL_WILD_PACKET, &pcie->raid->error_code);
			pblaze_generate_bug_report(pcie->raid);

			if (pcie->interrupt_mode == PB_INTX_MODE)
				pblaze_pcie_disable_intx(pcie);
			/* BUG_ON(1); */
			return;
		}

		DPRINTK(INFO, "pkt_recycle_id %d\n", pkt_recycle_id);
		head = pcie->pool.pool + pkt_recycle_id;
		*pkt_next_id = head->next; /* Connect to the prev chain list */
		pkt_next_id = &head->next; /* The new free tail id */

		/* Loop through the cycle chain */
		ioreq = head->ioreq;
		pkt_tmp_id = pkt_recycle_id;
		do {
			iter = pcie->pool.pool + pkt_tmp_id;
			chain_len++;
			pcie->pool.packets_stat[pkt_tmp_id] = 0;

			/* Normal Read, Write */
			if (likely(iter->addr != (dma_addr_t)0))
				dma_unmap_page(dev, iter->addr, iter->size,
					       (pb_is_write_bio(ioreq->bio) ? DMA_TO_DEVICE : DMA_FROM_DEVICE));
			pkt_tmp_id = iter->next;
		} while (pkt_tmp_id != pkt_recycle_id);

		if (ioreq != pcie->dummy_ioreq)
			pblaze_raid_complete_io(pcie->raid, ioreq);
	}

	if (pkt_id != PKT_NULL &&
	    (pblaze_pcie_set_free_pkt_range(&pcie->pool, pkt_id, pkt_next_id, chain_len)))
		pblaze_pcie_issue_ioreq(pcie, false);
}

static u32 pblaze_pcie_get_intr_stat(struct pblaze_pcie *pcie)
{
	u32 old, new;

	do {
		old = pcie->intr_stat;
		if ((old & INTR_LOCK_MASK) || !old)
			return 0;
		new = INTR_LOCK_MASK;
	} while (pb_atomic_cmpxchg(&pcie->intr_stat, old, new) != old);

	return old;
}

static void pblaze_pcie_tasklet(unsigned long arg)
{
	u32 cur, i;
	struct pblaze_recycle_desc *recycle_desc;
	struct pblaze_tasklet_entry *tasklet_entry = (struct pblaze_tasklet_entry *)arg;
	struct pblaze_pcie *pcie = tasklet_entry->pcie;

	/* During reading init_monitor, the raid is NULL */
	if (pcie->raid) {
		pb_atomic_add(&pcie->raid->flying_pipe_size, pcie->pool.flying_packets);
		pb_atomic_inc(&pcie->raid->flying_pipe_count);
	}

	/*
	 * pcie device handle can't be reentrant,
	 * and interrupt will modify intr_stat
	 * Sometimes the device interrupt disable is fly-by-night.
	 */
	cur = 0;
	do {
		cur = pblaze_pcie_get_intr_stat(pcie);
		if (!cur)
			goto recycle;

		if (pcie->interrupt_mode == PB_INTX_MODE)
			pblaze_pcie_disable_intx(pcie);

		for (i = COMM_DEV_START; i != COMM_DEV_END; ++i) {
			if (cur & (1 << i)) {
				DPRINTK(DEBUG, "interrupt device %d\n", i);
				pblaze_pcd_handle(pcie, i);
			}
		}

		if (pcie->interrupt_mode == PB_INTX_MODE)
			pblaze_pcie_enable_intx(pcie);

		pb_atomic_and(&pcie->intr_stat, (INTR_LOCK_MASK - 1));
	} while (true);

recycle:
	if (pcie->interrupt_mode == PB_INTX_MODE) {
		pblaze_pcie_enable_intx(pcie);
		pcie->interrupt_flag = 0;
	}

	/* recycle can be reentrant */
	recycle_desc = per_cpu_ptr(pcie->pool.recycle_percpu, tasklet_entry->id);
	pblaze_pcie_recycle_pkts(pcie, recycle_desc);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
irqreturn_t pblaze_pcie_interrupt(int irq, void *dev, struct pt_regs *regs)
#else
irqreturn_t pblaze_pcie_interrupt(int irq, void *dev)
#endif
{
	int cnt = 0;
	u64 status, temp;
	u16 pkt_id;
	u32 push_pos;
	struct pblaze_pcie *pcie = (struct pblaze_pcie *)dev;
	struct pblaze_recycle_desc *recycle_desc;
	int cpu;

	DPRINTK(DEBUG, "pcie interrupt ++\n");

#ifdef HARDWARE_FAULTTOLERANT
	if (test_bit(DEV_FAULT, &pcie->raid->flags)) {
		if (pcie->interrupt_mode == PB_INTX_MODE)
			pblaze_pcie_disable_intx(pcie);

		return IRQ_HANDLED;
	}
#endif
	/* Don't worry about preempt in interrupt, So don't need use get_cpu */
	cpu = smp_processor_id();
	recycle_desc = per_cpu_ptr(pcie->pool.recycle_percpu, cpu);
	push_pos = recycle_desc->push_pos;

	while (true) {
#ifdef __i386__
		status = readl(pcie->regs + ISRINFO_ADDR);
		status <<= 32;
		status |= readl(pcie->regs + ISRINFO_ADDR);
#else
		status = readq(pcie->regs + ISRINFO_ADDR);
#endif
		if (status == 0xffffffffffffffffULL) {
			DPRINTK(ALERT, "dma status full f\n");

			set_bit(DEV_FAULT, &pcie->raid->flags);
			set_bit(PB_FATAL_FAULT, &pcie->raid->error_code);
			pblaze_generate_bug_report(pcie->raid);
			if (pcie->interrupt_mode == PB_INTX_MODE)
				pblaze_pcie_disable_intx(pcie);

			return IRQ_HANDLED;
		}

		if (likely((1ull << INT_IDX0_VALID_SHIFT) & status)) {
			pkt_id = (status >> INT_IDX0_SHIFT) & INTERRUPT_QUEUE_MASK;
			pblaze_pcie_stat_recvd(&pcie->pool, pkt_id);
			pblaze_pcie_stat_inc(&pcie->raid->recv_cnt);
			recycle_desc->rfifo[push_pos++ & INTERRUPT_QUEUE_MASK] = pkt_id;
			++cnt;
			DPRINTK(INFO, "idx_0 %u\n", pkt_id);
		}

		if (likely((1ull << INT_IDX1_VALID_SHIFT) & status)) {
			pkt_id = (status>>INT_IDX1_SHIFT) & INTERRUPT_QUEUE_MASK;
			pblaze_pcie_stat_recvd(&pcie->pool, pkt_id);
			pblaze_pcie_stat_inc(&pcie->raid->recv_cnt);
			recycle_desc->rfifo[push_pos++ & INTERRUPT_QUEUE_MASK] = pkt_id;
			++cnt;
			DPRINTK(INFO, "idx_1 %u\n", pkt_id);
		}

		if (likely((1ull << INT_IDX2_VALID_SHIFT) & status)) {
			pkt_id = (status>>INT_IDX2_SHIFT) & INTERRUPT_QUEUE_MASK;
			pblaze_pcie_stat_recvd(&pcie->pool, pkt_id);
			pblaze_pcie_stat_inc(&pcie->raid->recv_cnt);
			recycle_desc->rfifo[push_pos++ & INTERRUPT_QUEUE_MASK] = pkt_id;
			++cnt;
			DPRINTK(INFO, "idx_2 %u\n", pkt_id);
		}

		if (likely((1ull << INT_IDX3_VALID_SHIFT) & status)) {
			pkt_id = (status>>INT_IDX3_SHIFT) & INTERRUPT_QUEUE_MASK;
			pblaze_pcie_stat_recvd(&pcie->pool, pkt_id);
			pblaze_pcie_stat_inc(&pcie->raid->recv_cnt);
			recycle_desc->rfifo[push_pos++ & INTERRUPT_QUEUE_MASK] = pkt_id;
			++cnt;
			DPRINTK(INFO, "idx_3 %u\n", pkt_id);
		}

		if (cnt > INTERRUPT_QUEUE_SIZE) {
			DPRINTK(ERR, "Too many pkt count over queue\n");

			set_bit(DEV_FAULT, &pcie->raid->flags);
			set_bit(PB_FATAL_TOO_MANY_PACKETS, &pcie->raid->error_code);
			pblaze_generate_bug_report(pcie->raid);
			if (pcie->interrupt_mode == PB_INTX_MODE)
				pblaze_pcie_disable_intx(pcie);

			return IRQ_HANDLED;
		}

		if (status & (1ull << IDX_FIFO_EMPTY)) {
			DPRINTK(DEBUG, "FIFO_EMPTY, nr_idx recvd %d\n", cnt);
#ifdef PBLAZE_COUNTER
			/* Note: the acct info is not quite right under msix */
			pcie->cur_cnt = cnt;
			if (cnt > pcie->max_cnt)
				pcie->max_cnt = cnt;
#endif
			recycle_desc->push_pos = push_pos;
			temp = (status >> INT_EVENT_SHIFT) & INT_EVENT_MASK;
			if (unlikely(temp)) {
				if (pcie->interrupt_mode == PB_INTX_MODE)
					temp &= pcie->intr_mask;
				pb_atomic_or(&pcie->intr_stat, temp);
				if (pcie->interrupt_mode == PB_INTX_MODE) {
					pcie->interrupt_flag = PBLAZE_INT_MUST_DISABLE;
					pblaze_pcie_disable_intx(pcie);
				}
			} else if (cnt == 0) {
				DPRINTK(INFO, "other share device interrupt\n");
				break;
			}

			tasklet_schedule(&pcie->tasklet_entries[cpu].pcie_tasklet);
			return IRQ_HANDLED;
		}
	}

	if (pcie->interrupt_mode == PB_MSIX_MODE) {
		return IRQ_HANDLED;
	} else {
		if (status & (1ull << INT_EXIST)) {
			return IRQ_HANDLED;
		} else {
			DPRINTK(INFO, "IRQ_NONE return\n");
			pb_atomic_inc(&pcie->empty_cnt);
			if (pcie->empty_cnt > 50) {
				pcie->empty_cnt = 0;
				return IRQ_HANDLED;
			} else {
				return IRQ_NONE;
			}
		}
	}
}

static char *pblaze_pcie_get_sn(struct pblaze_init_monitor *init_monitor)
{
	char *sn_str;

	sn_str = (char *)pb_get_str_by_key(init_monitor->rom, "Serial Number:");
	DPRINTK(NOTICE, "init_monitor: serial number:%s\n", sn_str);

	return sn_str;
}

static u32 pblaze_pcie_get_id(struct pblaze_init_monitor *init_monitor)
{
	u32 pcie_id = 0;
	const char *id_str;

	id_str = pb_get_str_by_key(init_monitor->rom, "DeviceIndex:");
	pcie_id = simple_strtoul(id_str, NULL, 10);
	BUG_ON(pcie_id >= NR_DEVICE);
	DPRINTK(NOTICE, "init_monitor: device index:%d\n", pcie_id);

	return pcie_id;
}

static u32 pblaze_pcie_get_num(struct pblaze_init_monitor *init_monitor)
{
	const char *model_str;

	model_str = pb_get_str_by_key(init_monitor->rom, "Model:");
	if (model_str[3] == 'L') {
		DPRINTK(NOTICE, "Pblaze3 L card\n");
		return 1;
	} else if (model_str[3] == 'H') {
		DPRINTK(NOTICE, "Pblaze3 H card\n");
		return 2;
	} else {
		WARN_ON(1);
		return 0;
	}
}

int pblaze_pcie_recv_init_monitor(struct pblaze_pcie *pcie)
{
	return pblaze_comm_imm(CMD_READROM, sizeof(pcie->init_monitor.rom),
			(u16 *)pcie->init_monitor.rom, false, &pcie->pcd_slave);
}

static int pblaze_pcie_config_interrupt(struct pci_dev *dev, struct pblaze_pcie *pcie, int cpu_num)
{
	int ret;
	int i;
	u32 value;
	int msix_num;
	struct msix_entry *msix;

	msix_num = min(cpu_num, MSIX_TABLE_SIZE);

	if (pci_find_capability(dev, PCI_CAP_ID_MSIX) && pcie->interrupt_mode != PB_MSIX_UNSUPPORT_MODE) {
		DPRINTK(NOTICE, "Device has msix capability\n");

		/* Set msix table count to cpu_num */
		value = readl(pcie->regs + ISRMASK_ADDR);
		value &= 0xffff00ff;
		value |= msix_num << MSIX_TABLE_SHIFT;
		writel(value, pcie->regs + ISRMASK_ADDR);

		for (i = 0; i < msix_num; i++) {
			msix = &pcie->msix_entries[i];
			msix->entry = i;
		}

		value = readl(pcie->regs + ISRMASK_ADDR);
		if (value & (1 << MSIX_TABLE_LOCK_SHIFT)) {
			DPRINTK(ERR, "The msix table is locked, now unlock it\n");
			value &= (~(1 << MSIX_TABLE_LOCK_SHIFT));
			writel(value, pcie->regs + ISRMASK_ADDR);
		}

		ret = pci_enable_msix(dev, pcie->msix_entries, msix_num);
		if (ret) {
			DPRINTK(ERR, "pci enable msix failed\n");
			goto enable_intx_mode;
		}

		/* Lock the msix table to avoid os change it */
		if (pblaze_msi_tbl_lock == 1) {
			value = readl(pcie->regs + ISRMASK_ADDR);
			value |= (1 << MSIX_TABLE_LOCK_SHIFT);
			writel(value, pcie->regs + ISRMASK_ADDR);
		}

		for (i = 0; i < msix_num; i++) {
			msix = &pcie->msix_entries[i];
			printk("pblaze msix sector:%u\n", msix->vector);

			ret = request_irq(msix->vector, pblaze_pcie_interrupt, 0, PCIE_DRV_NAME, (void *)pcie);
			if (ret) {
				DPRINTK(ERR, "request irq failed\n");
				goto failed_request_irq;
			}
		}

		pcie->interrupt_mode = PB_MSIX_MODE;

		return 0;
	}

enable_intx_mode:
	printk("Device has intx capability, irq = %d\n", dev->irq);

	ret = request_irq(dev->irq, pblaze_pcie_interrupt, IRQF_SHARED, PCIE_DRV_NAME, (void *)pcie);
	if (ret) {
		free_irq(dev->irq, (void *)pcie);
		pcie->interrupt_mode = PB_UNKNOWN_MODE;
		return ret;
	}

	pcie->interrupt_mode = PB_INTX_MODE;
	return ret;

failed_request_irq:
	for (i = 0; i < msix_num; i++) {
		msix = &pcie->msix_entries[i];
		free_irq(msix->vector, (void *)pcie);
	}
	pci_disable_msix(dev);
	pcie->interrupt_mode = PB_UNKNOWN_MODE;

	return ret;
}

static void pblaze_pcie_free_interrupt(struct pci_dev *dev, struct pblaze_pcie *pcie, int cpu_num)
{
	int i;
	int msix_num;
	u32 value;
	struct msix_entry *msix;

	msix_num = min(cpu_num, MSIX_TABLE_SIZE);

	if (pcie->interrupt_mode == PB_MSIX_MODE) {
		for (i = 0; i < msix_num; i++) {
			msix = &pcie->msix_entries[i];
			free_irq(msix->vector, (void *)pcie);
		}
		pci_disable_msix(dev);

		/* Unlock the msix table */
		if (pblaze_msi_tbl_lock == 1) {
			value = readl(pcie->regs + ISRMASK_ADDR);
			value &= (~(1 << MSIX_TABLE_LOCK_SHIFT));
			writel(value, pcie->regs + ISRMASK_ADDR);
		}
	} else if (pcie->interrupt_mode == PB_INTX_MODE) {
		free_irq(dev->irq, (void *)pcie);
	}
}

static inline void pblaze_set_interrupt_mode(struct pblaze_pcie *pcie)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 32)
	pcie->interrupt_mode = PB_MSIX_UNSUPPORT_MODE;
#else
	if (pblaze_msi_disable == 1)
		pcie->interrupt_mode = PB_MSIX_UNSUPPORT_MODE;
#endif
}

static inline void pblaze_set_flow_control_compatible(int pblaze_int_interval,
						      struct pblaze_pcie *pcie)
{
	writel(pblaze_int_interval, pcie->regs + ISRDELAY_ADDR);
	mdelay(1);

	/* set flow contorl value for new release bit. */
	writel(pblaze_int_interval & 0x3fff, pcie->regs + FLOW_CONTROL_DATA);
	mdelay(1);
	writel(COEFF_C_ADDR, pcie->regs + FLOW_CONTROL_ADDR);
	mdelay(1);

	writel(1, pcie->regs + FLOW_CONTROL_DATA);
	mdelay(1);
	writel(WRITE_FC_EN_ADDR, pcie->regs + FLOW_CONTROL_ADDR);
}

static void	pblaze_pcie_get_link_status(struct pci_dev *dev, 
										struct pblaze_pcie *pcie)
{
	int cap = 0;
	u16 value;

	pcie_set_readrq(dev, 512);
	cap = pci_find_capability(dev, PCI_CAP_ID_EXP);
	if (cap) {
		pci_read_config_word(dev, cap + PCI_EXP_LNKSTA, &value);
		pcie->link_width = (value & PCI_EXP_LNKSTA_NLW) >> 4;
		if (pcie->link_width < 8)
			printk("Warnning: the pcie slot is not x8\n");

		if (value & PCI_EXP_LNKSTA_CLS_2_5GB) {
			pcie->link_gen = PB_PCI_LINK_GEN_1;
		} else if (value & PCI_EXP_LNKSTA_CLS_5_0GB) {
			pcie->link_gen = PB_PCI_LINK_GEN_2;
		} else {
			pcie->link_gen = PB_PCI_LINK_GEN_3;
		}
	}
}

static int __devinit pblaze_pcie_probe(struct pci_dev *dev,
				       const struct pci_device_id *id)
{
	int ret = 0;
	int cpu_num;
	u32 index;
	char *dev_sn;
	u32 dev_id, dev_num;
	unsigned long iobase, iolen;
	dma_addr_t addr;
	struct pblaze_pcie *pcie;
	struct pblaze_tasklet_entry *entry;

	DPRINTK(DEBUG, "pcie probe\n");

	cpu_num = num_online_cpus();
	pb_atomic_add(pblaze_get_driver_size(), sizeof(struct pblaze_pcie) + cpu_num * sizeof(struct pblaze_tasklet_entry));
	pcie = vmalloc(sizeof(struct pblaze_pcie) + cpu_num * sizeof(struct pblaze_tasklet_entry));
	if (!pcie) {
		ret = -ENOMEM;
		goto failed_alloc_pcie;
	}
	memset(pcie, 0, sizeof(struct pblaze_pcie));

	pcie->tasklet_num = cpu_num;
	kref_init(&pcie->refcn);
	pci_set_drvdata(dev, pcie);
	pcie->dev = dev;
	pcie->dma_process = DMA_PROCESS_INIT;

	ret = pblaze_pcie_init_dma_pool(&pcie->pool);
	if (ret)
		goto failed_init_dma_pool;

	for (index = 0; index < cpu_num; index++) {
		entry = &pcie->tasklet_entries[index];
		entry->id = index;
		entry->pcie = pcie;
		tasklet_init(&entry->pcie_tasklet, pblaze_pcie_tasklet, (unsigned long)entry);
	}

	ret = pci_enable_device(dev);
	if (ret)
		goto failed_enable_dev;

	iobase = pci_resource_start(dev, 0);
	iolen = pci_resource_len(dev, 0);
	if (!request_mem_region(iobase, iolen, "regs")) {
		ret = -EIO;
		goto failed_request_region;
	}

	pcie->regs = ioremap_nocache(iobase, iolen);
	if (!pcie->regs) {
		ret = -ENOMEM;
		goto failed_ioremap;
	}

	pblaze_set_flow_control_compatible(pblaze_int_interval, pcie);

	pcie->pcd[0].read_reg = pcie->regs + REG_READ_MB0;
	pcie->pcd[0].write_reg = pcie->regs + REG_WRITE_MB0;
	pcie->pcd[0].owner = pcie;

	pcie->pcd[1].read_reg = pcie->regs + REG_READ_MB1;
	pcie->pcd[1].write_reg = pcie->regs + REG_WRITE_MB1;
	pcie->pcd[1].owner = pcie;

	for (index = COMM_DEV_START; index != COMM_DEV_END; ++index) {
		spin_lock_init(&pcie->pcd[index].cmd_list_lock);
		INIT_LIST_HEAD(&pcie->pcd[index].cmd_list);
		pcie->pcd[index].status = ST_STAGE_INIT;
	}

	INIT_LIST_HEAD(&pcie->ioreq_list);
	spin_lock_init(&pcie->ioreq_list_lock);

	if (pci_set_dma_mask(dev, DMA_BIT_MASK(64)) ||
	    pci_set_consistent_dma_mask(dev, DMA_BIT_MASK(64))) {
		DPRINTK(ERR, "PCIe Disk: dma_set_mask Fails\n");
		ret = -EFAULT;
		goto failed_set_dma_mask;
	}

	pblaze_pcie_get_link_status(dev, pcie);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
	pci_enable_pcie_error_reporting(dev);
#endif

	pci_set_master(dev);

	pcie->secure_trim_addr = (void *)get_zeroed_page(GFP_KERNEL);
	if (!pcie->secure_trim_addr) {
		ret = -ENOMEM;
		goto failed_init_trim;
	}

	addr = dma_map_single(&dev->dev, pcie->secure_trim_addr,
			      PB_BLOCK_SIZE, DMA_TO_DEVICE);
	pcie->secure_trim_dma_addr = addr;

	/* Receive init_monitor from pice device */
	strncpy(pcie->init_monitor.driver_version, driver_version,
		min(strlen(driver_version) + 1u,
		sizeof(pcie->init_monitor.driver_version)));
	ret = pblaze_pcie_recv_init_monitor(pcie);
	if (ret == -1) {
		DPRINTK(ERR, "Pcie probe error: Read HW Rom failed!\n");
		goto failed_read_register;
	}

	dev_sn = pblaze_pcie_get_sn(&pcie->init_monitor);
	dev_id = pblaze_pcie_get_id(&pcie->init_monitor);
	pcie->sub_dev_index = dev_id;
	dev_num = pblaze_pcie_get_num(&pcie->init_monitor);
	ret = pblaze_raid_probe(pcie, dev_sn, dev_id, dev_num);
	if (ret)
		goto failed_raid_probe;

	pblaze_set_interrupt_mode(pcie);
	ret = pblaze_pcie_config_interrupt(dev, pcie, cpu_num);
	if (ret)
		goto failed_request_irq;

	init_MUTEX_LOCKED(&pcie->submit_sem);
	pcie->submit_thread =
		kthread_run(pblaze_pcie_issue_ioreq_slow, pcie, "pb_sm");
	if (IS_ERR(pcie->submit_thread)) {
		DPRINTK(INFO, "pcie submit thread run failed\n");
		ret = PTR_ERR(pcie->submit_thread);
		goto failed_create_kthread;
	}

	if (pcie->interrupt_mode == PB_INTX_MODE)
		pblaze_pcie_enable_intx(pcie);

	/*
	 * Simulate a device interrupt for all cpus,
	 * To avoid losing the first interrupt when interrupt enable
	 */
	pcie->intr_stat |= INT_EVENT_MASK;
	tasklet_schedule(&pcie->tasklet_entries[0].pcie_tasklet);

	DPRINTK(DEBUG, "pcie probe success\n");

	return 0;

failed_create_kthread:
	pcie->submit_thread = NULL;
failed_request_irq:
	pblaze_raid_remove(pcie);
	pblaze_raid_free(pcie);
failed_read_register:
failed_raid_probe:
	dma_unmap_single(&dev->dev, pcie->secure_trim_dma_addr,
			 PB_BLOCK_SIZE, DMA_TO_DEVICE);
	free_page((unsigned long)pcie->secure_trim_addr);
failed_init_trim:
failed_set_dma_mask:
	iounmap(pcie->regs);
failed_ioremap:
	release_mem_region(iobase, iolen);
failed_request_region:
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 28)
	pci_clear_master(dev);
#endif
	pci_disable_device(dev);
failed_enable_dev:
	free_percpu(pcie->pool.recycle_percpu);
failed_init_dma_pool:
	kref_put(&pcie->refcn, release_pblaze_pcie); /* for pcie_alloc */
failed_alloc_pcie:
	pb_atomic_sub(pblaze_get_driver_size(), sizeof(struct pblaze_pcie));
	return ret;
}

static void __devexit pblaze_pcie_remove(struct pci_dev *dev)
{
	int i;
	int cpu_num;
	struct pblaze_pcie *pcie = (struct pblaze_pcie *)pci_get_drvdata(dev);

	DPRINTK(DEBUG, "pcie remove\n");

	/* Reinit need interrupt enable */
	pblaze_raid_remove(pcie);

	if (pcie->interrupt_mode == PB_INTX_MODE)
		pblaze_pcie_disable_intx(pcie);

	cpu_num = pcie->tasklet_num;
	for (i = 0; i < cpu_num; i++)
		tasklet_kill(&pcie->tasklet_entries[i].pcie_tasklet);

	pcie->is_stop = true;
	up(&pcie->submit_sem);

	/* wait until submit thread exit */
	mb();
	while (pcie->is_stoped == false) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(HZ / 100);
	}

	pblaze_pcie_free_interrupt(dev, pcie, cpu_num);
	dma_unmap_single(&dev->dev, pcie->secure_trim_dma_addr,
			 PB_BLOCK_SIZE, DMA_TO_DEVICE);
	free_page((unsigned long)pcie->secure_trim_addr);

#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 28)
	pci_clear_master(dev);
#endif
	pci_disable_device(dev);

	free_percpu(pcie->pool.recycle_percpu);
	iounmap(pcie->regs);
	release_mem_region(pci_resource_start(dev, 0),
			   pci_resource_len(dev, 0));

	pblaze_raid_free(pcie);
	kref_put(&pcie->refcn, release_pblaze_pcie); /* for pcie_alloc */
	pb_atomic_sub(pblaze_get_driver_size(), sizeof(struct pblaze_pcie) + cpu_num * sizeof(struct pblaze_tasklet_entry));
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 16)
static void pblaze_pcie_shutdown(struct pci_dev *dev)
#else
static void pblaze_pcie_shutdown(struct device *device)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 16)
	struct pci_dev *dev;
	dev = container_of(device, struct pci_dev, dev);
#endif
	struct pblaze_pcie *pcie = (struct pblaze_pcie *)pci_get_drvdata(dev);
	struct pblaze_raid *raid = pcie->raid;

	DPRINTK(WARNING, "++\n");

	pblaze_dre_exit(raid);
	set_bit(DEV_SHUTDOWN, &raid->flags);
	udelay(1000);	/* To avoid outstanding_io update delay */
	mb();
	while (raid->outstanding_io != 0) {
		DPRINTK(WARNING, "Wait for all outstanding_io:%u done\n",
			raid->outstanding_io);
		udelay(100);
	}

	pcie->is_stop = true;
	up(&pcie->submit_sem);

	/* wait until submit thread exit */
	mb();
	while (pcie->is_stoped == false) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(HZ / 100);
	}

	if (raid->outstanding_io != 0) {
		DPRINTK(ERR, "The outstanding io is still not clear\n");
		WARN_ON(1);
	}

	/* Restart device to init status only one time */
	if (raid->nr_dev == raid->nr_dev_max) {
		pblaze_set_unload(raid);
		pblaze_raid_reinit(raid);
		raid->nr_dev--;
	}

	DPRINTK(WARNING, "--\n");
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 16)
pci_ers_result_t pblaze_pcie_error_detected(struct pci_dev *dev,
					    enum pci_channel_state error)
{
	return PCI_ERS_RESULT_NEED_RESET;
}

pci_ers_result_t pblaze_pcie_mmio_enabled(struct pci_dev *dev)
{
	return PCI_ERS_RESULT_NEED_RESET;
}

pci_ers_result_t pblaze_pcie_link_reset(struct pci_dev *dev)
{
	return PCI_ERS_RESULT_NEED_RESET;
}

pci_ers_result_t pblaze_pcie_slot_reset(struct pci_dev *dev)
{
	return PCI_ERS_RESULT_NEED_RESET;
}

void pblaze_pcie_resume(struct pci_dev *dev)
{
}

void pblaze_trigger_pcie_interrupt(struct pblaze_pcie *pcie)
{
	if (pcie->interrupt_flag == PBLAZE_INT_MUST_DISABLE) {
		return;
	}

	if (pcie->interrupt_mode == PB_INTX_MODE) {
		pblaze_pcie_disable_intx(pcie);
		pblaze_pcie_enable_intx(pcie);
	}
}

static struct pci_error_handlers pblaze_pcie_err_handler = {
	.error_detected = pblaze_pcie_error_detected,
	.mmio_enabled = pblaze_pcie_mmio_enabled,
	.link_reset = pblaze_pcie_link_reset,
	.slot_reset = pblaze_pcie_slot_reset,
	.resume = pblaze_pcie_resume,
};
#endif

struct pci_driver pblaze_pcie_driver = {
	.name = PCIE_DRV_NAME,
	.id_table = pblaze_pcie_tbl,
	.probe = pblaze_pcie_probe,
	.remove = __devexit_p(pblaze_pcie_remove),
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 16)
	.shutdown = pblaze_pcie_shutdown,
	.err_handler = &pblaze_pcie_err_handler,
#else
	.driver.shutdown = pblaze_pcie_shutdown,
#endif
};
