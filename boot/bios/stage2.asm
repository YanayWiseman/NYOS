[bits 16]

section .text.start
global stage2_start

extern bios_main
extern __bss_start
extern __bss_end

MAP_BUFFER   equ 0xA000
MAP_CAPACITY equ 64
CODE_SEL     equ 0x08
DATA_SEL     equ 0x10

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


section .rodata
align 8

gdt_start:
    dq 0x0000000000000000   ; Null descriptor
    dq 0x00CF9A000000FFFF   ; Flat 32-bit code segment
    dq 0x00CF92000000FFFF   ; Flat writable data segment
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
    dq 17              ; Starting LBA


section .note.GNU-stack noalloc noexec nowrite progbits