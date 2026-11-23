[bits 16]
org 0x8000

stage2_start:
    ; Inherit segment registers and stack from stage 1
    mov ah, 0x0E    ; BIOS teletype output
    mov al, '2'     ; Character to print
    mov bh, 0       ; Display page zero
    mov bl, 0x07    ; Color if in graphics mode
    int 0x10

    ; Prepare the E820 memory-map buffer
    xor ax, ax
    mov es, ax          ; Buffer segment = 0
    mov di, 0x9000      ; Buffer starts at physical address 0x9000

    ; Initialize memory-map enumeration
    xor ebx, ebx        ; Zero means request the first entry
    xor bp, bp          ; Number of stored entries = 0

; E820 collection loop
.e820_next:
    cmp bp, 64
    jae .mmap_error     ; Buffer is full: stop before overflowing

    ; Prepare this entry and the BIOS request
    mov dword [es:di + 20], 1   ; Default extended attributes
    mov eax, 0xE820             ; Get a memory-map entry
    mov edx, 0x534D4150         ; Required "SMAP" signature
    mov ecx, 24                 ; Space available for this entry
    stc
    int 0x15

    jc .mmap_finished          ; Can indicate end of enumeration

    cmp eax, 0x534D4150
    jne .mmap_error            ; BIOS must return "SMAP"

    cmp ecx, 20
    jb .mmap_error             ; Entry is too short
    je .check_length           ; Older BIOS: 20-byte entry

    cmp ecx, 24
    jb .mmap_error

    test dword [es:di + 20], 1
    jz .e820_continue          ; Skip disabled entries

.check_length:
    mov eax, [es:di + 8]
    or eax, [es:di + 12]
    jz .e820_continue          ; Skip zero-length regions

    inc bp                     ; Count this entry
    add di, 24                 ; Advance to the next buffer slot

.e820_continue:
    test ebx, ebx
    jnz .e820_next             ; Nonzero means more entries remain

.mmap_finished:
    test bp, bp
    jz .mmap_error             ; No valid entries were collected

    mov [mmap_count], bp
    mov ax, 0x0E4D             ; Teletype service, character 'M'
    jmp .mmap_report

.mmap_error:
    mov word [mmap_count], 0
    mov ax, 0x0E45             ; Teletype service, character 'E'

.mmap_report:
    mov bh, 0
    mov bl, 0x07
    int 0x10

    ; Continue only if memory-map collection succeeded
    cmp word [mmap_count], 0
    je .halt
    jmp .enable_a20

.enable_a20:
    ; Ask BIOS to enable A20
    mov ax, 0x2401
    int 0x15
    jc .a20_error

    test ah, ah
    jnz .a20_error

    ; Ask BIOS for the resulting A20 status
    mov ax, 0x2402
    int 0x15
    jc .a20_error

    test ah, ah
    jnz .a20_error

    cmp al, 1
    jne .a20_error

    ; BIOS reports that A20 is enabled
    mov ax, 0x0E41          ; AH = teletype, AL = 'A'
    jmp .a20_report

.a20_error:
    mov ax, 0x0E21          ; AH = teletype, AL = '!'

.a20_report:
    mov bh, 0
    mov bl, 0x07
    int 0x10
    jmp .halt

.halt:
    hlt
    jmp .halt

mmap_count: dw 0

; Pad this temporary stage 2 to one sector
times (8 * 512) - ($ - $$) db 0