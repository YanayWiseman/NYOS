[bits 16]
org 0x8000

stage2_start:
    ; Inherit segment registers and stack from stage 1

    mov ah, 0x0E    ; BIOS teletype output
    mov al, '2'     ; Character to print
    mov bh, 0       ; Display page zero
    mov bl, 0x07    ; Color if in graphics mode
    int 0x10

.halt:
    hlt
    jmp .halt

; Pad this temporary stage 2 to one sector
times 512 - ($ - $$) db 0