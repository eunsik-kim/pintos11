/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "lib/kernel/bitmap.h"
#include "include/threads/loader.h"
#include "threads/vaddr.h"
#include "lib/debug.h"
#include "include/threads/pte.h"
#include "include/userprog/process.h"

// #include "lib/kernel/bitmap.c"
static struct frame_table frame_table;
static struct lock cp_lock;

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
	lock_init(&cp_lock);
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
		
	printf("***page type passed in: %d\n",type);

	ASSERT (VM_TYPE(type) != VM_UNINIT)
	struct thread *cur = thread_current();
	struct supplemental_page_table *spt = &cur->spt;

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
		
		if (type & VM_ANON)
			initializer = anon_initializer;
		else if (type & VM_FILE)
			initializer = file_backed_initializer;
		else
			PANIC("Initializer not implemented");

		if ((type & VM_FILE) && !(type & VM_MMAP) && writable) { // bss segment
			initializer = anon_initializer;	
			type &= ~VM_FILE;
			type |= VM_ANON | VM_BSS;
		}
		
		type = (writable & 1) ? type | VM_WRITABLE : type;
		type = (type & VM_STACK) ? type |= VM_DIRTY : type;
		printf("---page type before uninit_new: %d\n", type);
		uninit_new(page, pg_round_down(upage), init, type, aux, initializer);
		circular_list_init(&page->cp_elem);
		page->pml4 = cur->pml4;

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
	
	// lock_acquire(&spt->hash_lock);
	struct hash_elem *spt_e = hash_find(&spt->spt_page_hash, &search_page.hash_elem);
	// lock_release(&spt->hash_lock);
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
	// lock_acquire(&spt->hash_lock);
	bool succ = (hash_insert(&spt->spt_page_hash, &page->hash_elem)==NULL)? true:false;
	// lock_release(&spt->hash_lock);
	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	// lock_acquire(&spt->hash_lock);
	bool succ = (hash_delete(&spt->spt_page_hash, &page->hash_elem)==NULL)? true:false;
	// lock_release(&spt->hash_lock);
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
	struct thread *cur = thread_current();
	if(!(page->type & (VM_COW || VM_WRITABLE)))
		return false;
	page->type &= ~VM_COW;
	page->type |= VM_DIRTY;
	lock_acquire(&cp_lock);
	// Copy On Write
	if (is_alone(&page->cp_elem)){ //use frame alone
		lock_release(&cp_lock);
		if (!pml4_set_page(cur->pml4, page->va, page->frame->kva, 1))
			return false;
		return true;
	}
	list_remove(&page->cp_elem); //delete reference
	struct frame *new_frame = vm_get_frame();
	lock_release(&cp_lock);
	memcpy(new_frame->kva, page->frame->kva, PGSIZE);
	page->frame = new_frame;
	new_frame->page = page;
	if (!pml4_set_page(cur->pml4, page->va, new_frame->kva, 1))
		return false;
	
	return false;

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
	/* Check Validity */
	if (is_kernel_vaddr(addr) | addr==NULL)
		return false; 

	/* if fault occurs in syscall */
	uint64_t rsp = cur->rsp;
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
	// lock_init(&spt->hash_lock);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
	// src to dst. called after initialization of dst
	// lock_acquire(&src->hash_lock);
	return hash_apply(&src->spt_page_hash, hash_copy_action);
	// lock_release(&src->hash_lock);
}

/* Free the resource hold by the supplemental page table
 * Clear hash elements but not the hash table structure
 * (내용물만 지우기 init된 구조체는 유지) */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread */
		// lock_acquire(&spt->hash_lock);
		hash_destroy(&spt->spt_page_hash, hash_free_page);
		// lock_release(&spt->hash_lock);
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
	struct page *free_page = hash_entry(e, struct page, hash_elem);
	if (aux && (free_page->type & VM_MMAP)){
		struct list *inherit_list = aux;
		pml4_clear_page(free_page->pml4, free_page->va);
		list_push_back(inherit_list, &free_page->hash_elem.list_elem);
		return;
	}
	vm_dealloc_page(free_page);
}

bool
hash_copy_action(struct hash_elem *e, void *aux UNUSED)
{
	struct lazy_load_segment_aux *cp_aux;
	struct thread *cur = thread_current();
	struct page *src_page = hash_entry(e, struct page, hash_elem);
	struct page *dst_page = (struct page*)malloc(sizeof(struct page));

	// copy and init page
	memcpy(dst_page, src_page, sizeof(struct page));
	circular_list_init(&dst_page->cp_elem);
	dst_page->pml4 = cur->pml4;

	enum vm_type type = VM_TYPE(src_page->type);
	enum vm_type uninit_type = src_page->operations->type; //초기화 하기 전이면 위의 type이랑 operations type이 다를듯

	if (uninit_type == VM_UNINIT)
	{
		if (!(cp_aux = (struct lazy_load_segment_aux *)malloc(sizeof(struct lazy_load_segment_aux))))
			return false;
		
		memcpy(cp_aux, src_page->uninit.aux, sizeof(struct lazy_load_segment_aux));
		dst_page->uninit.aux = cp_aux;
		goto end;

	} else if (type & VM_FILE){
		if (!(cp_aux = (struct lazy_load_segment_aux *)malloc(sizeof(struct lazy_load_segment_aux))));
			return false;
		
		memcpy(cp_aux, src_page->file.aux, sizeof(struct lazy_load_segment_aux));
		dst_page->file.aux = cp_aux;
	}

	switch (VM_TYPE(src_page->type))
	{
		case(VM_FRAME | VM_FILE):
		case(VM_FRAME | VM_ANON):
			// for COW, make page unwritable
			if (src_page->type & VM_WRITABLE)
				ASSERT(pml4_set_page(src_page->pml4, src_page->va, src_page->frame->kva, 0));
			if (!pml4_set_page(cur->pml4, dst_page->va, dst_page->frame->kva, 0))
				return false;
			
			enum vm_type type = VM_COW;
			if (pml4_is_dirty(src_page->pml4, src_page->va))
				type |= VM_DIRTY;
			dst_page->type |= type;
			src_page->type |= type;

		case VM_ANON: //swap
		case VM_FILE: //disk
			//make a linked list of cp_elem to find child process
			list_insert(&src_page->cp_elem, &dst_page->cp_elem);
			goto end;
		default:
			PANIC("wrong access");
	}

end:
	return spt_insert_page(&cur->spt, dst_page);
}


/* page & frame 삭제 */
bool frame_delete(struct page *delete_page)
{
	struct hash_elem *e;
	if (delete_page->frame){ //if frame exists
		delete_page->type &= ~VM_FRAME;
		if (is_alone(&delete_page->cp_elem)){
			if(!(e=hash_delete(&frame_table, &delete_page->frame->hash_elem)))
				return false;
			free(delete_page->frame);
			delete_page->frame = NULL;
			return true;
		} else {
			delete_page->frame = NULL;
			pml4_clear_page(delete_page->pml4, delete_page->va);
		}
	}
	return false;
}

void disable_shared_frame(struct page *page)
{
	ASSERT(page->frame);

	page->frame = NULL;
	page->type &= ~VM_FRAME;
	pml4_clear_page(page->pml4, page->va);
	
}

void enable_shared_frame(struct page *page, struct frame *frame)
{
	ASSERT(!page->frame);
	ASSERT(page->type & VM_COW);

	page->type |= VM_FRAME;
	page->frame = frame;
	pml4_set_page(page->pml4, frame->page->va, frame->kva, 0);
}


static struct frame *find_shared_frame(struct page *page)
{
	if (page->type & VM_WRITABLE)
		return NULL;
	
	struct lazy_load_segment_aux *aux = page->uninit.aux;
	struct frame *frame;

	if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, 0))
		return NULL;
	page->type |= VM_COW;
	page->uninit.init = NULL;
	return frame;
}

