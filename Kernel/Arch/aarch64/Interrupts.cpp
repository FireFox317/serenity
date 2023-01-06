/*
 * Copyright (c) 2022, Timon Kruiper <timonkruiper@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Kernel/Arch/CPU.h>
#include <Kernel/Arch/Interrupts.h>
#include <Kernel/Arch/PageFault.h>
#include <Kernel/Arch/TrapFrame.h>
#include <Kernel/Arch/aarch64/InterruptManagement.h>
#include <Kernel/Interrupts/GenericInterruptHandler.h>
#include <Kernel/Interrupts/SharedIRQHandler.h>
#include <Kernel/Interrupts/UnhandledInterruptHandler.h>
#include <Kernel/KSyms.h>
#include <Kernel/Panic.h>
#include <Kernel/PerformanceManager.h>
#include <Kernel/Thread.h>

#include <LibC/mallocdefs.h>

namespace Kernel {

extern "C" void syscall_handler(TrapFrame const*) __attribute__((used));

static Array<GenericInterruptHandler*, 64> s_interrupt_handlers;

extern "C" void handle_interrupt(TrapFrame&);
extern "C" void handle_interrupt(TrapFrame& trap_frame)
{
    Processor::current().enter_trap(trap_frame, true);

    for (auto& interrupt_controller : InterruptManagement::the().controllers()) {
        auto pending_interrupts = interrupt_controller->pending_interrupts();

        // TODO: Add these interrupts as a source of entropy for randomness.
        u8 irq = 0;
        while (pending_interrupts) {
            if ((pending_interrupts & 0b1) != 0b1) {
                irq += 1;
                pending_interrupts >>= 1;
                continue;
            }

            auto* handler = s_interrupt_handlers[irq];
            VERIFY(handler);
            handler->increment_call_count();
            handler->handle_interrupt(*trap_frame.regs);
            handler->eoi();

            irq += 1;
            pending_interrupts >>= 1;
        }
    }

    Processor::current().exit_trap(trap_frame);
}

static u16 convert_esr_to_exception_code(Kernel::Aarch64::ESR_EL1 esr)
{
    u16 exception_code = 0;
    // TODO: This is also valid for Instruction Fault Status Code
    u8 data_fault_status_code = esr.ISS & 0x3f;
    if (data_fault_status_code >= 0b001100 && data_fault_status_code <= 0b001111) {
        // Permission fault
        exception_code |= 0b1;
    }
    if (data_fault_status_code >= 0b000100 && data_fault_status_code <= 0b000111) {
        // Translation fault
        exception_code &= ~0b1;
    }

    if (Aarch64::exception_class_is_data_abort(esr.EC)) {
        u8 is_write = (esr.ISS & (1 << 6)) == (1 << 6);
        if (is_write)
            exception_code |= 0b10;
    }

    if (Aarch64::exception_class_is_instruction_abort(esr.EC)) {
        exception_code |= 0x10;
    }

    return exception_code;
}

static void page_fault_handler(TrapFrame const* trap_frame, Aarch64::ESR_EL1 esr_el1)
{
    auto fault_address = Aarch64::FAR_EL1::read().virtual_address;
    auto& regs = *trap_frame->regs;

    auto current_thread = Thread::current();

    if (current_thread) {
        current_thread->set_handling_page_fault(true);
        PerformanceManager::add_page_fault_event(*current_thread, regs);
    }

    ScopeGuard guard = [current_thread] {
        if (current_thread)
            current_thread->set_handling_page_fault(false);
    };

    // dbgln("Fault address: {} at: 0x{:x}", fault_address, regs.elr_el1);

    auto exception_code = convert_esr_to_exception_code(esr_el1);
    PageFault fault { exception_code, VirtualAddress { fault_address } };
    auto response = MM.handle_page_fault(fault);

    if (response == PageFaultResponse::ShouldCrash || response == PageFaultResponse::OutOfMemory || response == PageFaultResponse::BusError) {
        // if (faulted_in_kernel && handle_safe_access_fault(regs, fault_address)) {
        //     // If this would be a ring0 (kernel) fault and the fault was triggered by
        //     // safe_memcpy, safe_strnlen, or safe_memset then we resume execution at
        //     // the appropriate _fault label rather than crashing
        //     return;
        // }

        if (response == PageFaultResponse::BusError && current_thread->has_signal_handler(SIGBUS)) {
            current_thread->send_urgent_signal_to_self(SIGBUS);
            return;
        }

        if (response != PageFaultResponse::OutOfMemory && current_thread) {
            if (current_thread->has_signal_handler(SIGSEGV)) {
                current_thread->send_urgent_signal_to_self(SIGSEGV);
                return;
            }
        }

        dbgln("elr_el1: {:#x}", regs.elr_el1);
        dump_backtrace_from_base_pointer(regs.x[29]);

        dbgln("Unrecoverable page fault, {}{}{} address {}",
            exception_code & PageFaultFlags::ReservedBitViolation ? "reserved bit violation / " : "",
            exception_code & PageFaultFlags::InstructionFetch ? "instruction fetch / " : "",
            exception_code & PageFaultFlags::Write ? "write to" : "read from",
            VirtualAddress(fault_address));
        constexpr FlatPtr malloc_scrub_pattern = explode_byte(MALLOC_SCRUB_BYTE);
        constexpr FlatPtr free_scrub_pattern = explode_byte(FREE_SCRUB_BYTE);
        constexpr FlatPtr kmalloc_scrub_pattern = explode_byte(KMALLOC_SCRUB_BYTE);
        constexpr FlatPtr kfree_scrub_pattern = explode_byte(KFREE_SCRUB_BYTE);
        if (response == PageFaultResponse::BusError) {
            dbgln("Note: Address {} is an access to an undefined memory range of an Inode-backed VMObject", VirtualAddress(fault_address));
        } else if ((fault_address & 0xffff0000) == (malloc_scrub_pattern & 0xffff0000)) {
            dbgln("Note: Address {} looks like it may be uninitialized malloc() memory", VirtualAddress(fault_address));
        } else if ((fault_address & 0xffff0000) == (free_scrub_pattern & 0xffff0000)) {
            dbgln("Note: Address {} looks like it may be recently free()'d memory", VirtualAddress(fault_address));
        } else if ((fault_address & 0xffff0000) == (kmalloc_scrub_pattern & 0xffff0000)) {
            dbgln("Note: Address {} looks like it may be uninitialized kmalloc() memory", VirtualAddress(fault_address));
        } else if ((fault_address & 0xffff0000) == (kfree_scrub_pattern & 0xffff0000)) {
            dbgln("Note: Address {} looks like it may be recently kfree()'d memory", VirtualAddress(fault_address));
        } else if (fault_address < 4096) {
            dbgln("Note: Address {} looks like a possible nullptr dereference", VirtualAddress(fault_address));
        } else if constexpr (SANITIZE_PTRS) {
            constexpr FlatPtr refptr_scrub_pattern = explode_byte(REFPTR_SCRUB_BYTE);
            constexpr FlatPtr nonnullrefptr_scrub_pattern = explode_byte(NONNULLREFPTR_SCRUB_BYTE);
            constexpr FlatPtr ownptr_scrub_pattern = explode_byte(OWNPTR_SCRUB_BYTE);
            constexpr FlatPtr nonnullownptr_scrub_pattern = explode_byte(NONNULLOWNPTR_SCRUB_BYTE);
            constexpr FlatPtr lockrefptr_scrub_pattern = explode_byte(LOCKREFPTR_SCRUB_BYTE);
            constexpr FlatPtr nonnulllockrefptr_scrub_pattern = explode_byte(NONNULLLOCKREFPTR_SCRUB_BYTE);

            if ((fault_address & 0xffff0000) == (refptr_scrub_pattern & 0xffff0000)) {
                dbgln("Note: Address {} looks like it may be a recently destroyed LockRefPtr", VirtualAddress(fault_address));
            } else if ((fault_address & 0xffff0000) == (nonnullrefptr_scrub_pattern & 0xffff0000)) {
                dbgln("Note: Address {} looks like it may be a recently destroyed NonnullLockRefPtr", VirtualAddress(fault_address));
            } else if ((fault_address & 0xffff0000) == (ownptr_scrub_pattern & 0xffff0000)) {
                dbgln("Note: Address {} looks like it may be a recently destroyed OwnPtr", VirtualAddress(fault_address));
            } else if ((fault_address & 0xffff0000) == (nonnullownptr_scrub_pattern & 0xffff0000)) {
                dbgln("Note: Address {} looks like it may be a recently destroyed NonnullOwnPtr", VirtualAddress(fault_address));
            } else if ((fault_address & 0xffff0000) == (lockrefptr_scrub_pattern & 0xffff0000)) {
                dbgln("Note: Address {} looks like it may be a recently destroyed LockRefPtr", VirtualAddress(fault_address));
            } else if ((fault_address & 0xffff0000) == (nonnulllockrefptr_scrub_pattern & 0xffff0000)) {
                dbgln("Note: Address {} looks like it may be a recently destroyed NonnullLockRefPtr", VirtualAddress(fault_address));
            }
        }

        if (current_thread) {
            auto& current_process = current_thread->process();
            if (current_process.is_user_process()) {
                auto fault_address_string = KString::formatted("{:p}", fault_address);
                auto fault_address_view = fault_address_string.is_error() ? ""sv : fault_address_string.value()->view();
                (void)current_process.try_set_coredump_property("fault_address"sv, fault_address_view);
                (void)current_process.try_set_coredump_property("fault_type"sv, fault.type() == PageFault::Type::PageNotPresent ? "NotPresent"sv : "ProtectionViolation"sv);
                StringView fault_access;
                if (fault.is_instruction_fetch())
                    fault_access = "Execute"sv;
                else
                    fault_access = fault.access() == PageFault::Access::Read ? "Read"sv : "Write"sv;
                (void)current_process.try_set_coredump_property("fault_access"sv, fault_access);
            }
        }

        if (response == PageFaultResponse::BusError)
            return handle_crash(regs, "Page Fault (Bus Error)", SIGBUS, false);
        return handle_crash(regs, "Page Fault", SIGSEGV, response == PageFaultResponse::OutOfMemory);
    } else if (response == PageFaultResponse::Continue) {
        dbgln_if(PAGE_FAULT_DEBUG, "Continuing after resolved page fault");
    } else {
        VERIFY_NOT_REACHED();
    }
}

extern "C" void exception_common(Kernel::TrapFrame* trap_frame);
extern "C" void exception_common(Kernel::TrapFrame* trap_frame)
{
    Processor::current().enter_trap(*trap_frame, false);

    constexpr bool print_stack_frame = false;
    if constexpr (print_stack_frame) {
        dbgln("Exception Generated by processor!");

        auto* regs = trap_frame->regs;

        dbgln(" x0={:p}  x1={:p}  x2={:p}  x3={:p}  x4={:p}", regs->x[0], regs->x[1], regs->x[2], regs->x[3], regs->x[4]);
        dbgln(" x5={:p}  x6={:p}  x7={:p}  x8={:p}  x9={:p}", regs->x[5], regs->x[6], regs->x[7], regs->x[8], regs->x[9]);
        dbgln("x10={:p} x11={:p} x12={:p} x13={:p} x14={:p}", regs->x[10], regs->x[11], regs->x[12], regs->x[13], regs->x[14]);
        dbgln("x15={:p} x16={:p} x17={:p} x18={:p} x19={:p}", regs->x[15], regs->x[16], regs->x[17], regs->x[18], regs->x[19]);
        dbgln("x20={:p} x21={:p} x22={:p} x23={:p} x24={:p}", regs->x[20], regs->x[21], regs->x[22], regs->x[23], regs->x[24]);
        dbgln("x25={:p} x26={:p} x27={:p} x28={:p} x29={:p}", regs->x[25], regs->x[26], regs->x[27], regs->x[28], regs->x[29]);
        dbgln("x30={:p}", regs->x[30]);

        // Special registers
        Aarch64::SPSR_EL1 spsr_el1 = {};
        memcpy(&spsr_el1, (u8*)&regs->spsr_el1, sizeof(u64));

        dbgln("spsr_el1: {:#x} (NZCV({:#b}) DAIF({:#b}) M({:#b}))", regs->spsr_el1, ((regs->spsr_el1 >> 28) & 0b1111), ((regs->spsr_el1 >> 6) & 0b1111), regs->spsr_el1 & 0b1111);
        dbgln("elr_el1: {:#x}", regs->elr_el1);
        dbgln("tpidr_el0: {:#x}", regs->tpidr_el0);
        dbgln("sp_el0: {:#x}", regs->sp_el0);

        auto esr_el1 = Kernel::Aarch64::ESR_EL1::read();
        dbgln("esr_el1: EC({:#b}) IL({:#b}) ISS({:#b}) ISS2({:#b})", esr_el1.EC, esr_el1.IL, esr_el1.ISS, esr_el1.ISS2);
        dbgln("Exception Class: {}", Aarch64::exception_class_to_string(esr_el1.EC));
        if (Aarch64::exception_class_has_set_far(esr_el1.EC))
            dbgln("Faulting Virtual Address: 0x{:x}", Aarch64::FAR_EL1::read().virtual_address);

        if (Aarch64::exception_class_is_data_abort(esr_el1.EC))
            dbgln("Data Fault Status Code: {}", Aarch64::data_fault_status_code_to_string(esr_el1.ISS));

        auto ip = regs->elr_el1;
        auto const* symbol = symbolicate_kernel_address(ip);
        dbgln("\033[31;1m{:p}  {} +{}\033[0m", ip, (symbol ? symbol->name : "(k?)"), (symbol ? ip - symbol->address : 0));

        // dump_backtrace_from_base_pointer(regs->x[29]);
    }

    auto esr_el1 = Kernel::Aarch64::ESR_EL1::read();

    if (Aarch64::exception_class_is_svc_instruction_execution(esr_el1.EC)) {
        // Syscall!
        syscall_handler(trap_frame);

    } else if (Aarch64::exception_class_is_data_abort(esr_el1.EC) || Aarch64::exception_class_is_instruction_abort(esr_el1.EC)) {

        page_fault_handler(trap_frame, esr_el1);
    } else {
        // unexpected, print backtrace etc
        PANIC("OOPS");
        Kernel::Processor::halt();
    }

    Processor::current().exit_trap(*trap_frame);
}

// FIXME: Share the code below with Arch/x86_64/Interrupts.cpp
//        While refactoring, the interrupt handlers can also be moved into the InterruptManagement class.
GenericInterruptHandler& get_interrupt_handler(u8 interrupt_number)
{
    auto*& handler_slot = s_interrupt_handlers[interrupt_number];
    VERIFY(handler_slot != nullptr);
    return *handler_slot;
}

static void revert_to_unused_handler(u8 interrupt_number)
{
    auto handler = new UnhandledInterruptHandler(interrupt_number);
    handler->register_interrupt_handler();
}

void register_generic_interrupt_handler(u8 interrupt_number, GenericInterruptHandler& handler)
{
    auto*& handler_slot = s_interrupt_handlers[interrupt_number];
    if (handler_slot != nullptr) {
        if (handler_slot->type() == HandlerType::UnhandledInterruptHandler) {
            if (handler_slot) {
                auto* unhandled_handler = static_cast<UnhandledInterruptHandler*>(handler_slot);
                unhandled_handler->unregister_interrupt_handler();
                delete unhandled_handler;
            }
            handler_slot = &handler;
            return;
        }
        if (handler_slot->is_shared_handler() && !handler_slot->is_sharing_with_others()) {
            VERIFY(handler_slot->type() == HandlerType::SharedIRQHandler);
            static_cast<SharedIRQHandler*>(handler_slot)->register_handler(handler);
            return;
        }
        if (!handler_slot->is_shared_handler()) {
            if (handler_slot->type() == HandlerType::SpuriousInterruptHandler) {
                // FIXME: Add support for spurious interrupts on aarch64
                TODO_AARCH64();
            }
            VERIFY(handler_slot->type() == HandlerType::IRQHandler);
            auto& previous_handler = *handler_slot;
            handler_slot = nullptr;
            SharedIRQHandler::initialize(interrupt_number);
            VERIFY(handler_slot);
            static_cast<SharedIRQHandler*>(handler_slot)->register_handler(previous_handler);
            static_cast<SharedIRQHandler*>(handler_slot)->register_handler(handler);
            return;
        }
        VERIFY_NOT_REACHED();
    } else {
        handler_slot = &handler;
    }
}

void unregister_generic_interrupt_handler(u8 interrupt_number, GenericInterruptHandler& handler)
{
    auto*& handler_slot = s_interrupt_handlers[interrupt_number];
    VERIFY(handler_slot != nullptr);
    if (handler_slot->type() == HandlerType::UnhandledInterruptHandler) {
        return;
    }
    if (handler_slot->is_shared_handler() && !handler_slot->is_sharing_with_others()) {
        VERIFY(handler_slot->type() == HandlerType::SharedIRQHandler);
        auto* shared_handler = static_cast<SharedIRQHandler*>(handler_slot);
        shared_handler->unregister_handler(handler);
        if (!shared_handler->sharing_devices_count()) {
            handler_slot = nullptr;
            revert_to_unused_handler(interrupt_number);
        }
        return;
    }
    if (!handler_slot->is_shared_handler()) {
        VERIFY(handler_slot->type() == HandlerType::IRQHandler);
        handler_slot = nullptr;
        revert_to_unused_handler(interrupt_number);
        return;
    }
}

void initialize_interrupts()
{
    for (u8 i = 0; i < s_interrupt_handlers.size(); ++i) {
        auto* handler = new UnhandledInterruptHandler(i);
        handler->register_interrupt_handler();
    }
}

}
