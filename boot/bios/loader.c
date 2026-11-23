#include <stdint.h>

extern void bios_outb(uint16_t port, uint8_t value);
extern _Noreturn void bios_halt(void);

_Noreturn void bios_main(uint32_t map_address, uint32_t entry_count, uint32_t boot_drive)
{
    const char *message = "BIOS loader: C11 reached";

    if (map_address != 0x9000 || entry_count == 0 || entry_count > 64 || boot_drive > 0xFF) 
    {
        message = "BIOS loader: invalid C entry arguments";
    }

    /* Our current QEMU test uses VGA text mode. */
    volatile uint16_t *screen = (volatile uint16_t *)0xB8000;

    for (uint32_t i = 0; message[i] != '\0'; ++i) 
    {
        /* Write on the second screen row. */
        screen[80 + i] = (uint16_t)(0x0700u | (uint8_t)message[i]);

        /* Also send the character to QEMU's debug console. */
        bios_outb(0xE9, (uint8_t)message[i]);
    }

    bios_outb(0xE9, '\n');
    bios_halt();
}