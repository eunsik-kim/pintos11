/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "userprog/process.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);
static void delete_mmap_page(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->type = type;
	page->operations = &file_ops;
	page->file.data = page->uninit.aux;

	/* insert mmap list */
	if (page->file.data->mmap_list)	
		list_push_back(page->file.data->mmap_list, &page->file.mmap_elem);
	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	// enable pml4 for pages which is sharing redundant frames
	struct list_elem *next_e = &page->cp_elem; 
	while((next_e = list_next(next_e)) != &page->cp_elem) {
		struct page *f_page = list_entry(next_e, struct page, cp_elem);
		enable_redundant_frame(f_page, page->frame);	// enable pml4
	}
	return lazy_load_segment(page, page->file.data);
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	ASSERT(page->frame && (page->type & VM_FRAME));
	struct lazy_load_data *data = page->file.data;

	// isdirty
	if ((page->type & VM_DIRTY) || pml4_is_dirty(page->pml4, page->va)) 
		return true;

	// not removed
	if (!data->inode->removed) 
		ASSERT(inode_write_at(data->inode, page->frame->kva, data->readb, data->ofs) == data->readb);

	// disable pml4 for pages which is sharing redundant frames
	struct list_elem *next_e = &page->cp_elem; 
	do {
		struct page *f_page = list_entry(next_e, struct page, cp_elem);
		disable_redundant_frame(f_page);	
	} while((next_e = list_next(next_e)) != &page->cp_elem);
	
	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	delete_mmap_page(page);
	ftb_delete_frame(page);	
}


/* mmap에 의해 호출된 file들만 삭제시 disk에 기록, 로딩시 upload한 실행파일은 기록안함 */
static void delete_mmap_page(struct page *page) {
	struct file_page *file_page = &page->file;
	struct lazy_load_data *data = file_page->data;

	if (page->type & VM_MMAP) {		// only mmap call, not elf file
		bool isdrity = (page->type & VM_DIRTY) || pml4_is_dirty(thread_current()->pml4, page->va); 
		// not removed and drity
		if ((page->type & VM_FRAME) && isdrity && data->inode && !data->inode->removed) 
			ASSERT(inode_write_at(data->inode, page->frame->kva, data->readb, data->ofs) == data->readb);
		
		// delete mmap_list
		if (data->mmap_list) {
			list_remove(&file_page->mmap_elem);
			if (list_empty(data->mmap_list)) {
				inode_close(data->inode);
				free(data->mmap_list);
			}
		}
		// delete lazy load data
		free(data);
	}
}