[bits 16]
org 0x7C00

start:
    cli ; Disable Interrupts
    
    xor ax, ax 

    ; Initialize data segments and stack segment
    mov ds, ax    
    mov es, ax
    mov ss, ax

    ; Set stack pointer
    mov sp, 0x7C00

    cld                     
    mov [boot_drive], dl    ; Save BIOS boot drive number
    sti                     

    ; Print NYOS
    ; Print 'N'
    mov ah, 0x0E    ; BIOS teletype output
    mov al, 'N'     ; Character to print
    mov bh, 0       ; Display page zero
    mov bl, 0x07    ; Color if in graphics mode
    int 0x10        ; Call BIOS video service
    
    ; Print 'Y'
    mov al, 'Y'
    int 0x10
    
    ; Print 'O'
    mov al, 'O'
    int 0x10

    ; Print 'S'
    mov al, 'S'
    int 0x10

    ; Check for extended BIOS disk access.
    mov dl, [boot_drive]
    mov ax, 0x4100
    mov bx, 0x55AA
    push ds
    int 0x13
    pop ds
    jc .disk_error
    cmp bx, 0xAA55
    jne .disk_error
    test cx, 1
    jz .disk_error

    ; Load the 16 KiB stage-2 slot at physical address 0x8000.
    mov dl, [boot_drive]
    mov si, stage2_dap
    mov ax, 0x4200
    push ds
    int 0x13
    pop ds
    jc .disk_error
    cmp word [stage2_dap + 2], 32
    jne .disk_error
    mov dl, [boot_drive]     ; Pass the original boot drive to stage 2
    jmp 0x0000:0x8000

.disk_error:
    mov ah, 0x0E
    mov al, 'E'
    mov bh, 0
    mov bl, 0x07
    int 0x10

.halt:
    hlt
    jmp .halt


; Saved BIOS boot drive
boot_drive: db 0

align 4
stage2_dap:
    db 0x10, 0
    dw 32
    dw 0x8000, 0
    dq 1

; Pad everything before the signature to 510 bytes
times 510 - ($ - $$) db 0

; BIOS boot signature: emits bytes 0x55, 0xAA
dw 0xAA55

