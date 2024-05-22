/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/vaddr.h"
#include "lib/string.h"
#include "include/devices/disk.h"
#include "lib/kernel/bitmap.h"
#include "lib/round.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

struct swap_table {
	struct lock s_lock;               
	struct bitmap *used_map
};

/* An ATA device. */
struct disk {
	char name[8];               /* Name, e.g. "hd0:1". */
	struct channel *channel;    /* Channel disk is on. */
	int dev_no;                 /* Device 0 or 1 for master or slave. */

	bool is_ata;                /* 1=This device is an ATA disk. */
	disk_sector_t capacity;     /* Capacity in sectors (if is_ata). */

	long long read_cnt;         /* Number of sectors read. */
	long long write_cnt;        /* Number of sectors written. */
};

static void delete_swap_anon_page(struct page *page);

// similar to palloc pool which is differ from using next-fit
static struct swap_table stb;
static size_t next_fit_idx;
struct lock anon_cp_lock;


/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* TODO: Set up the swap_disk. */
	swap_disk = (struct disk *)malloc(sizeof swap_disk);
	swap_disk = disk_get(1, 1);
	lock_init(&stb.s_lock);

	// cal pages cnts
	size_t dsk_pages = DIV_ROUND_UP(bitmap_buf_size(swap_disk->capacity/8), PGSIZE);
	// allocate page to used_map 
	stb.used_map = palloc_get_multiple(0, dsk_pages);
	// set all used_map to be 0
	stb.used_map = bitmap_create_in_buf(swap_disk->capacity/8, stb.used_map, dsk_pages * PGSIZE);	 
	next_fit_idx = 0;	// next fit idx
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->type = type;
	page->operations = &anon_ops;
	if (!(page->type & VM_BSS))
		memset(page->frame->kva, 0, PGSIZE);
	
	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	// if (page->type & VM_NOSWAP) {
	// 	page->type &= ~VM_NOSWAP;
	// 	memset(page->frame->kva, 0, PGSIZE);
	// 	return true;
	// }

	size_t readb = 0;
	size_t size = PGSIZE;
	size_t disk_sector = page->anon.disk_sector;
	while (size) {	// size_left must be divided 512
		int size_left = (size > DISK_SECTOR_SIZE) ? DISK_SECTOR_SIZE : size;
		disk_read(swap_disk, disk_sector++, page->frame->kva + readb);
		size -= DISK_SECTOR_SIZE;
		readb += size_left;
	}
	// mark bitmap false
	ASSERT(bitmap_all(stb.used_map, page->anon.disk_sector/8, 1));
	bitmap_set_multiple(stb.used_map, page->anon.disk_sector/8, 1, false);

	// enable pml4 for pages which is sharing redundant frames
	struct list_elem *next_e = &page->cp_elem; 
	while((next_e = list_next(next_e)) != &page->cp_elem) {
		struct page *f_page = list_entry(next_e, struct page, cp_elem);
		enable_redundant_frame(f_page, page->frame);	// enable pml4
	}

	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	ASSERT(page->frame && (page->type & VM_FRAME));
	// not bss and not dirty (no need since malloc isn't impl)
	// if (!(page->type & VM_BSS) && !((page->type & VM_DIRTY) || pml4_is_dirty(page->pml4, page->va))) {
	// 	page->type |= VM_NOSWAP;
	//	add disable_redundant_frame
	// 	return true;	
	// }

	// similar to palloc_get_multiple but find policy is next-fit
	size_t start = next_fit_idx;
	lock_acquire(&stb.s_lock);
	size_t bit_idx = bitmap_scan_and_flip(stb.used_map, next_fit_idx, 1, false);
	if (bit_idx == BITMAP_ERROR) {
		bit_idx = bitmap_scan_and_flip(stb.used_map, 0, 1, false);
		if (bit_idx == BITMAP_ERROR)
			PANIC("no place in swap disk");
	}
	lock_release(&stb.s_lock);
	next_fit_idx = (bitmap_size(stb.used_map) < bit_idx + 1) ? 0 : bit_idx + 1;
	

	// similar to inode_write_at except for fixed size(PGSIZE)
	size_t writeb = 0;
	size_t size = PGSIZE;
	size_t disk_sector = bit_idx * 8;
	while (size) {	// size_left must be divided 512
		size_t size_left = (size > DISK_SECTOR_SIZE) ? DISK_SECTOR_SIZE : size;
		disk_write(swap_disk, disk_sector++, page->frame->kva + writeb);
		size -= DISK_SECTOR_SIZE;
		writeb += size_left;
	}

	// cp disksector pos for pages which is sharing redundant frames
	struct list_elem *next_e = &page->cp_elem; 
	do {
		struct page *f_page = list_entry(next_e, struct page, cp_elem);
		f_page->anon.disk_sector = bit_idx * 8;
		disable_redundant_frame(f_page);	// disable pml4
	} while((next_e = list_next(next_e)) != &page->cp_elem);

	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	// marking stb bitmap sector usable 
	delete_swap_anon_page(page);
	ftb_delete_frame(page);
	return;
}

/* cpwrite에 의해 page을 중복 참조하지 않는 경우 경우만 삭제 */
static void delete_swap_anon_page(struct page *page)
{	
	if (!(page->type & VM_FRAME) && is_alone(&page->cp_elem)) {
		size_t disk_sector = page->anon.disk_sector;

		ASSERT(bitmap_all(stb.used_map, disk_sector/8, 1));
		bitmap_set_multiple(stb.used_map, disk_sector/8, 1, false);	
	}
}