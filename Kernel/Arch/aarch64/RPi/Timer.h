/*
 * Copyright (c) 2021, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <Kernel/Interrupts/IRQHandler.h>
#include <Kernel/Library/NonnullLockRefPtr.h>
#include <Kernel/Time/HardwareTimer.h>

namespace Kernel::RPi {

struct TimerRegisters;

class Timer final : public HardwareTimer<IRQHandler> {
public:
    virtual ~Timer();

    static NonnullLockRefPtr<Timer> initialize(Function<void(RegisterState const&)>);
    // Timer(Function<void(RegisterState const&)>);

    virtual HardwareTimerType timer_type() const override { return HardwareTimerType::i8253; }
    virtual StringView model() const override { return "i8254"sv; }
    virtual size_t ticks_per_second() const override;

    virtual bool is_periodic() const override { return true; }
    virtual bool is_periodic_capable() const override { return true; }
    virtual void set_periodic() override { }
    virtual void set_non_periodic() override { }
    virtual void disable() override { }

    virtual void reset_to_default_ticks_per_second() override;
    virtual bool try_to_set_frequency(size_t frequency) override;
    virtual bool is_capable_of_frequency(size_t frequency) const override;
    virtual size_t calculate_nearest_possible_frequency(size_t frequency) const override;

    u64 microseconds_since_boot();

    void set_interrupt_interval_usec(u32);
    void enable_interrupt_mode();

    enum class ClockID {
        Reserved = 0,
        EMMC = 1,
        UART = 2,
        ARM = 3,
        CORE = 4,
        V3D = 5,
        H264 = 6,
        ISP = 7,
        SDRAM = 8,
        PIXEL = 9,
        PWM = 10,
        HEVC = 11,
        EMMC2 = 12,
        M2MC = 13,
        PIXEL_BVB = 14,
    };
    static u32 set_clock_rate(ClockID, u32 rate_hz, bool skip_setting_turbo = true);

private:
    explicit Timer(Function<void(RegisterState const&)>);

    enum class TimerID : u32 {
        Timer0 = 0,
        Timer1 = 1,
        Timer2 = 2,
        Timer3 = 3,
    };
    void set_compare(TimerID, u32 compare);
    void clear_interrupt(TimerID);

    //^ IRQHandler
    virtual bool handle_irq(RegisterState const&) override;

    TimerRegisters volatile* m_registers;
    u32 m_interrupt_interval { 0 };
    u32 m_current_timer_value { 0 };
};

}
