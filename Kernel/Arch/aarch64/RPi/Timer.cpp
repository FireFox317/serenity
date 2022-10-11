/*
 * Copyright (c) 2021, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/NeverDestroyed.h>
#include <Kernel/Arch/aarch64/RPi/MMIO.h>
#include <Kernel/Arch/aarch64/RPi/Mailbox.h>
#include <Kernel/Arch/aarch64/RPi/Timer.h>
#include <Kernel/Sections.h>

namespace Kernel::RPi {

// "12.1 System Timer Registers" / "10.2 System Timer Registers"
struct TimerRegisters {
    u32 control_and_status;
    u32 counter_low;
    u32 counter_high;
    u32 compare[4];
};

// Bits of the `control_and_status` register.
// See "CS register" in Broadcom doc for details.
enum FlagBits {
    SystemTimerMatch0 = 1 << 0,
    SystemTimerMatch1 = 1 << 1,
    SystemTimerMatch2 = 1 << 2,
    SystemTimerMatch3 = 1 << 3,
};

Timer::Timer(Function<void(RegisterState const&)> callback)
    : HardwareTimer(1, move(callback))
    , m_registers(MMIO::the().peripheral<TimerRegisters>(0x3000))
{
    // OPTIMAL_TICKS_PER_SECOND_RATE = 1/250
    // dbgln("value: {}", (int)((1.0 / OPTIMAL_TICKS_PER_SECOND_RATE) * 1e6));
    set_interrupt_interval_usec((1.0 / OPTIMAL_TICKS_PER_SECOND_RATE) * 1e6);
    // set_interrupt_interval_usec(4000);
    enable_interrupt_mode();
}

Timer::~Timer() { }

UNMAP_AFTER_INIT NonnullLockRefPtr<Timer> Timer::initialize(Function<void(RegisterState const&)> callback)
{
    return adopt_lock_ref(*new Timer(move(callback)));
}

u64 Timer::microseconds_since_boot()
{
    u32 high = m_registers->counter_high;
    u32 low = m_registers->counter_low;
    if (high != m_registers->counter_high) {
        high = m_registers->counter_high;
        low = m_registers->counter_low;
    }
    return (static_cast<u64>(high) << 32) | low;
}

size_t Timer::ticks_per_second() const
{
    return m_frequency;
};

void Timer::reset_to_default_ticks_per_second()
{
    // InterruptDisabler disabler;
    // bool success = try_to_set_frequency(OPTIMAL_TICKS_PER_SECOND_RATE);
    // VERIFY(success);
}

bool Timer::try_to_set_frequency(size_t)
{
    // InterruptDisabler disabler;
    // if (!is_capable_of_frequency(frequency))
    //     return false;
    // disable_irq();
    // size_t reload_value = BASE_FREQUENCY / frequency;
    // IO::out8(TIMER0_CTL, LSB(reload_value));
    // IO::out8(TIMER0_CTL, MSB(reload_value));
    // m_frequency = frequency;
    // enable_irq();
    return true;
}
bool Timer::is_capable_of_frequency(size_t frequency) const
{
    VERIFY(frequency != 0);
    return true;
}
size_t Timer::calculate_nearest_possible_frequency(size_t frequency) const
{
    VERIFY(frequency != 0);
    return frequency;
}

bool Timer::handle_irq(RegisterState const& regs)
{
    // dmesgln("Timer fired: {} us", m_current_timer_value);
    auto result = HardwareTimer::handle_irq(regs);

    m_current_timer_value += m_interrupt_interval;
    set_compare(TimerID::Timer1, m_current_timer_value);

    VERIFY(m_current_timer_value > microseconds_since_boot());

    clear_interrupt(TimerID::Timer1);
    return result;
};

void Timer::enable_interrupt_mode()
{
    m_current_timer_value = microseconds_since_boot();
    m_current_timer_value += m_interrupt_interval;
    set_compare(TimerID::Timer1, m_current_timer_value);

    enable_irq();
}

void Timer::set_interrupt_interval_usec(u32 interrupt_interval)
{
    m_interrupt_interval = interrupt_interval;
}

void Timer::clear_interrupt(TimerID id)
{
    m_registers->control_and_status = 1 << to_underlying(id);
}

void Timer::set_compare(TimerID id, u32 compare)
{
    m_registers->compare[to_underlying(id)] = compare;
}

class SetClockRateMboxMessage : Mailbox::Message {
public:
    u32 clock_id;
    u32 rate_hz;
    u32 skip_setting_turbo;

    SetClockRateMboxMessage()
        : Mailbox::Message(0x0003'8002, 12)
    {
        clock_id = 0;
        rate_hz = 0;
        skip_setting_turbo = 0;
    }
};

u32 Timer::set_clock_rate(ClockID clock_id, u32 rate_hz, bool skip_setting_turbo)
{
    struct __attribute__((aligned(16))) {
        Mailbox::MessageHeader header;
        SetClockRateMboxMessage set_clock_rate;
        Mailbox::MessageTail tail;
    } message_queue;

    message_queue.set_clock_rate.clock_id = static_cast<u32>(clock_id);
    message_queue.set_clock_rate.rate_hz = rate_hz;
    message_queue.set_clock_rate.skip_setting_turbo = skip_setting_turbo ? 1 : 0;

    if (!Mailbox::the().send_queue(&message_queue, sizeof(message_queue))) {
        dbgln("Timer::set_clock_rate() failed!");
        return 0;
    }

    return message_queue.set_clock_rate.rate_hz;
}

}
