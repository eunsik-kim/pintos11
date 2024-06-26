#ifndef VM_VM_H
#define VM_VM_H
#include <stdbool.h>
#include "threads/palloc.h"
#include "lib/kernel/hash.h"
#include "filesys/inode.h"
#include "threads/mmu.h"
#include "userprog/syscall.h"

enum vm_type {
	/* page not initialized */
	VM_UNINIT = 0,
	/* page not related to the file, aka anonymous page */
	VM_ANON = 1,
	/* page that realated to the file */
	VM_FILE = (1<<1),
	/* page that hold the page cache, for project 4 */
	VM_PAGE_CACHE = (1<<1) + 1,

	/* Bit flags to store state */
	VM_FRAME = (1<<2),

	/* Auxillary bit flag marker for store information. You can add more
	 * markers, until the value is fit in the int. */
	VM_MMAP = (1<<3),
	VM_STACK = (1<<4),
	VM_WRITABLE = (1<<5),
	VM_CPWRITE = (1<<6),		// cpwrite
	VM_DIRTY = (1<<7),
	VM_ACCESS = (1<<8),
	VM_NOSWAP = (1<<9),
	VM_BSS = (1<<10),
	/* DO NOT EXCEED THIS VALUE. */
	VM_MARKER_END = (1 << 31),
};

#include "vm/uninit.h"
#include "vm/anon.h"
#include "vm/file.h"
#ifdef EFILESYS
#include "filesys/page_cache.h"
#endif

struct page_operations;
struct thread;

#define VM_TYPE(type) (type & 7)

/* The representation of "page".
 * This is kind of "parent class", which has four "child class"es, which are
 * uninit_page, file_page, anon_page, and page cache (project4).
 * DO NOT REMOVE/MODIFY PREDEFINED MEMBER OF THIS STRUCTURE. */
struct page {
	const struct page_operations *operations;
	void *va;              /* Address in terms of user space */
	struct frame *frame;   /* Back reference for frame */

	/* Your implementation */
	enum vm_type type;					// check status
	uint64_t *pml4;						// swap out
	struct list_elem cp_elem;			// for copy 
	struct hash_elem hash_elem;			// for spt
	
	/* Per-type data are binded into the union.
	 * Each function automatically detects the current union */
	union {
		struct uninit_page uninit;
		struct anon_page anon;
		struct file_page file;
#ifdef EFILESYS
		struct page_cache page_cache;
#endif
	};
};

#include <threads/synch.h>
 
/* The representation of "frame" */
struct frame {
	struct hash_elem hash_elem;
	void *kva;
	struct page *page;
};

/* frame table for tracking USER frame(page) to evict page */
struct frame_table
{
	struct hash frames;
	struct lock frame_lock;
};

/* The function table for page operations.
 * This is one way of implementing "interface" in C.
 * Put the table of "method" into the struct's member, and
 * call it whenever you needed. */
struct page_operations {
	bool (*swap_in) (struct page *, void *);
	bool (*swap_out) (struct page *);
	void (*destroy) (struct page *);
	enum vm_type type;
};

#define swap_in(page, v) (page)->operations->swap_in ((page), v)
#define swap_out(page) (page)->operations->swap_out (page)
#define destroy(page) \
	if ((page)->operations->destroy) (page)->operations->destroy (page)

/* Representation of current process's memory space.
 * We don't want to force you to obey any specific design for this struct.
 * All designs up to you for this. */
struct supplemental_page_table {
	struct hash pages;
};

/* vm */
unsigned frame_hash(const struct hash_elem *p_, void *aux);
bool frame_less(const struct hash_elem *a_, const struct hash_elem *b_, void *aux);
bool page_less(const struct hash_elem *a_, const struct hash_elem *b_, void *aux);
unsigned page_hash(const struct hash_elem *p_, void *aux );
void hash_free_page(struct hash_elem *e, void *aux);
bool ftb_delete_frame(struct page *delete_page);

/* stack growth */
bool vm_stack_growth(void *addr);
bool check_rsp_valid(void *addr);

/* share and cp page */
bool hash_copy_action(struct hash_elem *e, void *aux);
// bool Is_alone(struct list_elem *elem, struct semaphore *sema);
// void List_insert(struct list_elem *dst_elem, struct list_elem *src_elem, struct semaphore *sema);
// void List_remove(struct list_elem *elem, struct semaphore *sema);

/* sap out & in */
void disable_redundant_frame(struct page *page);
void enable_redundant_frame(struct page *page, struct frame *n_frame);

/* check whether page is sharing or not */
#define is_alone(elem) (((elem)->next) == (elem))

/* initialize for copy elem which is used in hash_copy_action */
#define circular_list_init(elem) do { \
    (elem)->next = (elem); \
    (elem)->prev = (elem); \
} while (0)

#include "threads/thread.h"
void supplemental_page_table_init (struct supplemental_page_table *spt);
bool supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src);
void supplemental_page_table_kill (struct supplemental_page_table *spt);
struct page *spt_find_page (struct supplemental_page_table *spt,
		void *va);
bool spt_insert_page (struct supplemental_page_table *spt, struct page *page);
void spt_remove_page (struct supplemental_page_table *spt, struct page *page);

void vm_init (void);
bool vm_try_handle_fault (struct intr_frame *f, void *addr, bool user,
		bool write, bool not_present);

#define vm_alloc_page(type, upage, writable) \
	vm_alloc_page_with_initializer ((type), (upage), (writable), NULL, NULL)
bool vm_alloc_page_with_initializer (enum vm_type type, void *upage,
		bool writable, vm_initializer *init, void *aux);
void vm_dealloc_page (struct page *page);
bool vm_claim_page (void *va);
enum vm_type page_get_type (struct page *page);

#endif  /* VM_VM_H */
