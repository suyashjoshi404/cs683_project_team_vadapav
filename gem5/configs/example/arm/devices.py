# Copyright (c) 2016-2017, 2019, 2021-2023, 2025 Arm Limited
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

# System components used by the bigLITTLE.py configuration script

import m5
from m5.objects import *

m5.util.addToPath("../../")
from common import ObjectList
from common.Caches import *

# Import the walker cache hierarchy
from gem5.components.cachehierarchies.classic.private_l1_private_l2_walk_cache_hierarchy import (
    PrivateL1PrivateL2WalkCacheHierarchy,
)
from gem5.components.cachehierarchies.classic.caches.mmu_cache import MMUCache


have_kvm = "ArmV8KvmCPU" in ObjectList.cpu_list.get_names()
have_fastmodel = "FastModelCortexA76" in ObjectList.cpu_list.get_names()


class L1I(L1_ICache):
    tag_latency = 1
    data_latency = 1
    response_latency = 1
    mshrs = 16
    tgts_per_mshr = 20
    size = "64KiB"
    assoc = 8
    write_buffers = 16


class L1D(L1_DCache):
    tag_latency = 4
    data_latency = 4
    response_latency = 1
    mshrs = 16
    tgts_per_mshr = 20
    size = "48KiB"
    assoc = 12
    write_buffers = 16


class L2(L2Cache):
    tag_latency = 15
    data_latency = 15
    response_latency = 1
    mshrs = 32
    tgts_per_mshr = 20
    size = "2MiB"
    assoc = 16
    write_buffers = 32
    clusivity = "mostly_excl"
    lru_ways = 8
    preserve_ways = 8


class L3(Cache):
    size = "3MiB"
    assoc = 12
    tag_latency = 32
    data_latency = 32
    response_latency = 1
    mshrs = 64
    tgts_per_mshr = 20
    write_buffers = 64
    clusivity = "mostly_excl"

class WalkCache(PageTableWalkerCache):
   tag_latency = 2
   data_latency = 2
   response_latency = 0
   mshrs = 16
   tgts_per_mshr = 20
   size = '32KiB'
   assoc = 4
   write_buffers = 16


class MemBus(SystemXBar):
    badaddr_responder = BadAddr(warn_access="warn")
    default = Self.badaddr_responder.pio


class ArmCpuCluster(CpuCluster):
    def __init__(
        self,
        system,
        num_cpus,
        cpu_clock,
        cpu_voltage,
        cpu_type,
        l1i_type,
        l1d_type,
        l2_type,
        l1i_rp=None,
        l1d_rp=None,
        l2_rp=None,
        l1i_hwp=None,
        l1d_hwp=None,
        l2_hwp=None,
        bp_type=None,
        indirect_bp_type=None,
        tarmac_gen=False,
        tarmac_dest=None,
        args=None,
    ):
        super().__init__()
        self._cpu_type = cpu_type
        self._l1i_type = l1i_type
        self._l1d_type = l1d_type
        self._l2_type = l2_type
        self._l1i_rp = l1i_rp
        self._l1d_rp = l1d_rp
        self._l2_rp = l2_rp
        self._l1i_hwp = l1i_hwp
        self._l1d_hwp = l1d_hwp
        self._l2_hwp = l2_hwp
        self._bp_type = bp_type
        self._indirect_bp_type = indirect_bp_type
        # Store args for later use in cache configuration
        self._args = args

        self.voltage_domain = VoltageDomain(voltage=cpu_voltage)
        self.clk_domain = SrcClockDomain(
            clock=cpu_clock, voltage_domain=self.voltage_domain
        )

        self.generate_cpus(cpu_type, num_cpus)
        print(cpu_type)
        for cpu in self.cpus:
            if tarmac_gen:
                cpu.tracer = TarmacTracer()
                if tarmac_dest is not None:
                    cpu.tracer.outfile = tarmac_dest
            if args.maxinsts:
                cpu.max_insts_any_thread = args.maxinsts
                if args.warmup_insts:
                    cpu.max_insts_any_thread += args.warmup_insts
            # Apply branch predictor if specified
            if self._bp_type is not None and hasattr(cpu, 'branchPred'):
                cpu.branchPred = ObjectList.bp_list.get(self._bp_type)()
                if args.num_btb_entries is not None and hasattr(cpu.branchPred, 'btb'):
                    import math
                    cpu.branchPred.btb.numEntries = args.num_btb_entries
                    cpu.branchPred.btb.tagBits = 64 - (math.log(cpu.branchPred.btb.numEntries,2) + 2)
                    cpu.branchPred.btb.associativity = args.btb_associativity  
            
            # Apply indirect branch predictor if specified
            if self._indirect_bp_type is not None and hasattr(cpu, 'branchPred') and hasattr(cpu.branchPred, 'indirectBranchPred'):
                cpu.branchPred.indirectBranchPred = ObjectList.indirect_bp_list.get(self._indirect_bp_type)()

            if args.num_ras_entries is not None and hasattr(cpu, 'branchPred') and hasattr(cpu.branchPred, 'ras'):
                cpu.branchPred.ras.numEntries = args.num_ras_entries

            if args.disable_decoupled_frontend:
                cpu.decoupledFrontEnd = False
            else:
                cpu.branchPred.requiresBTBHit = True
                cpu.branchPred.takenOnlyHistory = True

            cpu.mmu.l2_shared.size= 2048
            cpu.mmu.l2_shared.assoc = 8
            cpu.minInstSize = 4

            if l2_rp == "LRUEmissaryRP":
                cpu.enableEMISSARY = True
                cpu.emissaryEnableIQEmpty = True
                cpu.enableStarvationEMISSARY = True
                cpu.starveRandomness = args.starve_randomness if args.starve_randomness is not None else 50

        system.addCpuCluster(self)

        """Apply branch predictors to all CPUs in the cluster"""
       
    # def applyInstructionLimits(self, maxinsts=None, warmup_insts=None):
    #     """Apply instruction limits to all CPUs in the cluster"""
    #     for cpu in self.cpus:
    #         # Apply maximum instructions if specified
    #         
            
    #         # Apply warmup instructions if specified (for CPU switching scenarios)
    #         if warmup_insts is not None and hasattr(cpu, 'max_insts_any_thread'):
    #             # Warmup instructions are typically used with CPU switching
    #             # This would be handled differently depending on the switching scenario
    #             pass

    def addL1(self):
        for cpu in self.cpus:
            l1i = None if self._l1i_type is None else self._l1i_type()
            l1d = None if self._l1d_type is None else self._l1d_type()
            # iwc = None if self._wcache_type is None else self._wcache_type()
            # dwc = None if self._wcache_type is None else self._wcache_type()
            
            # Apply replacement policies if specified
            if l1i is not None and self._l1i_rp is not None:
                l1i.replacement_policy = ObjectList.rp_list.get(self._l1i_rp)()
            if l1d is not None and self._l1d_rp is not None:
                l1d.replacement_policy = ObjectList.rp_list.get(self._l1d_rp)()
            
            # Apply prefetchers if specified
            if l1i is not None and self._l1i_hwp is not None:
                l1i.prefetcher = ObjectList.hwp_list.get(self._l1i_hwp)()
                # Set CPU parameter for FetchDirectedPrefetcher
                if hasattr(l1i.prefetcher, 'cpu'):
                    l1i.prefetcher.cpu = cpu
                # Register MMU for prefetchers that need it (e.g., FetchDirectedPrefetcher)
                if hasattr(l1i.prefetcher, 'registerMMU') and hasattr(cpu, 'mmu'):
                    l1i.prefetcher.registerMMU(cpu.mmu)
                    l1i.prefetcher.use_virtual_addresses = True
            if l1d is not None and self._l1d_hwp is not None:
                l1d.prefetcher = ObjectList.hwp_list.get(self._l1d_hwp)()
                l1d.prefetcher.use_virtual_addresses = True
            
            cpu.addPrivateSplitL1Caches(l1i, l1d)

    def addL2(self, clk_domain):
        if self._l2_type is None:
            return
        self.toL2Bus = L2XBar(width=64, clk_domain=clk_domain)
        self.l2 = self._l2_type()
        
        # Apply replacement policy if specified
        if self._l2_rp is not None:
            self.l2.replacement_policy = ObjectList.rp_list.get(self._l2_rp)()
        
        # Apply prefetcher if specified
        if self._l2_hwp is not None:
            self.l2.prefetcher = ObjectList.hwp_list.get(self._l2_hwp)()
        
        # Configure LRUEmissaryRP specific parameters
        if self._l2_rp == "LRUEmissaryRP":
            preserve_ways_value = self.l2.assoc/2
            if self._args is not None and hasattr(self._args, 'preserve_ways') and self._args.preserve_ways is not None:
                preserve_ways_value = self._args.preserve_ways
            
            # Set preserve_ways and calculate lru_ways
            self.l2.preserve_ways = preserve_ways_value
            self.l2.lru_ways = self.l2.assoc - preserve_ways_value

         # Add walker caches for each CPU before connecting ports
        for i, cpu in enumerate(self.cpus):
            if hasattr(cpu, 'mmu'):
                print(f"Adding walker caches for CPU {i}")
                # Create walker caches using the existing WalkCache class
                cpu.mmu.itb_walker_cache = WalkCache()
                cpu.mmu.dtb_walker_cache = WalkCache() 
                cpu.mmu.stage2_itb_walker_cache = WalkCache()
                cpu.mmu.stage2_dtb_walker_cache = WalkCache()
                
                # Connect walker caches to the L2 bus
                cpu.mmu.itb_walker_cache.mem_side = self.toL2Bus.cpu_side_ports
                cpu.mmu.dtb_walker_cache.mem_side = self.toL2Bus.cpu_side_ports
                cpu.mmu.stage2_itb_walker_cache.mem_side = self.toL2Bus.cpu_side_ports
                cpu.mmu.stage2_dtb_walker_cache.mem_side = self.toL2Bus.cpu_side_ports
                
                # Connect walker ports to walker caches instead of bus
                cpu.mmu.itb_walker.port = cpu.mmu.itb_walker_cache.cpu_side
                cpu.mmu.dtb_walker.port = cpu.mmu.dtb_walker_cache.cpu_side
                cpu.mmu.stage2_itb_walker.port = cpu.mmu.stage2_itb_walker_cache.cpu_side
                cpu.mmu.stage2_dtb_walker.port = cpu.mmu.stage2_dtb_walker_cache.cpu_side
                
                print(f"Connected walker caches for CPU {i}")

        # Connect CPUs to L2 bus through their L1 caches (standard approach)
        for cpu in self.cpus:
            # Connect L1 caches to L2 bus
            cpu.icache.mem_side = self.toL2Bus.cpu_side_ports
            cpu.dcache.mem_side = self.toL2Bus.cpu_side_ports
        self.toL2Bus.mem_side_ports = self.l2.cpu_side

    def addPMUs(
        self,
        ints,
        events=[],
        stat_counters=[],
        exit_sim_on_control=False,
        exit_sim_on_interrupt=False,
    ):
        """
        Instantiates 1 ArmPMU per PE. The method is accepting a list of
        interrupt numbers (ints) used by the PMU and a list of events to
        register in it.

        :param ints: List of interrupt numbers. The code will iterate over
            the cpu list in order and will assign to every cpu in the cluster
            a PMU with the matching interrupt.
        :type ints: List[int]
        :param events: Additional events to be measured by the PMUs
        :type events: List[Union[ProbeEvent, SoftwareIncrement]]
        :param exit_sim_on_control: If true, exit the sim loop when the PMU is
            enabled, disabled, or reset.
        :type exit_on_control: bool
        :param exit_sim_on_interrupt: If true, exit the sim loop when the PMU
            triggers an interrupt.
        :type exit_on_control: bool

        """
        # If ALL option has been passed, simply enable everything
        stat_counters = (
            EventTypeId.vals if "ALL" in stat_counters else stat_counters
        )

        assert len(ints) == len(self.cpus)
        for cpu, pint in zip(self.cpus, ints):
            int_cls = ArmPPI if pint < 32 else ArmSPI
            for isa in cpu.isa:
                isa.pmu = ArmPMU(interrupt=int_cls(num=pint))
                isa.pmu.exitOnPMUControl = exit_sim_on_control
                isa.pmu.exitOnPMUInterrupt = exit_sim_on_interrupt
                isa.pmu.addArchEvents(
                    cpu=cpu,
                    itb=cpu.mmu.itb,
                    dtb=cpu.mmu.dtb,
                    l2_shared=cpu.mmu.l2_shared,
                    icache=getattr(cpu, "icache", None),
                    dcache=getattr(cpu, "dcache", None),
                    l2cache=getattr(self, "l2", None),
                )
                for ev in events:
                    isa.pmu.addEvent(ev)

                isa.pmu.statCounters = isa.pmu.archStatCounters(*stat_counters)

    def connectMemSide(self, bus):
        try:
            self.l2.mem_side = bus.cpu_side_ports
        except AttributeError:
            for cpu in self.cpus:
                cpu.connectCachedPorts(bus.cpu_side_ports)


class AtomicCluster(ArmCpuCluster):
    def __init__(
        self,
        system,
        num_cpus,
        cpu_clock,
        cpu_voltage="1.0V",
        tarmac_gen=False,
        tarmac_dest=None,
    ):
        super().__init__(
            system,
            num_cpus,
            cpu_clock,
            cpu_voltage,
            cpu_type=ObjectList.cpu_list.get("AtomicSimpleCPU"),
            l1i_type=None,
            l1d_type=None,
            l2_type=None,
            l1i_rp=None,
            l1d_rp=None,
            l2_rp=None,
            l1i_hwp=None,
            l1d_hwp=None,
            l2_hwp=None,
            bp_type=None,
            indirect_bp_type=None,
            tarmac_gen=tarmac_gen,
            tarmac_dest=tarmac_dest,
        )

    def addL1(self):
        pass


class KvmCluster(ArmCpuCluster):
    def __init__(
        self,
        system,
        num_cpus,
        cpu_clock,
        cpu_voltage="1.0V",
        tarmac_gen=False,
        tarmac_dest=None,
    ):
        super().__init__(
            system,
            num_cpus,
            cpu_clock,
            cpu_voltage,
            cpu_type=ObjectList.cpu_list.get("ArmV8KvmCPU"),
            l1i_type=None,
            l1d_type=None,
            l2_type=None,
            l1i_rp=None,
            l1d_rp=None,
            l2_rp=None,
            l1i_hwp=None,
            l1d_hwp=None,
            l2_hwp=None,
            bp_type=None,
            indirect_bp_type=None,
            tarmac_gen=tarmac_gen,
            tarmac_dest=tarmac_dest,
        )

    def addL1(self):
        pass


class FastmodelCluster(CpuCluster):
    def __init__(self, system, num_cpus, cpu_clock, cpu_voltage="1.0V"):
        super().__init__()

        # Setup GIC
        gic = system.realview.gic
        gic.sc_gic.cpu_affinities = ",".join(
            ["0.0.%d.0" % i for i in range(num_cpus)]
        )

        # Parse the base address of redistributor.
        redist_base = gic.get_redist_bases()[0]
        redist_frame_size = 0x40000 if gic.sc_gic.has_gicv4_1 else 0x20000
        gic.sc_gic.reg_base_per_redistributor = ",".join(
            [
                "0.0.%d.0=%#x" % (i, redist_base + redist_frame_size * i)
                for i in range(num_cpus)
            ]
        )

        gic_a2t = AmbaToTlmBridge64(amba=gic.amba_m)
        gic_t2g = TlmToGem5Bridge64(
            tlm=gic_a2t.tlm, gem5=system.iobus.cpu_side_ports
        )
        gic_g2t = Gem5ToTlmBridge64(gem5=system.membus.mem_side_ports)
        gic_g2t.addr_ranges = gic.get_addr_ranges()
        gic_t2a = AmbaFromTlmBridge64(tlm=gic_g2t.tlm)
        gic.amba_s = gic_t2a.amba

        system.gic_hub = SubSystem()
        system.gic_hub.gic_a2t = gic_a2t
        system.gic_hub.gic_t2g = gic_t2g
        system.gic_hub.gic_g2t = gic_g2t
        system.gic_hub.gic_t2a = gic_t2a

        self.voltage_domain = VoltageDomain(voltage=cpu_voltage)
        self.clk_domain = SrcClockDomain(
            clock=cpu_clock, voltage_domain=self.voltage_domain
        )

        # Setup CPU
        assert num_cpus <= 4
        CpuClasses = [
            FastModelCortexA76x1,
            FastModelCortexA76x2,
            FastModelCortexA76x3,
            FastModelCortexA76x4,
        ]
        CpuClass = CpuClasses[num_cpus - 1]

        cpu = CpuClass(
            GICDISABLE=False, BROADCASTATOMIC=False, BROADCASTOUTER=False
        )
        for core in cpu.cores:
            core.semihosting_enable = False
            core.RVBARADDR = 0x10
            core.redistributor = gic.redistributor
            core.createThreads()
            core.createInterruptController()
        self.cpus = [cpu]

        self.cpu_hub = SubSystem()
        a2t = AmbaToTlmBridge64(amba=cpu.amba)
        t2g = TlmToGem5Bridge64(tlm=a2t.tlm, gem5=system.membus.cpu_side_ports)
        self.cpu_hub.a2t = a2t
        self.cpu_hub.t2g = t2g

        system.addCpuCluster(self)

    def require_caches(self):
        return False

    def memory_mode(self):
        return "atomic_noncaching"

    def addL1(self):
        pass

    def addL2(self, clk_domain):
        pass

    def connectMemSide(self, bus):
        pass


class ClusterSystem:
    """
    Base class providing cpu clusters generation/handling methods to
    SE/FS systems
    """

    def __init__(self, **kwargs):
        self._clusters = []

    def numCpuClusters(self):
        return len(self._clusters)

    def addCpuCluster(self, cpu_cluster):
        self._clusters.append(cpu_cluster)

    def addCaches(self, need_caches, last_cache_level):
        if not need_caches:
            # connect each cluster to the memory hierarchy
            for cluster in self._clusters:
                cluster.connectMemSide(self.membus)
            return

        cluster_mem_bus = self.membus
        assert last_cache_level >= 1 and last_cache_level <= 3
        for cluster in self._clusters:
            cluster.addL1()
        if last_cache_level > 1:
            for cluster in self._clusters:
                cluster.addL2(cluster.clk_domain)
        if last_cache_level > 2:
            max_clock_cluster = max(
                self._clusters, key=lambda c: c.clk_domain.clock[0]
            )
            self.l3 = L3(clk_domain=max_clock_cluster.clk_domain)
            self.toL3Bus = L2XBar(width=64)
            self.toL3Bus.mem_side_ports = self.l3.cpu_side
            self.l3.mem_side = self.membus.cpu_side_ports
            cluster_mem_bus = self.toL3Bus

        # connect each cluster to the memory hierarchy
        for cluster in self._clusters:
            cluster.connectMemSide(cluster_mem_bus)


class SimpleSeSystem(System, ClusterSystem):
    """
    Example system class for syscall emulation mode
    """

    # Use a fixed cache line size of 64 bytes
    cache_line_size = 64

    def __init__(self, **kwargs):
        System.__init__(self, **kwargs)
        ClusterSystem.__init__(self, **kwargs)
        # Create a voltage and clock domain for system components
        self.voltage_domain = VoltageDomain(voltage="3.3V")
        self.clk_domain = SrcClockDomain(
            clock="1GHz", voltage_domain=self.voltage_domain
        )

        # Create the off-chip memory bus.
        self.membus = SystemXBar()

    def connect(self):
        self.system_port = self.membus.cpu_side_ports


class BaseSimpleSystem(ArmSystem, ClusterSystem):
    cache_line_size = 64

    def __init__(self, mem_size, platform, **kwargs):
        ArmSystem.__init__(self, **kwargs)
        ClusterSystem.__init__(self, **kwargs)

        self.voltage_domain = VoltageDomain(voltage="1.0V")
        self.clk_domain = SrcClockDomain(
            clock="1GHz", voltage_domain=Parent.voltage_domain
        )

        if platform is None:
            self.realview = VExpress_GEM5_V1()
        else:
            self.realview = platform

        if hasattr(self.realview.gic, "cpu_addr"):
            self.gic_cpu_addr = self.realview.gic.cpu_addr

        self.terminal = Terminal()
        self.vncserver = VncServer()

        self.iobus = IOXBar()
        # Device DMA -> MEM
        self.mem_ranges = self.getMemRanges(int(Addr(mem_size)))

    def getMemRanges(self, mem_size):
        """
        Define system memory ranges. This depends on the physical
        memory map provided by the realview platform and by the memory
        size provided by the user (mem_size argument).
        The method is iterating over all platform ranges until they cover
        the entire user's memory requirements.
        """
        mem_ranges = []
        for mem_range in self.realview._mem_regions:
            size_in_range = min(mem_size, mem_range.size())

            mem_ranges.append(
                AddrRange(start=mem_range.start, size=size_in_range)
            )

            mem_size -= size_in_range
            if mem_size == 0:
                return mem_ranges

        raise ValueError("memory size too big for platform capabilities")


class SimpleSystem(BaseSimpleSystem):
    """
    Meant to be used with the classic memory model
    """

    def __init__(self, caches, mem_size, platform=None, **kwargs):
        super().__init__(mem_size, platform, **kwargs)

        self.membus = MemBus()
        # CPUs->PIO
        self.iobridge = Bridge(delay="50ns")

        self._caches = caches
        if self._caches:
            self.iocache = IOCache(addr_ranges=self.mem_ranges)
        else:
            self.dmabridge = Bridge(delay="50ns", ranges=self.mem_ranges)

    def connect(self):
        self.iobridge.mem_side_port = self.iobus.cpu_side_ports
        self.iobridge.cpu_side_port = self.membus.mem_side_ports

        if self._caches:
            self.iocache.mem_side = self.membus.cpu_side_ports
            self.iocache.cpu_side = self.iobus.mem_side_ports
        else:
            self.dmabridge.mem_side_port = self.membus.cpu_side_ports
            self.dmabridge.cpu_side_port = self.iobus.mem_side_ports

        if hasattr(self.realview.gic, "cpu_addr"):
            self.gic_cpu_addr = self.realview.gic.cpu_addr
        self.realview.attachOnChipIO(self.membus, self.iobridge)
        self.realview.attachIO(self.iobus)
        self.system_port = self.membus.cpu_side_ports

    def attach_pci(self, dev):
        self.realview.attachPciDevice(dev, self.iobus)


class ArmRubySystem(BaseSimpleSystem):
    """
    Meant to be used with ruby
    """

    def __init__(self, mem_size, platform=None, **kwargs):
        super().__init__(mem_size, platform, **kwargs)
        self._dma_ports = []
        self._mem_ports = []

    def connect(self):
        self.realview.attachOnChipIO(
            self.iobus, dma_ports=self._dma_ports, mem_ports=self._mem_ports
        )

        self.realview.attachIO(self.iobus, dma_ports=self._dma_ports)

        for cluster in self._clusters:
            for i, cpu in enumerate(cluster.cpus):
                self.ruby._cpu_ports[i].connectCpuPorts(cpu)

    def attach_pci(self, dev):
        self.realview.attachPciDevice(
            dev, self.iobus, dma_ports=self._dma_ports
        )
