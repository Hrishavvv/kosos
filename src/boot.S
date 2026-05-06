.section .multiboot2,"a"
.align 8
header_start:
.long 0xE85250D6
.long 0
.long header_end - header_start
.long -(0xE85250D6 + 0 + (header_end - header_start))
.align 8
.short 5
.short 0
.long 20
.long 0
.long 0
.long 0

.align 8
.short 0
.short 0
.long 8
header_end:

.section .bss
.align 16
stack_bottom:
.skip 65536
stack_top:

.align 4096
pml4_table:
.skip 4096
pdp_table:
.skip 4096
pd_table0:
.skip 4096
pd_table1:
.skip 4096
pd_table2:
.skip 4096
pd_table3:
.skip 4096

.section .text
.code32
.global _start
.type _start, @function
_start:
    cli
    mov %ebx, multiboot_info_ptr
    movl $0, multiboot_info_ptr+4
    mov $stack_top, %esp

    call setup_paging
    call load_gdt
    call enable_long_mode

    ljmp $0x08, $long_mode_entry

setup_paging:
    mov $pml4_table, %edi
    xor %eax, %eax
    mov $4096*6/4, %ecx
    rep stosl

    mov $pml4_table, %eax
    mov $pdp_table, %ebx
    or $0x03, %ebx
    mov %ebx, (%eax)
    movl $0, 4(%eax)

    mov $pdp_table, %edi
    mov $pd_table0, %ebx
    or $0x03, %ebx
    mov %ebx, 0(%edi)
    movl $0, 4(%edi)
    mov $pd_table1, %ebx
    or $0x03, %ebx
    mov %ebx, 8(%edi)
    movl $0, 12(%edi)
    mov $pd_table2, %ebx
    or $0x03, %ebx
    mov %ebx, 16(%edi)
    movl $0, 20(%edi)
    mov $pd_table3, %ebx
    or $0x03, %ebx
    mov %ebx, 24(%edi)
    movl $0, 28(%edi)

    mov $pd_table0, %edi
    mov $0x00000000, %ebx
    call fill_pd
    mov $pd_table1, %edi
    mov $0x40000000, %ebx
    call fill_pd
    mov $pd_table2, %edi
    mov $0x80000000, %ebx
    call fill_pd
    mov $pd_table3, %edi
    mov $0xC0000000, %ebx
    call fill_pd
    ret

fill_pd:
    mov $512, %ecx
1:
    mov %ebx, %eax
    or $0x83, %eax
    mov %eax, (%edi)
    movl $0, 4(%edi)
    add $8, %edi
    add $0x200000, %ebx
    loop 1b
    ret

load_gdt:
    lgdt gdt64_descriptor
    ret

enable_long_mode:
    mov %cr4, %eax
    or $0x00000620, %eax
    mov %eax, %cr4

    mov $pml4_table, %eax
    mov %eax, %cr3

    mov $0xC0000080, %ecx
    rdmsr
    or $0x00000100, %eax
    wrmsr

    mov %cr0, %eax
    and $0xFFFFFFFB, %eax
    or $0x80000023, %eax
    mov %eax, %cr0
    ret

.align 16
long_mode_entry:
    .code64
    mov $stack_top, %rsp
    mov $stack_top, %rbp

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    mov multiboot_info_ptr(%rip), %rdi
    call kmain64
2:
    hlt
    jmp 2b

.code32

.section .data
multiboot_info_ptr:
.quad 0

.section .rodata
gdt64:
    .quad 0
    .quad 0x00AF9A000000FFFF
    .quad 0x00AF92000000FFFF
gdt64_end:
gdt64_descriptor:
    .word gdt64_end - gdt64 - 1
    .long gdt64

.section .note.GNU-stack,"",@progbits
