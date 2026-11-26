#ifndef NYOS_BIOS_LIMINE_H
#define NYOS_BIOS_LIMINE_H

#include "memory.h"

struct elf64_image
{
    uint64_t entry;
    uint64_t virtual_base;
    uint32_t physical_base;
    uint32_t size;
};

enum limine_result
{
    LIMINE_LEGACY,
    LIMINE_READY,
    LIMINE_INVALID
};

bool bios_map_page(uint32_t root, uint64_t virt, uint32_t phys, uint64_t flags);
enum limine_result limine_prepare(const struct elf64_image* image,
                                 const struct e820_entry* map, uint32_t count,
                                 uint32_t pml4, uint64_t* stack_top);

#endif
