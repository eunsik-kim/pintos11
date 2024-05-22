/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/mmu.h"
#include "vm/uninit.h"
#include "userprog/process.h"

static struct frame_table ftb;
static struct hash cpy_mmap_list;

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
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, uint64_t writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)
	struct thread *cur = thread_current();
	struct supplemental_page_table *spt = &cur->spt;

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

		if (type & VM_FILE) {
			if (writable >= VM_MMAP) {
				struct lazy_load_data *data = aux;
				data->mmap_list = writable & ~1;
				type = type | VM_MMAP;
			} else if (writable & 1)	{ // bss segment
				initializer = anon_initializer;	
				type &= ~VM_FILE;
				type |= VM_ANON | VM_BSS;
			}
		}
		type = (writable & 1) ? type | VM_WRITABLE : type;
		type = (type & VM_STACK) ? type |= VM_DIRTY : type;
		uninit_new(new_page, pg_round_down(upage), init, type, aux, initializer);
		// make circular linked list to find child process which is forked by parent in swaped state
		circular_list_init(&new_page->cp_elem);
		new_page->pml4 = cur->pml4;

		/* TODO: Insert the page into the spt. */
		return spt_insert_page(spt, new_page);
	}	
err:
	return false;
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	hash_init(&spt->pages, page_hash, page_less, NULL);
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
	struct hash_elem *insert_e = hash_insert(&spt->pages, &page->hash_elem);
	int succ = (insert_e == NULL) ? true : false;
	return succ;
}

/* remove PAGE in spt */
void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	ASSERT(hash_delete(&spt->pages, &page->hash_elem));
	vm_dealloc_page(page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *nframe, *victim = NULL;
	/* TODO: The policy for eviction is up to you. */
	struct hash_iterator i;
	struct thread *cur = thread_current();
	hash_first(&i, &ftb.frames);
	while (1) {
		if (!hash_next(&i)){
			hash_first(&i, &ftb.frames);
			hash_next(&i);
		}

		nframe = hash_entry(hash_cur(&i), struct frame, hash_elem);
		/* using clock algorithm */
		if (pml4_is_accessed(cur->pml4, nframe->page->va))
			pml4_set_accessed(cur->pml4, nframe->page->va, false);
		else {
			victim = nframe;
			break;
		}
	}
	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim = vm_get_victim ();
	if (!swap_out(victim->page))
		return NULL;
	return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame;
	void *kva = palloc_get_page(PAL_USER);
	if (kva != NULL){
		frame = (struct frame *) calloc(1, sizeof(struct frame));
		if (!frame)
			return NULL;
		frame->kva = kva;
		hash_insert(&ftb.frames, &frame->hash_elem);
	} else 
		frame = vm_evict_frame();
	
	ASSERT (frame != NULL);
	return frame;
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page) {
	struct thread *cur = thread_current();
	if (!(page->type & (VM_CPWRITE || VM_WRITABLE)))
		return false;
	page->type &= ~VM_CPWRITE;
	page->type |= VM_DIRTY;

	// cp on wrt
	if (is_alone(&page->cp_elem)){		// use frame alone
		if (!pml4_set_page(cur->pml4, page->va, page->frame->kva, 1))
			return false;
		return true;
	}
	list_remove(&page->cp_elem); 	// delete redundant refer

	struct frame *new_frame = vm_get_frame();	
	memcpy(new_frame->kva, page->frame->kva, PGSIZE);
	page->frame = new_frame;
	new_frame->page = page;
	if (!pml4_set_page(cur->pml4, page->va, new_frame->kva, 1))
		return false;
	return true;	
}

// check rsp in PGSIZE down from last rsp(simple is best!)
bool check_rsp_valid(void *addr) {
	struct thread *cur = thread_current();
	return (cur->stack_bottom - PGSIZE <= addr) && (addr < cur->stack_bottom);
}

/* Growing the stack. alloc new anon page */
bool vm_stack_growth (void *addr) {
	if ((USER_STACK - (uint64_t)addr) >= (1 << 20))	 // out of stack limit
		return false;

	void *stack_bottom = pg_round_down(addr);
	if (!vm_alloc_page(VM_ANON | VM_STACK, stack_bottom, true))
		return false;

	if (!vm_claim_page(stack_bottom))
		return false;

	thread_current()->stack_bottom = stack_bottom;
	return true;
}

/* 잘못된 접근 예외처리, stack 증가, cpwrite에 따른 frame 복사처리,
 * lazyload를 실행한 뒤 성공 유무 return */
bool
vm_try_handle_fault (struct intr_frame *f, void *addr, bool user, 
									bool write, bool not_present) {
	struct page *page;
	struct thread *cur = thread_current();
	if (addr == NULL || is_kernel_vaddr(addr))
		return false;

	// if fault occurs in syscall
	uint64_t cur_rsp;
	if (cur->last_rsp){
		cur_rsp = cur->last_rsp;
		cur->last_rsp = NULL;
	} else 
		cur_rsp = f->rsp;

	// check stack growth (simple policy)
	if (not_present && check_rsp_valid(cur_rsp)){
		if (!vm_stack_growth(addr))
			return false;
		return true;
	}

	if (!(page = spt_find_page(&cur->spt, addr)))
		return false;

	if (!not_present && page->operations->type == VM_UNINIT)	// lazy load
		goto end;
	
	switch (VM_TYPE(page->type))	// cpwrite
	{
	case (VM_FRAME | VM_FILE):
	case (VM_FRAME | VM_ANON):
		if (write && vm_handle_wp(page))
			return true;
		return false;
	case VM_ANON:			// swap
	case VM_FILE:			// disk
		if (!not_present)
			return false;
		goto end;
	default:
		PANIC("wrong access");
	}	

end:
	if (write)
		page->type |= VM_DIRTY;
	return vm_do_claim_page (page);
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

	/* Set links */
	page->frame = frame;
	frame->page = page;

	page->type |= VM_FRAME;
	int is_writable = page->type & VM_WRITABLE;
	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	ASSERT(page->va);
	if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, is_writable))
		return false;
	return swap_in (page, frame->kva);
}

/* cpy_mmap_list destory할때 사용하는 함수 */
void mm_free_frame(struct hash_elem *e, void *aux UNUSED) {	
	struct frame *mm_list_elem = hash_entry(e, struct frame, hash_elem);
	free(mm_list_elem);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src) {

	hash_init(&cpy_mmap_list, frame_hash, frame_less, NULL);
	bool flg = hash_apply(&src->pages, hash_copy_action);
	hash_destroy(&cpy_mmap_list, mm_free_frame);
	return flg;
}

/* hash 안에서 mmap_list와 new mmap_list는 1-1, 해당 mmap_list 찾아서 return */
struct list *find_new_mmap_list(struct page *src_page)
{
	struct frame *find_frame, *label_frame = calloc(1, sizeof(struct frame));
	if (label_frame == NULL)
		return NULL;

	struct list *mmap_list;
	label_frame->kva = src_page->file.data->mmap_list;
	struct hash_elem *find_elem = hash_find(&cpy_mmap_list, label_frame);
	// hash안에 없는경우 새롭게 생성 후 hash에 삽입, 찾으면 mmap_list return
	if (!find_elem) {	
		if ((mmap_list = (struct list *)malloc(sizeof (struct list))) == NULL)
			return NULL;

		// initialize mmap_list 
		list_init(mmap_list);
		label_frame->page = (void *)mmap_list;
		struct inode *inode = src_page->file.data->inode;
		inode_reopen(inode);
		hash_insert(&cpy_mmap_list, label_frame);
	} else {
		find_frame = hash_entry(find_elem, struct frame, hash_elem);
		mmap_list = (struct list *)find_frame->page;
	}
	return mmap_list;
}

/*
 * 1. uninit인 경우 2. frame에 존재하는 경우, 3.disk or SWAP에 존재하는경우
 * 각 page의 lazy_load_data, mmap_list 복사, frame은 vm_handle_wp에서 복사 되도록 설정 
 */
bool hash_copy_action(struct hash_elem *e, void *aux UNUSED)
{	
	struct list *mmap_list;	
	struct lazy_load_data *cp_aux;
	struct thread *cur = thread_current();
	struct page *src_page = hash_entry(e, struct page, hash_elem);
	struct page *dst_page = (struct page *) calloc(1, sizeof(struct page));
	
	// cp and init page
	memcpy(dst_page, src_page, sizeof(struct page));
	circular_list_init(&dst_page->cp_elem);
	dst_page->pml4 = cur->pml4;

	enum vm_type ty = VM_TYPE(src_page->type);
	enum vm_type uninit_type = src_page->operations->type;

	if ((uninit_type == VM_UNINIT) || (ty & VM_FILE))
	{
		// aux copy for lazy load
		if (!(cp_aux = (struct lazy_load_data *)malloc(sizeof(struct lazy_load_data))))
			return false;
		
		if (uninit_type == VM_UNINIT){
			memcpy(cp_aux, src_page->uninit.aux, sizeof(struct lazy_load_data));
			dst_page->uninit.aux = cp_aux;	
		} else {
			memcpy(cp_aux, src_page->file.data, sizeof(struct lazy_load_data));
			dst_page->file.data = cp_aux;	
		}
		
		// cp mmap_list for munmap call
		if (src_page->type & VM_MMAP) {
			if ((mmap_list = find_new_mmap_list(src_page)) == NULL)
				return false;

			cp_aux->mmap_list = mmap_list;
			if (uninit_type == VM_UNINIT) // uninit is not listed in mmap_list
				goto end;
			list_push_back(mmap_list, &dst_page->file.mmap_elem);	
		}
	}

	switch (VM_TYPE(src_page->type))
	{	
	case (VM_FRAME | VM_FILE):
	case (VM_FRAME | VM_ANON):
		// for cpwrite, make page unwritable
		if (src_page->type & VM_WRITABLE)
			ASSERT(pml4_set_page(src_page->pml4, src_page->va, src_page->frame->kva, 0));			
		if (!pml4_set_page(cur->pml4, dst_page->va, dst_page->frame->kva, 0))
			return false;
			
		enum vm_type ty = VM_CPWRITE;
		if (pml4_is_dirty(src_page->pml4, src_page->va))
			ty |= VM_DIRTY;
		dst_page->type |= ty;
		src_page->type |= ty;

	case VM_ANON:		// swap	
	case VM_FILE:		// disk
		// make circular linked list to find child process
		list_insert(&src_page->cp_elem, &dst_page->cp_elem);
		goto end;
		goto end;

	default:
		PANIC("wrong access");
	}
end:
	return spt_insert_page(&cur->spt, dst_page);
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	hash_destroy(&spt->pages, hash_free_page);
}	

/* spt destory할때 사용하는 함수, exec할 땐 mmap page는 안지우고 살려둠 */
void hash_free_page(struct hash_elem *e, void *aux)
{	
	struct page *free_page = hash_entry(e, struct page, hash_elem);
	if (aux && (free_page->type & VM_MMAP)) {
		struct list *inherit_list = aux;
		pml4_clear_page(free_page->pml4, free_page->va); 	// for keeping multiple page destory 
		list_push_back(inherit_list, &free_page->hash_elem.list_elem);
		return;
	}
	vm_dealloc_page(free_page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	if (!is_alone(&page->cp_elem))
		list_remove(&page->cp_elem);	
	destroy (page);
	free (page);
}

/* 
 * anon or file destory할 때, frame 삭제하는 함수, 삭제한 경우만 true return
 * cpwrite에 의해 frame을 중복 참조하지 않는 경우 경우만 삭제
 */
bool ftb_delete_frame(struct page *delete_page){
	struct hash_elem *e;
	if (delete_page->frame){
		delete_page->type &= ~VM_FRAME;
		if (is_alone(&delete_page->cp_elem)) {	// pml4 destroy in pml4 destroy
			if (!(e = hash_delete(&ftb, &delete_page->frame->hash_elem)))
				return false; 

			free(delete_page->frame);
			delete_page->frame = NULL;		// dangler pointer	
			return true;
		} else {
			delete_page->frame = NULL;		// dangler pointer	
			pml4_clear_page(delete_page->pml4, delete_page->va); 	// for keeping pml4 page destory safely
		}
	}
	return false;
}

/* frame을 공유하는 page들이 swap out될 때 사용, kva를 user의 pml4에서 제거 */
void disable_redundant_frame(struct page *page) {
	ASSERT(page->frame);

	page->frame = NULL;				// dangler pointer	
	page->type &= ~VM_FRAME;
	pml4_clear_page(page->pml4, page->va); 	
}

/* frame을 공유하는 page들이 swap in될 때 사용, kva를 user의 pml4에 추가 */
void enable_redundant_frame(struct page *page, struct frame *n_frame){
	ASSERT(!page->frame);
	ASSERT(page->type & VM_CPWRITE);

	page->type |= VM_FRAME;
	page->frame = n_frame;
	pml4_set_page(page->pml4, n_frame->page->va, n_frame->kva, 0); 
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

/* no msync, no need(yet not impl)*/
static struct frame *find_shared_frame(struct page *page)
{
	if (page->type & VM_WRITABLE)
		return NULL; 

	struct lazy_load_data *data = page->uninit.aux;
	struct frame *find_frame;

	if (!pml4_set_page(thread_current()->pml4, page->va, find_frame->kva, 0))
		return NULL;
	page->type |= VM_CPWRITE;
	page->uninit.init = NULL;		// not lazy load
	return find_frame;
}