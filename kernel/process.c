#include <assert.h>
#include <stdint.h>
#include <stddef.h>

#include <console.h>
#include <gdt.h>
#include <heap.h>
#include <idt.h>
#include <list.h>
#include <pmm.h>
#include <process.h>
#include <string.h>
#include <task.h>
#include <vmm.h>

void __attribute__((noreturn)) __restore_process();

process_t *current_process;
node_t *current_process_node;
list_t *process_list;

extern void *isrs_start;
extern void *isrs_end;

process_t *kidle_init() {
	process_t *process = (process_t *)kcalloc(1, sizeof(process_t));
	process->kstack = 0;
	process->pdir = kernel_directory;
	process->pid = 0;
	process->fd_table = kcalloc(1, sizeof(fd_table_t));

	size_t stack_size_blocks = 16;
	uintptr_t v_stack = find_vspace(kernel_directory, stack_size_blocks + 1);
	for (size_t i = 0; i < stack_size_blocks; i++) {
		uintptr_t vaddr = v_stack + i*BLOCK_SIZE + BLOCK_SIZE;
		uintptr_t block = pmm_alloc_blocks_safe(1);
		map_page(get_table_alloc(vaddr, kernel_directory),  vaddr,
			block,
			PAGE_TABLE_PRESENT | PAGE_TABLE_READWRITE);
	}

	// guard
	map_page(get_table_alloc(v_stack, kernel_directory), v_stack, PAGE_VALUE_GUARD, 0);

	process->esp = v_stack + (BLOCK_SIZE * (stack_size_blocks + 1)) ;
	process->ebp = process->esp;
	process->eip = (uintptr_t)kidle;
	return process;
}

// TODO: ensure no additional information gets leaked (align and FILL a block)
process_t *process_init(uintptr_t start, uintptr_t end) {
	printf("process_init(0x%x, 0x%x)\n", start, end);
	assert(start % 0x100 == 0);

	uintptr_t virt_text_start = 0x1000000;
	uintptr_t virt_heap_start = 0x2000000; // max 16mb text

	process_t *process = (process_t *)kcalloc(1, sizeof(process_t));

	// kernel stack (16kb)
	// FIXME: use malloc when available
	uintptr_t kstack = (uintptr_t)pmm_alloc_blocks_safe(4);
	uintptr_t kstack_top = kstack + 4 * BLOCK_SIZE;
	printf("kernel_stack: 0x%x - 0x%x\n", kstack, kstack_top);
	map_pages((void *)kstack, (void *)kstack_top,
		PAGE_TABLE_PRESENT | PAGE_TABLE_READWRITE, "kstack");
	memset((void *)kstack, 0, 4*BLOCK_SIZE);
	process->kstack = kstack_top;
	process->fd_table = kcalloc(1, sizeof(fd_table_t));
	process->fd_table->entries[0] = &tty_node;
	process->fd_table->entries[1] = &tty_node;
	process->fd_table->entries[2] = &tty_node;

	// page directory
	process->pdir = (uint32_t *)pmm_alloc_blocks_safe(1);
	map_direct_kernel((uintptr_t)process->pdir);
	memset((void *)process->pdir, 0, BLOCK_SIZE);

	// isrs FIXME: size is hardcoded, might be smaller/bigger than 1 block
	for (uintptr_t i = 0; i < ((uintptr_t)&isrs_end - (uintptr_t)&isrs_start); i += BLOCK_SIZE) {
		uintptr_t addr = (uintptr_t)&isrs_start + i;
		map_page(get_table_alloc(addr, process->pdir), addr, addr,
			PAGE_TABLE_PRESENT);
	}

	// kstack FIXME: size is hardcoded
	for (uintptr_t i = 0; i < 4; i++) {
		uintptr_t addr = kstack + i*BLOCK_SIZE;
		map_page(get_table_alloc(addr, process->pdir), addr, addr,
			PAGE_TABLE_PRESENT | PAGE_TABLE_READWRITE);
	}

	// TSS
	// FIXME: might be bigger than 1 block
	map_page(get_table_alloc((uintptr_t)&tss, process->pdir),
		(uintptr_t)&tss,
		(uintptr_t)&tss,
		PAGE_TABLE_PRESENT);
	// GDT
	// FIXME: might be bigger than 1 block
	map_page(get_table_alloc((uintptr_t)gdt, process->pdir),
		(uintptr_t)gdt,
		(uintptr_t)gdt,
		PAGE_TABLE_PRESENT);
	// IDT
	// FIXME: might be bigger than 1 block
	map_page(get_table_alloc((uintptr_t)idt_entries, process->pdir),
		(uintptr_t)idt_entries,
		(uintptr_t)idt_entries,
		PAGE_TABLE_PRESENT);

	// allocate some userspace heap (16kb)
	// FIXME: size hardcoded
	for (unsigned int i = 0; i < 4; i++) {
		// allocate non-continous space
		uintptr_t virtaddr = virt_heap_start + i*BLOCK_SIZE;
		uintptr_t userspace_heap = (uintptr_t)pmm_alloc_blocks_safe(1);
		map_direct_kernel(userspace_heap);
		memset((void *)userspace_heap, 0, BLOCK_SIZE);
		map_page(get_table(userspace_heap, kernel_directory), userspace_heap,
			0, 0);
		map_page(get_table_alloc(virtaddr, process->pdir), virtaddr,
			userspace_heap,
			PAGE_TABLE_PRESENT | PAGE_TABLE_READWRITE | PAGE_TABLE_USER);
	}

	// map the .text section
	printf(".text: 0x%x - 0x%x => 0x%x - 0x%x\n",
		virt_text_start, virt_text_start + (end - start),
		start, end);
	for (uintptr_t i = 0; i < (end - start); i += BLOCK_SIZE) {
		uintptr_t physaddr = start + i;
		uintptr_t virtaddr = virt_text_start + i;
		map_page(get_table_alloc(virtaddr, process->pdir),
			virtaddr,
			physaddr,
			PAGE_TABLE_PRESENT | PAGE_TABLE_USER | PAGE_TABLE_READWRITE);
	}

	registers_t *regs = (registers_t *)(kstack_top - sizeof(registers_t));
	printf("regs: 0x%x\n", (uintptr_t)regs);
	regs->old_directory = (uint32_t)process->pdir;
	regs->gs = 0x23;
	regs->fs = 0x23;
	regs->es = 0x23;
	regs->ds = 0x23;
	regs->edi = 0;
	regs->esi = 0;
	regs->ebp = 0;
	regs->esp = 0;
	regs->ebx = 0;
	regs->ecx = 0;
	regs->eax = 0;
	regs->isr_num = 0;
	regs->err_code = 0;
	regs->eip = virt_text_start;
	regs->cs = 0x1B;
	regs->eflags = 0x200; // enable interrupts
	regs->usersp = virt_heap_start + BLOCK_SIZE;
	regs->ss = regs->ds;

	process->regs = regs;

	process->esp = (uint32_t)regs;
	process->ebp = process->esp;
	process->eip = (uintptr_t)return_to_regs;
	return process;
}

process_t *next_process() {
	assert(process_list != NULL);
	node_t *n = current_process_node->next;
	if (n == NULL) {
		n = process_list->head;
	}
	current_process_node = n;
	current_process = current_process_node->value;
	return current_process;
}

extern uintptr_t read_eip();

// switch to some new task and return when we get scheduled again
// TODO: rewrite this completely in asm
__attribute__((used))
void _switch_task() {
	if (current_process == NULL) {
		return;
	}

	uint32_t esp, ebp, eip;
	__asm__ __volatile__("mov %%esp, %0" : "=r" (esp));
	__asm__ __volatile__("mov %%ebp, %0" : "=r" (ebp));
	eip = read_eip();
	if (eip == 0) {
		return;
	}
	current_process->esp = esp;
	current_process->ebp = ebp;
	current_process->eip = eip;

	__switch_direct();
}

// DESTRUCTIVE: don't save anything and NEVER return
__attribute__((noreturn))
void __switch_direct(void) {
	current_process = next_process();
	__restore_process();
}

__attribute__((noreturn))
void __restore_process() {
	tss_set_kstack(current_process->kstack);

	uint32_t esp = current_process->esp;
	uint32_t ebp = current_process->ebp;
	uint32_t eip = current_process->eip;

	__asm__ __volatile__ (
		"mov %0, %%ebx\n"
		"mov %1, %%esp\n"
		"mov %2, %%ebp\n"
		// "mov %3, %%cr3\n"
		"mov $0, %%eax\n"
		"jmp *%%ebx"
		:
		: "r" (eip), "r" (esp), "r" (ebp) /*, "r"(current_dir)*/
		: "ebx", "esp", "eax");
#ifndef __TINYC__
	__builtin_unreachable();
#endif
}

void process_remove(process_t *p) {
	list_remove(process_list, p);
	if (current_process == p) {
		current_process_node = process_list->head;
		current_process = current_process_node->value;
	}
}

void process_add(process_t *p) {
	if (process_list == NULL) {
		process_list = list_init();
	}
	list_insert(process_list, p);
	return;
}

__attribute__((noreturn))
void process_enable(void) {
	__asm__ __volatile__ ("cli");
	current_process_node = process_list->head;
	current_process = current_process_node->value;
	assert(current_process != NULL);

	__restore_process();
}
