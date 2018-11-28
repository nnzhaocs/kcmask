/*
 *  linux/mm/page_io.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Swap reorganised 29.12.95, 
 *  Asynchronous swapping added 30.12.95. Stephen Tweedie
 *  Removed race in async swapping. 14.4.1996. Bruno Haible
 *  Add swap of shared pages through the page cache. 20.2.1998. Stephen Tweedie
 *  Always use brw_page, life becomes simpler. 12 May 1998 Eric Biederman
 */

#include <linux/mm.h>
#include <linux/kernel_stat.h>
#include <linux/gfp.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/bio.h>
#include <linux/swapops.h>
#include <linux/buffer_head.h>
#include <linux/writeback.h>
#include <linux/frontswap.h>
#include <linux/aio.h>
#include <linux/blkdev.h>
#include <asm/pgtable.h>
static int swap_writepage_modified(struct page *page, struct writeback_control *wbc);
static int swap_writepage_tobuffer(struct page *page);
static int swap_readpage_modified(struct page *page);
static int swap_readpage_frombuffer(struct page *page);
static int swap_readpage_normal(struct page *page);
static int swap_writepage_normal(struct page *page, struct writeback_control *wbc);
static void get_flag_amb_entry(void);

void __inline__ asm_call_interrupt(void)
{
	{
		 __asm__ __volatile__ ("int $238");
	}
	return ;
}
static struct bio *get_swap_bio(gfp_t gfp_flags,
				struct page *page, bio_end_io_t end_io)
{
	struct bio *bio;

	bio = bio_alloc(gfp_flags, 1);
	if (bio) {
		bio->bi_sector = map_swap_page(page, &bio->bi_bdev);
		bio->bi_sector <<= PAGE_SHIFT - 9;
		bio->bi_io_vec[0].bv_page = page;
		bio->bi_io_vec[0].bv_len = PAGE_SIZE;
		bio->bi_io_vec[0].bv_offset = 0;
		bio->bi_vcnt = 1;
		bio->bi_size = PAGE_SIZE;
		bio->bi_end_io = end_io;
	}
	return bio;
}

void end_swap_bio_write(struct bio *bio, int err)
{
	const int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	struct page *page = bio->bi_io_vec[0].bv_page;

	if (!uptodate) {
		SetPageError(page);
		/*
		 * We failed to write the page out to swap-space.
		 * Re-dirty the page in order to avoid it being reclaimed.
		 * Also print a dire warning that things will go BAD (tm)
		 * very quickly.
		 *
		 * Also clear PG_reclaim to avoid rotate_reclaimable_page()
		 */
		set_page_dirty(page);
		printk(KERN_ALERT "Write-error on swap-device (%u:%u:%Lu)\n",
				imajor(bio->bi_bdev->bd_inode),
				iminor(bio->bi_bdev->bd_inode),
				(unsigned long long)bio->bi_sector);
		ClearPageReclaim(page);
	}
	end_page_writeback(page);
	bio_put(bio);
}

void end_swap_bio_read(struct bio *bio, int err)
{
	const int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	struct page *page = bio->bi_io_vec[0].bv_page;

	if (!uptodate) {
		SetPageError(page);
		ClearPageUptodate(page);
		printk(KERN_ALERT "Read-error on swap-device (%u:%u:%Lu)\n",
				imajor(bio->bi_bdev->bd_inode),
				iminor(bio->bi_bdev->bd_inode),
				(unsigned long long)bio->bi_sector);
	} else {
		SetPageUptodate(page);
	}
	unlock_page(page);
	bio_put(bio);
}

int generic_swapfile_activate(struct swap_info_struct *sis,
				struct file *swap_file,
				sector_t *span)
{
	struct address_space *mapping = swap_file->f_mapping;
	struct inode *inode = mapping->host;
	unsigned blocks_per_page;
	unsigned long page_no;
	unsigned blkbits;
	sector_t probe_block;
	sector_t last_block;
	sector_t lowest_block = -1;
	sector_t highest_block = 0;
	int nr_extents = 0;
	int ret;

	blkbits = inode->i_blkbits;
	blocks_per_page = PAGE_SIZE >> blkbits;

	/*
	 * Map all the blocks into the extent list.  This code doesn't try
	 * to be very smart.
	 */
	probe_block = 0;
	page_no = 0;
	last_block = i_size_read(inode) >> blkbits;
	while ((probe_block + blocks_per_page) <= last_block &&
			page_no < sis->max) {
		unsigned block_in_page;
		sector_t first_block;

		first_block = bmap(inode, probe_block);
		if (first_block == 0)
			goto bad_bmap;

		/*
		 * It must be PAGE_SIZE aligned on-disk
		 */
		if (first_block & (blocks_per_page - 1)) {
			probe_block++;
			goto reprobe;
		}

		for (block_in_page = 1; block_in_page < blocks_per_page;
					block_in_page++) {
			sector_t block;

			block = bmap(inode, probe_block + block_in_page);
			if (block == 0)
				goto bad_bmap;
			if (block != first_block + block_in_page) {
				/* Discontiguity */
				probe_block++;
				goto reprobe;
			}
		}

		first_block >>= (PAGE_SHIFT - blkbits);
		if (page_no) {	/* exclude the header page */
			if (first_block < lowest_block)
				lowest_block = first_block;
			if (first_block > highest_block)
				highest_block = first_block;
		}

		/*
		 * We found a PAGE_SIZE-length, PAGE_SIZE-aligned run of blocks
		 */
		ret = add_swap_extent(sis, page_no, 1, first_block);
		if (ret < 0)
			goto out;
		nr_extents += ret;
		page_no++;
		probe_block += blocks_per_page;
reprobe:
		continue;
	}
	ret = nr_extents;
	*span = 1 + highest_block - lowest_block;
	if (page_no == 0)
		page_no = 1;	/* force Empty message */
	sis->max = page_no;
	sis->pages = page_no - 1;
	sis->highest_bit = page_no - 1;
out:
	return ret;
bad_bmap:
	printk(KERN_ERR "swapon: swapfile has holes\n");
	ret = -EINVAL;
	goto out;
}

//#define RESERVED_MEMORY_OFFSET  0x100000000     /* Offset is 4GB */
u8 *reserved_memory = NULL;
EXPORT_SYMBOL_GPL(reserved_memory);

/* Reserved memory addressing
 * --------------------------------------------------------------------------------------------
 * We map the reserved memory physical address to a virtual address in KCMASK module initially.
 * Always check if reserved_memory == NULL or not.
 * If it's null, instead of throwing an error, we simply write/read the data into backend
 * swap devices using normal swap_writepage_normal or swap_readpage_normal.
 * */

atomic_t modified_hw = ATOMIC_INIT(1);
EXPORT_SYMBOL_GPL(modified_hw);

/* Identify modified hardware or normal hardware by using a simple atomic variable
 * ---------------------------------------------------------------------------------------------
 * Use modified_hw to identify the hardware type: normal or modified.
 * normal: running as norm hardware.
 * modified: not executing the interrupt routine for 'int 238'.
 * life becomes simple!
 * */
/*
enum swap_rw_modified_ops {
	SWAP_WRITEPAGE_TOBUFFER,
	SWAP_WRITEPAGE_NORMAL,
	SWAP_READPAGE_FROMBUFFER
};
*/

//EXPORT_SYMBOL_GPL(swap_rw_modified_ops);

/* Three major operations: write to buffer, write to swap device, and read from buffer.
 * Here buffer is reserved mem.
 * ---------------------------------------------------------------------------------------------
 * SWAP_WRITEPAGE_TOBUFFER
 * Initially, we set the flag area in the reserved mem to SWAP_WRITEPAGE_TOBUFFER, meaning that
 * the all swap-out pages are writting to the reserved mem sequentially (since our reserve mem
 * can only hold one pagedata) instead of backend swap devices.
 * The hardware will read data from reserved mem and store it in our secret compression area.
 *-----------------------------------------------------------------------------------------------
 * SWAP_WRITEPAGE_NORMAL
 * Hardware sets flag area in the reserved mem to SWAP_WRITEPAGE_NORMAL, meaning that our secret
 * compression area is full so we need to do real swap-out (i.e., writing pagedata to backend swap
 * devices) to free some space in our secret compression area.
 * We have to change flag to our default flag (SWAP_WRITEPAGE_TOBUFFER) after we are done.
 * -----------------------------------------------------------------------------------------------
 * SWAP_READPAGE_FROMBUFFER
 * Hardware sets flag area in the reserved mem to SWAP_READPAGE_FROMBUFFER, meaning that the page
 * that caused page fault is in our secret compression area and this page is stored in the reserved
 * mem by hardware. Instead of reading from swap device, we read from the reserved mem.
 * We have to change flag to our default flag (SWAP_WRITEPAGE_TOBUFFER) after we are done.
 * ------------------------------------------------------------------------------------------------
 * */

//struct amb_entry {
//	pgoff_t offset;
//	enum swap_rw_modified_ops flag;
	//u8 mem_test[4096];
//};
//u8 mem_t[8192] = {0};
//struct amb_area {
//	struct amb_entry entry;
//	spinlock_t lock;
//};

struct amb_area amb_test;
EXPORT_SYMBOL_GPL(amb_test);

u8 flag_handle = 0; //1KB
u8 v_address_handle = 1; //8KB unsigned long
//u8 v_address_handle = 1 + flag_handle; //8KB unsigned long
//u8 sector_address_handle = 8 + v_address_handle; // u64
u8 sector_address_handle = 9; // u64
//u8 pagedata_handle = 8 + sector_address_handle; //4 kb
u8 pagedata_handle = 17; //4 kb

EXPORT_SYMBOL_GPL(flag_handle);

u8 flag_len = 1;
u8 v_address_len = 8;
u8 sector_address_len = 8;
u16 pagedata_len = 4096;

/*  reserved memory stack
 * 					       	offset (B):
 * |--------------------|		0
 * |       flag (1B)    |
 * |--------------------|		1
 * |virtual address (8B)|
 * |--------------------|		9
 * | swap address (8B)  |
 * |--------------------|		17
 * |   pagedata (4KB)   |
 * |--------------------|		4017
 */

int swap_writepage(struct page *page, struct writeback_control *wbc)
{
	int ret = 0;

	if (try_to_free_swap(page)) {
		unlock_page(page);
		goto out;
	}

	if (frontswap_store(page) == 0) {
		set_page_writeback(page);
		unlock_page(page);
		end_page_writeback(page);
		goto out;
	}

	printk("call asm interrupt to identify HW\n");
	asm_call_interrupt();
	printk("finished interrupt handler\n");

	if (atomic_read(&modified_hw) == 0){
		//traditional operations
		return swap_writepage_normal(page, wbc);
	}
	//write to buffer or to swap device
	return swap_writepage_modified(page, wbc);
out:
	return ret;
}

/*
 * 1. kcmask module should be loaded at booting. Otherwise, write/read reqs goto swap devices as normal.
 * 2. or can we set amb_test.entry.flag in the interrupt handler?
 * TODO:
 * 3. Coordinate the emu swap_rw_modified_ops flag with hardware's flag settings.
 * 4. we could make two reserved mems: one for write and another one for read.
 * */

static int swap_writepage_modified(struct page *page, struct writeback_control *wbc)
{
	int ret = 0;
	enum swap_rw_modified_ops flag = SWAP_WRITEPAGE_TOBUFFER;

	printk("Modified_HW: amb_memory_address dest = %lx\n", (long unsigned int)reserved_memory);

	if (NULL == reserved_memory) {
		printk("Get NULL pointer Err...\n");
		ret = -1;
		goto out;
	}

	get_flag_amb_entry();
	if (amb_test.entry.flag == SWAP_WRITEPAGE_TOBUFFER){
		printk("SWAP_WRITEPAGE_TOBUFFER");
		return swap_writepage_tobuffer(page);
	}
	else if (amb_test.entry.flag == SWAP_WRITEPAGE_NORMAL){
		// write to swap
		printk("SWAP_WRITEPAGE_NORMAL, it should not happen???");

		spin_lock(&amb_test.lock);
		memcpy(reserved_memory+flag_handle, &flag, flag_len);
		spin_unlock(&amb_test.lock);
		goto out;
	}
	else if (amb_test.entry.flag == SWAP_READPAGE_FROMBUFFER){
		printk("SWAP_READPAGE_FROMBUFFER, it should not happen!");
		spin_lock(&amb_test.lock);
		memcpy(reserved_memory+flag_handle, &flag, flag_len);
		spin_unlock(&amb_test.lock);

		ret = -1;
		goto out;
		//return swap_readpage_frombuffer(page);
	}
	else{
		ret = -1;
		printk("Unkown swap_rw_ops: %d Err...\n", amb_test.entry.flag);
		goto out;
	}

out:
	return swap_writepage_normal(page, wbc);
}

static int swap_writepage_tobuffer(struct page *page)
{
	unsigned long *src;
	sector_t sector;
        int ret = 0;

	if (reserved_memory){
		sector = (sector_t)__page_file_index(page) << (PAGE_SHIFT - 9);//swap_page_sector(page);
		src = kmap_atomic(page);

		spin_lock(&amb_test.lock);
		memcpy(reserved_memory+sector_address_handle, &sector, sector_address_len);

		memcpy(reserved_memory+pagedata_handle, src, pagedata_len);
		spin_unlock(&amb_test.lock);

		kunmap_atomic(src);

		printk("sector: %ld, page is %lx\n", sector, (long unsigned int)page);
		//flag just leave it. it still == SWAP_WRITEPAGE_TOBUFFER.
	}

	return ret;
}

static void get_flag_amb_entry(void)
{
	spin_lock(&amb_test.lock);
	//dst, src, size
	memcpy(&amb_test.entry.flag, reserved_memory+flag_handle, flag_len);
	spin_unlock(&amb_test.lock);
}

//static void get_pagedata_amb_entry()
//{
//	spin_lock(&amb_test->lock);
//	//dst, src, size
//	memcpy(&amb_test.entry.mem_test, reserved_memory+pagedata_handle, pagedata_len);
//	spin_lock(&amb_test->lock);
//}

static int swap_writepage_normal(struct page *page, struct writeback_control *wbc)
{
	int ret = 0;
	ret = __swap_writepage(page, wbc, end_swap_bio_write);
	return ret;
}



static sector_t swap_page_sector(struct page *page)
{
	return (sector_t)__page_file_index(page) << (PAGE_CACHE_SHIFT - 9);
}

int __swap_writepage(struct page *page, struct writeback_control *wbc,
	void (*end_write_func)(struct bio *, int))
{
	struct bio *bio;
	int ret, rw = WRITE;
	struct swap_info_struct *sis = page_swap_info(page);

	if (sis->flags & SWP_FILE) {
		struct kiocb kiocb;
		struct file *swap_file = sis->swap_file;
		struct address_space *mapping = swap_file->f_mapping;
		struct iovec iov = {
			.iov_base = kmap(page),
			.iov_len  = PAGE_SIZE,
		};

		init_sync_kiocb(&kiocb, swap_file);
		kiocb.ki_pos = page_file_offset(page);
		kiocb.ki_left = PAGE_SIZE;
		kiocb.ki_nbytes = PAGE_SIZE;

		set_page_writeback(page);
		unlock_page(page);
		ret = mapping->a_ops->direct_IO(KERNEL_WRITE,
						&kiocb, &iov,
						kiocb.ki_pos, 1);
		kunmap(page);
		if (ret == PAGE_SIZE) {
			count_vm_event(PSWPOUT);
			ret = 0;
		} else {
			/*
			 * In the case of swap-over-nfs, this can be a
			 * temporary failure if the system has limited
			 * memory for allocating transmit buffers.
			 * Mark the page dirty and avoid
			 * rotate_reclaimable_page but rate-limit the
			 * messages but do not flag PageError like
			 * the normal direct-to-bio case as it could
			 * be temporary.
			 */
			set_page_dirty(page);
			ClearPageReclaim(page);
			pr_err_ratelimited("Write error on dio swapfile (%Lu)\n",
				page_file_offset(page));
		}
		end_page_writeback(page);
		return ret;
	}

	ret = bdev_write_page(sis->bdev, swap_page_sector(page), page, wbc);
	if (!ret) {
		count_vm_event(PSWPOUT);
		return 0;
	}

	ret = 0;
	bio = get_swap_bio(GFP_NOIO, page, end_write_func);
	if (bio == NULL) {
		set_page_dirty(page);
		unlock_page(page);
		ret = -ENOMEM;
		goto out;
	}
	if (wbc->sync_mode == WB_SYNC_ALL)
		rw |= REQ_SYNC;
	count_vm_event(PSWPOUT);
	set_page_writeback(page);
	unlock_page(page);
	submit_bio(rw, bio);
out:
	return ret;
}

int swap_readpage(struct page *page)
{
	if (atomic_read(&modified_hw) == 0){
		//traditional operations
		return swap_readpage_normal(page);
	}

	return swap_readpage_modified(page);
}

static int swap_readpage_modified(struct page *page)
{
	int ret = 0;
	enum swap_rw_modified_ops flag = SWAP_WRITEPAGE_TOBUFFER;

	printk("Modified_HW: amb_memory_address dest = %lx\n", (long unsigned int)reserved_memory);

	if (NULL == reserved_memory) {
		printk("Get NULL pointer Err...\n");
		ret = -1;
		goto out;
	}

	get_flag_amb_entry();
	if (amb_test.entry.flag == SWAP_WRITEPAGE_TOBUFFER){
		//write to buffer
		printk("SWAP_WRITEPAGE_TOBUFFER, it should not happen!");
		goto out; //swap_writepage_tobuffer(struct page *page);
	}
	else if (amb_test.entry.flag == SWAP_WRITEPAGE_NORMAL){
		struct writeback_control wbc = {
		        .sync_mode = WB_SYNC_NONE,
		};
		// write to swap
		printk("SWAP_WRITEPAGE_NORMAL, mean compressed mem is full");

		//reset flag
		spin_lock(&amb_test.lock);
		memcpy(reserved_memory+flag_handle, &flag, flag_len);
		spin_unlock(&amb_test.lock);

		return swap_writepage_normal(page, &wbc);
	}
	else if (amb_test.entry.flag == SWAP_READPAGE_FROMBUFFER){
		printk("SWAP_READPAGE_FROMBUFFER, mean data found in our compress mem");
		return swap_readpage_frombuffer(page);
	}
	else{
		printk("Unkown swap_rw_ops: %d Err...\n", amb_test.entry.flag);
		ret = -1;
		goto out;
	}

out:
	return swap_readpage_normal(page);
}

static int swap_readpage_frombuffer(struct page *page)
{
	unsigned long *dst;
	//int ret;
	enum swap_rw_modified_ops flag = SWAP_WRITEPAGE_TOBUFFER;

	dst = kmap_atomic(page);
	spin_lock(&amb_test.lock);
	//dst, src, size
	memcpy(dst, reserved_memory+pagedata_handle, pagedata_len);
	//reset flag to writebuffer;
	memcpy(reserved_memory+flag_handle, &flag, flag_len);

	spin_unlock(&amb_test.lock);
	kunmap_atomic(dst);

	return 0;
}

static int swap_readpage_normal(struct page *page)
{
	struct bio *bio;
	int ret = 0;
	struct swap_info_struct *sis = page_swap_info(page);

	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(PageUptodate(page), page);
	if (frontswap_load(page) == 0) {
		SetPageUptodate(page);
		unlock_page(page);
		goto out;
	}

	if (sis->flags & SWP_FILE) {
		struct file *swap_file = sis->swap_file;
		struct address_space *mapping = swap_file->f_mapping;

		ret = mapping->a_ops->readpage(swap_file, page);
		if (!ret)
			count_vm_event(PSWPIN);
		return ret;
	}

	ret = bdev_read_page(sis->bdev, swap_page_sector(page), page);
	if (!ret) {
		count_vm_event(PSWPIN);
		return 0;
	}

	ret = 0;
	bio = get_swap_bio(GFP_KERNEL, page, end_swap_bio_read);
	if (bio == NULL) {
		unlock_page(page);
		ret = -ENOMEM;
		goto out;
	}
	count_vm_event(PSWPIN);
	submit_bio(READ, bio);
out:
	return ret;
}

int swap_set_page_dirty(struct page *page)
{
	struct swap_info_struct *sis = page_swap_info(page);

	if (sis->flags & SWP_FILE) {
		struct address_space *mapping = sis->swap_file->f_mapping;
		return mapping->a_ops->set_page_dirty(page);
	} else {
		return __set_page_dirty_no_writeback(page);
	}
}
