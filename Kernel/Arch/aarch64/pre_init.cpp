/*
 * Copyright (c) 2022, Timon Kruiper <timonkruiper@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <AK/Types.h>
#include <Kernel/Arch/aarch64/CPU.h>

// We arrive here from boot.S with the MMU disabled and in an unknown exception level (EL).
// The kernel is linked at the virtual address, so we have to be really carefull when accessing
// global variables, as the MMU is not yet enabled.

// TODO: Rename to Prekernel?

namespace Kernel {

template<typename T>
static T* adjust_by_mapping_base(T* ptr)
{
    return (T*)((FlatPtr)ptr - 0x2000000000);
}

int test_variable { 0 };

static void send(StringView string)
{
    for (auto c : string)
        *((u32*)(0x3F000000 + 0x201000)) = c;
}

extern "C" [[noreturn]] void init();

extern "C" [[noreturn]] void pre_init();
extern "C" [[noreturn]] void pre_init()
{
    *adjust_by_mapping_base(&test_variable) = 12;

    send("Hello\n"sv);

    drop_to_exception_level_1();

    init_page_tables();

    // At this point the MMU is enabled, physical memory is identity mapped,
    // and the kernel is also mapped into higher memory (kernel_base), however we are still executing
    // from the physical memory address, so we have to jump to the kernel in high memory, and also
    // switch the stack pointer to high memory, such that we can unmap the physical memory.
    send("Nice\n"sv);

    // Continue execution at high virtual address
    asm volatile(
        "ldr x0, =1f \n"
        "br x0 \n"
        "1: \n" ::
            : "x0");

    send("Nice 1\n"sv);

    // "mov x0, %[base] \n"
    asm volatile(
        "mov x0, %[base] \n"
        "add sp, sp, x0 \n" ::[base] "r"(0x2000000000)
        : "x0");

    send("Nice 2\n"sv);

    unmap_identity_map_kernel();

    // send("Nice 3\n"sv);

    asm volatile(
        "msr SPSel, #1 \n" // Switch to SP_EL1, which is set to 0x2000040000 in drop_to_el1 in Exceptions.cpp.
        "mov x29, xzr \n"
        "mov x30, xzr \n"
        "b init \n");

    while (true) { }
}

}
