#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "lib/user/syscall.h"
#include "lib/string.h"
#include "threads/malloc.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "filesys/inode.h"
#include "devices/disk.h"
#include "threads/palloc.h"
#include "userprog/process.h"
#include "vm/vm.h"
#include "filesys/directory.h"
#include "filesys/fat.h"

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081			/* Segment selector msr */
#define MSR_LSTAR 0xc0000082		/* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */
#define MAX_STDOUT (1 << 9)
#define MAX_FD (1 << 9)			/* PGSIZE / sizeof(struct file) */

struct func_params
{	
	int fd;
	int offset;
	struct fpage *find_page;
	struct file *file;
	struct file_entry *fety;
};

/* if access to filesys.c, should sync */
void *stdin_ptr;
void *stdout_ptr;
void *stderr_ptr;

typedef enum {FILE, FETY} file_type;
typedef enum {OPEN, CLOSE, DUP2} call_type;

/* system call */
pid_t fork(const char *thread_name);
int exec(const char *file);
int wait(pid_t);
bool create(const char *file, unsigned initial_size);
bool remove(const char *file);
int open(const char *file);
int filesize(int fd);
int read(int fd, void *buffer, unsigned size);
int write(int fd, const void *buffer, unsigned size);
void seek(int fd, unsigned position);
unsigned tell(int fd);
void close(int fd);
int process_exec(void *f_name);
void syscall_handler(struct intr_frame *);
struct thread *find_child(pid_t pid, struct list *fork_list);
int dup2(int oldfd, int newfd);
bool open_fety_fdt_in_page(struct func_params *params, struct thread *t);
bool open_fdt_in_page(struct func_params *params, struct thread *t);
bool delete_fety_fdt_in_page(struct func_params *params, struct thread *t);
struct fpage *add_page_to_list(struct list_elem *elem, struct list *ls);
bool find_file_in_page(struct func_params *params, struct list *ls);
void update_offset(struct fpage *table, int i, call_type type);
void write_to_read_page(void *uaddr);
void *mmap(void *addr, size_t length, int writable, int fd, off_t offset);
void munmap(void *addr);
bool chdir (const char *dir);
bool mkdir (const char *dir);
bool readdir (int fd, char name[READDIR_MAX_LEN + 1]);
bool isdir (int fd);
int inumber (int fd);
int symlink (const char* target, const char* linkpath);
void syscall_init(void)
{
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 |
							((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			  FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
	stdin_ptr = (0x123456780);		// no mean, just for fun
	stdout_ptr = stdin_ptr + 8;
	stderr_ptr = stdin_ptr + 16;
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f)
{	
	int syscall = f->R.rax;
	thread_current()->last_rsp = f->rsp;		
	switch (syscall)
	{
		case SYS_HALT:
			halt();
			break;
		case SYS_EXIT:
			exit(f->R.rdi);
			break;
		case SYS_FORK:
			f->R.rax = fork(f->R.rdi);
			break;
		case SYS_EXEC:
			f->R.rax = exec(f->R.rdi);
			break;
		case SYS_WAIT:
			f->R.rax = wait(f->R.rdi);
			break;
		case SYS_CREATE:
			f->R.rax = create(f->R.rdi, f->R.rsi);
			break;
		case SYS_REMOVE:
			f->R.rax = remove(f->R.rdi);
			break;
		case SYS_OPEN:
			f->R.rax = open(f->R.rdi);
			break;
		case SYS_FILESIZE:
			f->R.rax = filesize(f->R.rdi);
			break;
		case SYS_READ:
			f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		case SYS_WRITE:
			f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		case SYS_SEEK:
			seek(f->R.rdi, f->R.rsi);
			break;
		case SYS_TELL:
			f->R.rax = tell(f->R.rdi);
			break;
		case SYS_CLOSE:
			close(f->R.rdi);
			break;
		case SYS_MMAP:
			f->R.rax = mmap(f->R.rdi, f->R.rsi, f->R.rdx, f->R.r10, f->R.r8);
			break;
		case SYS_MUNMAP:
			munmap(f->R.rdi);
			break;
		case SYS_CHDIR:
			f->R.rax = chdir(f->R.rdi);
			break;
		case SYS_MKDIR:
			f->R.rax = mkdir(f->R.rdi);
			break;
		case SYS_READDIR:
			f->R.rax = readdir(f->R.rdi, f->R.rsi);
			break;
		case SYS_ISDIR:
			f->R.rax = isdir(f->R.rdi);
			break;
		case SYS_INUMBER:
			f->R.rax = inumber(f->R.rdi);
			break;
		case SYS_SYMLINK:
			f->R.rax = symlink(f->R.rdi, f->R.rsi);
			break;
		case SYS_DUP2:
			f->R.rax = dup2(f->R.rdi, f->R.rsi);
			break;
		default:
			printf("We don't implemented yet.");
			break;
	}
	thread_current()->last_rsp = NULL;
}

/*
 * 요청된 user 가상주소값이 1.NULL이 아닌지 2. kernel영역을 참조하는지
 * 3. 물리주소내에 mapping하는지 확인하여 위반하는경우, 4. lazy load인경우 제외하고 종료
 */
void check_address(void *uaddr)
{
	struct thread *cur = thread_current();
	if (uaddr == NULL || is_kernel_vaddr(uaddr) || (pml4_get_page(cur->pml4, uaddr) == NULL)) {
		if (!vm_claim_page(uaddr))	// lazy load한 page 중 fake uaddr인 경우가 존재
			exit(-1);
	}
}

/* 읽기전용 페이지에 쓰려고 하는경우 exit 호출 */
void write_to_read_page(void *uaddr)
{	
#ifdef VM	
	struct page *find_page = spt_find_page(&thread_current()->spt, uaddr);
	if (find_page && !(find_page->type & VM_WRITABLE))
		exit(-1);
#endif
}

/* list를 순회하며 pid를 가지는 thread return, 못찾으면 NULL return */
struct thread *find_child(pid_t pid, struct list *fork_list)
{	
	struct thread *child;
	struct list_elem *start_elem = list_head(fork_list);
	while ((start_elem = list_next(start_elem)) != list_tail(fork_list)) {
		child = list_entry(start_elem, struct thread, fork_elem);
		if (child->tid == pid) 
			return child;
	}
	return NULL;
}

/* 
 * 모든 page를 traverse하며, table arr에서 file을 찾으면 true return, 못찾으면 false return 
 * params에 찾은 file과 fdt_page, offset에 저장, 입력시 fd에 +1해야 됨
 */ 
bool find_file_in_page(struct func_params *params, struct list *ls)
{	
	struct fpage *table;
	struct list_elem *start_elem = list_head(ls);
	while ((start_elem = list_next(start_elem)) != list_tail(ls)) {
		table = list_entry(start_elem, struct fpage, elem);
		for (int i = table->s_elem; i < table->e_elem; i++) 
			if (table->d.fdt[i].fd == params->fd){
				params->file = table->d.fdt[i].fety->file;
				params->find_page = table;
				params->offset = i;
				return true;
			}
	}
	return false;
}

/* list에 빈 page가 없는경우 사용, page를 ls에 추가한 뒤 0으로 초기화*/
struct fpage *add_page_to_list(struct list_elem *elem, struct list *ls) {
	struct fpage *newpage = list_entry(elem, struct fpage, elem);
	if (elem == list_tail(ls)){	 
		if ((newpage = palloc_get_page(PAL_ZERO)) == NULL) 
			return NULL;
		list_push_back(ls, &newpage->elem);
	} 	
	return newpage;
}

/* page search를 빠르게 하기 위해 offset update하는 함수 */
void update_offset(struct fpage *table, int i, call_type type)
{
	if (type == OPEN) {
		table->s_ety = (i < MAX_FETY) ? i+1 : i;
		table->s_elem = (i < table->s_elem) ? i : table->s_elem;
		table->e_elem = (i == table->e_elem) && (i < MAX_FETY) ? i + 1 : table->e_elem;	
	} else if (type == CLOSE) {
		table->s_ety = (i < table->s_ety) ? i : table->s_ety;
		table->s_elem =  (i == table->s_elem) && (i < MAX_FETY) ? i + 1 : table->s_elem;
		table->e_elem = (i + 1 == table->e_elem) ? i : table->e_elem;
	}
}

/* 
 * fet_list를 순회하면서 file_entry 추가와 open_fdt_in_page호출 후 성공여부 return
 * 입력시 params에 file 저장, 출력시 fd 저장 
 */
bool open_fety_fdt_in_page(struct func_params *params, struct thread *t)
{
	struct fdt *new_fdt = NULL;
	struct file_entry *new_fety = NULL;
	struct fpage *fet_table, *fdt_table;
	struct list_elem *start_elem = list_head(&t->fet_list);
	while (1) {	
		start_elem = list_next(start_elem);
		if ((fet_table = add_page_to_list(start_elem, &t->fet_list)) == NULL)
			return false;

		// make fety first
		for (int i = fet_table->s_ety; i <= fet_table->e_elem; i++) {
			new_fety = &fet_table->d.fet[i];
			if (new_fety->file == NULL) {
				new_fety->file = params->file;
				new_fety->refc++;
				update_offset(fet_table, i, OPEN);
				break;
			}
		}
		if (new_fety->file == params->file)
			break;
	}

	// params->fd = find_lowest_fd();  // fdt_list 전체 순화하면서 가장 낮은 fd 찾는 함수(미구현)
	params->fety = new_fety;
	if (!open_fdt_in_page(params, t))
		return false;
	return true;
}	

/*
 * fdt_list를 순회하면서 fdt 추가 성공여부 return 
 * 입력시 params에 fety(fd 선택) 저장, 출력시 fd 저장 
 */
bool open_fdt_in_page(struct func_params *params, struct thread *t)
{
	int new_fd = 0;
	struct fpage *fdt_table;
	struct fdt *new_fdt = NULL;
	struct list_elem *start_elem = list_head(&t->fdt_list);
	while (1) {
		start_elem = list_next(start_elem);
		if ((fdt_table = add_page_to_list(start_elem, &t->fdt_list)) == NULL)
			return false;

		// make fdt and connect to fety
		for (int i = fdt_table->s_ety; i <= fdt_table->e_elem; i++) {
			new_fdt = &fdt_table->d.fdt[i];
			if (new_fdt->fety == NULL) {
				new_fdt->fety = params->fety;
				new_fdt->fd = (params->fd != 0) ? params->fd : new_fd + i + 1;
				params->fd = new_fd + i;
				update_offset(fdt_table, i, OPEN);
				return true;
			}
		}
		new_fd += MAX_FETY;
	}
	NOT_REACHED();
}

/* 
 * fdt_list를 순회하면서 file과 file_entry, fdt 삭제 시도 후 성공여부 return 
 * 입력시 params에 fd+1, 출력시 find_page, offset 저장 (페이지 삭제는 미구현)
 */
bool delete_fety_fdt_in_page(struct func_params *params, struct thread *t){
	struct fdt *new_fdt;
	struct file_entry *new_fety;
	struct fpage *fdt_table, *fet_table;
	struct list_elem *start_elem = list_head(&t->fdt_list);

	while ((start_elem = list_next(start_elem)) != list_tail(&t->fdt_list)) {
		fdt_table = list_entry(start_elem, struct fpage, elem);
		for (int i = fdt_table->s_elem; i < fdt_table->e_elem; i++){
			new_fdt = &fdt_table->d.fdt[i];
			if (new_fdt->fd == params->fd) {
				new_fety = new_fdt->fety;

				// no reference to fety, then delete fety
				if (--new_fety->refc == 0){	
					if (is_user_vaddr(new_fety->file)) continue;
					if (checkdir(new_fety->file)) {
						cwd_cnt_down(getptr(new_fety->file));
						dir_close(getptr(new_fety->file));
					} else
						file_close(new_fety->file);
					new_fety->file = NULL;
					fet_table = pg_round_down(new_fety);
					update_offset(fet_table, i, CLOSE);
				}

				// delete fdt
				new_fdt->fety = NULL;
				new_fdt->fd = 0;

				params->find_page = fdt_table;
				params->offset = i;
				update_offset(fdt_table, i, CLOSE);
				return true;
			}
		}
	}
	return false;	
}

/* power_off로 kenel process(qemu)종료 */
void halt()
{
	power_off();
}

/* kernel이 종료한 경우를 제외하고 종료 메시지 출력 후 thread_exit() */
void exit(int status)
{
	struct thread *curr = thread_current();
	curr->exit_status = status;
	if (strcmp(curr->name, "main"))
		printf("%s: exit(%d)\n", curr->name, status);
	thread_exit();
}

/* parent process의 pml4, intr_frame, fd copy 후 return tid, 실패 시 return TID_ERROR */
pid_t fork(const char *thread_name)
{	
	check_address(thread_name);
	pid_t tid = process_fork(thread_name);
	if (tid == TID_ERROR)
		return TID_ERROR;

	sema_down(&thread_current()->fork_sema);		// 자식 fork완료전 대기
	if (find_child(tid, &thread_current()->fork_list) == NULL)	// __do_fork 실패한 경우
		return TID_ERROR;
	return tid;
}

/* 
 * file name을 parsing하여 해당 code를 적재한 intr_frame, pml4를 만들고 해당 process로 변경 
 * process 생성에 실패한 경우만 exit(-1) 실행, 반환값 없음
 */
int exec(const char *file)
{	
	check_address(file);
	char *fn_copy = palloc_get_page(0);
	if (fn_copy == NULL)
		exit(-1);
	strlcpy(fn_copy, file, PGSIZE);

	int exec_status;
	if ((exec_status = process_exec(fn_copy)) == -1)
		exit(-1);
	NOT_REACHED();
	return -1;
}

/* 
 * fork한 pid를 대기, fork한 process 존재하지 않는 경우 -1 return
 * fork한 process의 exit의 status return
 */
int wait(pid_t pid) 
{
	struct thread *child, *parent = thread_current();
	if ((child = find_child(pid, &parent->fork_list)) == NULL)
		return -1;

	sema_down(&child->wait_sema);
	list_remove(&child->fork_elem);
	child->fork_elem.prev = NULL;		
	int temp = child->exit_status;		// receive status, preemption
	sema_up(&child->fork_sema);
	return temp;
}

/* initial_size의 file을 생성 후 성공여부 return, 이미 존재하거나 메모리 부족 시 fail */
bool create(const char *file, unsigned initial_size)
{	
	check_address(file);
	return filesys_create(file, initial_size);
}

/* file을 삭제 후 성공여부 return, file이 없거나 inode 생성에 실패시 fail 
 dirtory가 비어 있지 않으면 삭제 실패, root directory인경우 삭제 실패,
 현재 process나 다른 process의 cwd는 삭제 불가능 */
bool remove(const char *file)
{
	check_address(file);
	return filesys_remove(file);
}

/*
 * 잘못된 파일 이름을 가지거나 disk에 파일이 없는 경우 -1 반환.
 * file_entry를 fet_list에서 없는 경우 (메모리 부족한 경우 page 추가 하여) fety 생성.
 * thread 내에 file_entry와 fdt를 만들어 fd 값을 저장한 뒤, fd 값을 반환.
 * directory도 열 수 있음
 */
int open(const char *file)
{
	check_address(file);
	struct file *file_entity = filesys_open(file);
	if (file_entity == NULL) 	// wrong file name or oom or not in disk (initialized from arg -p)
		return -1;
	
	if (checkdir(file_entity))
		cwd_cnt_up(getptr(file_entity));

	// find file_entry
	struct func_params params;
	params.file = file_entity;
	params.fd = 0;
	if (!open_fety_fdt_in_page(&params, thread_current())) {
		free(file_entity);
		return -1;
	}
	return params.fd;
}

/* fd에 해당하는 file의 크기 return */
int filesize(int fd)
{
	struct func_params params;
	params.fd = fd + 1;
	if (!find_file_in_page(&params, &thread_current()->fdt_list))
		return -1;

	struct file *cur_file = getptr(params.file);
	if (is_user_vaddr(cur_file)) 
		return -1;

	return file_length(cur_file);
}

/* fd값에 따라 읽은 만큼 byte(<=length)값 반환, 못 읽는 경우 -1, 읽을 지점이 파일의 끝인경우 0 반환 
 dir를 읽으려고 하는경우 -1 return */
int read(int fd, void *buffer, unsigned size)
{
	struct func_params params;
	params.fd = fd + 1;
	if (!find_file_in_page(&params, &thread_current()->fdt_list))
		return -1;

	struct file *cur_file = params.file;
	if (cur_file != stdin_ptr && is_user_vaddr(cur_file))  // wrong fd
		return -1; 

	if (size == 0) // not read
		return 0; 

	if (checkdir(cur_file)) 	// cannot read dir
		return -1;

	check_address(buffer);
	write_to_read_page(buffer);	
	int bytes_read = size;
	if (cur_file == stdin_ptr)
	{
		uint8_t byte;
		while (size--) {
			byte = input_getc();	  // console 입력을 받아
			*(char *)buffer++ = byte; // 1byte씩 저장
		}
	} else {
		if (cur_file->pos == inode_length(cur_file->inode)) // end of file
			return 0;

		if ((bytes_read = file_read(cur_file, buffer, size)) == 0)  // could not read
			return -1;
	}
	return bytes_read;
}

/* fd값에 따라 적은 만큼 byte(<=length)값 반환, 못 적는 경우 -1 반환, dir를 쓰려고 하는경우 -1 return */
int write(int fd, const void *buffer, unsigned size)
{
	struct func_params params;
	params.fd = fd + 1;
	if (!find_file_in_page(&params, &thread_current()->fdt_list))
		return -1;
	struct file *cur_file = params.file;
	
	if (cur_file == NULL || cur_file == stdin_ptr)  // no bytes could be written at all
		return 0;
	
	if (checkdir(cur_file)) 	// cannot write dir
		return -1;

	check_address(buffer);
	int bytes_write = size;
	if (cur_file == stdout_ptr) // stdout: lock을 걸고 buffer 전체를 입력
	{
		int iter_cnt = size / MAX_STDOUT + 1;
		int less_size;
		while (iter_cnt--) { // 입력 buffer가 512보다 큰경우 slicing 해서 출력 
			less_size = (size > MAX_STDOUT) ? MAX_STDOUT : size;
			putbuf(buffer, less_size);
			buffer += less_size;
			size -= MAX_STDOUT;
		}
	} else if (cur_file == stderr_ptr) // stderr: (stdout과 다르게 어떻게 해야할지 모르겠음)한글자씩 작성할때마다 lock이 걸림
		while (size-- > 0)
			putchar(buffer++);

	else{	// file growth is not implemented by the basic file system
		bytes_write = file_write(cur_file, buffer, size);
	}  
	return bytes_write;
}

/* 
 * 현재 파일의 읽는 pos를 변경
 * 참고 (inode size < position인 경우 write할 때 자동으로 0으로 채워지는지 확인) 
 */
void seek(int fd, unsigned position)
{
	struct func_params params;
	params.fd = fd + 1;
	if (!find_file_in_page(&params, &thread_current()->fdt_list))
		return -1;

	struct file *cur_file = getptr(params.file);
	if (is_user_vaddr(cur_file)) return;
	file_seek(cur_file, position);
}

/* 현재 파일을 읽는 위치 return */
unsigned tell(int fd)
{
	struct func_params params;
	params.fd = fd + 1;
	if (!find_file_in_page(&params, &thread_current()->fdt_list))
		return -1;

	struct file *cur_file = getptr(params.file);
	if (is_user_vaddr(cur_file)) 
		return -1;

	return file_tell(cur_file);
}

/* fd에 해당하는 file를 close, file_entry를 NULL로 초기화 */
void close(int fd)
{
	struct func_params params;
	params.fd = fd + 1;
	delete_fety_fdt_in_page(&params, thread_current());
}

/* 
 * newfd가 가리키는 file을 닫고 oldfd의 fety을 newfd의 fety가 가리키도록 바꿈
 * oldfd가 없는 경우 return -1, 성공하면 newfd return
 */
int dup2(int oldfd, int newfd) 
{	
	struct func_params params;
	struct thread *t = thread_current();
	params.fd = oldfd + 1;
	if (!find_file_in_page(&params, &t->fdt_list))
		return -1;

	if (oldfd == newfd)
		return newfd;

	struct file_entry *new_fety = params.find_page->d.fdt[params.offset].fety;
	new_fety->refc++;
	params.fd = newfd + 1;
	// newfd가 원래 존재하는 경우
	if (delete_fety_fdt_in_page(&params, t)){ 
		params.find_page->d.fdt[params.offset].fety = new_fety;
		params.find_page->d.fdt[params.offset].fd = newfd + 1;
	} else { // newfd에 해당하는 fdt를 새롭게 생성 	
		params.fety = new_fety;
		if (!open_fdt_in_page(&params, t))
			return -1;
	}		
	return newfd;
}

/* fd로 열린 파일의 오프셋(offset) 바이트부터 length 바이트 만큼을 
프로세스의 가상주소공간의 주소 addr 에 매핑 하여 주소를 반환, 실패시 -1반환
lazy load, 메모리 중복 검사 및 다수의 예외 처리 포함 */
void *mmap(void *addr, size_t length, int writable, int fd, off_t offset)
{		
	// 불가능 예외처리...
	if ((addr == NULL) || ((uint64_t)addr % PGSIZE != 0) || (length == 0) 
		|| (offset % PGSIZE != 0) || is_kernel_vaddr(addr) 
			|| (is_kernel_vaddr(length)) || is_kernel_vaddr(addr + length)) 
		return NULL;

	struct func_params params;
	params.fd = fd + 1;
	if (!find_file_in_page(&params, &thread_current()->fdt_list))
		return NULL;

	struct file *cur_file = params.file;
	uint64_t file_len = file_length(cur_file);
	if (is_user_vaddr(cur_file) || (file_len == 0))	
		return NULL;

	writable = (writable) ? 1 : 0;
	cur_file = (uint64_t)cur_file | 1;	// packing to verify mmap call
	length = offset + length > file_len ? file_len : length;	// 파일 최대 길이 까지만 읽기
	size_t zerob = (length % PGSIZE) ? PGSIZE - length % PGSIZE : 0;	
	if (!load_segment(cur_file, offset, addr, length, zerob, writable))
		return NULL;

	return addr;
}

/* 지정된 주소 범위 addr에 대한 매핑을 해제, mmap에 의해 연결된 page들을 unmap*/
void munmap(void *addr)
{	
	struct thread *cur = thread_current();
	struct page *mpage = spt_find_page(&cur->spt, addr);
	if (!mpage || !(mpage->type & VM_MMAP))
		return;

	int pg_cnt = mpage->file.data->pg_cnt;
	while(pg_cnt--) {
		spt_remove_page(&cur->spt, mpage);
		addr += PGSIZE;
		mpage = spt_find_page(&cur->spt, addr);
	}
}

/* directory path가 유효한지 검증하며 cwd를 변경 */
bool chdir (const char *dir) {
	struct thread* cur = thread_current();
	char *file_name = malloc(NAME_MAX + 1);
	if (!file_name)
		return NULL;
		
	struct dir *tar_dir;
	struct inode *inode = NULL;
	if (!(tar_dir = find_dir(dir, file_name))){	// find dir last before 
		free(file_name);
		return false;
	}
	
	if (!dir_lookup(tar_dir, file_name, &inode)) {	// open last dir inode
		dir_close(tar_dir);
		free(file_name);
		return false;
	}
	dir_close(tar_dir);
	free(file_name);

	if (!inode->data.isdir)	// wrong path
		return false;

	if (!(tar_dir = dir_open(inode))) 
		return false;
	
	cwd_cnt_down(cur->cwd);
	dir_close(cur->cwd);
	cur->cwd = tar_dir;
	cwd_cnt_up(tar_dir);
	return true;
}

/* directory 생성, directory 존재 유무와 생성 성공유무에 따라서 return */
bool mkdir (const char *dir) {
	char *file_name = malloc(NAME_MAX + 1);
	if (!file_name)
		return false;

	struct dir *tar_dir;
	struct inode *inode = NULL;
	if (!(tar_dir = find_dir(dir, file_name))){ // find dir before last 
		free(file_name);
		return false;
	}		
	
	// make space to place new directory
	cluster_t clst;
	if (!(clst = fat_create_chain(0))) {
		free(file_name);
		return false;
	}
	disk_sector_t inode_sector = cluster_to_sector(clst);

	// create directory 	
	if (!dir_create(inode_sector, 16, dir_get_inode(tar_dir)->sector)) {
		dir_close(tar_dir);
		free(file_name);
		return false;
	}

	if(!dir_add(tar_dir, file_name, inode_sector)) {	// include checking same file_name
		dir_close(tar_dir);
		free(file_name);
		return false;
	}
	free(file_name);
	dir_close(tar_dir);
	return true;
}

/* directory에서 entry를 읽음, 성공하면 name에 잘 저장
 .과 ..은 반환 안되고, null로 끝나게 해야됨, 
 변경이 안된다면 한번씩 읽도록 작성(동기화 x) */
bool readdir (int fd, char name[READDIR_MAX_LEN + 1]) {
	struct func_params params;
	params.fd = fd + 1;
	if (!find_file_in_page(&params, &thread_current()->fdt_list))
		return false;

	if (is_user_vaddr(params.file)) 
		return false;
	
	struct dir *dir = getptr(params.file); 
	return dir_readdir(dir, name);
}

/* fd가 dir인지 check해서 return */
bool isdir (int fd) {
	struct func_params params;
	params.fd = fd + 1;
	if (!find_file_in_page(&params, &thread_current()->fdt_list))
		return false;

	if (is_user_vaddr(params.file)) 
		return false;
	
	return checkdir(params.file);
}

/* inode num을 return 해야되는데 그냥 귀찮으니깐 inode의 sector num return */
int inumber (int fd) {
	struct func_params params;
	params.fd = fd + 1;
	if (!find_file_in_page(&params, &thread_current()->fdt_list))
		return -1;

	if (is_user_vaddr(params.file)) 
		return -1;
	
	struct file *cur_file = getptr(params.file);  // include directory
	return cur_file->inode->sector;
}

/* linkpath를 통해 target으로 이어지는 symbolic link 파일 만들기
 target의 inode 정보를 기록하여 read나 write때 load하여 사용
 성공하면 0, 실패하면 -1 return */
int symlink (const char* target, const char* linkpath) {

	// find the cluster to place sybolic file
	if (!create(linkpath, DISK_SECTOR_SIZE))
		return -1;

	struct file *file_entity = filesys_open(linkpath);
	struct inode_disk *disk_inode = &file_entity->inode->data;

	// init symlink 
	disk_inode->isdir = 2;	// link flg
	disk_write(filesys_disk, file_entity->inode->sector, disk_inode);
	
	// record file name
	char buf[512];
	strlcpy(buf, target, sizeof(target) + 1);
	disk_write(filesys_disk, disk_inode->start, buf);
	file_close(file_entity);
	return 0;
}