#ifndef FILESYS_DIRECTORY_H
#define FILESYS_DIRECTORY_H

#include <stdbool.h>
#include <stddef.h>
#include "devices/disk.h"

/* Maximum length of a file name component.
 * This is the traditional UNIX maximum length.
 * After directories are implemented, this maximum length may be
 * retained, but much longer full path names must be allowed. */
#define NAME_MAX 14
#define MAX_FILE_PATH 100

struct inode;

/* Subdirectories and Soft Links */
struct dir *find_dir(char *paths, char *file_name);
void cwd_cnt_up(struct dir *dir);
void cwd_cnt_down(struct dir *dir);
bool symlink_change_dir(struct inode* inode);

/* Opening and closing directories. */
bool dir_create (disk_sector_t sector, size_t entry_cnt, disk_sector_t parent_sector);
struct dir *dir_open (struct inode *);
struct dir *dir_open_root (void);
struct dir *dir_reopen (struct dir *);
void dir_close (struct dir *);
struct inode *dir_get_inode (struct dir *);

/* Reading and writing. */
bool dir_lookup (const struct dir *, const char *name, struct inode **);
bool dir_add (struct dir *, const char *name, disk_sector_t);
bool dir_remove (struct dir *, const char *name);
bool dir_readdir (struct dir *, char name[NAME_MAX + 1]);

#endif /* filesys/directory.h */
