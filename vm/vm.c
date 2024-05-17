/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/mmu.h"
#include "vm/uninit.h"
#include "userprog/process.h"

static struct frame_table ftb;

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
	hash_init(&ftb.frames, frame_hash, frame_less, NULL);
	lock_init(&ftb.hash_lock);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		struct page *new_page = (struct page *) calloc(1, sizeof(struct page));
		if (new_page == NULL)
			return false;

		bool (*initializer)(struct page *, enum vm_type, void *);
		if (type & VM_ANON)
			initializer = anon_initializer;
		else if (type & VM_FILE)
			initializer = file_backed_initializer;
		else 
			PANIC("Initializer not implemented ");
		type = (writable) ? type | VM_WRITABLE : type;
		uninit_new(new_page, pg_round_down(upage), init, type, aux, initializer);
		
		/* TODO: Insert the page into the spt. */
		return spt_insert_page(spt, new_page);
	}	
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	struct page label_page, *find_page;
	label_page.va = pg_round_down(va);
	struct hash_elem *find_e = hash_find(&spt->pages, &label_page.hash_elem);
	find_page = (find_e != NULL) ? hash_entry(find_e, struct page, hash_elem) : NULL;
	return find_page;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt, struct page *page) {
	lock_acquire(&spt->hash_lock);
	struct hash_elem *insert_e = hash_insert(&spt->pages, &page->hash_elem);
	int succ = (insert_e == NULL) ? true : false;
	lock_release(&spt->hash_lock);
	return succ;
}

/* remove PAGE in spt */
void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	lock_acquire(&spt->hash_lock);
	int succ = hash_delete(&spt->pages, &page->hash_elem);
	lock_release(&spt->hash_lock);
	ASSERT(succ);
	vm_dealloc_page(page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame;
	void *new_page = palloc_get_page(PAL_USER);
	if (new_page != NULL){
		frame = (struct frame *) calloc(1, sizeof(struct frame));
		frame->kva = new_page;
		frame->unwritable = 0;
		hash_insert(&ftb.frames, &frame->hash_elem);
	} else {
		PANIC("Not implemented");
		frame = vm_evict_frame();
	}
	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* 
 * 
 */
bool
vm_try_handle_fault (struct intr_frame *f, void *addr, bool user, 
		bool write, bool not_present) {
	struct thread *cur = thread_current();
	
	if (addr == NULL || is_kernel_vaddr(addr))
		return false;

	struct page *page = spt_find_page(&cur->spt, addr);
	if (!page)
		return false;
	if (page->operations->type == VM_UNINIT)
		goto end;
	
	switch (VM_TYPE(page->type))
	{
	case VM_FRAME | VM_FILE:
	case VM_FRAME | VM_ANON:
		if (write && !(page->type & VM_CPWRITE) && !(page->type & VM_WRITABLE))
			return false;
		page->type &= ~VM_CPWRITE;
		if (!page->frame->unwritable){
			if (!pml4_set_page(cur->pml4, page->va, page->frame->kva, 1))
				return false;
			return true;
		}
		page->frame->unwritable--;
		struct frame *new_frame = vm_get_frame();	
		if (new_frame == NULL){
			printf("Not enough page to cpywrite");
			return false;
		}
		memcpy(new_frame->kva, page->frame->kva, PGSIZE);
		new_frame->page = page;
		page->frame = new_frame;
		pml4_set_page(cur->pml4, page->va, new_frame->kva, 1);
		return true;

	case VM_ANON:	
	case VM_FILE:
		goto end;
	default:
		PANIC("wrong access");
	}	

end:
	return vm_do_claim_page (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va) {
	struct page *page = spt_find_page(&thread_current()->spt, va);
	if (page == NULL)
		return false;
	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();	
	if (frame == NULL)
		return false;
	/* Set links */
	frame->page = page;
	page->frame = frame;

	page->type |= VM_FRAME;
	int is_writable = page->type & VM_WRITABLE;
	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	ASSERT(page->va);
	if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, is_writable))
		return false;
	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	hash_init(&spt->pages, page_hash, page_less, NULL);
	lock_init(&spt->hash_lock);
	list_init(&spt->lazy_list);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src) {
	return hash_apply(&src->pages, hash_copy_action);
}

/*
 * 1. uninit인 경우 2. frame에 존재하는 경우, 3.disk에 존재하는 경우, 4.SWAP에 존재하는경우
 * 각 page 복사, frame은 pagefault시 복사, lazy_load_data 복사,  
 */
bool hash_copy_action(struct hash_elem *e, void *aux)
{	
	struct thread *cur = thread_current();
	struct page *src_page = hash_entry(e, struct page, hash_elem);
	struct page *des_page = (struct page *) calloc(1, sizeof(struct page));

	if (src_page->operations->type == VM_UNINIT){
		// aux copy
		struct lazy_load_data *cp_aux = (struct lazy_load_data *) calloc(1, sizeof(struct lazy_load_data));
		memcpy(cp_aux, src_page->uninit.aux, sizeof(struct lazy_load_data));
		// aux insert list
		list_push_back(&cur->spt.lazy_list, &cp_aux->elem); 
		memcpy(des_page, src_page, sizeof(struct page));
		des_page->uninit.aux = cp_aux;
		return spt_insert_page(&cur->spt, des_page);
	}

	switch (VM_TYPE(src_page->type))
	{
	case VM_FRAME | VM_FILE:	
	case VM_FRAME | VM_ANON:
		// if data in frame, do not need to cpy frame 
		ASSERT(pml4_set_page((uint64_t *)aux, src_page->va, src_page->frame->kva, 0));
		if (!pml4_set_page(cur->pml4, src_page->va, src_page->frame->kva, 0))
			return false;
		src_page->type |= VM_CPWRITE; 
		src_page->frame->unwritable++;
		goto end;

	case VM_ANON:	
	case VM_FILE:
		goto end;

	default:
		PANIC("wrong access");
	}
end:
	memcpy(des_page, src_page, sizeof(struct page));
	return spt_insert_page(&cur->spt, des_page);
}


/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	lock_acquire(&spt->hash_lock);
	hash_destroy(&spt->pages, hash_free_page);
	lock_release(&spt->hash_lock);

	// destroy lazy load data 
	int length = list_size(&spt->lazy_list);
	while (length--) 
		free(list_entry(list_pop_front(&spt->lazy_list), struct lazy_load_data, elem));
}

/* 
 * anon or file destory할 때, page삭제 하며 frame 삭제할 때 사용하는 함수 
 * cpwrite에 의해 unwritable cnt가 0인 경우만 삭제
 */
bool ftb_delete_frame(struct page *delete_page){
	if (delete_page->frame){
		if (!delete_page->frame->unwritable) {
			hash_delete(&ftb, &delete_page->frame->hash_elem);
			free(delete_page->frame);
		}
		else {
			delete_page->frame->unwritable--;
			pml4_clear_page(thread_current()->pml4, delete_page->va); // for keeping multiple page destory 
		}
	}
}

/* destory할때 사용하는 함수 */
void hash_free_page(struct hash_elem *e, void *aux UNUSED)
{	
	struct page *free_page = hash_entry(e, struct page, hash_elem);
	vm_dealloc_page(free_page);
}

/* spt hashing 하는 함수 */
unsigned page_hash(const struct hash_elem *p_, void *aux UNUSED) {
	const struct page *p = hash_entry(p_, struct page, hash_elem);
	return hash_bytes(&p->va, sizeof p->va);
}

/* Returns true if page a precedes page b. */
bool page_less(const struct hash_elem *a_,
           const struct hash_elem *b_, void *aux UNUSED) {
	const struct page *a = hash_entry(a_, struct page, hash_elem);
	const struct page *b = hash_entry(b_, struct page, hash_elem);

	return a->va < b->va;
}

/* ftb hashing 하는 함수 */
unsigned frame_hash(const struct hash_elem *p_, void *aux UNUSED) {
	const struct frame *p = hash_entry(p_, struct frame, hash_elem);
	return hash_bytes(&p->kva, sizeof p->kva);
}

/* Returns true if page a precedes page b. */
bool frame_less(const struct hash_elem *a_,
           const struct hash_elem *b_, void *aux UNUSED) {
	const struct frame *a = hash_entry(a_, struct frame, hash_elem);
	const struct frame *b = hash_entry(b_, struct frame, hash_elem);
	return a->kva < b->kva;
}
