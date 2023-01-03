/*
 * Copyright (c) 2018-2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2018-2022, James Mintram <me@jamesrm.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Kernel/Arch/aarch64/ASM_wrapper.h>
#include <Kernel/Memory/PageDirectory.h>
#include <AK/Singleton.h>
#include <Kernel/Locking/SpinlockProtected.h>
#include <Kernel/Thread.h>

namespace Kernel::Memory {

struct CR3Map {
    SpinlockProtected<IntrusiveRedBlackTree<&PageDirectory::m_tree_node>> map { LockRank::None };
};

static Singleton<CR3Map> s_cr3_map;


void PageDirectory::register_page_directory(PageDirectory* directory)
{
    s_cr3_map->map.with([&](auto& map) {
        map.insert(directory->cr3(), *directory);
    });
    // dbgln("FIXME: PageDirectory: Actually implement registering a page directory!");
}

void PageDirectory::deregister_page_directory(PageDirectory* directory)
{
    s_cr3_map->map.with([&](auto& map) {
        map.remove(directory->cr3());
    });
    // TODO_AARCH64();
}

LockRefPtr<PageDirectory> PageDirectory::find_current()
{
        return s_cr3_map->map.with([&](auto& map) {
        return map.find(Aarch64::Asm::get_ttbr0_el1());
    });
}

void activate_kernel_page_directory(PageDirectory const& page_directory)
{
    Aarch64::Asm::set_ttbr0_el1(page_directory.cr3());
    // (void)page_directory;
    dbgln("PageDirectory: Activating kernel page directory: 0x{:x}!", page_directory.cr3());
}

void activate_page_directory(PageDirectory const& page_directory, Thread* current_thread)
{
    current_thread->regs().set_page_table_base_pointer(page_directory.cr3());
    Aarch64::Asm::set_ttbr0_el1(page_directory.cr3());
    // (void)page_directory;
    dbgln("FIXME: PageDirectory: Actually activate page directory for Thread!");
}

}
