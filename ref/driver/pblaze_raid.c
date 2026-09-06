#include <linux/vmalloc.h>

#include "pblaze_pcie.h"
#include "pblaze_raid.h"

/* The global raid device list in system */
struct list_head raid_list;
spinlock_t raid_list_lock;

struct pblaze_raid_driver raid_drv;

extern int pblaze_virt_dev_register(struct pblaze_raid *raid, int minor);
extern void pblaze_virt_dev_unregister(struct pblaze_raid *raid);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
void pblaze_raid_collect_bug_report(void *data)
#else
void pblaze_raid_collect_bug_report(struct work_struct *work)
#endif
{
	int ret = 0;
	char *envp[] = {PATH_STR, NULL};
	char *argv[] = {"/bin/bash", BUG_REPORT_TOOL, NULL};
	unsigned long wait_flag = UMH_WAIT_EXEC;

	ret = call_usermodehelper(argv[0], argv, envp, wait_flag);
}

static u32 pblaze_get_mapping_cycle(u16 *array, u32 nr_dev)
{
	u32 i;
	u32 gcd, cycle = 0;

	gcd = array[0];
	for (i = 1; i < nr_dev; i++)
		gcd = pb_gcd(gcd, array[i]);

	for (i = 0; i < nr_dev; i++)
		cycle += array[i] / gcd;

	return cycle;
}

/*
 * Getting the mapping sub addr and sub device for logical addr
 * @addr: the real logical address
 * @index: recursion level
 * @array: the tetris number array
 * @sub_addr: the logical address of sub device
 * @sub_dev: the index of sub device
 *
 * FIXME: change to unrecursion
 */
static void pblaze_calc_mapping(u32 addr, u32 index, u16 *array,
				s32 *sub_addr, s32 *sub_dev)
{
	u32 i, j;
	u16 t0, t1 = 0;

	/* Only one pcie device */
	if (NR_DEVICE == 1) {
		*sub_addr = addr;
		*sub_dev = 0;
		return;
	}

	t0 = array[index];
	for (i =  index + 1; i < NR_DEVICE; i++)
		t1 += array[i];

	/* Reach to the last two pcie devices */
	if (index == NR_DEVICE - 2) {
		if (((addr * t0) % (t0 + t1)) < t0) {
			*sub_addr = (addr * t0) / (t0 + t1);
			*sub_dev = index;
		} else {
			*sub_addr = (addr * t1) / (t0 + t1);
			*sub_dev = index + 1;
		}

		return;
	} else {
		if (((addr * t0) % (t0 + t1)) < t0) {
			*sub_addr = (addr * t0) / (t0 + t1);
			*sub_dev = index;

			return;
		} else {
			j = (addr * t1) / (t0 + t1);
			pblaze_calc_mapping(j, index + 1, array,
					    sub_addr, sub_dev);
		}
	}
}

/*
 * Complete the mapping address tabble, if the slot is -1, search prev nonzero
 * slot and replace it.
 * @addr_tbl: the pcie device's address mapping table
 * @cycle: the mapping cycle
 */
static void pblaze_scan_mapping(s32 *addr_tbl, u32 cycle)
{
	s32 i = 0;
	s32 temp = -1;

	/*
	 * Walk through the table, replace -1 addr with prev nonzero addr.
	 * Think about this case: -1,-1,0,1,-1,2,-1
	 * After scan things like this: 0,0,0,1,2,2,3
	 */
	while (i < cycle) {
		if (addr_tbl[i] == -1)
			addr_tbl[i] = temp + 1;
		else
			temp = addr_tbl[i];

		i++;
	}
}

/* Not Used */
void pblaze_print_mapping_table(struct pblaze_raid *raid)
{
	int i, j;
	u32 cycle = raid->cycle;

	printk("------------------------------------------------------\n");
	printk("raid device table:");
	for (i = 0; i < cycle; i++) {
		printk("%d\t", raid->dev_tbl[i]);
	}
	printk("\n");

	for (i = 0; i < raid->nr_dev_max; i++) {
		printk("Table: pcie:%u address table:", i);
		for (j = 0; j < cycle; j++) {
			printk("%d\t", raid->pcie_devs[i]->addr_tbl[j]);
		}
		printk("\n");
		printk("Table: pcie:%u quota:%u\n", i, raid->pcie_devs[i]->quota);
	}
	printk("------------------------------------------------------\n");
}

static int pblaze_get_mapping_table(struct pblaze_raid *raid)
{
	u32 i;
	u16 total;
	u32 cycle, size;
	s32 *pos;
	s32 sub_addr, sub_dev;

	cycle = pblaze_get_mapping_cycle(raid->tetris_num, raid->nr_dev_max);
	raid->cycle = cycle;

	vfree(raid->dev_tbl);

	/* one for dev_tbl, the others for pcie device's addr_tbl */
	size = cycle * (1 + raid->nr_dev_max) * sizeof(s32);

	do {
		raid->dev_tbl = vmalloc(size);
	} while (!raid->dev_tbl);

	memset(raid->dev_tbl, -1, size);
	pos = raid->dev_tbl + cycle;
	for (i = 0; i < raid->nr_dev_max; i++) {
		raid->pcie_devs[i]->addr_tbl = pos;
		pos += cycle;
	}

	for (i = 0; i < cycle; i++) {
		pblaze_calc_mapping(i, 0, raid->tetris_num, &sub_addr, &sub_dev);
		raid->dev_tbl[i] = sub_dev;
		raid->pcie_devs[sub_dev]->addr_tbl[i] = sub_addr;
	}

	total = raid->tetris_total_num;
	for (i = 0; i < raid->nr_dev_max; i++) {
		pblaze_scan_mapping(raid->pcie_devs[i]->addr_tbl, cycle);

		/* calc every pcie quota in a cycle */
		raid->pcie_devs[i]->quota = cycle * raid->tetris_num[i] / total;
	}

	return 0;
}

/* Specailly if one pcie, always return addr */
static inline s32 pblaze_get_sub_addr(u64 addr, u32 cycle, u32 quota, s32 *tbl)
{
	/* index = addr % cycle, addr = addr / cycle*/
	u32 index = do_div(addr, cycle);

	return addr * quota + tbl[index];
}

void __pblaze_raid_complete_io(struct pblaze_raid *raid, struct io_request *ioreq)
{
	int is_write;
	u32 latency;
	u32 old, new;
	u32 id;
	struct pblaze_latency_monitor *latency_monitor;

	is_write = pb_is_write_bio(ioreq->bio);

	if (ioreq->error == 0 && ioreq->is_dre == false) {
		/* Record the last pcie io error code in ioreq */
		if (pb_is_trim_bio(ioreq->bio)) {
			pb_atomic_inc(&raid->write_io);
			pb_atomic_inc(&raid->write_size);
		} else if (pb_is_has_data_bio(ioreq->bio)) {
			if (is_write) {
				pb_atomic_inc(&raid->write_io);
				pb_atomic_add(&raid->write_size, ioreq->ioreq_len);
			} else {
				pb_atomic_inc(&raid->read_io);
				pb_atomic_add(&raid->read_size, ioreq->ioreq_len);
			}
		}

		/* Update raid device statistics such as latency monitor */
		latency = pb_get_tsc_tick() - ioreq->submit_tick;
		latency_monitor = raid->latency_monitor + is_write;
		old = raid->latency_jiffies;
		new = jiffies;

		if (((new - old) > HZ * 2) && (pb_atomic_cmpxchg(&raid->latency_jiffies, old, new) == old)) {
			memset(raid->latency_monitor, 0,
			       sizeof(struct pblaze_latency_monitor) * LATENCY_COUNTER);
			pb_atomic_xchg(&raid->flying_pipe_size, 0);
			pb_atomic_xchg(&raid->flying_pipe_count, 0);
		}

		pb_atomic_inc(&latency_monitor->count);
		pb_atomic_add(&latency_monitor->total_latency, latency);
		pb_atomic_max(&latency_monitor->max_latency, latency);
		pb_atomic_min(&latency_monitor->min_latency, latency);

		/* TODO: make sure __builtin_clz works well */
		id = latency ? (LATENCY_BUCKET - 1 - __builtin_clz(latency)) : 0;
		pb_atomic_inc(&latency_monitor->bucket_count[id]);
	}

	pb_atomic_dec(&raid->outstanding_io);

	pblaze_pcie_stat_inc(&raid->cmpl_cnt);
	pb_ioreq_complete_io(ioreq, ioreq->error);
}

void pblaze_raid_complete_io(struct pblaze_raid *raid, struct io_request *ioreq)
{
	if (pb_atomic_dec(&ioreq->ioreq_ref) == 0)
		__pblaze_raid_complete_io(raid, ioreq);
}

int pblaze_raid_prepare_ioreq(struct pblaze_raid *raid, struct io_request *ioreq)
{
	int i, j;
	bool is_write = 0;
	u64 start, len, end, cur, temp;
	u32 cycle, quota, start_in_cycle;
	u32 sub_dev;
	s32 *dev_tbl = raid->dev_tbl;
	s32 *addr_tbl;
	struct device *dev;
	struct bio_vec *bvec;
	struct io_request_priv *privs = ioreq->privs;
	struct io_request_priv *priv;
	dma_addr_t dma_addr;
	dma_addr_t *priv_dma[NR_DEVICE] = {0};

	if (!raid->is_tbl_ready) {
		DPRINTK(ERR, "mapping table is not ready\n");
		pb_ioreq_complete_io(ioreq, -EINVAL);
		return -1;
	}

	start = ioreq->ioreq_start;
	len = ioreq->ioreq_len;
	end = start + len;
	cycle = raid->cycle;
	temp = start;
	start_in_cycle = do_div(temp, cycle);

	BUG_ON((pb_get_ioreq_start(ioreq) & PB_B2S_MASK) != 0);
	BUG_ON((pb_get_ioreq_len(ioreq) & PB_B2S_MASK) != 0);

	if (pb_is_has_data_bio(ioreq->bio)) {
		if ((len == 0) || (MAX_DMA_LENGTH < len) ||
		    (start + len) > (raid->disk_size_MB << PB_B2MB_SHIFT)) {
			DPRINTK(ERR, "Illegal data io, S: %llu, L:%llu, T: %u\n",
				start, len, raid->disk_size_MB);
			pb_ioreq_complete_io(ioreq, -EINVAL);
			set_bit(PB_WARNING_EXCEED_RANGE, &raid->error_code);
			return -1;
		}
	} else if (pb_is_trim_bio(ioreq->bio)) {
		if (len == 0) {
			DPRINTK(DEBUG, "Trival trim io, seemd here will never hit\n");
			pb_ioreq_complete_io(ioreq, 0);
			return -1;
		}

		if ((MAX_DMA_LENGTH < len) ||
		    (start + len) > (raid->disk_size_MB << PB_B2MB_SHIFT)) {
			DPRINTK(ERR, "Illegal trim io, S: %llu, L:%llu, T: %u\n",
				start, len, raid->disk_size_MB);
			pb_ioreq_complete_io(ioreq, -EINVAL);
			set_bit(PB_WARNING_EXCEED_RANGE, &raid->error_code);
			return -1;
		}
	} else {
		DPRINTK(DEBUG, "Empty barrier io, seemd here will never hit\n");
		pb_ioreq_complete_io(ioreq, 0);
		return -1;
	}

#ifdef RDONLY_CHECK
	if (test_bit(DEV_RDONLY, &raid->flags) &&
	    pb_is_write_bio(ioreq->bio)) {
		DPRINTK(ERR, "Hardware read only in ioreq dispatch\n");
		pb_ioreq_complete_io(ioreq, -EIO);
		return -1;
	}
#endif

#ifdef HARDWARE_FAULTTOLERANT
	if (test_bit(DEV_FAULT, &raid->flags)) {
		DPRINTK(ERR, "Hardware fault in ioreq dispatch\n");
		pb_ioreq_complete_io(ioreq, -EINVAL);
		return -1;
	}

	raid->last_insert_jiffies = jiffies;
#endif

	if (test_bit(DEV_SHUTDOWN, &raid->flags)) {
		DPRINTK(ERR, "Device is already shutdown\n");
		pb_ioreq_complete_io(ioreq, -EIO);
		return -1;
	} else {
		mb();
		pb_atomic_inc(&raid->outstanding_io);
	}

	if (pb_is_trim_bio(ioreq->bio)) {
		sub_dev = dev_tbl[start_in_cycle];
		priv = privs + sub_dev;
		quota = raid->pcie_devs[sub_dev]->quota;
		addr_tbl = raid->pcie_devs[sub_dev]->addr_tbl;

		priv->start = pblaze_get_sub_addr(start, cycle, quota, addr_tbl);
		priv->len = 1;
		(void)pb_atomic_inc(&ioreq->ioreq_ref);

		/* If trim ignore the conflict dre write */
		is_write = 1;
	} else if (pb_is_has_data_bio(ioreq->bio)) {
		dev = raid->pcie_child_dev;
		bvec = pb_bio_iovec(ioreq->bio);
		is_write = pb_is_write_bio(ioreq->bio);

		/* Do the first dma_map_page */
		dma_addr = dma_map_page(dev, bvec->bv_page,
					bvec->bv_offset, bvec->bv_len,
					(is_write) ? DMA_TO_DEVICE : DMA_FROM_DEVICE);

		/*
		 * Get sub pcie device start address and io request length.
		 * At the same time, alloc dma map address space.
		 */
		cur = start;
		i = start_in_cycle;
		while (cur != min(start + cycle, end)) {
			sub_dev = dev_tbl[i];
			priv = privs + sub_dev;
			if (priv->len == 0) {
				quota = raid->pcie_devs[sub_dev]->quota;
				addr_tbl = raid->pcie_devs[sub_dev]->addr_tbl;
				priv->start = pblaze_get_sub_addr(cur, cycle, quota, addr_tbl);
				BUG_ON(priv->start == -1);
				priv->len = pblaze_get_sub_addr(end, cycle, quota, addr_tbl) - priv->start;
				BUG_ON(priv->len == 0);
				(void)pb_atomic_inc(&ioreq->ioreq_ref);

				/* If non-align, need record the bottom half addr */
				j = priv->len;
				if (j > PB_S_DMA_CNT) {
					priv->big_dma_tbl = kmalloc(sizeof(dma_addr_t) * j, GFP_ATOMIC);
					if (!priv->big_dma_tbl) {
						DPRINTK(ERR, "pb alloc dma addr error\n");
						dma_unmap_page(dev, bvec->bv_offset, bvec->bv_len,
							       (is_write) ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
						pb_atomic_dec(&raid->outstanding_io);
						pb_ioreq_complete_io(ioreq, -ENOMEM);
						return -1;
					}

					priv_dma[sub_dev] = priv->big_dma_tbl;
				} else {
					/* Use inline space for small io */
					priv_dma[sub_dev] = priv->small_dma_tbl;
				}
			}

			cur++;
			(i == cycle - 1) ? (i = 0) : i++;
		}

		cur = start;
		i = start_in_cycle;
		while (cur != end) {
			sub_dev = dev_tbl[i];
			priv = privs + sub_dev;

			/* Update dma map addr */
			*(priv_dma[sub_dev]) = dma_addr;
			WARN_ON(pb_get_dma_align(dma_addr));
			priv_dma[sub_dev]++;

			/* Bio_vec reach the end when hit it */
			if (cur == end - 1)
				break;

			bvec++;
			dma_addr = dma_map_page(dev, bvec->bv_page,
						bvec->bv_offset, bvec->bv_len,
						(is_write) ? DMA_TO_DEVICE : DMA_FROM_DEVICE);

			cur++;
			(i == cycle - 1) ? (i = 0) : i++;
		}
	}

	ioreq->submit_tick = pb_get_tsc_tick();

	return 0;
}
EXPORT_SYMBOL(pblaze_raid_prepare_ioreq);

int pblaze_raid_insert_ioreq(struct pblaze_raid *raid, struct io_request *ioreq, bool check)
{
	int i;
	int cpu, hash;
	bool is_write = pb_is_write_bio(ioreq->bio);
	bool is_empty = false;
	struct pblaze_pcie *pcie;
	struct io_request_priv *privs = ioreq->privs;
	struct io_request_priv *priv;
	struct pblaze_recycle_desc *recycle_desc;

	/* Make sure free ioreq not happing before the insert operation done */
	(void)pb_atomic_inc(&ioreq->ioreq_ref);

	/* Insert into pcie devices ioreq list */
	for (i = 0; i < raid->nr_dev_max; i++) {
		priv = privs + i;
		if (priv->len && (!priv->is_inserted)) {
			pcie = raid->pcie_devs[i];
			spin_lock_bh(&pcie->ioreq_list_lock);
			if (test_bit(PB_DRE_QUERY_WRITE, &raid->dre_flags) && is_write &&
			    (raid->dre_idx >= ioreq->ioreq_start) &&
			    (raid->dre_idx < ioreq->ioreq_start + ioreq->ioreq_len))
				clear_bit(PB_DRE_QUERY_WRITE, &raid->dre_flags);

			/*
			 * The firmware need ensure that the write-read sequence for same address
			 * can't reorder.
			 * Consider two io write same address, A is align, B is unalign, when B is
			 * inserted into ioreq_list first, with conflict vec filled, when A come,
			 * thanks to ioreq_list_lock, A must see that the conlict vec is set;
			 *
			 * when A is insert into ioreq_list first, if the conflict vec is not set,
			 * B must be not inserted into ioreq_list, safe at this senario, otherwise
			 * we don't care B is already inserted into ioreq_list, just delay A process
			 * until the conflict vec cleared.
			 */
			if (is_write && check &&
			    pb_test_conflict(&raid->conflict, ioreq->ioreq_start, ioreq->ioreq_len)) {
				spin_unlock_bh(&pcie->ioreq_list_lock);
				pblaze_raid_complete_io(raid, ioreq);

				return 1;
			} else {
				is_empty = list_empty(&pcie->ioreq_list);
				list_add_tail(&priv->node, &pcie->ioreq_list);
				pcie->ioreq_list_length++;
				priv->is_inserted = true;
			}
			spin_unlock_bh(&pcie->ioreq_list_lock);

			if (is_empty) {
				pblaze_pcie_issue_ioreq(pcie, false);
			}

			cpu = pb_atomic_inc(&pcie->tasklet_iter);
			recycle_desc = per_cpu_ptr(pcie->pool.recycle_percpu, cpu % (pcie->tasklet_num));
			if (pblaze_pcie_is_recycle_available(recycle_desc) &&
			    pb_test_reenter(pcie->task_hash, &hash)) {
				pblaze_pcie_recycle_pkts(pcie, recycle_desc);
				pb_clear_reenter(pcie->task_hash, hash);
			}
		}
	}

	pblaze_raid_complete_io(raid, ioreq);

	return 0;
}
EXPORT_SYMBOL(pblaze_raid_insert_ioreq);

/*
 * Dispatch the dre ioreq to pcie device, Because the dre ioreq is alway one
 * block, and the page which is allocated by kernel has align dma map address,
 * and we should avoid dre write to insert between the os read/write flow,
 * so I create a simple version dpc interface for it.
 *
 * @raid: raid device descriptor
 * @ioreq: dre and log dre io request
 * @is_log: is log dre io request
 *
 * return value:
 * -1: error cuz device is unwork
 * 0: insert failed cuz write position conflict
 * 1: insert success
 */
int pblaze_raid_dpc_dre_ioreq(struct pblaze_raid *raid, struct io_request *ioreq, bool is_log)
{
	int ret = 1;
	bool is_empty = false;
	bool is_write;
	u64 start, temp;
	u32 cycle, start_in_cycle;
	u32 sub_dev;
	struct bio_vec *bvec;
	struct io_request_priv *priv;
	struct pblaze_pcie *pcie;

	if (!raid->is_tbl_ready) {
		DPRINTK(ERR, "mapping table is not ready\n");
		pb_ioreq_complete_io(ioreq, -EINVAL);
		return -1;
	}

	start = ioreq->ioreq_start;
	cycle = raid->cycle;
	temp = start;
	start_in_cycle = do_div(temp, cycle);

#ifdef RDONLY_CHECK
	if (test_bit(DEV_RDONLY, &raid->flags) &&
	    pb_is_write_bio(ioreq->bio)) {
		DPRINTK(ERR, "Hardware read only in ioreq dispatch\n");
		pb_ioreq_complete_io(ioreq, -EIO);
		return -1;
	}
#endif

#ifdef HARDWARE_FAULTTOLERANT
	if (test_bit(DEV_FAULT, &raid->flags)) {
		DPRINTK(ERR, "Hardware fault in ioreq dispatch\n");
		pb_ioreq_complete_io(ioreq, -EINVAL);
		return -1;
	}

	raid->last_insert_jiffies = jiffies;
#endif

	if (test_bit(DEV_SHUTDOWN, &raid->flags)) {
		DPRINTK(ERR, "Device is already shutdown\n");
		pb_ioreq_complete_io(ioreq, -EIO);
		return -1;
	} else {
		mb();
		pb_atomic_inc(&raid->outstanding_io);
	}

	bvec = pb_bio_iovec(ioreq->bio);
	is_write = pb_is_write_bio(ioreq->bio);
	sub_dev = (u32)raid->dev_tbl[start_in_cycle];
	priv = ioreq->privs + sub_dev;
	pcie = raid->pcie_devs[sub_dev];
	priv->start = pblaze_get_sub_addr(start, cycle, pcie->quota, pcie->addr_tbl);
	BUG_ON(priv->start == -1);
	priv->len = 1;
	priv->small_dma_tbl[0] = dma_map_page(raid->pcie_child_dev, bvec->bv_page,
					      bvec->bv_offset, bvec->bv_len,
					      (is_write) ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
	BUG_ON(pb_get_dma_align(priv->small_dma_tbl[0]) != 0);
	ioreq->ioreq_ref = 1;

	/*
	 * Insert into pcie devices ioreq list, and it don't need inc
	 * ioreq->ioreq_ref to avoid ioreq struct free too early because the io
	 * is always on one pcie.
	 */
	ioreq->submit_tick = pb_get_tsc_tick();
	ioreq->submit_jiffies = jiffies;

	/* With spin_lock protect, Do need worry about dre_flags synchronism */
	spin_lock_bh(&pcie->ioreq_list_lock);
	is_empty = list_empty(&pcie->ioreq_list);

	if (is_log) {
		list_add_tail(&priv->node, &pcie->ioreq_list);
		pcie->ioreq_list_length++;
	} else {
		if (is_write) {
			if (test_bit(PB_DRE_QUERY_WRITE, &raid->dre_flags) &&
			    !pb_test_conflict(&raid->conflict, start, 1)) {
				list_add_tail(&priv->node, &pcie->ioreq_list);
				pcie->ioreq_list_length++;
				clear_bit(PB_DRE_QUERY_WRITE, &raid->dre_flags);
			} else {
				dma_unmap_page(raid->pcie_child_dev, priv->small_dma_tbl[0],
					       bvec->bv_len, DMA_TO_DEVICE);
				pb_atomic_dec(&raid->outstanding_io);
				ret = 0;
			}
		} else {
			set_bit(PB_DRE_QUERY_WRITE, &raid->dre_flags);
			list_add_tail(&priv->node, &pcie->ioreq_list);
			pcie->ioreq_list_length++;
		}
	}
	spin_unlock_bh(&pcie->ioreq_list_lock);

	if (is_empty && (ret == 1)) {
		pblaze_pcie_issue_ioreq(pcie, true);
	}

	return ret;
}

void release_pblaze_raid(struct kref *refcn)
{
	struct pblaze_raid *raid;

	raid = container_of(refcn, struct pblaze_raid, refcn);
	kfree(raid);
}

/* TODO: remove is_unload_lock */
bool pblaze_test_unload(struct pblaze_raid *raid)
{
	unsigned long flag;
	bool is_unload;

	spin_lock_irqsave(&raid->is_unload_lock, flag);
	is_unload = raid->is_unload;
	spin_unlock_irqrestore(&raid->is_unload_lock, flag);

	return is_unload;
}

void pblaze_set_unload(struct pblaze_raid *raid)
{
	unsigned long flag;

	spin_lock_irqsave(&raid->is_unload_lock, flag);
	raid->is_unload = true;
	spin_unlock_irqrestore(&raid->is_unload_lock, flag);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 28)
static int pblaze_raid_open(struct inode *inode, struct file *filp)
#else
static int pblaze_raid_open(struct block_device *bd, fmode_t mode)
#endif
{
	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 28)
static int pblaze_raid_release(struct inode *inode, struct file *filp)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
static int pblaze_raid_release(struct gendisk *gd, fmode_t mode)
#else
static void pblaze_raid_release(struct gendisk *gd, fmode_t mode)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
	return 0;
#endif
}

static int pblaze_raid_media_changed(struct gendisk *gd)
{
	return false;
}

static int pblaze_raid_revalidate(struct gendisk *gd)
{
	DPRINTK(WARNING, "pblaze_raid_revalidate called\n");

	return 0;
}

static int pblaze_raid_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	struct pblaze_raid *raid;

	DPRINTK(DEBUG, "++\n");
	raid = bdev->bd_disk->private_data;
	geo->heads = 0x40;
	geo->sectors = 0x20;
	geo->cylinders = pblaze_virt_dev_get_size(raid->virt_dev) >> PB_S2MB_SHIFT;;
	geo->start = 0;
	DPRINTK(DEBUG, "--\n");

	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 28)
static int pblaze_raid_ioctl(struct inode *inode, struct file *filp,
			     unsigned int cmd, unsigned long arg)
{
	struct block_device *bd = inode->i_bdev;
#else
static int pblaze_raid_ioctl(struct block_device *bd, fmode_t mode,
			     unsigned int cmd, unsigned long arg)
{
#endif
	int ret = CMD_ECHO_SUCCEED;
	struct pblaze_raid *raid = bd->bd_disk->private_data;
	struct hd_geometry geo;
	struct pblaze_firmware_name ffn;
	u32 dev_idx;
	u32 disksizeMB;
	const struct firmware *pfw;
	struct pblaze_write_rom_info *prom;
	struct pblaze_firmware_data *pfd;

	DPRINTK(DEBUG, " PCIe ioctl 0x%x 0x%lx\n", cmd, arg);
	switch (cmd) {
	case IOCTL_LOCKROM:
		if (raid->nr_dev_max != raid->nr_dev) {
			ret = -CMD_ECHO_PROBE_NO_FINISH;
		} else {
			if (unlikely(!arg)) {
				DPRINTK(ERR, "IOCTL_REINITIALIZE:arg error\n");
				ret = -CMD_ECHO_INVALID_PARMA;
			} else {
				if (copy_from_user(&dev_idx, (void __user *)arg, sizeof(u32))) {
					DPRINTK(ERR, "IOCTL_LOCKROM:copy_from_uer error\n");
					ret = -CMD_ECHO_FAILED;
				} else {
					ret = pblaze_raid_lock_rom(raid, dev_idx);
					if (ret != CMD_ECHO_SUCCEED) {
						DPRINTK(ERR, "pblaze_raid_lock_rom failed\n");
					} else {
						raid->init_monitor.rom[PB_LOCKROM_OFFSET] = PB_LOCKROM_MAGIC;
					}
				}
			}
		}
		break;

	case IOCTL_WRITEROM:
		if (raid->nr_dev_max != raid->nr_dev) {
			ret = -CMD_ECHO_PROBE_NO_FINISH;
		} else {
			if (unlikely(!arg)) {
				DPRINTK(ERR, "IOCTL_WRITEROM:arg error\n");
				ret = -CMD_ECHO_INVALID_PARMA;
			} else {
				prom = vmalloc(sizeof(struct pblaze_write_rom_info));
				if (!prom) {
					DPRINTK(ERR, "IOCTL_WRITEROM:vmalloc error\n");
					ret = -CMD_ECHO_MALLOC_FAILED;
				} else if (copy_from_user(prom, (void __user *)arg, sizeof(struct pblaze_write_rom_info))) {
					DPRINTK(ERR, "IOCTL_WRITEROM:copy_from_uer error\n");
					vfree(prom);
					ret = -CMD_ECHO_FAILED;
				} else {
					ret = pblaze_raid_send_rom(raid, PB_ROM_SIZE, prom->init_monitor.rom, prom->dev_idx);
					if (ret != CMD_ECHO_SUCCEED) {
						DPRINTK(ERR, "pblaze_raid_send_rom failed\n");
					}
					vfree(prom);
				}
			}
		}
		break;

	case HDIO_GETGEO:
		pblaze_raid_getgeo(bd, &geo);
		if (unlikely(!arg)) {
			DPRINTK(ERR, "HDIO_GETGEO:arg error\n");
			ret = -CMD_ECHO_INVALID_PARMA;
		} else if (copy_to_user((void __user *)arg, &geo, sizeof(geo))) {
			DPRINTK(ERR, "HDIO_GETGEO:copy_from_uer error\n");
			ret = -CMD_ECHO_FAILED;
		}
		break;

	case IOCTL_ATTACH:
		down(&raid->ioctl_sem);
		ret = pblaze_raid_attach(raid);
		up(&raid->ioctl_sem);
		break;

	case IOCTL_DETACH:
		down(&raid->ioctl_sem);
		ret = pblaze_raid_detach(raid);
		up(&raid->ioctl_sem);
		break;

	case IOCTL_REINITIALIZE:
		if (raid->status != ST_STAGE_RUN) {
			ret = -CMD_ECHO_FAILED;
		} else if (unlikely(!arg)) {
			DPRINTK(ERR, "IOCTL_REINITIALIZE:arg error\n");
			ret = -CMD_ECHO_INVALID_PARMA;
		} else {
			if (copy_from_user(&disksizeMB, (void __user *)arg, sizeof(u32))) {
				DPRINTK(ERR, "IOCTL_REINITIALIZE:copy_from_uer error\n");
				ret = -CMD_ECHO_FAILED;
			} else if (disksizeMB > raid->disk_capacity_MB) {
				DPRINTK(ERR, "IOCTL_REINITIALIZE SIZE TOO LARGE %d\n", disksizeMB);
				ret = -CMD_ECHO_FAILED;
			} else {
				down(&raid->ioctl_sem);
				if (raid->outstanding_handle != 0) {
					DPRINTK(ERR, "IOCTL_REINITIALIZE:outstanding_handle should be 0 but actual is %d\n", 
                                                raid->outstanding_handle);
					ret = -CMD_ECHO_FAILED;
				} else {
					pblaze_raid_beacon(raid, true);
					ret = pblaze_raid_safe_erase(raid, (disksizeMB << PB_B2MB_SHIFT) / raid->tetris_total_num);
					if (ret == CMD_ECHO_SUCCEED) {
						pblaze_raid_wait_status(raid, ST_STAGE_RUN, HZ * 600);
					}
					pblaze_raid_beacon(raid, raid->is_baecon_On);
				}
				up(&raid->ioctl_sem);
			}
		}
		break;

	case IOCTL_BAECON:
		if (unlikely(!arg)) {
			ret = -CMD_ECHO_INVALID_PARMA;
		} else {
			if (copy_from_user(&raid->is_baecon_On, (void __user *)arg, sizeof(u32))) {
				ret = -CMD_ECHO_FAILED;
			} else {
				pblaze_raid_beacon(raid, raid->is_baecon_On != 0);
			}
		}
		break;

	case IOCTL_UPDATEFIRMWARE:
		if (raid->nr_dev_max != raid->nr_dev) {
			ret = -CMD_ECHO_PROBE_NO_FINISH;
		} else if (unlikely(copy_from_user(&ffn, (void __user *)arg, sizeof(struct pblaze_firmware_name)))) {
			ret = -CMD_ECHO_FAILED;
		} else {
			ffn.name[NFS_MAXPATHLEN-1] = 0;
			down(&raid->ioctl_sem);
			if (raid->outstanding_handle != 0) {
				ret = -CMD_ECHO_FAILED;
			} else {
				ret = request_firmware(&pfw, ffn.name, raid->pcie_child_dev);
				if (!ret) {
					pfd = (struct pblaze_firmware_data *)pfw->data;
					if (pfw->size < (unsigned long)sizeof(struct pblaze_firmware_data) || pfd->length != (u32)(pfw->size - (unsigned long)(((struct pblaze_firmware_data *)0)->data))) {
						DPRINTK(ERR, "Device Firmware Size Mismatch, the file %s maybe corrupt\n", ffn.name);
						ret = -CMD_ECHO_FAILED;
					} else if (pfd->magic != MAGICNUMBER) {
						DPRINTK(ERR, "Invalid MagicNumber in Device Firmware, the file %s maybe corrupt\n", ffn.name);
						ret = -CMD_ECHO_FAILED;
					} else if (pfd->model != simple_strtoul(pb_get_str_by_key(raid->init_monitor.rom, "Firmware Model:"), NULL, 10)) {
						DPRINTK(ERR, "Firmware model mismatch, should:%s, actual:%d\n", pb_get_str_by_key(raid->pcie_child->init_monitor.rom, "Firmware Model:"), pfd->model);
						ret = -CMD_ECHO_FAILED;
					} else if (pfd->version < simple_strtoul(pb_get_str_by_key(raid->init_monitor.rom, "Firmware VersionCode:"), NULL, 10)) {
						DPRINTK(ERR, "New firmware version %d is lower than original one %d\n", pfd->version, 1);
						ret = -CMD_ECHO_FAILED;
					} else {
						pblaze_raid_beacon(raid, true);
						ret = pblaze_raid_send_fw(raid, pfw->size, (char *)pfw->data);
						pblaze_raid_beacon(raid, raid->is_baecon_On);
					}
					release_firmware(pfw);
				} else {
					DPRINTK(ERR, "Cannot load firmware %s\n", ffn.name);
				}
			}
			up(&raid->ioctl_sem);
		}
		break;

	case IOCTL_QUERYSTAT:
		if (raid->nr_dev_max != raid->nr_dev) {
			ret = -CMD_ECHO_PROBE_NO_FINISH;
		} else if (unlikely(!arg)) {
			ret = -CMD_ECHO_INVALID_PARMA;
		} else {
			/* recv_monitor can't reenterable cuz raid->ioctl_monitor is just one copy */
			down(&raid->ioctl_sem);
			ret = pblaze_raid_recv_monitor(raid, &raid->ioctl_monitor);
			if (ret) {
				DPRINTK(ERR, "pblaze read rominfo failed.\n");
			} else if (unlikely(copy_to_user((void __user *)arg, &raid->ioctl_monitor, sizeof(struct pblaze_ioctl_monitor)))) {
				ret = -CMD_ECHO_FAILED;
			}
			up(&raid->ioctl_sem);
		}
		break;

	case IOCTL_INITIALQUERYSTAT:
		if (raid->nr_dev_max != raid->nr_dev) {
			ret = -CMD_ECHO_PROBE_NO_FINISH;
		} else if (unlikely(!arg)) {
			ret = -CMD_ECHO_INVALID_PARMA;
		} else if (unlikely(copy_to_user((void __user *)arg, &raid->init_monitor, sizeof(struct pblaze_init_monitor)))) {
			ret = -CMD_ECHO_FAILED;
		}
		break;

	default:
		ret = -CMD_ECHO_FAILED;
		break;
	}
	return ret;
}

#define PRINT_NEW_BLOCK 0x80
#define PRINT_LOW_WORD  0x01
#define PRINT_HIGH_WORD 0x02

static struct pblaze_fpga_reg regs[] = {
	{0x7C80325C, 0x01, "dma_entry_dv_cnt"},
	{0x7C80325E, 0x02, "dma_unalige_entry_dv_cnt"},
	{0x7C803200, 0x01, "lba_cmd_struct_dv_cnt"},
	{0x7C803204, 0x01, "axi_daddr_dv_cmd_cnt"},
	{0x7C803208, 0x01, "lba_cmd_list_fifo_wptr_mb_cnt"},
	{0x7C803224, 0x01, "read_request_ram_read_cnt"},
	{0x7C803226, 0x02, "read_request_ram_unaliged_read_cnt"},
	{0x7C803228, 0x01, "read_request_ram_trim_cnt"},
	{0x7C80322C, 0x01, "read_request_ram_write_cnt"},
	{0x7C80322E, 0x02, "read_request_ram_unaliged_write_cnt"},
	{0x7C80320C, 0x01, "read_entry_cnt"},
	{0x7C80320E, 0x02, "read_unaliged_entry_cnt"},
	{0x7C803210, 0x01, "write_entry_cnt"},
	{0x7C803212, 0x02, "write_unaliged_entry_cnt"},
	{0x7C803214, 0x01, "interrupt_s2c_dv_cnt"},
	{0x7C803218, 0x01, "c2s_read_req_cnt"},
	{0x7C80321C, 0x01, "c2s_read_dma_dv_cnt"},
	{0x7C803230, 0x01, "tx_tlp_read_request_cnt"},
	{0x7C803220, 0x01, "s2c_write_req_cnt"},
	{0x7C803234, 0x01, "axi_daddr_dv_s2c_sof_cnt"},
	{0x7C803244, 0x01, "register_s2c_cnt"},
	{0x7C80323C, 0x01, "register_c2s_cnt"},
	{0x7C803240, 0x01, "register_int_cnt"},
	{0x7C803242, 0x02, "inta_cnt"},
	{0x7C803248, 0x01, "int_reg_status"},
	{0x7C80324C, 0x01, "msix_int_cnt"},
	{0x7C803250, 0x01, "c2s_read_dma_done_cnt"},
	{0x7C803254, 0x01, "c2s_interrupt_en_cnt"},
	{0x7C803258, 0x01, "register_int_cnt_read64"},
	{0x7C803258, 0x02, "register_int_cnt_read32"},
	{0x7C803260, 0x01, "mb_wptr_ram_s2c_done_wptr_clr_cnt"},
	{0x7C803264, 0x01, "mb_wptr_ram_s2c_done_wptr_clr_cnt_clr_cnt"},
	{0x7C803268, 0x01, "s2c_complete_index_dv_cnt"},
	{0x7C803274, 0x01, "s2c_egress_index_dv_cnt"},
	{0x7C803276, 0x02, "mb_release_index_dv_cnt"},
	{0x7C803278, 0x01, "fc_interrupt_trim_dv_cnt"},

	{0x7C803380, 0x81, "S_rd_cnt_nand"},
	{0x7C803382, 0x02, "S_rd_cnt_nand_front"},
	{0x7C803384, 0x01, "S_rd_cnt"},
	{0x7C803388, 0x01, "CH0_S_we_cnt"},
	{0x7C80338A, 0x02, "CH1_S_we_cnt"},
	{0x7C80338C, 0x01, "CH2_S_we_cnt"},
	{0x7C80338E, 0x02, "CH3_S_we_cnt"},
	{0x7C803390, 0x01, "CH4_S_we_cnt"},
	{0x7C803392, 0x02, "CH5_S_we_cnt"},
	{0x7C803394, 0x01, "CH6_S_we_cnt"},
	{0x7C803396, 0x02, "CH7_S_we_cnt"},
	{0x7C803398, 0x01, "CH8_S_we_cnt"},
	{0x7C80339A, 0x02, "CH9_S_we_cnt"},
	{0x7C80339C, 0x01, "CH10_S_we_cnt"},
	{0x7C80339E, 0x02, "CH11_S_we_cnt"},
	{0x7C8033A0, 0x01, "CH0_read_cnt"},
	{0x7C8033A2, 0x02, "CH1_read_cnt"},
	{0x7C8033A4, 0x01, "CH2_read_cnt"},
	{0x7C8033A6, 0x02, "CH3_read_cnt"},
	{0x7C8033A8, 0x01, "CH4_read_cnt"},
	{0x7C8033AA, 0x02, "CH5_read_cnt"},
	{0x7C8033AC, 0x01, "CH6_read_cnt"},
	{0x7C8033AE, 0x02, "CH7_read_cnt"},
	{0x7C8033B0, 0x01, "CH8_read_cnt"},
	{0x7C8033B2, 0x02, "CH9_read_cnt"},
	{0x7C8033B4, 0x01, "CH10_read_cnt"},
	{0x7C8033B6, 0x02, "CH11_read_cnt"},
	{0x7C8033B8, 0x01, "CH0_read_front_cnt"},
	{0x7C8033BA, 0x02, "CH1_read_front_cnt"},
	{0x7C8033BC, 0x01, "CH2_read_front_cnt"},
	{0x7C8033BE, 0x02, "CH3_read_front_cnt"},
	{0x7C8033C0, 0x01, "CH4_read_front_cnt"},
	{0x7C8033C2, 0x02, "CH5_read_front_cnt"},
	{0x7C8033C4, 0x01, "CH6_read_front_cnt"},
	{0x7C8033C6, 0x02, "CH7_read_front_cnt"},
	{0x7C8033C8, 0x01, "CH8_read_front_cnt"},
	{0x7C8033CA, 0x02, "ch9_read_front_cnt"},
	{0x7C8033CC, 0x01, "ch10_read_front_cnt"},
	{0x7C8033CE, 0x02, "ch11_read_front_cnt"},
	{0x7C8033D0, 0x01, "inst_read_cnt"},
	{0x7C8033D2, 0x02, "inst_prog_cnt"},

	{0x7C803304, 0x81, "CH0_S_interleave_cnt"},
	{0x7C803306, 0x02, "CH1_S_interleave_cnt"},
	{0x7C803308, 0x01, "CH2_S_interleave_cnt"},
	{0x7C80330A, 0x02, "CH3_S_interleave_cnt"},
	{0x7C80330C, 0x01, "CH4_S_interleave_cnt"},
	{0x7C80330E, 0x02, "CH5_S_interleave_cnt"},
	{0x7C803310, 0x01, "CH6_S_interleave_cnt"},
	{0x7C803312, 0x02, "CH7_S_interleave_cnt"},
	{0x7C803314, 0x01, "CH8_S_interleave_cnt"},
	{0x7C803316, 0x02, "CH9_S_interleave_cnt"},
	{0x7C803318, 0x01, "CH10_S_interleave_cnt"},
	{0x7C80331A, 0x02, "CH11_S_interleave_cnt"},
	{0x7C80331C, 0x01, "CH0_S_8k_cnt_wr_nand_cmd"},
	{0x7C80331E, 0x02, "CH1_S_8k_cnt_wr_nand_cmd"},
	{0x7C803320, 0x01, "CH2_S_8k_cnt_wr_nand_cmd"},
	{0x7C803322, 0x02, "CH3_S_8k_cnt_wr_nand_cmd"},
	{0x7C803324, 0x01, "CH4_S_8k_cnt_wr_nand_cmd"},
	{0x7C803326, 0x02, "CH5_S_8k_cnt_wr_nand_cmd"},
	{0x7C803328, 0x01, "CH6_S_8k_cnt_wr_nand_cmd"},
	{0x7C80332A, 0x02, "CH7_S_8k_cnt_wr_nand_cmd"},
	{0x7C80332C, 0x01, "CH8_S_8k_cnt_wr_nand_cmd"},
	{0x7C80332E, 0x02, "CH9_S_8k_cnt_wr_nand_cmd"},
	{0x7C803330, 0x01, "CH10_S_8k_cnt_wr_nand_cmd"},
	{0x7C803332, 0x02, "CH11_S_8k_cnt_wr_nand_cmd"},
	{0x7C803334, 0x01, "CH0_S_cmd_cnt_controller"},
	{0x7C803336, 0x02, "CH1_S_cmd_cnt_controller"},
	{0x7C803338, 0x01, "CH2_S_cmd_cnt_controller"},
	{0x7C80333A, 0x02, "CH3_S_cmd_cnt_controller"},
	{0x7C80333C, 0x01, "CH4_S_cmd_cnt_controller"},
	{0x7C80333E, 0x02, "CH5_S_cmd_cnt_controller"},
	{0x7C803340, 0x01, "CH6_S_cmd_cnt_controller"},
	{0x7C803342, 0x02, "CH7_S_cmd_cnt_controller"},
	{0x7C803344, 0x01, "CH8_S_cmd_cnt_controller"},
	{0x7C803346, 0x02, "CH9_S_cmd_cnt_controller"},
	{0x7C803348, 0x01, "CH10_S_cmd_cnt_controller"},
	{0x7C80334A, 0x02, "CH11_S_cmd_cnt_controller"},
	{0x7C80334C, 0x01, "CH0_S_cmd_cnt_intel"},
	{0x7C80334E, 0x02, "CH1_S_cmd_cnt_intel"},
	{0x7C803350, 0x01, "CH2_S_cmd_cnt_intel"},
	{0x7C803352, 0x02, "CH3_S_cmd_cnt_intel"},
	{0x7C803354, 0x01, "CH4_S_cmd_cnt_intel"},
	{0x7C803356, 0x02, "CH5_S_cmd_cnt_intel"},
	{0x7C803358, 0x01, "CH6_S_cmd_cnt_intel"},
	{0x7C80335A, 0x02, "CH7_S_cmd_cnt_intel"},
	{0x7C80335C, 0x01, "CH8_S_cmd_cnt_intel"},
	{0x7C80335E, 0x02, "CH9_S_cmd_cnt_intel"},
	{0x7C803360, 0x01, "CH10_S_cmd_cnt_intel"},
	{0x7C803362, 0x02, "CH11_S_cmd_cnt_intel"},
	{0x7C803364, 0x01, "CH0_none_read_error_cnt"},
	{0x7C803366, 0x02, "CH1_none_read_error_cnt"},
	{0x7C803368, 0x01, "CH2_none_read_error_cnt"},
	{0x7C80336A, 0x02, "CH3_none_read_error_cnt"},
	{0x7C80336C, 0x01, "CH4_none_read_error_cnt"},
	{0x7C80336E, 0x02, "CH5_none_read_error_cnt"},
	{0x7C803370, 0x01, "CH6_none_read_error_cnt"},
	{0x7C803372, 0x02, "CH7_none_read_error_cnt"},
	{0x7C803374, 0x01, "CH8_none_read_error_cnt"},
	{0x7C803376, 0x02, "CH9_none_read_error_cnt"},
	{0x7C803378, 0x01, "CH10_none_read_error_cnt"},
	{0x7C80337A, 0x02, "CH11_none_read_error_cnt"},

	{0x7C803284, 0x81, "S_4k_cnt_mux"},
	{0x7C803288, 0x01, "S_4k_cnt_decode"},
	{0x7C80328C, 0x01, "S_4k_cnt_demux"},
	{0x7C803290, 0x01, "S_8k_cnt_cmd"},
	{0x7C803294, 0x01, "S_pkg_gen_cnt"},
	{0x7C803298, 0x01, "S_axi_valid_pos_cnt"},
	{0x7C80329A, 0x02, "S_axi_valid_cnt"},
	{0x7C80329C, 0x01, "S_arvalid_cnt"},
	{0x7C80329E, 0x02, "nand_rpkg_error_header_dv_cnt"},
	{0x7C8032A0, 0x01, "nand_rpkg_bch_error_header_dv_cnt"},
	{0x7C8032A2, 0x02, "nand2mem_wdone_dv_cnt"},
	{0x7C8032A4, 0x01, "xor_wdone_dv_cnt"},
	{0x7C8032A6, 0x02, "xor_rpkg_sob_cnt"},
	{0x7C8032A8, 0x01, "nand_rpkg_2pcie_dv_cnt"},
	{0x7C8032AA, 0x02, "mem_rpkg_sob_cnt"},
	{0x7C8032AC, 0x01, "nand_rpkg_2mem_sob_cnt"},
	{0x7C8032AE, 0x02, "d2p_cmd_dv_cnt"},
	{0x7C8032B0, 0x01, "mem_rpkg_buf_sob_cnt_cnt"},
	{0x7C8032B2, 0x02, "mem_rpkg_2n_sob_cnt"},
	{0x7C8032B4, 0x01, "mem_rpkg_2n_sob_xen_cnt"},
	{0x7C8032B6, 0x02, "n2x_cmd_header_only_cnt"},
	{0x7C8032B8, 0x01, "n2x_cmd_dv_4k_cnt"}
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
static int pblaze_fpga_write_proc(struct file *file, const char __user *buffer, unsigned long count, void *data)
#else
static ssize_t pblaze_fpga_write_proc(struct file *file, const char __user *buffer, size_t count, loff_t *data)
#endif
{
	char *cmd;
	ssize_t ret = 0;
	struct pblaze_pcie *pcie = (struct pblaze_pcie *)data;

	cmd = kmalloc(count, GFP_KERNEL);
	if (cmd == NULL) {
		DPRINTK(ERR, "Can't alloc fpga cmd buffer\n");
		return -ENOMEM;
	}

	if (copy_from_user(cmd, buffer, count)) {
		ret = -EFAULT;
		goto end;
	}

	cmd[count - 1] = '\0';
	if (strcmp(cmd, "clear") != 0) {
		ret = -EINVAL;
		goto end;
	} else {
		writel(0, pcie->regs + 0x74);
	}

end:
	kfree(cmd);
	return ret ? ret : count;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
static int pblaze_fpga_read_proc(char *page, char **start, off_t off,
				 int count, int *eof, void *data)
{
	struct pblaze_pcie *dev = (struct pblaze_pcie *)data;
	static const char *begin = "--------------------- BEGIN --------------------------------\t";
	static const char *end =   "--------------------- END   --------------------------------\t\t";
	static const char *mid =   "------------------------------------------------------------\t\t";
	static int nr = sizeof(regs) / sizeof(struct pblaze_fpga_reg);
	int i, k;
	u32 value;
	int len = 0;
	off_t pos = 0;

	len += sprintf(page + len, "%s\n", begin);
	for (i = 0; i < nr; i++) {
		writel(regs[i].addr, dev->regs + 0x70);

		/* read 16 times for sure */
		for (k = 0; k < 16; k++)
			value = readl(dev->regs + 0x74);

		if (regs[i].flag & PRINT_NEW_BLOCK)
			len += sprintf(page + len, "%s\n", mid);

		if (regs[i].flag & PRINT_LOW_WORD)
			value &= 0xffff;
		else if (regs[i].flag & PRINT_HIGH_WORD) {
			value &= 0xffff0000;
			value >>= 16;
		}

		len += sprintf(page + len, "%-44s: %5d  %4x %8x\n",
			       regs[i].name, value, value, regs[i].addr);

		if (len + pos > off+count)
			goto done;
		if (len + pos < off) {
			pos += len;
			len = 0;
		}
	}

	len += sprintf(page + len, "%s\n", end);
	*eof = 1;
done:
	if (off >= len + pos)
		return 0;
	*start = page + (off - pos);
	return ((count < pos + len - off) ? count : pos + len - off);
}

static int pblaze_stat_proc_show_raid(struct pblaze_raid *raid, char *page, 
				      int ret)
{
	ret += sprintf(page + ret, "Raid:\n");
	ret += sprintf(page + ret, 
		       "driver version: %s\n"
		       "disk capacity: %lluMB\n"
		       "disk size: %uMB\n"
		       "tetris total num: %u\n"
		       "is_unload: %u\n"
		       "status: %u\n"
		       "flags: 0x%lx\n"
		       "outstanding_io: %u\n",
		       raid->init_monitor.driver_version,
		       raid->disk_capacity_MB,
		       raid->disk_size_MB,
		       raid->tetris_total_num,
		       raid->is_unload, 
		       raid->status, 
		       raid->flags, 
		       raid->outstanding_io);

	ret += sprintf(page + ret,
		       "dre_flags: 0x%lx\n"
		       "dre_timeout: %lu\n"
		       "dre_idx: %u, dre_log_idx: %u, dre_max_idx: %u\n",
		       raid->dre_flags, raid->dre_timeout, raid->dre_idx,
		       raid->dre_log_idx, raid->dre_max_idx);

	ret += sprintf(page + ret, "conflict cnt: %u, mask:%lx\n",
		       raid->conflict.cnt, raid->conflict.mask);
	return ret;
}

static int pblaze_stat_read_proc(char *page, char **start, off_t off,
				 int count, int *eof, void *data)
{
	int ret = 0;
	struct pblaze_pcie *pcie = (struct pblaze_pcie *)data;
	static u16 slots[INTERRUPT_QUEUE_SIZE];
	int i, j;
	static int nr;
	u8 *stats = (u8 *)&pcie->pool.packets_stat[0];
	struct pblaze_comm_dev *pcd;
#ifdef PBLAZE_COUNTER
	volatile u32 *ops_cnt = pcie->pool.ops_cnt;
	volatile u32 *pkts_stat = pcie->pool.pkts_stat;
#endif

	if (off != 0)
		goto more;

	ret += pblaze_stat_proc_show_raid(pcie->raid, page, ret);

	ret += sprintf(page + ret, "\nPcie: %d\n", pcie->sub_dev_index);
	ret += sprintf(page + ret, "ioreq_list: %llu\n", pcie->ioreq_list_length);
	ret += sprintf(page + ret, "isr: %x, isr_mask: %x, enabled: %x\n",
		       pcie->intr_stat, pcie->intr_mask, readl(pcie->regs + ISRMASK_ADDR));

	for (i = COMM_DEV_START; i < COMM_DEV_END; i++) {
		pcd = pcie->pcd + i;
		ret += sprintf(page+ret, "c2s_last: %x, c2s: %x, s2c: %x\n",
			       pcie->c2s_values[i],
			       readl(pcd->read_reg), readl(pcd->write_reg));
	}

	nr = 0;
	for (i = 0; i < INTERRUPT_QUEUE_SIZE; i++)
		if (stats[i])
			slots[nr++] = i;

#ifdef PBLAZE_COUNTER
	ret += sprintf(page + ret, "cur_intr_cnt: %u, max_intr_cnt: %u\n",
		       pcie->cur_cnt, pcie->max_cnt);
	ret += sprintf(page + ret, "\nfree\talloced\tsent\trecvd\n");
	ret += sprintf(page + ret, "%u\t%u\t%u\t%u\t\n",
		       pkts_stat[NR_PKT_FREE],
		       pkts_stat[NR_PKT_ALLOCED],
		       pkts_stat[NR_PKT_SENT],
		       pkts_stat[NR_PKT_RECVD]);
	ret += sprintf(page + ret, "\ndma_ops\t\tdma_cmds\tdma_entrys\tdma_read\tdma_write\n");
	ret += sprintf(page + ret, "%u\t\t%u\t\t%u\t\t%u\t\t%u\n",
		       ops_cnt[NR_DMA_OPS],
		       ops_cnt[NR_DMA_CMDS],
		       ops_cnt[NR_DMA_ENTRYS],
		       ops_cnt[NR_DMA_READ],
		       ops_cnt[NR_DMA_WRITE]);
	ret += sprintf(page + ret, "\nsubmit_cnt: %u, recv_cnt: %u, cmpl_cnt: %u\n",
		       pcie->raid->submit_cnt, pcie->raid->recv_cnt, pcie->raid->cmpl_cnt);

#endif

more:
	for (i = off, j = 0; i < nr; i++) {
		if (ret >= (PAGE_SIZE - 1024 - 256)) {
			*start = (char *)(unsigned long)j;
			goto end;
		}

		ret += sprintf(page + ret, (i % 16 || i == 0) ? "%5d" : "\n%5d", slots[i]);
		j++;
	}
	ret += sprintf(page + ret, "\n");

	*start = (char *)(unsigned long)j;
	*eof = 1;
end:
	return ret;
}
#else
static int pblaze_fpga_show_proc(struct seq_file *m, void *v)
{
	struct pblaze_pcie *dev = (struct pblaze_pcie *)m->private;
	static const char *begin = "--------------------- BEGIN --------------------------------\t";
	static const char *end =   "--------------------- END   --------------------------------\t\t";
	static const char *mid =   "------------------------------------------------------------\t\t";
	static int nr = sizeof(regs) / sizeof(struct pblaze_fpga_reg);
	int i, j, k;
	u32 value;

	seq_printf(m, "%s\n", begin);

	for (i = 0, j = 0; i < nr; i++) {
		writel(regs[i].addr, dev->regs + 0x70);

		/* read 16 times for sure */
		for (k = 0; k < 16; k++)
			value = readl(dev->regs + 0x74);

		if (regs[i].flag & PRINT_NEW_BLOCK)
			seq_printf(m, "%s\n", mid);
		if (regs[i].flag & PRINT_LOW_WORD)
			value &= 0xffff;
		else if (regs[i].flag & PRINT_HIGH_WORD) {
			value &= 0xffff0000;
			value >>= 16;
		}

		seq_printf(m, "%-44s: %5d  %4x %8x\n", regs[i].name, value, value, regs[i].addr);
		j++;
	}

	seq_printf(m, "%s\n", end);
	return 0;
}

static int pblaze_fpga_open_proc(struct inode *inode, struct file *file)
{
	return single_open(file, pblaze_fpga_show_proc, PDE_DATA(inode));
}

static const struct file_operations pb_proc_fpga_operations = {
	.owner		= THIS_MODULE,
	.open		= pblaze_fpga_open_proc,
	.read		= seq_read,
	.write		= pblaze_fpga_write_proc,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void pblaze_stat_seq_show_raid(struct pblaze_raid *raid, struct seq_file *m)
{
	seq_printf(m, "Raid:\n"
		       "driver version: %s\n"
		       "disk capacity: %lluMB\n"
		       "disk size: %uMB\n"
		       "tetris total num: %u\n"
		       "is_unload: %u\n"
		       "status: %u\n"
		       "flags: 0x%lx\n"
		       "outstanding_io: %u\n",
		       raid->init_monitor.driver_version,
		       raid->disk_capacity_MB,
		       raid->disk_size_MB,
		       raid->tetris_total_num,
		       raid->is_unload, 
		       raid->status, 
		       raid->flags, 
		       raid->outstanding_io);

	seq_printf(m, "dre_flags: 0x%lx\n"
		       "dre_timeout: %lu\n"
		       "dre_idx: %u, dre_log_idx: %u, dre_max_idx: %u\n",
		       raid->dre_flags, raid->dre_timeout, raid->dre_idx,
		       raid->dre_log_idx, raid->dre_max_idx);

	seq_printf(m, "conflict cnt: %u, mask:%lx\n",
		       raid->conflict.cnt, raid->conflict.mask);
}

static int pblaze_stat_show_proc(struct seq_file *m, void *v)
{
	struct pblaze_pcie *pcie = (struct pblaze_pcie *)m->private;
	static u16 slots[INTERRUPT_QUEUE_SIZE];
	int i;
	static int nr;
	u8 *stats = (u8 *)&pcie->pool.packets_stat[0];
	struct pblaze_comm_dev *pcd;
#ifdef PBLAZE_COUNTER
	volatile u32 *ops_cnt = pcie->pool.ops_cnt;
	volatile u32 *pkts_stat = pcie->pool.pkts_stat;
#endif

	pblaze_stat_seq_show_raid(pcie->raid, m);

	seq_printf(m, "\nPcie: %u\n ioreq_list: %llu\nisr: %x, isr_mask: %x, enabled: %x\n",
		   pcie->sub_dev_index, pcie->ioreq_list_length, pcie->intr_stat, 
		   pcie->intr_mask, readl(pcie->regs + ISRMASK_ADDR));

	for (i = COMM_DEV_START; i < COMM_DEV_END; i++) {
		pcd = pcie->pcd + i;
		seq_printf(m, "c2s_last: %x, c2s: %x, s2c: %x\n",
			   pcie->c2s_values[i], readl(pcd->read_reg), readl(pcd->write_reg));
	}

	nr = 0;
	for (i = 0; i < INTERRUPT_QUEUE_SIZE; i++)
		if (stats[i])
			slots[nr++] = i;

#ifdef PBLAZE_COUNTER
	seq_printf(m, "cur_intr_cnt: %u, max_intr_cnt: %u\n",
		   pcie->cur_cnt, pcie->max_cnt);
	seq_printf(m, "\nfree\talloced\tsent\trecvd\n");
	seq_printf(m, "%u\t%u\t%u\t%u\n",
		   pkts_stat[NR_PKT_FREE],
		   pkts_stat[NR_PKT_ALLOCED],
		   pkts_stat[NR_PKT_SENT],
		   pkts_stat[NR_PKT_RECVD]);
	seq_printf(m, "\ndma_ops\t\tdma_cmds\tdma_ents\tdma_read\tdma_write\n");
	seq_printf(m, "%u\t\t%u\t\t%u\t\t%u\t\t%u\n",
		   ops_cnt[NR_DMA_OPS],
		   ops_cnt[NR_DMA_CMDS],
		   ops_cnt[NR_DMA_ENTRYS],
		   ops_cnt[NR_DMA_READ],
		   ops_cnt[NR_DMA_WRITE]);
	seq_printf(m, "\nsubmit_cnt: %u, recv_cnt: %u, cmpl_cnt: %u\n",
		   pcie->raid->submit_cnt, pcie->raid->recv_cnt, pcie->raid->cmpl_cnt);

#endif

	for (i = 0; i < nr; i++) {
		seq_printf(m, (i % 16 || i == 0) ? "%5d" : "\n%5d", slots[i]);
	}
	seq_printf(m, "\n");

	return 0;
}

static int pblaze_stat_open_proc(struct inode *inode, struct file *file)
{
	return single_open(file, pblaze_stat_show_proc, PDE_DATA(inode));
}

static const struct file_operations pb_proc_stat_operations = {
	.owner		= THIS_MODULE,
	.open		= pblaze_stat_open_proc,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 9, 1)
static void pblaze_remove_proc_for_pcie(struct pblaze_pcie *pcie, struct proc_dir_entry *raid_ent)
{
	u32 id;
	char stat_str[8], fpga_str[8];

	id = pcie->sub_dev_index;
	snprintf(stat_str, 8, "stat-%u", id);
	snprintf(fpga_str, 8, "fpga-%u", id);

	remove_proc_entry(stat_str, raid_ent);
	remove_proc_entry(fpga_str, raid_ent);
}
#endif

static void pblaze_remove_proc_for_raid(struct pblaze_raid *raid)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 9, 1)
	int i;
	struct pblaze_pcie *pcie;

	for (i = 0; i < raid->nr_dev_max; i++) {
		pcie = raid->pcie_devs[i];
		if (pcie == NULL)
			continue;

		pblaze_remove_proc_for_pcie(pcie, raid->proc_dir);
	}

	remove_proc_entry(raid->proc_dir->name, raid->proc_dir->parent);
#else
	remove_proc_subtree(raid->gendisk->disk_name, NULL);
#endif
	raid->proc_dir = NULL;
}

static int pblaze_create_proc_for_pcie(struct pblaze_pcie *pcie,
				       struct proc_dir_entry *raid_ent)
{
	int i;
	u32 id;
	char stat_str[8], fpga_str[8];
	struct proc_dir_entry *ent;

	id = pcie->sub_dev_index;
	snprintf(stat_str, 8, "stat-%u", id);
	snprintf(fpga_str, 8, "fpga-%u", id);

	/* If create one proc file failed, just return without deleting others */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
	ent = create_proc_entry(stat_str, 0444, raid_ent);
	if (ent == NULL) {
		DPRINTK(ERR, "Create entry stat failed\n");
		return -ENOMEM;
	}
	ent->read_proc = pblaze_stat_read_proc;
	ent->write_proc = NULL;
	ent->data = pcie;

	ent = create_proc_entry(fpga_str, 0644, raid_ent);
	if (ent == NULL) {
		DPRINTK(ERR, "Create entry fpga failed\n");
		return -ENOMEM;
	}
	ent->read_proc = pblaze_fpga_read_proc;
	ent->write_proc = pblaze_fpga_write_proc;
	ent->data = pcie;
#else
	ent = proc_create_data(stat_str, 0444, raid_ent, &pb_proc_stat_operations, pcie);
	if (ent == NULL) {
		DPRINTK(ERR, "Create entry stat failed\n");
		return -ENOMEM;
	}

	ent = proc_create_data(fpga_str, 0644, raid_ent, &pb_proc_fpga_operations, pcie);
	if (ent == NULL) {
		DPRINTK(ERR, "Create entry fpga failed\n");
		return -ENOMEM;
	}
#endif

	for (i = 0; i < NR_PKT_STAT_ITEMS; i++)
		pblaze_pcie_stat_set(&pcie->pool.pkts_stat[i], 0);
	pblaze_pcie_stat_set(&pcie->pool.pkts_stat[NR_PKT_FREE], INTERRUPT_QUEUE_SIZE);
	for (i = 0; i < NR_OPS_CNT_ITEMS; i++)
		pblaze_pcie_stat_set(&pcie->pool.ops_cnt[i], 0);

	return 0;
}

static int pblaze_create_proc_for_raid(struct pblaze_raid *raid)
{
	int ret = 0;
	int flag = 0;
	int i;
	struct pblaze_pcie *pcie;

	raid->proc_dir = proc_mkdir(raid->gendisk->disk_name, NULL);
	if (raid->proc_dir == NULL)
		return -ENOMEM;

	/* If create proc file on one pcie failed, just return without deleting raid proc file */
	for (i = 0; i < raid->nr_dev_max; i++) {
		pcie = raid->pcie_devs[i];
		ret = pblaze_create_proc_for_pcie(pcie, raid->proc_dir);
		if (ret) {
			flag = 1;
			DPRINTK(WARNING, "Create proc for pcie:%d failed\n", i);
		}
	}

	/* Return the last error */
	return (flag == 0) ? 0 : -1;
}

static void pblaze_raid_proc_init(void)
{
	int ret = 0;
	struct pblaze_raid *raid;

	if (list_empty(&raid_list)) {
		DPRINTK(NOTICE, "Raid list is empty, skip proc init\n");
		return;
	}

	list_for_each_entry(raid, &raid_list, node) {
		if (raid->proc_dir != NULL)
			continue;
		ret = pblaze_create_proc_for_raid(raid);
		if (ret) {
			DPRINTK(WARNING, "Create proc for raid failed\n");
		}
	}
}

void pblaze_raid_proc_exit(void)
{
	struct pblaze_raid *raid;

	if (list_empty(&raid_list))
		return;

	list_for_each_entry(raid, &raid_list, node) {
		if (raid->proc_dir == NULL)
			continue;

		pblaze_remove_proc_for_raid(raid);
	}
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 32)
struct block_device_operations pblaze_raid_ops = {
#else
const struct block_device_operations pblaze_raid_ops = {
#endif
	.open = pblaze_raid_open,
	.release = pblaze_raid_release,
	.media_changed = pblaze_raid_media_changed,
	.revalidate_disk = pblaze_raid_revalidate,
	.ioctl = pblaze_raid_ioctl,
#if	LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 9)
	.getgeo = pblaze_raid_getgeo,
#endif
	.owner = THIS_MODULE,
};

static void pblaze_raid_check_timeout(struct pblaze_raid *raid)
{
	int i = 0;
	for (i = 0; i < raid->nr_dev; i++) {
		pblaze_pcie_check_timeout(raid->pcie_devs[i]);
	}
}

static void pblaze_trigger_raid_interrupt(struct pblaze_raid *raid)
{
	struct pblaze_pcie *pcie = NULL;
	int i = 0;

	for (i = 0; i < raid->nr_dev; i++) {
		pcie = raid->pcie_devs[i];
		pblaze_trigger_pcie_interrupt(pcie);
	}
	return;
}

void pblaze_dump_pcie_fpga_counter(struct pblaze_pcie *dev)
{
	static const char *begin = "------------------- BEGIN -----------------\t";
	static const char *end = "------------------- END ---------------------\t";
	static const char *mid = "---------------------------------------------\t";
	int i = 0, k = 0;
	u32 value = 0;
	int nr = sizeof(regs) / sizeof(struct pblaze_fpga_reg);

	printk("dump fpga counter of pcie 0x%p:\n", dev);
	printk(KERN_ALERT  "%s\n", begin);

	printk(KERN_ALERT  "pcd 0 read 0x%x, write 0x%x\n",
				   readl(dev->pcd[0].read_reg), readl(dev->pcd[0].write_reg)); 

	printk(KERN_ALERT  "pcd 1 read 0x%x, write 0x%x\n",
				   readl(dev->pcd[1].read_reg), readl(dev->pcd[1].write_reg)); 

	for (i = 0; i < nr; i++) {
		writel(regs[i].addr, dev->regs + 0x70);

		/* read 16 times for sure */
		for (k = 0; k < 16; k++)
			value = readl(dev->regs + 0x74);

		if (regs[i].flag & 0x80)
			printk(KERN_ALERT  "%s\n", mid);

		if (regs[i].flag & 0x01)
			value &= 0xffff;
		else if (regs[i].flag & 0x02) {
			value &= 0xffff0000;
			value >>= 16;
		}

		printk(KERN_ALERT  "%-44s: %5d  %4x %8x\n",
			       regs[i].name, value, value, regs[i].addr);

	}
	printk(KERN_ALERT  "%s\n", end);
}

static void pblaze_dump_raid_fpga_counter(struct pblaze_raid *raid)
{
	struct pblaze_pcie *pcie = NULL;
	int i = 0;

	for (i = 0; i < raid->nr_dev; i++) {
		pcie = raid->pcie_devs[i];
		pblaze_dump_pcie_fpga_counter(pcie);
	}
}

#ifdef HARDWARE_FAULTTOLERANT
void pblaze_raid_timer(unsigned long  dev)
{
	struct pblaze_raid *raid = (struct pblaze_raid *)dev;

	if (unlikely(raid->is_unload)) {
		return;
	}

	if (raid->outstanding_io == 0) {
		raid->io_timer_list.expires = jiffies + HZ * 2;
		add_timer(&raid->io_timer_list);
		return;
	}

	if (time_after_eq(jiffies, raid->last_insert_jiffies + HZ * 1200)) {
		set_bit(DEV_FAULT, &raid->flags);
		set_bit(PB_CRITICAL_IO_TIMEOUT, &raid->error_code);
		pblaze_raid_check_timeout(raid);
		DPRINTK(ALERT, "ERROR: IO excetion\n");
		pblaze_generate_bug_report(raid);
	} else if (time_after_eq(jiffies, raid->last_insert_jiffies + HZ)) {
		pblaze_dump_raid_fpga_counter(raid);
		pblaze_trigger_raid_interrupt(raid);
		raid->io_timer_list.expires = jiffies + HZ;
		add_timer(&raid->io_timer_list);
	}
}

#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)
int pblaze_raid_make_request(struct request_queue *q, struct bio *bio)
#else
blk_qc_t pblaze_raid_make_request(struct request_queue *q, struct bio *bio)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 24)
	bio_endio(bio, 0, -EIO);
#else
	bio_endio(bio);
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)
	return 0;
#else
	return BLK_QC_T_NONE;
#endif
}

struct pblaze_raid *pblaze_raid_match(char *sn)
{
	int flag = 0;
	size_t len_old, len_new;
	struct pblaze_raid *pair_raid = NULL;

	if (list_empty(&raid_list))
		return NULL;

	len_old = strlen(sn);

	/* iter raid list and compare sn */
	spin_lock(&raid_list_lock);
	list_for_each_entry(pair_raid, &raid_list, node) {
		len_new = strlen(pair_raid->dev_sn);
		if (len_old != len_new)
			continue;

		if (pair_raid->nr_dev == pair_raid->nr_dev_max) {
			DPRINTK(WARNING, "Found a reduplicate pcie dev\n");
			continue;
		}

		if (strcmp(pair_raid->dev_sn, sn) == 0) {
			flag = 1;
			break;
		}
	}
	spin_unlock(&raid_list_lock);

	return flag == 1 ? pair_raid : NULL;
}

static struct pblaze_raid *
pblaze_raid_probe_one(struct pblaze_pcie *pcie, char *sn, u32 dev_num)
{
	int ret = 0;
	struct pblaze_raid *raid = NULL;

	DPRINTK(DEBUG, "raid probe one\n");

	pb_atomic_add(&raid_drv.driver_mem_size, sizeof(struct pblaze_raid));

	/* alloc raid device */
	raid = kmalloc(sizeof(struct pblaze_raid), GFP_KERNEL);
	if (!raid) {
		ret = -ENOMEM;
		goto failed_alloc_raid;
	}

	memset(raid, 0, sizeof(struct pblaze_raid));
	kref_init(&raid->refcn);
	raid->dev_sn = sn;
	raid->nr_dev_max = dev_num;
	memcpy(&raid->init_monitor, &pcie->init_monitor, sizeof(struct pblaze_init_monitor));
	init_waitqueue_head(&raid->stat_wq);
	init_completion(&raid->dre_exit_completion);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
	INIT_WORK(&(raid->stat_work), pblaze_raid_status_change_worker, raid);
	INIT_WORK(&(raid->bug_report_work), pblaze_raid_collect_bug_report, NULL);
#else
	INIT_WORK(&(raid->stat_work), pblaze_raid_status_change_worker);
	INIT_WORK(&(raid->bug_report_work), pblaze_raid_collect_bug_report);
#endif
	spin_lock_init(&raid->is_unload_lock);
	spin_lock_init(&raid->conflict.lock);

	return raid;

failed_alloc_raid:
	pb_atomic_sub(&raid_drv.driver_mem_size, sizeof(struct pblaze_raid));
	return NULL;
}

static int pblaze_raid_probe_two(struct pblaze_raid *raid)
{
	int ret = 0;
	int minor = -1;

	DPRINTK(DEBUG, "raid probe two\n");

	/* create request queue for raid */
	raid->queue = blk_alloc_queue(GFP_KERNEL);
	if (!raid->queue) {
		ret = -ENOMEM;
		goto failed_alloc_queue;
	}

	raid->queue->queuedata = raid;
	blk_queue_make_request(raid->queue, pblaze_raid_make_request);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 38)
	blk_queue_flush(raid->queue, REQ_FLUSH);
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 30)
	blk_queue_hardsect_size(raid->queue, PB_BLOCK_SIZE);
#else
	blk_queue_logical_block_size(raid->queue, PB_BLOCK_SIZE);
	blk_queue_physical_block_size(raid->queue, PB_BLOCK_SIZE);
	blk_queue_io_min(raid->queue, PB_BLOCK_SIZE);
	blk_queue_io_opt(raid->queue, PB_BLOCK_SIZE);

#ifdef TRIM_SUPPORT
#ifdef RHEL_RELEASE_CODE
	blk_queue_discard_granularity(raid->queue, PB_BLOCK_SIZE);
#endif
	queue_flag_set_unlocked(QUEUE_FLAG_DISCARD, raid->queue);
	blk_queue_max_discard_sectors(raid->queue, PB_BLOCK_SIZE / PB_SECTOR_SIZE);
#endif
#endif
	blk_queue_bounce_limit(raid->queue, BLK_BOUNCE_ANY);
	blk_queue_dma_alignment(raid->queue, 7);

	/*
	 * blk_queue_max_hw_segments Enables a low level driver to set an upper
	 * limit on the number of hw data segments in a request. This would be
	 * the largest number of address/length pairs the host adapter can
	 * actually give at once to the device.
	 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 34) && \
    (!defined(RHEL_RELEASE_CODE) || \
    RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(6, 0))
	blk_queue_max_hw_segments(raid->queue, MAX_DMA_LENGTH);
#else
	blk_queue_max_segments(raid->queue, MAX_DMA_LENGTH);
#endif

	/*
	 * blk_queue_max_segment_size Enables a low level driver to set an
	 * upper limit on the size of a coalesced segment
	 */
	blk_queue_max_segment_size(raid->queue, PB_BLOCK_SIZE);

	/*
	 * Care should be paid to sector length so that multi thread long
	 * requests are not intersected and lead to random write.
	 */
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 30)
	blk_queue_max_sectors(raid->queue, MAX_DMA_LENGTH << PB_B2S_SHIFT);
#else
	blk_queue_max_hw_sectors(raid->queue, MAX_DMA_LENGTH << PB_B2S_SHIFT);
#endif

	/* Create gendisk and set pblaze_raid_ops */
	raid->gendisk = alloc_disk(MEMDISK_MINORS);
	if (!raid->gendisk) {
		ret = -ENOMEM;
		goto failed_alloc_disk;
	}

	raid->gendisk->major = raid_drv.major;
	minor = pb_atomic_inc(&raid_drv.minor) - 1;
	raid->gendisk->first_minor = minor * MEMDISK_MINORS;
	raid->gendisk->minors = MEMDISK_MINORS;
	raid->gendisk->fops = &pblaze_raid_ops;
	raid->gendisk->queue = raid->queue;
	raid->gendisk->private_data = raid;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 28)
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, raid->gendisk->queue);
#endif
	snprintf(raid->gendisk->disk_name, 32, "memcon%c", minor + 'a');
	set_capacity(raid->gendisk, 0);
	add_disk(raid->gendisk);

	/* Register virtual device */
	ret = pblaze_virt_dev_register(raid, minor);
	if (ret < 0)
		goto failed_virt_dev_reg;

	DPRINTK(DEBUG, "raid add success\n");

	/* Set init flag, when raid online and update done, set ready */
	set_bit(PB_DRE_INITING, &raid->dre_flags);

	pblaze_raid_proc_init();

	/* start timer */
#ifdef HARDWARE_FAULTTOLERANT
	init_timer(&raid->io_timer_list);
	raid->io_timer_list.function = pblaze_raid_timer;
	raid->io_timer_list.data = (unsigned long)raid;
	raid->io_timer_list.expires = jiffies;
	add_timer(&raid->io_timer_list);
#endif

	/* Recalc the table at every device online event */
	raid->dev_tbl = NULL;

	return 0;

failed_virt_dev_reg:
	put_disk(raid->gendisk);
failed_alloc_disk:
	blk_cleanup_queue(raid->queue);
failed_alloc_queue:
	pblaze_set_unload(raid);
	pblaze_raid_reinit(raid);
	kref_put(&raid->refcn, release_pblaze_raid);

	return ret;
}

static void pblaze_raid_remove_begin(struct pblaze_raid *raid)
{
	/* Do the following things when the first pcie remove */
#ifdef HARDWARE_FAULTTOLERANT
	del_timer_sync(&raid->io_timer_list);
#endif
	pblaze_dre_exit(raid);
	pblaze_raid_proc_exit();
	pblaze_virt_dev_unregister(raid);
	pblaze_set_unload(raid);
	pblaze_raid_reinit(raid);

	/* After that, ioctl can't work */
	del_gendisk(raid->gendisk);
	put_disk(raid->gendisk);
	raid->gendisk = NULL;

	blk_cleanup_queue(raid->queue);

	vfree(raid->dev_tbl);
	raid->dev_tbl = NULL;
}

static void pblaze_raid_remove_end(struct pblaze_raid *raid)
{
	spin_lock(&raid_list_lock);
	list_del(&raid->node);
	spin_unlock(&raid_list_lock);

	kref_put(&raid->refcn, release_pblaze_raid);
	pb_atomic_sub(&raid_drv.driver_mem_size, sizeof(struct pblaze_raid));
}

int pblaze_raid_probe(struct pblaze_pcie *pcie, char *sn, u32 index, u32 num)
{
	int ret = 0;
	struct pblaze_raid *raid;

	raid = pblaze_raid_match(sn);
	if (!raid) {
		DPRINTK(NOTICE, "Create new raid device\n");

		raid = pblaze_raid_probe_one(pcie, sn, num);
		if (!raid) {
			ret = -ENOMEM;
			goto failed_raid_probe_one;
		}
		raid->pcie_child = pcie;
		raid->pcie_child_dev = &pcie->dev->dev;	/* dma map and unmap will use it */

		/* link into raid list */
		spin_lock(&raid_list_lock);
		list_add(&raid->node, &raid_list);
		spin_unlock(&raid_list_lock);
	}

	DPRINTK(NOTICE, "Before Link into raid, nr_dev:%u, nr_dev_max:%u\n", raid->nr_dev, raid->nr_dev_max);

	/* Link into raid, we should make sure the nr_dev updating is after pcie device setting */
	raid->pcie_devs[index] = pcie;
	pcie->raid = raid;
	mb();
	raid->nr_dev++;
	sema_init(&raid->ioctl_sem, 1);

	DPRINTK(NOTICE, "Link into raid, nr_dev:%u, nr_dev_max:%u\n", raid->nr_dev, raid->nr_dev_max);

	if (raid->nr_dev == raid->nr_dev_max) {
		int i;

		/* Make a little check */
		for (i = 0; i < raid->nr_dev_max; i++) {
			if (raid->pcie_devs[i] == NULL)
				BUG_ON(1);
		}

		ret = pblaze_raid_probe_two(raid);
		if (ret)
			goto failed_raid_probe_two;
	}

	return 0;

failed_raid_probe_two:
	pblaze_raid_remove_end(raid);
failed_raid_probe_one:
	return ret;
}

void pblaze_raid_remove(struct pblaze_pcie *pcie)
{
	struct pblaze_raid *raid = pcie->raid;

	DPRINTK(DEBUG, "raid remove\n");

	/* Do the following things when the first pcie remove */
	if (raid->nr_dev == raid->nr_dev_max)
		pblaze_raid_remove_begin(raid);

	raid->pcie_devs[pcie->sub_dev_index] = NULL;
	raid->nr_dev--;
}

void pblaze_raid_free(struct pblaze_pcie *pcie)
{
	struct pblaze_raid *raid = pcie->raid;

	/* Do the following things when the last pcie remove */
	if (raid->nr_dev == 0)
		pblaze_raid_remove_end(raid);
}

int pblaze_raid_update(struct pblaze_raid *raid);

static void pblaze_raid_prepare_online(struct pblaze_raid *raid)
{
	int ret = 0;

	if (raid->is_unload) {
		return;
	}

	ret = pblaze_raid_update(raid);
	if (ret) {
		DPRINTK(ERR, "Raid update information error\n");
		raid->status = ST_STAGE_STOP;
		return;
	}

	/* Calc mappding addr and dev table */
	ret = pblaze_get_mapping_table(raid);
	if (ret) {
		raid->is_tbl_ready = false;

		DPRINTK(ERR, "Raid get mapping table error\n");
		return;
	}

	raid->is_tbl_ready = true;

	/* Dre init at init status, allow dre thread ready to run */
	if (test_bit(PB_DRE_INITING, &raid->dre_flags)) {
		set_bit(PB_DRE_READY, &raid->dre_flags);
		clear_bit(PB_DRE_INITING, &raid->dre_flags);
		pblaze_dre_init(raid);
	}

	printk("Raid device is on running state\n");
}

static void pblaze_raid_check_status(struct pblaze_raid *raid)
{
	struct pblaze_pcie *pcie;
	u32 new_status;
	u32 i, j;

	new_status = raid->new_status;

	DPRINTK(INFO, "raid check status, raid->status 0x%x, new_status 0x%x\n",
			raid->status, raid->new_status);	
	/* Seemd the new_status check can be removed */
	if (raid->status == ST_STAGE_RUN && new_status != ST_STAGE_RUN)
		raid->status = ST_STAGE_STOP;

	/*
	 * Iter all pcie device
	 * status will be modified only in tasklet, so it don't need lock.
	 */
	for (j = 0; j < raid->nr_dev_max; j++) {
		pcie = raid->pcie_devs[j];
		for (i = COMM_DEV_START; i != COMM_DEV_END; i++) {
			if (pcie->pcd[i].status != new_status)
				goto special_handle;
		}
	}

	if (new_status == ST_STAGE_RUN) {
		pblaze_raid_prepare_online(raid);
	}

	raid->status = new_status;
	return;

special_handle:
	for (j = 0; j < raid->nr_dev_max; j++) {
		pcie = raid->pcie_devs[j];
		for (i = COMM_DEV_START; i != COMM_DEV_END; i++) {
			if (pcie->pcd[i].status > ST_STAGE_READY)
				return;
		}
	}

	/* All status are ready or init means reinit can finish */
	raid->status = ST_STAGE_INIT;
}

/* Wait pblaze_raid status */
long pblaze_raid_wait_status(struct pblaze_raid *raid, u32 stat, long timeout)
{
	long ret;

	if (!raid)
		return -1;

	ret = wait_event_interruptible_timeout(raid->stat_wq,
				raid->status == stat, timeout);

	if (ret < 0) {
		return ret;
	} else if (ret == 0) {
		set_bit(PB_CRITICAL_STATUS_TIMEOUT, &raid->error_code);
		return -ETIMEDOUT;
	}

	return 0;
}

/* Wait pblaze_comm_dev status*/
long pblaze_raid_wait_pcd_status(struct pblaze_raid *raid, struct pblaze_comm_dev *pcd,
				 u32 stat, long timeout)
{
	long ret;

	if (!raid)
		return -1;

	ret = wait_event_interruptible_timeout(raid->stat_wq,
				pcd->status == stat, timeout);

	if (ret < 0)
		return ret;
	else if (ret == 0)
		return -ETIMEDOUT;

	return 0;
}


#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
void pblaze_raid_status_change_worker(void *data)
#else
void pblaze_raid_status_change_worker(struct work_struct *work)
#endif
{
	struct pblaze_raid *raid;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
	raid = (struct pblaze_raid *)data;
#else
	raid = container_of(work, struct pblaze_raid, stat_work);
#endif

	pblaze_raid_check_status(raid);

	if (waitqueue_active(&raid->stat_wq)) {
		DPRINTK(DEBUG, "worker: wake up wait queue\n");
		wake_up_interruptible(&raid->stat_wq);
	}
}


s16 pblaze_read_pcb_temperature(struct pblaze_raid *raid)
{
	return (s16)(((readl(raid->pcie_child->regs + REG_PCB_TEMPERATURE) & PCB_TEMPERATURE_MASK)) << 3);
}

u32 pblaze_read_xadc_value(struct pblaze_raid *raid, int which)
{
	u32 value;

	writel(which << XADC_WRITE_SHIFT, raid->pcie_child->regs + REG_XADC);

	do {
		value = readl(raid->pcie_child->regs + REG_XADC);
	} while (((value & XADC_WRITE_MASK) >> XADC_WRITE_SHIFT) != which);

	return readl(raid->pcie_child->regs + REG_XADC) & XADC_READ_MASK;
}

void pblaze_ioctl_req_init(struct pblaze_ioctl_request *ioctl_request,
			   struct pblaze_cmd *ioctl_cmd, u32 major_cmd,
			   u32 cmd_seq_len, void *data_in, void** data_out,
			   u32 pending_dev)
{
	int i;
	bool is_write;

	is_write = IS_CMDTODEV(major_cmd);
	ioctl_request->major_cmd = major_cmd;
	ioctl_request->len = cmd_seq_len / sizeof(u16);
	ioctl_request->is_write = is_write;
	init_completion(&ioctl_request->ioctl_comp);
	ioctl_request->pending_dev = pending_dev;
	ioctl_request->request_status = CMD_ECHO_SUCCEED;

	for (i = 0; i != pending_dev; ++i) {
		if (is_write) {
			ioctl_cmd[i].data = data_in;
		} else if (data_out != NULL) {
			ioctl_cmd[i].data = data_out[i];
		}
		ioctl_cmd[i].ioctl_req = ioctl_request;
	}
}

/*
 * One of the raid ioctl cmd implement function. It will call pblaze_comm_noimm
 * to communicate with the cpu devie of pcie. most of the time, These functions
 * will be blocked until the required cpu devices have handled the cmd. Things
 * need be comfirmed is the cmd relates with how many cpu devices.
 *
 * refer to 3.2.9
 */
int pblaze_raid_lock_rom(struct pblaze_raid *raid, u32 dev_idx)
{
	struct pblaze_ioctl_request ioctl_request;
	struct pblaze_cmd ioctl_cmd;

	if (dev_idx >= raid->nr_dev_max) {
		return -CMD_ECHO_INVALID_DEVICE;
	}

	pblaze_ioctl_req_init(&ioctl_request, &ioctl_cmd, CMD_LOCKROM,
			      0, NULL, NULL, 1);

	/* send CMD_LOCKROM to pcie device 0 cpu 1 */
	pblaze_comm_noimm(&ioctl_cmd, raid->pcie_devs[dev_idx], 1u);
	wait_for_completion(&ioctl_request.ioctl_comp);

	return ioctl_request.request_status;
}

int pblaze_raid_send_rom(struct pblaze_raid *raid, u32 size,
			 char *data, u32 dev_idx)
{
	struct pblaze_ioctl_request ioctl_request;
	struct pblaze_cmd ioctl_cmd;

	if (dev_idx >= raid->nr_dev_max) {
		return -CMD_ECHO_INVALID_DEVICE;
	}

	pblaze_ioctl_req_init(&ioctl_request, &ioctl_cmd, CMD_WRITEROM,
			      size, data, NULL, 1);

	/* send CMD_BAECON to pcie device 0 cpu 1 */
	pblaze_comm_noimm(&ioctl_cmd, raid->pcie_devs[dev_idx], 1u);
	wait_for_completion(&ioctl_request.ioctl_comp);

	return ioctl_request.request_status;
}

static char pblaze_get_lockrom(struct pblaze_raid *raid)
{
	return raid->init_monitor.rom[PB_LOCKROM_OFFSET];
}

int pblaze_raid_recv_monitor(struct pblaze_raid *raid,
			      struct pblaze_ioctl_monitor *monitor)
{
	u32 i;
	u32 pending_dev;
	u32 flying_pipe_count;
	u32 cur_latency_jiffies;
	u32 nxt_latency_jiffies;
	struct pblaze_ioctl_request ioctl_request;
	struct pblaze_cmd ioctl_cmd[NR_DEVICE * NR_CPU];
	struct pblaze_monitor *monitor_ptrs[NR_DEVICE * NR_CPU];

	pending_dev = raid->nr_dev_max * NR_CPU;
	for (i = 0; i != pending_dev; ++i) {
		monitor_ptrs[i] = &raid->pcie_devs[i / NR_CPU]->pcd[i % NR_CPU].monitor;
	}

	pblaze_ioctl_req_init(&ioctl_request, ioctl_cmd, CMD_READMON,
			      sizeof(struct pblaze_monitor), NULL, (void **)monitor_ptrs, pending_dev);

	for (i = 0; i != pending_dev; ++i) {
		pblaze_comm_noimm(&ioctl_cmd[i], raid->pcie_devs[i / NR_CPU], i % NR_CPU);
	}

	wait_for_completion(&ioctl_request.ioctl_comp);

	memset(monitor, 0, sizeof(struct pblaze_ioctl_monitor));

	monitor->board_temperature_min = 0x4fff;
	for (i = 0; i != pending_dev; ++i) {
		monitor->logical_read += monitor_ptrs[i]->logical_read;
		monitor->logical_write += monitor_ptrs[i]->logical_write;
		monitor->flash_read += monitor_ptrs[i]->flash_read;
		monitor->flash_write += monitor_ptrs[i]->flash_write;
		monitor->dead_block += monitor_ptrs[i]->dead_block;
		monitor->dead_die |= monitor_ptrs[i]->dead_die;
		monitor->init_count = monitor_ptrs[i]->init_count;
		monitor->start_count = monitor_ptrs[i]->start_count;
		monitor->tetris_num += monitor_ptrs[i]->tetris_size_in_system;
		monitor->temperature_unsafe |= monitor_ptrs[i]->over_heating;
		monitor->comm_status[i] = raid->pcie_devs[i / NR_CPU]->pcd[i % NR_CPU].status;
		if (monitor->board_temperature_max < monitor_ptrs[i]->boardtemp_max)
			monitor->board_temperature_max = monitor_ptrs[i]->boardtemp_max;
		if (monitor->board_temperature_min > monitor_ptrs[i]->boardtemp_min)
			monitor->board_temperature_min = monitor_ptrs[i]->boardtemp_min;
	}

	monitor->current_tick = pb_timestamp();
	monitor->read_iops = raid->read_io;
	monitor->read_size = raid->read_size;
	monitor->write_iops = raid->write_io;
	monitor->write_size = raid->write_size;
	monitor->board_temperature = pblaze_read_pcb_temperature(raid);			/* board temperature */
	monitor->temperature = pblaze_read_xadc_value(raid, XADC_TEMP_CHN);		/* core */
	monitor->temperature_max = pblaze_read_xadc_value(raid, XADC_MAX_TEMP_CHN);	/* core max */
	monitor->temperature_min = pblaze_read_xadc_value(raid, XADC_MIN_TEMP_CHN);	/* core min */
	monitor->vcore = pblaze_read_xadc_value(raid, XADC_INT_CHN);
	monitor->vcore_max = pblaze_read_xadc_value(raid, XADC_MAX_INT_CHN);
	monitor->vcore_min = pblaze_read_xadc_value(raid, XADC_MIN_INT_CHN);
	monitor->vpll = pblaze_read_xadc_value(raid, XADC_VCCAUX_CHN);
	monitor->vpll_max = pblaze_read_xadc_value(raid, XADC_MAX_VCCAUX_CHN);
	monitor->vpll_min = pblaze_read_xadc_value(raid, XADC_MIN_VCCAUX_CHN);
	monitor->lock_rom = (u16)pblaze_get_lockrom(raid);
	monitor->vddr = pblaze_read_xadc_value(raid, XADC_VDDR3_CHN);			/* reserved for read only flag */
	monitor->disk_size_MB = raid->disk_size_MB;
	monitor->link_width = raid->pcie_child->link_width;
	monitor->link_gen = raid->pcie_child->link_gen;
	monitor->driver_mem_size = raid_drv.driver_mem_size;
	flying_pipe_count = raid->flying_pipe_count;
	monitor->flying_pipe_percent = (flying_pipe_count == 0) ? 0 :
					100 * raid->flying_pipe_size /
					(flying_pipe_count * raid->nr_dev * INTERRUPT_QUEUE_SIZE);

	/* load latency monitor and refresh stock latency monitor if needed */
	memcpy(monitor->latency_monitor, raid->latency_monitor,
	       sizeof(struct pblaze_latency_monitor) * LATENCY_COUNTER);
	cur_latency_jiffies = raid->latency_jiffies;
	nxt_latency_jiffies = jiffies;

	if (nxt_latency_jiffies - cur_latency_jiffies > 2 * HZ &&
		pb_atomic_cmpxchg(&raid->latency_jiffies, cur_latency_jiffies, nxt_latency_jiffies) == cur_latency_jiffies) {
		memset(raid->latency_monitor, 0,
		       sizeof(struct pblaze_latency_monitor) * LATENCY_COUNTER);
		raid->flying_pipe_count = 0;
		raid->flying_pipe_size = 0;
	}

	monitor->error_code = raid->error_code;
	return 0;
}

int pblaze_raid_update(struct pblaze_raid *raid)
{
	u32 i, j, k;
	u32 size = 0;
	u16 num = 0;
	u32 pending_dev = 0;
	char *rom;
	struct pblaze_ioctl_request ioctl_request;
	struct pblaze_cmd ioctl_cmd[NR_DEVICE * NR_CPU];
	void *monitor_pointer[NR_DEVICE * NR_CPU];
	bool read_only = false;
	u8 tetris_num[NR_DEVICE * NR_CPU];
	u16 dead_die[NR_DEVICE * NR_CPU];
	char ts[NR_DEVICE * NR_CPU][24];

	/* FIXME: may need to use pcie monitor */
	struct pblaze_monitor monitor[NR_DEVICE * NR_CPU];

	pending_dev = raid->nr_dev_max * NR_CPU;
	for (i = 0; i != pending_dev; ++i) {
		monitor_pointer[i] = &monitor[i];
	}

	pblaze_ioctl_req_init(&ioctl_request, ioctl_cmd, CMD_READMON,
			      sizeof(struct pblaze_monitor), NULL, monitor_pointer, pending_dev);

	for (i = 0; i != pending_dev; ++i)
		pblaze_comm_noimm(&ioctl_cmd[i], raid->pcie_devs[i / NR_CPU], i % NR_CPU);

	wait_for_completion_interruptible(&ioctl_request.ioctl_comp);

	if (ioctl_request.request_status != CMD_ECHO_SUCCEED) {
		DPRINTK(WARNING, "read monitor failed\n");
		return -1;
	}

	/* Only calc mapping table will need it, seemd reset to 0 is safe */
	for (i = 0; i != raid->nr_dev_max; i++)
		raid->tetris_num[i] = 0;

	for (i = 0; i != pending_dev; ++i) {
		size += monitor[i].lsa_limit_in_tetris * monitor[i].tetris_size_in_system;
		tetris_num[i] = monitor[i].tetris_size_in_system;
		raid->tetris_num[i / NR_CPU] += tetris_num[i];

		dead_die[i] = monitor[i].dead_die;
		if (dead_die[i] > 0)
			read_only = true;
	}

	for (i = 0; i != raid->nr_dev_max; i++) {
		num += raid->tetris_num[i];
		DPRINTK(NOTICE, "dev %u has tetris number %u\n", i, raid->tetris_num[i]);
	}

	raid->disk_size_MB = (size >> PB_B2MB_SHIFT);
	DPRINTK(NOTICE, "raid disk_size_MB:%u\n", raid->disk_size_MB);

	raid->tetris_total_num = num;

	raid->disk_capacity_MB = num;
	rom = raid->init_monitor.rom;
	raid->disk_capacity_MB *= simple_strtoul(pb_get_str_by_key(rom, "MaxAddressedBlockSizeInTetris:"), NULL, 10);
	raid->disk_capacity_MB *= NR_LUN_PER_TETRIS;
	raid->disk_capacity_MB *= simple_strtoul(pb_get_str_by_key(rom, "FlashBlockSize:"), NULL, 10);
	raid->disk_capacity_MB *= simple_strtoul(pb_get_str_by_key(rom, "FlashPageSize:"), NULL, 10);
	raid->disk_capacity_MB *= simple_strtoul(pb_get_str_by_key(rom, "FlashPlaneSize:"), NULL, 10);
	raid->disk_capacity_MB *= simple_strtoul(pb_get_str_by_key(rom, "FlashSectorSize:"), NULL, 10);
	raid->disk_capacity_MB >>= 20;
	DPRINTK(NOTICE, "raid disk_capacity_MB:%lld\n", raid->disk_capacity_MB);

	if (raid->disk_capacity_MB == 0)
		raid->disk_capacity_MB = raid->disk_size_MB;

	if (read_only) {
#ifdef RDONLY_CHECK
		set_bit(DEV_RDONLY, &raid->flags);
#endif
		for (i = 0; i != pending_dev; ++i) {
			k = 0;
			for (j = 0; (j < tetris_num[i]) && (j < 24); j++) {
				if (dead_die[i] & (1 << j)) {
					ts[i][j] = '_';
					k++;
				} else {
					ts[i][j] = 'Y';
				}
			}

			ts[i][j] = '\n';
			DPRINTK(WARNING, "Pcie:%u, pcd:%u, tetris stat [%d/%u]:%s\n",
				(i / NR_CPU), (i % NR_CPU), k, tetris_num[i], ts[i]);
		}

		DPRINTK(WARNING, "raid:%s becomes read only\n", raid->gendisk->disk_name);
	}

	return 0;
}

void pblaze_raid_reinit(struct pblaze_raid *raid)
{
	long timeout = 0;
	u32 i, j;
	u32 lsa_limit = 0;
	u32 pending_dev = 0;
	struct pblaze_ioctl_request ioctl_request;
	struct pblaze_cmd ioctl_cmd[NR_DEVICE * NR_CPU];

#ifdef HARDWARE_FAULTTOLERANT
	if (test_bit(DEV_FAULT, &raid->flags)) {
		DPRINTK(ALERT, "Hardware fault, close disk is skipped\n");
			return;
	}
#endif

	/* Iter all the cpu devices, check whether it need wait to run */
	for (i = 0; i != raid->nr_dev_max; i++)
		for (j = COMM_DEV_START; j != COMM_DEV_END; j++)
			if (raid->pcie_devs[i]->pcd[j].status == ST_STAGE_MID) {
				DPRINTK(NOTICE, "pcie:%u, pcd:%u is ST_STAGE_MID\n", i, j);
				pblaze_raid_wait_pcd_status(raid, &raid->pcie_devs[i]->pcd[j],
							    ST_STAGE_RUN, HZ * 600);
			}

	/* Iter all the cpu devices, check whether it need restart */
	for (i = 0; i != raid->nr_dev_max; i++)
		for (j = COMM_DEV_START; j != COMM_DEV_END; j++)
			if (raid->pcie_devs[i]->pcd[j].status == ST_STAGE_RUN)
				pending_dev++;

	if (pending_dev == 0) {
		DPRINTK(NOTICE, "Skip reinit\n");
		return;
	}

	pblaze_ioctl_req_init(&ioctl_request, ioctl_cmd, CMD_REINIT,
			      (sizeof(u32)), (u16 *)&lsa_limit, NULL, pending_dev);

	pending_dev = 0;
	for (i = 0; i != raid->nr_dev_max; i++)
		for (j = COMM_DEV_START; j != COMM_DEV_END; j++)
			if (raid->pcie_devs[i]->pcd[j].status == ST_STAGE_RUN) {
				pblaze_comm_noimm(&ioctl_cmd[pending_dev], raid->pcie_devs[i], j);
				pending_dev++;
			}

	wait_for_completion(&ioctl_request.ioctl_comp);

	if (ioctl_request.request_status != CMD_ECHO_SUCCEED)
		DPRINTK(ERR, "reinit work failed\n");

	timeout = pblaze_raid_wait_status(raid, ST_STAGE_INIT, HZ * 600);
	if (timeout) {
		DPRINTK(ERR, "wait for status change timeout\n");
	}
}

void pblaze_raid_beacon(struct pblaze_raid *raid, bool is_baecon)
{
	u16 baecon_value;
	struct pblaze_ioctl_request ioctl_request;
	struct pblaze_cmd ioctl_cmd;

	baecon_value = is_baecon;
	pblaze_ioctl_req_init(&ioctl_request, &ioctl_cmd, CMD_BAECON,
			      sizeof(u16), &baecon_value, NULL, 1);

	/* send CMD_BAECON to pcie device 0 cpu 1 */
	pblaze_comm_noimm(&ioctl_cmd, raid->pcie_devs[0], 1u);
	wait_for_completion(&ioctl_request.ioctl_comp);
}

int pblaze_raid_send_fw(struct pblaze_raid *raid, u32 size, char *data)
{
	u32 i;
	struct pblaze_ioctl_request ioctl_request;
	struct pblaze_cmd ioctl_cmd[NR_DEVICE];

	pblaze_dre_exit(raid);
	raid->dre_flags = 0;
	pblaze_ioctl_req_init(&ioctl_request, ioctl_cmd, CMD_WRITEFIRMWARE,
			      size, data, NULL, raid->nr_dev_max);

	for (i = 0; i != raid->nr_dev_max; ++i) {
		pblaze_comm_noimm(&ioctl_cmd[i], raid->pcie_devs[i], 0u);
	}

	wait_for_completion(&ioctl_request.ioctl_comp);

	return ioctl_request.request_status;
}

int pblaze_raid_safe_erase(struct pblaze_raid *raid, u32 per_tetris_size_page)
{
	u32 i;
	u32 pending_dev;
	struct pblaze_ioctl_request ioctl_request;
	struct pblaze_cmd ioctl_cmd[NR_DEVICE * NR_CPU];

	pblaze_dre_exit(raid);
	raid->dre_flags = 0;
	set_bit(PB_DRE_INITING, &raid->dre_flags);

	pending_dev = raid->nr_dev_max * NR_CPU;
	pblaze_ioctl_req_init(&ioctl_request, ioctl_cmd, CMD_REINIT, sizeof(u32),
			      &per_tetris_size_page, NULL, pending_dev);

	for (i = 0; i != pending_dev; ++i) {
		pblaze_comm_noimm(&ioctl_cmd[i],
				  raid->pcie_devs[i / NR_CPU], i % NR_CPU);
	}

	wait_for_completion(&ioctl_request.ioctl_comp);

	/* At this moment, the raid status may be RUN still */
	raid->status = ST_STAGE_STOP;

	return ioctl_request.request_status;
}

int pblaze_raid_attach(struct pblaze_raid *raid)
{
	int ret;

	down(&raid_drv.sem);
	if (raid->virt_dev->is_attached) {
		ret = -EEXIST;
		} else {
		ret = device_attach(&raid->virt_dev->dev);
		if (ret == 0)
			ret = -EINVAL;
		else if (ret == 1)
			ret = 0;
	}
	up(&raid_drv.sem);

	return ret;
}

int pblaze_raid_detach(struct pblaze_raid *raid)
{
	int ret = 0;

	down(&raid_drv.sem);
	if (!raid->virt_dev->is_attached)
		ret = -EEXIST;
	else if (raid->virt_dev->open_handles)
		ret = -EBUSY;
	else {
		device_release_driver(&raid->virt_dev->dev);
	}
	up(&raid_drv.sem);

	return ret;
}

static void pblaze_dre_read_idx(struct pblaze_raid *raid)
{
	int is_insert;
	struct io_request *ioreq = raid->dre_log_req;
	struct bio *bio = ioreq->bio;
	struct page *page = bio->bi_io_vec[0].bv_page;
	unsigned long *addr;

	ioreq->ioreq_start = raid->dre_log_idx;
	pb_bio_start(bio) = ioreq->ioreq_start << PB_B2S_SHIFT;
	bio->bi_rw = 0;

	is_insert = pblaze_raid_dpc_dre_ioreq(raid, ioreq, true);
	if (is_insert == 1)
		wait_for_completion_interruptible(&raid->dre_completion);

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 4, 1)
	addr = (unsigned long *)kmap_atomic(page, KM_USER0);
#else
	addr = (unsigned long *)kmap_atomic(page);
#endif
	if (*addr == PB_DRE_ECC) {
		raid->dre_idx = *(u32 *)(++addr);
		--addr;
	} else {
		raid->dre_idx = 0;
	}
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 4, 1)
	kunmap_atomic(addr, KM_USER0);
#else
	kunmap_atomic(addr);
#endif
	DPRINTK(NOTICE, "dre_idx:%u\n", raid->dre_idx);
}

static void pblaze_dre_write_idx(struct pblaze_raid *raid)
{
	int is_insert;
	struct io_request *ioreq = raid->dre_log_req;
	struct bio *bio = ioreq->bio;
	struct page *page = bio->bi_io_vec[0].bv_page;
	unsigned long *addr;

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 4, 1)
	addr = (unsigned long *)kmap_atomic(page, KM_USER0);
#else
	addr = (unsigned long *)kmap_atomic(page);
#endif
	*addr = PB_DRE_ECC;
	*(u32 *)(++addr) = raid->dre_idx;
	--addr;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 4, 1)
	kunmap_atomic(addr, KM_USER0);
#else
	kunmap_atomic(addr);
#endif

	/* pblaze_dre_alloc_req already set the io length */
	ioreq->ioreq_start = raid->dre_log_idx;
	pb_bio_start(bio) = ioreq->ioreq_start << PB_B2S_SHIFT;
	bio->bi_rw |= MEM_BIO_RW;

	is_insert = pblaze_raid_dpc_dre_ioreq(raid, ioreq, true);
	if (is_insert == 1) {
		wait_for_completion_interruptible(&raid->dre_completion);
		DPRINTK(INFO, "wait write index done\n");
	}
}

static void pblaze_dre_issue_req(struct pblaze_raid *raid, int rw)
{
	int is_insert;
	struct io_request *ioreq = raid->dre_req;
	struct bio *bio = ioreq->bio;

	/* pblaze_dre_alloc_req already set the io length */
	ioreq->ioreq_start = raid->dre_idx;
	pb_bio_start(bio) = ioreq->ioreq_start << PB_B2S_SHIFT;
	if (rw)
		bio->bi_rw |= MEM_BIO_RW;
	else
		bio->bi_rw = 0;

	is_insert = pblaze_raid_dpc_dre_ioreq(raid, ioreq, false);
	if (is_insert == 1) {
		wait_for_completion_interruptible(&raid->dre_completion);
		DPRINTK(INFO, "wait normal dre done, rw:%d, idx:%u\n",
			rw, raid->dre_idx);
	} else {
		DPRINTK(NOTICE, "dre insert failed cuz write conflict\n");
	}
}

static void pblaze_calc_dre_timeout(struct pblaze_raid *raid)
{
	u32 msec;
	unsigned long timeout;

	msec = (PB_DRE_PERIOD * PB_DAY_PER_M * PB_HOUR_PER_D * PB_MIN_PER_H * PB_SEC_PER_M);
	msec = msec >> PB_B2MB_SHIFT;
	msec = msec * PB_MSEC_PER_S;
	msec = msec / raid->disk_size_MB;
	timeout = msecs_to_jiffies(msec);
	raid->dre_timeout = timeout;

	DPRINTK(NOTICE, "msec:%u timeout:%lu\n", msec, timeout);
}

static void pblaze_do_dre_one(struct pblaze_raid *raid)
{

	if (test_bit(DEV_RDONLY, &raid->flags) || 
		test_bit(DEV_FAULT, &raid->flags)) {
		return;
	}

	/* If PB_DRE_READY set, raid is online and raid info is updated */
	if (test_bit(PB_DRE_READY, &raid->dre_flags)) {
		raid->dre_max_idx = (raid->disk_size_MB << PB_B2MB_SHIFT) - 1;
		raid->dre_log_idx = raid->dre_max_idx;
		pblaze_calc_dre_timeout(raid);
		raid->dre_log_jiffies = jiffies;
		pblaze_dre_read_idx(raid);

		clear_bit(PB_DRE_READY, &raid->dre_flags);
		set_bit(PB_DRE_WORKING, &raid->dre_flags);

		DPRINTK(NOTICE, "read log idx done, begin dre\n");
	} else if (test_bit(PB_DRE_WORKING, &raid->dre_flags)) {
		pblaze_dre_issue_req(raid, 0);
		pblaze_dre_issue_req(raid, 1);

		++raid->dre_idx;
		raid->dre_idx %= raid->dre_max_idx;

		if (time_after_eq(jiffies, raid->dre_log_jiffies + PB_DRE_LOG_PERIOD)) {
			raid->dre_log_jiffies = jiffies;
			pblaze_dre_write_idx(raid);

			DPRINTK(NOTICE, "write log idx done:%u\n", raid->dre_idx);
		}
	}
}


static int pblaze_dre_thread(void *arg)
{
	struct pblaze_raid *raid = (struct pblaze_raid *)arg;

	allow_signal(SIGKILL);
	while (!kthread_should_stop()) {
		if (signal_pending(current))
			flush_signals(current);

		pblaze_do_dre_one(raid);

		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(raid->dre_timeout);
	}

	complete(&raid->dre_exit_completion);

	return 0;
}


static void pblaze_dre_complete_req(struct io_request *dre_ioreq, int error)
{
	int i;
	struct pblaze_raid *raid = (struct pblaze_raid *)dre_ioreq->bio->bi_private;

	for (i = 0; i < raid->nr_dev; i++)
		dre_ioreq->privs[i].len = 0;

	complete(&raid->dre_completion);
}

static struct io_request *pblaze_dre_alloc_req(struct pblaze_raid *raid)
{
	struct bio *bio;
	struct io_request *dre_ioreq;
	struct page *page;

	bio = bio_alloc(GFP_KERNEL, 1);
	if (bio == NULL)
		goto failed_out0;

	page = alloc_page(GFP_KERNEL | GFP_DMA);
	if (page == NULL)
		goto failed_out1;

	dre_ioreq = kmem_cache_alloc(pblaze_get_ioreq_slab(), GFP_KERNEL);
	if (dre_ioreq == NULL)
		goto failed_out3;

	bio->bi_io_vec[0].bv_page = page;
	bio->bi_io_vec[0].bv_offset = 0;
	bio->bi_io_vec[0].bv_len = PAGE_SIZE;
	pb_bio_start(bio) = PAGE_SIZE;
	bio->bi_vcnt = 1;
	bio->bi_private = raid;
	bio->bi_end_io = NULL;
	memset(dre_ioreq, 0, sizeof(struct io_request));
	dre_ioreq->ioreq_len = 1;
	dre_ioreq->bio = bio;
	dre_ioreq->complete_ioreq = pblaze_dre_complete_req;

	return dre_ioreq;

failed_out3:
	__free_page(page);
failed_out1:
	bio_put(bio);
failed_out0:
	return NULL;
}

static void pblaze_dre_release_req(struct io_request *ioreq)
{
	__free_page(ioreq->bio->bi_io_vec[0].bv_page);
	bio_put(ioreq->bio);
	kmem_cache_free(pblaze_get_ioreq_slab(), ioreq);
}

int pblaze_dre_init(struct pblaze_raid *raid)
{
	struct io_request *dre_req, *dre_log_req;

	DPRINTK(DEBUG, "++\n");
	dre_req = pblaze_dre_alloc_req(raid);
	if (dre_req == NULL)
		return -ENOMEM;

	dre_log_req = pblaze_dre_alloc_req(raid);
	if (dre_log_req == NULL) {
		pblaze_dre_release_req(dre_req);
		return -ENOMEM;
	}

	dre_req->is_dre = true;
	dre_log_req->is_dre = true;
	raid->dre_req = dre_req;
	raid->dre_log_req = dre_log_req;
	raid->dre_timeout = HZ;
	init_completion(&raid->dre_completion);

	raid->dre_thread = kthread_run(pblaze_dre_thread, raid, "pb_dre");
	if (IS_ERR(raid->dre_thread)) {
		DPRINTK(ERR, "register dre work thread error\n");

		pblaze_dre_release_req(raid->dre_req);
		raid->dre_req = NULL;
		pblaze_dre_release_req(raid->dre_log_req);
		raid->dre_log_req = NULL;

		return PTR_ERR(raid->dre_thread);
	}

	return 0;
}

void pblaze_dre_exit(struct pblaze_raid *raid)
{
	if (raid->dre_thread) {
		kthread_stop(raid->dre_thread);
		raid->dre_thread = NULL;
		wait_for_completion(&raid->dre_exit_completion);
	}

	if (raid->dre_req) {
		pblaze_dre_release_req(raid->dre_req);
		raid->dre_req = NULL;
	}

	if (raid->dre_log_req) {
		pblaze_dre_release_req(raid->dre_log_req);
		raid->dre_log_req = NULL;
	}

	DPRINTK(DEBUG, "--\n");
}

