/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Types.h>
#include <Kernel/Arch/InterruptManagement.h>
#include <Kernel/Arch/Processor.h>
#include <Kernel/BootInfo.h>
#include <Kernel/Bus/PCI/Access.h>
#include <Kernel/Bus/PCI/Initializer.h>
#include <Kernel/Bus/USB/USBManagement.h>
#include <Kernel/Bus/VirtIO/Device.h>
#include <Kernel/CommandLine.h>
#include <Kernel/Devices/Audio/Management.h>
#include <Kernel/Devices/DeviceControlDevice.h>
#include <Kernel/Devices/DeviceManagement.h>
#include <Kernel/Devices/FullDevice.h>
#include <Kernel/Devices/HID/HIDManagement.h>
#include <Kernel/Devices/KCOVDevice.h>
#include <Kernel/Devices/MemoryDevice.h>
#include <Kernel/Devices/NullDevice.h>
#include <Kernel/Devices/PCISerialDevice.h>
#include <Kernel/Devices/RandomDevice.h>
#include <Kernel/Devices/SelfTTYDevice.h>
#include <Kernel/Devices/SerialDevice.h>
#include <Kernel/Devices/ZeroDevice.h>
#include <Kernel/FileSystem/SysFS/Registry.h>
#include <Kernel/FileSystem/SysFS/Subsystems/Firmware/Directory.h>
#include <Kernel/FileSystem/VirtualFileSystem.h>
#include <Kernel/Firmware/ACPI/Initialize.h>
#include <Kernel/Firmware/ACPI/Parser.h>
#include <Kernel/Graphics/Console/BootFramebufferConsole.h>
#include <Kernel/Graphics/Console/VGATextModeConsole.h>
#include <Kernel/Graphics/GraphicsManagement.h>
#include <Kernel/Heap/kmalloc.h>
#include <Kernel/KSyms.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Multiboot.h>
#include <Kernel/Net/NetworkTask.h>
#include <Kernel/Net/NetworkingManagement.h>
#include <Kernel/Panic.h>
#include <Kernel/Prekernel/Prekernel.h>
#include <Kernel/Process.h>
#include <Kernel/Random.h>
#include <Kernel/Scheduler.h>
#include <Kernel/Sections.h>
#include <Kernel/Storage/StorageManagement.h>
#include <Kernel/TTY/ConsoleManagement.h>
#include <Kernel/TTY/PTYMultiplexer.h>
#include <Kernel/TTY/VirtualConsole.h>
#include <Kernel/Tasks/FinalizerTask.h>
#include <Kernel/Tasks/SyncTask.h>
#include <Kernel/Time/TimeManagement.h>
#include <Kernel/WorkQueue.h>
#include <Kernel/kstdio.h>

#include <Kernel/Arch/aarch64/RPi/Framebuffer.h>

// Defined in the linker script
typedef void (*ctor_func_t)();
extern ctor_func_t start_heap_ctors[];
extern ctor_func_t end_heap_ctors[];
extern ctor_func_t start_ctors[];
extern ctor_func_t end_ctors[];

extern uintptr_t __stack_chk_guard;
READONLY_AFTER_INIT uintptr_t __stack_chk_guard __attribute__((used));

// extern "C" u8 start_of_safemem_text[];
// extern "C" u8 end_of_safemem_text[];
// extern "C" u8 start_of_safemem_atomic_text[];
// extern "C" u8 end_of_safemem_atomic_text[];

extern "C" u8 end_of_kernel_image[];

multiboot_module_entry_t multiboot_copy_boot_modules_array[16];
size_t multiboot_copy_boot_modules_count;

READONLY_AFTER_INIT bool g_in_early_boot;

extern "C" const u32 disk_image_start;
extern "C" const u32 disk_image_size;

namespace Kernel {

[[noreturn]] static void init_stage2(void*);

// boot.S expects these functions to exactly have the following signatures.
// We declare them here to ensure their signatures don't accidentally change.
extern "C" void init_finished(u32 cpu) __attribute__((used));
extern "C" [[noreturn]] void init_ap(FlatPtr cpu, Processor* processor_info);
extern "C" [[noreturn]] void init(BootInfo const&);

READONLY_AFTER_INIT VirtualConsole* tty0;

ProcessID g_init_pid { 0 };

ALWAYS_INLINE static Processor& bsp_processor()
{
    // This solves a problem where the bsp Processor instance
    // gets "re"-initialized in init() when we run all global constructors.
    alignas(Processor) static u8 bsp_processor_storage[sizeof(Processor)];
    return (Processor&)bsp_processor_storage;
}

// SerenityOS Kernel C++ entry point :^)
//
// This is where C++ execution begins, after boot.S transfers control here.
//
// The purpose of init() is to start multi-tasking. It does the bare minimum
// amount of work needed to start the scheduler.
//
// Once multi-tasking is ready, we spawn a new thread that starts in the
// init_stage2() function. Initialization continues there.

Atomic<Graphics::Console*> g_boot_console;

extern "C" [[noreturn]] UNMAP_AFTER_INIT void init(BootInfo const&)
{
    g_in_early_boot = true;

    // FIXME: Don't hardcode this
    multiboot_memory_map_t mmap[] = {
        { sizeof(struct multiboot_mmap_entry) - sizeof(u32),
            (u64)0x0,
            (u64)0x3F000000,
            MULTIBOOT_MEMORY_AVAILABLE }
    };

    multiboot_memory_map = mmap;
    multiboot_memory_map_count = 1;

    multiboot_flags = 0x4;
    multiboot_copy_boot_modules_count = 1;
    auto disk_image_start_physical_addr = ((FlatPtr)&disk_image_start - kernel_load_base);
    multiboot_copy_boot_modules_array[0].start = disk_image_start_physical_addr;
    multiboot_copy_boot_modules_array[0].end = disk_image_start_physical_addr + disk_image_size;

    // start_of_prekernel_image = PhysicalAddress { boot_info.start_of_prekernel_image };
    // end_of_prekernel_image = PhysicalAddress { boot_info.end_of_prekernel_image };
    // physical_to_virtual_offset = boot_info.physical_to_virtual_offset;
    // kernel_mapping_base = boot_info.kernel_mapping_base;
    // kernel_load_base = boot_info.kernel_load_base;
    // gdt64ptr = boot_info.gdt64ptr;
    // code64_sel = boot_info.code64_sel;
    // boot_pml4t = PhysicalAddress { boot_info.boot_pml4t };
    // boot_pdpt = PhysicalAddress { boot_info.boot_pdpt };
    // boot_pd0 = PhysicalAddress { boot_info.boot_pd0 };
    // boot_pd_kernel = PhysicalAddress { boot_info.boot_pd_kernel };
    // boot_pd_kernel_pt1023 = (Memory::PageTableEntry*)boot_info.boot_pd_kernel_pt1023;
    // kernel_cmdline = (char const*)boot_info.kernel_cmdline;
    // multiboot_flags = boot_info.multiboot_flags;
    // multiboot_memory_map = (multiboot_memory_map_t*)boot_info.multiboot_memory_map;
    // multiboot_memory_map_count = boot_info.multiboot_memory_map_count;
    // multiboot_modules = (multiboot_module_entry_t*)boot_info.multiboot_modules;
    // multiboot_modules_count = boot_info.multiboot_modules_count;
    // multiboot_framebuffer_addr = PhysicalAddress { boot_info.multiboot_framebuffer_addr };
    // multiboot_framebuffer_pitch = boot_info.multiboot_framebuffer_pitch;
    // multiboot_framebuffer_width = boot_info.multiboot_framebuffer_width;
    // multiboot_framebuffer_height = boot_info.multiboot_framebuffer_height;
    // multiboot_framebuffer_bpp = boot_info.multiboot_framebuffer_bpp;
    // multiboot_framebuffer_type = boot_info.multiboot_framebuffer_type;

    // We need to copy the command line before kmalloc is initialized,
    // as it may overwrite parts of multiboot!
    CommandLine::early_initialize(kernel_cmdline);
    // memcpy(multiboot_copy_boot_modules_array, multiboot_modules, multiboot_modules_count * sizeof(multiboot_module_entry_t));
    // multiboot_copy_boot_modules_count = multiboot_modules_count;

    new (&bsp_processor()) Processor();
    bsp_processor().install(0);

    // Invoke the constructors needed for the kernel heap
    for (ctor_func_t* ctor = start_heap_ctors; ctor < end_heap_ctors; ctor++)
        (*ctor)();
    kmalloc_init();

    load_kernel_symbol_table();

    bsp_processor().initialize();

    CommandLine::initialize();
    Memory::MemoryManager::initialize(0);

    // NOTE: If the bootloader provided a framebuffer, then set up an initial console.
    // If the bootloader didn't provide a framebuffer, then set up an initial text console.
    // We do so we can see the output on the screen as soon as possible.
    // if (!kernel_command_line().is_early_boot_console_disabled()) {
    //     if (!multiboot_framebuffer_addr.is_null() && multiboot_framebuffer_type == MULTIBOOT_FRAMEBUFFER_TYPE_RGB) {
    //         g_boot_console = &try_make_lock_ref_counted<Graphics::BootFramebufferConsole>(multiboot_framebuffer_addr, multiboot_framebuffer_width, multiboot_framebuffer_height, multiboot_framebuffer_pitch).value().leak_ref();
    //     } else {
    //         g_boot_console = &Graphics::VGATextModeConsole::initialize().leak_ref();
    //     }
    // }

    auto& framebuffer = RPi::Framebuffer::the();
    if (framebuffer.initialized()) {
        g_boot_console = &try_make_lock_ref_counted<Graphics::BootFramebufferConsole>(PhysicalAddress((PhysicalPtr)framebuffer.gpu_buffer()), framebuffer.width(), framebuffer.height(), framebuffer.pitch()).value().leak_ref();
        // draw_logo(static_cast<Graphics::BootFramebufferConsole*>(g_boot_console.load())->unsafe_framebuffer_data());
    }

    dmesgln("Starting SerenityOS...");

    DeviceManagement::initialize();
    SysFSComponentRegistry::initialize();
    DeviceManagement::the().attach_null_device(*NullDevice::must_initialize());
    DeviceManagement::the().attach_console_device(*ConsoleDevice::must_create());
    DeviceManagement::the().attach_device_control_device(*DeviceControlDevice::must_create());

    MM.unmap_prekernel();

    // // Ensure that the safemem sections are not empty. This could happen if the linker accidentally discards the sections.
    // VERIFY(+start_of_safemem_text != +end_of_safemem_text);
    // VERIFY(+start_of_safemem_atomic_text != +end_of_safemem_atomic_text);

    // Invoke all static global constructors in the kernel.
    // Note that we want to do this as early as possible.
    for (ctor_func_t* ctor = start_ctors; ctor < end_ctors; ctor++)
        (*ctor)();

    InterruptManagement::initialize();

    // Initialize TimeManagement before using randomness!
    TimeManagement::initialize(0);

    __stack_chk_guard = get_fast_random<uintptr_t>();

    Process::initialize();

    Scheduler::initialize();

    {
        LockRefPtr<Thread> init_stage2_thread;
        (void)Process::create_kernel_process(init_stage2_thread, KString::must_create("init_stage2"sv), init_stage2, nullptr, THREAD_AFFINITY_DEFAULT, Process::RegisterProcess::No);
        // We need to make sure we drop the reference for init_stage2_thread
        // before calling into Scheduler::start, otherwise we will have a
        // dangling Thread that never gets cleaned up
    }

    Scheduler::start();
    VERIFY_NOT_REACHED();
}

void init_stage2(void*)
{
    // This is a little bit of a hack. We can't register our process at the time we're
    // creating it, but we need to be registered otherwise finalization won't be happy.
    // The colonel process gets away without having to do this because it never exits.
    Process::register_new(Process::current());

    VERIFY_INTERRUPTS_ENABLED();

    WorkQueue::initialize();

    // Initialize the PCI Bus as early as possible, for early boot (PCI based) serial logging
    PCI::initialize();
    if (!PCI::Access::is_disabled()) {
        PCISerialDevice::detect();
    }

    VirtualFileSystem::initialize();

    // if (!is_serial_debug_enabled())
    //     (void)SerialDevice::must_create(0).leak_ref();
    // (void)SerialDevice::must_create(1).leak_ref();
    // (void)SerialDevice::must_create(2).leak_ref();
    // (void)SerialDevice::must_create(3).leak_ref();

    MUST(HIDManagement::initialize());

    GraphicsManagement::the().initialize();
    ConsoleManagement::the().initialize();

    SyncTask::spawn();
    FinalizerTask::spawn();

    auto boot_profiling = kernel_command_line().is_boot_profiling_enabled();

    if (!PCI::Access::is_disabled()) {
        USB::USBManagement::initialize();
    }
    FirmwareSysFSDirectory::initialize();

    if (!PCI::Access::is_disabled()) {
        VirtIO::detect();
    }

    NetworkingManagement::the().initialize();

#ifdef ENABLE_KERNEL_COVERAGE_COLLECTION
    (void)KCOVDevice::must_create().leak_ref();
#endif
    (void)MemoryDevice::must_create().leak_ref();
    (void)ZeroDevice::must_create().leak_ref();
    (void)FullDevice::must_create().leak_ref();
    (void)RandomDevice::must_create().leak_ref();
    (void)SelfTTYDevice::must_create().leak_ref();
    PTYMultiplexer::initialize();

    AudioManagement::the().initialize();

    StorageManagement::the().initialize(kernel_command_line().root_device(), kernel_command_line().is_force_pio(), kernel_command_line().is_nvme_polling_enabled());
    if (VirtualFileSystem::the().mount_root(StorageManagement::the().root_filesystem()).is_error()) {
        PANIC("VirtualFileSystem::mount_root failed");
    }

    // Switch out of early boot mode.
    g_in_early_boot = false;

    // NOTE: Everything marked READONLY_AFTER_INIT becomes non-writable after this point.
    MM.protect_readonly_after_init_memory();

    // NOTE: Everything in the .ksyms section becomes read-only after this point.
    MM.protect_ksyms_after_init();

    // NOTE: Everything marked UNMAP_AFTER_INIT becomes inaccessible after this point.
    MM.unmap_text_after_init();

    LockRefPtr<Thread> thread;
    auto userspace_init = kernel_command_line().userspace_init();
    auto init_args = kernel_command_line().userspace_init_args();

    auto init_or_error = Process::try_create_user_process(thread, userspace_init, UserID(0), GroupID(0), move(init_args), {}, tty0);
    if (init_or_error.is_error())
        PANIC("init_stage2: Error spawning init process: {}", init_or_error.error());

    g_init_pid = init_or_error.value()->pid();

    thread->set_priority(THREAD_PRIORITY_HIGH);

    if (boot_profiling) {
        dbgln("Starting full system boot profiling");
        MutexLocker mutex_locker(Process::current().big_lock());
        auto const enable_all = ~(u64)0;
        auto result = Process::current().profiling_enable(-1, enable_all);
        VERIFY(!result.is_error());
    }

    NetworkTask::spawn();

    Process::current().sys$exit(0);
    VERIFY_NOT_REACHED();
}

// Define some Itanium C++ ABI methods to stop the linker from complaining.
// If we actually call these something has gone horribly wrong
void* __dso_handle __attribute__((visibility("hidden")));

}
