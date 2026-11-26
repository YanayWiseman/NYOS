#include "memory.h"

#define PAGE_SIZE       4096ULL
#define LOW_MEMORY_END  0x100000ULL
#define ADDRESS_LIMIT   0x100000000ULL

static const struct e820_entry *memory_map;
static uint32_t memory_map_count;
static uint64_t next_address;
static struct memory_allocation allocations[MEMORY_MAX_ALLOCATIONS];
static uint32_t allocation_count;

const struct memory_allocation* memory_allocations(uint32_t* count)
{
    *count = allocation_count;
    return allocations;
}


/*
	nitializes the physical allocator once.
	Input: E820 map and entry count.
	Output: True on success.
*/
bool memory_init(const struct e820_entry* map, uint32_t count)
{
    if (memory_map != NULL || map == NULL || count == 0 || count > 64) 
    {
        return false;
    }

    /* Reject entries whose address ranges overflow. */
    for (uint32_t i = 0; i < count; ++i) 
    {
        if (map[i].length > UINT64_MAX - map[i].base) 
        {
            return false;
        }
    }

    memory_map = map;
    memory_map_count = count;

    /*
        Reserve everything below 1 MiB.
        This covers our loader, map buffer, stacks and BIOS data.
    */
    next_address = LOW_MEMORY_END;

    return true;
}


/*
	Reserves contiguous physical pages.
	Input: Number of 4 KiB pages.
	Output: Physical address, or zero on failure.
*/
uint32_t memory_alloc_pages(uint32_t pages)
{
    if (memory_map == NULL || pages == 0) 
    {
        return 0;
    }

    uint64_t bytes = (uint64_t)pages * PAGE_SIZE;

    if (bytes > ADDRESS_LIMIT - LOW_MEMORY_END) 
    {
        return 0;
    }

    uint64_t best_address = 0;

    for (uint32_t i = 0; i < memory_map_count; ++i) 
    {
        const struct e820_entry* region = &memory_map[i];

        if (region->type != 1 || region->length == 0) 
        {
            continue;
        }

        uint64_t end = region->base + region->length;

        if (end > ADDRESS_LIMIT) 
        {
            end = ADDRESS_LIMIT;
        }

        uint64_t candidate = region->base;

        /* Previously allocated addresses cannot be reused. */
        if (candidate < next_address) 
        {
            candidate = next_address;
        }

        if (candidate >= end) 
        {
            continue;
        }

        /* Round upward to a 4 KiB boundary. */
        candidate = (candidate + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        for (;;) // Just an infinite loop, yeah I know it looks scary 
        {
            if (candidate >= end || bytes > end - candidate) 
            {
                break;
            }

            bool blocked = false;

            /*
                A reserved entry may overlap a usable entry.
                Reserved regions take priority.
            */
            for (uint32_t j = 0; j < memory_map_count; ++j) 
            {
                const struct e820_entry* reserved = &memory_map[j];

                if (reserved->type == 1 || reserved->length == 0) 
                {
                    continue;
                }

                uint64_t reserved_end = reserved->base + reserved->length;

                if (candidate < reserved_end && reserved->base < candidate + bytes) 
                {
                    /* Retry immediately after the reserved region. */
                    if (reserved_end >= ADDRESS_LIMIT) 
                    {
                        candidate = ADDRESS_LIMIT;
                    } 
                    else 
                    {
                        candidate = (reserved_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                    }

                    blocked = true;
                    break;
                }
            }

            if (blocked) 
            {
                continue;
            }

            /* E820 entries are not guaranteed to be sorted. */
            if (best_address == 0 || candidate < best_address) 
            {
                best_address = candidate;
            }

            break;
        }
    }

    if (best_address == 0) 
    {
        return 0;
    }

    if (allocation_count != 0 && best_address == next_address)
    {
        allocations[allocation_count - 1].length += (uint32_t)bytes;
    }
    else
    {
        if (allocation_count == MEMORY_MAX_ALLOCATIONS)
        {
            return 0;
        }
        allocations[allocation_count++] = (struct memory_allocation) {
            (uint32_t)best_address, (uint32_t)bytes
        };
    }
    next_address = best_address + bytes;
    return (uint32_t)best_address;
}
