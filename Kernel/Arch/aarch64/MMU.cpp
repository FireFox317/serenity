/*
 * Copyright (c) 2021, James Mintram <me@jamesrm.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause

 */

// __stack_chk_guard
#pragma GCC push_options
#pragma GCC optimize("-fno-stack-protector")

#include <AK/Types.h>

#include <Kernel/Arch/aarch64/CPU.h>

#include <Kernel/Arch/PageDirectory.h>
#include <Kernel/Arch/aarch64/ASM_wrapper.h>
#include <Kernel/Arch/aarch64/RPi/MMIO.h>
#include <Kernel/Arch/aarch64/RPi/UART.h>
#include <Kernel/Arch/aarch64/Registers.h>
#include <Kernel/BootInfo.h>
#include <Kernel/Panic.h>
#include <Kernel/Sections.h>

// Documentation here for Aarch64 Address Translations
// https://documentation-service.arm.com/static/5efa1d23dbdee951c1ccdec5?token=

// These come from the linker script
extern u8 page_tables_phys_start[];
extern u8 page_tables_phys_end[];
extern u8 start_of_kernel_image[];
extern u8 end_of_kernel_image[];

namespace Kernel {

// physical memory
constexpr u32 START_OF_NORMAL_MEMORY = 0x00000000;
constexpr u32 END_OF_NORMAL_MEMORY = 0x3EFFFFFF;

ALWAYS_INLINE static u64* descriptor_to_pointer(FlatPtr descriptor)
{
    return (u64*)(descriptor & DESCRIPTOR_MASK);
}

namespace {
class PageBumpAllocator {
public:
    PageBumpAllocator(u64* start, u64* end)
        : m_start(start)
        , m_end(end)
        , m_current(start)
    {
        if (m_start >= m_end) {
            // PANIC("Invalid memory range passed to PageBumpAllocator");
        }
        if ((FlatPtr)m_start % PAGE_TABLE_SIZE != 0 || (FlatPtr)m_end % PAGE_TABLE_SIZE != 0) {
            // PANIC("Memory range passed into PageBumpAllocator not aligned to PAGE_TABLE_SIZE");
        }
    }

    u64* take_page()
    {
        if (m_current == m_end) {
            // PANIC("Prekernel pagetable memory exhausted");
        }

        u64* page = m_current;
        m_current += (PAGE_TABLE_SIZE / sizeof(FlatPtr));

        zero_page(page);
        return page;
    }

private:
    void zero_page(u64* page)
    {
        // Memset all page table memory to zero
        for (u64* p = page; p < page + (PAGE_TABLE_SIZE / sizeof(u64)); p++) {
            *p = 0;
        }
    }

    u64 const* m_start;
    u64 const* m_end;
    u64* m_current;
};
}

template<typename T>
static T* adjust_by_mapping_base(T* ptr)
{
    return (T*)((FlatPtr)ptr - 0x2000000000);
}

template<typename T>
static T* physical_to_virtual(T* ptr)
{
    return (T*)((FlatPtr)ptr + 0x2000000000);
}

// static u64* get_page_table(u64* page_table, VirtualAddress virtual_addr)
// {
//     // Each level has 9 bits (512 entries)
//     u64 level0_idx = (virtual_addr.get() >> 39) & 0x1FF;
//     u64 level1_idx = (virtual_addr.get() >> 30) & 0x1FF;
//     u64 level2_idx = (virtual_addr.get() >> 21) & 0x1FF;

//     u64* level1_table = page_table;

//     if (level1_table[level0_idx] == 0)
//         return nullptr;

//     u64* level2_table = physical_to_virtual(descriptor_to_pointer(level1_table[level0_idx]));

//     if (level2_table[level1_idx] == 0)
//         return nullptr;

//     u64* level3_table = physical_to_virtual(descriptor_to_pointer(level2_table[level1_idx]));

//     if (level3_table[level2_idx] == 0)
//         return nullptr;

//     return physical_to_virtual(descriptor_to_pointer(level3_table[level2_idx]));
// }

static u64* insert_page_table(PageBumpAllocator& allocator, u64* page_table, VirtualAddress virtual_addr)
{
    // Each level has 9 bits (512 entries)
    u64 level0_idx = (virtual_addr.get() >> 39) & 0x1FF;
    u64 level1_idx = (virtual_addr.get() >> 30) & 0x1FF;
    u64 level2_idx = (virtual_addr.get() >> 21) & 0x1FF;

    u64* level1_table = page_table;

    if (level1_table[level0_idx] == 0) {
        level1_table[level0_idx] = (FlatPtr)allocator.take_page();
        level1_table[level0_idx] |= TABLE_DESCRIPTOR;
    }

    u64* level2_table = descriptor_to_pointer(level1_table[level0_idx]);

    if (level2_table[level1_idx] == 0) {
        level2_table[level1_idx] = (FlatPtr)allocator.take_page();
        level2_table[level1_idx] |= TABLE_DESCRIPTOR;
    }

    u64* level3_table = descriptor_to_pointer(level2_table[level1_idx]);

    if (level3_table[level2_idx] == 0) {
        level3_table[level2_idx] = (FlatPtr)allocator.take_page();
        level3_table[level2_idx] |= TABLE_DESCRIPTOR;
    }

    return descriptor_to_pointer(level3_table[level2_idx]);
}

static void map_page(PageBumpAllocator& allocator, u64* page_table, VirtualAddress vaddr, PhysicalAddress paddr, u64 flags)
{
    u64* level4_table = insert_page_table(allocator, page_table, vaddr);
    u64 level3_idx = (vaddr.get() >> 12) & 0x1FF;
    u64* l4_entry = &level4_table[level3_idx];
    *l4_entry = paddr.get();
    *l4_entry |= flags;
}

static void insert_identity_entries_for_physical_memory_range(PageBumpAllocator& allocator, u64* page_table, FlatPtr start, FlatPtr end, u64 flags)
{
    // Not very efficient, but simple and it works.
    for (FlatPtr addr = start; addr < end; addr += GRANULE_SIZE) {
        u64* level4_table = insert_page_table(allocator, page_table, VirtualAddress { addr });

        u64 level3_idx = (addr >> 12) & 0x1FF;
        u64* l4_entry = &level4_table[level3_idx];
        *l4_entry = addr;
        *l4_entry |= flags;
    }
}

static void setup_quickmap_page_table(PageBumpAllocator& allocator, u64* root_table)
{
    // FIXME: Rename boot_pd_kernel_pt1023 to quickmap_page_table
    // FIXME: Rename KERNEL_PT1024_BASE to quickmap_page_table_address
    // KERNEL_PT1024_BASE
    *adjust_by_mapping_base(&boot_pd_kernel_pt1023) = physical_to_virtual((PageTableEntry*)insert_page_table(allocator, root_table, VirtualAddress { (0x2000000000 + 0x3FE00000) }));
}

static void map_kernel_high_memory(PageBumpAllocator& allocator, u64* root_table)
{
    u64 normal_memory_flags = ACCESS_FLAG | PAGE_DESCRIPTOR | INNER_SHAREABLE | NORMAL_MEMORY;
    u64 device_memory_flags = ACCESS_FLAG | PAGE_DESCRIPTOR | OUTER_SHAREABLE | DEVICE_MEMORY;

    FlatPtr start_of_range = (((FlatPtr)start_of_kernel_image) & ~(FlatPtr)0x1fffff);
    FlatPtr end_of_range = (((FlatPtr)end_of_kernel_image) & ~(FlatPtr)0x1fffff) + 0x200000 - 1;

    for (FlatPtr addr = start_of_range; addr < end_of_range; addr += GRANULE_SIZE) {
        map_page(allocator, root_table, VirtualAddress { addr }, PhysicalAddress { addr - 0x2000000000 }, normal_memory_flags);
    }

    for (FlatPtr addr = 0x2000000000 + 0x3F000000; addr < 0x2000000000 + 0x3F000000 + 0x00FFFFFF; addr += GRANULE_SIZE) {
        map_page(allocator, root_table, VirtualAddress { addr }, PhysicalAddress { addr - 0x2000000000 }, device_memory_flags);
    }
}

static void build_identity_map(PageBumpAllocator& allocator, u64* root_table)
{
    u64 normal_memory_flags = ACCESS_FLAG | PAGE_DESCRIPTOR | INNER_SHAREABLE | NORMAL_MEMORY;
    u64 device_memory_flags = ACCESS_FLAG | PAGE_DESCRIPTOR | OUTER_SHAREABLE | DEVICE_MEMORY;

    // Align the identity mapping of the kernel image to 2 MiB, the rest of the memory is initially not mapped.
    FlatPtr start_of_range = (((FlatPtr)start_of_kernel_image - 0x2000000000) & ~(FlatPtr)0x1fffff);
    FlatPtr end_of_range = (((FlatPtr)end_of_kernel_image - 0x2000000000) & ~(FlatPtr)0x1fffff) + 0x200000 - 1;

    insert_identity_entries_for_physical_memory_range(allocator, root_table, start_of_range, end_of_range, normal_memory_flags);
    // insert_identity_entries_for_physical_memory_range(allocator, root_table, RPi::MMIO::the().peripheral_base_address(), RPi::MMIO::the().peripheral_end_address(), device_memory_flags);
    insert_identity_entries_for_physical_memory_range(allocator, root_table, 0x3F000000, 0x3F000000 + 0x00FFFFFF, device_memory_flags);
}

static void switch_to_page_table(u8* page_table)
{
    Aarch64::Asm::set_ttbr0_el1((FlatPtr)page_table);
    Aarch64::Asm::set_ttbr1_el1((FlatPtr)page_table);
}

static void activate_mmu()
{
    Aarch64::MAIR_EL1 mair_el1 = {};
    mair_el1.Attr[0] = 0xFF;       // Normal memory
    mair_el1.Attr[1] = 0b00000100; // Device-nGnRE memory (non-cacheble)
    Aarch64::MAIR_EL1::write(mair_el1);

    // Configure cacheability attributes for memory associated with translation table walks
    Aarch64::TCR_EL1 tcr_el1 = {};

    tcr_el1.SH1 = Aarch64::TCR_EL1::InnerShareable;
    tcr_el1.ORGN1 = Aarch64::TCR_EL1::NormalMemory_Outer_WriteBack_ReadAllocate_WriteAllocateCacheable;
    tcr_el1.IRGN1 = Aarch64::TCR_EL1::NormalMemory_Inner_WriteBack_ReadAllocate_WriteAllocateCacheable;
    tcr_el1.T1SZ = 16;

    tcr_el1.SH0 = Aarch64::TCR_EL1::InnerShareable;
    tcr_el1.ORGN0 = Aarch64::TCR_EL1::NormalMemory_Outer_WriteBack_ReadAllocate_WriteAllocateCacheable;
    tcr_el1.IRGN0 = Aarch64::TCR_EL1::NormalMemory_Inner_WriteBack_ReadAllocate_WriteAllocateCacheable;
    tcr_el1.T0SZ = 16;

    tcr_el1.TG1 = Aarch64::TCR_EL1::TG1GranuleSize::Size_4KB;
    tcr_el1.TG0 = Aarch64::TCR_EL1::TG0GranuleSize::Size_4KB;

    // Auto detect the Intermediate Physical Address Size
    Aarch64::ID_AA64MMFR0_EL1 feature_register = Aarch64::ID_AA64MMFR0_EL1::read();
    tcr_el1.IPS = feature_register.PARange;

    Aarch64::TCR_EL1::write(tcr_el1);

    // Enable MMU in the system control register
    Aarch64::SCTLR_EL1 sctlr_el1 = Aarch64::SCTLR_EL1::read();
    sctlr_el1.M = 1; // Enable MMU
    Aarch64::SCTLR_EL1::write(sctlr_el1);

    Aarch64::Asm::flush();
}

static u64* get_page_directory(u64* root_table, VirtualAddress virtual_addr)
{
    u64 level0_idx = (virtual_addr.get() >> 39) & 0x1FF;
    u64 level1_idx = (virtual_addr.get() >> 30) & 0x1FF;

    u64* level1_table = root_table;

    if (level1_table[level0_idx] == 0)
        return nullptr;

    u64* level2_table = descriptor_to_pointer(level1_table[level0_idx]);

    if (level2_table[level1_idx] == 0)
        return nullptr;

    return descriptor_to_pointer(level2_table[level1_idx]);
}

static u64* get_page_directory_table(u64* root_table, VirtualAddress virtual_addr)
{
    u64 level0_idx = (virtual_addr.get() >> 39) & 0x1FF;

    u64* level1_table = root_table;

    if (level1_table[level0_idx] == 0)
        return nullptr;

    return descriptor_to_pointer(level1_table[level0_idx]);
}

static void setup_kernel_page_directory(u64* root_table)
{
    *adjust_by_mapping_base(&boot_pd_kernel) = PhysicalAddress((PhysicalPtr)get_page_directory(root_table, VirtualAddress { 0x2000000000 }));
    VERIFY(!(*adjust_by_mapping_base(&boot_pd_kernel)).is_null());

    // FIXME: Rename boot_pml4t to something architecture agnostic.
    *adjust_by_mapping_base(&boot_pml4t) = PhysicalAddress((PhysicalPtr)root_table);

    // FIXME: Rename to directory_table or similar
    *adjust_by_mapping_base(&boot_pdpt) = PhysicalAddress((PhysicalPtr)get_page_directory_table(root_table, VirtualAddress { 0x2000000000 }));
}

void init_page_tables()
{
    // We currently identity map the physical memory, so the offset is 0.
    *adjust_by_mapping_base(&physical_to_virtual_offset) = 0x2000000000;
    *adjust_by_mapping_base(&kernel_mapping_base) = 0x2000000000;

    PageBumpAllocator allocator(adjust_by_mapping_base((u64*)page_tables_phys_start), adjust_by_mapping_base((u64*)page_tables_phys_end));
    auto root_table = allocator.take_page();
    build_identity_map(allocator, root_table);
    map_kernel_high_memory(allocator, root_table);
    setup_quickmap_page_table(allocator, root_table);
    setup_kernel_page_directory(root_table);

    switch_to_page_table(adjust_by_mapping_base(page_tables_phys_start));
    activate_mmu();
}

void unmap_identity_map_kernel()
{
    // get_page_directory_table()
    // FlatPtr start_of_range = (((FlatPtr)start_of_kernel_image - 0x2000000000) & ~(FlatPtr)0x1fffff);
    // FlatPtr end_of_range = (((FlatPtr)end_of_kernel_image - 0x2000000000) & ~(FlatPtr)0x1fffff) + 0x200000 - 1;

    // for (FlatPtr addr = start_of_range; addr < end_of_range; addr += GRANULE_SIZE) {
    //     VirtualAddress vaddr {addr};
    //     u64* level4_table = get_page_table((u64*)page_tables_phys_start, vaddr);
    //     u64 level3_idx = (vaddr.get() >> 12) & 0x1FF;
    //     u64* l4_entry = &level4_table[level3_idx];
    //     *l4_entry = 0;
    // }
    // Each level has 9 bits (512 entries)
    VirtualAddress virtual_addr { (FlatPtr)0 };

    u64 level0_idx = (virtual_addr.get() >> 39) & 0x1FF;
    u64 level1_idx = (virtual_addr.get() >> 30) & 0x1FF;

    u64* level1_table = (u64*)page_tables_phys_start;

    u64* level2_table = physical_to_virtual(descriptor_to_pointer(level1_table[level0_idx]));

    level2_table[level1_idx] = 0;
}

}

#pragma GCC pop_options
