/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "filesys/inode.h"
#include "include/userprog/process.h"

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
	//mmap va space allocation?? linear space 1:1 mapped with file
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;
	page->file.aux = page->uninit.aux; //메모리 영역이 겹쳐도 왼쪽에 값만 제대로 들어가면 되므로 상관없음. 여러 번 하는 경우에는 문제가 생길 수 있다
	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {

	struct list_elem *next_e = &page->cp_elem;
	while ((next_e = list_next(next_e)) != &page->cp_elem) {
		struct page *f_page = list_entry(next_e, struct page, cp_elem);
		enable_shared_frame(f_page, page->frame); //enable pml4
	}
	return lazy_load_segment(page, page->file.aux);
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	ASSERT(page->frame && (page->type & VM_FRAME));
	struct lazy_load_segment_aux *aux = page->file.aux;

	/* is dirty */
	if ((page->type & VM_DIRTY) || pml4_is_dirty(page->pml4, page->va))
		return true;

	/* not removed */
	if (!aux->inode->removed)
		ASSERT(inode_write_at(aux->inode, page->frame->kva, aux->read_bytes, aux->ofs)==aux->read_bytes);

	/* disable pml4 for pages that share frames */
	struct list_elem *next_e = &page->cp_elem;
	do{
		struct page *f_page = list_entry(next_e, struct page, cp_elem);
		disable_shared_frame(f_page);
	} while ((next_e = list_next(next_e)) != &page->cp_elem );

	return true;

}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	delete_mmap_page(page);
	frame_delete(page);
}

/* Do the mmap */
static void delete_mmap_page(struct page *page)
{
	struct file_page *file_page = &page->file;
	struct lazy_load_segment_aux *aux = file_page->aux;

	if (page->type & VM_MMAP)
	{
		bool is_dirty = (page->type & VM_DIRTY) || pml4_is_dirty(thread_current()->pml4, page->va);
		// not removed and dirty
		if ((page->type & VM_FRAME) && is_dirty && aux->inode && !aux->inode->removed)
			ASSERT(inode_write_at(aux->inode, page->frame->kva, aux->read_bytes, aux->ofs) == aux->read_bytes);

		inode_close(aux->inode);
		free(aux);
	}
}
