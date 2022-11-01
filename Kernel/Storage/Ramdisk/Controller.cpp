/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <AK/Types.h>
#include <Kernel/Library/LockRefPtr.h>
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

// extern "C" const u32 smaller_disk_image_start;
// extern "C" const u32 smaller_disk_image_size;

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
                dmesgln("whoop: {}", region_or_error.value()->vaddr());
                m_devices.append(RamdiskDevice::create(*this, region_or_error.release_value(), 6, count));
            }
            count++;
        }
    });

    // dbgln("smaller_disk_start: {}", (PhysicalAddress((PhysicalPtr) reinterpret_cast<u8 const*>(&smaller_disk_image_start))));
    // size_t length = Memory::page_round_up(smaller_disk_image_size).release_value_but_fixme_should_propagate_errors();
    // auto region_or_error = MM.allocate_kernel_region((PhysicalAddress((PhysicalPtr) reinterpret_cast<u8 const*>(&smaller_disk_image_start))), length, "Ramdisk"sv, Memory::Region::Access::ReadWrite);
    // m_devices.append(RamdiskDevice::create(*this, region_or_error.release_value(), 6, count));
}

RamdiskController::~RamdiskController() = default;

LockRefPtr<StorageDevice> RamdiskController::device(u32 index) const
{
    if (index >= m_devices.size())
        return nullptr;
    return m_devices[index];
}

}
