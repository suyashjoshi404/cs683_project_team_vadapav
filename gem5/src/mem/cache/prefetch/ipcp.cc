#include "mem/cache/prefetch/ipcp.hh"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/random.hh"
#include "base/trace.hh"
#include "debug/HWPrefetch.hh"
#include "mem/cache/base.hh"
#include "params/IPCPPrefetcher.hh"

namespace gem5
{

    GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
    namespace prefetch
    {

        IPCP::IPCP(const IPCPPrefetcherParams &p)
            : Queued(p),
              m_log_num_sets_in_recent_access_tag_array_l1(p.log_num_sets_in_recent_access_tag_array_l1),
              m_num_ways_in_recent_access_tag_array_l1(p.num_ways_in_recent_access_tag_array_l1),
              m_ip_table_tag_mask(p.ip_table_tag_mask),
              m_log_num_sets_in_ip_table_l1(p.log_num_sets_in_ip_table_l1),
              m_num_ways_in_ip_table_l1(p.num_ways_in_ip_table_l1),
              m_ip_delta_table_tag_mask(p.ip_delta_table_tag_mask),
              m_log_num_sets_in_ip_delta_table_l1(p.log_num_sets_in_ip_delta_table_l1),
              m_num_ways_in_ip_delta_table_l1(p.num_ways_in_ip_delta_table_l1),
              m_saturating_counter_max_l1(p.saturating_counter_max_l1),
              m_base_prefetch_degree_l1(p.base_prefetch_degree_l1),
              m_num_entries_in_nl_buffer_l1(p.num_entries_in_nl_buffer_l1),
              m_nl_threshold_numer_l1(p.nl_threshold_numer_l1),
              m_nl_threshold_denom_l1(p.nl_threshold_denom_l1),
              m_pointer_last(p.pointer_last),
              m_pointer_non_last(p.pointer_non_last),
              m_stride_conf_max(p.stride_conf_max),
              m_stride_conf_threshold(p.stride_conf_threshold),
              m_partial_ip_mask(p.partial_ip_mask),
              m_num_strides_in_long_hist_ip_table(p.num_strides_in_long_hist_ip_table),
              m_long_hist_ip_table_tag_mask(p.long_hist_ip_table_tag_mask),
              m_num_entries_in_long_hist_ip_table(p.num_entries_in_long_hist_ip_table),
              m_long_hist_match_length(p.long_hist_match_length),
              m_num_ip_table_l1_entries(p.num_ip_table_l1_entries),
              m_num_ghb_entries(p.num_ghb_entries),
              m_num_ip_index_bits(p.num_ip_index_bits),
              m_num_ip_tag_bits(p.num_ip_tag_bits),
              m_s_type(p.s_type),
              m_cs_type(p.cs_type),
              m_cplx_type(p.cplx_type),
              m_nl_type(p.nl_type)
        {
            // Initialize derived parameters
            m_num_cpus = 1; // For simplicity, assume single core (can be parameterized later)
            m_log2_block_size = floorLog2(blkSize);
            m_prediction_threshold_l1 = ((m_num_cpus == 1) ? 2 : 3);
            m_num_sets_in_ip_table_l1 = (1 << m_log_num_sets_in_ip_table_l1);
            m_num_sets_in_ip_delta_table_l1 = (1 << m_log_num_sets_in_ip_delta_table_l1);
            m_page_offset_mask = ((1 << m_page_shift) - 1);
            m_throttle_level_max_l1 = (2 + m_base_prefetch_degree_l1);
            m_num_sets_in_recent_access_tag_array_l1 = (1 << m_log_num_sets_in_recent_access_tag_array_l1);

            // Initialize state variables
            throttle_level_L1 = 0;
            prev_cpu_cycle = 0; // used to trigger throttling
            num_misses = 0;
            mpkc = 0;
            spec_nl = 0;

            // Allocate memory for data structures
            trackers_l1 = new IP_TABLE_L1[m_num_ip_table_l1_entries]();
            ghb_l1 = new uint64_t[m_num_ghb_entries]();

            // Allocate 2D arrays
            ipTableL1 = new IPtableL1 *[m_num_sets_in_ip_table_l1];
            for (uint64_t i = 0; i < m_num_sets_in_ip_table_l1; i++)
            {
                ipTableL1[i] = new IPtableL1[m_num_ways_in_ip_table_l1]();
            }

            ipDeltaTableL1 = new IPDeltaTableL1 *[m_num_sets_in_ip_delta_table_l1];
            for (uint64_t i = 0; i < m_num_sets_in_ip_delta_table_l1; i++)
            {
                ipDeltaTableL1[i] = new IPDeltaTableL1[m_num_ways_in_ip_delta_table_l1]();
            }

            recentAccessTagArrayL1 = new RecentAccessTagArrayL1 *[m_num_sets_in_recent_access_tag_array_l1];
            for (uint64_t i = 0; i < m_num_sets_in_recent_access_tag_array_l1; i++)
            {
                recentAccessTagArrayL1[i] = new RecentAccessTagArrayL1[m_num_ways_in_recent_access_tag_array_l1]();
            }

            nlBufferL1 = new NLBufferL1[m_num_entries_in_nl_buffer_l1]();
            longHistIPTableL1 = new LongHistIPtableL1[m_num_entries_in_long_hist_ip_table]();
            longHistory = new char[m_num_entries_in_long_hist_ip_table + 1]();

            // Initialize arrays
            for (uint64_t i = 0; i < BASE_PREFETCH_DEGREE_L1; i++)
            {
                degreeInsertionsL1[i] = 0;
                degreeHitsL1[i] = 0;
            }

            // Initialize stride arrays
            for (int i = 0; i < BASE_PREFETCH_DEGREE_L1; i++)
            {
                ipTableStride[i] = 0;
                ipPrefetchStride[i] = 0;
            }

            // Initialize DPT_l1 array
            for (int i = 0; i < 4096; i++)
            {
                DPT_l1[i].conf = 0;
                DPT_l1[i].delta = 0;
            }
        }

        IPCP::~IPCP()
        {
            // Clean up allocated memory
            delete[] trackers_l1;
            delete[] ghb_l1;
            delete[] nlBufferL1;
            delete[] longHistIPTableL1;
            delete[] longHistory;

            // Clean up 2D arrays
            for (uint64_t i = 0; i < m_num_sets_in_ip_table_l1; i++)
            {
                delete[] ipTableL1[i];
            }
            delete[] ipTableL1;

            for (uint64_t i = 0; i < m_num_sets_in_ip_delta_table_l1; i++)
            {
                delete[] ipDeltaTableL1[i];
            }
            delete[] ipDeltaTableL1;

            for (uint64_t i = 0; i < m_num_sets_in_recent_access_tag_array_l1; i++)
            {
                delete[] recentAccessTagArrayL1[i];
            }
            delete[] recentAccessTagArrayL1;
        }

        void
        IPCP::calculatePrefetch(const PrefetchInfo &pfi,
                                std::vector<AddrPriority> &addresses,
                                const CacheAccessor &cache)
        {
            Addr pf_addr = pfi.getAddr();
            Addr pc = pfi.getPC();
            bool is_miss = pfi.isCacheMiss();

            // Call main prefetcher logic
            l1dPrefetcherOperate(pf_addr, pc, is_miss ? 0 : 1, addresses);
        }

        void
        IPCP::l1dPrefetcherOperate(uint64_t addr, uint64_t ip, uint8_t cache_hit,
                                   std::vector<AddrPriority> &addresses)
        {
            int i = 0;
            uint64_t curr_page = addr >> m_page_shift;
            uint64_t cl_addr = addr >> m_log2_block_size;
            uint64_t cl_offset = (addr >> m_log2_block_size) & 0x3F;
            uint16_t signature = 0, last_signature = 0;
            int prefetch_degree = 0;
            int spec_nl_threshold = 0;
            int num_prefs = 0;
            uint64_t __attribute__((unused)) metadata = 0;
            uint16_t ip_tag = (ip >> m_num_ip_index_bits) & ((1 << m_num_ip_tag_bits) - 1);

            uint64_t pageid = addr >> m_page_shift;
            unsigned char offset = (addr & m_page_offset_mask) >> m_log2_block_size;
            bool did_pref = false;
            bool current_delta_nonzero = false;

            // Set prefetch degree based on system configuration
            if (m_num_cpus == 1)
            {
                prefetch_degree = 3;
                spec_nl_threshold = 15;
            }
            else
            {
                prefetch_degree = 2;
                spec_nl_threshold = 5;
            }

            // Update miss counter
            if (cache_hit == 0)
                num_misses += 1;

            // Update spec nl bit when num misses crosses certain threshold
            if (num_misses == 256)
            {
                // For simplicity, we'll use a basic threshold based on miss rate
                if (num_misses > spec_nl_threshold)
                    spec_nl = 0;
                else
                    spec_nl = 1;
                num_misses = 0;
            }

            // Long history IP table lookup
            char longHistIPTableNewDelta = 0;
            for (i = 0; i < (int)m_num_entries_in_long_hist_ip_table; i++)
            {
                if (longHistIPTableL1[i].valid &&
                    (longHistIPTableL1[i].ip == (ip & m_long_hist_ip_table_tag_mask)))
                {
                    for (int j = 0; j < NUM_STRIDES_IN_LONG_HIST_IP_TABLE; j++)
                    {
                        longHistory[j] = longHistIPTableL1[i].stride[j];
                    }

                    if (pageid == (longHistIPTableL1[i].block_addr >> (m_page_shift - m_log2_block_size)))
                    {
                        longHistory[NUM_STRIDES_IN_LONG_HIST_IP_TABLE] =
                            offset - (((longHistIPTableL1[i].block_addr << m_log2_block_size) & m_page_offset_mask) >> m_log2_block_size);
                    }
                    else
                    {
                        longHistory[NUM_STRIDES_IN_LONG_HIST_IP_TABLE] = 0;
                    }

                    longHistIPTableNewDelta = longHistory[NUM_STRIDES_IN_LONG_HIST_IP_TABLE];
                    longHistIPTableL1[i].block_addr = cl_addr;

                    if (longHistIPTableNewDelta != 0)
                    {
                        for (int j = 0; j < NUM_STRIDES_IN_LONG_HIST_IP_TABLE - 1; j++)
                        {
                            longHistIPTableL1[i].stride[j] = longHistIPTableL1[i].stride[j + 1];
                        }
                        longHistIPTableL1[i].stride[NUM_STRIDES_IN_LONG_HIST_IP_TABLE - 1] = longHistIPTableNewDelta;
                    }
                    break;
                }
            }

            if (i == (int)m_num_entries_in_long_hist_ip_table)
            {
                // Find empty entry or replace LRU
                for (i = 0; i < (int)m_num_entries_in_long_hist_ip_table; i++)
                {
                    if (!longHistIPTableL1[i].valid)
                        break;
                }
                if (i == (int)m_num_entries_in_long_hist_ip_table)
                {
                    uint64_t maxlru = 0;
                    int rep_index = 0;
                    for (i = 0; i < (int)m_num_entries_in_long_hist_ip_table; i++)
                    {
                        if (longHistIPTableL1[i].lru >= maxlru)
                        {
                            maxlru = longHistIPTableL1[i].lru;
                            rep_index = i;
                        }
                    }
                    i = rep_index;
                }
                assert(i < m_num_entries_in_long_hist_ip_table);
                longHistIPTableL1[i].ip = (ip & m_long_hist_ip_table_tag_mask);
                longHistIPTableL1[i].block_addr = cl_addr;
                for (int j = 0; j < NUM_STRIDES_IN_LONG_HIST_IP_TABLE; j++)
                {
                    longHistIPTableL1[i].stride[j] = 0;
                }
                longHistIPTableL1[i].valid = true;
            }

            // Update LRU for long history table
            for (int j = 0; j < (int)m_num_entries_in_long_hist_ip_table; j++)
            {
                longHistIPTableL1[j].lru++;
            }
            longHistIPTableL1[i].lru = 0;

            // NL Buffer Lookup
            for (i = 0; i < (int)m_num_entries_in_nl_buffer_l1; i++)
            {
                if (nlBufferL1[i].valid && (nlBufferL1[i].tag == cl_addr))
                {
                    degreeHitsL1[nlBufferL1[i].degree - 1]++;
                    nlBufferL1[i].valid = false;
                    break;
                }
            }

            // IP Table Lookup
            bool constantStrideValid = false;
            char constantStride = 0;
            int ipTableIndex = (pageid) & (m_num_sets_in_ip_table_l1 - 1);
            uint64_t ipTableTag = ((pageid) >> m_log_num_sets_in_ip_table_l1) & m_ip_table_tag_mask;
            int ii;

            for (ii = 0; ii < (int)m_num_ways_in_ip_table_l1; ii++)
            {
                if (ipTableL1[ipTableIndex][ii].valid && (ipTableL1[ipTableIndex][ii].tag == ipTableTag))
                {
                    if ((signed)(offset - ipTableL1[ipTableIndex][ii].offset) != 0)
                    {
                        current_delta_nonzero = true;
                        for (i = 0; i < BASE_PREFETCH_DEGREE_L1; i++)
                        {
                            if (ipTableL1[ipTableIndex][ii].stride[i] == 0)
                            {
                                ipTableL1[ipTableIndex][ii].stride[i] = (signed)(offset - ipTableL1[ipTableIndex][ii].offset);
                                break;
                            }
                        }
                        if (i == BASE_PREFETCH_DEGREE_L1)
                        {
                            for (i = 0; i < BASE_PREFETCH_DEGREE_L1; i++)
                            {
                                ipTableL1[ipTableIndex][ii].stride[i] = ipTableL1[ipTableIndex][ii].stride[i + 1];
                            }
                            ipTableL1[ipTableIndex][ii].stride[i] = (signed)(offset - ipTableL1[ipTableIndex][ii].offset);
                        }

                        if (i == 0)
                        {
                            ipTableL1[ipTableIndex][ii].conf = 0;
                            ipTableL1[ipTableIndex][ii].confPointer = m_pointer_last;
                        }
                        else if (ipTableL1[ipTableIndex][ii].stride[i] == ipTableL1[ipTableIndex][ii].stride[i - 1])
                        {
                            if (ipTableL1[ipTableIndex][ii].confPointer == m_pointer_last)
                            {
                                if (ipTableL1[ipTableIndex][ii].conf < m_stride_conf_max)
                                {
                                    ipTableL1[ipTableIndex][ii].conf++;
                                }
                            }
                            else
                            {
                                ipTableL1[ipTableIndex][ii].conf = 1;
                                ipTableL1[ipTableIndex][ii].confPointer = m_pointer_last;
                            }
                        }
                        else
                        {
                            if (ipTableL1[ipTableIndex][ii].confPointer == m_pointer_last)
                            {
                                ipTableL1[ipTableIndex][ii].confPointer = m_pointer_non_last;
                                if (ipTableL1[ipTableIndex][ii].conf > 0)
                                {
                                    ipTableL1[ipTableIndex][ii].conf--;
                                }
                            }
                            else
                            {
                                assert(i > 1);
                                ipTableL1[ipTableIndex][ii].confPointer = m_pointer_last;
                                if (ipTableL1[ipTableIndex][ii].stride[i] == ipTableL1[ipTableIndex][ii].stride[i - 2])
                                {
                                    if (ipTableL1[ipTableIndex][ii].conf < m_stride_conf_max)
                                    {
                                        ipTableL1[ipTableIndex][ii].conf++;
                                    }
                                }
                                else
                                {
                                    ipTableL1[ipTableIndex][ii].conf = 0;
                                }
                            }
                        }

                        if ((ipTableL1[ipTableIndex][ii].conf >= m_stride_conf_threshold) &&
                            (ipTableL1[ipTableIndex][ii].stride[i] == ipTableL1[ipTableIndex][ii].stride[i - 1]))
                        {
                            constantStride = ipTableL1[ipTableIndex][ii].stride[i];
                            constantStrideValid = true;
                        }

                        ipTableL1[ipTableIndex][ii].offset = offset;
                    }
                    break;
                }
            }

            if (ii == (int)m_num_ways_in_ip_table_l1)
            {
                for (ii = 0; ii < (int)m_num_ways_in_ip_table_l1; ii++)
                {
                    if (!ipTableL1[ipTableIndex][ii].valid)
                        break;
                }
                if (ii == (int)m_num_ways_in_ip_table_l1)
                {
                    uint64_t maxlru = 0;
                    int repl_index = -1;
                    for (ii = 0; ii < (int)m_num_ways_in_ip_table_l1; ii++)
                    {
                        if (ipTableL1[ipTableIndex][ii].lru > maxlru)
                        {
                            maxlru = ipTableL1[ipTableIndex][ii].lru;
                            repl_index = ii;
                        }
                    }
                    ii = repl_index;
                }
                ipTableL1[ipTableIndex][ii].tag = ipTableTag;
                ipTableL1[ipTableIndex][ii].offset = offset;
                for (i = 0; i < BASE_PREFETCH_DEGREE_L1; i++)
                {
                    ipTableL1[ipTableIndex][ii].stride[i] = 0;
                }
                ipTableL1[ipTableIndex][ii].valid = true;
            }

            for (i = 0; i < (int)m_num_ways_in_ip_table_l1; i++)
            {
                ipTableL1[ipTableIndex][i].lru++;
            }
            ipTableL1[ipTableIndex][ii].lru = 0;

            int lastNonZeroIndex = -1;
            for (i = 0; i < BASE_PREFETCH_DEGREE_L1; i++)
            {
                ipTableStride[i] = ipTableL1[ipTableIndex][ii].stride[i];
                if (ipTableStride[i] != 0)
                {
                    lastNonZeroIndex = i;
                }
            }

            // // IP delta table lookup and training
            // if (current_delta_nonzero) {
            //     for (i = 0; i < lastNonZeroIndex; i++) {
            //         assert(ipTableStride[i] != 0);
            //         unsigned delta;
            //         delta = (ipTableStride[i] >= 0) ? ipTableStride[i] : ((-ipTableStride[i]) | (1 << (m_page_shift - m_log2_block_size)));
            //         int ipDeltaTableIndex = (((pageid) << 3) ^ delta) & (m_num_sets_in_ip_delta_table_l1 - 1);
            //         uint64_t ipDeltaTableTag = ((((pageid) << 3) ^ delta) >> m_log_num_sets_in_ip_delta_table_l1) & m_ip_delta_table_tag_mask;

            //         for (ii = 0; ii < (int)m_num_ways_in_ip_delta_table_l1; ii++) {
            //             if (ipDeltaTableL1[ipDeltaTableIndex][ii].valid &&
            //                 (ipDeltaTableL1[ipDeltaTableIndex][ii].tag == ipDeltaTableTag)) {
            //                 break;
            //             }
            //         }

            //         if (ii < (int)m_num_ways_in_ip_delta_table_l1) {
            //             for (int j = 0; j < (int)m_num_ways_in_ip_delta_table_l1; j++) {
            //                 ipDeltaTableL1[ipDeltaTableIndex][j].lru++;
            //             }
            //             ipDeltaTableL1[ipDeltaTableIndex][ii].lru = 0;
            //             ipDeltaTableL1[ipDeltaTableIndex][ii].partial_ip = ip & m_partial_ip_mask;
            //             ipDeltaTableL1[ipDeltaTableIndex][ii].partial_ip_valid = true;
            //         } else {
            //             for (ii = 0; ii < (int)m_num_ways_in_ip_delta_table_l1; ii++) {
            //                 if (!ipDeltaTableL1[ipDeltaTableIndex][ii].valid) break;
            //             }
            //             if (ii == (int)m_num_ways_in_ip_delta_table_l1) {
            //                 uint64_t maxlru = 0;
            //                 int repl_index = -1;
            //                 for (ii = 0; ii < (int)m_num_ways_in_ip_delta_table_l1; ii++) {
            //                     if (ipDeltaTableL1[ipDeltaTableIndex][ii].lru > maxlru) {
            //                         maxlru = ipDeltaTableL1[ipDeltaTableIndex][ii].lru;
            //                         repl_index = ii;
            //                     }
            //                 }
            //                 ii = repl_index;
            //             }
            //             ipDeltaTableL1[ipDeltaTableIndex][ii].tag = ipDeltaTableTag;
            //             ipDeltaTableL1[ipDeltaTableIndex][ii].partial_ip = ip & m_partial_ip_mask;
            //             ipDeltaTableL1[ipDeltaTableIndex][ii].partial_ip_valid = true;
            //             ipDeltaTableL1[ipDeltaTableIndex][ii].valid = true;
            //             for (int j = 0; j < (int)m_num_ways_in_ip_delta_table_l1; j++) {
            //                 ipDeltaTableL1[ipDeltaTableIndex][j].lru++;
            //             }
            //             ipDeltaTableL1[ipDeltaTableIndex][ii].lru = 0;
            //         }
            //     }
            // }
            if (current_delta_nonzero)
            {
                for (i = 0; i < lastNonZeroIndex; i++)
                {
                    assert(ipTableStride[i] != 0);
                    unsigned delta;
                    delta = (ipTableStride[i] >= 0) ? ipTableStride[i] : ((-ipTableStride[i]) | (1 << (m_page_shift - m_log2_block_size)));
                    int ipDeltaTableIndex = (((pageid) << 3) ^ delta) & (m_num_sets_in_ip_delta_table_l1 - 1);
                    uint64_t ipDeltaTableTag = ((((pageid) << 3) ^ delta) >> m_log_num_sets_in_ip_delta_table_l1) & m_ip_delta_table_tag_mask;
                    for (ii = 0; ii < m_num_ways_in_ip_delta_table_l1; ii++)
                    {
                        if (ipDeltaTableL1[ipDeltaTableIndex][ii].valid && (ipDeltaTableL1[ipDeltaTableIndex][ii].tag == ipDeltaTableTag))
                        {
                            if (ipTableStride[lastNonZeroIndex] == ipDeltaTableL1[ipDeltaTableIndex][ii].stride[lastNonZeroIndex - i - 1])
                            {
                                if (ipDeltaTableL1[ipDeltaTableIndex][ii].counters[lastNonZeroIndex - i - 1] < m_saturating_counter_max_l1)
                                {
                                    ipDeltaTableL1[ipDeltaTableIndex][ii].counters[lastNonZeroIndex - i - 1]++;
                                }
                            }
                            else
                            {
                                ipDeltaTableL1[ipDeltaTableIndex][ii].stride[lastNonZeroIndex - i - 1] = ipTableStride[lastNonZeroIndex];
                                ipDeltaTableL1[ipDeltaTableIndex][ii].counters[lastNonZeroIndex - i - 1] = 1;
                            }
                            break;
                        }
                    }
                    if (ii == m_num_ways_in_ip_delta_table_l1)
                    {
                        for (ii = 0; ii < m_num_ways_in_ip_delta_table_l1; ii++)
                        {
                            if (!ipDeltaTableL1[ipDeltaTableIndex][ii].valid)
                                break;
                        }
                        if (ii == m_num_ways_in_ip_delta_table_l1)
                        {
                            uint64_t maxlru = 0;
                            int repl_index = -1;
                            for (ii = 0; ii < m_num_ways_in_ip_delta_table_l1; ii++)
                            {
                                if (ipDeltaTableL1[ipDeltaTableIndex][ii].lru > maxlru)
                                {
                                    maxlru = ipDeltaTableL1[ipDeltaTableIndex][ii].lru;
                                    repl_index = ii;
                                }
                            }
                            ii = repl_index;
                        }
                        ipDeltaTableL1[ipDeltaTableIndex][ii].partial_ip_valid = false;
                        ipDeltaTableL1[ipDeltaTableIndex][ii].tag = ipDeltaTableTag;
                        for (int j = 0; j < BASE_PREFETCH_DEGREE_L1; j++)
                        {
                            if (i + j + 1 < BASE_PREFETCH_DEGREE_L1)
                            {
                                ipDeltaTableL1[ipDeltaTableIndex][ii].stride[j] = ipTableStride[i + j + 1];
                                if (ipTableStride[i + j + 1] != 0)
                                    ipDeltaTableL1[ipDeltaTableIndex][ii].counters[j] = 1;
                                else
                                    ipDeltaTableL1[ipDeltaTableIndex][ii].counters[j] = 0;
                            }
                            else
                            {
                                ipDeltaTableL1[ipDeltaTableIndex][ii].stride[j] = 0;
                                ipDeltaTableL1[ipDeltaTableIndex][ii].counters[j] = 0;
                            }
                        }
                        ipDeltaTableL1[ipDeltaTableIndex][ii].valid = true;
                    }
                    for (int j = 0; j < m_num_ways_in_ip_delta_table_l1; j++)
                        ipDeltaTableL1[ipDeltaTableIndex][j].lru++;
                    ipDeltaTableL1[ipDeltaTableIndex][ii].lru = 0;
                }
            }
            // Update recent access tag array
            recentAccessTagArrayL1LookupAndInsertIfMiss(cl_addr);

            // Main prefetching decision
            int index = ip & ((1 << m_num_ip_index_bits) - 1);

            if (trackers_l1[index].ip_tag != ip_tag)
            {
                // New/conflict IP
                if (trackers_l1[index].ip_valid == 0)
                {
                    trackers_l1[index].ip_tag = ip_tag;
                    trackers_l1[index].last_page = curr_page;
                    trackers_l1[index].last_cl_offset = cl_offset;
                    trackers_l1[index].last_stride = 0;
                    trackers_l1[index].signature = 0;
                    trackers_l1[index].conf = 0;
                    trackers_l1[index].str_valid = 0;
                    trackers_l1[index].str_strength = 0;
                    trackers_l1[index].str_dir = 0;
                    trackers_l1[index].ip_valid = 1;
                }
                else
                {
                    // Conflict, reset
                    trackers_l1[index].ip_valid = 0;
                }

                // Issue a next line prefetch upon encountering new IP
                uint64_t pf_address = ((addr >> m_log2_block_size) + 1) << m_log2_block_size;
                addresses.push_back(AddrPriority(pf_address, 0));
                return;
            }
            else
            {
                // Same IP encountered, set valid bit
                trackers_l1[index].ip_valid = 1;
            }

            // Calculate stride
            int64_t stride = 0;
            if (cl_offset > trackers_l1[index].last_cl_offset)
            {
                stride = cl_offset - trackers_l1[index].last_cl_offset;
            }
            else
            {
                stride = trackers_l1[index].last_cl_offset - cl_offset;
                stride *= -1;
            }

            // Don't do anything if same address is seen twice in a row
            if (stride == 0)
            {
                return;
            }

            // Page boundary learning
            if (curr_page != trackers_l1[index].last_page)
            {
                if (stride < 0)
                    stride += 64;
                else
                    stride -= 64;
            }

            // Update confidence
            trackers_l1[index].conf = updateConf(stride, trackers_l1[index].last_stride,
                                                 trackers_l1[index].conf);

            // Update CS only if confidence is zero
            if (trackers_l1[index].conf == 0)
            {
                trackers_l1[index].last_stride = stride;
            }

            last_signature = trackers_l1[index].signature;

            // Update complex stride confidence
            DPT_l1[last_signature].conf = updateConf(stride, DPT_l1[last_signature].delta,
                                                     DPT_l1[last_signature].conf);

            // Update CPLX only if confidence is zero
            if (DPT_l1[last_signature].conf == 0)
            {
                DPT_l1[last_signature].delta = stride;
            }

            // Calculate and update new signature
            signature = updateSignatureL1(last_signature, stride);
            trackers_l1[index].signature = signature;

            // Check for stream
            checkForStreamL1(index, cl_addr);

            // Generate prefetches based on confidence and stream detection
            if (trackers_l1[index].str_valid == 1)
            {
                // Stream IP - prefetch with higher degree
                prefetch_degree = prefetch_degree * 2;
                for (int i = 0; i < prefetch_degree; i++)
                {
                    uint64_t pf_addr;
                    if (trackers_l1[index].str_dir == 1)
                    {
                        pf_addr = ((cl_addr + i + 1) << m_log2_block_size);
                    }
                    else
                    {
                        pf_addr = ((cl_addr - i - 1) << m_log2_block_size);
                    }

                    // Check page boundary
                    if ((pf_addr >> m_page_shift) != (addr >> m_page_shift))
                    {
                        break;
                    }
                    addresses.push_back(AddrPriority(pf_addr, 0));
                    num_prefs++;
                }
            }
            else if (trackers_l1[index].conf > 1 && trackers_l1[index].last_stride != 0)
            {
                // Constant stride prediction
                for (int i = 0; i < prefetch_degree; i++)
                {
                    uint64_t pf_addr = ((cl_addr + (trackers_l1[index].last_stride * (i + 1))) << m_log2_block_size);

                    // Check page boundary
                    if ((pf_addr >> m_page_shift) != (addr >> m_page_shift))
                    {
                        break;
                    }
                    addresses.push_back(AddrPriority(pf_addr, 0));
                    num_prefs++;
                }
            }
            else if (DPT_l1[signature].conf >= 0 && DPT_l1[signature].delta != 0)
            {
                // Complex stride prediction - if conf>=0, continue looking for delta
                int pref_offset = 0;
                uint16_t temp_signature = signature;
                for (int i = 0; i < prefetch_degree; i++)
                {
                    pref_offset += DPT_l1[temp_signature].delta;
                    uint64_t pf_addr = ((cl_addr + pref_offset) << m_log2_block_size);

                    // Check page boundary and other conditions
                    if ((pf_addr >> m_page_shift) != (addr >> m_page_shift) ||
                        (DPT_l1[temp_signature].conf == -1) ||
                        (DPT_l1[temp_signature].delta == 0))
                    {
                        // if new entry in DPT or delta is zero, break
                        break;
                    }

                    if (DPT_l1[temp_signature].conf > 0)
                    { // prefetch only when conf>0 for CPLX
                        addresses.push_back(AddrPriority(pf_addr, 0));
                        num_prefs++;
                    }
                    signature = updateSignatureL1(temp_signature, DPT_l1[temp_signature].delta);
                }
            }
            // Update IP table entries
            trackers_l1[index].last_cl_offset = cl_offset;
            trackers_l1[index].last_page = curr_page;

            // Update GHB
            int ghb_index = 0;
            for (ghb_index = 0; ghb_index < (int)m_num_ghb_entries; ghb_index++)
            {
                if (cl_addr == ghb_l1[ghb_index])
                {
                    break;
                }
            }

            // Only update the GHB upon finding a new cl address
            if (ghb_index == (int)m_num_ghb_entries)
            {
                for (ghb_index = (int)m_num_ghb_entries - 1; ghb_index > 0; ghb_index--)
                {
                    ghb_l1[ghb_index] = ghb_l1[ghb_index - 1];
                }
                ghb_l1[0] = cl_addr;
            }

            // Advanced IP table-based prefetching
            if ((lastNonZeroIndex == -1) || !current_delta_nonzero)
            {

                // Long history IP prefetcher fallback
                if (longHistIPTableNewDelta)
                {
                    int j, length, chosen_j = -1;
                    // Determine stride pattern match
                    for (j = 0; j < NUM_STRIDES_IN_LONG_HIST_IP_TABLE + 1 - (int)m_long_hist_match_length; j++)
                    {
                        length = 0;
                        while (length != (int)m_long_hist_match_length)
                        {
                            if (longHistory[NUM_STRIDES_IN_LONG_HIST_IP_TABLE + 1 - (int)m_long_hist_match_length + length] !=
                                longHistory[j + length])
                                break;
                            length++;
                        }
                        if (length == (int)m_long_hist_match_length)
                        {
                            chosen_j = j;
                            break;
                        }
                    }

                    if (chosen_j != -1)
                    {
                        j = chosen_j + (int)m_long_hist_match_length;
                        if (throttle_level_L1 == 0)
                        {
                            while (j < NUM_STRIDES_IN_LONG_HIST_IP_TABLE + 1)
                            {
                                uint64_t pf_address = (cl_addr + longHistory[j]) << m_log2_block_size;

                                if (!recentAccessTagArrayL1DetermineHit(pf_address >> m_log2_block_size))
                                {
                                    addresses.push_back(AddrPriority(pf_address, 0));
                                    recentAccessTagArrayL1Insert(pf_address >> m_log2_block_size);
                                    did_pref = true;
                                }
                                j++;
                            }
                            if (did_pref)
                                return;
                        }
                    }
                }

                // Next line prefetcher fallback
                uint64_t pf_address = (cl_addr + 1) << m_log2_block_size;
                if (throttle_level_L1 == 0)
                {
                    // Insert possible next line prefetch candidates in the NL buffer
                    i = 0;
                    while (i < BASE_PREFETCH_DEGREE_L1)
                    {
                        if (degreeHitsL1[i] * m_nl_threshold_denom_l1 > degreeInsertionsL1[i] * m_nl_threshold_numer_l1)
                        {
                            if (!recentAccessTagArrayL1DetermineHit(pf_address >> m_log2_block_size))
                            {
                                addresses.push_back(AddrPriority(pf_address, 0));
                                recentAccessTagArrayL1Insert(pf_address >> m_log2_block_size);
                            }
                        }
                        pf_address = pf_address + (1 << m_log2_block_size);
                        i++;
                    }
                }
                i = 0;
                pf_address = (cl_addr + 1) << m_log2_block_size;
                while (i < BASE_PREFETCH_DEGREE_L1)
                {
                    if ((pf_address >> m_page_shift) != (addr >> m_page_shift))
                        break;
                    nlBufferL1Insert(pf_address >> m_log2_block_size, i);
                    pf_address = pf_address + (1 << m_log2_block_size);
                    i++;
                }
                return;
            }

            // IP delta table lookup to decide prefetch candidates
            ipPrefetchStride[0] = ipTableStride[lastNonZeroIndex];
            for (i = 1; i < BASE_PREFETCH_DEGREE_L1; i++)
            {
                ipPrefetchStride[i] = 0;
            }

            unsigned delta = (ipTableStride[lastNonZeroIndex] >= 0) ? ipTableStride[lastNonZeroIndex] : ((-ipTableStride[lastNonZeroIndex]) | (1 << (m_page_shift - m_log2_block_size)));
            int ipDeltaTableIndex = (((pageid) << 3) ^ delta) & (m_num_sets_in_ip_delta_table_l1 - 1);
            uint64_t ipDeltaTableTag = ((((pageid) << 3) ^ delta) >> m_log_num_sets_in_ip_delta_table_l1) & m_ip_delta_table_tag_mask;

            for (ii = 0; ii < (int)m_num_ways_in_ip_delta_table_l1; ii++)
            {
                if (ipDeltaTableL1[ipDeltaTableIndex][ii].valid &&
                    (ipDeltaTableL1[ipDeltaTableIndex][ii].tag == ipDeltaTableTag))
                {
                    for (int j = 0; j < BASE_PREFETCH_DEGREE_L1; j++)
                    {
                        if (m_num_cpus == 1)
                        {
                            if (((i < BASE_PREFETCH_DEGREE_L1) && (ipDeltaTableL1[ipDeltaTableIndex][ii].counters[i - 1] >= m_prediction_threshold_l1)) ||
                                ((i == BASE_PREFETCH_DEGREE_L1) && (ipDeltaTableL1[ipDeltaTableIndex][ii].counters[i - 1] >= (m_prediction_threshold_l1 + 1))))
                            {
                                ipPrefetchStride[i] = ipDeltaTableL1[ipDeltaTableIndex][ii].stride[i - 1];
                            }
                        }
                        else
                        {
                            if (ipDeltaTableL1[ipDeltaTableIndex][ii].counters[i - 1] >= m_prediction_threshold_l1)
                            {
                                ipPrefetchStride[i] = ipDeltaTableL1[ipDeltaTableIndex][ii].stride[i - 1];
                            }
                        }
                    }
                    break;
                }
            }

            // Generate prefetches from IP table
            // if (ii < (int)m_num_ways_in_ip_delta_table_l1) {
            //     uint64_t pf_address = cl_addr << m_log2_block_size;
            //     bool stopPrefetching = false;
            //     int num_pref = 0;

            //     if (throttle_level_L1 < m_throttle_level_max_l1) {
            //         for (i = 1; i < BASE_PREFETCH_DEGREE_L1 + 1; i++) {
            //             if (ipPrefetchStride[i] == 0) break;
            //             pf_address = pf_address + (ipPrefetchStride[i] << m_log2_block_size);

            //             if ((pf_address >> m_page_shift) != (addr >> m_page_shift)) continue;

            //             if (!recentAccessTagArrayL1DetermineHit(pf_address >> m_log2_block_size)) {
            //                 addresses.push_back(AddrPriority(pf_address, 0));
            //                 did_pref = true;
            //                 num_pref++;
            //                 recentAccessTagArrayL1Insert(pf_address >> m_log2_block_size);
            //             }
            //         }
            //     }
            // }
            if (ii < m_num_ways_in_ip_delta_table_l1)
            {
                for (int j = 0; j < m_num_ways_in_ip_delta_table_l1; j++)
                    ipDeltaTableL1[ipDeltaTableIndex][j].lru++;

                ipDeltaTableL1[ipDeltaTableIndex][ii].lru = 0;
                ipDeltaTableL1[ipDeltaTableIndex][ii].partial_ip = ip & m_partial_ip_mask;
                ipDeltaTableL1[ipDeltaTableIndex][ii].partial_ip_valid = true;
            }
            else
            {
                for (ii = 0; ii < m_num_ways_in_ip_delta_table_l1; ii++)
                {
                    if (ipDeltaTableL1[ipDeltaTableIndex][ii].valid && ipDeltaTableL1[ipDeltaTableIndex][ii].partial_ip_valid && (ipDeltaTableL1[ipDeltaTableIndex][ii].partial_ip == (ip & m_partial_ip_mask)))
                    {
                        for (i = 1; i < BASE_PREFETCH_DEGREE_L1; i++)
                        {
                            if (m_num_cpus == 1)
                            {
                                if (((i < BASE_PREFETCH_DEGREE_L1) && (ipDeltaTableL1[ipDeltaTableIndex][ii].counters[i - 1] >= m_prediction_threshold_l1)) ||
                                    ((i == BASE_PREFETCH_DEGREE_L1) && (ipDeltaTableL1[ipDeltaTableIndex][ii].counters[i - 1] >= (m_prediction_threshold_l1 + 1))))
                                {
                                    ipPrefetchStride[i] = ipDeltaTableL1[ipDeltaTableIndex][ii].stride[i - 1];
                                }
                            }
                            else
                            {
                                if (ipDeltaTableL1[ipDeltaTableIndex][ii].counters[i - 1] >= m_prediction_threshold_l1)
                                {
                                    ipPrefetchStride[i] = ipDeltaTableL1[ipDeltaTableIndex][ii].stride[i - 1];
                                }
                            }
                        }
                        break;
                    }
                }
            }
            uint64_t pf_address = cl_addr << m_log2_block_size;
            bool stopPrefetching = false;
            int num_pref = 0;
            if (throttle_level_L1 < m_throttle_level_max_l1)
            {
                for (i = 1; i < ((throttle_level_L1 > 2) ? BASE_PREFETCH_DEGREE_L1 - (throttle_level_L1 - 2) + 1 : BASE_PREFETCH_DEGREE_L1 + 1); i++)
                {
                    if (ipPrefetchStride[i] == 0)
                        break;
                    pf_address = pf_address + (ipPrefetchStride[i] << m_log2_block_size);
                    if ((pf_address >> m_page_shift) != (addr >> m_page_shift))
                        continue;

                    if (!recentAccessTagArrayL1DetermineHit(pf_address >> m_log2_block_size))
                    {
                        if (PQ.occupancy < (PQ.SIZE - 1))
                        {
                            addresses.push_back(AddrPriority(pf_address, 0)); //               assert(prefetch_line(ip, addr, pf_address, FILL_L1, 0));
                            did_pref = true;
                        }
                        else if (PQ.occupancy < PQ.SIZE)
                        {
                            uint32_t pfmetadata = 0;
                            unsigned char residue = 0;
                            for (int j = i + 1; j < ((throttle_level_L1 > 2) ? BASE_PREFETCH_DEGREE_L1 - (throttle_level_L1 - 2) + 1 : BASE_PREFETCH_DEGREE_L1 + 1); j++)
                            {
                                if (ipPrefetchStride[j] == 0)
                                    break;
                                unsigned char delta = ((ipPrefetchStride[j] < 0) ? ((-ipPrefetchStride[j]) | (1 << (m_page_shift - m_log2_block_size))) : ipPrefetchStride[j]);
                                pfmetadata = pfmetadata | (delta << ((1 + m_page_shift - m_log2_block_size) * residue));
                                residue++;
                            }
                            addresses.push_back(AddrPriority(pf_address, 0));
                            did_pref = true;
                        }
                        else
                        {
                            addresses.push_back(AddrPriority(pf_address, 0));
                            stopPrefetching = true;
                            break;
                        }
                        num_pref++;
                        recentAccessTagArrayL1Insert(pf_address >> m_log2_block_size);
                    }
                }
            }

            if (throttle_level_L1 < 2)
            {
                if ((BASE_PREFETCH_DEGREE_L1 > 1) && !stopPrefetching && (i < BASE_PREFETCH_DEGREE_L1 + 1))
                {
                    if ((ipPrefetchStride[i - 1] != 0) && (i > 1) && (ipPrefetchStride[i - 1] == ipPrefetchStride[i - 2]))
                    {
                        assert(num_pref < BASE_PREFETCH_DEGREE_L1);

                        while (1)
                        {
                            pf_address = pf_address + (ipPrefetchStride[i - 1] << m_log2_block_size);
                            if ((pf_address >> m_page_shift) != (addr >> m_page_shift))
                                break;

                            if (!recentAccessTagArrayL1DetermineHit(pf_address >> m_log2_block_size))
                            {
                                if (PQ.occupancy < (PQ.SIZE - 1))
                                {
                                    addresses.push_back(AddrPriority(pf_address, 0));
                                    //                     assert(prefetch_line(ip, addr, pf_address, FILL_L1, 0));
                                    num_pref++;
                                    did_pref = true;
                                }
                                else if (PQ.occupancy < PQ.SIZE)
                                {
                                    uint32_t pfmetadata = 0x80000000U;
                                    unsigned char delta = ((ipPrefetchStride[i - 1] < 0) ? ((-ipPrefetchStride[i - 1]) | (1 << (m_page_shift - m_log2_block_size))) : ipPrefetchStride[i - 1]);
                                    pfmetadata = pfmetadata | delta;
                                    //                     assert(prefetch_line(ip, addr, pf_address, FILL_L1, pfmetadata));
                                    addresses.push_back(AddrPriority(pf_address, 0));
                                    num_pref++;
                                    did_pref = true;
                                }
                                else
                                {
                                    addresses.push_back(AddrPriority(pf_address, 0));
                                    //                     assert(!prefetch_line(ip, addr, pf_address, FILL_L1, 0));
                                    break;
                                }
                                recentAccessTagArrayL1Insert(pf_address >> m_log2_block_size);
                            }
                            if (num_pref == (BASE_PREFETCH_DEGREE_L1))
                                break;
                        }
                        if ((num_pref == (BASE_PREFETCH_DEGREE_L1)) && (PQ.occupancy < PQ.SIZE))
                        {
                            pf_address = pf_address + (ipPrefetchStride[i - 1] << m_log2_block_size);
                            if ((pf_address >> m_page_shift) == (addr >> m_page_shift))
                            {
                                uint32_t pfmetadata = 0x80000000U;
                                unsigned char delta = ((ipPrefetchStride[i - 1] < 0) ? ((-ipPrefetchStride[i - 1]) | (1 << (m_page_shift - m_log2_block_size))) : ipPrefetchStride[i - 1]);
                                pfmetadata = pfmetadata | delta;
                                addresses.push_back(AddrPriority(pf_address, 0));
                                //                  assert(prefetch_line(ip, addr, pf_address, FILL_L1, pfmetadata));
                                did_pref = true;
                            }
                        }
                    }
                    else if (constantStrideValid)
                    {
                        while (1)
                        {
                            pf_address = pf_address + (constantStride << m_log2_block_size);
                            if ((pf_address >> m_page_shift) != (addr >> m_page_shift))
                                break;
                            if (!recentAccessTagArrayL1DetermineHit(pf_address >> m_log2_block_size))
                            {
                                if (PQ.occupancy < (PQ.SIZE - 1))
                                {
                                    //                     assert(prefetch_line(ip, addr, pf_address, FILL_L1, 0));
                                    addresses.push_back(AddrPriority(pf_address, 0));
                                    num_pref++;
                                    did_pref = true;
                                }
                                else if (PQ.occupancy < PQ.SIZE)
                                {
                                    uint32_t pfmetadata = 0x80000000U;
                                    unsigned char delta = ((constantStride < 0) ? ((-constantStride) | (1 << (m_page_shift - m_log2_block_size))) : constantStride);
                                    pfmetadata = pfmetadata | delta;
                                    //                     assert(prefetch_line(ip, addr, pf_address, FILL_L1, pfmetadata));
                                    addresses.push_back(AddrPriority(pf_address, 0));

                                    num_pref++;
                                    did_pref = true;
                                }
                                else
                                {
                                    addresses.push_back(AddrPriority(pf_address, 0));
                                    //                     assert(!prefetch_line(ip, addr, pf_address, FILL_L1, 0));
                                    break;
                                }

                                recentAccessTagArrayL1Insert(pf_address >> m_log2_block_size);
                            }

                            if (num_pref == (BASE_PREFETCH_DEGREE_L1))
                                break;
                        }
                        if ((num_pref == (BASE_PREFETCH_DEGREE_L1)) && (PQ.occupancy < PQ.SIZE))
                        {
                            pf_address = pf_address + (constantStride << m_log2_block_size);
                            if ((pf_address >> m_page_shift) == (addr >> m_page_shift))
                            {
                                uint32_t pfmetadata = 0x80000000U;
                                unsigned char delta = ((constantStride < 0) ? ((-constantStride) | (1 << (m_page_shift - m_log2_block_size))) : constantStride);
                                pfmetadata = pfmetadata | delta;
                                addresses.push_back(AddrPriority(pf_address, 0));
                                //                  assert(prefetch_line(ip, addr, pf_address, FILL_L1, pfmetadata));
                                did_pref = true;
                            }
                        }
                    }
                }
                else if (!stopPrefetching)
                {
                    if ((ipPrefetchStride[i - 1] != 0) && (i > 1) && (ipPrefetchStride[i - 1] == ipPrefetchStride[i - 2]))
                    {
                        pf_address = pf_address + (ipPrefetchStride[i - 1] << m_log2_block_size);
                        if ((pf_address >> m_page_shift) == (addr >> m_page_shift))
                        {
                            uint32_t pfmetadata = 0x80000000U;
                            unsigned char delta = ((ipPrefetchStride[i - 1] < 0) ? ((-ipPrefetchStride[i - 1]) | (1 << (m_page_shift - m_log2_block_size))) : ipPrefetchStride[i - 1]);
                            pfmetadata = pfmetadata | delta;
                            addresses.push_back(AddrPriority(pf_address, 0));
                            //               prefetch_line(ip, addr, pf_address, FILL_L1, pfmetadata);
                            did_pref = true;
                        }
                    }
                    else if (constantStrideValid)
                    {
                        /////////////////////////////////////////////////////////////////////////////////////////////////////////
                        //    HAND  OVER  REMAINING  PREFETCH  INJECTION  TO  L2  CACHE  BY  ENCODING  THE  METADATA           //
                        ////////////////////////////////////////////////////////////////////////////////////////////////////////
                        pf_address = pf_address + (constantStride << m_log2_block_size);
                        if ((pf_address >> m_page_shift) == (addr >> m_page_shift))
                        {
                            uint32_t pfmetadata = 0x80000000U;
                            unsigned char delta = ((constantStride < 0) ? ((-constantStride) | (1 << (m_page_shift - m_log2_block_size))) : constantStride);
                            pfmetadata = pfmetadata | delta;
                            addresses.push_back(AddrPriority(pf_address, 0));
                            //               prefetch_line(ip, addr, pf_address, FILL_L1, pfmetadata);
                            did_pref = true;
                        }
                    }
                }
            }
            if (!did_pref && longHistIPTableNewDelta)
            {
                int j, length, chosen_j = -1;
                for (j = 0; j < NUM_STRIDES_IN_LONG_HIST_IP_TABLE + 1 - m_long_hist_match_length; j++)
                {
                    length = 0;
                    while (length != m_long_hist_match_length)
                    {
                        if (longHistory[NUM_STRIDES_IN_LONG_HIST_IP_TABLE + 1 - m_long_hist_match_length + length] != longHistory[j + length])
                            break;
                        length++;
                    }
                    if (length == m_long_hist_match_length)
                    {
                        assert(chosen_j == -1);
                        chosen_j = j;
                        break;
                    }
                }
                if (chosen_j != -1)
                {
                    j = chosen_j + m_long_hist_match_length;
                    if (throttle_level_L1 == 0)
                    {
                        while (j < NUM_STRIDES_IN_LONG_HIST_IP_TABLE + 1)
                        {
                            uint64_t pf_address = (cl_addr + longHistory[j]) << m_log2_block_size;
                            if (!recentAccessTagArrayL1DetermineHit(pf_address >> m_log2_block_size))
                            {
                                //                  if (prefetch_line(ip, addr, pf_address, FILL_L1, 0)) {
                                addresses.push_back(AddrPriority(pf_address, 0));
                                recentAccessTagArrayL1Insert(pf_address >> m_log2_block_size);
                                did_pref = true;
                                //                  }
                            }
                            j++;
                        }
                    }
                }
            }

            if (!did_pref)
            {
                uint64_t pf_address = (cl_addr + 1) << m_log2_block_size;
                if (throttle_level_L1 == 0)
                {
                    i = 0;
                    while (i < BASE_PREFETCH_DEGREE_L1)
                    {
                        if (degreeHitsL1[i] * m_nl_threshold_denom_l1 > degreeInsertionsL1[i] * m_nl_threshold_numer_l1)
                        {
                            if (!recentAccessTagArrayL1DetermineHit(pf_address >> m_log2_block_size))
                            {
                                //                if (prefetch_line(ip, addr, pf_address, FILL_L1, 0)) {
                                addresses.push_back(AddrPriority(pf_address, 0));
                                recentAccessTagArrayL1Insert(pf_address >> m_log2_block_size);
                                //                  }
                            }
                        }
                        pf_address = pf_address + (1 << m_log2_block_size);
                        i++;
                    }
                }
                i = 0;
                pf_address = (cl_addr + 1) << m_log2_block_size;
                while (i < BASE_PREFETCH_DEGREE_L1)
                {
                    if ((pf_address >> m_page_shift) != (addr >> m_page_shift))
                        break;
                    nlBufferL1Insert(pf_address >> m_log2_block_size, i);
                    pf_address = pf_address + (1 << m_log2_block_size);
                    i++;
                }
            }

            // //This is just to avoid metadata not used
            //    DPRINTF(RubyPrefetcher, "Metadata %#x\n", metadata);

            //    SIG_DP(cout << metadata << endl);
            return;
        }

        // Helper method implementations
        uint16_t
        IPCP::updateSignatureL1(uint16_t old_sig, int delta)
        {
            uint16_t new_sig = 0;
            int sig_delta = 0;

            // 7-bit sign magnitude form
            sig_delta = (delta < 0) ? (((-1) * delta) + (1 << 6)) : delta;
            new_sig = ((old_sig << 1) ^ sig_delta) & 0xFFF; // 12-bit signature

            return new_sig;
        }

        void
        IPCP::checkForStreamL1(int index, uint64_t cl_addr)
        {
            int pos_count = 0, neg_count = 0, count = 0;
            uint64_t check_addr = cl_addr;

            // Check for positive stream
            for (int i = 0; i < (int)m_num_ghb_entries; i++)
            {
                check_addr--;
                for (int j = 0; j < (int)m_num_ghb_entries; j++)
                {
                    if (check_addr == ghb_l1[j])
                    {
                        pos_count++;
                        break;
                    }
                }
            }

            check_addr = cl_addr;

            // Check for negative stream
            for (int i = 0; i < (int)m_num_ghb_entries; i++)
            {
                check_addr++;
                for (int j = 0; j < (int)m_num_ghb_entries; j++)
                {
                    if (check_addr == ghb_l1[j])
                    {
                        neg_count++;
                        break;
                    }
                }
            }

            if (pos_count > neg_count)
            {
                trackers_l1[index].str_dir = 1;
                count = pos_count;
            }
            else
            {
                trackers_l1[index].str_dir = 0;
                count = neg_count;
            }

            if (count > (int)m_num_ghb_entries / 2)
            {
                trackers_l1[index].str_valid = 1;
                if (count >= ((int)m_num_ghb_entries * 3) / 4)
                {
                    trackers_l1[index].str_strength = 1;
                }
            }
            else
            {
                if (trackers_l1[index].str_strength == 0)
                {
                    trackers_l1[index].str_valid = 0;
                }
            }
        }

        int
        IPCP::updateConf(int stride, int pred_stride, int conf)
        {
            if (stride == pred_stride)
            {
                conf++;
                if (conf > 3)
                    conf = 3;
            }
            else
            {
                conf--;
                if (conf < 0)
                    conf = 0;
            }
            return conf;
        }

        // Other helper methods can be implemented as needed
        void
        IPCP::nlBufferL1Insert(uint64_t cl_addr, int current_degree_index)
        {
            int j;
            for (j = 0; j < (int)m_num_entries_in_nl_buffer_l1; j++)
            {
                if (nlBufferL1[j].valid && (nlBufferL1[j].tag == cl_addr))
                {
                    if (nlBufferL1[j].degree > (current_degree_index + 1))
                    {
                        assert(degreeInsertionsL1[nlBufferL1[j].degree - 1] > 0);
                        degreeInsertionsL1[nlBufferL1[j].degree - 1]--;
                        nlBufferL1[j].degree = current_degree_index + 1; // Always favor smaller degree NL prefetcher
                        degreeInsertionsL1[current_degree_index]++;
                        for (int jj = 0; jj < (int)m_num_entries_in_nl_buffer_l1; jj++)
                        {
                            nlBufferL1[jj].lru++;
                        }
                        nlBufferL1[j].lru = 0;
                    }
                    break;
                }
            }
            if (j == (int)m_num_entries_in_nl_buffer_l1)
            { // MISS
                for (j = 0; j < (int)m_num_entries_in_nl_buffer_l1; j++)
                {
                    if (!nlBufferL1[j].valid)
                        break;
                }
                if (j == (int)m_num_entries_in_nl_buffer_l1)
                {
                    uint64_t maxlru = 0;
                    int repl_index = -1;
                    for (j = 0; j < (int)m_num_entries_in_nl_buffer_l1; j++)
                    {
                        if (nlBufferL1[j].lru >= maxlru)
                        {
                            maxlru = nlBufferL1[j].lru;
                            repl_index = j;
                        }
                    }
                    j = repl_index;
                }
                nlBufferL1[j].tag = cl_addr;
                nlBufferL1[j].degree = current_degree_index + 1;
                nlBufferL1[j].valid = true;
                degreeInsertionsL1[current_degree_index]++;
                for (int jj = 0; jj < (int)m_num_entries_in_nl_buffer_l1; jj++)
                {
                    nlBufferL1[jj].lru++;
                }
                nlBufferL1[j].lru = 0;
            }
        }

        void
        IPCP::recentAccessTagArrayL1LookupAndInsertIfMiss(uint64_t cl_addr)
        {
            int recentAccessTagArrayIndex = cl_addr & (m_num_sets_in_recent_access_tag_array_l1 - 1);
            uint64_t recentAccessTagArrayTag = cl_addr >> m_log_num_sets_in_recent_access_tag_array_l1;
            int ii;

            for (ii = 0; ii < (int)m_num_ways_in_recent_access_tag_array_l1; ii++)
            {
                if (recentAccessTagArrayL1[recentAccessTagArrayIndex][ii].valid &&
                    (recentAccessTagArrayL1[recentAccessTagArrayIndex][ii].tag == recentAccessTagArrayTag))
                {
                    break;
                }
            }

            if (ii == (int)m_num_ways_in_recent_access_tag_array_l1)
            {
                for (ii = 0; ii < (int)m_num_ways_in_recent_access_tag_array_l1; ii++)
                {
                    if (!recentAccessTagArrayL1[recentAccessTagArrayIndex][ii].valid)
                        break;
                }
                if (ii == (int)m_num_ways_in_recent_access_tag_array_l1)
                {
                    uint64_t maxlru = 0;
                    int repl_index = -1;
                    for (ii = 0; ii < (int)m_num_ways_in_recent_access_tag_array_l1; ii++)
                    {
                        if (recentAccessTagArrayL1[recentAccessTagArrayIndex][ii].lru > maxlru)
                        {
                            maxlru = recentAccessTagArrayL1[recentAccessTagArrayIndex][ii].lru;
                            repl_index = ii;
                        }
                    }
                    ii = repl_index;
                }
                recentAccessTagArrayL1[recentAccessTagArrayIndex][ii].tag = recentAccessTagArrayTag;
                recentAccessTagArrayL1[recentAccessTagArrayIndex][ii].valid = true;
            }

            for (int jj = 0; jj < (int)m_num_ways_in_recent_access_tag_array_l1; jj++)
            {
                recentAccessTagArrayL1[recentAccessTagArrayIndex][jj].lru++;
            }
            recentAccessTagArrayL1[recentAccessTagArrayIndex][ii].lru = 0;
        }

        bool
        IPCP::recentAccessTagArrayL1DetermineHit(uint64_t cl_addr)
        {
            int recentAccessTagArrayIndex = cl_addr & (m_num_sets_in_recent_access_tag_array_l1 - 1);
            uint64_t recentAccessTagArrayTag = cl_addr >> m_log_num_sets_in_recent_access_tag_array_l1;
            int ii;

            for (ii = 0; ii < (int)m_num_ways_in_recent_access_tag_array_l1; ii++)
            {
                if (recentAccessTagArrayL1[recentAccessTagArrayIndex][ii].valid &&
                    (recentAccessTagArrayL1[recentAccessTagArrayIndex][ii].tag == recentAccessTagArrayTag))
                {
                    return true;
                }
            }
            return false;
        }

        void
        IPCP::recentAccessTagArrayL1Insert(uint64_t cl_addr)
        {
            int recentAccessTagArrayIndex = cl_addr & (m_num_sets_in_recent_access_tag_array_l1 - 1);
            uint64_t recentAccessTagArrayTag = cl_addr >> m_log_num_sets_in_recent_access_tag_array_l1;
            int ii;

            for (ii = 0; ii < (int)m_num_ways_in_recent_access_tag_array_l1; ii++)
            {
                if (!recentAccessTagArrayL1[recentAccessTagArrayIndex][ii].valid)
                    break;
            }

            if (ii == (int)m_num_ways_in_recent_access_tag_array_l1)
            {
                uint64_t maxlru = 0;
                int repl_index = -1;
                for (ii = 0; ii < (int)m_num_ways_in_recent_access_tag_array_l1; ii++)
                {
                    if (recentAccessTagArrayL1[recentAccessTagArrayIndex][ii].lru > maxlru)
                    {
                        maxlru = recentAccessTagArrayL1[recentAccessTagArrayIndex][ii].lru;
                        repl_index = ii;
                    }
                }
                ii = repl_index;
            }

            recentAccessTagArrayL1[recentAccessTagArrayIndex][ii].tag = recentAccessTagArrayTag;
            recentAccessTagArrayL1[recentAccessTagArrayIndex][ii].valid = true;

            for (int jj = 0; jj < (int)m_num_ways_in_recent_access_tag_array_l1; jj++)
            {
                recentAccessTagArrayL1[recentAccessTagArrayIndex][jj].lru++;
            }
            recentAccessTagArrayL1[recentAccessTagArrayIndex][ii].lru = 0;
        }

        uint64_t
        IPCP::encodeMetadata(int stride, uint16_t type, int in_spec_nl)
        {
            uint64_t metadata = 0;

            // First encode stride in the last 8 bits
            if (stride > 0)
            {
                metadata = stride;
            }
            else
            {
                metadata = ((-1 * stride) | 0b1000000);
            }

            // Encode the type of IP in the next 4 bits
            metadata = metadata | (type << 8);

            // Encode the speculative NL bit in the next 1 bit
            metadata = metadata | (in_spec_nl << 12);

            return metadata;
        }

    } // namespace prefetch
} // namespace gem5
