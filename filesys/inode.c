#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/fat.h"
#include "userprog/syscall.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* List of open inodes, so that opening a single inode twice
 * returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) {
	list_init (&open_inodes);
}

/* Returns the number of sectors to allocate for an inode SIZE
 * bytes long. */
static inline size_t
bytes_to_sectors (off_t size) {
	return DIV_ROUND_UP (size, DISK_SECTOR_SIZE);
}

/* Reads an inode from SECTOR
 * and returns a `struct inode' that contains it.
 * Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (disk_sector_t sector) {
	struct list_elem *e;
	struct inode *inode;

	/* Check whether this inode is already open. */
	for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
			e = list_next (e)) {
		inode = list_entry (e, struct inode, elem);
		if (inode->sector == sector) {
			inode_reopen (inode);
			return inode; 
		}
	}

	/* Allocate memory. */
	inode = malloc (sizeof *inode);
	if (inode == NULL)
		return NULL;

	/* Initialize. */
	list_push_front (&open_inodes, &inode->elem);
	inode->sector = sector;
	inode->open_cnt = 1;
	inode->deny_write_cnt = 0;
	inode->cwd_cnt = 0;
	inode->removed = false;
	lock_init(&inode->w_lock);
	disk_read (filesys_disk, inode->sector, &inode->data);
	return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode) {
	if (inode != NULL)
		inode->open_cnt++;
	return inode;
}

/* Returns INODE's inode number. */
disk_sector_t
inode_get_inumber (const struct inode *inode) {
	return inode->sector;
}

/* Marks INODE to be deleted when it is closed by the last caller who
 * has it open. */
void
inode_remove (struct inode *inode) {
	ASSERT(inode->data.magic == INODE_MAGIC);
	ASSERT (inode != NULL);
	inode->removed = true;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
	void
inode_deny_write (struct inode *inode) 
{	
	inode->deny_write_cnt++;
	ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
 * Must be called once by each inode opener who has called
 * inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) {
	ASSERT(inode->data.magic == INODE_MAGIC);
	ASSERT (inode->deny_write_cnt > 0);
	ASSERT (inode->deny_write_cnt <= inode->open_cnt);
	inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode) {
	ASSERT(inode->data.magic == INODE_MAGIC);
	struct thread *cur = thread_current();
	bool flg = symlink_change_file(inode);
	off_t len = inode->data.length;
	return len;
}

#ifndef EFILESYS

/* Returns the disk sector that contains byte offset POS within
 * INODE.
 * Returns -1 if INODE does not contain data for a byte at offset
 * POS. */
static disk_sector_t
byte_to_sector (const struct inode *inode, off_t pos) {
	ASSERT (inode != NULL);
	if (pos < inode->data.length)
		return inode->data.start + pos / DISK_SECTOR_SIZE;
	else
		return -1;
}

/* Closes INODE and writes it to disk.
 * If this was the last reference to INODE, frees its memory.
 * If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) {
	/* Ignore null pointer. */
	if (inode == NULL)
		return;

	/* Release resources if this was the last opener. */
	if (--inode->open_cnt == 0) {
		/* Remove from inode list and release lock. */
		list_remove (&inode->elem);

		/* Deallocate blocks if removed. */
		if (inode->removed) {
			free_map_release (inode->sector, 1);
			free_map_release (inode->data.start,
					bytes_to_sectors (inode->data.length)); 
		}

		free (inode); 
	}
}

/* Initializes an inode with LENGTH bytes of data and
 * writes the new inode to sector SECTOR on the file system
 * disk.
 * Returns true if successful.
 * Returns false if memory or disk allocation fails. */
bool
inode_create (disk_sector_t sector, off_t length) {
	struct inode_disk *disk_inode = NULL;
	bool success = false;

	ASSERT (length >= 0);

	/* If this assertion fails, the inode structure is not exactly
	 * one sector in size, and you should fix that. */
	ASSERT (sizeof *disk_inode == DISK_SECTOR_SIZE);

	disk_inode = calloc (1, sizeof *disk_inode);
	if (disk_inode != NULL) {
		size_t sectors = bytes_to_sectors (length);
		disk_inode->length = length;
		disk_inode->magic = INODE_MAGIC;
		if (free_map_allocate (sectors, &disk_inode->start)) {
			disk_write (filesys_disk, sector, disk_inode);
			if (sectors > 0) {
				static char zeros[DISK_SECTOR_SIZE];
				size_t i;

				for (i = 0; i < sectors; i++) 
					disk_write (filesys_disk, disk_inode->start + i, zeros); 
			}
			success = true; 
		} 
		free (disk_inode);
	}
	return success;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
 * Returns the number of bytes actually read, which may be less
 * than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) {
	uint8_t *buffer = buffer_;
	off_t bytes_read = 0;
	uint8_t *bounce = NULL;

	while (size > 0) {
		/* Disk sector to read, starting byte offset within sector. */
		disk_sector_t sector_idx = byte_to_sector (inode, offset);
		int sector_ofs = offset % DISK_SECTOR_SIZE;

		/* Bytes left in inode, bytes left in sector, lesser of the two. */
		off_t inode_left = inode_length (inode) - offset;
		int sector_left = DISK_SECTOR_SIZE - sector_ofs;
		int min_left = inode_left < sector_left ? inode_left : sector_left;

		/* Number of bytes to actually copy out of this sector. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;

		if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) {
			/* Read full sector directly into caller's buffer. */
			disk_read (filesys_disk, sector_idx, buffer + bytes_read); 
		} else {
			/* Read sector into bounce buffer, then partially copy
			 * into caller's buffer. */
			if (bounce == NULL) {
				bounce = malloc (DISK_SECTOR_SIZE);
				if (bounce == NULL)
					break;
			}
			disk_read (filesys_disk, sector_idx, bounce);
			memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
		}

		/* Advance. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_read += chunk_size;
	}
	free (bounce);

	return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
 * Returns the number of bytes actually written, which may be
 * less than SIZE if end of file is reached or an error occurs.
 * (Normally a write at end of file would extend the inode, but
 * growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
		off_t offset) {
	const uint8_t *buffer = buffer_;
	off_t bytes_written = 0;
	uint8_t *bounce = NULL;

	if (inode->deny_write_cnt)
		return 0;

	while (size > 0) {
		/* Sector to write, starting byte offset within sector. */
		disk_sector_t sector_idx = byte_to_sector (inode, offset);
		int sector_ofs = offset % DISK_SECTOR_SIZE;

		/* Bytes left in inode, bytes left in sector, lesser of the two. */
		off_t inode_left = inode_length (inode) - offset;
		int sector_left = DISK_SECTOR_SIZE - sector_ofs;
		int min_left = inode_left < sector_left ? inode_left : sector_left;

		/* Number of bytes to actually write into this sector. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;

		if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) {
			/* Write full sector directly to disk. */
			disk_write (filesys_disk, sector_idx, buffer + bytes_written); 
		} else { 
			/* We need a bounce buffer. */
			if (bounce == NULL) {
				bounce = malloc (DISK_SECTOR_SIZE);
				if (bounce == NULL)
					break;
			}

			/* If the sector contains data before or after the chunk
			   we're writing, then we need to read in the sector
			   first.  Otherwise we start with a sector of all zeros. */
			if (sector_ofs > 0 || chunk_size < sector_left) 
				disk_read (filesys_disk, sector_idx, bounce);
			else
				memset (bounce, 0, DISK_SECTOR_SIZE);
			memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
			disk_write (filesys_disk, sector_idx, bounce); 
		}

		/* Advance. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_written += chunk_size;
	}
	free (bounce);

	return bytes_written;
}
#else

#include "include/filesys/fat.h"

/* inode의 offset위치에 해당하는 sector return 실패하면 -1 return. */
static disk_sector_t
byte_to_sector (const struct inode *inode, off_t pos) {
	ASSERT(inode->data.magic == INODE_MAGIC);
	ASSERT (inode != NULL);
	if (pos > inode->data.length)
		return -1;

	cluster_t clst;
	int sector_cnt = pos / DISK_SECTOR_SIZE;
	clst = sector_to_cluster(inode->data.start);
	while (sector_cnt-- > 0) 
		clst = fat_get(clst);

	if (sector_cnt > 0)
		return -1;

	return cluster_to_sector(clst);
}

/* inode의 offset위치에 해당하는 sector return 실패하면 -1 return. */
static disk_sector_t
byte_to_sector2 (const struct inode *inode, off_t pos) {
	ASSERT(inode->data.magic == INODE_MAGIC);
	ASSERT (inode != NULL);
	if (pos > inode->data.length)
		return -1;

	cluster_t clst;
	int sector_cnt = pos / DISK_SECTOR_SIZE - (pos % DISK_SECTOR_SIZE == 0);
	clst = sector_to_cluster(inode->data.start);
	while (sector_cnt-- > 0) 
		clst = fat_get(clst);

	if (sector_cnt > 0)
		return -1;

	return cluster_to_sector(clst);
}

/* Closes INODE and writes it to disk.
 * If this was the last reference to INODE, frees its memory.
 * If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) {
	/* Ignore null pointer. */
	if (inode == NULL)
		return;

	/* Release resources if this was the last opener. */
	if (--inode->open_cnt == 0) {
		/* Remove from inode list and release lock. */
		list_remove (&inode->elem);

		/* Deallocate blocks if removed. 
			only target file can delete disk */
		if (inode->removed && (inode->sector == inode->data.target_sector)) {
			disk_write(filesys_disk, inode->sector, &inode->data);
			fat_remove_chain(sector_to_cluster(inode->sector), 0);
		}
			
		free (inode); 
	}
}

/* Initializes an inode with LENGTH bytes of data and
 * writes the new inode to sector SECTOR on the file system
 * disk.
 * Returns true if successful.
 * Returns false if memory or disk allocation fails. */
bool
inode_create (disk_sector_t sector, off_t length) {
	struct inode_disk *disk_inode = NULL;
	bool success = false;

	ASSERT (length >= 0);

	/* If this assertion fails, the inode structure is not exactly
	 * one sector in size, and you should fix that. */
	ASSERT (sizeof *disk_inode == DISK_SECTOR_SIZE);

	disk_inode = calloc (1, sizeof *disk_inode);
	if (disk_inode != NULL) {
		// create fat_chain
		cluster_t clst, s_clst;
		static char zeros[DISK_SECTOR_SIZE];
		int create_cnt = bytes_to_sectors(length);
		s_clst = clst = sector_to_cluster(sector);
		
		while (clst && create_cnt--) 
			clst = fat_create_chain(clst);

		if (create_cnt > 0 || !clst) {	// if fail to create chain
			fat_remove_chain(s_clst, 0);
			return false;
		}

		// write inode struct on disk
		success = true; 
		if (fat_get(s_clst) != EOChain)
			disk_inode->start = cluster_to_sector(fat_get(s_clst));
		else	 // if length == 0
			disk_inode->start = sector;
		disk_inode->length = length;
		disk_inode->target_sector = sector;
		disk_inode->magic = INODE_MAGIC;
		disk_write (filesys_disk, sector, disk_inode);
		
		// memset 0 on disk
		while ((s_clst = fat_get(s_clst)) != EOChain) 
			disk_write (filesys_disk, disk_inode->start, zeros);
		
		free (disk_inode);
	}
	return success;
}

/* file read with fat */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) {
	ASSERT(inode->data.magic == INODE_MAGIC);
	symlink_change_file(inode);
	off_t origin_size = size, origin_offset = offset;
	cluster_t clst;
	off_t bytes_read = 0;
	uint8_t *bounce = NULL;
	uint8_t *buffer = buffer_;
	disk_sector_t sector_idx = byte_to_sector (inode, offset);
	lock_acquire(&inode->w_lock);	
	int len = inode_length (inode);
	lock_release(&inode->w_lock);	

	while (size > 0) {
		/* Disk sector to read, starting byte offset within sector. */
		int sector_ofs = offset % DISK_SECTOR_SIZE;

		/* Bytes left in inode, bytes left in sector, lesser of the two. */
		off_t inode_left = len - offset;
		int sector_left = DISK_SECTOR_SIZE - sector_ofs;
		int min_left = inode_left < sector_left ? inode_left : sector_left;

		/* Number of bytes to actually copy out of this sector. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;

		if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) {
			/* Read full sector directly into caller's buffer. */
			disk_read (filesys_disk, sector_idx, buffer + bytes_read); 
		} else {
			/* Read sector into bounce buffer, then partially copy
			 * into caller's buffer. */
			if (bounce == NULL) {
				bounce = malloc (DISK_SECTOR_SIZE);
				if (bounce == NULL)
					break;
			}
			disk_read (filesys_disk, sector_idx, bounce);
			memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
		}

		/* Advance. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_read += chunk_size;

		// find next sector
		if ((clst = fat_get(sector_to_cluster(sector_idx))) == EOChain)
			break;
		sector_idx = cluster_to_sector(clst);
	}
	free (bounce);
	
	return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
 * Returns the number of bytes actually written, which may be
 * less than SIZE if end of file is reached or an error occurs.
 * (Normally a write at end of file would extend the inode, but
 * growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size, off_t offset) {
	ASSERT(inode->data.magic == INODE_MAGIC);
	if (size + offset == 0)
		return 0;

	symlink_change_file(inode);
	if (inode->deny_write_cnt)
		return 0;

	cluster_t clst;
	uint8_t *bounce = NULL;
	const uint8_t *buffer = buffer_;
	disk_sector_t sector_idx = file_growth(inode, size, offset);
	off_t bytes_written = 0, len = inode_length(inode);
	
	while (size > 0) {
		/* Sector to write, starting byte offset within sector. */
		int sector_ofs = offset % DISK_SECTOR_SIZE;

		/* Bytes left in inode, bytes left in sector, lesser of the two. */
		off_t inode_left = len - offset;
		int sector_left = DISK_SECTOR_SIZE - sector_ofs;
		int min_left = inode_left < sector_left ? inode_left : sector_left;

		/* Number of bytes to actually write into this sector. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;

		if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) {
			/* Write full sector directly to disk. */
			disk_write (filesys_disk, sector_idx, buffer + bytes_written); 
		} else { 
			/* We need a bounce buffer. */
			if (bounce == NULL) {
				bounce = malloc (DISK_SECTOR_SIZE);
				if (bounce == NULL)
					break;
			}

			/* If the sector contains data before or after the chunk
			   we're writing, then we need to read in the sector
			   first.  Otherwise we start with a sector of all zeros. */
			if (sector_ofs > 0 || chunk_size < sector_left) 
				disk_read (filesys_disk, sector_idx, bounce);
			else
				memset (bounce, 0, DISK_SECTOR_SIZE);
			memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
			disk_write (filesys_disk, sector_idx, bounce); 
		}

		/* Advance. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_written += chunk_size;

		// find next sector
		clst = sector_to_cluster(sector_idx);
		if ((clst = fat_get(clst)) == EOChain)
			break;
		sector_idx = cluster_to_sector(clst);
	}
	free (bounce);
	return bytes_written;
}

disk_sector_t file_growth(struct inode *inode, off_t size, off_t offset) {
	disk_sector_t sector, sector_idx;
	cluster_t clst, last_clst, off_clst;
	off_t create_cnt, add_length = offset + size - inode->data.length;
	off_t res = inode->data.length % DISK_SECTOR_SIZE;
	off_t temp = ((res + add_length) / DISK_SECTOR_SIZE - ((res + add_length) % DISK_SECTOR_SIZE == 0));
	// file growth case 1: no sector to write, case 2: not enough place to write
	if ((inode->data.length == 0) || (!res && add_length > 0) || temp > 0) {
		lock_acquire(&inode->w_lock);	// file growth is atomic action
		if (inode->data.length == 0) {	// case 1
			clst = last_clst = sector_to_cluster(inode->data.start);	
			create_cnt = bytes_to_sectors(add_length);		
		} else {	// case 2
			if (!res && add_length > 0) {
				clst = last_clst = sector_to_cluster(byte_to_sector2(inode, inode->data.length));
				create_cnt = bytes_to_sectors(add_length);
			} else{
				clst = last_clst = sector_to_cluster(byte_to_sector(inode, inode->data.length));
				create_cnt = temp;		
			}
		}
		
		// append cluster chain 
		while (clst && create_cnt--) 
			clst = fat_create_chain(clst);
		
		if (create_cnt > 0 || !clst) {
			if (inode->data.length != 0)	// case 2
				fat_remove_chain(fat_get(last_clst), last_clst);
			else if (fat_get(last_clst) != EOChain)	// case 1, only created chain  
				fat_remove_chain(fat_get(last_clst), last_clst);
			lock_release(&inode->w_lock);
			return false;
		}

		// case 1, reset start sector 
		if (inode->data.length == 0)
			inode->data.start = cluster_to_sector(fat_get(last_clst));

		// update inode struct on disk
		inode->data.length += add_length;
		disk_write (filesys_disk, inode->data.target_sector, &inode->data);
		sector_idx = byte_to_sector(inode, offset);	

		// memset 0 until offset_sector from last_clst behind on disk
		static char zeros[DISK_SECTOR_SIZE];
		off_clst = sector_to_cluster(sector_idx);
		clst = last_clst;
		while (clst != off_clst) {
			clst = fat_get(clst);
			disk_write (filesys_disk, cluster_to_sector(clst), zeros);	
		}
		if (last_clst != off_clst)	// careful not to overlap last sector
			disk_write (filesys_disk, cluster_to_sector(off_clst), zeros);	
		lock_release(&inode->w_lock);	
		
	} else {
		lock_acquire(&inode->w_lock);	// file growth is atomic action
		if (add_length > 0) {
			// update inode struct on disk (not append sector)
			inode->data.length += add_length;
			disk_write (filesys_disk, inode->data.target_sector, &inode->data);
		}
		sector_idx = byte_to_sector(inode, offset);
		lock_release(&inode->w_lock);	
	}
	
	return sector_idx;
}

/* read나 write하기전 target file로 변--신 */
bool symlink_change_file(struct inode* inode) {
	if (!checklink(inode->data.isdir))
		return false;
	
	char target[512];
	disk_sector_t sector = inode->data.start;
	
	/* chaining until finding target file */
	while (sector) {
		disk_read(filesys_disk, sector, target);
		void *file_entity = filesys_open(target);
		if (!file_entity)
			PANIC("No such file or directory");
		struct file *file = getptr(file_entity);

		if (checklink(file->inode->data.isdir))
			sector = file->inode->data.start;
		else {
			sector = 0;
			memcpy(&inode->data, &file->inode->data, sizeof(struct inode_disk)); 
		}	

		if (checkdir(file_entity)) 
			dir_close(getptr(file_entity));
		else
			file_close(file);
	}
	return true;
}

/* read나 write한뒤 원래 link file로 변--신 */
void file_change_symlink(struct inode* inode) {
	if (inode->sector == inode->data.target_sector)
		return;
	
	disk_read (filesys_disk, inode->sector, &inode->data);
}
#endif