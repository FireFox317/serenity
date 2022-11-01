/*
 * Copyright (c) 2022, Timon Kruiper <timonkruiper@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/Vector.h>

#include <Kernel/Arch/Processor.h>
#include <Kernel/Arch/RegisterState.h>
#include <Kernel/Arch/TrapFrame.h>
#include <Kernel/Arch/aarch64/ASM_wrapper.h>
#include <Kernel/Arch/aarch64/CPU.h>
#include <Kernel/Arch/aarch64/Registers.h>
#include <Kernel/InterruptDisabler.h>

#include <Kernel/Process.h>
#include <Kernel/Scheduler.h>
#include <Kernel/Thread.h>

extern "C" uintptr_t vector_table_el1;
extern "C" [[noreturn]] void asm_exit_trap();

namespace Kernel {

extern "C" void thread_context_first_enter(void);
extern "C" void enter_thread_context(Thread* from_thread, Thread* to_thread) __attribute__((used));
extern "C" void context_first_init(Thread* from_thread, Thread* to_thread) __attribute__((used));
extern "C" void exit_kernel_thread(void);

Processor* g_current_processor;

void Processor::initialize(u32 cpu)
{
    VERIFY(g_current_processor == nullptr);

    auto current_exception_level = static_cast<u64>(Aarch64::Asm::get_current_exception_level());
    dbgln("CPU{} started in: EL{}", cpu, current_exception_level);

    dbgln("Drop CPU{} to EL1", cpu);
    drop_to_exception_level_1();

    // Load EL1 vector table
    Aarch64::Asm::el1_vector_table_install(&vector_table_el1);

    g_current_processor = this;
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

void Processor::flush_tlb(Memory::PageDirectory const*, VirtualAddress vaddr, size_t page_count)
{
    flush_tlb_local(vaddr, page_count);
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

u32 Processor::smp_wake_n_idle_processors(u32)
{
    return 0;
    // (void)wake_count;
    // TODO_AARCH64();
}

NAKED void thread_context_first_enter(void)
{
    asm(
        "ldr x0, [sp, #0] \n"
        "ldr x1, [sp, #8] \n"
        "add sp, sp, 16 \n"
        "bl context_first_init \n"
        "b asm_exit_trap \n");
}

void Processor::initialize_context_switching(Thread& initial_thread)
{
    VERIFY(initial_thread.process().is_kernel_process());

    auto& regs = initial_thread.regs();

    m_scheduler_initialized = true;
    Processor::set_current_in_scheduler(true);

    // VERIFY(!Processor::are_interrupts_enabled());
    // clang-format off
    asm volatile(
        // "msr SPSel, #1 \n"
        "mov sp, %[new_sp] \n"

        "sub sp, sp, 16 \n"
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
    // VERIFY(is_kernel_mode());

    dbgln_if(CONTEXT_SWITCH_DEBUG, "switch_context --> switching out of: {} {}", VirtualAddress(from_thread), *from_thread);

    // m_in_critical is restored in enter_thread_context
    from_thread->save_critical(m_in_critical);

    //     u64 spsel;
    // asm("mrs  %[value], SPSel"
    //     : [value] "=r"(spsel));

    //     dbgln("current spsel: {}", spsel);

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

        "sub sp, sp, 16 \n"
        "str %[from_thread], [sp, #0] \n"
        "str %[to_thread], [sp, #8] \n"

        "mov x0, %[from_thread] \n"
        "mov x1, %[to_thread] \n"
        "bl enter_thread_context \n"
        "ldr x0, %[to_ip]\n"
        "br x0 \n"

        "1: \n"
        "add sp, sp, #16 \n"

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

        "add sp, sp, #248 \n"

        "sub sp, sp, #264 \n"
        "ldr %[from_thread], [sp, #0] \n"
        "ldr %[to_thread],   [sp, #8] \n"
        "add sp, sp, #264 \n"
        :
        [from_ip] "=m"(from_thread->regs().elr_el1),
        [from_sp] "=m"(from_thread->regs().sp_el0),
        "=r"(from_thread),
        "=r"(to_thread)

        : [to_ip] "m"(to_thread->regs().elr_el1),
        [to_sp] "m"(to_thread->regs().sp_el0),
        [from_thread] "r"(from_thread),
        [to_thread] "r"(to_thread)
        : "memory");

    // dbgln("hmm");

    // // clang-format off
    // // Switch to new thread context, passing from_thread and to_thread
    // // through to the new context using registers rdx and rax
    // asm volatile(
    //     // NOTE: changing how much we push to the stack affects thread_context_first_enter()!
    //     "pushfq \n"
    //     "pushq %%rbx \n"
    //     "pushq %%rcx \n"
    //     "pushq %%rbp \n"
    //     "pushq %%rsi \n"
    //     "pushq %%rdi \n"
    //     "pushq %%r8 \n"
    //     "pushq %%r9 \n"
    //     "pushq %%r10 \n"
    //     "pushq %%r11 \n"
    //     "pushq %%r12 \n"
    //     "pushq %%r13 \n"
    //     "pushq %%r14 \n"
    //     "pushq %%r15 \n"
    //     "movq %%rsp, %[from_rsp] \n"
    //     "leaq 1f(%%rip), %%rbx \n"
    //     "movq %%rbx, %[from_rip] \n"
    //     "movq %[to_rsp0], %%rbx \n"
    //     "movl %%ebx, %[tss_rsp0l] \n"
    //     "shrq $32, %%rbx \n"
    //     "movl %%ebx, %[tss_rsp0h] \n"
    //     "movq %[to_rsp], %%rsp \n"
    //     "pushq %[to_thread] \n"
    //     "pushq %[from_thread] \n"
    //     "pushq %[to_rip] \n"
    //     "cld \n"
    //     "movq 16(%%rsp), %%rsi \n"
    //     "movq 8(%%rsp), %%rdi \n"
    //     "jmp enter_thread_context \n"
    //     "1: \n"
    //     "popq %%rdx \n"
    //     "popq %%rax \n"
    //     "popq %%r15 \n"
    //     "popq %%r14 \n"
    //     "popq %%r13 \n"
    //     "popq %%r12 \n"
    //     "popq %%r11 \n"
    //     "popq %%r10 \n"
    //     "popq %%r9 \n"
    //     "popq %%r8 \n"
    //     "popq %%rdi \n"
    //     "popq %%rsi \n"
    //     "popq %%rbp \n"
    //     "popq %%rcx \n"
    //     "popq %%rbx \n"
    //     "popfq \n"
    //     : [from_rsp] "=m" (from_thread->regs().rsp),
    //     [from_rip] "=m" (from_thread->regs().rip),
    //     [tss_rsp0l] "=m" (m_tss.rsp0l),
    //     [tss_rsp0h] "=m" (m_tss.rsp0h),
    //     "=d" (from_thread), // needed so that from_thread retains the correct value
    //     "=a" (to_thread) // needed so that to_thread retains the correct value
    //     : [to_rsp] "g" (to_thread->regs().rsp),
    //     [to_rsp0] "g" (to_thread->regs().rsp0),
    //     [to_rip] "c" (to_thread->regs().rip),
    //     [from_thread] "d" (from_thread),
    //     [to_thread] "a" (to_thread)
    //     : "memory", "rbx"
    // );
    // clang-format on

    dbgln_if(CONTEXT_SWITCH_DEBUG, "switch_context <-- from {} {} to {} {}", VirtualAddress(from_thread), *from_thread, VirtualAddress(to_thread), *to_thread);
    //     (void)from_thread;
    // (void)to_thread;
    // TODO_AARCH64();
}

void Processor::assume_context(Thread& thread, FlatPtr flags)
{
    (void)thread;
    (void)flags;
    TODO_AARCH64();
}

FlatPtr Processor::init_context(Thread& thread, bool)
{
    u64 kernel_stack_top = thread.kernel_stack_top();

    // Add a random offset between 0-256 (16-byte aligned)
    // kernel_stack_top -= round_up_to_power_of_two(get_fast_random<u8>(), 16);

    u64 stack_top = kernel_stack_top;

    // stack_top -= 2 * sizeof(u64);
    // u64 beginning = stack_top;

    // *reinterpret_cast<u64*>(kernel_stack_top - 1 * sizeof(u64)) = 0;
    // *reinterpret_cast<u64*>(kernel_stack_top - 2 * sizeof(u64)) = 0xaaaaaaaa;

    auto& regs = thread.regs();

    stack_top -= sizeof(RegisterState);
    RegisterState& iretframe = *reinterpret_cast<RegisterState*>(stack_top);
    memcpy(iretframe.x, regs.x, sizeof(regs.x));

    // NOTE: By not setting the link register (x29), backtraces end in the
    // iretframe.x[29] = beginning;
    iretframe.x[30] = FlatPtr(&exit_kernel_thread); // asdf
    iretframe.elr_el1 = regs.elr_el1;
    iretframe.sp_el0 = kernel_stack_top;

    // VERIFY(beginning % 16 == 0);

    // Aarch64::SPSR_EL1 saved_program_status_register_el1 = {};

    // Don't mask any interrupts, so all interrupts are enabled when trasfering into the new context
    // // Mask (disable) all interrupts
    // saved_program_status_register_el1.D = 1;
    // saved_program_status_register_el1.A = 1;
    // saved_program_status_register_el1.I = 1;
    // saved_program_status_register_el1.F = 1;

    // Indicate EL1 as exception origin mode (so we go back there)
    // t means use sp0 stack pointer
    // saved_program_status_register_el1.M = Aarch64::SPSR_EL1::Mode::EL1t;

    iretframe.spsr_el1 = 0b0100;

    // make space for a trap frame
    stack_top -= sizeof(TrapFrame);
    TrapFrame& trap = *reinterpret_cast<TrapFrame*>(stack_top);
    trap.regs = &iretframe;
    trap.next_trap = nullptr;

    // stack_top -= sizeof(u64); // pointer to TrapFrame
    // *reinterpret_cast<u64*>(stack_top) = stack_top + 8;

    regs.sp_el0 = stack_top;
    regs.elr_el1 = FlatPtr(&thread_context_first_enter);

    return stack_top;

    // TODO_AARCH64();
    // VERIFY(g_scheduler_lock.is_locked());
    // if (leave_crit) {
    //     // Leave the critical section we set up in in Process::exec,
    //     // but because we still have the scheduler lock we should end up with 1
    //     VERIFY(in_critical() == 2);
    //     m_in_critical = 1; // leave it without triggering anything or restoring flags
    // }

    // u64 kernel_stack_top = thread.kernel_stack_top();

    // // Add a random offset between 0-256 (16-byte aligned)
    // // kernel_stack_top -= round_up_to_power_of_two(get_fast_random<u8>(), 16);

    // u64 stack_top = kernel_stack_top;

    // // TODO: handle NT?
    // VERIFY((cpu_flags() & 0x24000) == 0); // Assume !(NT | VM)

    // auto& regs = thread.regs();
    // bool return_to_user = (regs.cs & 3) != 0;

    // stack_top -= 1 * sizeof(u64);
    // *reinterpret_cast<u64*>(kernel_stack_top - 2 * sizeof(u64)) = FlatPtr(&exit_kernel_thread);

    // stack_top -= sizeof(RegisterState);

    // // we want to end up 16-byte aligned, %rsp + 8 should be aligned
    // stack_top -= sizeof(u64);
    // *reinterpret_cast<u64*>(kernel_stack_top - sizeof(u64)) = 0;

    // // set up the stack so that after returning from thread_context_first_enter()
    // // we will end up either in kernel mode or user mode, depending on how the thread is set up
    // // However, the first step is to always start in kernel mode with thread_context_first_enter
    // RegisterState& iretframe = *reinterpret_cast<RegisterState*>(stack_top);
    // iretframe.rdi = regs.rdi;
    // iretframe.rsi = regs.rsi;
    // iretframe.rbp = regs.rbp;
    // iretframe.rsp = 0;
    // iretframe.rbx = regs.rbx;
    // iretframe.rdx = regs.rdx;
    // iretframe.rcx = regs.rcx;
    // iretframe.rax = regs.rax;
    // iretframe.r8 = regs.r8;
    // iretframe.r9 = regs.r9;
    // iretframe.r10 = regs.r10;
    // iretframe.r11 = regs.r11;
    // iretframe.r12 = regs.r12;
    // iretframe.r13 = regs.r13;
    // iretframe.r14 = regs.r14;
    // iretframe.r15 = regs.r15;
    // iretframe.rflags = regs.rflags;
    // iretframe.rip = regs.rip;
    // iretframe.cs = regs.cs;
    // if (return_to_user) {
    //     iretframe.userspace_rsp = regs.rsp;
    //     iretframe.userspace_ss = GDT_SELECTOR_DATA3 | 3;
    // } else {
    //     iretframe.userspace_rsp = kernel_stack_top;
    //     iretframe.userspace_ss = 0;
    // }

    // // make space for a trap frame
    // stack_top -= sizeof(TrapFrame);
    // TrapFrame& trap = *reinterpret_cast<TrapFrame*>(stack_top);
    // trap.regs = &iretframe;
    // trap.prev_irq_level = 0;
    // trap.next_trap = nullptr;

    // stack_top -= sizeof(u64); // pointer to TrapFrame
    // *reinterpret_cast<u64*>(stack_top) = stack_top + 8;

    // if constexpr (CONTEXT_SWITCH_DEBUG) {
    //     if (return_to_user) {
    //         dbgln("init_context {} ({}) set up to execute at rip={}:{}, rsp={}, stack_top={}, user_top={}",
    //             thread,
    //             VirtualAddress(&thread),
    //             iretframe.cs, regs.rip,
    //             VirtualAddress(regs.rsp),
    //             VirtualAddress(stack_top),
    //             iretframe.userspace_rsp);
    //     } else {
    //         dbgln("init_context {} ({}) set up to execute at rip={}:{}, rsp={}, stack_top={}",
    //             thread,
    //             VirtualAddress(&thread),
    //             iretframe.cs, regs.rip,
    //             VirtualAddress(regs.rsp),
    //             VirtualAddress(stack_top));
    //     }
    // }

    // // make switch_context() always first return to thread_context_first_enter()
    // // in kernel mode, so set up these values so that we end up popping iretframe
    // // off the stack right after the context switch completed, at which point
    // // control is transferred to what iretframe is pointing to.
    // regs.rip = FlatPtr(&thread_context_first_enter);
    // regs.rsp0 = kernel_stack_top;
    // regs.rsp = stack_top;
    // regs.cs = GDT_SELECTOR_CODE0;
    // return stack_top;
}

void Processor::enter_trap(TrapFrame& trap, bool raise_irq)
{
    VERIFY_INTERRUPTS_DISABLED();
    VERIFY(&Processor::current() == this);
    // trap.prev_irq_level = m_in_irq;
    if (raise_irq)
        m_in_irq++;
    auto* current_thread = Processor::current_thread();
    if (current_thread) {
        auto& current_trap = current_thread->current_trap();
        trap.next_trap = current_trap;
        current_trap = &trap;
        // The cs register of this trap tells us where we will return back to
        // auto new_previous_mode = ((trap.regs->cs & 3) != 0) ? Thread::PreviousMode::UserMode : Thread::PreviousMode::KernelMode;
        // if (current_thread->set_previous_mode(new_previous_mode) && trap.prev_irq_level == 0) {
        //     current_thread->update_time_scheduled(TimeManagement::scheduler_current_time(), new_previous_mode == Thread::PreviousMode::KernelMode, false);
        // }
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

    // VERIFY(m_in_irq >= trap.prev_irq_level);
    m_in_irq = 0;

    auto* current_thread = Processor::current_thread();
    if (current_thread) {
        auto& current_trap = current_thread->current_trap();
        current_trap = trap.next_trap;
        Thread::PreviousMode new_previous_mode;
        if (current_trap) {
            VERIFY(current_trap->regs);
            // If we have another higher level trap then we probably returned
            // from an interrupt or irq handler. The cs register of the
            // new/higher level trap tells us what the mode prior to it was
            // new_previous_mode = ((current_trap->regs->cs & 3) != 0) ? Thread::PreviousMode::UserMode : Thread::PreviousMode::KernelMode;
        } else {
            // If we don't have a higher level trap then we're back in user mode.
            // Which means that the previous mode prior to being back in user mode was kernel mode
            new_previous_mode = Thread::PreviousMode::KernelMode;
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
    (void)thread;
    (void)max_frames;
    TODO_AARCH64();
    return Vector<FlatPtr, 32> {};
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

extern "C" void enter_thread_context(Thread* from_thread, Thread* to_thread)
{
    VERIFY(from_thread == to_thread || from_thread->state() != Thread::State::Running);
    VERIFY(to_thread->state() == Thread::State::Running);

    Processor::set_current_thread(*to_thread);

    // auto& from_regs = from_thread->regs();
    // auto& to_regs = to_thread->regs();

    auto& processor = Processor::current();

    // if (from_thread->process().is_traced())
    //     read_debug_registers_into(from_thread->debug_register_state());

    // if (to_thread->process().is_traced()) {
    //     write_debug_registers_from(to_thread->debug_register_state());
    // } else {
    //     clear_debug_registers();
    // }

    // if (from_regs.cr3 != to_regs.cr3)
    //     write_cr3(to_regs.cr3);

    to_thread->set_cpu(processor.id());

    auto in_critical = to_thread->saved_critical();
    VERIFY(in_critical > 0);
    Processor::restore_critical(in_critical);
}

extern "C" void context_first_init(Thread* from_thread, Thread* to_thread)
{
    VERIFY(!are_interrupts_enabled());
    // VERIFY(is_kernel_mode());

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

void exit_kernel_thread(void)
{
    Thread::current()->exit();
}

StringView Processor::platform_string()
{
    return "aarch64"sv;
}

}
