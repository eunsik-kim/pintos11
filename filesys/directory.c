#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "filesys/fat.h"
#include "threads/thread.h"
#include <string.h>

/* A directory. */
struct dir {
	struct inode *inode;                /* Backing store. */
	off_t pos;                          /* Current position. */
	struct lock d_lock;
};

/* A single directory entry. */
struct dir_entry { 	
	disk_sector_t inode_sector;         /* Sector number of header. */
	char name[NAME_MAX + 1];            /* Null terminated file name. */
	bool in_use;                        /* In use or free? */
};

/* Creates a directory with space for ENTRY_CNT entries in the
 * given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (disk_sector_t sector, size_t entry_cnt, disk_sector_t parent_sector) {
	bool flg = inode_create (sector, entry_cnt * sizeof (struct dir_entry));
	if (!flg)
		return false;
	struct inode *inode = inode_open(sector);
	// checking dir
	inode->data.isdir = 1;
	disk_write (filesys_disk, sector, &inode->data);
	struct dir_entry e;
	off_t ofs;

	for (int i=0; i<2; i++) {
		/* Write slot. */
		e.in_use = true;
		if (i == 0) {
			strlcpy (e.name, ".", 2);
			e.inode_sector = sector;
		} else {
			strlcpy (e.name, "..", 3);
			e.inode_sector = parent_sector;
		}	
		flg = inode_write_at (inode, &e, sizeof e,  i * 20) == sizeof e;
	}
	return flg;
}

/* Opens and returns the directory for the given INODE, of which
 * it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) {
	struct dir *dir = calloc (1, sizeof *dir);
	if (inode != NULL && dir != NULL) {
		dir->inode = inode;
		dir->pos = 0;
		lock_init(&dir->d_lock);
		return dir;
	} else {
		inode_close (inode);
		free (dir);
		return NULL;
	}
}

/* Opens the root directory and returns a directory for it.
 * Return true if successful, false on failure. */
struct dir *
dir_open_root (void) {
#ifndef EFILESYS
	return dir_open (inode_open (ROOT_DIR_SECTOR));
#else
	struct inode *root = inode_open (cluster_to_sector(ROOT_DIR_CLUSTER));
	root->cwd_cnt++;
	return dir_open (root);
#endif
}

/* Opens and returns a new directory for the same inode as DIR.
 * Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) {
	return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) {
	if (dir != NULL) {
		inode_close (dir->inode);
		free (dir);
	}
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) {
	return dir->inode;
}

/* 절대경로 or 상대경로를 받아와서 경로중 마지막 dir과 file_name을 반환하는 함수 */
struct dir *find_dir(char *origin_paths, char *file_name) {
	ASSERT (origin_paths != NULL);

	size_t ofs;
	struct dir_entry e;
	struct inode *inode = NULL;
	char *paths, *rest_path, *cur_path, *next_path;
	struct thread *cur = thread_current();
	struct dir *next_dir, *cwd = thread_current()->cwd;

	// save due to strtok_r
	paths = palloc_get_page(0);
	strlcpy(paths, origin_paths, strlen(origin_paths) + 1);

	if (cwd && paths[0] != '/')    // 상대경로
		next_dir = dir_reopen(cwd);
	else
		next_dir = dir_open_root();		

	// parse paths
	cur_path = strtok_r(paths, "/", &rest_path);
	if ((cwd && paths[0] != '/') && !cur_path) { // if relative path is null_path
		dir_close(next_dir);
		palloc_free_page(paths);
		return NULL;
	}

	if (!cur_path)	// null indicates current directory
		cur_path = ".";

	if (strlen(cur_path) > NAME_MAX)	// validate file name
		return NULL;

	next_path = strtok_r(NULL, "/", &rest_path);

	while (next_path) {
		if (strlen(next_path) > NAME_MAX)	// validate file name
			return NULL;

		if (!dir_lookup(next_dir, cur_path, &inode)) {	// find sub dir
			dir_close(next_dir);
			palloc_free_page(paths);
			return NULL;
		}
		dir_close(next_dir);
		next_dir = dir_open(inode);
		if (!next_dir->inode->data.isdir) { 	// wrong path
			dir_close(next_dir);
			palloc_free_page(paths);
			return NULL;
		}		

		cur_path = next_path;
		if (strstr(rest_path, "/")) {
			next_path = strtok_r(NULL, "/", &rest_path);
			if (!next_path)		// null indicates current directory
				cur_path =".";
		} else
			next_path = strtok_r(NULL, "/", &rest_path);
		
		
	}
	strlcpy(file_name, cur_path, strnlen(cur_path, NAME_MAX) + 1);
	palloc_free_page(paths);
	return next_dir;
}

/* Searches DIR for a file with the given NAME.
 * If successful, returns true, sets *EP to the directory entry
 * if EP is non-null, and sets *OFSP to the byte offset of the
 * directory entry if OFSP is non-null.
 * otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
		struct dir_entry *ep, off_t *ofsp) {
	struct dir_entry e;
	size_t ofs;

	ASSERT (dir != NULL);
	ASSERT (name != NULL);

	for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
			ofs += sizeof e)
		if (e.in_use && !strcmp (name, e.name)) {
			if (ep != NULL)
				*ep = e;
			if (ofsp != NULL)
				*ofsp = ofs;
			return true;
		}
	return false;
}

/* Searches DIR for a file with the given NAME
 * and returns true if one exists, false otherwise.
 * On success, sets *INODE to an inode for the file, otherwise to
 * a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
		struct inode **inode) {
	struct dir_entry e;

	ASSERT (dir != NULL);
	ASSERT (name != NULL);

	if (lookup (dir, name, &e, NULL))
		*inode = inode_open (e.inode_sector);
	else
		*inode = NULL;

	return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
 * file by that name.  The file's inode is in sector
 * INODE_SECTOR.
 * Returns true if successful, false on failure.
 * Fails if NAME is invalid (i.e. too long) or a disk or memory
 * error occurs. */
bool
dir_add (struct dir *dir, const char *name, disk_sector_t inode_sector) {
	lock_acquire(&dir->d_lock);
	struct dir_entry e;
	off_t ofs;
	bool success = false;

	ASSERT (dir != NULL);
	ASSERT (name != NULL);

	/* Check NAME for validity. */
	if (*name == '\0' || strlen (name) > NAME_MAX)
		return false;

	/* Check that NAME is not in use. */
	if (lookup (dir, name, NULL, NULL))
		goto done;

	/* Set OFS to offset of free slot.
	 * If there are no free slots, then it will be set to the
	 * current end-of-file.

	 * inode_read_at() will only return a short read at end of file.
	 * Otherwise, we'd need to verify that we didn't get a short
	 * read due to something intermittent such as low memory. */
	for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
			ofs += sizeof e)
		if (!e.in_use)
			break;

	/* Write slot. include folder growth */
	e.in_use = true;
	strlcpy (e.name, name, sizeof e.name);
	e.inode_sector = inode_sector;
	success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

done:
	lock_release(&dir->d_lock);
	return success;
}

/* Removes any entry for NAME in DIR.
 * Returns true if successful, false on failure,
 * which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) {
	lock_acquire(&dir->d_lock);
	struct dir_entry e, temp_e;
	struct inode *inode = NULL;
	bool success = false;
	off_t temp_ofs, ofs;

	ASSERT (dir != NULL);
	ASSERT (name != NULL);

	/* Find directory entry. */
	if (!lookup (dir, name, &e, &ofs))
		goto done;

	/* Open inode. */
	inode = inode_open (e.inode_sector);
	if (inode == NULL)
		goto done;

	/* directory라면 cwd_cnt가 1이상이거나 비어있지 않으면 삭제 금지 */
	if (inode->data.isdir & 1) {
		if ((inode->cwd_cnt > 0))
			goto done;

		for (temp_ofs = 40; inode_read_at (inode, &temp_e, sizeof temp_e, temp_ofs) == sizeof temp_e;		
				temp_ofs += sizeof temp_e)	// . and .. 제외
			if (temp_e.in_use)
				goto done;
	}
		
	/* Erase directory entry. */
	e.in_use = false;
	if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e)
		goto done;

	/* Remove inode. */
	inode_remove (inode);
	success = true;

done:
	inode_close (inode);
	lock_release(&dir->d_lock);
	return success;
}

/* Reads the next directory entry in DIR and stores the name in
 * NAME.  Returns true if successful, false if the directory
 * contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1]) {
	struct dir_entry e;
	lock_acquire(&dir->d_lock);
	while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) {
		dir->pos += sizeof e;
		if (!strcmp(".", e.name) || !strcmp("..", e.name))
			continue;

		if (e.in_use) {
			strlcpy (name, e.name, NAME_MAX + 1);
			lock_release(&dir->d_lock);
			return true;
		}
	}
	lock_release(&dir->d_lock);
	return false;
}

void cwd_cnt_up(struct dir *dir) {
	dir->inode->cwd_cnt++;
}

void cwd_cnt_down(struct dir *dir) {
	dir->inode->cwd_cnt--;
}