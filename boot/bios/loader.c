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

static uint16_t read_le16(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t* p)
{
    return (uint32_t)read_le16(p) | ((uint32_t)read_le16(p + 2) << 16);
}

static uint64_t read_le64(const uint8_t* p)
{
    return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}

/*
    Checks a static x86-64 ELF.
    Input: File bytes, size, entry output pointer.
    Output: Loadable segment count, or zero on failure.
*/
static uint32_t elf64_validate(const uint8_t* file, uint32_t size, uint64_t* entry_out)
{
    enum 
    {
        EHDR_SIZE = 64, PHDR_SIZE = 56,
        PT_LOAD = 1, PT_DYNAMIC = 2, PT_INTERP = 3, PF_X = 1
    };

    /* The disk staging check already verified the ELF magic. */
    if (size < EHDR_SIZE ||
        file[4] != 2 || file[5] != 1 || file[6] != 1 ||
        read_le16(file + 16) != 2 ||  /* ET_EXEC */
        read_le16(file + 18) != 62 || /* EM_X86_64 */
        read_le32(file + 20) != 1 ||
        read_le16(file + 52) != EHDR_SIZE ||
        read_le16(file + 54) != PHDR_SIZE)
    {
        return 0;
    }

    uint64_t entry = read_le64(file + 24);
    uint64_t phoff = read_le64(file + 32);
    uint32_t count = read_le16(file + 56);

    /* Check the table bounds before reading any program headers. */
    if (count == 0 || phoff < EHDR_SIZE || phoff > size || count * PHDR_SIZE > size - (uint32_t)phoff)
    {
        return 0;
    }

    const uint8_t* ph = file + (uint32_t)phoff;
    uint32_t load_count = 0;
    bool entry_is_executable = false;

    for (uint32_t i = 0; i < count; ++i, ph += PHDR_SIZE)
    {
        uint32_t type = read_le32(ph);
        if (type == PT_DYNAMIC || type == PT_INTERP)
        {
            return 0;
        }
        if (type != PT_LOAD)
        {
            continue;
        }

        uint64_t offset = read_le64(ph + 8);
        uint64_t vaddr = read_le64(ph + 16);
        uint64_t filesz = read_le64(ph + 32);
        uint64_t memsz = read_le64(ph + 40);
        uint64_t align = read_le64(ph + 48);

        if (filesz > memsz || offset > size || filesz > size - offset || memsz > UINT64_MAX - vaddr)
        {
            return 0;
        }
        if (align > 1 && ((align & (align - 1)) != 0 || ((vaddr ^ offset) & (align - 1)) != 0))
        {
            return 0;
        }
        if (memsz == 0)
        {
            continue;
        }

        ++load_count;
        if ((read_le32(ph + 4) & PF_X) != 0 && entry >= vaddr && entry - vaddr < memsz)
        {
            entry_is_executable = true;
        }
    }

    if (!entry_is_executable)
    {
        return 0;
    }

    *entry_out = entry;
    return load_count;
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
    uint64_t entry;
    uint32_t segments = elf64_validate(destination, size, &entry);
    if (segments == 0)
    {
        debug_puts("Unsupported or invalid ELF64\n");
        bios_halt();
    }
    debug_puts("ELF64 entry: ");
    debug_hex64(entry);
    debug_puts("\nLoadable segments: ");
    debug_dec(segments);
    debug_puts("\n");
    debug_dec(size);
    debug_puts(" bytes at ");
    debug_hex64(address);
    debug_puts("\n");
    bios_halt();
}