/*
 * Copyright (c) 2022, Timon Kruiper <timonkruiper@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/Vector.h>

#include <Kernel/Arch/Processor.h>
#include <Kernel/Arch/SafeMem.h>
#include <Kernel/Arch/TrapFrame.h>
#include <Kernel/Arch/aarch64/ASM_wrapper.h>
#include <Kernel/Arch/aarch64/CPU.h>
#include <Kernel/Arch/aarch64/CPUID.h>
#include <Kernel/InterruptDisabler.h>
#include <Kernel/Memory/ScopedAddressSpaceSwitcher.h>
#include <Kernel/Process.h>
#include <Kernel/Random.h>
#include <Kernel/Scheduler.h>
#include <Kernel/Thread.h>
#include <Kernel/Time/TimeManagement.h>

namespace Kernel {

extern "C" void thread_context_first_enter(void);
extern "C" void exit_kernel_thread(void);
extern "C" void context_first_init(Thread* from_thread, Thread* to_thread) __attribute__((used));
extern "C" void enter_thread_context(Thread* from_thread, Thread* to_thread) __attribute__((used));

Processor* g_current_processor;
READONLY_AFTER_INIT FPUState Processor::s_clean_fpu_state;

static void store_fpu_state(FPUState* fpu_state)
{
    asm volatile(
        "mov x0, %[fpu_state]\n"
        "stp q0, q1, [x0, #(0 * 16)]\n"
        "stp q2, q3, [x0, #(2 * 16)]\n"
        "stp q4, q5, [x0, #(4 * 16)]\n"
        "stp q6, q7, [x0, #(6 * 16)]\n"
        "stp q8, q9, [x0, #(8 * 16)]\n"
        "stp q10, q11, [x0, #(10 * 16)]\n"
        "stp q12, q13, [x0, #(12 * 16)]\n"
        "stp q14, q15, [x0, #(14 * 16)]\n"
        "stp q16, q17, [x0, #(16 * 16)]\n"
        "stp q18, q19, [x0, #(18 * 16)]\n"
        "stp q20, q21, [x0, #(20 * 16)]\n"
        "stp q22, q23, [x0, #(22 * 16)]\n"
        "stp q24, q25, [x0, #(24 * 16)]\n"
        "stp q26, q27, [x0, #(26 * 16)]\n"
        "stp q28, q29, [x0, #(28 * 16)]\n"
        "stp q30, q31, [x0, #(30 * 16)]\n"
        "\n" ::[fpu_state] "r"(fpu_state));
}

static void load_fpu_state(FPUState* fpu_state)
{
    asm volatile(
        "mov x0, %[fpu_state]\n"
        "ldp q0, q1, [x0, #(0 * 16)]\n"
        "ldp q2, q3, [x0, #(2 * 16)]\n"
        "ldp q4, q5, [x0, #(4 * 16)]\n"
        "ldp q6, q7, [x0, #(6 * 16)]\n"
        "ldp q8, q9, [x0, #(8 * 16)]\n"
        "ldp q10, q11, [x0, #(10 * 16)]\n"
        "ldp q12, q13, [x0, #(12 * 16)]\n"
        "ldp q14, q15, [x0, #(14 * 16)]\n"
        "ldp q16, q17, [x0, #(16 * 16)]\n"
        "ldp q18, q19, [x0, #(18 * 16)]\n"
        "ldp q20, q21, [x0, #(20 * 16)]\n"
        "ldp q22, q23, [x0, #(22 * 16)]\n"
        "ldp q24, q25, [x0, #(24 * 16)]\n"
        "ldp q26, q27, [x0, #(26 * 16)]\n"
        "ldp q28, q29, [x0, #(28 * 16)]\n"
        "ldp q30, q31, [x0, #(30 * 16)]\n"
        "\n" ::[fpu_state] "r"(fpu_state));
}

void Processor::install(u32 cpu)
{
    VERIFY(g_current_processor == nullptr);
    m_cpu = cpu;
    m_features = detect_cpu_features();
    m_physical_address_bit_width = detect_physical_address_bit_width();
    m_virtual_address_bit_width = detect_virtual_address_bit_width();

    g_current_processor = this;
}

void Processor::initialize()
{
    deferred_call_pool_init();

    dmesgln("CPU[{}]: Supports {}", m_cpu, build_cpu_feature_names(m_features));
    dmesgln("CPU[{}]: Physical address bit width: {}", m_cpu, m_physical_address_bit_width);
    dmesgln("CPU[{}]: Virtual address bit width: {}", m_cpu, m_virtual_address_bit_width);
    if (!has_feature(CPUFeature::RNG))
        dmesgln("CPU[{}]: {} not detected, randomness will be poor", m_cpu, cpu_feature_to_description(CPUFeature::RNG));

    store_fpu_state(&s_clean_fpu_state);
}

[[noreturn]] void Processor::halt()
{
    disable_interrupts();
    for (;;)
        asm volatile("wfi");
}

void Processor::flush_tlb_local(VirtualAddress, size_t)
{
    // FIXME: Figure out how to flush a single page
    asm volatile("dsb ishst");
    asm volatile("tlbi vmalle1is");
    asm volatile("dsb ish");
    asm volatile("isb");
}

void Processor::flush_entire_tlb_local()
{
    asm volatile("dsb ishst");
    asm volatile("tlbi vmalle1is");
    asm volatile("dsb ish");
    asm volatile("isb");
}

void Processor::flush_tlb(Memory::PageDirectory const*, VirtualAddress vaddr, size_t page_count)
{
    flush_tlb_local(vaddr, page_count);
}

void Processor::leave_critical()
{
    InterruptDisabler disabler;
    current().do_leave_critical();
}

void Processor::do_leave_critical()
{
    VERIFY(m_in_critical > 0);
    if (m_in_critical == 1) {
        if (m_in_irq == 0) {
            // FIXME: Call deferred_call_execute_pending()!
            VERIFY(m_in_critical == 1);
        }
        m_in_critical = 0;
        if (m_in_irq == 0)
            check_invoke_scheduler();
    } else {
        m_in_critical = m_in_critical - 1;
    }
}

u32 Processor::clear_critical()
{
    InterruptDisabler disabler;
    auto prev_critical = in_critical();
    auto& proc = current();
    proc.m_in_critical = 0;
    if (proc.m_in_irq == 0)
        proc.check_invoke_scheduler();
    return prev_critical;
}

u32 Processor::smp_wake_n_idle_processors(u32 wake_count)
{
    (void)wake_count;
    // FIXME: Actually wake up other cores when SMP is supported for aarch64.
    return 0;
}

void Processor::initialize_context_switching(Thread& initial_thread)
{
    VERIFY(initial_thread.process().is_kernel_process());

    m_scheduler_initialized = true;

    // FIXME: Figure out if we need to call {pre_,post_,}init_finished once aarch64 supports SMP
    Processor::set_current_in_scheduler(true);

    auto& regs = initial_thread.regs();
    // clang-format off
    asm volatile(
        "mov sp, %[new_sp] \n"

        "sub sp, sp, 24 \n"
        "str %[from_to_thread], [sp, #0] \n"
        "str %[from_to_thread], [sp, #8] \n"
        "br %[new_ip] \n"
        :: [new_sp] "r" (regs.sp_el0),
        [new_ip] "r" (regs.elr_el1),
        [from_to_thread] "r" (&initial_thread)
    );
    // clang-format on

    VERIFY_NOT_REACHED();
}

void Processor::switch_context(Thread*& from_thread, Thread*& to_thread)
{
    VERIFY(!m_in_irq);
    VERIFY(m_in_critical == 1);

    dbgln_if(CONTEXT_SWITCH_DEBUG, "switch_context --> switching out of: {} {}", VirtualAddress(from_thread), *from_thread);

    // m_in_critical is restored in enter_thread_context
    from_thread->save_critical(m_in_critical);

    // clang-format off
    asm volatile(
        "sub sp, sp, #248 \n"
        "stp x0, x1,     [sp, #(0 * 0)] \n"
        "stp x2, x3,     [sp, #(2 * 8)] \n"
        "stp x4, x5,     [sp, #(4 * 8)] \n"
        "stp x6, x7,     [sp, #(6 * 8)] \n"
        "stp x8, x9,     [sp, #(8 * 8)] \n"
        "stp x10, x11,   [sp, #(10 * 8)] \n"
        "stp x12, x13,   [sp, #(12 * 8)] \n"
        "stp x14, x15,   [sp, #(14 * 8)] \n"
        "stp x16, x17,   [sp, #(16 * 8)] \n"
        "stp x18, x19,   [sp, #(18 * 8)] \n"
        "stp x20, x21,   [sp, #(20 * 8)] \n"
        "stp x22, x23,   [sp, #(22 * 8)] \n"
        "stp x24, x25,   [sp, #(24 * 8)] \n"
        "stp x26, x27,   [sp, #(26 * 8)] \n"
        "stp x28, x29,   [sp, #(28 * 8)] \n"
        "str x30,        [sp, #(30 * 8)] \n"
        "mov x0, sp \n"
        "str x0, %[from_sp] \n"
        "ldr x0, =1f \n"
        "str x0, %[from_ip] \n"

        "ldr x0, %[to_sp] \n"
        "mov sp, x0 \n"

        "sub sp, sp, 24 \n"
        "ldr x0, %[from_thread] \n"
        "ldr x1, %[to_thread] \n"
        "ldr x2, %[to_ip] \n"
        "str x0, [sp, #0] \n"
        "str x1, [sp, #8] \n"
        "str x2, [sp, #16] \n"

        "bl enter_thread_context \n"
        "ldr x0, [sp, #16]\n"
        "br x0 \n"

        "1: \n"
        "add sp, sp, 24 \n"

        "ldp x0, x1,     [sp, #(0 * 0)] \n"
        "ldp x2, x3,     [sp, #(2 * 8)] \n"
        "ldp x4, x5,     [sp, #(4 * 8)] \n"
        "ldp x6, x7,     [sp, #(6 * 8)] \n"
        "ldp x8, x9,     [sp, #(8 * 8)] \n"
        "ldp x10, x11,   [sp, #(10 * 8)] \n"
        "ldp x12, x13,   [sp, #(12 * 8)] \n"
        "ldp x14, x15,   [sp, #(14 * 8)] \n"
        "ldp x16, x17,   [sp, #(16 * 8)] \n"
        "ldp x18, x19,   [sp, #(18 * 8)] \n"
        "ldp x20, x21,   [sp, #(20 * 8)] \n"
        "ldp x22, x23,   [sp, #(22 * 8)] \n"
        "ldp x24, x25,   [sp, #(24 * 8)] \n"
        "ldp x26, x27,   [sp, #(26 * 8)] \n"
        "ldp x28, x29,   [sp, #(28 * 8)] \n"
        "ldr x30,        [sp, #(30 * 8)] \n"

        "sub sp, sp, 24 \n"
        "ldr x0, [sp, #0] \n"
        "ldr x1, [sp, #8] \n"
        "str x0, %[from_thread] \n"
        "str x1, %[to_thread] \n"

        "add sp, sp, #272 \n"
        :
        [from_ip] "=m"(from_thread->regs().elr_el1),
        [from_sp] "=m"(from_thread->regs().sp_el0),
        "=m"(from_thread),
        "=m"(to_thread)

        : [to_ip] "m"(to_thread->regs().elr_el1),
        [to_sp] "m"(to_thread->regs().sp_el0),
        [from_thread] "m"(from_thread),
        [to_thread] "m"(to_thread)
        : "memory", "x0", "x1", "x2");
    // clang-format on

    dbgln_if(CONTEXT_SWITCH_DEBUG, "switch_context <-- from {} {} to {} {}", VirtualAddress(from_thread), *from_thread, VirtualAddress(to_thread), *to_thread);
}

void Processor::assume_context(Thread& thread)
{
    dbgln_if(CONTEXT_SWITCH_DEBUG, "Assume context for thread {} {}", VirtualAddress(&thread), thread);

    VERIFY_INTERRUPTS_DISABLED();
    Scheduler::prepare_after_exec();
    // in_critical() should be 2 here. The critical section in Process::exec
    // and then the scheduler lock
    VERIFY(Processor::in_critical() == 2);

    Processor::current().init_context(thread, true);

    // FIXME: Without this, we crash in Processor::exit_trap.
    //        Figure out if we're missing something, or what's wrong.
    thread.current_trap() = nullptr;

    asm volatile(
        "mov sp, %[to_sp] \n"

        "sub sp, sp, 24 \n"
        "mov x0, %[from_thread] \n"
        "mov x1, %[from_thread] \n"
        "mov x2, %[to_ip] \n"

        "str x0, [sp, #0] \n"
        "str x1, [sp, #8] \n"
        "str x2, [sp, #16] \n"

        "bl enter_thread_context \n"
        "ldr x0, [sp, #16]\n"
        "br x0 \n"
        :
        : [to_sp] "r"(thread.regs().sp_el0),
        [to_ip] "r"(thread.regs().elr_el1),
        [from_thread] "r"(&thread)
        : "memory", "x0", "x1", "x2");

    VERIFY_NOT_REACHED();
}

FlatPtr Processor::init_context(Thread& thread, bool leave_crit)
{
    VERIFY(g_scheduler_lock.is_locked());
    if (leave_crit) {
        // Leave the critical section we set up in Process::exec,
        // but because we still have the scheduler lock we should end up with 1
        VERIFY(in_critical() == 2);
        m_in_critical = 1; // leave it without triggering anything or restoring flags
    }

    u64 kernel_stack_top = thread.kernel_stack_top();

    // Add a random offset between 0-256 (16-byte aligned)
    kernel_stack_top -= round_up_to_power_of_two(get_fast_random<u8>(), 16);

    u64 stack_top = kernel_stack_top;

    auto& thread_regs = thread.regs();

    // Push a RegisterState and TrapFrame onto the stack, which will be popped of the stack and restored into the
    // state of the processor by restore_previous_context.
    stack_top -= sizeof(RegisterState);
    RegisterState& eretframe = *reinterpret_cast<RegisterState*>(stack_top);
    memcpy(eretframe.x, thread_regs.x, sizeof(thread_regs.x));

    // x30 is the Link Register for the aarch64 ABI, so this will return to exit_kernel_thread when main thread function returns.
    if (thread.process().is_kernel_process())
        eretframe.x[30] = FlatPtr(&exit_kernel_thread);
    eretframe.elr_el1 = thread_regs.elr_el1;
    eretframe.sp_el0 = thread_regs.sp_el0;
    eretframe.spsr_el1 = thread_regs.spsr_el1;

    // Push a TrapFrame onto the stack
    stack_top -= sizeof(TrapFrame);
    TrapFrame& trap = *reinterpret_cast<TrapFrame*>(stack_top);
    trap.regs = &eretframe;
    trap.next_trap = nullptr;

    if constexpr (CONTEXT_SWITCH_DEBUG) {
        dbgln("init_context {} ({}) set up to execute at ip={}, sp={}, stack_top={}",
            thread,
            VirtualAddress(&thread),
            VirtualAddress(thread_regs.elr_el1),
            VirtualAddress(thread_regs.sp_el0),
            VirtualAddress(stack_top));
    }

    // This make sure the thread first executes thread_context_first_enter, which will actually call restore_previous_context
    // which restores the context set up above.
    thread_regs.set_sp(stack_top);
    thread_regs.set_ip(FlatPtr(&thread_context_first_enter));

    return stack_top;
}

void Processor::enter_trap(TrapFrame& trap, bool raise_irq)
{
    VERIFY_INTERRUPTS_DISABLED();
    VERIFY(&Processor::current() == this);
    // FIXME: Figure out if we need prev_irq_level, see duplicated code in Kernel/Arch/x86/common/Processor.cpp
    if (raise_irq)
        m_in_irq++;
    auto* current_thread = Processor::current_thread();
    if (current_thread) {
        auto& current_trap = current_thread->current_trap();
        trap.next_trap = current_trap;
        current_trap = &trap;
        auto new_previous_mode = trap.regs->previous_mode();
        if (current_thread->set_previous_mode(new_previous_mode)) {
            current_thread->update_time_scheduled(TimeManagement::scheduler_current_time(), new_previous_mode == ExecutionMode::Kernel, false);
        }
    } else {
        trap.next_trap = nullptr;
    }
}

void Processor::exit_trap(TrapFrame& trap)
{
    VERIFY_INTERRUPTS_DISABLED();
    VERIFY(&Processor::current() == this);

    // Temporarily enter a critical section. This is to prevent critical
    // sections entered and left within e.g. smp_process_pending_messages
    // to trigger a context switch while we're executing this function
    // See the comment at the end of the function why we don't use
    // ScopedCritical here.
    m_in_critical = m_in_critical + 1;

    // FIXME: Figure out if we need prev_irq_level, see duplicated code in Kernel/Arch/x86/common/Processor.cpp
    m_in_irq = 0;

    // Process the deferred call queue. Among other things, this ensures
    // that any pending thread unblocks happen before we enter the scheduler.
    deferred_call_execute_pending();

    auto* current_thread = Processor::current_thread();
    if (current_thread) {
        auto& current_trap = current_thread->current_trap();
        current_trap = trap.next_trap;
        ExecutionMode new_previous_mode;
        if (current_trap) {
            VERIFY(current_trap->regs);
            new_previous_mode = current_trap->regs->previous_mode();
        } else {
            // If we don't have a higher level trap then we're back in user mode.
            // Which means that the previous mode prior to being back in user mode was kernel mode
            new_previous_mode = ExecutionMode::Kernel;
        }

        if (current_thread->set_previous_mode(new_previous_mode))
            current_thread->update_time_scheduled(TimeManagement::scheduler_current_time(), true, false);
    }

    VERIFY_INTERRUPTS_DISABLED();

    // Leave the critical section without actually enabling interrupts.
    // We don't want context switches to happen until we're explicitly
    // triggering a switch in check_invoke_scheduler.
    m_in_critical = m_in_critical - 1;
    if (!m_in_irq && !m_in_critical)
        check_invoke_scheduler();
}

ErrorOr<Vector<FlatPtr, 32>> Processor::capture_stack_trace(Thread& thread, size_t max_frames)
{
    FlatPtr frame_ptr = 0, ip = 0;
    Vector<FlatPtr, 32> stack_trace;

    auto walk_stack = [&](FlatPtr stack_ptr) -> ErrorOr<void> {
        constexpr size_t max_stack_frames = 4096;
        bool is_walking_userspace_stack = false;
        TRY(stack_trace.try_append(ip));
        size_t count = 1;
        while (stack_ptr && stack_trace.size() < max_stack_frames) {
            FlatPtr retaddr;

            count++;
            if (max_frames != 0 && count > max_frames)
                break;

            if (!Memory::is_user_address(VirtualAddress { stack_ptr })) {
                if (is_walking_userspace_stack) {
                    dbgln("SHENANIGANS! Userspace stack points back into kernel memory");
                    break;
                }
            } else {
                is_walking_userspace_stack = true;
            }

            if (Memory::is_user_range(VirtualAddress(stack_ptr), sizeof(FlatPtr) * 2)) {
                if (copy_from_user(&retaddr, &((FlatPtr*)stack_ptr)[1]).is_error() || !retaddr)
                    break;
                TRY(stack_trace.try_append(retaddr));
                if (copy_from_user(&stack_ptr, (FlatPtr*)stack_ptr).is_error())
                    break;
            } else {
                void* fault_at;
                if (!safe_memcpy(&retaddr, &((FlatPtr*)stack_ptr)[1], sizeof(FlatPtr), fault_at) || !retaddr)
                    break;
                TRY(stack_trace.try_append(retaddr));
                if (!safe_memcpy(&stack_ptr, (FlatPtr*)stack_ptr, sizeof(FlatPtr), fault_at))
                    break;
            }
        }
        return {};
    };
    auto capture_current_thread = [&]() {
        frame_ptr = (FlatPtr)__builtin_frame_address(0);
        ip = (FlatPtr)__builtin_return_address(0);

        return walk_stack(frame_ptr);
    };

    // Since the thread may be running on another processor, there
    // is a chance a context switch may happen while we're trying
    // to get it. It also won't be entirely accurate and merely
    // reflect the status at the last context switch.
    SpinlockLocker lock(g_scheduler_lock);
    if (&thread == Processor::current_thread()) {
        VERIFY(thread.state() == Thread::State::Running);
        // Leave the scheduler lock. If we trigger page faults we may
        // need to be preempted. Since this is our own thread it won't
        // cause any problems as the stack won't change below this frame.
        lock.unlock();
        TRY(capture_current_thread());
    } else if (thread.is_active()) {
        VERIFY(thread.cpu() != Processor::current_id());
        VERIFY_NOT_REACHED();
    } else {
        switch (thread.state()) {
        case Thread::State::Running:
            VERIFY_NOT_REACHED(); // should have been handled above
        case Thread::State::Runnable:
        case Thread::State::Stopped:
        case Thread::State::Blocked:
        case Thread::State::Dying:
        case Thread::State::Dead: {
            // We need to retrieve ebp from what was last pushed to the kernel
            // stack. Before switching out of that thread, it switch_context
            // pushed the callee-saved registers, and the last of them happens
            // to be ebp.
            ScopedAddressSpaceSwitcher switcher(thread.process());
            auto& regs = thread.regs();
            auto* stack_top = reinterpret_cast<FlatPtr*>(regs.sp());
            if (Memory::is_user_range(VirtualAddress(stack_top), sizeof(FlatPtr))) {
                if (copy_from_user(&frame_ptr, &((FlatPtr*)stack_top)[0]).is_error())
                    frame_ptr = 0;
            } else {
                void* fault_at;
                if (!safe_memcpy(&frame_ptr, &((FlatPtr*)stack_top)[0], sizeof(FlatPtr), fault_at))
                    frame_ptr = 0;
            }

            ip = regs.ip();

            // TODO: We need to leave the scheduler lock here, but we also
            //       need to prevent the target thread from being run while
            //       we walk the stack
            lock.unlock();
            TRY(walk_stack(frame_ptr));
            break;
        }
        default:
            dbgln("Cannot capture stack trace for thread {} in state {}", thread, thread.state_string());
            break;
        }
    }
    return stack_trace;
}

void Processor::check_invoke_scheduler()
{
    VERIFY_INTERRUPTS_DISABLED();
    VERIFY(!m_in_irq);
    VERIFY(!m_in_critical);
    VERIFY(&Processor::current() == this);
    if (m_invoke_scheduler_async && m_scheduler_initialized) {
        m_invoke_scheduler_async = false;
        Scheduler::invoke_async();
    }
}

NAKED void thread_context_first_enter(void)
{
    asm(
        "ldr x0, [sp, #0] \n"
        "ldr x1, [sp, #8] \n"
        "add sp, sp, 24 \n"
        "bl context_first_init \n"
        "b restore_context_and_eret \n");
}

void exit_kernel_thread(void)
{
    Thread::current()->exit();
}

extern "C" void context_first_init([[maybe_unused]] Thread* from_thread, [[maybe_unused]] Thread* to_thread)
{
    VERIFY(!are_interrupts_enabled());

    dbgln_if(CONTEXT_SWITCH_DEBUG, "switch_context <-- from {} {} to {} {} (context_first_init)", VirtualAddress(from_thread), *from_thread, VirtualAddress(to_thread), *to_thread);

    VERIFY(to_thread == Thread::current());

    Scheduler::enter_current(*from_thread);

    auto in_critical = to_thread->saved_critical();
    VERIFY(in_critical > 0);
    Processor::restore_critical(in_critical);

    // Since we got here and don't have Scheduler::context_switch in the
    // call stack (because this is the first time we switched into this
    // context), we need to notify the scheduler so that it can release
    // the scheduler lock. We don't want to enable interrupts at this point
    // as we're still in the middle of a context switch. Doing so could
    // trigger a context switch within a context switch, leading to a crash.
    Scheduler::leave_on_first_switch(InterruptsState::Disabled);
}

extern "C" void enter_thread_context(Thread* from_thread, Thread* to_thread)
{
    VERIFY(from_thread == to_thread || from_thread->state() != Thread::State::Running);
    VERIFY(to_thread->state() == Thread::State::Running);

    Processor::set_current_thread(*to_thread);

    store_fpu_state(&from_thread->fpu_state());

    auto& from_regs = from_thread->regs();
    auto& to_regs = to_thread->regs();
    if (from_regs.ttbr0_el1 != to_regs.ttbr0_el1) {
        Aarch64::Asm::set_ttbr0_el1(to_regs.ttbr0_el1);
        Processor::flush_entire_tlb_local();
    }

    to_thread->set_cpu(Processor::current().id());

    Processor::set_thread_specific_data(to_thread->thread_specific_data());

    auto in_critical = to_thread->saved_critical();
    VERIFY(in_critical > 0);
    Processor::restore_critical(in_critical);

    load_fpu_state(&to_thread->fpu_state());
}

StringView Processor::platform_string()
{
    return "aarch64"sv;
}

void Processor::set_thread_specific_data(VirtualAddress thread_specific_data)
{
    Aarch64::Asm::set_tpidr_el0(thread_specific_data.get());
}

UNMAP_AFTER_INIT void Processor::deferred_call_pool_init()
{
    size_t pool_count = sizeof(m_deferred_call_pool) / sizeof(m_deferred_call_pool[0]);
    for (size_t i = 0; i < pool_count; i++) {
        auto& entry = m_deferred_call_pool[i];
        entry.next = i < pool_count - 1 ? &m_deferred_call_pool[i + 1] : nullptr;
        new (entry.handler_storage) DeferredCallEntry::HandlerFunction;
        entry.was_allocated = false;
    }
    m_pending_deferred_calls = nullptr;
    m_free_deferred_call_pool_entry = &m_deferred_call_pool[0];
}

void Processor::deferred_call_return_to_pool(DeferredCallEntry* entry)
{
    VERIFY(m_in_critical);
    VERIFY(!entry->was_allocated);

    entry->handler_value() = {};

    entry->next = m_free_deferred_call_pool_entry;
    m_free_deferred_call_pool_entry = entry;
}

DeferredCallEntry* Processor::deferred_call_get_free()
{
    VERIFY(m_in_critical);

    if (m_free_deferred_call_pool_entry) {
        // Fast path, we have an entry in our pool
        auto* entry = m_free_deferred_call_pool_entry;
        m_free_deferred_call_pool_entry = entry->next;
        VERIFY(!entry->was_allocated);
        return entry;
    }

    auto* entry = new DeferredCallEntry;
    new (entry->handler_storage) DeferredCallEntry::HandlerFunction;
    entry->was_allocated = true;
    return entry;
}

void Processor::deferred_call_execute_pending()
{
    VERIFY(m_in_critical);

    if (!m_pending_deferred_calls)
        return;
    auto* pending_list = m_pending_deferred_calls;
    m_pending_deferred_calls = nullptr;

    // We pulled the stack of pending deferred calls in LIFO order, so we need to reverse the list first
    auto reverse_list =
        [](DeferredCallEntry* list) -> DeferredCallEntry* {
        DeferredCallEntry* rev_list = nullptr;
        while (list) {
            auto next = list->next;
            list->next = rev_list;
            rev_list = list;
            list = next;
        }
        return rev_list;
    };
    pending_list = reverse_list(pending_list);

    do {
        pending_list->invoke_handler();

        // Return the entry back to the pool, or free it
        auto* next = pending_list->next;
        if (pending_list->was_allocated) {
            pending_list->handler_value().~Function();
            delete pending_list;
        } else
            deferred_call_return_to_pool(pending_list);
        pending_list = next;
    } while (pending_list);
}

void Processor::deferred_call_queue_entry(DeferredCallEntry* entry)
{
    VERIFY(m_in_critical);
    entry->next = m_pending_deferred_calls;
    m_pending_deferred_calls = entry;
}

void Processor::deferred_call_queue(Function<void()> callback)
{
    // NOTE: If we are called outside of a critical section and outside
    // of an irq handler, the function will be executed before we return!
    ScopedCritical critical;
    auto& cur_proc = Processor::current();

    auto* entry = cur_proc.deferred_call_get_free();
    entry->handler_value() = move(callback);

    cur_proc.deferred_call_queue_entry(entry);
}

}
