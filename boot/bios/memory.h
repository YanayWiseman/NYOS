#ifndef BIOS_MEMORY_H
#define BIOS_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct e820_entry 
{
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attributes;
};

_Static_assert(sizeof(struct e820_entry) == 24, "E820 entries must occupy 24 bytes");
_Static_assert(offsetof(struct e820_entry, type) == 16, "Unexpected E820 type offset");
_Static_assert(offsetof(struct e820_entry, attributes) == 20, "Unexpected E820 attributes offset");

bool memory_init(const struct e820_entry* map, uint32_t count);
uint32_t memory_alloc_pages(uint32_t pages);

#endif