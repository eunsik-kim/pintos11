#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/disk.h"
#include "lib/kernel/list.h"
#include "threads/synch.h"

struct bitmap;

void inode_init (void);
bool inode_create (disk_sector_t, off_t);
struct inode *inode_open (disk_sector_t);
struct inode *inode_reopen (struct inode *);
disk_sector_t inode_get_inumber (const struct inode *);
void inode_close (struct inode *);
void inode_remove (struct inode *);
off_t inode_read_at (struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at (struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write (struct inode *);
void inode_allow_write (struct inode *);
off_t inode_length (const struct inode *);
/* for symlink */
bool symlink_change_file(struct inode* inode);
void file_change_symlink(struct inode* inode);

/* for file growth */
disk_sector_t file_growth(struct inode *inode, off_t size, off_t offset);

/* On-disk inode.
 * Must be exactly DISK_SECTOR_SIZE bytes long. */
struct inode_disk {
	disk_sector_t start;                /* First data sector. */
	disk_sector_t target_sector;        /* target file sector */
	off_t length;                       /* File size in bytes. */
	unsigned isdir;						/* dir checking */
	unsigned magic;                     /* Magic number. */
	uint32_t unused[123];               /* Not used. */
};

/* In-memory inode. */
struct inode {
	struct list_elem elem;              /* Element in inode list. */
	disk_sector_t sector;               /* Sector number of disk location. */
	int open_cnt;                       /* Number of openers. */
	bool removed;                       /* True if deleted, false otherwise. */
	uint32_t deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
	uint32_t cwd_cnt;						/* checking cwd */
	struct lock w_lock;					/* for synchronization */
	struct inode_disk data;             /* Inode content. */
};

#endif /* filesys/inode.h */
