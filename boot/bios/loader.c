#include "memory.h"
#include <stdint.h>


extern void bios_outb(uint16_t port, uint8_t value);
extern _Noreturn void bios_halt(void);

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
    if (!memory_init(map, entry_count)) 
    {
        debug_puts("Memory allocator initialization failed\n");
        bios_halt();
    }
    
    /* The ELF slot begins with a four-byte file size. */
    const volatile uint32_t* slot_size = (const volatile uint32_t*)(uintptr_t)0x10000;
    const volatile uint8_t* source = (const volatile uint8_t*)(uintptr_t)0x10004;
    uint32_t size = *slot_size;

    if (size < 4 || size > 64u * 512u - 4u || source[0] != 0x7F || source[1] != 'E' || source[2] != 'L' || source[3] != 'F')
    {
        debug_puts("Invalid ELF payload slot\n");
        bios_halt();
    }

    uint32_t address = memory_alloc_pages((size + 4095u) / 4096u);
    if (address == 0)
    {
        debug_puts("Cannot allocate ELF buffer\n");
        bios_halt();
    }

    uint8_t* destination = (uint8_t*)(uintptr_t)address;
    for (uint32_t i = 0; i < size; ++i)
    {
        destination[i] = source[i];
    }

    debug_puts("ELF staged: ");
    debug_dec(size);
    debug_puts(" bytes at ");
    debug_hex64(address);
    debug_puts("\n");
    bios_halt();
}