/*
 *  linux/fs/ext4/file.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/file.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext4 fs regular file handling primitives
 *
 *  64-bit file support on 64-bit platforms by Jakub Jelinek
 *	(jj@sunsite.ms.mff.cuni.cz)
 */

#include <linux/time.h>
#include <linux/fs.h>
#include <linux/jbd2.h>
#include <linux/iomap.h>
#include <linux/mount.h>
#include <linux/path.h>
#include <linux/dax.h>
#include <linux/aio.h>
#include <linux/quotaops.h>
#include <linux/pagevec.h>
#include "ext4.h"
#include "ext4_jbd2.h"
#include "xattr.h"
#include "acl.h"

/*
 * Called when an inode is released. Note that this is different
 * from ext4_file_open: open gets called at every open, but release
 * gets called only when /all/ the files are closed.
 */
static int ext4_release_file(struct inode *inode, struct file *filp)
{
	if (ext4_test_inode_state(inode, EXT4_STATE_DA_ALLOC_CLOSE)) {
		ext4_alloc_da_blocks(inode);
		ext4_clear_inode_state(inode, EXT4_STATE_DA_ALLOC_CLOSE);
	}
	/* if we are the last writer on the inode, drop the block reservation */
	if ((filp->f_mode & FMODE_WRITE) &&
			(atomic_read(&inode->i_writecount) == 1) &&
		        !EXT4_I(inode)->i_reserved_data_blocks)
	{
		down_write(&EXT4_I(inode)->i_data_sem);
		ext4_discard_preallocations(inode);
		up_write(&EXT4_I(inode)->i_data_sem);
	}
	if (is_dx(inode) && filp->private_data)
		ext4_htree_free_dir_info(filp->private_data);

	return 0;
}

static void ext4_unwritten_wait(struct inode *inode)
{
	wait_queue_head_t *wq = ext4_ioend_wq(inode);

	wait_event(*wq, (atomic_read(&EXT4_I(inode)->i_unwritten) == 0));
}

/*
 * This tests whether the IO in question is block-aligned or not.
 * Ext4 utilizes unwritten extents when hole-filling during direct IO, and they
 * are converted to written only after the IO is complete.  Until they are
 * mapped, these blocks appear as holes, so dio_zero_block() will assume that
 * it needs to zero out portions of the start and/or end block.  If 2 AIO
 * threads are at work on the same unwritten block, they must be synchronized
 * or one thread will zero the other's data, causing corruption.
 */
static int
ext4_unaligned_aio(struct inode *inode, const struct iovec *iov,
		   unsigned long nr_segs, loff_t pos)
{
	struct super_block *sb = inode->i_sb;
	int blockmask = sb->s_blocksize - 1;
	size_t count = iov_length(iov, nr_segs);
	loff_t final_size = pos + count;

	if (pos >= i_size_read(inode))
		return 0;

	if ((pos & blockmask) || (final_size & blockmask))
		return 1;

	return 0;
}

/* Is IO overwriting allocated and initialized blocks? */
static bool ext4_overwrite_io(struct inode *inode, loff_t pos, loff_t len)
{
	struct ext4_map_blocks map;
	unsigned int blkbits = inode->i_blkbits;
	int err, blklen;

	if (pos + len > i_size_read(inode))
		return false;

	map.m_lblk = pos >> blkbits;
	map.m_len = (EXT4_BLOCK_ALIGN(pos + len, blkbits) >> blkbits)
		- map.m_lblk;
	blklen = map.m_len;

	err = ext4_map_blocks(NULL, inode, &map, 0);
	/*
	 * 'err==len' means that all of the blocks have been preallocated,
	 * regardless of whether they have been initialized or not. To exclude
	 * unwritten extents, we need to check m_flags.
	 */
	return err == blklen && (map.m_flags & EXT4_MAP_MAPPED);
}

static ssize_t ext4_write_checks(struct kiocb *iocb, const struct iovec *iov,
				 unsigned long nr_segs, loff_t *pos)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(iocb->ki_filp);
	size_t length = iov_length(iov, nr_segs);
	ssize_t ret;

	ret = generic_write_checks(file, pos, &length, S_ISBLK(inode->i_mode));
	if (ret < 0)
		return ret;

	iocb->ki_pos = *pos;

	/*
	 * If we have encountered a bitmap-format file, the size limit
	 * is smaller than s_maxbytes, which is for extent-mapped files.
	 */
	if (!(ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))) {
		struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);

		if ((*pos > sbi->s_bitmap_maxbytes ||
		    (*pos == sbi->s_bitmap_maxbytes && length > 0)))
			return -EFBIG;

		if (*pos + length > sbi->s_bitmap_maxbytes) {
			nr_segs = iov_shorten((struct iovec *)iov, nr_segs,
					      sbi->s_bitmap_maxbytes - *pos);
		}
	}
	return iov_length(iov, nr_segs);
}

static ssize_t
ext4_file_dio_write(struct kiocb *iocb, const struct iovec *iov,
		    unsigned long nr_segs, loff_t pos)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;
	struct blk_plug plug;
	int unaligned_aio = 0;
	ssize_t ret;
	int overwrite = 0;

	if (ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS) &&
	    !is_sync_kiocb(iocb))
		unaligned_aio = ext4_unaligned_aio(inode, iov, nr_segs, pos);

	/* Unaligned direct AIO must be serialized; see comment above */
	if (unaligned_aio) {
		mutex_lock(ext4_aio_mutex(inode));
		ext4_unwritten_wait(inode);
	}

	BUG_ON(iocb->ki_pos != pos);

	mutex_lock(&inode->i_mutex);
	blk_start_plug(&plug);

	iocb->private = &overwrite;

	/* Check whether we do a DIO overwrite or not */
	if (ext4_should_dioread_nolock(inode) && !unaligned_aio &&
	    ext4_overwrite_io(inode, iocb->ki_pos, iov_length(iov, nr_segs)))
		overwrite = 1;

	ret = __generic_file_aio_write(iocb, iov, nr_segs, &iocb->ki_pos);
	mutex_unlock(&inode->i_mutex);

	if (ret > 0) {
		ssize_t err;

		err = generic_write_sync(file, pos, ret);
		if (err < 0 && ret > 0)
			ret = err;
	}
	blk_finish_plug(&plug);

	if (unaligned_aio)
		mutex_unlock(ext4_aio_mutex(inode));

	return ret;
}

#ifdef CONFIG_FS_DAX
static ssize_t
ext4_file_dax_write(
	struct kiocb		*iocb,
	const struct iovec	*iovp,
	unsigned long		nr_segs,
	loff_t			pos)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t			ret;
	size_t			size = 0;

	inode_lock(inode);
	ret = generic_segment_checks(iovp, &nr_segs, &size, VERIFY_WRITE);
	if (ret < 0)
		return ret;
	ret = ext4_write_checks(iocb, iovp, nr_segs, &pos);
	if (ret < 0)
		goto out;
	ret = file_remove_privs(iocb->ki_filp);
	if (ret)
		goto out;
	ret = file_update_time(iocb->ki_filp);
	if (ret)
		goto out;

	ret = dax_iomap_rw(WRITE, iocb, iovp, nr_segs, pos,
					size, &ext4_iomap_ops);
out:
	inode_unlock(inode);

	if (ret > 0) {
		int err;
		err = generic_write_sync(iocb->ki_filp, pos, ret);
		if (err < 0)
			ret = err;
	}
	return ret;
}
#endif

static ssize_t
ext4_file_write(struct kiocb *iocb, const struct iovec *iov,
		unsigned long nr_segs, loff_t pos)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;
	int overwrite = 0;

	ret = ext4_write_checks(iocb, iov, nr_segs, &pos);
	if (ret <= 0)
		return ret;

#ifdef CONFIG_FS_DAX
	if (IS_DAX(inode))
		return ext4_file_dax_write(iocb, iov, nr_segs, pos);
#endif

	iocb->private = &overwrite; /* RHEL7 only - prevent DIO race */
	if (unlikely(io_is_direct(iocb->ki_filp)))
		ret = ext4_file_dio_write(iocb, iov, nr_segs, pos);
	else
		ret = generic_file_aio_write(iocb, iov, nr_segs, pos);

	return ret;
}

#ifdef CONFIG_FS_DAX
static int ext4_dax_huge_fault(struct vm_fault *vmf,
		enum page_entry_size pe_size)
{
	int result;
	handle_t *handle = NULL;
	struct inode *inode = file_inode(vmf->vma->vm_file);
	struct super_block *sb = inode->i_sb;

	/*
	 * We have to distinguish real writes from writes which will result in a
	 * COW page; COW writes should *not* poke the journal (the file will not
	 * be changed). Doing so would cause unintended failures when mounted
	 * read-only.
	 *
	 * We check for VM_SHARED rather than vmf->cow_page since the latter is
	 * unset for pe_size != PE_SIZE_PTE (i.e. only in do_cow_fault); for
	 * other sizes, dax_iomap_fault will handle splitting / fallback so that
	 * we eventually come back with a COW page.
	 */
	bool write = (vmf->flags & FAULT_FLAG_WRITE) &&
		(vmf->vma->vm_flags & VM_SHARED);

	if (write) {
		sb_start_pagefault(sb);
		file_update_time(vmf->vma->vm_file);
		down_read(&EXT4_I(inode)->i_mmap_sem);
		handle = ext4_journal_start_sb(sb, EXT4_HT_WRITE_PAGE,
					       EXT4_DATA_TRANS_BLOCKS(sb));
	} else {
		down_read(&EXT4_I(inode)->i_mmap_sem);
	}
	if (!IS_ERR(handle))
		result = dax_iomap_fault(vmf, pe_size, &ext4_iomap_ops);
	else
		result = VM_FAULT_SIGBUS;
	if (write) {
		if (!IS_ERR(handle))
			ext4_journal_stop(handle);
		up_read(&EXT4_I(inode)->i_mmap_sem);
		sb_end_pagefault(sb);
	} else {
		up_read(&EXT4_I(inode)->i_mmap_sem);
	}

	return result;
}

static inline int ext4_dax_fault(struct vm_area_struct *vma,
		struct vm_fault *vmf)
{
	return ext4_dax_huge_fault(vmf, PE_SIZE_PTE);
}

/*
 * Handle write fault for VM_MIXEDMAP mappings. Similarly to ext4_dax_fault()
 * handler we check for races agaist truncate. Note that since we cycle through
 * i_mmap_sem, we are sure that also any hole punching that began before we
 * were called is finished by now and so if it included part of the file we
 * are working on, our pte will get unmapped and the check for pte_same() in
 * wp_pfn_shared() fails. Thus fault gets retried and things work out as
 * desired.
 */
static int ext4_dax_pfn_mkwrite(struct vm_area_struct *vma,
				struct vm_fault *vmf)
{
	struct inode *inode = file_inode(vma->vm_file);
	struct super_block *sb = inode->i_sb;
	loff_t size;
	int ret;

	sb_start_pagefault(sb);
	file_update_time(vma->vm_file);
	down_read(&EXT4_I(inode)->i_mmap_sem);
	size = (i_size_read(inode) + PAGE_SIZE - 1) >> PAGE_SHIFT;
	if (vmf->pgoff >= size)
		ret = VM_FAULT_SIGBUS;
	else
		ret = dax_pfn_mkwrite(vma, vmf);
	up_read(&EXT4_I(inode)->i_mmap_sem);
	sb_end_pagefault(sb);

	return ret;
}

static const struct vm_operations_struct ext4_dax_vm_ops = {
	.fault		= ext4_dax_fault,
	.huge_fault	= ext4_dax_huge_fault,
	.page_mkwrite	= ext4_dax_fault,
	.pfn_mkwrite	= ext4_dax_pfn_mkwrite,
	.remap_pages	= generic_file_remap_pages,
};
#else
#define ext4_dax_vm_ops	ext4_file_vm_ops
#endif

static const struct vm_operations_struct ext4_file_vm_ops = {
	.fault		= ext4_filemap_fault,
	.page_mkwrite   = ext4_page_mkwrite,
	.remap_pages	= generic_file_remap_pages,
};

static int ext4_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	file_accessed(file);
	if (IS_DAX(file_inode(file))) {
		vma->vm_ops = &ext4_dax_vm_ops;
		vma->vm_flags |= VM_MIXEDMAP | VM_HUGEPAGE;
		vma->vm_flags2 |= VM_PFN_MKWRITE | VM_HUGE_FAULT;
	} else {
		vma->vm_ops = &ext4_file_vm_ops;
	}
	return 0;
}

static int ext4_file_open(struct inode * inode, struct file * filp)
{
	struct super_block *sb = inode->i_sb;
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct vfsmount *mnt = filp->f_path.mnt;
	struct path path;
	char buf[64], *cp;

	if (unlikely(!(sbi->s_mount_flags & EXT4_MF_MNTDIR_SAMPLED) &&
		     !(sb->s_flags & MS_RDONLY))) {
		sbi->s_mount_flags |= EXT4_MF_MNTDIR_SAMPLED;
		/*
		 * Sample where the filesystem has been mounted and
		 * store it in the superblock for sysadmin convenience
		 * when trying to sort through large numbers of block
		 * devices or filesystem images.
		 */
		memset(buf, 0, sizeof(buf));
		path.mnt = mnt;
		path.dentry = mnt->mnt_root;
		cp = d_path(&path, buf, sizeof(buf));
		if (!IS_ERR(cp)) {
			handle_t *handle;
			int err;

			handle = ext4_journal_start_sb(sb, EXT4_HT_MISC, 1);
			if (IS_ERR(handle))
				return PTR_ERR(handle);
			BUFFER_TRACE(sbi->s_sbh, "get_write_access");
			err = ext4_journal_get_write_access(handle, sbi->s_sbh);
			if (err) {
				ext4_journal_stop(handle);
				return err;
			}
			strlcpy(sbi->s_es->s_last_mounted, cp,
				sizeof(sbi->s_es->s_last_mounted));
			ext4_handle_dirty_super(handle, sb);
			ext4_journal_stop(handle);
		}
	}
	/*
	 * Set up the jbd2_inode if we are opening the inode for
	 * writing and the journal is present
	 */
	if (filp->f_mode & FMODE_WRITE) {
		int ret = ext4_inode_attach_jinode(inode);
		if (ret < 0)
			return ret;
	}
	return dquot_file_open(inode, filp);
}

/*
 * ext4_llseek() handles both block-mapped and extent-mapped maxbytes values
 * by calling generic_file_llseek_size() with the appropriate maxbytes
 * value for each.
 */
loff_t ext4_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_mapping->host;
	loff_t maxbytes;

	if (!(ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS)))
		maxbytes = EXT4_SB(inode->i_sb)->s_bitmap_maxbytes;
	else
		maxbytes = inode->i_sb->s_maxbytes;

	switch (whence) {
	default:
		return generic_file_llseek_size(file, offset, whence,
						maxbytes, i_size_read(inode));
	case SEEK_HOLE:
		inode_lock(inode);
		offset = iomap_seek_hole(inode, offset, &ext4_iomap_ops);
		inode_unlock(inode);
		break;
	case SEEK_DATA:
		inode_lock(inode);
		offset = iomap_seek_data(inode, offset, &ext4_iomap_ops);
		inode_unlock(inode);
		break;
	}

	if (offset < 0)
		return offset;
	return vfs_setpos(file, offset, maxbytes);
}

#ifdef CONFIG_FS_DAX
static ssize_t
ext4_file_dax_read(
	struct kiocb		*iocb,
	const struct iovec	*iovp,
	unsigned long		nr_segs,
	loff_t			pos)
{
	size_t			size = 0;
	ssize_t			ret = 0;
	struct inode *inode = file_inode(iocb->ki_filp);

	ret = generic_segment_checks(iovp, &nr_segs, &size, VERIFY_WRITE);
	if (ret < 0)
		return ret;

	if (!size)
		return 0; /* skip atime */

	inode_lock(inode);
	/*
	 * Recheck under inode lock - at this point we are sure it cannot
	 * change anymore
	 */
	if (!IS_DAX(inode)) {
		inode_unlock(inode);
		/* Fallback to buffered IO in case we cannot support DAX */
		return generic_file_aio_read(iocb, iovp, nr_segs, pos);
	}
	ret = dax_iomap_rw(READ, iocb, iovp, nr_segs, pos,
					size, &ext4_iomap_ops);
	inode_unlock(inode);

	file_accessed(iocb->ki_filp);
	return ret;
}
#endif

static ssize_t
ext4_file_read(
	struct kiocb		*iocb,
	const struct iovec	*iovp,
	unsigned long		nr_segs,
	loff_t 			pos)
{
#ifdef CONFIG_FS_DAX
	if (IS_DAX(file_inode(iocb->ki_filp)))
		return ext4_file_dax_read(iocb, iovp, nr_segs, pos);
#endif
	return generic_file_aio_read(iocb, iovp, nr_segs, pos);
}

const struct file_operations ext4_file_operations = {
	.llseek		= ext4_llseek,
	.read		= do_sync_read,
	.write		= do_sync_write,
	.aio_read	= ext4_file_read,
	.aio_write	= ext4_file_write,
	.unlocked_ioctl = ext4_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= ext4_compat_ioctl,
#endif
	.mmap		= ext4_file_mmap,
	.open		= ext4_file_open,
	.release	= ext4_release_file,
	.fsync		= ext4_sync_file,
	.get_unmapped_area = thp_get_unmapped_area,
	.splice_read	= generic_file_splice_read,
	.splice_write	= generic_file_splice_write,
	.fallocate	= ext4_fallocate,
};

const struct inode_operations ext4_file_inode_operations = {
	.setattr	= ext4_setattr,
	.getattr	= ext4_getattr,
	.setxattr	= generic_setxattr,
	.getxattr	= generic_getxattr,
	.listxattr	= ext4_listxattr,
	.removexattr	= generic_removexattr,
	.get_acl	= ext4_get_acl,
	.fiemap		= ext4_fiemap,
};

