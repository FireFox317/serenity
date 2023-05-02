/*
 * Copyright (c) 2023, Timon Kruiper <timonkruiper@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Kernel/Bus/USB/DWC2/DWC2Controller.h>
#include <Kernel/Memory/TypedMapping.h>

#if ARCH(AARCH64)
#    include <Kernel/Arch/aarch64/RPi/MMIO.h>
#endif

namespace Kernel::USB {

// Useful resources:
// - https://web.archive.org/web/20211125211715/http://www.capital-micro.com/PDF/CME-M7_Family_User_Guide_EN.pdf (Chapter 18, page 370)
// - https://static6.arrow.com/aropdfconversion/aa9a14376a75e7c5d6daa9f6aaed8411909d2021/rt3050_5x_v2.0_081408_0902.pdf (Chapter 3.19, page 130)
// - https://gitlab.com/qemu-project/qemu/-/blob/master/hw/usb/hcd-dwc2.c

// 18.5.2.2 Global CSR Map
struct CoreGlobalRegisters {
    u32 GOTGCTL;   // Control and Status Register
    u32 GOTGINT;   // Interrupt Register
    u32 GAHBCFG;   // AHB Configuration Register
    u32 GUSBCFG;   // USB Configuration Register
    u32 GRSTCTL;   // Reset Register
    u32 GINTSTS;   // Interrupt Register
    u32 GINTMSK;   // Interrupt Mask Register
    u32 GRXSTSR;   // Receive Status Debug Read / Status Read and Pop Registers
    u32 GRXSTSP;   // Receive Status Debug Read / Status Read and Pop Registers
    u32 GRXFSIZ;   // Recevice FIFO Size Register
    u32 GNPTXFSIZ; // Non-Periodic Transmit FIFO Size Register
    u32 GNPTXSTS;  // Non-Periodic Transmit FIFO / Queue Status Register
    u32 GI2CCTL;   // I2C Access Register
    u32 GPVNDCTL;  // PHY Vendor Control Register
    u32 GGPIO;     // General Purpose Input/Output Register
    u32 GUID;      // User ID Register
    union {
        struct {
            u32 revision_minor : 12;
            u32 revision_major : 4;
            u32 id : 16;
        };
        u32 raw;
    } GSNPSID;        // Synopsys ID Register
    u32 GHWCFG1;      // User HW Config1 Register
    u32 GHWCFG2;      // User HW Config2 Register
    u32 GHWCFG3;      // User HW Config3 Register
    u32 GHWCFG4;      // User HW Config4 Register
    u32 GLPMCFG;      // Core LPM Configuration Register
    u32 GPWRDN;       // Power Down Register
    u32 GDFIFOCFG;    // DFIFO Software Config Register
    u32 GADPCTL;      // ADP Timer, Control and Status Register
    u32 _reserved0[0x27];
    u32 HPTXFSIZ;     // Host Periodic Transmit FIFO Size Register
    u32 DPTXFSIZ[16]; // Array of Device Periodic Transmit FIFO-n Size / Device IN Endpoint Transmit FIFO Size Register (DIEPTXFn)
    u32 _reserved1[0xaf];
};
static_assert(AssertSize<CoreGlobalRegisters, 1024>()); // 1KiB

// Part of 18.5.2.3 Host Mode CSR Map
struct HostChannelRegisters {
    u32 HCCHAR;   // Host Channel-n Characteristics Register
    u32 HCSPLT;   // Host Channel-n Split Control Register
    u32 HCINT;    // Host Channel-n Interrupt Register
    u32 HCINTMSK; // Host Channel-n Interrupt Mask Register
    u32 HCTSIZ;   // Host Channel-n Transfer Size Register
    u32 HCDMA;    // Host Channel-n DMA Address Register
    u32 _reserved;
    u32 HCDMAB;   // Host Channel-n DMA Buffer Address Register
};
static_assert(AssertSize<HostChannelRegisters, 0x20>());

// 18.5.2.3 Host Mode CSR Map
struct HostModeRegisters {
    u32 HCFG;                    // Host Configuration Register
    u32 HFIR;                    // Host Frame Interval Register
    u32 HFNUM;                   // Host Frame Number/Frame Time Remaining Register
    u32 _reserved0;
    u32 HPTXSTS;                 // Host Periodic Transmit FIFO/Queue Status Register
    u32 HAINT;                   // Host All Channels Interrupt Register
    u32 HAINTMSK;                // Host All Channels Interrupt Mask Register
    u32 HFLBAddr;                // Host Frame List Base Address Register
    u32 _reserved1[0x8];
    u32 HPRT;                    // Host Port Control and Status Register
    u32 _reserved2[0x2f];
    HostChannelRegisters HC[16]; // Array of Host Channel Registers
    u32 _reserved3[0x40];
};
static_assert(AssertSize<HostModeRegisters, 1024>()); // 1KiB

// 18.5.2.4 Device Mode CSR Map
// TODO: Actually make this one correct, add arrays and correct reserved ranges etc.
struct DeviceModeRegisters {
    u32 DCFG;         // Device Configuration Register
    u32 DCTL;         // Device Control Register
    u32 DSTS;         // Device Status Register
    u32 _reserved0;
    u32 DIEPMSK;      // Device IN Endpoint Common Interrupt Mask Register
    u32 DOEPMSK;      // Device OUT Endpoint Common Interrupt Mask Register
    u32 DAINT;        // Device All Endpoints Interrupt Register
    u32 DAINTMSK;     // Device All Endpoints Interrupt Mask Register
    u32 DTKNQR1;      // Device IN Token Sequence Learning Queue Read Register 1
    u32 DTKNQR2;      // Device IN Token Sequence Learning Queue Read Register 2
    u32 DTKNQR3;      // Device IN Token Sequence Learning Queue Read Register 3
    u32 DTKNQR4;      // Device IN Token Sequence Learning Queue Read Register 4
    u32 DVBUSDIS;     // Device VBUS Discharge Time Register
    u32 DVBUSPULSE;   // Device VBUS Pulsing Time Register
    u32 DTHRCTL;      // Device Threshold Control Register
    u32 DIEPEMPMSK;   // Device IN Endpoint FIFO Empty Interrupt Mask Register
    u32 DEACHINT;     // Device Each Endpoint Interrupt Register
    u32 DEACHINTMSK;  // Device Each Endpoint Interrupt Register Mask
    u32 DIEPEACHMSKn; // Device Each In Endpoint-n Interrupt Register
    u32 DOEPEACHMSKn; // Device Each Out Endpoint-n Interrupt Register
    u32 DIEPCTL0;     // Device Control IN Endpoint 0 Control Register
    u32 _reserved1;
    u32 DIEPCTLn;     // Device Endpoint-n Control Register
    u32 DIEPINTn;     // Device Endpoint-n Interrupt Register
    u32 _reserved2;
    u32 DIEPTSIZ0;    // Device Endpoint 0 Transfer Size Register
    u32 DIEPTSIZn;    // Device Endpoint-n Transfer Size Register
    u32 DIEPDMAn;     // Device Endpoint-n DMA Address Register
    u32 DTXFSTSn;     // Device IN Endpoint Transmit FIFO Status Register
    // u32 DOEPDMABn;    // Device Endpoint-n DMA Buffer Address Register
    u32 DOEPCTL0;          // Device Control OUT Endpoint 0 Control Register
    u32 _reserved3;
    u32 DOEPCTLn;          // Device Endpoint-n Control Register
    u32 DOEPINTn;          // Device Endpoint-n Interrupt Register
    u32 _reserved4;
    u32 DOEPTSIZ0;         // Device Endpoint 0 Transfer Size Register
    u32 DOEPTSIZn;         // Device Endpoint-n Transfer Size Register
    u32 DOEPDMAn;          // Device Endpoint-n DMA Address Register
    u32 DOEPDMABn;         // Device Endpoint-n DMA Buffer Address Register
    u32 _reserved5[0x15a]; // TODO: This is not correct!
};
// TODO: Verify the DeviceModeRegisters layout!
static_assert(AssertSize<DeviceModeRegisters, 1536>()); // 1.5KiB

// 18.5.2.6 Power and Clock Gating CSR Map
struct PowerAndClockGatingRegisters {
    u32 PCGCCTL; // Power and Clock Gating Control Register
    u32 _reserved0[0x7f];
};
static_assert(AssertSize<PowerAndClockGatingRegisters, 512>()); // 0.5KiB

struct ControlAndStatusRegisters {
    CoreGlobalRegisters core_global_regs;                     // @ 0x0000
    HostModeRegisters host_mode_regs;                         // @ 0x0400
    DeviceModeRegisters device_mode_regs;                     // @ 0x0800
    PowerAndClockGatingRegisters power_and_clock_gating_regs; // @ 0x0E00
};
static_assert(AssertSize<ControlAndStatusRegisters, 4096>());

DWC2Controller::DWC2Controller(ControlAndStatusRegisters volatile* csr_regs)
    : m_csr_regs(csr_regs)
{
}

ErrorOr<NonnullLockRefPtr<DWC2Controller>> DWC2Controller::try_to_initialize()
{
#if ARCH(X86_64)
    return EIO;
#elif ARCH(AARCH64)
    // FIXME: Use TypedMapping, but that currently doesn't work for aarch64.
    auto* csr_regs = RPi::MMIO::the().peripheral<ControlAndStatusRegisters>(0x98'0000);
    auto controller = TRY(adopt_nonnull_lock_ref_or_enomem(new (nothrow) DWC2Controller(csr_regs)));
    TRY(controller->initialize());
    return controller;
#else
#    error "Unknown architecture"
#endif
}

ErrorOr<void> DWC2Controller::initialize()
{
    if (m_csr_regs->core_global_regs.GSNPSID.id != 0x4f54) {
        dbgln("DWC2Controller: Unknown DWC Core OTG: 0x{:x}", (u32)m_csr_regs->core_global_regs.GSNPSID.raw);
        return ENOTSUP;
    }
    auto revision_major = m_csr_regs->core_global_regs.GSNPSID.revision_major;
    auto revision_minor = m_csr_regs->core_global_regs.GSNPSID.revision_minor;
    dbgln("DWC2Controller: Revision: {:x}.{:x}", revision_major, revision_minor);

    if (revision_major != 2 or revision_minor != 0x94a)
        dbgln("DWC2Controller: Running with unsupported revision, the driver might not work!");

    dbgln("DWC2Controller: Receive FIFO Size: {}", (u32)m_csr_regs->core_global_regs.GRXFSIZ);
    return {};
}

ErrorOr<void> DWC2Controller::reset()
{
    return {};
}
ErrorOr<void> DWC2Controller::stop()
{
    return {};
}
ErrorOr<void> DWC2Controller::start()
{
    return {};
}

void DWC2Controller::cancel_async_transfer(NonnullLockRefPtr<Transfer>)
{
}
ErrorOr<size_t> DWC2Controller::submit_control_transfer(Transfer&)
{
    return 0;
}
ErrorOr<size_t> DWC2Controller::submit_bulk_transfer(Transfer&)
{
    return 0;
}
ErrorOr<void> DWC2Controller::submit_async_interrupt_transfer(NonnullLockRefPtr<Transfer>, u16)
{
    return {};
}

}
