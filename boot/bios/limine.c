#include "limine.h"

#define BASE_REVISION UINT64_C(3)
#define HHDM_OFFSET UINT64_C(0xFFFF800000000000)
#define EXECUTABLE_MIN UINT64_C(0xFFFFFFFF80000000)
#define PHYS_LIMIT UINT64_C(0x100000000)
#define PAGE_SIZE 4096u
#define PAGE_BITS UINT64_C(0xFFF)
#define MAX_REQUESTS 128u
#define MAX_RANGES (64 + MEMORY_MAX_ALLOCATIONS + 3)
#define MAX_ENTRIES (2 * MAX_RANGES)

enum map_type
{
    USABLE, RESERVED, ACPI_RECLAIMABLE, ACPI_NVS, BAD,
    BOOTLOADER_RECLAIMABLE, EXECUTABLE
};

struct request_set
{
    uint8_t* base;
    uint8_t* hhdm;
    uint8_t* memmap;
};

struct map_entry
{
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct responses
{
    uint64_t hhdm[2];
    uint64_t memmap[3];
    uint64_t pointers[MAX_ENTRIES];
    struct map_entry entries[MAX_ENTRIES];
};

struct range
{
    uint64_t begin;
    uint64_t end;
    uint32_t type;
    uint32_t priority;
};

static uint64_t read64(const uint8_t* bytes)
{
    uint64_t value;
    __builtin_memcpy(&value, bytes, sizeof(value));
    return value;
}

static void write64(uint8_t* bytes, uint64_t value)
{
    __builtin_memcpy(bytes, &value, sizeof(value));
}

static bool matches(const uint8_t* bytes, uint64_t first, uint64_t second)
{
    return read64(bytes) == first && read64(bytes + 8) == second;
}

/* Find aligned in-image records and reject duplicate request IDs. */
static enum limine_result find_requests(const struct elf64_image* image,
                                        struct request_set* requests)
{
    uint8_t* bytes = (uint8_t*)(uintptr_t)image->physical_base;
    uint32_t begin = 0;
    uint32_t end = image->size;
    for (uint32_t i = 0; end - i >= 16; i += 8)
    {
        uint8_t* at = bytes + i;
        if (matches(at, UINT64_C(0xADC0E0531BB10D03),
                        UINT64_C(0x9572709F31764C62)))
        {
            end = i;
            break;
        }
        if (end - i >= 32 &&
            matches(at, UINT64_C(0xF6B8F4B39DE7D1AE), UINT64_C(0xFAB91A6940FCB9CF)) &&
            matches(at + 16, UINT64_C(0x785C6ED015D3E316), UINT64_C(0x181E920A7852B9D9)))
        {
            begin = i + 32;
            i += 24;
        }
    }

    uint32_t seen[MAX_REQUESTS];
    uint32_t count = 0;
    for (uint32_t i = begin; end - i >= 24; i += 8)
    {
        uint8_t* at = bytes + i;
        if (matches(at, UINT64_C(0xF9562B2D5C95A6C8), UINT64_C(0x6A7B384944536BDC)))
        {
            if (requests->base != NULL || read64(at + 16) != BASE_REVISION)
            {
                return LIMINE_INVALID;
            }
            requests->base = at;
            i += 16;
            continue;
        }
        if (!matches(at, UINT64_C(0xC7B1DD30DF4C8B88), UINT64_C(0x0A82E883A194F07B)))
        {
            continue;
        }
        if (end - i < 48 || count == MAX_REQUESTS)
        {
            return LIMINE_INVALID;
        }
        uint64_t id_a = read64(at + 16);
        uint64_t id_b = read64(at + 24);
        for (uint32_t j = 0; j < count; ++j)
        {
            if (matches(bytes + seen[j] + 16, id_a, id_b))
            {
                return LIMINE_INVALID;
            }
        }
        seen[count++] = i;

        if (id_a == UINT64_C(0x48DCF1CB8AD2B852) && id_b == UINT64_C(0x63984E959A98244B))
        {
            requests->hhdm = at;
        }
        if (id_a == UINT64_C(0x67CF3D9D378A806F) && id_b == UINT64_C(0xE304ACDFC50C3C62))
        {
            requests->memmap = at;
        }
        i += 40;
    }

    if (requests->base == NULL && count == 0)
    {
        return LIMINE_LEGACY;
    }
    if (requests->base == NULL || image->virtual_base < EXECUTABLE_MIN)
    {
        return LIMINE_INVALID;
    }
    return LIMINE_READY;
}

/* Build a sorted map, with firmware reservations taking priority over RAM. */
static uint32_t make_memory_map(struct map_entry* entries,
                                 const struct e820_entry* e820, uint32_t count,
                                 const struct elf64_image* image)
{
    struct range ranges[MAX_RANGES];
    uint32_t used = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        uint64_t base = e820[i].base;
        uint64_t end = base + e820[i].length;
        uint32_t type;
        switch (e820[i].type)
        {
            case 1:
                type = USABLE;
                if (end > PHYS_LIMIT)
                {
                    return 0;  /* The 32-bit allocator supports RAM below 4 GiB. */
                }
                base = (base + PAGE_BITS) & ~PAGE_BITS;
                end &= ~PAGE_BITS;
                break;
            case 3: type = ACPI_RECLAIMABLE; break;
            case 4: type = ACPI_NVS; break;
            case 5: type = BAD; break;
            default: type = RESERVED; break;
        }

        if (type != USABLE)
        {
            if (end > UINT64_MAX - PAGE_BITS)
            {
                return 0;
            }
            base &= ~PAGE_BITS;
            end = (end + PAGE_BITS) & ~PAGE_BITS;
        }

        if (e820[i].length != 0 && base < end)
        {
            uint32_t priority = 1;
            switch (type)
            {
                case ACPI_RECLAIMABLE: priority = 5; break;
                case ACPI_NVS: priority = 6; break;
                case RESERVED: priority = 7; break;
                case BAD: priority = 8; break;
                default: break;
            }
            ranges[used++] = (struct range) {base, end, type, priority};
        }
    }

    /* Keep BIOS scratch memory reserved. Stage 2 contains the live GDT. */
    ranges[used++] = (struct range) {0, 0x100000, RESERVED, 2};
    ranges[used++] = (struct range) {0x8000, 0xC000, BOOTLOADER_RECLAIMABLE, 3};

    uint32_t allocation_count;
    const struct memory_allocation* allocation = memory_allocations(&allocation_count);
    for (uint32_t i = 0; i < allocation_count; ++i)
    {
        uint64_t base = allocation[i].base;
        ranges[used++] = (struct range) {
            base, base + allocation[i].length, BOOTLOADER_RECLAIMABLE, 3
        };
    }
    ranges[used++] = (struct range) {
        image->physical_base, (uint64_t)image->physical_base + image->size, EXECUTABLE, 4
    };

    uint32_t out = 0;
    for (uint64_t at = 0;;)
    {
        uint64_t next = UINT64_MAX;
        uint32_t priority = 0;
        uint32_t type = RESERVED;
        for (uint32_t i = 0; i < used; ++i)
        {
            const struct range* r = &ranges[i];
            if (at < r->begin)
            {
                if (r->begin < next)
                {
                    next = r->begin;
                }
            }
            else if (at < r->end)
            {
                if (r->end < next)
                {
                    next = r->end;
                }
                if (r->priority > priority ||
                    (r->priority == priority && r->type > type))
                {
                    priority = r->priority;
                    type = r->type;
                }
            }
        }

        if (next == UINT64_MAX)
        {
            return out;
        }
        if (priority != 0)
        {
            struct map_entry* previous = out ? &entries[out - 1] : NULL;
            if (previous != NULL && previous->type == type &&
                previous->base + previous->length == at)
            {
                previous->length += next - at;
            }
            else
            {
                if (out == MAX_ENTRIES)
                {
                    return 0;
                }
                entries[out++] = (struct map_entry) {at, next - at, type};
            }
        }
        at = next;
    }
}

/* This QEMU pc target has one I/O APIC at the standard address. */
static bool mask_pc_ioapic(void)
{
    volatile uint32_t* select = (volatile uint32_t*)(uintptr_t)0xFEC00000u;
    volatile uint32_t* window = (volatile uint32_t*)(uintptr_t)0xFEC00010u;
    *select = 1;
    uint32_t version = *window;
    uint32_t last = (version >> 16) & 0xFF;
    if (version == 0 || version == UINT32_MAX || last > 119)
    {
        return false;
    }
    for (uint32_t i = 0; i <= last; ++i)
    {
        *select = 0x10 + 2 * i;
        uint32_t redirection = *window;
        if (((redirection >> 8) & 7) <= 1)
        {
            *window = redirection | (1u << 16);
        }
    }
    return true;
}

enum limine_result limine_prepare(const struct elf64_image* image,
                                 const struct e820_entry* map, uint32_t count,
                                 uint32_t pml4, uint64_t* stack_top)
{
    struct request_set requests = {NULL, NULL, NULL};
    enum limine_result result = find_requests(image, &requests);
    if (result != LIMINE_READY)
    {
        return result;
    }

    uint32_t stack = memory_alloc_pages(16);
    uint32_t storage = memory_alloc_pages((sizeof(struct responses) + PAGE_SIZE - 1) / PAGE_SIZE);
    if (stack == 0 || storage == 0)
    {
        return LIMINE_INVALID;
    }
    struct responses* data = (struct responses*)(uintptr_t)storage;
    uint32_t entries = make_memory_map(data->entries, map, count, image);
    if (entries == 0)
    {
        return LIMINE_INVALID;
    }

    /* Types 0, 5 and 6 have the same direct mapping at base revision 3. */
    bool mapped_loader = false;
    for (uint32_t i = 0; i < entries; ++i)
    {
        const struct map_entry* entry = &data->entries[i];
        if (entry->type != USABLE && entry->type != BOOTLOADER_RECLAIMABLE &&
            entry->type != EXECUTABLE)
        {
            continue;
        }
        uint64_t end = entry->base + entry->length;
        if (entry->base <= 0x8000 && end >= 0xC000)
        {
            mapped_loader = true;
        }
        for (uint64_t addr = entry->base; addr < end; addr += PAGE_SIZE)
        {
            if (!bios_map_page(pml4, HHDM_OFFSET + addr, (uint32_t)addr, 2))
            {
                return LIMINE_INVALID;
            }
        }
    }

    /* The page-table allocations made above now appear as reclaimable. */
    entries = make_memory_map(data->entries, map, count, image);
    if (entries == 0 || !mapped_loader || !mask_pc_ioapic())
    {
        return LIMINE_INVALID;
    }

    if (requests.memmap != NULL)
    {
        for (uint32_t i = 0; i < entries; ++i)
        {
            data->pointers[i] = HHDM_OFFSET + (uintptr_t)&data->entries[i];
        }
        data->memmap[0] = 0;
        data->memmap[1] = entries;
        data->memmap[2] = HHDM_OFFSET + (uintptr_t)&data->pointers[0];
        write64(requests.memmap + 40, HHDM_OFFSET + (uintptr_t)&data->memmap[0]);
    }
    if (requests.hhdm != NULL)
    {
        data->hhdm[0] = 0;
        data->hhdm[1] = HHDM_OFFSET;
        write64(requests.hhdm + 40, HHDM_OFFSET + (uintptr_t)&data->hhdm[0]);
    }
    write64(requests.base + 8, BASE_REVISION);
    write64(requests.base + 16, 0);
    *stack_top = HHDM_OFFSET + stack + 16u * PAGE_SIZE;
    return LIMINE_READY;
}
