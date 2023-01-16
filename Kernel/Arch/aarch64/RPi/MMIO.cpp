/*
 * Copyright (c) 2021, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Kernel/Arch/aarch64/MainIdRegister.h>
#include <Kernel/Arch/aarch64/RPi/MMIO.h>
#include <Kernel/Memory/MemoryManager.h>

namespace Kernel::RPi {

MMIO::MMIO()
    : m_base_address(0xFE00'0000)
{
    MainIdRegister id;
    if (id.part_num() <= MainIdRegister::RaspberryPi3)
        m_base_address = 0x3F00'0000;
}

MMIO& MMIO::the()
{
    static MMIO instance;
    return instance;
}

ErrorOr<NonnullOwnPtr<Memory::Region>> MMIO::map_peripheral(FlatPtr offset, StringView name)
{
    return MM.allocate_kernel_region(PhysicalAddress(m_base_address + offset), PAGE_SIZE, name, Memory::Region::Access::ReadWrite);
}

}
