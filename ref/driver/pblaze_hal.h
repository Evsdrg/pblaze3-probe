#ifndef _PBLAZE_HAL_H_
#define _PBLAZE_HAL_H_

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/genhd.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <asm/atomic.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/device.h>
#include <linux/blkdev.h>
#include <linux/dma-mapping.h>
#include <linux/hdreg.h>
#include <linux/reboot.h>
#include <linux/ctype.h>
#include <asm/msr.h>
#include <linux/nfs.h>
#include <linux/firmware.h>
#include <linux/completion.h>
#include <linux/kobject.h>
#include <linux/kref.h>
#include <linux/proc_fs.h>
#include <linux/workqueue.h>
#include <linux/kmod.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
#include <linux/aer.h>
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
#include <asm/semaphore.h>
#else
#include <linux/semaphore.h>
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
#include <linux/seq_file.h>
#endif

#define HARDWARE_FAULTTOLERANT
#define RDONLY_CHECK
/* Trim will not be supported from 10/12/2015. */
/* #define TRIM_SUPPORT */
#define PBLAZE_DEBUG
#define PBLAZE_COUNTER

#define MEM_PRINTK_EMERG		0	/* No use */
#define MEM_PRINTK_ALERT		1	/* Hardfault, readonly, bad block etc */
#define MEM_PRINTK_CRIT			2
#define MEM_PRINTK_ERR			3	/* Error */
#define MEM_PRINTK_WARNING		4	/* Error but slight */
#define MEM_PRINTK_NOTICE		5
#define MEM_PRINTK_INFO			6
#define MEM_PRINTK_DEBUG		7	/* Debug huge info */

#ifdef PBLAZE_DEBUG
#define PFX				"[pblaze3] "
#define MEM_DEBUG_LEVEL			MEM_PRINTK_ERR

#define DPRINTK(level, fmt, args...)					\
	do {								\
		if (MEM_PRINTK_##level <= MEM_DEBUG_LEVEL)		\
			printk(KERN_##level PFX "%s: %d: " fmt,		\
			       __FUNCTION__, __LINE__, ## args);	\
	} while (0)
#else
#define DPRINTK(level, fmt, args...)
#endif

#ifndef RHEL_RELEASE_VERSION
#define RHEL_RELEASE_VERSION(a, b)	0
#endif

/* kernel port data struct */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 19)
typedef struct kmem_cache kmem_cache_t;
#endif

#ifndef __devinit
#define __devinit
#define __devinitdata
#endif

#ifndef __devexit
#define __devexit
#define __devexitdata
#endif

#ifndef __devexit_p
#define __devexit_p(x) x
#define __devexitdata_p(x) x
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 18)
typedef u32 bool;
enum {
	false   = 0,
	true    = 1
};
#endif

#define PB_SIGNATURE			0x05300000
#define TICKFREQ			10000

/* Common macro */
#define NR_DEVICE			2
#define NR_CPU				2
#define NR_LUN_PER_TETRIS		7

#define COMM_DEV_COUNT			2
#define COMM_DEV_START			0
#define COMM_DEV_END			(COMM_DEV_START + COMM_DEV_COUNT)

#define TSC_SHIFT			10

#define MEM_PRINTK_EMERG		0
#define MEM_PRINTK_ALERT		1
#define MEM_PRINTK_CRIT			2
#define MEM_PRINTK_ERR			3
#define MEM_PRINTK_WARNING		4
#define MEM_PRINTK_NOTICE		5
#define MEM_PRINTK_INFO			6
#define MEM_PRINTK_DEBUG		7

#define SCHEDULE_PERIOD			10

#define INTERRUPT_QUEUE_SIZE		0x1000
#define INTERRUPT_QUEUE_MASK		(INTERRUPT_QUEUE_SIZE - 1)

#define MAX_DMA_LENGTH			0x7f
#define KB_SIZE				0x400
#define MAX_CARDNUM			16
#define MAX_IO_NUM			(INTERRUPT_QUEUE_SIZE * MAX_CARDNUM)

#define PB_BLOCK_SHIFT			12
#define PB_BLOCK_SIZE			(1 << PB_BLOCK_SHIFT)
#define PB_BLOCK_MASK			(PB_BLOCK_SIZE - 1)
#define PB_SECTOR_SHIFT			9
#define PB_SECTOR_SIZE			(1 << PB_SECTOR_SHIFT)

/* Block to sector */
#define PB_B2S_SHIFT			(PB_BLOCK_SHIFT - PB_SECTOR_SHIFT)
#define PB_B2S_SIZE			(1 << PB_B2S_SHIFT)
#define PB_B2S_MASK			(PB_B2S_SIZE - 1)

/* Block to MB */
#define PB_B2MB_SHIFT			(20 - PB_BLOCK_SHIFT)

/* Sector to MB */
#define PB_S2MB_SHIFT			(20 - PB_SECTOR_SHIFT)

/* monitor stuff macro */
#define PB_ROM_SIZE			4096
#define DRVVER_SIZE			64
#define PB_LOCKROM_OFFSET		(PB_ROM_SIZE - 1)
#define PB_LOCKROM_MAGIC		(0xAB)

#define LATENCY_BUCKET			32
#define LATENCY_COUNTER			2

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 27)
#define MEM_BIO_BARRIER			(1u << BIO_RW_BARRIER)
#define MEM_BIO_TRIM			(1ull << 32)
#define MEM_BIO_SECURE			(1ull << 33)
#define MEM_BIO_RW			(1u << BIO_RW)
#define MEM_BIO_FLUSH			(1ull << 34)

#define MEM_REQ_BARRIER			(REQ_HARDBARRIER | REQ_SOFTBARRIER)
#define MEM_REQ_TRIM			(1ull << 32)
#define MEM_REQ_SECURE			(1ull << 33)
#define MEM_REQ_RW			REQ_RW
#define MEM_REQ_FLUSH			(1ull << 34)
#elif LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 35)
#define MEM_BIO_BARRIER			(1u << BIO_RW_BARRIER)
#define MEM_BIO_TRIM			(1u << BIO_RW_DISCARD)
#define MEM_BIO_SECURE			(1ull << 33)
#define MEM_BIO_RW			(1u << BIO_RW)
#define MEM_BIO_FLUSH			REQ_FUA

#define MEM_REQ_BARRIER			(REQ_HARDBARRIER | REQ_SOFTBARRIER)
#define MEM_REQ_TRIM			REQ_DISCARD
#define MEM_REQ_SECURE			(1ull << 33)
#define MEM_REQ_RW			REQ_RW
#define MEM_REQ_FLUSH			REQ_FUA
#elif LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 36)
#define MEM_BIO_BARRIER			(REQ_HARDBARRIER | REQ_SOFTBARRIER)
#define MEM_BIO_TRIM			REQ_DISCARD
#define MEM_BIO_SECURE			REQ_SECURE
#define MEM_BIO_RW			REQ_WRITE
#define MEM_BIO_FLUSH			(REQ_FLUSH | REQ_FUA)

#define MEM_REQ_BARRIER			(REQ_HARDBARRIER | REQ_SOFTBARRIER)
#define MEM_REQ_TRIM			REQ_DISCARD
#define MEM_REQ_SECURE			REQ_SECURE
#define MEM_REQ_RW			REQ_WRITE
#define MEM_REQ_FLUSH			(REQ_FLUSH | REQ_FUA)
#elif LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 38)
#define MEM_BIO_BARRIER			(REQ_SOFTBARRIER)
#define MEM_BIO_TRIM			REQ_DISCARD
#define MEM_BIO_SECURE			REQ_SECURE
#define MEM_BIO_RW			REQ_WRITE
#define MEM_BIO_FLUSH			(REQ_FLUSH | REQ_FUA)

#define MEM_REQ_BARRIER			(REQ_SOFTBARRIER)
#define MEM_REQ_TRIM			REQ_DISCARD
#define MEM_REQ_SECURE			REQ_SECURE
#define MEM_REQ_RW			REQ_WRITE
#define MEM_REQ_FLUSH			(REQ_FLUSH | REQ_FUA)
#else
#define MEM_BIO_BARRIER			(REQ_SOFTBARRIER)
#define MEM_BIO_TRIM			REQ_DISCARD
#define MEM_BIO_SECURE			REQ_SECURE
#define MEM_BIO_RW			REQ_WRITE
#define MEM_BIO_FLUSH			(REQ_FLUSH | REQ_FLUSH_SEQ | REQ_FUA)

#define MEM_REQ_BARRIER			(REQ_SOFTBARRIER)
#define MEM_REQ_TRIM			REQ_DISCARD
#define MEM_REQ_SECURE			REQ_SECURE
#define MEM_REQ_RW			REQ_WRITE
#define MEM_REQ_FLUSH			(REQ_FLUSH | REQ_FLUSH_SEQ | REQ_FUA)
#endif

#define PB_BIO_TRIM			(MEM_BIO_TRIM | MEM_BIO_RW)
#define PB_BIO_SECURE_TRIM		(MEM_BIO_SECURE | MEM_BIO_TRIM | MEM_BIO_RW)

#define	PATH_STR			"PATH=/sbin:/bin:/usr/sbin:/usr/bin"
#define	BUG_REPORT_TOOL			"/usr/bin/pblaze_bug_report"	

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
#define	UMH_WAIT_EXEC	(0)
#endif

enum {
	NR_PKT_FREE,
	NR_PKT_ALLOCED,
	NR_PKT_SENT,
	NR_PKT_RECVD,
	NR_IDX_RECVD,
	NR_PKT_STAT_ITEMS
};

enum {
	NR_DMA_OPS,
	NR_DMA_CMDS,
	NR_DMA_ENTRYS,
	NR_DMA_READ,
	NR_DMA_WRITE,
	NR_OPS_CNT_ITEMS
};

enum {
	ST_STAGE_INIT,
	ST_STAGE_READY,
	ST_STAGE_RUN,
	ST_STAGE_MID = 10,	/* The middle stage between ready and run */
	ST_STAGE_STOP,		/* Some core status are not run */
	ST_STAGE_COUNT
};

#define PB_S_DMA_CNT			4
#define REQSTACK_SIZE			16

#define MSIX_TABLE_SIZE			32
#define MSIX_TABLE_SHIFT		8
#define MSIX_TABLE_LOCK_SHIFT		1

#define MAX_PENDING_CNT			(sizeof(unsigned long) * 8)

struct io_range {
	u64 start;
	u64 len;
};

struct io_conflict {
	struct io_range vec[MAX_PENDING_CNT];
	unsigned long mask;
	u32 cnt;
	spinlock_t lock;
};

/* io request private information for pcie device */
struct io_request_priv {
	/* link into pblaze_pcie->ioreq_list */
	struct list_head node;

	u32 start;				/* io request start sector */
	u32 len;				/* io request sector length */
	dma_addr_t small_dma_tbl[PB_S_DMA_CNT];	/* dma mapping address */
	dma_addr_t *big_dma_tbl;
	bool is_inserted;
};

struct io_request;
typedef void (pblaze_complete_ioreq)(struct io_request *ioreq, int error);

struct io_request {
	struct pblaze_disk *disk;
	struct bio *bio;
	bool is_dre;

	u32 stack[REQSTACK_SIZE];
	u32 sp;				/* cur offset from start of stack */
	int error;
	volatile int ioreq_ref;		/* the number of pblaze_pcie in ioreq */
	u64 ioreq_len;
	u64 ioreq_start;
	unsigned long submit_jiffies;		/* For disk's xx_ticks */
	unsigned long issue_jiffies;		/* For disk's xx_ticks */
	u32 submit_tick;		/* For latency monitor's xx_latency */
	pblaze_complete_ioreq *complete_ioreq;
	struct list_head sync_node;	/* pblaze_sync_queue->sync_ioreq_list */
	struct io_request_priv privs[NR_DEVICE];
};

enum {
	IOW_ALIGN_TYPE = 1,
	IOW_UNALIGN_TYPE = 2,
	IOW_COUNT
};

struct io_wrapper {
	void *private;			/* bio or io_request depend on type */
	struct list_head req_node;      /* Link to disk->req_list */
	u32 type;
};

struct pblaze_fpga_reg {
	u32 addr;
	u8 flag;
	const char *name;
};

struct pblaze_init_monitor {
	char rom[PB_ROM_SIZE];
	char driver_version[DRVVER_SIZE];
};

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

struct pblaze_latency_monitor {
	u32 count;
	u32 total_latency;
	u32 max_latency;
	u32 min_latency;
	u32 bucket_count[LATENCY_BUCKET];
};

struct pblaze_ioctl_monitor {
	u64 logical_write;
	u64 flash_write;
	u64 logical_read;
	u64 flash_read;
	u32 dead_block;
	u32 dead_die;
	u32 init_count;
	u32 start_count;
	u32 comm_status[NR_DEVICE * NR_CPU];
	u32 current_tick;
	u32 read_iops;
	u32 read_size;
	u32 write_iops;
	u32 write_size;
	u32 disk_size_MB;
	u32 tetris_num;
	u32 link_width;
	u32 link_gen;
	u32 driver_mem_size;
	u16 temperature;
	u16 temperature_max;
	u16 temperature_min;
	u16 vcore;
	u16 vcore_max;
	u16 vcore_min;
	u16 vpll;
	u16 vpll_max;
	u16 vpll_min;
	u16 lock_rom;
	u16 vddr;
	s16 board_temperature_max;
	s16 board_temperature_min;
	u16 temperature_unsafe;
	s16 board_temperature;
	u16 flying_pipe_percent;
	u64 error_code;
	struct pblaze_latency_monitor latency_monitor[LATENCY_COUNTER];
};

struct pblaze_firmware_name {
	char name[NFS_MAXPATHLEN];
};

struct pblaze_firmware_data {
	u32 magic;
	u32 model;
	u32 version;
	u32 length;
	u32 crc;
	u8 data[1];
};

struct pblaze_virt_dev;

struct pblaze_virt_drv {
	struct device_driver driver;

	const char *name;
	int (*probe)(struct pblaze_virt_dev *dev);
	void (*remove)(struct pblaze_virt_dev *dev);
	void (*shutdown)(struct pblaze_virt_dev *dev);
	void (*setcapacity)(struct pblaze_virt_dev *dev);
	int signature;
};

struct pblaze_virt_dev {
	struct device dev;

	char name[256];
	struct pblaze_virt_drv *driver;		/* attached driver */
	struct pblaze_raid *parent_dev;		/* which has allocated it */
	struct semaphore *online_sem;

	int minor;
	volatile int open_handles;
	bool is_attached;

	int signature;
};

extern struct bus_type pblaze_virt_bus_type;
extern int pblaze_virt_drv_register(struct pblaze_virt_drv *virt_drv,
			     struct module *owner, const char *mod_name);
extern void pblaze_virt_drv_unregister(struct pblaze_virt_drv *driver);

extern struct semaphore *pblaze_virt_drv_get_lock(void);
extern struct pblaze_virt_dev *pblaze_get_virt_dev(int minor);
extern sector_t pblaze_virt_dev_get_size(struct pblaze_virt_dev *virt_dev);

kmem_cache_t *pblaze_get_dma_addr_slab(void);
extern kmem_cache_t *pblaze_get_ioreq_slab(void);
extern struct semaphore *pblaze_get_ioreq_limiter(void);
extern u32 *pblaze_get_driver_size(void);

extern int pblaze_raid_prepare_ioreq(struct pblaze_raid *raid, struct io_request *ioreq);
extern int pblaze_raid_insert_ioreq(struct pblaze_raid *raid, struct io_request *ioreq, bool check);

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 16)
#define DMA_BIT_MASK(n) (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))
#endif

#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 36)
#define init_MUTEX(sem)         sema_init(sem, 1)
#define init_MUTEX_LOCKED(sem)  sema_init(sem, 0)
#endif

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 30)
static inline sector_t blk_rq_pos(const struct request *req)
{
	return req->sector;
}

static inline unsigned int blk_rq_sectors(const struct request *req)
{
	return req->nr_sectors;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)

#define pb_bio_start(bio)				((bio)->bi_sector)
#define pb_bio_iovec(bio)				(bio_iovec(bio))
#define pb_bio_size(bio)				((bio)->bi_size)
#define pb_bio_iovec_idx(bio, idx)		(bio_iovec_idx(bio, idx))
#define pb_bio_idx(bio)					(bio->bi_idx)

#else

#define pb_bio_start(bio)				(bio->bi_iter.bi_sector)
#define pb_bio_iovec(bio)				(bio->bi_io_vec)
#define pb_bio_size(bio)				(bio->bi_iter.bi_size)
#define pb_bio_iovec_idx(bio, idx)		(&bio->bi_io_vec[idx])
#define pb_bio_idx(bio)					(bio->bi_iter.bi_idx)

#endif

/* request and bio check set */
static inline bool pb_is_write_bio(struct bio *bio)
{
	return bio_data_dir(bio);
}

static inline bool pb_is_flush_bio(struct bio *bio)
{
	return bio->bi_rw & MEM_BIO_FLUSH;
}

static inline bool pb_is_barrier_bio(struct bio *bio)
{
	return bio->bi_rw & MEM_BIO_BARRIER;
}

static inline bool pb_is_trim_bio(struct bio *bio)
{
	return (bio->bi_rw & PB_BIO_TRIM) == PB_BIO_TRIM;
}

static inline bool pb_is_secure_trim_bio(struct bio *bio)
{
	return (bio->bi_rw & PB_BIO_SECURE_TRIM) == PB_BIO_SECURE_TRIM;
}

static inline bool pb_is_unsecure_trim_bio(struct bio *bio)
{
	return (bio->bi_rw & PB_BIO_SECURE_TRIM) == PB_BIO_TRIM;
}

static inline bool pb_is_has_data_bio(struct bio *bio)
{
	return (bio->bi_io_vec != NULL) && (bio_segments(bio) != 0) &&
	       !pb_is_trim_bio(bio);
}

/* IO request attribute operate set */
static inline sector_t pb_get_ioreq_start(struct io_request *ioreq)
{
	return pb_bio_start(ioreq->bio);
}

static inline sector_t pb_get_ioreq_len(struct io_request *ioreq)
{
	return bio_sectors(ioreq->bio);
}

static inline void pb_ioreq_inc_sp(struct io_request *ioreq, int size)
{
	ioreq->sp += size;
	BUG_ON(ioreq->sp >= (REQSTACK_SIZE * 4));
}

static inline void pb_ioreq_dec_sp(struct io_request *ioreq, int size)
{
	ioreq->sp -= size;
}

static inline void *pb_ioreq_get_sp(struct io_request *ioreq)
{
	return (void *)(((u8 *)(ioreq->stack)) + ioreq->sp);
}

static inline u32 pb_get_dma_align(dma_addr_t addr)
{
	return addr & PB_BLOCK_MASK;
}

/* IO request operate set */
struct dma_addr_list *pb_alloc_dma_addr(void);
void pb_free_dma_addr(struct dma_addr_list *dma_addr);
void pb_ioreq_complete_io(struct io_request *ioreq, int error);

void pb_acct_stats(struct gendisk *disk, struct io_request *ioreq);
void pb_acct_io_done(struct gendisk *disk, struct io_request *ioreq, int error);

/* From gcc version 4.1.2, __sync_* functions supported */
#if __GNUC__ > 4 || \
    (__GNUC__ == 4 && (__GNUC_MINOR__ > 1 || \
    (__GNUC_MINOR__ == 1 && __GNUC_PATCHLEVEL__ >= 2)))
#define pb_atomic_cmpxchg		__sync_val_compare_and_swap
#define pb_atomic_cmpxchg16		__sync_val_compare_and_swap
#define pb_atomic_add			__sync_fetch_and_add
#define pb_atomic_add16			__sync_fetch_and_add
#define pb_atomic_sub			__sync_fetch_and_sub
#define pb_atomic_sub16			__sync_fetch_and_sub

#define pb_atomic_add_and_fetch		__sync_add_and_fetch
#define pb_atomic_add_and_fetch16	__sync_add_and_fetch
#define pb_atomic_sub_and_fetch		__sync_sub_and_fetch
#define pb_atomic_sub_and_fetch16	__sync_sub_and_fetch

#define pb_atomic_or			__sync_fetch_and_or
#define pb_atomic_or16			__sync_fetch_and_or
#define pb_atomic_and			__sync_fetch_and_and
#define pb_atomic_and16			__sync_fetch_and_and
#else
static inline u32 pb_atomic_cmpxchg(volatile u32 *pv, u32 oldv, u32 newv)
{
	return cmpxchg(pv, oldv, newv);
}

static inline u16 pb_atomic_cmpxchg16(volatile u16 *pv, u16 oldv, u16 newv)
{
	return cmpxchg(pv, oldv, newv);
}

static inline u32 pb_atomic_add(volatile u32 *pv, u32 v)
{
	u32 base, target;
	do {
		base = *pv;
		target = base + v;
	} while (base != pb_atomic_cmpxchg(pv, base, target));
	return base;
}

static inline u16 pb_atomic_add16(volatile u16 *pv, u16 v)
{
	u16 base, target;
	do {
		base = *pv;
		target = base + v;
	} while (base != pb_atomic_cmpxchg16(pv, base, target));
	return base;
}

static inline u32 pb_atomic_sub(volatile u32 *pv, u32 v)
{
	u32 base, target;
	do {
		base = *pv;
		target = base - v;
	} while (base != pb_atomic_cmpxchg(pv, base, target));
	return base;
}

static inline u16 pb_atomic_sub16(volatile u16 *pv, u16 v)
{
	u16 base, target;
	do {
		base = *pv;
		target = base - v;
	} while (base != pb_atomic_cmpxchg16(pv, base, target));
	return base;
}

static inline u32 pb_atomic_add_and_fetch(volatile u32 *pv, u32 v)
{
	u32 base, target;
	do {
		base = *pv;
		target = base + v;
	} while (base != pb_atomic_cmpxchg(pv, base, target));
	return target;
}

static inline u16 pb_atomic_add_and_fetch16(volatile u16 *pv, u16 v)
{
	u16 base, target;
	do {
		base = *pv;
		target = base + v;
	} while (base != pb_atomic_cmpxchg16(pv, base, target));
	return target;
}

static inline u32 pb_atomic_sub_and_fetch(volatile u32 *pv, u32 v)
{
	u32 base, target;
	do {
		base = *pv;
		target = base - v;
	} while (base != pb_atomic_cmpxchg(pv, base, target));
	return target;
}

static inline u16 pb_atomic_sub_and_fetch16(volatile u16 *pv, u16 v)
{
	u16 base, target;
	do {
		base = *pv;
		target = base - v;
	} while (base != pb_atomic_cmpxchg16(pv, base, target));
	return target;
}

static inline u32 pb_atomic_or(volatile u32 *pv, u32 v)
{
	u32 base, target;
	do {
		base = *pv;
		target = base | v;
	} while (base != pb_atomic_cmpxchg(pv, base, target));
	return base;
}

static inline u16 pb_atomic_or16(volatile u16 *pv, u16 v)
{
	u16 base, target;
	do {
		base = *pv;
		target = base | v;
	} while (base != pb_atomic_cmpxchg16(pv, base, target));
	return base;
}

static inline u32 pb_atomic_and(volatile u32 *pv, u32 v)
{
	u32 base, target;
	do {
		base = *pv;
		target = base & v;
	} while (base != pb_atomic_cmpxchg(pv, base, target));
	return base;
}

static inline u16 pb_atomic_and16(volatile u16 *pv, u16 v)
{
	u16 base, target;
	do {
		base = *pv;
		target = base & v;
	} while (base != pb_atomic_cmpxchg16(pv, base, target));
	return base;
}
#endif

static inline u32 pb_atomic_xchg(volatile u32 *pv, u32 v)
{
	u32 base, target;
	do {
		base = *pv;
		target = v;
	} while (base != pb_atomic_cmpxchg(pv, base, target));
	return base;
}

static inline u16 pb_atomic_xchg16(volatile u16 *pv, u16 v)
{
	u16 base, target;
	do {
		base = *pv;
		target = v;
	} while (base != pb_atomic_cmpxchg16(pv, base, target));
	return base;
}

/* set the appointed bit to 1, and return the old bit value */
static inline int pb_atomic_test_and_set_bit(volatile u32 *base, int pos)
{
	u32 new, old;
	new = 1u << pos;
	old = pb_atomic_or(base, new);
	old = (old >> pos) & 1;
	return old;
}

/* clear the appointed bit to 0, and return the old bit value */
static inline int pb_atomic_test_and_clear_bit(volatile u32 *base, int pos)
{
	u32 new, old;
	new = ~(1u << pos);
	old = pb_atomic_and(base, new);
	old = (old >> pos) & 1;
	return old;
}

static inline u32 pb_atomic_max(volatile u32 *base, u32 value)
{
	u32 new, old;
	do {
		old = *base;
		if (old >= value) {
			new = old;
			break;
		}
		new = value;
	} while (pb_atomic_cmpxchg(base, old, new) != old);
	return new;
}

static inline u32 pb_atomic_min(volatile u32 *base, u32 value)
{
	u32 new, old;
	do {
		old = *base;
		if (old <= value && old != 0) {
			new = old;
			break;
		}
		new = value;
	} while (pb_atomic_cmpxchg(base, old, new) != old);
	return new;
}

#define pb_atomic_inc(a)		pb_atomic_add_and_fetch(a, 1u)
#define pb_atomic_inc16(a)		pb_atomic_add_and_fetch16(a, 1u)
#define pb_atomic_dec(a)		pb_atomic_sub_and_fetch(a, 1u)
#define pb_atomic_dec16(a)		pb_atomic_sub_and_fetch16(a, 1u)

/* porting from linux kernel 2.6.34 */
#define pb_swap(a, b) \
	do { typeof(a) __tmp = (a); (a) = (b); (b) = __tmp; } while (0)

u32 pb_gcd(u32 a, u32 b);

/*
 * TICKFREQ must be larger than max possible HZ value
 * to ensure u32 overflow in CurrentTick.
 */
u32 pb_timestamp(void);
u32 pb_get_tsc_tick(void);

const char *pb_get_str_by_key(const char* rom, const char* key);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 16)
int device_attach(struct device *dev)
#endif

#if LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 16)
int pcie_set_readrq(struct pci_dev *dev, int rq);
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 24) && defined(RHEL_RELEASE_CODE)
static inline void blk_queue_discard_granularity(struct request_queue *q, unsigned int gran)
{
	q->limits.discard_granularity = gran;
}
#endif

static inline bool pb_test_reenter(volatile u32 *task_hash, int *hash)
{
	int pid;
	int base, offset;

	pid = (current->pid) & 255;
	*hash = pid;
	base = pid >> 5;	/* 3 + 5 */
	offset = pid & ((1 << 5) - 1);

	return !pb_atomic_test_and_set_bit(task_hash + base, offset);
}

static inline void pb_clear_reenter(volatile u32 *task_hash, int hash)
{
	int base, offset;

	base = hash >> 5;
	offset = hash & ((1 << 5) - 1);

	pb_atomic_test_and_clear_bit(task_hash + base, offset);
}

/*
 * This func is for unalign io
 * 0: safe to submit to pice list directly
 * 1: hit confict, need insert into the delay list
 */
static inline int pb_tas_conflict(struct io_conflict *conflict, u64 start, u64 len)
{
	int slot;
	struct io_range *range;

	spin_lock_bh(&conflict->lock);
	/* FIXME: use mask but not cnt */
	if (conflict->cnt == MAX_PENDING_CNT) {
		spin_unlock_bh(&conflict->lock);
		return 1;
	}

	slot = find_first_bit(&conflict->mask, MAX_PENDING_CNT);
	while (slot < MAX_PENDING_CNT) {
		range = &conflict->vec[slot];
		if ((start + len > range->start) &&
		    (start < range->start + range->len)) {
			spin_unlock_bh(&conflict->lock);
			return 1;
		}

		slot = find_next_bit(&conflict->mask, MAX_PENDING_CNT, slot + 1);
	}

	slot = find_first_zero_bit(&conflict->mask, MAX_PENDING_CNT);
	BUG_ON(slot >= MAX_PENDING_CNT);
	range = &conflict->vec[slot];
	range->start = start;
	range->len = len;
	__set_bit(slot, &conflict->mask);
	conflict->cnt++;
	spin_unlock_bh(&conflict->lock);

	return 0;
}

/*
 * Remove this lock
 * 0: safe to submit to pice list directly
 * 1: hit confict, need insert into the delay list
 */
static inline int pb_test_conflict(struct io_conflict *conflict, u64 start, u64 len)
{
	int slot;
	struct io_range *range;

	//spin_lock_bh(&conflict->lock);
	if (conflict->mask == 0) {
		//spin_unlock_bh(&conflict->lock);
		return 0;
	}

	slot = find_first_bit(&conflict->mask, MAX_PENDING_CNT);
	while (slot < MAX_PENDING_CNT) {
		range = &conflict->vec[slot];
		if ((start + len > range->start) &&
		    (start < range->start + range->len)) {
			//spin_unlock_bh(&conflict->lock);

			return 1;
		}

		slot = find_next_bit(&conflict->mask, MAX_PENDING_CNT, slot + 1);
	}
	//spin_unlock_bh(&conflict->lock);

	return 0;
}

static inline void pb_clear_conflict(struct io_conflict *conflict, u64 start, u64 len)
{
	int slot;
	int hit = 0;
	struct io_range *range;

	spin_lock_bh(&conflict->lock);
	slot = find_first_bit(&conflict->mask, MAX_PENDING_CNT);
	while (slot < MAX_PENDING_CNT) {
		range = &conflict->vec[slot];
		if ((start == range->start) && (len == range->len)) {
			__clear_bit(slot, &conflict->mask);
			conflict->cnt--;
			hit = 1;
			break;
		}

		slot = find_next_bit(&conflict->mask, MAX_PENDING_CNT, slot + 1);
	}
	spin_unlock_bh(&conflict->lock);

	if (hit == 1) {
		DPRINTK(DEBUG, "hit when clear, cnt:%u, S:%llu, L:%llu\n",
			conflict->cnt, start, len);
	} else {
		DPRINTK(ERR, "not hit when clear, cnt:%u, S:%llu, L:%llu\n",
			conflict->cnt, start, len);
	}
}

#endif
