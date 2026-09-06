
#include "pblaze_hal.h"

void pb_ioreq_complete_io(struct io_request *ioreq, int error)
{
	ioreq->complete_ioreq(ioreq, error);
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 25)) && (LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 28))
void part_round_stats(struct hd_struct *part)
{
	unsigned long now = jiffies;

	if (now == part->stamp)
		return;

	if (part->in_flight) {
		__part_stat_add(part, time_in_queue,
				part->in_flight * (now - part->stamp));
		__part_stat_add(part, io_ticks, (now - part->stamp));
	}
	part->stamp = now;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 25)
void pb_acct_stats(struct gendisk *disk, struct io_request *ioreq)
{
	preempt_disable();
	disk_round_stats(disk);
	atomic_inc((atomic_t *)&(disk->in_flight));
	preempt_enable();
}
#elif LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 28)
void pb_acct_stats(struct gendisk *disk, struct io_request *ioreq)
{
	struct hd_struct *part;
	int is_write = pb_is_write_bio(ioreq->bio);
	int cpu;
	struct hd_struct *part;

	preempt_disable();
	disk_round_stats(disk);
	atomic_inc((atomic_t *)&(disk->in_flight));

	part = get_part(disk, ioreq->ioreq_start << PB_B2S_SHIFT);
	if (part) {
		part_round_stats(part);
		atomic_inc((atomic_t *)&(part->in_flight));
	}
	preempt_enable();
}
#else
static inline void pb_part_inc_in_flight(struct hd_struct *part, int rw)
{
	atomic_inc((atomic_t *)&(part->in_flight[rw]));
	if (part->partno)
		atomic_inc((atomic_t *)&(part_to_disk(part)->part0.in_flight[rw]));
}

void pb_acct_stats(struct gendisk *disk, struct io_request *ioreq)
{
	struct hd_struct *part;
	int is_write = pb_is_write_bio(ioreq->bio);
	int cpu;

	cpu = part_stat_lock();
	part = disk_map_sector_rcu(disk, pb_get_ioreq_start(ioreq));
	part_round_stats(cpu, part);
	pb_part_inc_in_flight(part, is_write);
	part_stat_unlock();
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 15)
void pb_acct_io_done(struct gendisk *disk, struct io_request *ioreq, int error)
{
	struct hd_struct *part;
	bool is_has_data, is_write;
	u32 duration;
	sector_t len;

	is_has_data = pb_is_has_data_bio(ioreq->bio);
	is_write = pb_is_write_bio(ioreq->bio);
	duration = jiffies - ioreq->submit_jiffies;
	len = pb_get_ioreq_len(ioreq);

	preempt_disable();
	if (!error && is_has_data) {
		/* Cuz of the spinlock, don't need part_round_stats to disable preempt */
		if (is_write) {
			__disk_stat_add(disk, write_sectors, len);
			__disk_stat_inc(disk, writes);
			__disk_stat_add(disk, write_ticks, duration);
		} else {
			__disk_stat_add(disk, read_sectors, len);
			__disk_stat_inc(disk, reads);
			__disk_stat_add(disk, read_ticks, duration);
		}
	}

	disk_round_stats(disk);
	atomic_dec((atomic_t *)(&disk->in_flight));
	preempt_enable();
}
#elif LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 25)
void pb_acct_io_done(struct gendisk *disk, struct io_request *ioreq, int error)
{
	bool is_has_data, is_write;
	u32 duration;
	sector_t len;

	is_has_data = pb_is_has_data_bio(ioreq->bio);
	is_write = pb_is_write_bio(ioreq->bio);
	duration = jiffies - ioreq->submit_jiffies;
	len = pb_get_ioreq_len(ioreq);

	preempt_disable();
	if (!error && is_has_data) {
		__disk_stat_add(disk, sectors[is_write], len);
		__disk_stat_inc(disk, ios[is_write]);
		__disk_stat_add(disk, ticks[is_write], duration);
	}

	disk_round_stats(disk);
	atomic_dec((atomic_t *)(&disk->in_flight));
	preempt_enable();
}
#elif LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 28)
void pb_acct_io_done(struct gendisk *disk, struct io_request *ioreq, int error)
{
	struct hd_struct *part;
	bool is_has_data, is_write;
	u32 duration;
	sector_t len;

	is_has_data = pb_is_has_data_bio(ioreq->bio);
	is_write = pb_is_write_bio(ioreq->bio);
	duration = jiffies - ioreq->submit_jiffies;
	len = pb_get_ioreq_len(ioreq);

	preempt_disable();
	if (!error && is_has_data) {
		__disk_stat_add(disk, sectors[is_write], len);
		__disk_stat_inc(disk, ios[is_write]);
		__disk_stat_add(disk, ticks[is_write], duration);
	}

	disk_round_stats(disk);
	atomic_dec((atomic_t *)&(disk->in_flight));
	part = get_part(disk, ioreq->ioreq_start << PB_B2S_SHIFT);
	if (part) {
		part_round_stats(part);
		atomic_dec((atomic_t *)&(part->in_flight));
	}
	preempt_enable();
}
#else
static inline void pb_part_dec_in_flight(struct hd_struct *part, int rw)
{
	atomic_dec((atomic_t *)&(part->in_flight[rw]));
	if (part->partno)
		atomic_dec((atomic_t *)&(part_to_disk(part)->part0.in_flight[rw]));
}

void pb_acct_io_done(struct gendisk *disk, struct io_request *ioreq, int error)
{
	bool is_has_data, is_write;
	struct hd_struct *part;
	u32 duration;
	sector_t len;
	int cpu;

	is_has_data = pb_is_has_data_bio(ioreq->bio);
	is_write = pb_is_write_bio(ioreq->bio);
	duration = jiffies - ioreq->submit_jiffies;
	len = pb_get_ioreq_len(ioreq);

	cpu = part_stat_lock();
	part = disk_map_sector_rcu(disk, pb_get_ioreq_start(ioreq));
	if (!error && is_has_data) {
		part_stat_add(cpu, part, sectors[is_write], len);
		part_stat_inc(cpu, part, ios[is_write]);
		part_stat_add(cpu, part, ticks[is_write], duration);
	}

	part_round_stats(cpu, part);
	preempt_disable();
	pb_part_dec_in_flight(part, is_write);
	preempt_enable();
	part_stat_unlock();
}
#endif

/* Porting from linux kernel 2.6.34 */
u32 pb_gcd(u32 a, u32 b)
{
	u32 r;

	if (a < b)
		pb_swap(a, b);
	while ((r = a % b) != 0) {
		a = b;
		b = r;
	}
	return b;
}

u32 pb_timestamp(void)
{
	u64 timestamp;

	timestamp = jiffies * (u64)TICKFREQ;
	do_div(timestamp, HZ);

	return (u32)timestamp;
}

u32 pb_get_tsc_tick(void)
{
	u64 tsc;

	rdtscll(tsc);

	return (u32)(tsc >> TSC_SHIFT);
}

const char *pb_get_str_by_key(const char* rom, const char* key)
{
	bool new_word;
	bool half1;
	const char *word_start = rom;

	if (rom == NULL || key == NULL)
		DPRINTK(WARNING, "Exception NULL %p %p\n", rom, key);

	new_word = false;
	half1 = true;
	while (true) {
		if (*rom++ == '\0') {
			if (!new_word)
				return --rom;
			if (half1 && strcmp(word_start, key) == 0)
				return rom;
			half1 = !half1;
			new_word = false;
			word_start = rom;
		} else {
			new_word = true;
		}
	}
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 16)
#define to_drv(node) container_of(node, struct device_driver, kobj.entry)
static int bus_match(struct device *dev, struct device_driver *drv)
{
	int error = -ENODEV;

	if (dev->bus->match(dev, drv)) {
		dev->driver = drv;
		if (drv->probe) {
			error = drv->probe(dev)
			if (error) {
				dev->driver = NULL;
				return error;
			}
		}
		device_bind_driver(dev);
		error = 0;
	}
	return error;
}

int device_attach(struct device *dev)
{
	struct bus_type *bus = dev->bus;
	struct list_head *entry;
	int error;

	if (dev->driver) {
		device_bind_driver(dev);
		return 1;
	}

	if (bus->match) {
		list_for_each(entry, &bus->drivers.list) {
			struct device_driver *drv = to_drv(entry);
			error = bus_match(dev, drv);
			if (!error)
				/* success, driver matched */
				return 1;
			if (error != -ENODEV)
				/* driver matched but the probe failed */
				DPRINTK(WARNING, "%s: probe of %s failed with error %d\n",
					drv->name, dev->bus_id, error);
		}
	}

	return 0;
}
#endif

#if LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 16)
/**
 * Codes ported from linux-2.6.9.elsmp
 * pcie_set_readrq - set PCI Express maximum memory read request
 * @dev: PCI device to query
 * @count: maximum memory read count in bytes
 * valid values are 128, 256, 512, 1024, 2048, 4096
 *
 * If possible sets maximum read byte count
 */
int pcie_set_readrq(struct pci_dev *dev, int rq)
{
	int cap, err = -EINVAL;
	u16 ctl, v;

	if (rq < 128 || rq > 4096 || (rq & (rq - 1)))
		goto out;

	v = (ffs(rq) - 8) << 12;

	cap = pci_find_capability(dev, PCI_CAP_ID_EXP);
	if (!cap)
		goto out;

	err = pci_read_config_word(dev, cap + PCI_EXP_DEVCTL, &ctl);
	if (err)
		goto out;

	if ((ctl & PCI_EXP_DEVCTL_READRQ) != v) {
		ctl &= ~PCI_EXP_DEVCTL_READRQ;
		ctl |= v;
		err = pci_write_config_dword(dev, cap + PCI_EXP_DEVCTL, ctl);
	}

out:
	return err;
}

#endif

