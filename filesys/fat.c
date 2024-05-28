#include "filesys/fat.h"
#include "devices/disk.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include <stdio.h>
#include "lib/round.h"
#include "filesys/directory.h"

/* Should be less than DISK_SECTOR_SIZE */
struct fat_boot {
	unsigned int magic;
	unsigned int sectors_per_cluster; /* Fixed to 1 */
	unsigned int total_sectors;
	unsigned int fat_start;
	unsigned int fat_sectors; /* Size of FAT in sectors. */
	unsigned int root_dir_cluster;
};

/* FAT FS */
struct fat_fs {
	struct fat_boot bs;
	unsigned int *fat;
	unsigned int fat_length;	// counts of clusters
	disk_sector_t data_start;	// sector where to start
	cluster_t last_clst;
	struct lock write_lock;
};

static struct fat_fs *fat_fs;

void fat_boot_create (void);
void fat_fs_init (void);

void
fat_init (void) {
	fat_fs = calloc (1, sizeof (struct fat_fs));
	if (fat_fs == NULL)
		PANIC ("FAT init failed");

	// Read boot sector from the disk
	unsigned int *bounce = malloc (DISK_SECTOR_SIZE);
	if (bounce == NULL)
		PANIC ("FAT init failed");
	disk_read (filesys_disk, FAT_BOOT_SECTOR, bounce);
	memcpy (&fat_fs->bs, bounce, sizeof (fat_fs->bs));
	free (bounce);

	// Extract FAT info
	if (fat_fs->bs.magic != FAT_MAGIC)
		fat_boot_create ();
	fat_fs_init ();
}

void
fat_open (void) {
	fat_fs->fat = calloc (fat_fs->fat_length, sizeof (cluster_t));
	if (fat_fs->fat == NULL)
		PANIC ("FAT load failed");

	// Load FAT directly from the disk
	uint8_t *buffer = (uint8_t *) fat_fs->fat;
	off_t bytes_read = 0;
	off_t bytes_left = sizeof (fat_fs->fat);
	const off_t fat_size_in_bytes = fat_fs->fat_length * sizeof (cluster_t);
	for (unsigned i = 0; i < fat_fs->bs.fat_sectors; i++) {
		bytes_left = fat_size_in_bytes - bytes_read;
		if (bytes_left >= DISK_SECTOR_SIZE) {
			disk_read (filesys_disk, fat_fs->bs.fat_start + i,
			           buffer + bytes_read);
			bytes_read += DISK_SECTOR_SIZE;
		} else {
			uint8_t *bounce = malloc (DISK_SECTOR_SIZE);
			if (bounce == NULL)
				PANIC ("FAT load failed");
			disk_read (filesys_disk, fat_fs->bs.fat_start + i, bounce);
			memcpy (buffer + bytes_read, bounce, bytes_left);
			bytes_read += bytes_left;
			free (bounce);
		}
	}
}

void
fat_close (void) {
	// Write FAT boot sector
	uint8_t *bounce = calloc (1, DISK_SECTOR_SIZE);
	if (bounce == NULL)
		PANIC ("FAT close failed");
	memcpy (bounce, &fat_fs->bs, sizeof (fat_fs->bs));
	disk_write (filesys_disk, FAT_BOOT_SECTOR, bounce);
	free (bounce);

	// Write FAT directly to the disk
	uint8_t *buffer = (uint8_t *) fat_fs->fat;
	off_t bytes_wrote = 0;
	off_t bytes_left = sizeof (fat_fs->fat);
	const off_t fat_size_in_bytes = fat_fs->fat_length * sizeof (cluster_t);
	for (unsigned i = 0; i < fat_fs->bs.fat_sectors; i++) {
		bytes_left = fat_size_in_bytes - bytes_wrote;
		if (bytes_left >= DISK_SECTOR_SIZE) {
			disk_write (filesys_disk, fat_fs->bs.fat_start + i,
			            buffer + bytes_wrote);
			bytes_wrote += DISK_SECTOR_SIZE;
		} else {
			bounce = calloc (1, DISK_SECTOR_SIZE);
			if (bounce == NULL)
				PANIC ("FAT close failed");
			memcpy (bounce, buffer + bytes_wrote, bytes_left);
			disk_write (filesys_disk, fat_fs->bs.fat_start + i, bounce);
			bytes_wrote += bytes_left;
			free (bounce);
		}
	}
}

void
fat_create (void) {
	// Create FAT boot
	fat_boot_create ();
	fat_fs_init ();

	// Create FAT table
	fat_fs->fat = calloc (fat_fs->fat_length, sizeof (cluster_t));
	if (fat_fs->fat == NULL)
		PANIC ("FAT creation failed");

	// Set up ROOT_DIR_CLST
	// fat_put (ROOT_DIR_CLUSTER, EOChain);
	// Fill up ROOT_DIR_CLUSTER region with 0
	uint8_t *buf = calloc (1, DISK_SECTOR_SIZE);
	if (buf == NULL)
		PANIC ("FAT create failed due to OOM");
	disk_write (filesys_disk, cluster_to_sector (ROOT_DIR_CLUSTER), buf);
	free (buf);
}

void
fat_boot_create (void) {
	unsigned int fat_sectors =
	    (disk_size (filesys_disk) - 1)
	    / (DISK_SECTOR_SIZE / sizeof (cluster_t) * SECTORS_PER_CLUSTER + 1) + 1;
	fat_fs->bs = (struct fat_boot){
	    .magic = FAT_MAGIC,
	    .sectors_per_cluster = SECTORS_PER_CLUSTER,
	    .total_sectors = disk_size (filesys_disk),
	    .fat_start = 1,
	    .fat_sectors = fat_sectors,
	    .root_dir_cluster = ROOT_DIR_CLUSTER,
	};
}

/* fat_length : 몇개의 cluster가 존재하는지, data_start: 어떤 sector에서 파일을 저장할 수 있는지 
 각각 초기화 fat_fs->bs의 정보를 활용*/
void
fat_fs_init (void) {
	/* TODO: Your code goes here. */
	lock_init(&fat_fs->write_lock);	
	// round up clusters 
	fat_fs->fat_length = (fat_fs->bs.total_sectors - 1) *  DISK_SECTOR_SIZE 
							/ (DISK_SECTOR_SIZE + sizeof(cluster_t) * SECTORS_PER_CLUSTER) - 2;
	fat_fs->data_start = fat_fs->bs.fat_start + (fat_fs->fat_length * sizeof(cluster_t)) / DISK_SECTOR_SIZE + 1;
	fat_fs->last_clst = ROOT_DIR_CLUSTER + 1;
}

/*----------------------------------------------------------------------------*/
/* FAT handling                                                               */
/*----------------------------------------------------------------------------*/

/* Add a cluster to the chain.
 * If CLST is 0, start a new chain.
 * Returns 0 if fails to allocate a new cluster. */
cluster_t
fat_create_chain (cluster_t clst) {
	/* TODO: Your code goes here. */
	ASSERT(clst != EOChain);
	lock_acquire(&fat_fs->write_lock);
	// search next-fit area
	cluster_t s_clst = fat_fs->last_clst;
	while (fat_fs->fat[s_clst-1]) {
		if (++s_clst > fat_fs->fat_length)
			s_clst = ROOT_DIR_CLUSTER + 0;

		// fail to alloc
		if (s_clst == fat_fs->last_clst - 1) {
			lock_release(&fat_fs->write_lock);
			return 0;
		}			
	}

	// append chain
	fat_fs->last_clst = s_clst;
	if (clst)
		fat_fs->fat[clst-1] = s_clst;
	fat_fs->fat[s_clst-1] = EOChain;
	lock_release(&fat_fs->write_lock);
	return s_clst;
}

/* Remove the chain of clusters starting from CLST.
 * If PCLST is 0, assume CLST as the start of the chain. */
void
fat_remove_chain (cluster_t clst, cluster_t pclst) {
	/* TODO: Your code goes here. */
	ASSERT(clst != EOChain && clst);
	lock_acquire(&fat_fs->write_lock);
	fat_fs->last_clst = clst;
	if (pclst) 
		fat_fs->fat[pclst-1] = EOChain;
	else {
		fat_fs->fat[clst-1] = 0;
		return lock_release(&fat_fs->write_lock);
	}
		

	// clean up chain
	cluster_t temp = clst;
	do {
		temp = fat_fs->fat[clst-1];
		fat_fs->fat[clst-1] = 0;
	} while ((clst = temp) != EOChain);
	lock_release(&fat_fs->write_lock);
}

/* Update a value in the FAT table. */
void
fat_put (cluster_t clst, cluster_t val) {
	/* TODO: Your code goes here. */
	ASSERT(clst != EOChain && clst);
	lock_acquire(&fat_fs->write_lock);
	fat_fs->fat[clst-1] = val;
	lock_release(&fat_fs->write_lock);
}

/* Fetch a value in the FAT table. */
cluster_t
fat_get (cluster_t clst) {
	/* TODO: Your code goes here. */
	ASSERT(clst != EOChain && clst);
	lock_acquire(&fat_fs->write_lock);
	cluster_t out = fat_fs->fat[clst-1];
	lock_release(&fat_fs->write_lock);
	return out;
}

/* Covert a cluster # to a sector number. */
disk_sector_t
cluster_to_sector (cluster_t clst) {
	/* TODO: Your code goes here. */
	lock_acquire(&fat_fs->write_lock);
	ASSERT(clst != EOChain && clst);
	disk_sector_t out = fat_fs->data_start + (clst - 1) * SECTORS_PER_CLUSTER;
	lock_release(&fat_fs->write_lock);
	return out;
}

/* Covert a sector number # a sector number. */
cluster_t
sector_to_cluster (disk_sector_t sector) {
	if (sector - fat_fs->data_start < 0)
		return 0;

	lock_acquire(&fat_fs->write_lock);
	cluster_t out = (sector - fat_fs->data_start) / SECTORS_PER_CLUSTER + 1;
	lock_release(&fat_fs->write_lock);
	return out;
}
