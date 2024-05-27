#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "devices/disk.h"

/* The disk that contains the file system. */
struct disk *filesys_disk;

static void do_format (void);

/* Initializes the file system module.
 * If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) {
	filesys_disk = disk_get (0, 1);
	if (filesys_disk == NULL)
		PANIC ("hd0:1 (hdb) not present, file system initialization failed");

	inode_init ();

#ifdef EFILESYS
	fat_init ();

	if (format)
		do_format ();

	fat_open ();
#else
	/* Original FS */
	free_map_init ();

	if (format)
		do_format ();

	free_map_open ();
#endif
}

/* Shuts down the file system module, writing any unwritten data
 * to disk. */
void
filesys_done (void) {
	/* Original FS */
#ifdef EFILESYS
	fat_close ();
#else
	free_map_close ();
#endif
}

#ifdef EFILESYS
#include "filesys/fat.h"

/* Creates a file named NAME with the given INITIAL_SIZE.
 * Returns true if successful, false otherwise.
 * Fails if a file named NAME already exists,
 * or if internal memory allocation fails. */
bool
filesys_create (const char *path, off_t initial_size) {
	char *file_name = malloc(NAME_MAX + 1);
	if (!file_name) 
		return false;

	struct dir *dir = find_dir(path, file_name);
	// make space to place new directory
	cluster_t clst;
	if (!(clst = fat_create_chain(0))) {
		free(file_name);
		return false;
	}
	disk_sector_t inode_sector = cluster_to_sector(clst);

	bool success = (dir != NULL
			&& inode_create (inode_sector, initial_size)
			&& dir_add (dir, file_name, inode_sector));
	dir_close (dir);
	free(file_name);
	return success;
}
#else
/* Creates a file named NAME with the given INITIAL_SIZE.
 * Returns true if successful, false otherwise.
 * Fails if a file named NAME already exists,
 * or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) {
	disk_sector_t inode_sector = 0;
	struct dir *dir = dir_open_root ();
	bool success = (dir != NULL
			&& free_map_allocate (1, &inode_sector)
			&& inode_create (inode_sector, initial_size)
			&& dir_add (dir, name, inode_sector));
	if (!success && inode_sector != 0)
		free_map_release (inode_sector, 1);
	dir_close (dir);
	free(file_name);

	return success;
}
#endif
/* Opens the file with the given NAME.
 * Returns the new file if successful or a null pointer
 * otherwise.
 * Fails if no file named NAME exists,
 * or if an internal memory allocation fails. 
 * if bit paking in file ptr, then it is directory */
struct file *
filesys_open (const char *path) {
	char *file_name = malloc(NAME_MAX + 1);
	if (!file_name)
		return NULL;

	struct dir *dir = find_dir(path, file_name);
	struct inode *inode = NULL;

	if (dir != NULL)
		dir_lookup (dir, file_name, &inode);
	dir_close (dir);
	free(file_name);

	if (inode && (inode->data.isdir & 1)) {
		dir = dir_open (inode);
		return ((uint64_t)dir | 1);
	} else
		return file_open (inode);
}

/* Deletes the file named NAME.
 * Returns true if successful, false on failure.
 * Fails if no file named NAME exists,
 * or if an internal memory allocation fails. */
bool
filesys_remove (const char *path) {
	char *file_name = malloc(NAME_MAX + 1);
	if (!file_name)
		return NULL;

	struct dir *dir = find_dir(path, file_name);
	bool success = dir != NULL && dir_remove (dir, file_name);
	dir_close (dir);
	free(file_name);

	return success;
}

/* Formats the file system. */
static void
do_format (void) {
	printf ("Formatting file system...");

#ifdef EFILESYS
	/* Create FAT and save it to the disk. */
	fat_create ();
	if (!fat_get(ROOT_DIR_CLUSTER))
		dir_create (cluster_to_sector (ROOT_DIR_CLUSTER), 16, cluster_to_sector (ROOT_DIR_CLUSTER));
	fat_close ();
#else
	free_map_create ();
	if (!dir_create (ROOT_DIR_SECTOR, 16, ROOT_DIR_SECTOR))
		PANIC ("root directory creation failed");
	free_map_close ();
#endif

	printf ("done.\n");
}
