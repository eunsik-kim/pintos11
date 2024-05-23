/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "lib/kernel/bitmap.h"
#include "include/threads/loader.h"
#include "threads/vaddr.h"
#include "lib/debug.h"
#include "include/threads/pte.h"

// #include "lib/kernel/bitmap.c"
static struct frame_table frame_table;

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
	frame_table_init(); /* Initialize Frame Table */

}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
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
	struct page *page = spt_find_page (spt, upage);
	if (!page) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		// struct initializer *
		page = (struct page *)malloc(sizeof(struct page));
		if (!page)
			return false;

		bool(*initializer)(struct page* page, enum vm_type type, void*); //set initializer
		switch (VM_TYPE(type)) // get VM_TYPE between uninit/anon/file
		{
			case VM_ANON:
				initializer = anon_initializer;
				break;
			case VM_FILE:
				initializer = file_backed_initializer;
				break;
			default:
				return false;
		}

		if (writable)
			type = type | VM_WRITABLE;

		uninit_new(page, pg_round_down(upage), init, type, aux, initializer);

		/* TODO: Insert the page into the spt. */
		return spt_insert_page(spt, page);

	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	struct page search_page, *found_page;
	search_page.va = pg_round_down(va);
	
	lock_acquire(&spt->hash_lock);
	struct hash_elem *spt_e = hash_find(&spt->spt_page_hash, &search_page.hash_elem);
	lock_release(&spt->hash_lock);
	if (spt_e){
		found_page = hash_entry(spt_e, struct page, hash_elem);
		return found_page;
	}
	return NULL;
	
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {
	lock_acquire(&spt->hash_lock);
	bool succ = (hash_insert(&spt->spt_page_hash, &page->hash_elem)==NULL)? true:false;
	lock_release(&spt->hash_lock);
	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	lock_acquire(&spt->hash_lock);
	bool succ = (hash_delete(&spt->spt_page_hash, &page->hash_elem)==NULL)? true:false;
	lock_release(&spt->hash_lock);
	return succ;
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
	struct frame *frame = NULL;
	/* TODO: Fill this function. */
	frame = (struct frame*)malloc(sizeof(struct frame)); //allocate physical memory
	if (frame){
		frame->kva = palloc_get_page(PAL_USER|PAL_ZERO);
		ASSERT (frame != NULL);
		// ASSERT (frame->page == NULL); // after adding PAL_ZERO 
		lock_acquire(&frame_table.frame_lock);
		hash_insert(&frame_table.ft_frame_hash, &frame->hash_elem); //insert 하는 과정에서 not_present page fault
		lock_release(&frame_table.frame_lock);
		return frame;
	} else {
		PANIC("No available frames! Implement page eviction.");
	}
}

/* Growing the stack. */
bool
vm_stack_growth (void *addr UNUSED) {
	if((USER_STACK - (uint64_t)addr)>=(1<<20)) // out of stack limit. max size 1MB
		return false;
	
	void *stack_bottom = pg_round_down(addr);

	if (!vm_alloc_page(VM_ANON|VM_STACK, stack_bottom, true)) // initialize page and insert into spt
		return false;

	if (!vm_claim_page(stack_bottom))
		return false;
	
	thread_current()->stack_bottom = stack_bottom;
	return true;
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {


}




/* Return true on success
 * handles already existing pages in spt by swap_in
 * for other cases are handled back in page fault*/
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	
	struct thread *cur = thread_current();
	struct supplemental_page_table *spt = &cur->spt;
	struct page *page = NULL;
	/* Check Validity
	/* user -> kernel request or va does not exist */
	if (user && is_kernel_vaddr(addr) | addr==NULL)
		return false; 

	void* rsp = cur->rsp;
	if (!rsp){
		rsp = f->rsp;
	} else {
		cur->rsp = NULL;
	}


	page = spt_find_page(spt, addr);
	if (not_present){
		page = spt_find_page(spt, addr);
		if (!page){
			/* Case 1) Stack Request */
			if ((cur->stack_bottom - PGSIZE <= addr) && (addr < cur->stack_bottom))
				return vm_stack_growth(addr);

		} else {
			/* Case 2) uninit page in spt -> lazy load */
			return vm_do_claim_page (page); // try to allocate the page in physical memory(frame)	
		}
	} else {
		/* Case 3) page exists in swap disk */
		
	}
	return false;

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
vm_claim_page (void *va UNUSED) {
	struct page *page = NULL;
	/* TODO: Fill this function */
	page = spt_find_page(&thread_current()->spt, va);
	if (page==NULL){
		return false;
	} else {
		return vm_do_claim_page (page);
	}
}

/* Claim the PAGE and set up the mmu.
	allocate page frame for the given page */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();
	if (frame==NULL)
		return false;

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* Set page types */
	bool writable = (page->type & VM_WRITABLE);

	


	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	if(pml4_set_page(thread_current()->pml4, page->va, frame->kva, writable)){
		/// Add swap_in type for file backed memory
		return swap_in(page, frame->kva);
	}
	PANIC("vm do claim page - pml4 set page failed");
}


/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	// spt->hash_lock = (struct lock*)malloc(sizeof(struct lock));
    // spt->spt_page_hash = (struct hash*)malloc(sizeof(struct hash));
	hash_init(&spt->spt_page_hash, page_hash, page_less, NULL);
	lock_init(&spt->hash_lock);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
	// src to dst. called after initialization of dst
	lock_acquire(&src->hash_lock);

	// initialize dst spt
	// hash_init(&dst->spt_page_hash, page_hash, page_less, NULL);
	// lock_init(&dst->hash_lock);0
	// lock_acquire(&dst->hash_lock);
	lock_acquire(&dst->hash_lock);

	// copy src hash elem to dst

	// hash_apply(dst, hash_copy);

	lock_release(&src->hash_lock);
	lock_release(&dst->hash_lock);

		// apply copy machanism

}

/* Free the resource hold by the supplemental page table
 * Clear hash elements but not the hash table structure
 * (내용물만 지우기 init된 구조체는 유지) */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread */
	if (hash_size(&spt->spt_page_hash)){
		lock_acquire(&spt->hash_lock);
		hash_clear(&spt->spt_page_hash, hash_free_page); //free hash elements
		lock_release(&spt->hash_lock);
	}	
	/* TODO: writeback all the modified contents to the storage. */
	// applied to file backed files?
}



/*  -----  Hash Functions  -----  */

unsigned
frame_hash(const struct hash_elem *p, void *aux UNUSED){
	const struct frame *fp = hash_entry(p, struct frame, hash_elem);
	return hash_bytes(&fp->kva, sizeof fp->kva);
}

bool
frame_less(	const struct hash_elem *a_,
			const struct hash_elem *b_, void *aux UNUSED){
	const struct frame *a = hash_entry(a_, struct frame, hash_elem);
	const struct frame *b = hash_entry(b_, struct frame, hash_elem);
	return a->kva < b->kva;
}

struct frame_table *
frame_table_init(){
	hash_init(&frame_table.ft_frame_hash, frame_hash, frame_less, NULL);
	lock_init(&frame_table.frame_lock);
}


/* Returns a hash value for page p. */
unsigned
page_hash (const struct hash_elem *p_, void *aux UNUSED) {
  const struct page *p = hash_entry (p_, struct page, hash_elem);
  return hash_bytes (&p->va, sizeof p->va);
}

/* Returns true if page a precedes page b. */
bool
page_less (const struct hash_elem *a_,
           const struct hash_elem *b_, void *aux UNUSED) {
  const struct page *a = hash_entry (a_, struct page, hash_elem);
  const struct page *b = hash_entry (b_, struct page, hash_elem);
  return a->va < b->va;
}

void 
hash_free_page(struct hash_elem *e, void *aux UNUSED)
{
	struct page *p = hash_entry(e, struct page, hash_elem);
	vm_dealloc_page(p);
}

bool
hash_copy_action(struct hash_elem *e, void *aux UNUSED)
{
	struct lazy_load_segment_aux *cp_aux;
	struct thread *cur = thread_current();
	struct page *src_page = hash_entry(e, struct page, hash_elem);
	struct page *dst_page = (struct page*)calloc(1, sizeof(struct page));

	// copy
	memcpy(dst_page, src_page, sizeof(struct page));
	// circular list init
	dst_page->pml4 = cur->pml4;

	enum vm_type type = VM_TYPE(src_page->type);
	enum vm_type uninit_type = src_page->operations->type; //초기화 하기 전이면 위의 type이랑 operations type이 다를듯

	if ((uninit_type == VM_UNINIT) || (type & VM_FILE))
	{
		;
	}

	switch (VM_TYPE(src_page->type))
	{
	case (VM_FRAME| VM_FILE ):
		break;
	
	default:
		break;
	}
	

}



/* page & frame 삭제 */
bool frame_delete(struct page *delete_page){ 
	struct thread *cur = thread_current();
	if (delete_page->frame){ //if frame exists
		// ASSERT(delete_page->frame->unwritable>=0);
		if (!delete_page->frame->unwritable){
			hash_delete(&frame_table, &delete_page->frame->hash_elem);
			free(delete_page->frame);
		} else{
			delete_page->frame->unwritable--;
			pml4_clear_page(cur->pml4, delete_page->va);
		}
	}

}