# Copyright (c) 2016-2017, 2020, 2022 Arm Limited
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""This script is the full system example script from the ARM
Research Starter Kit on System Modeling. More information can be found
at: http://www.arm.com/ResearchEnablement/SystemModeling
"""

import argparse
import os
import sys

import m5

from m5.objects import *
from m5.options import *
from m5.util import addToPath

m5.util.addToPath("../..")

import devices
from common import (
    Options,
    CacheConfig,
    MemConfig,
    ObjectList,
    SysPaths,
)
from common.cores.arm import (
    HPI,
    O3_ARM_v7a,
)

default_kernel = "vmlinux.arm64"
default_disk = "linaro-minimal-aarch64.img"
default_root_device = "/dev/vda2"


# Pre-defined CPU configurations. Each tuple must be ordered as : (cpu_class,
# l1_icache_class, l1_dcache_class, l2_Cache_class). Any of
# the cache class may be 'None' if the particular cache is not present.
cpu_types = {
    "atomic": (AtomicSimpleCPU, None, None, None),
    "minor": (MinorCPU, devices.L1I, devices.L1D, devices.L2),
    "hpi": (HPI.HPI, HPI.HPI_ICache, HPI.HPI_DCache, HPI.HPI_L2),
    "o3": (
        O3_ARM_v7a.O3_ARM_v7a_RedwoodCove,
        devices.L1I,
        devices.L1D,
        devices.L2,
    ),
}


def create_cow_image(name):
    """Helper function to create a Copy-on-Write disk image"""
    image = CowDiskImage()
    image.child.image_file = SysPaths.disk(name)

    return image


def create(args):
    """Create and configure the system object."""

    if args.script and not os.path.isfile(args.script):
        print(f"Error: Bootscript {args.script} does not exist")
        sys.exit(1)

    cpu_class = cpu_types[args.cpu][0]
    mem_mode = cpu_class.memory_mode()
    # Only simulate caches when using a timing CPU (e.g., the HPI model)
    want_caches = True if mem_mode == "timing" else False

    system = devices.SimpleSystem(
        want_caches,
        args.mem_size,
        mem_mode=mem_mode,
        workload=ArmFsLinux(object_file=SysPaths.binary(args.kernel)),
        readfile=args.script,
    )

    MemConfig.config_mem(args, system)

    # Add the PCI devices we need for this system. The base system
    # doesn't have any PCI devices by default since they are assumed
    # to be added by the configuration scripts needing them.
    system.pci_devices = [
        # Create a VirtIO block device for the system's boot
        # disk. Attach the disk image using gem5's Copy-on-Write
        # functionality to avoid writing changes to the stored copy of
        # the disk image.
        PciVirtIO(vio=VirtIOBlock(image=create_cow_image(args.disk_image)))
    ]

    # Attach the PCI devices to the system. The helper method in the
    # system assigns a unique PCI bus ID to each of the devices and
    # connects them to the IO bus.
    for dev in system.pci_devices:
        system.attach_pci(dev)

    # Wire up the system's memory system
    system.connect()

    # Add CPU clusters to the system
    system.cpu_cluster = [
        devices.ArmCpuCluster(
            system,
            args.num_cores,
            args.cpu_freq,
            "1.0V",
            *cpu_types[args.cpu],
            l1i_rp=args.l1i_rp_type,
            l1d_rp=args.l1d_rp_type,
            l2_rp=args.l2_rp_type,
            l1i_hwp=args.l1i_hwp_type,
            l1d_hwp=args.l1d_hwp_type,
            l2_hwp=args.l2_hwp_type,
            bp_type=args.bp_type,
            indirect_bp_type=args.indirect_bp_type,
            tarmac_gen=args.tarmac_gen,
            tarmac_dest=args.tarmac_dest,
            args=args,
        )
    ]

    # Create a cache hierarchy for the cluster. We are assuming that
    # clusters have core-private L1 caches and an L2 that's shared
    # within the cluster.
    system.addCaches(want_caches, last_cache_level=3)
    

    # Setup gem5's minimal Linux boot loader.
    system.realview.setupBootLoader(system, SysPaths.binary)

    if args.dtb:
        system.workload.dtb_filename = args.dtb
    else:
        # No DTB specified: autogenerate DTB
        system.workload.dtb_filename = os.path.join(
            m5.options.outdir, "system.dtb"
        )
        system.generateDtb(system.workload.dtb_filename)

    if args.initrd:
        system.workload.initrd_filename = args.initrd

    # Linux boot command flags
    kernel_cmd = [
        # Tell Linux to use the simulated serial port as a console
        "console=ttyAMA0",
        # Hard-code timi
        "lpj=19988480",
        # Disable address space randomisation to get a consistent
        # memory layout.
        "norandmaps",
        # Tell Linux where to find the root disk image.
        f"root={args.root_device}",
        # Mount the root disk read-write by default.
        "rw",
        # Tell Linux about the amount of physical memory present.
        f"mem={args.mem_size}",
    ]
    system.workload.command_line = " ".join(kernel_cmd)

    if args.with_pmu:
        for cluster in system.cpu_cluster:
            interrupt_numbers = [args.pmu_ppi_number] * len(cluster)
            cluster.addPMUs(interrupt_numbers)

    return system


def parse_stats(args):
    stats_file = os.path.join(m5.options.outdir, 'stats.txt')
    
    if not os.path.exists(stats_file):
        print(f"Warning: stats file {stats_file} does not exist")
        return False
        
    warmup_instcount = 5000000
    if args.warmup_insts:
        warmup_instcount = args.warmup_insts
    
    try:
        with open(stats_file, 'r') as stats_file_handle:
            for line in stats_file_handle:
                if "simInsts" in line and not line.strip().startswith('#'):
                    toks = line.split()
                    if len(toks) >= 2:
                        try:
                            inst_count = int(toks[1])
                            print(f"Current instruction count: {inst_count}, Target: {warmup_instcount}")
                            if inst_count >= warmup_instcount:
                                return True
                        except ValueError:
                            continue
    except Exception as e:
        print(f"Error reading stats file: {e}")
        return False
    
    return False

def run(args, system):
    cptdir = m5.options.outdir
    if args.checkpoint:
        print(f"Checkpoint directory: {cptdir}")
    
    # Warmup phase
    if args.warmup_insts:
        print(f"Warmup instructions: {args.warmup_insts}")
        # Set warmup instruction limit on all CPUs
        for cluster in system.cpu_cluster:
            for cpu in cluster.cpus:
                cpu.max_insts_any_thread = args.warmup_insts
        
        warmup_complete = False
        while not warmup_complete:
            # Simulate in chunks to check progress
            event = m5.simulate(250000000)  # 250M ticks at a time
            m5.stats.dump()
            
            # Check if we've reached warmup instruction count
            if parse_stats(args):
                print("Warmup done.")
                warmup_complete = True
                break
                
            # Check if simulation ended unexpectedly
            exit_msg = event.getCause()
            if exit_msg != "simulate() limit reached":
                print(f"Simulation ended during warmup: {exit_msg} @ {m5.curTick()}")
                warmup_complete = True
                
        # Reset stats after warmup
        print("Resetting stats after warmup...")
        m5.stats.dump()
        m5.stats.reset()
            # Copy final stats to separate file if warmup was used
        if args.warmup_insts:
            import shutil
            stats_file = os.path.join(m5.options.outdir, "stats.txt")
            final_stats_file = os.path.join(m5.options.outdir, "stats_warmup.txt")
            if os.path.exists(stats_file):
                shutil.copy2(stats_file, final_stats_file)
                with open(stats_file, 'w') as f:
                    f.write("")
                print(f"warmup stats has been renamed to {final_stats_file}")
        m5.stats.reset()
        # Set up new output destination for main simulation
        with open(os.path.join(m5.options.outdir, "stats.txt"), 'w') as f:
            f.write("Final stats after warmup\n")

    # Main simulation phase  
    max_insts_msg = f" with max instructions: {args.maxinsts}" if args.maxinsts else ""
    print(f"Starting main simulation{max_insts_msg}")

    # Set max instruction limit if specified
    if args.maxinsts:
        # After warmup, we need to set the total instruction limit 
        # (warmup + main simulation instructions)
        total_insts = args.maxinsts
        if args.warmup_insts:
            total_insts += args.warmup_insts
            print(f"Setting total instruction limit: {total_insts} (warmup: {args.warmup_insts} + main: {args.maxinsts})")
        else:
            print(f"Setting instruction limit: {total_insts}")
            
        # Set total instruction limit on each CPU
        for cluster in system.cpu_cluster:
            for cpu in cluster.cpus:
                cpu.max_insts_any_thread = total_insts

    while True:
        event = m5.simulate()
        exit_msg = event.getCause()
        
        if exit_msg == "checkpoint":
            print("Dropping checkpoint at tick %d" % m5.curTick())
            cpt_dir = os.path.join(m5.options.outdir, "cpt.%d" % m5.curTick())
            m5.checkpoint(os.path.join(cpt_dir))
            print("Checkpoint done.")
        else:
            print(f"{exit_msg} @ {m5.curTick()}")
            break
    
    # Dump final stats
    print("Dumping final statistics...")
    m5.stats.dump()
    
    if args.warmup_insts:
        import shutil
        stats_file = os.path.join(m5.options.outdir, "stats.txt")

        final_stats_file = os.path.join(m5.options.outdir, "stats_final.txt")
        if os.path.exists(stats_file):
            with open(stats_file, 'r' ) as f:
                lines = f.readlines()
            # Remove the second line if it exists
            if len(lines) > 1:
                lines.pop(1)  # Remove line at index 1 (second line)
            with open(stats_file, 'w') as f:
                f.writelines(lines)
            shutil.move(stats_file, final_stats_file)
            print(f"Simulation stats has been renamed to {final_stats_file}")

    
    sys.exit(event.getCode())


def arm_ppi_arg(int_num: int) -> int:
    """Argparse argument parser for valid Arm PPI numbers."""
    # PPIs (1056 <= int_num <= 1119) are not yet supported by gem5
    int_num = int(int_num)
    if 16 <= int_num <= 31:
        return int_num
    raise ValueError(f"{int_num} is not a valid Arm PPI number")


def main():
    parser = argparse.ArgumentParser(epilog=__doc__)

    parser.add_argument(
        "--dtb", type=str, default=None, help="DTB file to load"
    )
    parser.add_argument(
        "--kernel", type=str, default=default_kernel, help="Linux kernel"
    )
    parser.add_argument(
        "--initrd",
        type=str,
        default=None,
        help="initrd/initramfs file to load",
    )
    parser.add_argument(
        "--disk-image",
        type=str,
        default=default_disk,
        help="Disk to instantiate",
    )
    parser.add_argument(
        "--root-device",
        type=str,
        default=default_root_device,
        help=f"OS device name for root partition (default: {default_root_device})",
    )
    parser.add_argument(
        "--script", type=str, default="", help="Linux bootscript"
    )
    parser.add_argument(
        "--cpu",
        type=str,
        choices=list(cpu_types.keys()),
        default="atomic",
        help="CPU model to use",
    )
    parser.add_argument("--cpu-freq", type=str, default="4GHz")
    parser.add_argument(
        "--num-cores", type=int, default=1, help="Number of CPU cores"
    )
    # parser.add_argument(
    #     "--mem-type",
    #     # default="DDR3_1600_8x8",
    #     default="DDR5_6400_4x8",
    #     choices=ObjectList.mem_list.get_names(),
    #     help="type of memory to use",
    # )
    # parser.add_argument(
    #     "--mem-channels", type=int, default=1, help="number of memory channels"
    # )
    # parser.add_argument(
    #     "--mem-ranks",
    #     type=int,
    #     default=None,
    #     help="number of memory ranks per channel",
    # )
    # parser.add_argument(
    #     "--mem-size",
    #     action="store",
    #     type=str,
    #     default="16GiB",
    #     help="Specify the physical memory size",
    # )
    parser.add_argument(
        "--tarmac-gen",
        action="store_true",
        help="Write a Tarmac trace.",
    )
    parser.add_argument(
        "--tarmac-dest",
        choices=TarmacDump.vals,
        default="stdoutput",
        help="Destination for the Tarmac trace output. [Default: stdoutput]",
    )
    parser.add_argument(
        "--with-pmu",
        action="store_true",
        help="Add a PMU to each core in the cluster.",
    )
    parser.add_argument(
        "--pmu-ppi-number",
        type=arm_ppi_arg,
        default=23,
        help="The number of the PPI to use to connect each PMU to its core. "
        "Must be an integer and a valid PPI number (16 <= int_num <= 31).",
    )
    parser.add_argument("--checkpoint", action="store_true")
    parser.add_argument("--restore", type=str, default=None)

    #EMISSARY Randomness flags
    parser.add_argument("--preserve-ways", type=int, default=8)
    parser.add_argument("--starve-randomness", type=float, default=100.0,)

    Options.addCommonOptions(parser)

    args = parser.parse_args()

    root = Root(full_system=True)
    root.system = create(args)

    if args.restore is not None:
        m5.instantiate(args.restore)
    else:
        m5.instantiate()

    run(args, root.system)


if __name__ == "__m5_main__":
    main()
