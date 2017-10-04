; extern void gdt_flush(uintptr_t)

global gdt_flush:function (gdt_flush.end - gdt_flush)
gdt_flush:
	mov eax, [esp + 4]
	lgdt [eax]
	mov eax, 0x10
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax
	mov ss, ax
	jmp 0x08:.flush
.flush:
	ret
.end:
