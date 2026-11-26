[bits 16]

section .text.start
global stage2_start

extern bios_main
extern __bss_start
extern __bss_end

MAP_BUFFER   equ 0xC000
MAP_CAPACITY equ 64
CODE_SEL     equ 0x18
DATA_SEL     equ 0x20
CODE64_SEL   equ 0x28
DATA64_SEL   equ 0x30
HHDM_OFFSET  equ 0xFFFF800000000000

stage2_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    cld
    mov [bios_drive], dl
    sti

    mov al, '2'
    call .putc

    ; Prepare E820 enumeration
    xor ax, ax
    mov es, ax
    mov di, MAP_BUFFER
    xor ebx, ebx
    xor bp, bp
    mov word [mmap_calls], 0

.e820_next:
    cmp bp, MAP_CAPACITY
    jae .mmap_error

    ; Also limit calls that return skipped entries
    cmp word [mmap_calls], 256
    jae .mmap_error
    inc word [mmap_calls]

    mov dword [es:di + 20], 1
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    stc
    int 0x15

    jc .mmap_finished

    cmp eax, 0x534D4150
    jne .mmap_error

    cmp ecx, 20
    jb .mmap_error
    je .check_length

    cmp ecx, 24
    jne .mmap_error

    test dword [es:di + 20], 1
    jz .e820_continue

.check_length:
    mov eax, [es:di + 8]
    or eax, [es:di + 12]
    jz .e820_continue

    inc bp
    add di, 24

.e820_continue:
    test ebx, ebx
    jnz .e820_next

.mmap_finished:
    test bp, bp
    jz .mmap_error

    mov [mmap_count], bp
    mov al, 'M'
    call .putc
    
    ; Check that the boot drive supports extended BIOS reads.
    mov dl, [bios_drive]
    mov ax, 0x4100
    mov bx, 0x55AA
    push ds
    int 0x13
    pop ds
    jc .payload_error
    cmp bx, 0xAA55
    jne .payload_error
    test cx, 1
    jz .payload_error

    ; Read the ELF slot into low memory.
    mov dl, [bios_drive]
    mov si, payload_dap
    mov ax, 0x4200
    push ds
    int 0x13
    pop ds
    jc .payload_error
    cmp word [payload_dap + 2], 64
    jne .payload_error

    mov al, 'D'
    call .putc
    jmp .a20_begin

.payload_error:
    mov al, '!'
    call .putc
    jmp .halt

.mmap_error:
    mov word [mmap_count], 0
    mov al, 'E'
    call .putc
    jmp .halt

.a20_begin:
    ; A20 might already be enabled
    call check_a20
    test ax, ax
    jnz .a20_ready

    ; Ask BIOS to enable it
    mov ax, 0x2401
    int 0x15

    ; Independently verify the result
    call check_a20
    test ax, ax
    jz .a20_error

.a20_ready:
    mov al, 'A'
    call .putc

    ; Enter protected mode
    cli
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; Load CS with our 32-bit code selector
    jmp dword CODE_SEL:protected_entry

.a20_error:
    mov al, '!'
    call .putc

.halt:
    cli
    hlt
    jmp .halt

.putc:
    push bx
    mov ah, 0x0E
    mov bh, 0
    mov bl, 0x07
    int 0x10
    pop bx
    ret


; Return AX = 1 when A20 is enabled, otherwise AX = 0.
; Temporarily test physical addresses 0x500 and 0x100500.
check_a20:
    pushf
    cli
    push ds
    push es
    push si
    push di
    push bx

    xor ax, ax
    mov ds, ax
    mov si, 0x0500

    mov ax, 0xFFFF
    mov es, ax
    mov di, 0x0510

    ; Save both original bytes
    mov bl, [ds:si]
    mov bh, [es:di]

    ; Check whether writing the high address changes the low address
    mov byte [ds:si], 0
    mov byte [es:di], 0xFF

    cmp byte [ds:si], 0xFF
    mov ax, 0
    je .restore

    mov ax, 1

.restore:
    ; Restore high first because the addresses might alias
    mov [es:di], bh
    mov [ds:si], bl

    pop bx
    pop di
    pop si
    pop es
    pop ds
    popf
    ret


[bits 32]
section .text

protected_entry:
    ; Install flat data segments
    mov ax, DATA_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Reserve 0x68000–0x6FFFF for our 32 KiB stack
    mov esp, 0x70000
    xor ebp, ebp
    cld

    ; Clear uninitialized C global/static storage
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb

    ; Align the stack and pass arguments using i386 cdecl
    ; bios_main(map_address, entry_count, boot_drive)
    sub esp, 4

    movzx eax, byte [bios_drive]
    push eax

    movzx eax, word [mmap_count]
    push eax

    push dword MAP_BUFFER

    call bios_main

    ; Stop safely if the C function unexpectedly returns
    jmp bios_halt


global bios_halt
bios_halt:
    cli
.halt:
    hlt
    jmp .halt


; void bios_outb(uint16_t port, uint8_t value)
global bios_outb
bios_outb:
    mov dx, [esp + 4]
    mov al, [esp + 8]
    out dx, al
    ret

; void bios_enter_long_mode(uint32_t pml4, uint64_t entry, uint64_t stack_top)
; Returns only if the CPU lacks a required feature.
global bios_enter_long_mode
bios_enter_long_mode:
    ; Check whether the EFLAGS ID bit can be changed (CPUID available).
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    xor eax, ecx
    test eax, 1 << 21
    jz .unsupported

    push ebx
    xor eax, eax
    cpuid
    cmp eax, 1
    jb .restore_ebx

    mov eax, 1
    cpuid
    and edx, (1 << 5) | (1 << 6)     ; MSR and PAE
    cmp edx, (1 << 5) | (1 << 6)
    jne .restore_ebx

    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .restore_ebx

    mov eax, 0x80000001
    cpuid
    and edx, (1 << 29) | (1 << 20)   ; Long mode and NX
    cmp edx, (1 << 29) | (1 << 20)
    jne .restore_ebx
    pop ebx

    cli
    mov eax, [esp + 16]
    or eax, [esp + 20]
    jz .enable_paging               ; Plain ELF probes use the legacy path
    mov al, 0xFF
    out 0x21, al
    out 0xA1, al

    mov eax, 1
    cpuid
    test edx, 1 << 16               ; PAT
    jz .enable_paging
    wbinvd
    mov ecx, 0x277                 ; IA32_PAT
    mov eax, 0x00070406             ; WB, WT, UC-, UC
    mov edx, 0x00000105             ; WP, WC, UC, UC
    wrmsr
    wbinvd

.enable_paging:
    mov eax, cr4
    and eax, ~(1 << 12)              ; Use four levels of paging
    or eax, 1 << 5                   ; CR4.PAE
    mov cr4, eax

    mov eax, [esp + 4]               ; Physical address of the PML4
    mov cr3, eax

    mov ecx, 0xC0000080              ; IA32_EFER
    rdmsr
    or eax, (1 << 8) | (1 << 11)     ; LME and NXE
    wrmsr

    mov eax, cr0
    or eax, (1 << 31) | (1 << 16)    ; Paging and write protection
    mov cr0, eax
    jmp CODE64_SEL:.entry64

.restore_ebx:
    pop ebx
.unsupported:
    ret

[bits 64]
.entry64:
    mov ax, DATA64_SEL
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov esp, esp                    ; Zero the upper half of RSP
    mov rax, [rsp + 8]               ; Full 64-bit ELF entry address
    mov r8, [rsp + 16]               ; Dedicated Limine stack, or zero
    test r8, r8
    jnz .limine_entry
    and rsp, -16
    sub rsp, 8
    mov qword [rsp], 0               ; Set function-entry stack alignment
    xor ebp, ebp
    cld
    jmp rax

.limine_entry:
    mov rsi, rax                    ; Keep entry across WRMSR
    mov ax, DATA64_SEL
    mov fs, ax
    mov gs, ax
    xor eax, eax
    xor edx, edx
    mov ecx, 0xC0000100             ; IA32_FS_BASE
    wrmsr
    inc ecx                        ; IA32_GS_BASE
    wrmsr

    mov r9, HHDM_OFFSET
    lea rdx, [rel .higher_half]
    add rdx, r9
    jmp rdx

.higher_half:
    ; Move the GDT to its direct-map address before removing low mappings.
    sub rsp, 16
    mov word [rsp], gdt_end - gdt_start - 1
    lea rdx, [rel gdt_start]
    mov [rsp + 2], rdx
    lgdt [rsp]

    mov rsp, r8
    push qword 0
    mov [rsp - 8], rsi

    mov rcx, cr3
    mov qword [r9 + rcx], 0        ; Drop the temporary identity map
    mov cr3, rcx

    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    cld
    jmp qword [rsp - 8]

[bits 32]


section .rodata
align 8

gdt_start:
    dq 0x0000000000000000   ; Null descriptor
    dq 0x00009A000000FFFF   ; Flat 16-bit code segment
    dq 0x000092000000FFFF   ; Flat writable 16-bit data segment
    dq 0x00CF9A000000FFFF   ; Flat 32-bit code segment
    dq 0x00CF92000000FFFF   ; Flat writable data segment
    dq 0x00AF9A000000FFFF   ; 64-bit code segment
    dq 0x00CF92000000FFFF   ; Writable data segment for long mode
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start


section .data

mmap_count: dw 0
mmap_calls: dw 0
bios_drive: db 0
align 4
payload_dap:
    db 0x10, 0         ; Packet size and reserved byte
    dw 64              ; Number of sectors
    dw 0x0000, 0x1000  ; Offset, then segment: physical 0x10000
    dq 33              ; Starting LBA


section .note.GNU-stack noalloc noexec nowrite progbits
