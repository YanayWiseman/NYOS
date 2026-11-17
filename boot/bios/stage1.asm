[bits 16]
org 0x7C00

start:
    cli ; Disable Interrupts
    
    xor ax, ax ; Clear AX

    ; Initialize data segments and stack segment
    mov ds, ax    
    mov es, ax
    mov ss, ax

    ; Set stack pointer
    mov sp, 0x7C00

    cld ; String Operations advance forward
    mov [boot_drive], dl ; Save BIOS boot drive number
    sti ; Allow maskable hardware interrupts
    

