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
	if (page) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */

		/* TODO: Insert the page into the spt. */
		struct hash_elem *e = hash_entry(&page->hash_elem ,struct page, hash_elem);
		hash_insert(spt, e);
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	struct page search_page, *found_page;
	search_page.va = pg_round_down(va);
	
	struct hash_elem *spt_e;
	lock_acquire(&spt->hash_lock);
	spt_e = hash_find(&spt->spt_page_hash, &search_page.hash_elem);
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
	frame = (struct frame*)malloc(sizeof(struct page)); //allocate physical memory
	if (frame){
		frame->kva = palloc_get_page(PAL_USER);
		frame-> page = NULL;
		ASSERT (frame != NULL);
		ASSERT (frame->page == NULL);
		lock_acquire(&frame_table.frame_lock);
		hash_insert(&frame_table.ft_frame_hash, &frame->hash_elem);
		lock_release(&frame_table.frame_lock);
		return frame;
	} else {
		PANIC("No available frames! Implement page eviction.");
	}
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}



/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	if (user){
		if (!is_user_vaddr(addr))
			return false;
	} else {
		if (!is_kernel_vaddr(addr))
			return false;
	}

	/* TODO: Your code goes here */
	page = spt_find_page(spt, addr);

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

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	if(pml4_set_page(thread_current()->pml4, page->va, frame->kva))
		return swap_in (page, frame->kva);
	return false;
}


/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	hash_init(&spt->spt_page_hash, page_hash, page_less, NULL);
	lock_init(&spt->hash_lock);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
	// src to dst
	// lock_acquire(&src->hash_lock);
	// lock_acquire(&dst->hash_lock);
	// apply copy machanism

}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread */
	lock_acquire(&spt->hash_lock);
	hash_destroy (spt,  page_hash);
	lock_release(&spt->hash_lock);
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
	struct frame_table *frame_table=(struct frame_table*)malloc(sizeof(struct frame_table));
	hash_init(&frame_table->ft_frame_hash, frame_hash, frame_less, NULL);
	lock_init(&frame_table->frame_lock);
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
