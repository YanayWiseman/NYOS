#include <stddef.h>
#include <stdint.h>

extern void bios_outb(uint16_t port, uint8_t value);
extern _Noreturn void bios_halt(void);

/* Matches the 24-byte slots written by our assembly collector. */
struct e820_entry 
{
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attributes;
};

_Static_assert(sizeof(struct e820_entry) == 24, "E820 entries must occupy 24 bytes");
_Static_assert(offsetof(struct e820_entry, type) == 16, "Unexpected E820 field layout");
_Static_assert(offsetof(struct e820_entry, attributes) == 20, "Unexpected E820 field layout");

/*
	Prints a string.
	Input: String pointer.
	Output: None.
*/
static void debug_puts(const char *text)
{
    while (*text != '\0') 
    {
        bios_outb(0xE9, (uint8_t)*text++);
    }
}

/*
	Prints a hexadecimal number.
	Input: 64-bit unsigned integer.
	Output: None.
*/
static void debug_hex64(uint64_t value)
{
    static const char digits[] = "0123456789ABCDEF";

    debug_puts("0x");

    for (uint32_t i = 0; i < 16; ++i) 
    {
        bios_outb(0xE9, (uint8_t)digits[(uint32_t)(value >> 60)]);
        value <<= 4;
    }
}

/*
	Prints a decimal number.
	Input: 32-bit unsigned integer.
	Output: None.
*/
static void debug_dec(uint32_t value)
{
    char digits[10];
    uint32_t count = 0;

    do 
    {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);

    while (count != 0) 
    {
        bios_outb(0xE9, (uint8_t)digits[--count]);
    }
}

/*
	Runs the BIOS loader.
	Input: Map address, entry count, boot drive.
	Output: Never returns.
*/
_Noreturn void bios_main(uint32_t map_address, uint32_t entry_count, uint32_t boot_drive)
{
    const char* message = "BIOS loader: C11 reached";

    volatile uint16_t* screen = (volatile uint16_t*)0xB8000;

    for (uint32_t i = 0; message[i] != '\0'; ++i) 
    {
        screen[80 + i] = (uint16_t)(0x0700u | (uint8_t)message[i]);
    }

    debug_puts(message);
    debug_puts("\n");

    /* Validate the arguments supplied by our assembly startup. */
    if (map_address != 0x9000 || entry_count == 0 || entry_count > 64 || boot_drive > 0xFF) 
    {
        debug_puts("Invalid boot arguments\n");
        bios_halt();
    }

    /* Interpret the existing buffer as an array of E820 entries. */
    const struct e820_entry* map = (const struct e820_entry*)(uintptr_t)map_address;

    debug_puts("Boot drive: ");
    debug_dec(boot_drive);

    debug_puts("\nE820 entries: ");
    debug_dec(entry_count);
    debug_puts("\n");

    for (uint32_t i = 0; i < entry_count; ++i) 
    {
        debug_puts("Entry ");
        debug_dec(i);

        debug_puts(" base=");
        debug_hex64(map[i].base);

        debug_puts(" length=");
        debug_hex64(map[i].length);

        debug_puts(" type=");
        debug_dec(map[i].type);

        if (map[i].type == 1) 
        {
            debug_puts(" (usable RAM)");
        }

        debug_puts("\n");
    }

    debug_puts("Memory-map dump complete\n");
    bios_halt();
}