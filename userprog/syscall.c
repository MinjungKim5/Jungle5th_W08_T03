#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/palloc.h"
#include <string.h>

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

bool check_address (void *ptr);
tid_t fork (const char *thread_name, struct intr_frame *f);
tid_t exec (const char *cmd_line);
bool create (const char *file, uint64_t initial_size);
bool remove (const char *file);
int open (const char *file);
int filesize (int fd);
off_t read (int fd, void *buffer, unsigned size);
off_t write (int fd, const void *buffer, unsigned size);
void seek (int fd, unsigned position);
unsigned tell (int fd);\
void close (int fd);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

struct lock filesys_lock;

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

	lock_init(&filesys_lock);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.

	int syscall_type = f->R.rax;
	switch (syscall_type)
	{
	case SYS_HALT:
		power_off();
		break;
	
	case SYS_EXIT:
		exit(f->R.rdi);
		break;

	case SYS_FORK:
		f->R.rax = fork(f->R.rdi, f);
		break;
	
	case SYS_EXEC:
		f->R.rax = exec(f->R.rdi);
		break;

	case SYS_WAIT:
		f->R.rax = process_wait (f->R.rdi);
		break;

	case SYS_CREATE:
		// printf("create %s\n", f->R.rdi);
		f->R.rax = create(f->R.rdi, f->R.rsi);
		break;

	case SYS_REMOVE:
		f->R.rax = remove(f->R.rdi);
		break;

	case SYS_OPEN:
		// printf("open %s\n", f->R.rdi);
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
		// printf("fd is %d\n", f->R.rdi);
		// printf("buffer: %s\n", f->R.rsi);
		// printf("size is %d", f->R.rdx);
		break;

	case SYS_SEEK:
		seek (f->R.rdi, f->R.rsi);
		break;

	case SYS_CLOSE:
		close (f->R.rdi);
		break;

	default:
		break;
	}
	
	// printf ("system call!\n");
	// thread_exit ();
}

bool check_address (void *addr)
{
	if (addr == NULL) {
		exit(-1);
		return false;
	}

	if (is_kernel_vaddr(addr)) {
		// printf("invalid address!!!!!\n");
		exit(-1);
		return false;
	}

	return true;
}

void exit(status)
{
	struct thread *cur = thread_current();
	cur->exit_status = status;
	printf("%s: exit(%d)\n", cur->name, cur->exit_status);
	thread_exit();
}

tid_t fork (const char *thread_name, struct intr_frame *f)
{
	return process_fork(thread_name, f);
}

tid_t exec (const char *cmd_line)
{
	check_address(cmd_line);

	char *line_cpy = palloc_get_page(0);
	if (line_cpy == NULL) exit(-1);

	strlcpy(line_cpy, cmd_line, PGSIZE);
	int succ = process_exec(line_cpy);
	if (succ == -1) exit(-1);

	return succ;
}

bool create (const char *file, uint64_t initial_size)
{
	int valid = check_address(file);
	// printf("valid address %d\n", valid);
	lock_acquire(&filesys_lock);
	bool succ = filesys_create(file, initial_size);
	lock_release(&filesys_lock);
	return succ;
}

bool remove (const char *file)
{
	check_address(file);
	lock_acquire(&filesys_lock);
	bool succ = filesys_remove(file);
	lock_release(&filesys_lock);
	return succ;
}

int open (const char *file)
{
	check_address(file);
	struct file *f_open = NULL;
	// lock_acquire(&filesys_lock);

	f_open = filesys_open(file);
	if (f_open ==NULL) {
		return -1;
	}
	int fd = set_fd(f_open);
	if (fd == -1) file_close(f_open);
	// lock_release(&filesys_lock);

	// printf("open in fd %d\n", fd);
	return fd;
}

int filesize (int fd)
{
	struct file *file = get_file(fd);
	if (file == NULL) return -1;
	return file_length(file);
}

off_t read(int fd, void *buffer, unsigned size)
{
	check_address(buffer);
	int readbytes = -1;
	char *buf = buffer;
	lock_acquire(&filesys_lock);
	struct file *file = get_file(fd);
	if (fd == 0) {
		readbytes = 0;
		while (readbytes <= size) {
			*buf = input_getc();
			buf++;
			readbytes++;
		}
	}
	else if ((1 < fd < FD_MAX) && file) {
		readbytes = file_read(file, buffer, size);
	}

	lock_release(&filesys_lock);
	return readbytes;
}

off_t write (int fd, const void *buffer, unsigned size)
{
	check_address(buffer);
	int writtenbytes = -1;
	lock_acquire(&filesys_lock);
	struct file *file = get_file(fd);
	if (fd == 1) {
		putbuf(buffer, size);
		writtenbytes = size;
	}
	else if ((1 < fd < FD_MAX) && file)
	{
		writtenbytes = file_write(file, buffer, size);
	}
	
	lock_release(&filesys_lock);
	return writtenbytes;
}

void seek (int fd, unsigned position)
{
	struct file *file = get_file(fd);
	if (file == NULL) return;
	file_seek(file, position);
}

unsigned tell (int fd)
{
	struct file *file = get_file(fd);
	if (file == NULL) return;
	return file_tell(file);
}

void close (int fd)
{
	struct file *file = get_file(fd);
	if (file == NULL) return;
	file_close(file);
	close_file(fd);
}

