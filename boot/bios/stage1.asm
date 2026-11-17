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

    .halt:
        hlt
        jmp .halt

; Saved BIOS boot drive
boot_drive: db 0

; Pad everything before the signature to 510 bytes
times 510 - ($ - $$) db 0

; BIOS boot signature: emits bytes 0x55, 0xAA
dw 0xAA55

