/*
 * Copyright (c) 2022, Liav A. <liavalb@hotmail.co.il>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Kernel/Arch/DebugOutput.h>
#include <Kernel/Arch/aarch64/RPi/UART.h>
#include <Kernel/Memory/MemoryManager.h>

namespace Kernel {

void debug_output(char ch)
{
    if (Memory::MemoryManager::is_initialized())
        RPi::UART::the().send(ch);
}

}
