/*
 * Copyright (c) 2018-2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2018-2022, James Mintram <me@jamesrm.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Singleton.h>
#include <Kernel/Arch/aarch64/ASM_wrapper.h>
#include <Kernel/Memory/PageDirectory.h>
#include <Kernel/Thread.h>

namespace Kernel::Memory {

struct TTBR0Map {
    SpinlockProtected<IntrusiveRedBlackTree<&PageDirectory::m_tree_node>, LockRank::None> map {};
};

static Singleton<TTBR0Map> s_ttbr0_map;

void PageDirectory::register_page_directory(PageDirectory* directory)
{
    s_ttbr0_map->map.with([&](auto& map) {
        map.insert(directory->cr3(), *directory);
    });
}

void PageDirectory::deregister_page_directory(PageDirectory* directory)
{
    s_ttbr0_map->map.with([&](auto& map) {
        map.remove(directory->cr3());
    });
}

LockRefPtr<PageDirectory> PageDirectory::find_current()
{
    return s_ttbr0_map->map.with([&](auto& map) {
        return map.find(Aarch64::Asm::get_ttbr0_el1());
    });
}

void activate_kernel_page_directory(PageDirectory const& page_directory)
{
    Aarch64::Asm::set_ttbr0_el1(page_directory.cr3());
}

void activate_page_directory(PageDirectory const& page_directory, Thread* current_thread)
{
    current_thread->regs().ttbr0_el1 = page_directory.cr3();
    Aarch64::Asm::set_ttbr0_el1(page_directory.cr3());
}

}
