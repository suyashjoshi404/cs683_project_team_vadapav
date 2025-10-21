#ifndef __MEM_CACHE_PREFETCH_IPCP_HH__
#define __MEM_CACHE_PREFETCH_IPCP_HH__

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "base/sat_counter.hh"
#include "base/types.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"
#include "params/IPCPPrefetcher.hh"

namespace gem5
{

    struct IPCPPrefetcherParams;

    GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
    namespace prefetch
    {

// Define constants
#define BASE_PREFETCH_DEGREE_L1 4
#define NUM_STRIDES_IN_LONG_HIST_IP_TABLE 20

        class IPCP : public Queued
        {
        protected:
            // Configuration parameters
            const uint64_t m_log_num_sets_in_recent_access_tag_array_l1;
            const uint64_t m_num_ways_in_recent_access_tag_array_l1;
            const uint64_t m_ip_table_tag_mask;
            const uint64_t m_log_num_sets_in_ip_table_l1;
            const uint64_t m_num_ways_in_ip_table_l1;
            const uint64_t m_ip_delta_table_tag_mask;
            const uint64_t m_log_num_sets_in_ip_delta_table_l1;
            const uint64_t m_num_ways_in_ip_delta_table_l1;
            const uint64_t m_saturating_counter_max_l1;
            const uint64_t m_base_prefetch_degree_l1;
            const uint64_t m_num_entries_in_nl_buffer_l1;
            const uint64_t m_nl_threshold_numer_l1;
            const uint64_t m_nl_threshold_denom_l1;
            const bool m_pointer_last;
            const bool m_pointer_non_last;
            const uint64_t m_stride_conf_max;
            const uint64_t m_stride_conf_threshold;
            const uint64_t m_partial_ip_mask;
            const uint64_t m_num_strides_in_long_hist_ip_table;
            const uint64_t m_long_hist_ip_table_tag_mask;
            const uint64_t m_num_entries_in_long_hist_ip_table;
            const uint64_t m_long_hist_match_length;
            const uint64_t m_num_ip_table_l1_entries;
            const uint64_t m_num_ghb_entries;
            const uint64_t m_num_ip_index_bits;
            const uint64_t m_num_ip_tag_bits;
            const uint64_t m_s_type;
            const uint64_t m_cs_type;
            const uint64_t m_cplx_type;
            const uint64_t m_nl_type;

            // Derived parameters
            const Addr m_page_shift = 12;
            uint64_t m_num_cpus;
            uint64_t m_log2_block_size;
            uint64_t m_num_sets_in_ip_table_l1;
            uint64_t m_prediction_threshold_l1;
            uint64_t m_num_sets_in_ip_delta_table_l1;
            uint64_t m_page_offset_mask;
            uint64_t m_throttle_level_max_l1;
            uint64_t m_num_sets_in_recent_access_tag_array_l1;

            // State variables
            unsigned char throttle_level_L1;
            uint64_t prev_cpu_cycle;
            uint64_t num_misses;
            float mpkc;
            int spec_nl;

            // Data structures
            struct NLBufferL1
            {
                uint64_t tag;
                uint64_t lru;
                bool valid;
                unsigned char degree;

                NLBufferL1() : tag(0), lru(0), valid(false), degree(0) {}
            };

            struct RecentAccessTagArrayL1
            {
                uint64_t tag;
                uint64_t lru;
                bool valid;

                RecentAccessTagArrayL1() : tag(0), lru(0), valid(false) {}
            };
            // MSD: THIS IS NOT IMPLEMENTED, ALWAYS HAS SIZE AND ALWAYS CAN SEND PREFETCH REQUESTS
            // ANYWAYS IT WILL GET DROPPED WHEN PRIORITY QUEUE OF QUEUED_PREFETCHER CLASS GETS FULL
            class PrefetchQueue
            {
            public:
                uint32_t occupancy = 0;
                uint32_t SIZE = 1;
                PrefetchQueue() {}
            };
            PrefetchQueue PQ;

            struct IPtableL1
            {
                uint64_t tag;
                uint64_t lru;
                bool valid;
                unsigned char offset;
                char stride[BASE_PREFETCH_DEGREE_L1 + 1];
                unsigned char conf;
                bool confPointer;

                IPtableL1() : tag(0), lru(0), valid(false), offset(0), conf(0), confPointer(false)
                {
                    for (int i = 0; i < BASE_PREFETCH_DEGREE_L1; i++)
                    {
                        stride[i] = 0;
                    }
                }
            };

            struct IPDeltaTableL1
            {
                uint64_t tag;
                uint64_t lru;
                bool valid;
                char stride[BASE_PREFETCH_DEGREE_L1];
                unsigned char counters[BASE_PREFETCH_DEGREE_L1];
                uint64_t partial_ip;
                bool partial_ip_valid;

                IPDeltaTableL1() : tag(0), lru(0), valid(false), partial_ip(0), partial_ip_valid(false)
                {
                    for (int i = 0; i < BASE_PREFETCH_DEGREE_L1; i++)
                    {
                        stride[i] = 0;
                        counters[i] = 0;
                    }
                }
            };

            struct LongHistIPtableL1
            {
                uint64_t ip;
                uint64_t lru;
                bool valid;
                uint64_t block_addr;
                char stride[NUM_STRIDES_IN_LONG_HIST_IP_TABLE];

                LongHistIPtableL1() : ip(0), lru(0), valid(false), block_addr(0)
                {
                    for (int i = 0; i < NUM_STRIDES_IN_LONG_HIST_IP_TABLE; i++)
                    {
                        stride[i] = 0;
                    }
                }
            };

            struct IP_TABLE_L1
            {
                uint64_t ip_tag;
                uint64_t last_page;
                uint64_t last_cl_offset;
                int64_t last_stride;
                uint16_t ip_valid;
                int conf;
                uint16_t signature;
                uint16_t str_dir;
                uint16_t str_valid;
                uint16_t str_strength;

                IP_TABLE_L1() : ip_tag(0), last_page(0), last_cl_offset(0), last_stride(0),
                                ip_valid(0), conf(0), signature(0), str_dir(0), str_valid(0), str_strength(0) {}
            };

            struct DELTA_PRED_TABLE
            {
                int delta;
                int conf;

                DELTA_PRED_TABLE() : delta(0), conf(0) {}
            };

            // Table pointers
            NLBufferL1 *nlBufferL1;
            RecentAccessTagArrayL1 **recentAccessTagArrayL1;
            IPtableL1 **ipTableL1;
            IPDeltaTableL1 **ipDeltaTableL1;
            LongHistIPtableL1 *longHistIPTableL1;
            char *longHistory;
            IP_TABLE_L1 *trackers_l1;
            DELTA_PRED_TABLE DPT_l1[4096];
            uint64_t *ghb_l1;

            // Auxiliary arrays
            unsigned degreeInsertionsL1[BASE_PREFETCH_DEGREE_L1];
            unsigned degreeHitsL1[BASE_PREFETCH_DEGREE_L1];
            char ipTableStride[BASE_PREFETCH_DEGREE_L1 + 1];
            char ipPrefetchStride[BASE_PREFETCH_DEGREE_L1 + 1];

            // Helper methods
            void nlBufferL1Insert(uint64_t cl_addr, int current_degree_index);
            void recentAccessTagArrayL1LookupAndInsertIfMiss(uint64_t cl_addr);
            bool recentAccessTagArrayL1DetermineHit(uint64_t cl_addr);
            void recentAccessTagArrayL1Insert(uint64_t cl_addr);
            uint16_t updateSignatureL1(uint16_t old_sig, int delta);
            uint64_t encodeMetadata(int stride, uint16_t type, int in_spec_nl);
            void checkForStreamL1(int index, uint64_t cl_addr);
            int updateConf(int stride, int pred_stride, int conf);

            // Main prefetcher logic
            void l1dPrefetcherOperate(uint64_t addr, uint64_t ip, uint8_t cache_hit,
                                      std::vector<AddrPriority> &addresses);

        public:
            IPCP(const IPCPPrefetcherParams &p);
            ~IPCP();

            void calculatePrefetch(const PrefetchInfo &pfi,
                                   std::vector<AddrPriority> &addresses,
                                   const CacheAccessor &cache) override;
        };

    } // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_IPCP_HH__
