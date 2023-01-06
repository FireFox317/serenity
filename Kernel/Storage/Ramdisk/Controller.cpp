/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <AK/Types.h>
#include <Kernel/Library/LockRefPtr.h>
#include <Kernel/Panic.h>
#include <Kernel/Storage/Ramdisk/Controller.h>

namespace Kernel {

NonnullLockRefPtr<RamdiskController> RamdiskController::initialize()
{
    return adopt_lock_ref(*new RamdiskController());
}

bool RamdiskController::reset()
{
    TODO();
}

bool RamdiskController::shutdown()
{
    TODO();
}

size_t RamdiskController::devices_count() const
{
    return m_devices.size();
}

void RamdiskController::complete_current_request(AsyncDeviceRequest::RequestResult)
{
    VERIFY_NOT_REACHED();
}

extern "C" const u32 disk_image_start;
// extern "C" const u32 disk_image_end;
extern "C" const u32 disk_image_size;

RamdiskController::RamdiskController()
    : StorageController(0)
{
    // Populate ramdisk controllers from Multiboot boot modules, if any.
    size_t count = 0;
    MM.for_each_used_memory_range([&](auto& used_memory_range) {
        if (used_memory_range.type == Memory::UsedMemoryRangeType::BootModule) {
            size_t length = Memory::page_round_up(used_memory_range.end.get()).release_value_but_fixme_should_propagate_errors() - used_memory_range.start.get();
            auto region_or_error = MM.allocate_kernel_region(used_memory_range.start, length, "Ramdisk"sv, Memory::Region::Access::ReadWrite);
            if (region_or_error.is_error()) {
                dmesgln("RamdiskController: Failed to allocate kernel region of size {}", length);
            } else {
                m_devices.append(RamdiskDevice::create(*this, region_or_error.release_value(), 6, count));
            }
            count++;
        }
    });

    auto start = ((FlatPtr)&disk_image_start - 0x2000000000);
    // auto end = ((FlatPtr)&disk_image_end - 0x2000000000);
    // dbgln("start: {} end: {}", start, end);

    size_t length = Memory::page_round_up(disk_image_size).release_value_but_fixme_should_propagate_errors();
    // dbgln("disk_image_size: {}", length);
    // PANIC("HOI");
    auto region_or_error = MM.allocate_kernel_region(PhysicalAddress { start }, length, "Ramdisk"sv, Memory::Region::Access::ReadWrite);
    if (region_or_error.is_error()) {
        dmesgln("RamdiskController: Failed to allocate kernel region of size {}", length);
    } else {
        m_devices.append(RamdiskDevice::create(*this, region_or_error.release_value(), 6, count));
    }
    count++;
    if (count == 0)
        dmesgln("RamdiskController: No Ramdisks found!");
}

RamdiskController::~RamdiskController() = default;

LockRefPtr<StorageDevice> RamdiskController::device(u32 index) const
{
    if (index >= m_devices.size())
        return nullptr;
    return m_devices[index];
}

}
