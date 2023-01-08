/*
 * Copyright (c) 2018-2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <AK/Types.h>
#include <Kernel/VirtualAddress.h>

namespace Kernel {

struct PageFaultFlags {
    enum Flags {
        NotPresent = 0x00,
        ProtectionViolation = 0x01,
        Read = 0x00,
        Write = 0x02,
        UserMode = 0x04,
        SupervisorMode = 0x00,
        ReservedBitViolation = 0x08,
        InstructionFetch = 0x10,
    };
};

class PageFault {
public:
    PageFault(u16 code, VirtualAddress vaddr)
        : m_vaddr(vaddr)
    {
        m_type = (Type)(code & 1);
        m_access = (Access)(code & 2);
        m_mode = (Mode)(code & 4);
        m_is_reserved_bit_violation = (code & 8) == PageFaultFlags::ReservedBitViolation;
        m_is_instruction_fetch = (code & 16) == PageFaultFlags::InstructionFetch;
    }

    explicit PageFault(VirtualAddress vaddr)
        : m_vaddr(vaddr)
    {
    }

    enum class Type {
        PageNotPresent = PageFaultFlags::NotPresent,
        ProtectionViolation = PageFaultFlags::ProtectionViolation,
    };

    enum class Access {
        Read = PageFaultFlags::Read,
        Write = PageFaultFlags::Write,
    };

    enum class Mode {
        Supervisor = PageFaultFlags::SupervisorMode,
        User = PageFaultFlags::UserMode,
    };

    VirtualAddress vaddr() const { return m_vaddr; }
    u16 code() const
    {
        u16 code = 0;
        code |= (u16)m_type;
        code |= (u16)m_access;
        code |= (u16)m_mode;
        code |= m_is_reserved_bit_violation ? 8 : 0;
        code |= m_is_instruction_fetch ? 16 : 0;
        return code;
    }

    void set_type(Type type) { m_type = type; }
    Type type() const { return m_type; }

    void set_access(Access access) { m_access = access; }
    Access access() const { return m_access; }

    void set_mode(Mode mode) { m_mode = mode; }
    Mode mode() const { return m_mode; }

    bool is_not_present() const { return m_type == Type::PageNotPresent; }
    bool is_protection_violation() const { return m_type == Type::ProtectionViolation; }
    bool is_read() const { return m_access == Access::Read; }
    bool is_write() const { return m_access == Access::Write; }
    bool is_user() const { return m_mode == Mode::User; }
    bool is_supervisor() const { return m_mode == Mode::Supervisor; }
    bool is_reserved_bit_violation() const { return m_is_reserved_bit_violation; }
    bool is_instruction_fetch() const { return m_is_instruction_fetch; }

private:
    Type m_type;
    Access m_access;
    Mode m_mode;
    bool m_is_reserved_bit_violation { false };
    bool m_is_instruction_fetch { false };

    VirtualAddress m_vaddr;
};

}
