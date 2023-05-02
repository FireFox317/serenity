/*
 * Copyright (c) 2023, Timon Kruiper <timonkruiper@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <Kernel/Bus/USB/USBController.h>
#include <Kernel/Forward.h>

namespace Kernel::USB {

struct ControlAndStatusRegisters;

class DWC2Controller final : public USBController {
public:
    static ErrorOr<NonnullLockRefPtr<DWC2Controller>> try_to_initialize();

    // ^USBController
    virtual ErrorOr<void> initialize() override;

    virtual ErrorOr<void> reset() override;
    virtual ErrorOr<void> stop() override;
    virtual ErrorOr<void> start() override;

    virtual void cancel_async_transfer(NonnullLockRefPtr<Transfer> transfer) override;
    virtual ErrorOr<size_t> submit_control_transfer(Transfer&) override;
    virtual ErrorOr<size_t> submit_bulk_transfer(Transfer& transfer) override;
    virtual ErrorOr<void> submit_async_interrupt_transfer(NonnullLockRefPtr<Transfer> transfer, u16 ms_interval) override;

private:
    explicit DWC2Controller(ControlAndStatusRegisters volatile*);

    ControlAndStatusRegisters volatile* m_csr_regs;
};

}
