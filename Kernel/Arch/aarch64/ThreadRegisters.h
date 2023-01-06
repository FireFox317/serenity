/*
 * Copyright (c) 2022, Timon Kruiper <timonkruiper@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <Kernel/Memory/AddressSpace.h>

namespace Kernel {

struct ThreadRegisters {
    u64 x[31];
    u64 spsr_el1;
    u64 elr_el1;
    u64 sp_el0;
    u64 ttbr0_el1;

    FlatPtr ip() const { return elr_el1; }
    void set_ip(FlatPtr value) { elr_el1 = value; }

    void set_sp(FlatPtr value) { sp_el0 = value; }

    void set_page_table_base_pointer(FlatPtr value) { ttbr0_el1 = value; }

    void set_initial_state(bool is_kernel_process, Memory::AddressSpace& space, FlatPtr kernel_stack_top)
    {
        set_sp(kernel_stack_top);

        ttbr0_el1 = space.page_directory().cr3();

        set_spsr_el1(is_kernel_process);
    }

    void set_spsr_el1(bool is_kernel_process)
    {
        Aarch64::SPSR_EL1 saved_program_status_register_el1 = {};

        // Don't mask any interrupts, so all interrupts are enabled when transfering into the new context
        saved_program_status_register_el1.D = 0;
        saved_program_status_register_el1.A = 0;
        saved_program_status_register_el1.I = 0;
        saved_program_status_register_el1.F = 0;

        // Set exception origin mode to EL1t, so when the context is restored, we'll be executing in EL1 with SP_EL0
        // FIXME: This must be EL0t when aarch64 supports userspace applications.
        saved_program_status_register_el1.M = is_kernel_process ? Aarch64::SPSR_EL1::Mode::EL1h : Aarch64::SPSR_EL1::Mode::EL1t;
        memcpy(&spsr_el1, &saved_program_status_register_el1, sizeof(u64));
    }

    void set_entry_function(FlatPtr entry_ip, FlatPtr entry_data)
    {
        set_ip(entry_ip);
        x[0] = entry_data;
    }
};

}
