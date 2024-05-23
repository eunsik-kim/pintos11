#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);
bool process_init_fdt(struct thread *t);
bool process_duplicate_fdt(struct thread *parent, struct thread *child);
bool process_delete_fdt(struct thread *t);

#endif /* userprog/process.h */

struct lazy_load_segment_aux{
    struct file *file;
    off_t ofs;
    size_t page_read_bytes; 
    size_t page_zero_bytes;   
};