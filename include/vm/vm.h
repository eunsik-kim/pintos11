#ifndef VM_VM_H
#define VM_VM_H
#include <stdbool.h>
#include "threads/palloc.h"
#include "lib/kernel/hash.h"
#include "lib/debug.h"
#include "threads/synch.h"

struct frame_table {
	
	struct hash ft_frame_hash;	// frame table
	// struct list ft_list; //
	struct lock frame_lock; 		//frame_table lock

};

enum vm_type {
	/* page not initialized */
	VM_UNINIT = 0,
	/* page not related to the file, aka anonymous page */
	VM_ANON = 1, // bit 0
	/* page that realated to the file */
	VM_FILE = 2,
	/* page that hold the page cache, for project 4 */
	VM_PAGE_CACHE = 3, //bit 1

	/* Bit flags to store state */
	VM_FRAME = (1<<2), // bit 2

	/* Auxillary bit flag marker for store information. You can add more
	 * markers, until the value is fit in the int. */
	
	VM_WRITABLE = (1 << 3), //bit 3
	VM_ACCESS = (1 << 4),  //bit 4
	VM_DIRTY = (1 << 5), //bit 5
	VM_STACK = (1 << 6), //bit 6
	VM_COW = (1 << 7),
	VM_NOSWAP = (1 << 8),
	VM_BSS = (1 << 9),
	VM_MMAP = (1 << 10), //
	VM_MARKER_4 = (1 << 11),

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

#define VM_TYPE(type) ((type) & 7)

/* The representation of "page".
 * This is kind of "parent class", which has four "child class"es, which are
 * uninit_page, file_page, anon_page, and page cache (project4).
 * DO NOT REMOVE/MODIFY PREDEFINED MEMBER OF THIS STRUCTURE. */
struct page {
	const struct page_operations *operations;
	void *va;              /* Address in terms of user space */
	struct frame *frame;   /* Back reference for frame */

	/* Your implementation */
	enum vm_type type;
	struct hash_elem hash_elem;
	struct list_elem cp_elem; // for copying
	uint64_t *pml4; // for shared frame

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

/* The representation of "frame" */
struct frame {
	void *kva; //kernal virtual address beware of void pointer
	struct page *page; //page
	struct hash_elem hash_elem; //hash table element
	// int unwritable;
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
	// struct lock hash_lock;
	struct hash spt_page_hash;
};




#include "threads/thread.h"
void supplemental_page_table_init (struct supplemental_page_table *spt);
bool supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src);
void supplemental_page_table_kill (struct supplemental_page_table *spt);
struct page *spt_find_page (struct supplemental_page_table *spt,
		void *va);
bool spt_insert_page (struct supplemental_page_table *spt, struct page *page);
void spt_remove_page (struct supplemental_page_table *spt, struct page *page);
bool frame_delete(struct page *);

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
bool vm_stack_growth (void *addr UNUSED);




struct frame_table* frame_table_init();

void disable_shared_frame(struct page *page);
void enable_shared_frame(struct page *page, struct frame *frame);
static struct frame *find_shared_frame(struct page *page);


/*  -----  Hash Functions  -----  */

/* creating hash elem */
unsigned frame_hash(const struct hash_elem *p, void *aux UNUSED);
bool frame_less(	const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED);

unsigned page_hash (const struct hash_elem *p_, void *aux UNUSED);
bool page_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED);

/* hash action */
void hash_free_page(struct hash_elem *e, void *aux UNUSED);
bool hash_copy_action(struct hash_elem *e, void *aux);

/* initialize copy elem */
#define circular_list_init(elem) do { \
	(elem)->next = (elem); \
	(elem)->prev = (elem); \
} while (0)

/* check if the page is shared */
#define is_alone(elem) (((elem)->next)==(elem))

#endif  /* VM_VM_H */