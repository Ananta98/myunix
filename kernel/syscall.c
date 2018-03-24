#include <console.h>
#include <cpu.h>
#include <isr.h>
#include <pit.h>
#include <process.h>
#include <vmm.h>
#include <pmm.h>

// 0 on success, size mapped on failure
static size_t map_userspace_to_kernel(uint32_t *pdir, uintptr_t ptr, uintptr_t kptr, size_t n) {
//	printf("map_userspace_to_kernel(0x%x, 0x%x, 0x%x, 0x%x)\n", (uintptr_t)pdir, ptr, kptr, n);
	for (uintptr_t i = 0; i < n; i++) {
		uintptr_t u_virtaddr = ptr + (i * BLOCK_SIZE);
		uintptr_t k_virtaddr = kptr + (i * BLOCK_SIZE);

		uint32_t *table = get_table(u_virtaddr, pdir);
		if (table == NULL) {
			printf(" table == NULL; returning early: %u\n", i);
			return i;
		}
		uintptr_t page = get_page(table, u_virtaddr);
//		printf(" page [0x%x]: 0x%x\n", u_virtaddr, page);
		if (! (page && PAGE_TABLE_PRESENT)) {
			printf(" not present; returning early: %u\n", i);
			return i;
		}
		if (! (page && PAGE_TABLE_READWRITE)) {
			printf(" not read-write; returning early: %u\n", i);
			return i;
		}
		if (! (page && PAGE_TABLE_USER)) {
			printf(" not user; returning early: %u\n", i);
			return i;
		}
//		printf(" page[0x%x] = 0x%x\n", k_virtaddr, page);
		map_page(get_table(k_virtaddr, kernel_directory), k_virtaddr, page, PAGE_TABLE_PRESENT);
	}
	return 0;
}

// 0 on success
static void unmap_from_kernel(uintptr_t kptr, size_t n) {
//	printf("unmap_from_kernel(0x%x, 0x%x)\n", kptr, n);
	for (uintptr_t i = 0; i < n; i++) {
		uintptr_t virtaddr = i * BLOCK_SIZE + kptr;
//		printf(" page[0x%x] = 0\n", virtaddr);
		map_page(get_table(virtaddr, kernel_directory), virtaddr, 0, 0);
	}
}

// only call this with interrupts off or prepare for FUN
__attribute__((used))
static size_t copy_from_userspace(uint32_t *pdir, uintptr_t ptr, size_t n) {
	uintptr_t kptr = find_vspace(kernel_directory, (n/BLOCK_SIZE)); // FIXME: handle overflow of pointer into extra block
	size_t v =  map_userspace_to_kernel(pdir, ptr, kptr, n);
	if (v != 0) {
		unmap_from_kernel(kptr, v);
		return -1;
	}

	unmap_from_kernel(kptr, v);
	return 0;
}

/*
static void syscall_putc(registers_t *regs) {
	putc((char)regs->ebx);
}

static void syscall_getc(registers_t *regs) {
	regs->ebx = (uint32_t)getc();
}
*/

/*
static void syscall_execve(registers_t *regs) {
//	printf("execve()\n");
	regs->eax = -1;
}

static void syscall_fork(registers_t *regs) {
//	printf("fork()\n");
	regs->eax = -1;
}

static void syscall_fstat(registers_t *regs) {
//	printf("fstat()\n");
	regs->eax = 0;
}

static void syscall_stat(registers_t *regs) {
//	printf("stat()\n");
	regs->eax = 0;
}

static void syscall_link(registers_t *regs) {
//	printf("link()\n");
	regs->eax = -1;
}

static void syscall_lseek(registers_t *regs) {
//	printf("lseek()\n");
	regs->eax = 0;
}
*/
static void syscall_times(registers_t *regs) {
//	printf("times()\n");
	regs->eax = -1;
}
/*
static void syscall_unlink(registers_t *regs) {
//	printf("unlink()\n");
	regs->eax = -1;
}
*/
/*
static void syscall_wait(registers_t *regs) {
//	printf("wait()\n");
	regs->eax = -1;
}
*/

static void syscall_gettimeofday(registers_t *regs) {
//	printf("gettimeofday()\n");
	regs->eax = -1;
}

static void syscall_getpid(registers_t *regs) {
//	printf("getpid()\n");
	regs->eax = (uint32_t)current_process->pid;
}

static void syscall_exit(registers_t *regs) {
	(void)regs;
//	printf("exit(%u)\n", regs->ebx);
	while (1) {
		switch_task();
	}
}

static void syscall_open(registers_t *regs) {
	// TODO: validate pointer
//	printf("open()\n");
	regs->eax = -1;
}

static void syscall_close(registers_t *regs) {
//	printf("close()\n");
	regs->eax = -1;
}

static void syscall_read(registers_t *regs) {
//	printf("read()\n");
	// regs->ebx int fd
	// regs->ecx char *buf
	// regs->edx int len
	// FIXME: validate pointers
	if (regs->ebx != 0) {
		regs->eax = -1;
		return;
	}

	uintptr_t ptr = regs->ecx;
	size_t n = regs->edx;
	size_t n_blocks = (n / BLOCK_SIZE) + 1;
	uint32_t *pdir = current_process->pdir;
	uintptr_t kptr = find_vspace(kernel_directory, n_blocks); // FIXME: handle overflow of pointer into next block
	uintptr_t kptr2 = kptr + (ptr & 0xFFFF);
	size_t v =  map_userspace_to_kernel(pdir, ptr, kptr, n_blocks);
	if (v != 0) {
		printf("v: %u\n", v);
		unmap_from_kernel(kptr, v);
//		return -1;
		return;
	}

	for (uintptr_t i = 0; i < n; i++) {
		*(char *)(kptr2 + i) = getc();
	}

	unmap_from_kernel(kptr, v);
	regs->eax = regs->edx;
}

static void syscall_write(registers_t *regs) {
//	printf("write(%u, 0x%x, 0x%x) (eip = 0x%x)\n", regs->ebx, regs->ecx, regs->edx, regs->eip);
	// regs->ebx int fd
	// regs->ecx char *buf
	// regs->edx int len
	// FIXME: validate pointers !!!!
	if (regs->ebx == 0) {
		regs->eax = 0;
		return;
	}
	if (regs->ebx != 1) {
		regs->eax = -1;
		return;
	}

	uintptr_t ptr = regs->ecx;
	size_t n = regs->edx;
	size_t n_blocks = (n / BLOCK_SIZE) + 1;
	uint32_t *pdir = current_process->pdir;
	uintptr_t kptr = find_vspace(kernel_directory, n_blocks); // FIXME: handle overflow of pointer into next block
	uintptr_t kptr2 = kptr + (ptr & 0xFFFF);
	size_t v =  map_userspace_to_kernel(pdir, ptr, kptr, n_blocks);
	if (v != 0) {
		printf("v: %u\n", v);
		unmap_from_kernel(kptr, v);
//		return -1;
		return;
	}

	for (uintptr_t i = 0; i < n; i++) {
		putc(*(char *)(kptr2 + i));
	}


	unmap_from_kernel(kptr, v);
	regs->eax = regs->edx;
//	return 0;
}

static void syscall_sleep(registers_t *regs) {
	unsigned long target = ticks + regs->ebx;
	while (ticks <= target) {
		switch_task();
	}
	regs->eax = 0;
}

static void syscall_dumpregs(registers_t *regs) {
	dump_regs(regs);
}

static void syscall_handler(registers_t *regs) {
	switch (regs->eax) {
/*		case 0x00:
			syscall_putc(regs);
			break;
*/		case 0x01:
			syscall_exit(regs);
			break;
		case 0x02:
			syscall_sleep(regs);
			break;
		case 0x03:
			syscall_read(regs);
			break;
		case 0x04:
			syscall_write(regs);
			break;
		case 0x05:
			syscall_open(regs);
			break;
		case 0x06:
			syscall_close(regs);
			break;
		case 0x14:
			syscall_getpid(regs);
			break;
		case 0x2b:
			syscall_times(regs);
			break;
		case 0x4e:
			syscall_gettimeofday(regs);
			break;
		case 0xFF:
			syscall_dumpregs(regs);
			break;
		default:
			break;
	}
}

void syscall_init() {
	isr_set_handler(0x80, syscall_handler);
}
