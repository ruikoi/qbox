-- One QEMU hart and real Vortex AFU RTL in one SystemC kernel.
local path = debug.getinfo(1, "S").source:sub(2):match("(.*/)") or "./"
local artifacts = (os.getenv("VORTEX_ARTIFACTS") or (path.."out")).."/"
_OPENSBI_LOAD_ADDR = 0x80000000
_KERNEL64_LOAD_ADDR = 0x80200000
_DTB_LOAD_ADDR = 0x82800000
_INITRD_LOAD_ADDR = 0x88000000
dofile(path.."../../platforms/ubuntu/fw/riscv64_bootloader.lua")
platform = {
    moduletype="Container", quantum_ns=1000,
    router={moduletype="router"},
    qemu_inst_mgr={moduletype="QemuInstanceManager", construction_priority=-30},
    qemu_inst={moduletype="QemuInstance", construction_priority=-20, args={"&platform.qemu_inst_mgr", "RISCV64"},
               qemu_args="-nic none", tcg_mode="COROUTINE", sync_policy="tlm2", icount=true, icount_mips_shift=0},
    ram={moduletype="gs_memory",
         target_socket={address=0, size=0x100000000, priority=2048, bind="&router.initiator_socket"}},
    -- Shared GPU buffers remain TLM transactions, without host DMI caching.
    shared_ram={moduletype="gs_memory", dmi_allow=false,
                target_socket={address=0x20000000, size=0x4000000, bind="&router.initiator_socket"}},
    plic={moduletype="plic_sifive", args={"&platform.qemu_inst"},
          mem={address=0x0c000000, size=0x600000, bind="&router.initiator_socket"},
          num_sources=96, num_priorities=7, priority_base=0, pending_base=0x1000,
          enable_base=0x2000, enable_stride=0x80, context_base=0x200000,
          context_stride=0x1000, aperture_size=0x600000, hart_config="MS"},
    swi={moduletype="riscv_aclint_swi", args={"&platform.qemu_inst", "&platform.cpu"},
         mem={address=0x2000000, size=0x4000, bind="&router.initiator_socket"}, num_harts=1},
    timer={moduletype="riscv_aclint_mtimer", args={"&platform.qemu_inst"},
           mem={address=0x2004000, size=0x8000, bind="&router.initiator_socket"},
           num_harts=1, timecmp_base=0, time_base=0x7ff8, aperture_size=0x8000, provide_rdtime=true},
    reset={moduletype="sifive_test", args={"&platform.qemu_inst"},
           target_socket={address=0x100000, size=0x1000, bind="&router.initiator_socket"}},
    uart={moduletype="uart_16550", args={"&platform.qemu_inst"},
          mem={address=0x10000000, size=0x100, bind="&router.initiator_socket"},
          irq_out={bind="&plic.irq_in_10"}, regshift=0, baudbase=3686400},
    -- QEMU interrupt controllers look up realized harts during elaboration.
    cpu={moduletype="cpu_riscv64", construction_priority=-10, args={"&platform.qemu_inst", 0}, resetvec=0x1000, pmp=true,
         mem={bind="&router.target_socket"}, reset={bind="&reset.reset"},
         irq_in_7={bind="&timer.timer_irq_0"}},
    global_io={moduletype="global_peripheral_initiator", args={"&platform.qemu_inst", "&platform.cpu"},
               global_initiator={bind="&router.target_socket"}},
    gpu={moduletype="VortexGpuRtl", dylib_path="vortex_gpu_rtl",
         target_socket={address=0x40000000, size=0x20000, bind="&router.initiator_socket"},
         bus_master={bind="&router.target_socket"}, irq_out={bind="&plic.irq_in_12"}},
    vram={moduletype="gs_memory", dmi_allow=false,
          target_socket={address=0x100000000, size=0x100000000, bind="&router.initiator_socket"}},
    load={moduletype="loader", initiator_socket={bind="&router.target_socket"},
          {bin_file=artifacts.."fw_dynamic.bin", address=_OPENSBI_LOAD_ADDR},
          {bin_file=artifacts.."Image", address=_KERNEL64_LOAD_ADDR},
          {bin_file=artifacts.."platform.dtb", address=_DTB_LOAD_ADDR},
          {bin_file=artifacts.."riscv64-vortex-rtl.cpio.gz", address=_INITRD_LOAD_ADDR},
          {data=_bootloader_riscv64, address=0x10040},
          {data={0x0400f06f}, address=0x1000}},
}
