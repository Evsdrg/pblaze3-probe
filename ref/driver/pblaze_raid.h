#ifndef _PBLAZE_RAID_H_
#define _PBLAZE_RAID_H_

#include "pblaze_hal.h"
#include "pblaze_pcie.h"

#define PB_DISK_MINORS			32

#define PB_DAY_PER_M			30
#define PB_HOUR_PER_D			24
#define PB_MIN_PER_H			60
#define PB_SEC_PER_M			60
#define PB_MSEC_PER_S			1000
#define PB_DRE_PERIOD			3		/* 3 months */
#define PB_DRE_LOG_PERIOD		(HZ * 60 * 60)	/* 1 hours */
#define PB_DRE_ECC			(0xeccbeef)

/* Bit position of dre flag */
#define PB_DRE_QUERY_WRITE		0
#define PB_DRE_READY			1
#define PB_DRE_INITING			2
#define PB_DRE_WORKING			3

#define IOCTL_MAGIC			162
#define IOCTL_REINITIALIZE		_IOW(IOCTL_MAGIC, 1, u32)
#define IOCTL_BAECON			_IOW(IOCTL_MAGIC, 3, u32)
#define IOCTL_QUERYSTAT 		_IOR(IOCTL_MAGIC, 4, struct pblaze_ioctl_monitor)
#define IOCTL_INITIALQUERYSTAT		_IOR(IOCTL_MAGIC, 5, struct pblaze_init_monitor)
#define IOCTL_UPDATEFIRMWARE		_IOW(IOCTL_MAGIC, 6, struct pblaze_firmware_name)
#define IOCTL_ATTACH			_IO(IOCTL_MAGIC, 7)
#define IOCTL_DETACH			_IO(IOCTL_MAGIC, 8)
#define IOCTL_LOCKROM   		_IOW(IOCTL_MAGIC, 9, u32)
#define IOCTL_WRITEROM  		_IOW(IOCTL_MAGIC, 10 , struct pblaze_write_rom_info)

#define PB_ROM_LOCKED			1
#define PB_ROM_UNLOCKED			0	/* default value */
enum {
	PB_FATAL_FAULT,
	PB_FATAL_WILD_PACKET,
	PB_FATAL_TOO_MANY_PACKETS,
	PB_CRITICAL_IO_TIMEOUT,
	PB_CRITICAL_CMD_TIMEOUT,
	PB_CRITICAL_STATUS_TIMEOUT,
	PB_CRITICAL_RDONLY,
	PB_WARNING_EXCEED_RANGE,
	PB_NOTICE_PRINT,
};

struct pblaze_raid_driver {
	kmem_cache_t *ioreq_slab;
	struct semaphore sem;			/* protect this driver */
	int major;				/* For pblaze_raid*/
	volatile int minor;			/* For pblaze_virt_dev */
	u32 driver_mem_size;
	struct pblaze_virt_dev *virt_devs[PB_DISK_MINORS];
};

struct pblaze_raid {
	struct list_head node;

	struct device *pcie_child_dev;		/* Optimize the access path */
	struct pblaze_pcie *pcie_child;
	struct pblaze_pcie *pcie_devs[NR_DEVICE];
	u32 nr_dev;				/* num of attached pcie */
	u32 nr_dev_max;				/* num of raid childern */

	struct gendisk *gendisk;		/* For memcon device */
	struct request_queue *queue;
	struct io_conflict conflict;

	/* For dre work stuff */
	u32 dre_idx;
	u32 dre_log_idx;
	u32 dre_max_idx;
	unsigned long dre_timeout;
	unsigned long dre_log_jiffies;
	unsigned long dre_flags;
	struct io_request *dre_req;
	struct io_request *dre_log_req;
	struct completion dre_completion;
	struct completion dre_exit_completion;
	struct task_struct *dre_thread;

	u64 disk_capacity_MB;
	u32 disk_size_MB;
	u16 tetris_total_num;
	u32 is_baecon_On;

	volatile u32 read_io;
	volatile u32 read_size;
	volatile u32 write_io;
	volatile u32 write_size;
	volatile u32 flying_pipe_size;
	volatile u32 flying_pipe_count;

	volatile u32 latency_jiffies;
	struct pblaze_latency_monitor latency_monitor[LATENCY_COUNTER];
	struct pblaze_init_monitor init_monitor;
	struct pblaze_ioctl_monitor ioctl_monitor;

	volatile bool is_unload;		/* forbid change to ready */
	spinlock_t is_unload_lock;

	volatile u32 outstanding_handle;	/* num of virt_dev attached */
	struct pblaze_virt_dev *virt_dev;

	u32 status;				/* init, ready, run */
	u32 new_status;
	wait_queue_head_t stat_wq;
	struct work_struct stat_work;
	struct work_struct bug_report_work;

	/*
	 * Protect ioctl operation such as reinit, update_firmware,
	 * attach, detach. Using this sem to replace _builtin_ device_lock
	 * which is for linux device model.
	 */
	struct semaphore ioctl_sem;

	unsigned long flags;			/* Fault, readonly */
#ifdef HARDWARE_FAULTTOLERANT
	volatile unsigned long last_insert_jiffies;
	struct timer_list io_timer_list;
#endif
	volatile u32 outstanding_io;

	u32 tetris_capacity;			/* calc in sector */
	u32 tetris_size;			/* calc in sector */
	u16 tetris_num[NR_DEVICE];

	s32 *dev_tbl;				/* mapping dev table */
	u32 cycle;				/* mapping cycle*/
	bool is_tbl_ready;

	const char *dev_sn;
	struct proc_dir_entry *proc_dir;
	unsigned long error_code;

#ifdef PBLAZE_COUNTER
	volatile u32 submit_cnt;
	volatile u32 recv_cnt;
	volatile u32 cmpl_cnt;
#endif

	struct kref refcn;
};

struct pblaze_write_rom_info {
	u32 dev_idx;
	struct pblaze_init_monitor init_monitor;
};

bool pblaze_test_unload(struct pblaze_raid *raid);
void pblaze_set_unload(struct pblaze_raid *raid);

int pblaze_raid_probe(struct pblaze_pcie *pcie, char *sn, u32 index, u32 num);
void pblaze_raid_remove(struct pblaze_pcie *pcie);
void pblaze_raid_free(struct pblaze_pcie *pcie);

/* raid ioctl sub cmd implement series*/
int pblaze_raid_lock_rom(struct pblaze_raid *raid, u32 dev_idx);
int pblaze_raid_send_rom(struct pblaze_raid *raid, u32 size,
			       char *data, u32 dev_idx);
int pblaze_raid_recv_monitor(struct pblaze_raid *raid,
			      struct pblaze_ioctl_monitor *monitor);
void pblaze_raid_beacon(struct pblaze_raid *raid, bool is_baecon);
int pblaze_raid_send_fw(struct pblaze_raid *raid, u32 size, char *data);
int pblaze_raid_safe_erase(struct pblaze_raid *raid, u32 per_tetris_size_page);
int pblaze_raid_attach(struct pblaze_raid *raid);
int pblaze_raid_detach(struct pblaze_raid *raid);

/* Not for ioctl */
void pblaze_raid_reinit(struct pblaze_raid *raid);
void pblaze_raid_recv_tetris_num(struct pblaze_raid *raid, u16 *num);
void pblaze_raid_recv_disk_size(struct pblaze_raid *raid, u32 *size);

long pblaze_raid_wait_status(struct pblaze_raid *raid, u32 stat, long timeout);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
void pblaze_raid_status_change_worker(void *data);
#else
void pblaze_raid_status_change_worker(struct work_struct *work);
#endif

void pblaze_raid_complete_io(struct pblaze_raid *raid, struct io_request *ioreq);
void release_pblaze_raid(struct kref *refcn);
int pblaze_dre_init(struct pblaze_raid *raid);
void pblaze_dre_exit(struct pblaze_raid *raid);

void pblaze_dump_pcie_fpga_counter(struct pblaze_pcie *dev);
#endif
