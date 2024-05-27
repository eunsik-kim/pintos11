#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);
void check_address(void *addr);
void syscall_entry(void);
extern void *stdin_ptr;
extern void *stdout_ptr;
extern void *stderr_ptr;

#define MAX_FETY 126 // 126
#define checkdir(ptr) ((uint64_t)(ptr) & 1)
#define checklink(ptr) ((uint64_t)(ptr) & 2)
#define getptr(ptr)	 ((uint64_t)(ptr) & ~1)

#endif /* userprog/syscall.h */
