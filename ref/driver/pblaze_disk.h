#ifndef _PBLAZE_DISK_H_
#define _PBLAZE_DISK_H_

#include <asm/page.h>
#include <asm/kmap_types.h>

#include "pblaze_hal.h"
#include "pblaze_raid.h"

#define SYNCQUEUE_HASH	103
#define DISK_PROC_NAME	"diskstat"

struct pblaze_disk_driver {
	struct pblaze_virt_drv driver;
	int major;
};

struct pblaze_disk_stack {
	pblaze_complete_ioreq *old_complete_ioreq;
	struct pblaze_disk *disk;
};

struct pblaze_sche_req_status {
	volatile int count;
	int error;
};

/* For io request going through io scheduler */
struct pblaze_disk_sche_req_stack {
	pblaze_complete_ioreq *old_complete_ioreq;
	struct pblaze_sche_req_status *pstatus;
	struct pblaze_sche_req_status status;
	bool is_back_bio;
};

struct pblaze_disk {
	struct pblaze_virt_dev *virt_dev;
	struct pblaze_raid *raid;

	struct gendisk *gendisk;
	struct request_queue *queue;
	spinlock_t queue_lock;

	u64 req_list_length;
	struct list_head req_list;
	spinlock_t req_list_lock;

	volatile u32 io_counter;
	wait_queue_head_t io_wq;

	struct task_struct *sche0_thread;
	struct semaphore sche_sem;
	volatile bool is_stop;
	volatile bool is_stoped;

	struct proc_dir_entry *proc_dir;
	struct kref refcn;
};

static int __devinit pblaze_disk_probe(struct pblaze_virt_dev *virt_dev);
static void pblaze_disk_set_capacity(struct pblaze_virt_dev *virt_dev);
static void __devexit pblaze_disk_remove(struct pblaze_virt_dev *virt_dev);
static void pblaze_disk_shutdown(struct pblaze_virt_dev *virt_dev);

#endif
