/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "include/threads/vaddr.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);
// swap_disk management
static struct bitmap *swap_bitmap;
static struct lock swap_lock;

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	// /* TODO: Set up the swap_disk. */
	struct swap_disk *swap_disk = disk_get(1,1);
	// // print_disk_status(); //check disk status

	lock_init(&swap_lock); //global lock for handling swap_disk
	// swap_size: max number of pages that can be stored in swap_disk
	size_t swap_size = disk_size(swap_disk)/PGSIZE*DISK_SECTOR_SIZE;
	// printf("---swap_disk init size: %d\n", swap_size);
	swap_bitmap = bitmap_create(swap_size);

}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &anon_ops;

	memset(page->frame->kva, 0, PGSIZE);
	page->type = type;

	struct anon_page *anon_page = &page->anon;

}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
	// lock_acquire(&swap_lock);

	// lock_release(&swap_lock);
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	// lock_acquire(&swap_lock);

	// lock_release(&swap_lock);
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	frame_delete(page);
	return;
}


