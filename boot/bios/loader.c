#include "memory.h"
#include <stdint.h>

extern void bios_enter_long_mode(uint32_t pml4, uint64_t entry);
extern void bios_outb(uint16_t port, uint8_t value);
extern _Noreturn void bios_halt(void);

struct elf64_image
{
    uint64_t entry;
    uint64_t virtual_base;
    uint32_t physical_base;
    uint32_t size;
};


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
    Loads a static x86-64 ELF.
    Input: File bytes, size, image output pointer.
    Output: Segment count, or zero on failure.
*/
static uint32_t elf64_load(const uint8_t* file, uint32_t size, struct elf64_image* image)
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
    uint64_t virtual_base = 0;
    uint64_t last_end = 0;

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
        if (memsz == 0)
        {
            continue;
        }
        if (align > 1 && ((align & (align - 1)) != 0 || ((vaddr ^ offset) & (align - 1)) != 0))
        {
            return 0;
        }
        if (load_count == 0)
        {
            virtual_base = vaddr & ~UINT64_C(0xFFF);
        }
        else if (vaddr < last_end)
        {
            return 0; /* Overlapping or out-of-order segments. */
        }
        last_end = vaddr + memsz;

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

        uint64_t span = last_end - virtual_base;
    if (span > UINT32_MAX - 4095u)
    {
        return 0;
    }

    uint32_t pages = ((uint32_t)span + 4095u) / 4096u;
    uint32_t physical_base = memory_alloc_pages(pages);
    if (physical_base == 0)
    {
        return 0;
    }

    uint32_t image_size = pages * 4096u;
    uint8_t* target = (uint8_t*)(uintptr_t)physical_base;

    /* Clear the image so BSS and gaps start at zero. */
    for (uint32_t i = 0; i < image_size; ++i)
    {
        target[i] = 0;
    }

    /* The first pass already validated these headers and ranges. */
    ph = file + (uint32_t)phoff;
    for (uint32_t i = 0; i < count; ++i, ph += PHDR_SIZE)
    {
        if (read_le32(ph) != PT_LOAD)
        {
            continue;
        }

        uint32_t filesz = (uint32_t)read_le64(ph + 32);
        if (filesz == 0)
        {
            continue;
        }

        uint32_t offset = (uint32_t)read_le64(ph + 8);
        uint32_t delta = (uint32_t)(read_le64(ph + 16) - virtual_base);

        for (uint32_t j = 0; j < filesz; ++j)
        {
            target[delta + j] = file[offset + j];
        }
    }

    image->entry = entry;
    image->virtual_base = virtual_base;
    image->physical_base = physical_base;
    image->size = image_size;
    return load_count;
}

#define PAGE_SIZE    4096u
#define PAGE_MASK    UINT64_C(0xFFFFF000)  /* Physical pages below 4 GiB. */
#define PAGE_PRESENT UINT64_C(1)
#define PAGE_WRITE   UINT64_C(2)
#define PAGE_NX      (UINT64_C(1) << 63)

static uint32_t allocate_page_table(void)
{
    uint32_t address = memory_alloc_pages(1);
    if (address != 0)
    {
        uint64_t* table = (uint64_t*)(uintptr_t)address;
        for (uint32_t i = 0; i < 512; ++i)
        {
            table[i] = 0;
        }
    }

    return address;
}

/*
    Walks to a 4 KiB page entry.
    Input: PML4 address, virtual address, create flag.
    Output: Page entry pointer, or NULL on failure.
*/
static uint64_t* page_entry(uint32_t root, uint64_t address, bool create)
{
    /* Check that the address is canonical for four-level paging. */
    uint64_t high = address >> 47;
    if (high != 0 && high != 0x1FFFF)
    {
        return NULL;
    }

    uint64_t* table = (uint64_t*)(uintptr_t)root;
    for (uint32_t shift = 39; shift > 12; shift -= 9)
    {
        uint64_t* entry = &table[(address >> shift) & 0x1FF];
        if ((*entry & PAGE_PRESENT) == 0)
        {
            uint32_t next = create ? allocate_page_table() : 0;
            if (next == 0)
            {
                return NULL;
            }

            *entry = next | PAGE_PRESENT | PAGE_WRITE;
        }

        table = (uint64_t*)(uintptr_t)(*entry & PAGE_MASK);
    }

    return &table[(address >> 12) & 0x1FF];
}

static bool map_page(uint32_t root, uint64_t virt, uint32_t phys, uint64_t flags)
{
    uint64_t* slot = page_entry(root, virt, true);
    uint64_t entry = phys | flags | PAGE_PRESENT;

    if (slot == NULL || (*slot != 0 && *slot != entry))
    {
        return false;
    }

    *slot = entry;
    return true;
}

/*
    Builds the handoff page tables.
    Input: Validated ELF file and loaded image.
    Output: PML4 physical address, or zero on failure.
*/
static uint32_t build_page_tables(const uint8_t* file, const struct elf64_image* image)
{
    uint32_t root = allocate_page_table();
    if (root == 0)
    {
        return 0;
    }

    /* Identity-map low memory for stage 2, its stack, and VGA text. */
    for (uint32_t addr = PAGE_SIZE; addr < 0x100000; addr += PAGE_SIZE)
    {
        uint64_t flags = PAGE_WRITE;
        if (addr < 0x8000 || addr >= 0xA000)
        {
            flags |= PAGE_NX;
        }

        if (!map_page(root, addr, addr, flags))
        {
            return 0;
        }
    }

    /* Reuse the program headers already checked by elf64_load. */
    const uint8_t* ph = file + (uint32_t)read_le64(file + 32);
    uint32_t count = read_le16(file + 56);

    for (uint32_t i = 0; i < count; ++i, ph += 56)
    {
        if (read_le32(ph) != 1) /* PT_LOAD */
        {
            continue;
        }

        uint64_t size = read_le64(ph + 40);
        if (size == 0)
        {
            continue;
        }

        uint32_t elf_flags = read_le32(ph + 4);
        uint64_t flags = (elf_flags & 2) ? PAGE_WRITE : 0; /* PF_W */
        if ((elf_flags & 1) == 0)                        /* PF_X */
        {
            flags |= PAGE_NX;
        }

        uint64_t start = read_le64(ph + 16);
        uint64_t page = start & ~UINT64_C(0xFFF);
        uint64_t last = (start + size - 1) & ~UINT64_C(0xFFF);

        for (;; page += PAGE_SIZE)
        {
            uint32_t phys = image->physical_base + (uint32_t)(page - image->virtual_base);

            if (!map_page(root, page, phys, flags))
            {
                return 0;
            }
            if (page == last)
            {
                break;
            }
        }
    }

    return root;
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
    if (map_address != 0xA000 || entry_count == 0 || entry_count > 64 || boot_drive > 0xFF) 
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

    struct elf64_image image;
    uint32_t segments = elf64_load(destination, size, &image);
    if (segments == 0)
    {
        debug_puts("ELF64 load failed\n");
        bios_halt();
    }

    debug_puts("ELF64 entry: ");
    debug_hex64(image.entry);
    debug_puts("\nLoadable segments: ");
    debug_dec(segments);
    debug_puts("\nELF64 physical base: ");
    debug_hex64(image.physical_base);
    debug_puts("\nELF64 virtual base: ");
    debug_hex64(image.virtual_base);
    debug_puts("\nELF64 image bytes: ");
    debug_dec(image.size);
    debug_puts("\nELF64 segments loaded\n");
    
    uint32_t pml4 = build_page_tables(destination, &image);
    if (pml4 == 0)
    {
        debug_puts("Page table build failed\n");
        bios_halt();
    }

    uint64_t* entry = page_entry(pml4, image.entry, false);
    uint32_t expected = image.physical_base + (uint32_t)(image.entry - image.virtual_base);

    if (entry == NULL || (*entry & (PAGE_PRESENT | PAGE_NX)) != PAGE_PRESENT || (*entry & PAGE_MASK) != (expected & ~0xFFFu))
    {
        debug_puts("Entry mapping check failed\n");
        bios_halt();
    }

    debug_puts("PML4 at ");
    debug_hex64(pml4);
    debug_puts("\nEntry maps to ");
    debug_hex64(expected);
    debug_puts("\nPaging tables verified\n");
    bios_enter_long_mode(pml4, image.entry);

    debug_puts("CPU lacks long mode, NX, or required CPU features\n");
    bios_halt();
}