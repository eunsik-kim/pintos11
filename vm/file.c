/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "userprog/process.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

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
	page->operations = &file_ops;
	page->type = type;
	page->file.data = page->uninit.aux;
	/* insert mmap list */
	if (page->file.data->mmap_list)	
		list_push_back(page->file.data->mmap_list, &page->file.mmap_elem);
	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page = &page->file;
	struct lazy_load_data *data = file_page->data;
	struct thread *cur = thread_current();

	if (page->type & VM_FRAME) {	// 파일 삭제가 되지 않고 drity인 경우 disk에 기록
		ASSERT(data && data->inode);
		if (page->type & VM_MMAP) {	
			bool isdrity = (page->type & VM_DIRTY) || (page->frame->dirty) || pml4_is_dirty(cur->pml4, page->va); 
			if (!data->inode->removed && isdrity) 
				ASSERT(inode_write_at(data->inode, page->frame->kva, data->readb, data->ofs) == data->readb);
			
			// delete mmap_list
			list_remove(&file_page->mmap_elem);
			if (data->mmap_list && list_empty(data->mmap_list)) {
				free(data->mmap_list);
				inode_close(data->inode);
			}
		}
		ftb_delete_frame(page);	
	}
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
}

/* Do the munmap */
void
do_munmap (void *addr) {
}
