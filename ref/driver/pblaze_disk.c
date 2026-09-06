
#include "pblaze_disk.h"

MODULE_DESCRIPTION("MEMBlaze Disk Module");
MODULE_AUTHOR("MemBlaze");
MODULE_LICENSE("GPL");

/* This is ridiculous, name disk_drv will get a lot of warning */
struct pblaze_disk_driver disk_driver = {
	.driver.name = "memdisk driver",
	.driver.probe = pblaze_disk_probe,
	.driver.setcapacity = pblaze_disk_set_capacity,
	.driver.remove = __devexit_p(pblaze_disk_remove),
	.driver.shutdown = pblaze_disk_shutdown,
	.driver.signature = PB_SIGNATURE,
};

static void release_pblaze_disk(struct kref *refcn)
{
	struct pblaze_disk *disk;

	disk = container_of(refcn, struct pblaze_disk, refcn);
	kfree(disk);
}

static inline void pb_free_ioreq(struct io_request *ioreq)
{
	int i;
	struct pblaze_disk *disk= ioreq->disk;

	for (i = 0; i < NR_DEVICE; i++)
		kfree(ioreq->privs[i].big_dma_tbl);

	kmem_cache_free(pblaze_get_ioreq_slab(), ioreq);
	pb_atomic_dec(&disk->io_counter);

	if (waitqueue_active(&disk->io_wq)) {
		wake_up_interruptible(&disk->io_wq);
	}
}

static inline void pb_release_ioreq(struct io_request *ioreq, int error)
{
	struct bio *bio;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 24)
	unsigned int bytes_done;
#endif
	bio = ioreq->bio;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 24)
	bytes_done = pb_get_ioreq_len(ioreq) << PB_SECTOR_SHIFT;
#else
	pb_bio_size(bio) = 0;
#endif
	pb_free_ioreq(ioreq);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 24)
	bio_endio(bio, bytes_done, error);
#else
	bio_endio(bio);
#endif
}

/* alloc dma addr will delay to raid dispatch stage */
static inline struct io_request *pb_alloc_ioreq(struct bio *bio, struct pblaze_disk *disk)
{
	struct io_request *ioreq;

	if (disk->io_counter > MAX_IO_NUM)
		wait_event_interruptible(disk->io_wq, disk->io_counter < MAX_IO_NUM);

	ioreq = kmem_cache_alloc(pblaze_get_ioreq_slab(), GFP_KERNEL);
	if (!ioreq)
		return NULL;

	pb_atomic_inc(&disk->io_counter);
	memset(ioreq, 0, sizeof(struct io_request));
	ioreq->disk = disk;
	ioreq->bio = bio;
	ioreq->submit_jiffies = jiffies;
	ioreq->complete_ioreq = pb_release_ioreq;

	return ioreq;
}

static inline void pblaze_disk_insert_req(struct io_wrapper *iow, struct pblaze_disk *disk)
{
	bool is_empty;

	spin_lock(&disk->req_list_lock);
	is_empty = list_empty(&disk->req_list);
	list_add_tail(&iow->req_node, &disk->req_list);
	disk->req_list_length++;
	spin_unlock(&disk->req_list_lock);

	if (is_empty)
		up(&disk->sche_sem);
}

static struct io_wrapper *pblaze_disk_remove_req(struct pblaze_disk *disk)
{
	struct io_wrapper *iow = NULL;

	spin_lock(&disk->req_list_lock);
	if (!list_empty(&disk->req_list)) {
		iow = list_first_entry(&disk->req_list, struct io_wrapper, req_node);
		list_del_init(&iow->req_node);
		disk->req_list_length--;
	}
	spin_unlock(&disk->req_list_lock);

	return iow;
}

static void pblaze_disk_unalign_bio_copydata(struct bio *bio_usr,
		struct bio *bio_dma, sector_t start, sector_t len, bool is_write)
{
	struct bio_vec *bv_usr, *bv_dma;
	int offset_usr, len_usr;
	u64 pos_usr;
	int offset_dma, len_dma;
	u64 pos_dma;
	void *page_usr, *page_dma;
	u64 remain = (u64)(len << PB_SECTOR_SHIFT);
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 4, 1)
	bool in_si = in_softirq();
	int flag_step = in_si ? (KM_SOFTIRQ0 - KM_USER0) : 0;
#endif

	bv_usr = pb_bio_iovec(bio_usr);
	pos_usr = (pb_bio_start(bio_usr)) << PB_SECTOR_SHIFT;
	while (pos_usr + bv_usr->bv_len <= (start << PB_SECTOR_SHIFT)) {
		++bv_usr;
		pos_usr += bv_usr->bv_len;
	}
	offset_usr = bv_usr->bv_offset + ((start << PB_SECTOR_SHIFT) - pos_usr);
	len_usr = bv_usr->bv_len - ((start << PB_SECTOR_SHIFT) - pos_usr);

	/* Get dma start info */
	bv_dma = pb_bio_iovec(bio_dma);
	pos_dma = (pb_bio_start(bio_dma)) << PB_SECTOR_SHIFT;
	while (pos_dma + bv_dma->bv_len <= (start << PB_SECTOR_SHIFT)) {
		++bv_dma;
		pos_dma += bv_dma->bv_len;
	}
	offset_dma = bv_dma->bv_offset + ((start << PB_SECTOR_SHIFT) - pos_dma);
	len_dma = bv_dma->bv_len - ((start << PB_SECTOR_SHIFT) - pos_dma);

	/*
	 * As kernel growing, kmap_atomic will transitions to
	 * kmap_atomic(page) from kmap_atomic(page, idx).
	 * In some kernel version such as 3.0, kmap_atomic(page, idx) is same
	 * with kmap_atomic(page), which cause flag_step is not using warning.
	 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 4, 1)
	page_usr = kmap_atomic(bv_usr->bv_page, KM_USER0 + flag_step);
	page_dma = kmap_atomic(bv_dma->bv_page, KM_USER1 + flag_step);
#else
	page_usr = kmap_atomic(bv_usr->bv_page);
	page_dma = kmap_atomic(bv_dma->bv_page);
#endif

	while (true) {
		int lenmin = min(remain, (u64)min(len_usr, len_dma));
		if (is_write)
			memcpy(page_dma + offset_dma, page_usr + offset_usr, lenmin);
		else
			memcpy(page_usr + offset_usr, page_dma + offset_dma, lenmin);

		remain -= lenmin;
		if (!remain)
			break;
		if (len_usr == lenmin) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 4, 1)
			kunmap_atomic(page_usr, KM_USER0 + flag_step);
#else
			kunmap_atomic(page_usr);
#endif
			if (++bv_usr == pb_bio_iovec_idx(bio_usr, bio_usr->bi_vcnt)) {
				bio_usr = bio_usr->bi_next;
				bv_usr = pb_bio_iovec(bio_usr);
			}

			offset_usr = bv_usr->bv_offset;
			len_usr = bv_usr->bv_len;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 4, 1)
			page_usr = kmap_atomic(bv_usr->bv_page, KM_USER0 + flag_step);
#else
			page_usr = kmap_atomic(bv_usr->bv_page);
#endif
		} else {
			len_usr -= lenmin;
			offset_usr += lenmin;
		}

		if (len_dma == lenmin) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 4, 1)
			kunmap_atomic(page_dma, KM_USER1 + flag_step);
#else
			kunmap_atomic(page_dma);
#endif
			++bv_dma;
			offset_dma = bv_dma->bv_offset;
			len_dma = bv_dma->bv_len;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 4, 1)
			page_dma = kmap_atomic(bv_dma->bv_page, KM_USER1 + flag_step);
#else
			page_dma = kmap_atomic(bv_dma->bv_page);
#endif
		} else {
			len_dma -= lenmin;
			offset_dma += lenmin;
		}
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 4, 1)
	kunmap_atomic(page_usr, KM_USER0 + flag_step);
	kunmap_atomic(page_dma, KM_USER1 + flag_step);
#else
	kunmap_atomic(page_usr);
	kunmap_atomic(page_dma);
#endif
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 24)
int pblaze_disk_end_unalign_io(struct bio *bio, unsigned int bytes_done, int error)
#else
void pblaze_disk_end_unalign_io(struct bio *bio)
#endif
{
	int i;
	struct bio *orig_bio;
	struct bio_vec *bio_vec;

	orig_bio = (struct bio *)bio->bi_private;
	if (orig_bio) {
		for (i = 0; i != bio->bi_vcnt; ++i) {
			bio_vec = pb_bio_iovec_idx(bio, i);
			if (bio_vec->bv_page)
				__free_page(bio_vec->bv_page);
		}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 24)
		bio_endio(orig_bio, (bio_sectors(orig_bio) << PB_SECTOR_SHIFT), error);
#else
		pb_bio_size(orig_bio) = 0;
		bio_endio(orig_bio);
#endif
	}

	if (bio->bi_bdev)
		bio->bi_bdev = NULL;

	bio_put(bio);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 24)
	return error;
#endif
}

void pblaze_disk_loose_bio_copydata(struct bio *orig_bio, struct bio *clone_bio, bool is_write)
{
	unsigned short orig_index = 0, clone_index = 0;
	struct bio_vec *orig_vec, *clone_vec;
	void *orig_addr = NULL, *clone_addr = NULL;
	u32 orig_left = 0, clone_left = PAGE_SIZE;
	u32 orig_pos = 0, clone_pos = 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 4, 1)
	bool in_si = in_softirq();
	int flag_step = in_si ? (KM_SOFTIRQ1 - KM_USER0) : 0;
#endif

	/* The orig_vec's page may be in highmem, so we need kmap_atomic to access it */
	while (clone_index != clone_bio->bi_vcnt) {
		clone_vec = pb_bio_iovec_idx(clone_bio, clone_index);
		clone_addr = page_address(clone_vec->bv_page);
		clone_left = PAGE_SIZE;

		while (orig_index != orig_bio->bi_vcnt) {
			orig_vec = pb_bio_iovec_idx(orig_bio, orig_index);

			if (orig_left == 0) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 4, 1)
				orig_addr = kmap_atomic(orig_vec->bv_page, KM_USER0 + flag_step);
#else
				orig_addr = kmap_atomic(orig_vec->bv_page);
#endif
				orig_pos = orig_vec->bv_offset;
				orig_left = orig_vec->bv_len;
			}

			if (clone_left > orig_left) {
				if (is_write)
					memcpy(clone_addr + clone_pos, orig_addr + orig_pos, orig_left);
				else
					memcpy(orig_addr + orig_pos, clone_addr + clone_pos, orig_left);

				DPRINTK(DEBUG, "orig_index:%u, orig_pos:%u, clone_index:%u, clone_pos:%u\n",
					orig_index, orig_pos, clone_index, clone_pos);

				clone_pos += orig_left;
				clone_left -= orig_left;
				orig_left = 0;

				/* if (orig_left == 0) */
				orig_index++;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 4, 1)
				kunmap_atomic(orig_addr, KM_USER0 + flag_step);
#else
				kunmap_atomic(orig_addr);
#endif
			} else {
				if (is_write)
					memcpy(clone_addr + clone_pos, orig_addr + orig_pos, clone_left);
				else
					memcpy(orig_addr + orig_pos, clone_addr + clone_pos, clone_left);

				DPRINTK(DEBUG, "orig_index:%u, orig_pos:%u, clone_index:%u, clone_pos:%u\n",
					orig_index, orig_pos, clone_index, clone_pos);

				orig_pos += clone_left;
				orig_left -= clone_left;
				clone_pos = 0;
				clone_left = 0;
				clone_index++;

				if (orig_left == 0) {
					orig_index++;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 4, 1)
					kunmap_atomic(orig_addr, KM_USER0 + flag_step);
#else
					kunmap_atomic(orig_addr);
#endif
				}

				break;
			}
		}
	}
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 24)
int pblaze_disk_end_loose_io(struct bio *bio, unsigned int bytes_done, int error)
#else
void pblaze_disk_end_loose_io(struct bio *bio)
#endif
{
	unsigned short i;
	struct bio_vec *bio_vec;
	struct bio *sbio;
	int error = bio->bi_error;

	/* copy data for read */
	sbio = (struct bio *)bio->bi_private;
	if (!error && !bio_data_dir(bio))
		pblaze_disk_loose_bio_copydata(sbio, bio, false);

	for (i = 0; i != bio->bi_vcnt; i++) {
		bio_vec = pb_bio_iovec_idx(bio, i);
		if (bio_vec->bv_page)
			__free_page(bio_vec->bv_page);
	}

	if (bio->bi_bdev)
		bio->bi_bdev = NULL;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 24)
	bio_put(bio);
	bio_endio(sbio, bytes_done, error);
	return error;
#else
	pb_bio_size(sbio) = pb_bio_size(bio);
	bio_put(bio);
	bio_endio(sbio);
#endif
}

static inline struct bio *pblaze_disk_clone_loose_bio(struct bio *sbio, unsigned short vec_cnt)
{
	struct bio *b;
	unsigned short i;
	unsigned long addr;
	struct bio_vec *bio_vec;

	b = bio_alloc(GFP_KERNEL, vec_cnt);
	if (!b)
		return NULL;

	pb_bio_start(b) = pb_bio_start(sbio);
	b->bi_bdev = sbio->bi_bdev;
	b->bi_rw = sbio->bi_rw;
	b->bi_vcnt = vec_cnt;
	pb_bio_size(b) = pb_bio_size(sbio);
	pb_bio_idx(b) = pb_bio_idx(sbio);
	for (i = 0; i < vec_cnt; i++) {
		addr = __get_free_page(GFP_KERNEL);
		if (!addr)
			goto failed_alloc_page;

		bio_vec = pb_bio_iovec_idx(b, i);
		bio_vec->bv_page = virt_to_page(addr);
		bio_vec->bv_offset = 0;
		bio_vec->bv_len = PAGE_SIZE;
	}

	/* copy data for write */
	if (bio_data_dir(sbio))
		pblaze_disk_loose_bio_copydata(sbio, b, true);

	b->bi_private = sbio;
	b->bi_end_io = pblaze_disk_end_loose_io;

	return b;

failed_alloc_page:
	for (i = 0; i != vec_cnt; i++) {
		bio_vec = b->bi_io_vec + i;
		if (bio_vec->bv_page)
			__free_page(bio_vec->bv_page);
	}
	bio_put(b);

	return NULL;
}

static void pblaze_disk_complete_io(struct io_request *ioreq, int error)
{
	struct pblaze_disk *disk;
	struct pblaze_disk_stack *stack;

	pb_ioreq_dec_sp(ioreq, sizeof(struct pblaze_disk_stack));
	stack = pb_ioreq_get_sp(ioreq);
	disk = stack->disk;
	ioreq->complete_ioreq = stack->old_complete_ioreq;
	pb_acct_io_done(disk->gendisk, ioreq, error);
	pb_ioreq_complete_io(ioreq, error);
}

static void pblaze_disk_complete_unalign_io(struct io_request *ioreq, int error)
{
	int i;
	sector_t start, len;
	u64 ioreq_start, ioreq_len;
	struct bio *bio, *orig_bio;
	struct pblaze_disk *disk;
	struct pblaze_disk_sche_req_stack *sche_req_stack;
	struct page *page;

	bio = ioreq->bio;
	orig_bio = (struct bio *)bio->bi_private;
	disk = ioreq->disk;
	start = pb_bio_start(orig_bio);
	len = bio_sectors(orig_bio);

	pb_ioreq_dec_sp(ioreq, sizeof(struct pblaze_disk_sche_req_stack));
	sche_req_stack = pb_ioreq_get_sp(ioreq);
	ioreq->complete_ioreq = sche_req_stack->old_complete_ioreq;

	if (pb_is_write_bio(orig_bio)) {
		ioreq_start = start >> PB_B2S_SHIFT;
		ioreq_len = ((start + len + PB_B2S_MASK) >> PB_B2S_SHIFT) - ioreq_start;

		if (error) {
			DPRINTK(ERR, "Unalign write sequence process error:%d\n", error);
			sche_req_stack->pstatus->error = error;
		}

		switch (pb_atomic_dec(&sche_req_stack->pstatus->count)) {
		case 1:
			bio->bi_private = NULL;
			break;
		case -1:
			/* Write io complete */
			pb_acct_io_done(disk->gendisk, ioreq, error);
			break;
		case 0:
			/* Convert to align io request and issue it again */
			if (sche_req_stack->is_back_bio) {
				page = bio->bi_io_vec[ioreq_len - 1].bv_page;
				bio->bi_io_vec[ioreq_len - 1].bv_page = bio->bi_io_vec[0].bv_page;
				bio->bi_io_vec[0].bv_page = page;
			}

			bio->bi_rw = orig_bio->bi_rw;
			pb_bio_start(bio) = ioreq_start << PB_B2S_SHIFT;
			pb_bio_size(bio) = ioreq_len << PB_BLOCK_SHIFT;
			bio->bi_vcnt = ioreq_len;

			ioreq->ioreq_start = ioreq_start;
			ioreq->ioreq_len = ioreq_len;

			ioreq->complete_ioreq = pblaze_disk_complete_unalign_io;
			pb_ioreq_inc_sp(ioreq, sizeof(struct pblaze_disk_sche_req_stack));

			if (!error)
				error = sche_req_stack->pstatus->error;

			if (sche_req_stack->pstatus != &sche_req_stack->status) {
				kfree(sche_req_stack->pstatus);
				sche_req_stack->pstatus = &sche_req_stack->status;
				sche_req_stack->status.count = 0;
			}

			if (error) {
				pb_ioreq_complete_io(ioreq, error);
			} else {
				pblaze_disk_unalign_bio_copydata(orig_bio, bio, start, len, true);

				/*
				 * Clear ioreq and check all the ioreq's member
				 * Here we don't need worry about dma addr free,
				 * because the prev request just use two addr at most
				 * which will not trigger dma addr alloc.
				 */
				for (i = 0; i < NR_DEVICE; i++) {
					ioreq->privs[i].len = 0;
					ioreq->privs[i].is_inserted = false;
				}

				/* This is a real align integrated request */
				if (pblaze_raid_prepare_ioreq(disk->raid, ioreq) == 0)
					pblaze_raid_insert_ioreq(disk->raid, ioreq, false);
			}

			pb_clear_conflict(&disk->raid->conflict, ioreq_start, ioreq_len);
			return;
		}
	} else {
		/* Read io complete */
		pblaze_disk_unalign_bio_copydata(orig_bio, bio, start, len, false);
		pb_acct_io_done(disk->gendisk, ioreq, error);
	}

	pb_ioreq_complete_io(ioreq, error);
}

/*
 * 0: submit to pcie list successfully
 * 1: hit conflict
 */
static inline int pblaze_disk_process_io(struct pblaze_disk *disk, struct io_request *ioreq)
{
	/*
	 * First, this is a real align write io, and we've already called
	 * pblaze_raid_prepare_ioreq at make_request interface. Here
	 * we just insert it into pcie_list if the corresponding range vec
	 * is clear, so set the check true to check the io_conflict again.
	 */
	return pblaze_raid_insert_ioreq(disk->raid, ioreq, true);
}

/*
 * This is always data io
 * 0: submit to pcie list successfully
 * 1: hit conflict
 * -1: error for example -ENOMEM
 */
static inline int pblaze_disk_process_unalign_io(struct pblaze_disk *disk, struct bio *orig_bio)
{
	int i;
	sector_t start, len;
	u64 ioreq_start, ioreq_len;
	struct bio *bio = NULL, *back_bio = NULL;
	struct bio_vec *bio_vec;
	struct io_request *ioreq = NULL, *back_ioreq = NULL;
	struct pblaze_disk_sche_req_stack *sche_req_stack, *back_sche_req_stack;
	struct pblaze_sche_req_status *status = NULL;
	struct page *page;

	start = pb_bio_start(orig_bio);
	len = bio_sectors(orig_bio);
	ioreq_start = start >> PB_B2S_SHIFT;
	ioreq_len = ((start + len + PB_B2S_MASK) >> PB_B2S_SHIFT) - ioreq_start;

	/*
	 * Before process the whole rw/rrw sequence, lock the correponding range,
	 * after that, it's safe the process the rw/rrw sequence.
	 */
	if (pb_is_write_bio(orig_bio) &&
	    pb_tas_conflict(&disk->raid->conflict, ioreq_start, ioreq_len))
		return 1;

	bio = bio_alloc(GFP_KERNEL, ioreq_len);
	if (!bio) {
		DPRINTK(ERR, "Falied to alloc bio\n");
		goto failed_prepare_front;
	}
	ioreq = pb_alloc_ioreq(bio, disk);
	if (!ioreq) {
		DPRINTK(ERR, "Falied to alloc ioreq\n");
		goto failed_prepare_front;
	} else {
		bio->bi_private = orig_bio;
		bio->bi_end_io = pblaze_disk_end_unalign_io;
		bio->bi_rw = orig_bio->bi_rw;
		pb_bio_start(bio) = ioreq_start << PB_B2S_SHIFT;
		pb_bio_size(bio) = ioreq_len << PB_BLOCK_SHIFT;
		ioreq->ioreq_start = ioreq_start;
		ioreq->ioreq_len = ioreq_len;

		sche_req_stack = pb_ioreq_get_sp(ioreq);
		pb_ioreq_inc_sp(ioreq, sizeof(struct pblaze_disk_sche_req_stack));
		sche_req_stack->old_complete_ioreq = ioreq->complete_ioreq;
		ioreq->complete_ioreq = pblaze_disk_complete_unalign_io;
		sche_req_stack->is_back_bio = false;
		sche_req_stack->pstatus = &sche_req_stack->status;
		sche_req_stack->status.error = 0;
		sche_req_stack->status.count = 0;
		memset(bio->bi_io_vec, 0, sizeof(struct bio_vec) * ioreq_len);

		for (i = 0; i != ioreq_len; i++) {
			page = alloc_page(GFP_KERNEL);
			if (!page)
				goto failed_alloc_page;

			bio_vec = bio->bi_io_vec + i;
			bio_vec->bv_page = page;
			bio_vec->bv_offset = 0;
			bio_vec->bv_len = PB_BLOCK_SIZE;
		}

		/*
		 * The frontbio and backbio are always one block, which means
		 * the inline space in ioreq->privs->dma_addr is large enough.
		 * So we don't need worry about allocing dma_addr_list occured
		 * before the last ioreq issue to pcie.
		 */
		if (pb_is_write_bio(orig_bio)) {
			if ((start & PB_B2S_MASK) || len < PB_B2S_SIZE) {
				/* Link FrontBio */
				bio->bi_rw = 0;
				pb_bio_size(bio) = PB_BLOCK_SIZE;
				ioreq->ioreq_len = 1;
				++sche_req_stack->pstatus->count;
			} else if ((len & PB_B2S_MASK) != 0) {
				/* Link BackBio */
				bio->bi_rw = 0;
				pb_bio_start(bio) = 
					(ioreq_start + ioreq_len - 1) << PB_B2S_SHIFT;
				pb_bio_size(bio) = PB_BLOCK_SIZE;
				ioreq->ioreq_start = ioreq_start + ioreq_len - 1;
				ioreq->ioreq_len = 1;
				++sche_req_stack->pstatus->count;
				sche_req_stack->is_back_bio = true;
			}

			if (sche_req_stack->is_back_bio == false && ioreq_len > 1 &&
			    ((start + len) & PB_B2S_MASK) != 0) {
				/* Link BackBio */
				back_bio = bio_alloc(GFP_KERNEL, ioreq_len);
				if(!back_bio) {
					DPRINTK(ERR, "Falied to alloc back bio\n");
					goto failed_prepare_back;
				}
				back_ioreq = pb_alloc_ioreq(back_bio, disk);
				if (!back_ioreq) {
					DPRINTK(ERR, "Falied to alloc back ioreq\n");
					goto failed_prepare_back;
				}
				status = kmalloc(sizeof(struct pblaze_sche_req_status), GFP_KERNEL);
				if (!status) {
					DPRINTK(ERR, "Falied to alloc status\n");
					goto failed_prepare_back;
				}

				back_sche_req_stack = pb_ioreq_get_sp(back_ioreq);
				pb_ioreq_inc_sp(back_ioreq, sizeof(struct pblaze_disk_sche_req_stack));
				back_sche_req_stack->old_complete_ioreq = back_ioreq->complete_ioreq;
				back_ioreq->complete_ioreq = pblaze_disk_complete_unalign_io;
				back_sche_req_stack->is_back_bio = true;
				back_sche_req_stack->pstatus = status;

				/* Front_ioreq will share the status with back_ioreq */
				sche_req_stack->pstatus = status;
				status->count = 2;
				status->error = 0;

				back_bio->bi_rw = 0;
				pb_bio_start(back_bio) = 
					(ioreq_start + ioreq_len - 1) << PB_B2S_SHIFT;
				pb_bio_size(back_bio) = PB_BLOCK_SIZE;
				back_ioreq->ioreq_start = ioreq_start + ioreq_len - 1;
				back_ioreq->ioreq_len = 1;

				memcpy(back_bio->bi_io_vec, bio->bi_io_vec, ioreq_len * sizeof(struct bio_vec));
				back_bio->bi_io_vec[0].bv_page = bio->bi_io_vec[ioreq_len - 1].bv_page;
				back_bio->bi_io_vec[ioreq_len - 1].bv_page = bio->bi_io_vec[0].bv_page;

				back_bio->bi_private = orig_bio;
				back_bio->bi_end_io = pblaze_disk_end_unalign_io;
				back_bio->bi_vcnt = 1;
			}
		}

		bio->bi_vcnt = pb_bio_size(bio) >> PB_BLOCK_SHIFT;
		pb_acct_stats(disk->gendisk, ioreq);
		if (sche_req_stack->pstatus->count) {
			if (back_ioreq) {
				/* The range locked, dont need check conflict again */
				if (pblaze_raid_prepare_ioreq(disk->raid, ioreq) == 0)
					pblaze_raid_insert_ioreq(disk->raid, ioreq, false);
				if (pblaze_raid_prepare_ioreq(disk->raid, back_ioreq) == 0)
					pblaze_raid_insert_ioreq(disk->raid, back_ioreq, false);
			} else {
				if (pblaze_raid_prepare_ioreq(disk->raid, ioreq) == 0)
					pblaze_raid_insert_ioreq(disk->raid, ioreq, false);
			}
		} else {
			if (pblaze_raid_prepare_ioreq(disk->raid, ioreq) == 0)
				pblaze_raid_insert_ioreq(disk->raid, ioreq, false);
		}
	}

	return 0;

failed_prepare_back:
	kfree(status);
	if (back_ioreq)
		pb_free_ioreq(back_ioreq);
	if (back_bio)
		bio_put(back_bio);

failed_alloc_page:
	for (i = 0; i != ioreq_len; i++) {
		bio_vec = bio->bi_io_vec + i;
		if (bio_vec->bv_page)
			__free_page(bio_vec->bv_page);
	}
failed_prepare_front:
	if (ioreq)
		pb_free_ioreq(ioreq);
	if (bio)
		bio_put(bio);

	pb_clear_conflict(&disk->raid->conflict, ioreq_start, ioreq_len);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 24)
	bio_endio(orig_bio, 0, -ENOMEM);
#else
	bio_endio(orig_bio);
#endif

	return -1;
}

static void pblaze_disk_sche_req(struct pblaze_disk *disk, bool fast)
{
	unsigned long timeout;
	struct io_wrapper *iow;
	struct io_request *ioreq;
	struct bio *bio;

	timeout = jiffies + fast ? HZ / 100 : HZ / 10;
	while ((iow = pblaze_disk_remove_req(disk)) != NULL) {
		if (iow->type == IOW_ALIGN_TYPE) {
			ioreq = (struct io_request *)iow->private;

			if (pblaze_disk_process_io(disk, ioreq) == 1) {
				pblaze_disk_insert_req(iow, disk);
				break;
			} else {
				kfree(iow);
				iow = NULL;
			}
		} else if (iow->type == IOW_UNALIGN_TYPE) {
			bio = (struct bio *)iow->private;

			if (pblaze_disk_process_unalign_io(disk, bio) == 1) {
				pblaze_disk_insert_req(iow, disk);
				break;
			} else {
				kfree(iow);
				iow = NULL;
			}
		} else {
			BUG_ON(1);
		}

		if (time_after(jiffies, timeout)) {
			break;
		}
	}
}

static int pblaze_disk_sche_req_slow(void *arg)
{
	bool is_empty;
	unsigned long timeout;
	struct pblaze_disk *disk = (struct pblaze_disk *)arg;

	DPRINTK(INFO, "++\n");
	kref_get(&disk->refcn);
	timeout = jiffies + HZ;
	while (disk->is_stop == false) {
		spin_lock(&disk->req_list_lock);
		is_empty = list_empty(&disk->req_list);
		spin_unlock(&disk->req_list_lock);
		if (is_empty) {
			if (down_interruptible(&disk->sche_sem)) {
				DPRINTK(ERR, "Wait sche_sem error occur\n");
				break;
			}
		}

		if (time_after(jiffies, timeout)) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ / 100);
			timeout = jiffies + HZ;
		}

		pblaze_disk_sche_req(disk, false);
	}

	kref_put(&disk->refcn, release_pblaze_disk);

	mb();
	disk->is_stoped = true;

	DPRINTK(INFO, "--\n");
	return 0;
}

static void pblaze_disk_request(struct request_queue *queue)
{
	BUG_ON(1);
}

/*
static void pblaze_dump_bio(struct bio *bio)
{
	struct bio_vec *bvec = NULL;
	char *page_addr = NULL;
	int i;

	printk("bio 0x%p, sector %lu, size %d, rw 0x%lx\n",
		   bio, pb_bio_start(bio), pb_bio_size(bio), bio->bi_rw);

	for (i = 0; i < bio->bi_vcnt; i++) {
		bvec = pb_bio_iovec_idx(bio, i); 
		page_addr = page_address(bvec->bv_page);
		printk("start char %c, end char %c!\n",
			   *(page_addr + bvec->bv_offset), 
			   *(page_addr + bvec->bv_offset + bvec->bv_len - 1));
	}
}
*/

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)
int pblaze_disk_make_request(struct request_queue *q, struct bio *bio_orig)
#else
blk_qc_t pblaze_disk_make_request(struct request_queue *q, struct bio *bio_orig)
#endif
{
	int error;
	struct bio *bio = bio_orig;
	struct io_request *ioreq;
	struct pblaze_disk_stack *stack;
	struct pblaze_disk *disk = q->queuedata;
	sector_t start = pb_bio_start(bio);
	sector_t len = bio_sectors(bio);
	struct io_wrapper *iow;
	bool is_unalign;

	if (disk->is_stop) {
		error = -EIO;
		goto failed_endio;
	}

	is_unalign = ((start & PB_B2S_MASK) || (len & PB_B2S_MASK));
	if (pb_is_has_data_bio(bio)) {
		if (is_unalign) {
			iow = kmalloc(sizeof(struct io_wrapper), GFP_KERNEL);
			if (iow == NULL) {
				error = -ENOMEM;
				goto failed_endio;
			}

			memset(iow, 0, sizeof(struct io_wrapper));
			iow->private = bio;
			iow->type = IOW_UNALIGN_TYPE;
			pblaze_disk_insert_req(iow, disk);
			goto out;
		} else {
			/* Partial bio vec will generate 4K unalign dma addr */
			if ((len >> PB_B2S_SHIFT) != bio_segments(bio)) {
				bio = pblaze_disk_clone_loose_bio(bio_orig, (len >> PB_B2S_SHIFT));
				if (bio == NULL) {
					bio = bio_orig;
					error = -ENOMEM;
					goto failed_endio;
				}
			}
		}
	} else if (pb_is_trim_bio(bio)) {
		/* Dma can only process 4K align trim bio */
		if (is_unalign || len == 0) {
			error = 0;
			goto failed_endio;
		}
	} else if (pb_is_barrier_bio(bio) || pb_is_flush_bio(bio)) {
		/* Empty barrier/flush io is not supported */
		if (len == 0) {
			DPRINTK(WARNING, "Barrier/flush io is not supported\n");
			error = -EOPNOTSUPP;
			goto failed_endio;
		}
	} else {
		WARN_ON(1);
		DPRINTK(ERR, "Unknowed io type, flags:%lu, rw:%lu, S:%lu, L:%lu\n",
			(unsigned long)bio->bi_flags, (unsigned long)bio->bi_rw,
			(unsigned long)start, (unsigned long)len);
		error = -EOPNOTSUPP;
		goto failed_endio;
	}

	/* Process 4K align data io, trim io */
	ioreq = pb_alloc_ioreq(bio, disk);
	if (!ioreq) {
		error = -ENOMEM;
		goto failed_endio;
	}

	ioreq->ioreq_start = start >> PB_B2S_SHIFT;
	ioreq->ioreq_len = len >> PB_B2S_SHIFT;
	stack = pb_ioreq_get_sp(ioreq);
	pb_ioreq_inc_sp(ioreq, sizeof(struct pblaze_disk_stack));
	stack->disk = disk;
	stack->old_complete_ioreq = ioreq->complete_ioreq;
	ioreq->complete_ioreq = pblaze_disk_complete_io;
	pb_acct_stats(disk->gendisk, ioreq);

	if ((pblaze_raid_prepare_ioreq(disk->raid, ioreq) == 0) &&
	    (pblaze_raid_insert_ioreq(disk->raid, ioreq, true) == 1)) {
		iow = kmalloc(sizeof(struct io_wrapper), GFP_KERNEL);
		if (iow == NULL) {
			DPRINTK(ERR, "Failed to alloc iow\n");
			error = -ENOMEM;
			goto failed_endio;
		}

		memset(iow, 0, sizeof(struct io_wrapper));
		iow->private = ioreq;
		iow->type = IOW_ALIGN_TYPE;
		pblaze_disk_insert_req(iow, disk);
	}

out:
	pblaze_disk_sche_req(disk, true);
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)
	return 0;
#else
	return BLK_QC_T_NONE;
#endif

failed_endio:
	if (error)
		DPRINTK(ERR, "Failed to handle io, error:%d\n", error);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 24)
	bio_endio(bio, 0, error);
#else
	bio_endio(bio);
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)
	return 0;
#else
	return BLK_QC_T_NONE;
#endif
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 28)
int pblaze_disk_open(struct inode *inode, struct file *filp)
#else
int pblaze_disk_open(struct block_device *bd, fmode_t mode)
#endif
{
	int ret;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 28)
	struct block_device *bd = inode->i_bdev;
#endif
	struct pblaze_virt_dev *virt_dev;
	struct pblaze_disk *disk = bd->bd_disk->private_data;
	struct pblaze_disk *sub_disk;


	if (!disk->virt_dev->is_attached) {
		ret = -ERESTARTSYS;
		goto failed_open;
	}

	virt_dev = pblaze_get_virt_dev(disk->virt_dev->minor);
	sub_disk = (struct pblaze_disk *)dev_get_drvdata(&virt_dev->dev);
	if (sub_disk && (bd->bd_disk != sub_disk->gendisk)) {
		ret = -ERESTARTSYS;
		flush_scheduled_work();
		goto failed_open;
	}

	(void)pb_atomic_inc(&disk->virt_dev->open_handles);
	check_disk_change(bd);

	return 0;

failed_open:
	DPRINTK(ERR, "error code:%d\n", ret);

	return ret;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 28)
int pblaze_disk_release(struct inode *inode, struct file *filp)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
int pblaze_disk_release(struct gendisk *gd, fmode_t mode)
#else
void pblaze_disk_release(struct gendisk *gd, fmode_t mode)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 28)
	struct block_device *bd = inode->i_bdev;
	struct pblaze_disk *disk = bd->bd_disk->private_data;
#else
	struct pblaze_disk *disk = gd->private_data;
#endif

	(void)pb_atomic_dec(&disk->virt_dev->open_handles);

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
	return 0;
#endif
}

int pblaze_disk_media_changed(struct gendisk *gd)
{
	struct pblaze_disk *disk = gd->private_data;

	return get_capacity(gd) != pblaze_virt_dev_get_size(disk->virt_dev);
}

int pblaze_disk_revalidate(struct gendisk *gd)
{
	sector_t size;
	struct pblaze_disk *disk = gd->private_data;

	DPRINTK(INFO, "++\n");
	size = pblaze_virt_dev_get_size(disk->virt_dev);
	set_capacity(gd, size);
	DPRINTK(INFO, "--\n");

	return 0;
}

int pblaze_disk_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	struct pblaze_disk* disk;

	DPRINTK(DEBUG, "++\n");
	disk = bdev->bd_disk->private_data;
	geo->heads = 0x40;
	geo->sectors = 0x20;
	geo->cylinders = pblaze_virt_dev_get_size(disk->virt_dev) >> PB_S2MB_SHIFT;
	DPRINTK(DEBUG, "--\n");

	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 28)
int pblaze_disk_ioctl(struct inode *inode, struct file *filp,
		      unsigned int cmd, unsigned long arg)
{
	struct block_device *bd = inode->i_bdev;
#else
int pblaze_disk_ioctl(struct block_device *bd, fmode_t mode,
		      unsigned int cmd, unsigned long arg)
{
#endif
	int ret = 0;
	struct hd_geometry geo;

	switch (cmd) {
	case HDIO_GETGEO:
		pblaze_disk_getgeo(bd, &geo);
		if (unlikely(!arg))
			ret = -EINVAL;
		else {
			if (copy_to_user((void __user *)arg, &geo, sizeof(geo)))
				ret = -EFAULT;
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 32)
struct block_device_operations pblaze_disk_ops = {
#else
const struct block_device_operations pblaze_disk_ops = {
#endif
	.open = pblaze_disk_open,
	.release = pblaze_disk_release,
	.media_changed = pblaze_disk_media_changed,
	.revalidate_disk = pblaze_disk_revalidate,
	.ioctl = pblaze_disk_ioctl,
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 9)
	.getgeo = pblaze_disk_getgeo,
#endif
	.owner = THIS_MODULE,
};

static void blk_queue_congestion_threshold(struct request_queue *q)
{
	int nr;

	nr = q->nr_requests - (q->nr_requests / 8) + 1;
	if (nr > q->nr_requests)
		nr = q->nr_requests;
	q->nr_congestion_on = nr;

	nr = q->nr_requests - (q->nr_requests / 8) - (q->nr_requests / 16) - 1;
	if (nr < 1)
		nr = 1;
	q->nr_congestion_off = nr;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
static int pblaze_disk_read_proc(char *page, char **start, off_t off,
				 int count, int *eof, void *data)
{
	struct pblaze_disk *disk = (struct pblaze_disk *)data;
	int ret = 0;

	if (off != 0)
		goto end;

	ret += sprintf(page + ret, "Disk Name: %s\n", disk->gendisk->disk_name);
	ret += sprintf(page + ret, "io_counter: %u\n", disk->io_counter);
	ret += sprintf(page + ret, "req_list: %llu\n", disk->req_list_length);
	ret += sprintf(page + ret, "is_stop: %u, is_stoped %u\n", 
		       disk->is_stop, disk->is_stoped);
end:
	*eof = 1;
	return ret;
}

static int pblaze_create_proc_for_disk(struct pblaze_disk *disk)
{
	struct proc_dir_entry *ent;

	disk->proc_dir = proc_mkdir(disk->gendisk->disk_name, NULL);
	if (!disk->proc_dir) {
		return -ENOMEM;
	}

	ent = create_proc_entry(DISK_PROC_NAME, 0444, disk->proc_dir);
	if (!ent) {
		return -ENOMEM;
	}

	ent->read_proc = pblaze_disk_read_proc;
	ent->write_proc = NULL;
	ent->data = disk;

	return 0;
}
#define	PBLAZE_DISK_CREATE_PROC(x)	pblaze_create_proc_for_disk(x)
#else
static int pblaze_disk_seq_read(struct seq_file *m, void *v)
{
	struct pblaze_disk *disk = (struct pblaze_disk *)m->private;

	seq_printf(m, "Disk Name: %s\n", disk->gendisk->disk_name);
	seq_printf(m, "io_counter: %u\n", disk->io_counter);
	seq_printf(m, "is_stop: %u, is_stoped %u\n", 
		       disk->is_stop, disk->is_stoped);
	seq_printf(m, "req_list: %llu\n", disk->req_list_length);
	return 0;
}

static int pblaze_disk_open_proc(struct inode *inode, struct file *file)
{
	return single_open(file, pblaze_disk_seq_read, PDE_DATA(inode));
}

static const struct file_operations pb_proc_disk_operations = {
	.owner		= THIS_MODULE,
	.open		= pblaze_disk_open_proc,
	.read		= seq_read,
	.write		= NULL,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int pblaze_create_seq_for_disk(struct pblaze_disk *disk)
{
	struct proc_dir_entry *ent;

	disk->proc_dir = proc_mkdir(disk->gendisk->disk_name, NULL);
	if (!disk->proc_dir) {
		return -ENOMEM;
	}

	ent = proc_create_data(DISK_PROC_NAME, 0444, disk->proc_dir, &pb_proc_disk_operations, disk);
	if (ent == NULL) {
		DPRINTK(ERR, "Create entry stat failed\n");
		return -ENOMEM;
	}

	return 0;
}
#define	PBLAZE_DISK_CREATE_PROC(x)	pblaze_create_seq_for_disk(x)
#endif


static int __devinit pblaze_disk_probe(struct pblaze_virt_dev *virt_dev)
{
	int ret = 0;
	u32 minor;
	struct pblaze_disk *disk;

	DPRINTK(DEBUG, "disk probe\n");

	disk = kmalloc(sizeof(struct pblaze_disk), GFP_KERNEL);
	if (!disk) {
		ret = -ENOMEM;
		goto failed_alloc_disk;
	}

	memset(disk, 0, sizeof(struct pblaze_disk));
	kref_init(&disk->refcn);
	dev_set_drvdata(&virt_dev->dev, disk);
	init_waitqueue_head(&disk->io_wq);
	spin_lock_init(&disk->req_list_lock);
	INIT_LIST_HEAD(&disk->req_list);

	disk->virt_dev = virt_dev;
	disk->raid = virt_dev->parent_dev;

	/* Create request sche thread  */
	init_MUTEX_LOCKED(&disk->sche_sem);
	disk->sche0_thread = kthread_run(pblaze_disk_sche_req_slow, disk, "pb_sche0");
	if (IS_ERR(disk->sche0_thread)) {
		ret = PTR_ERR(disk->sche0_thread);
		disk->sche0_thread = NULL;
		goto failed_create_sche0;
	}

	/* Create request queue for pblaze disk */
	spin_lock_init(&disk->queue_lock);
	disk->queue = blk_init_queue(pblaze_disk_request, &disk->queue_lock);
	if (!disk->queue) {
		ret = -ENOMEM;
		goto failed_init_queue;
	}

	disk->queue->nr_requests = INTERRUPT_QUEUE_SIZE;
	blk_queue_congestion_threshold(disk->queue);
	disk->queue->make_request_fn = pblaze_disk_make_request;
	disk->queue->queuedata = disk;

	/* DMA controller does not support FLUSH primitive */
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 36)
	blk_queue_flush(disk->queue, 0);
#else
	blk_queue_ordered(disk->queue, QUEUE_ORDERED_NONE, NULL);
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 30)
	blk_queue_hardsect_size(disk->queue, PB_SECTOR_SIZE);
#else
	blk_queue_logical_block_size(disk->queue, PB_SECTOR_SIZE);
	blk_queue_physical_block_size(disk->queue, PB_SECTOR_SIZE);
	blk_queue_io_min(disk->queue, PB_SECTOR_SIZE);
	blk_queue_io_opt(disk->queue, PB_BLOCK_SIZE);
#ifdef TRIM_SUPPORT
#ifdef RHEL_RELEASE_CODE
	blk_queue_discard_granularity(disk->queue, PB_BLOCK_SIZE);
#endif
	queue_flag_set_unlocked(QUEUE_FLAG_DISCARD, disk->queue);
	blk_queue_max_discard_sectors(disk->queue, PB_BLOCK_SIZE / PB_SECTOR_SIZE);
#endif
#endif

	blk_queue_bounce_limit(disk->queue, BLK_BOUNCE_ANY);
	blk_queue_dma_alignment(disk->queue, 7);

	/*
	 * blk_queue_max_hw_segments Enables a low level driver to set an upper
	 * limit on the number of hw data segments in a request. This would be
	 * the largest number of address/length pairs the host adapter can
	 * actually give at once to the device.
	 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 34) && \
    (!defined(RHEL_RELEASE_CODE) || RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(6, 0))
	blk_queue_max_hw_segments(disk->queue, (MAX_DMA_LENGTH - 1) << PB_B2S_SHIFT);
#else
	blk_queue_max_segments(disk->queue, (MAX_DMA_LENGTH - 1) << PB_B2S_SHIFT);
#endif

	/*
	 * blk_queue_max_segment_size Enables a low level driver to set an upper
	 * limit on the size of a coalesced segment
	 */
	blk_queue_max_segment_size(disk->queue, PB_BLOCK_SIZE);

	/*
	 * Care should be paid to sector length so that multi thread long
	 * requests are not intersected and lead to random write.
	 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 30)
	blk_queue_max_sectors(disk->queue, (MAX_DMA_LENGTH - 1) << PB_B2S_SHIFT);
#else
	blk_queue_max_hw_sectors(disk->queue, (MAX_DMA_LENGTH - 1) << PB_B2S_SHIFT);
#endif

	/* Create gendisk for pblaze disk and set pblaze_disk_ops */
	disk->gendisk = alloc_disk(PB_DISK_MINORS);
	if (!disk->gendisk)
		goto failed_alloc_gendisk;

	disk->gendisk->major = disk_driver.major;
	minor = virt_dev->minor;
	disk->gendisk->first_minor = minor * PB_DISK_MINORS;
	disk->gendisk->minors = PB_DISK_MINORS;
	disk->gendisk->fops = &pblaze_disk_ops;
	disk->gendisk->queue = disk->queue;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 28)
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, disk->gendisk->queue);
#endif
	disk->gendisk->private_data = disk;
	snprintf(disk->gendisk->disk_name, 32, "memdisk%c", minor + 'a');

	set_capacity(disk->gendisk, pblaze_virt_dev_get_size(disk->virt_dev));
	add_disk(disk->gendisk);

	if (PBLAZE_DISK_CREATE_PROC(disk)) {
		DPRINTK(ERR, "Create proc for disk failed.\n");
		goto failed_create_proc;
	}

	DPRINTK(DEBUG, "virt dev probe success\n");

	return 0;

failed_create_proc:
	del_gendisk(disk->gendisk);
failed_alloc_gendisk:
	blk_cleanup_queue(disk->queue);
failed_init_queue:
	disk->is_stop = true;
	up(&disk->sche_sem);
failed_create_sche0:
	disk->sche0_thread = NULL;
	kref_put(&disk->refcn, release_pblaze_disk); /* For disk alloc */
failed_alloc_disk:
	return ret;
}

static void pblaze_disk_set_capacity(struct pblaze_virt_dev *virt_dev)
{
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 9, 1)
static void pblaze_disk_remove_proc(struct pblaze_disk *disk)
{
	remove_proc_entry(DISK_PROC_NAME, disk->proc_dir);
	remove_proc_entry(disk->gendisk->disk_name, disk->proc_dir->parent);
	disk->proc_dir = NULL;
}
#define	PBLAZE_DISK_REMOVE_PROC(x)	pblaze_disk_remove_proc(x)
#else
static void pblaze_disk_remove_seq(struct pblaze_disk *disk)
{
	remove_proc_subtree(disk->gendisk->disk_name, NULL);
	disk->proc_dir = NULL;
}
#define	PBLAZE_DISK_REMOVE_PROC(x)	pblaze_disk_remove_seq(x)
#endif

static void __devexit pblaze_disk_remove(struct pblaze_virt_dev *virt_dev)
{
	struct pblaze_disk *disk;

	disk = (struct pblaze_disk *)dev_get_drvdata(&virt_dev->dev);
	DPRINTK(INFO, "disk remove:%s\n", disk->gendisk->disk_name);

	disk->is_stop = true;

	PBLAZE_DISK_REMOVE_PROC(disk);

	while (true) {
		spin_lock(&disk->req_list_lock);
		if (list_empty(&disk->req_list)) {
			spin_unlock(&disk->req_list_lock);
			break;
		} else {
			DPRINTK(ERR, "disk req list is not empty\n");
		}
		spin_unlock(&disk->req_list_lock);

		pblaze_disk_sche_req(disk, false);
	}

	/* wait until disk sche thread exit */
	up(&disk->sche_sem);
	mb();
	while (disk->is_stoped == false) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(HZ / 100);
	}

	blk_cleanup_queue(disk->queue);
	del_gendisk(disk->gendisk);
	put_disk(disk->gendisk);
	kref_put(&disk->refcn, release_pblaze_disk); /* For disk alloc */

	DPRINTK(INFO, "disk remove done\n");
}

static void pblaze_disk_shutdown(struct pblaze_virt_dev *virt_dev)
{
	struct pblaze_disk *disk;

	disk = (struct pblaze_disk *)dev_get_drvdata(&virt_dev->dev);
	DPRINTK(WARNING, "pblaze disk shutdown:%s\n", disk->gendisk->disk_name);

	disk->is_stop = true;
	while (true) {
		spin_lock(&disk->req_list_lock);
		if (list_empty(&disk->req_list)) {
			spin_unlock(&disk->req_list_lock);
			break;
		} else {
			DPRINTK(WARNING, "disk req list is not empty\n");
		}
		spin_unlock(&disk->req_list_lock);

		pblaze_disk_sche_req(disk, false);
	}

	/* wait until disk sche thread exit */
	up(&disk->sche_sem);
	mb();
	while (disk->is_stoped == false) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(HZ / 100);
	}

	DPRINTK(WARNING, "pblaze disk shutdown:%s done\n", disk->gendisk->disk_name);
}

static int memdisk_init(void)
{
	int ret = 0;

	disk_driver.major = register_blkdev(disk_driver.major, "memdisk");
	if (unlikely(disk_driver.major < 0)) {
		DPRINTK(ERR, "memhd unable to get major number\n");
		ret = disk_driver.major;
		goto failed_register_blkdev;
	}

	ret = pblaze_virt_drv_register(&disk_driver.driver, THIS_MODULE,
				       "memdisk_mod");
	if (ret < 0) {
		DPRINTK(ERR, "memhd_register_driver failed\n");
		goto failed_register_virt_driver;
	}

	return 0;

failed_register_virt_driver:
	unregister_blkdev(disk_driver.major, "memdisk");
failed_register_blkdev:
	DPRINTK(ERR, "memdisk init failed with %d", ret);
	return ret;
}

static void memdisk_exit(void)
{
	DPRINTK(DEBUG, "pblaze disk exit\n");

	pblaze_virt_drv_unregister(&disk_driver.driver);
	unregister_blkdev(disk_driver.major, "memdisk");
}

module_init(memdisk_init);
module_exit(memdisk_exit);
