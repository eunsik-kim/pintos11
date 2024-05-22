#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H
#include "threads/thread.h"

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);
bool process_init_fdt(struct thread *t);
bool process_duplicate_fdt(struct thread *parent, struct thread *child);
bool process_delete_fdt(struct thread *t);

bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
						 uint32_t read_bytes, uint32_t zero_bytes,
						 uint64_t writable);

bool lazy_load_segment(struct page *page, void *aux);
struct lazy_load_data 
{	
	struct inode *inode;
	struct list *mmap_list;
	size_t ofs;
	size_t readb;
};


#endif /* userprog/process.h */
