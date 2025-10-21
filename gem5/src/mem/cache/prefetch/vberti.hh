/*
 * Copyright (c) 2018 Inria
 * Copyright (c) 2012-2013, 2015, 2022, 2024 Arm Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2005 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 * Describes a strided prefetcher.
 */

#ifndef __MEM_CACHE_PREFETCH_STRIDE_HH__
#define __MEM_CACHE_PREFETCH_STRIDE_HH__

#include <string>
#include <unordered_map>
#include <vector>
#include <tuple>
#include <queue>

#include "base/cache/associative_cache.hh"
#include "base/sat_counter.hh"
#include "base/types.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/cache/replacement_policies/replaceable_entry.hh"
#include "mem/cache/tags/indexing_policies/set_associative.hh"
#include "mem/cache/tags/tagged_entry.hh"
#include "mem/packet.hh"
#include "params/BertiRubyPrefetcher.hh"
#include "debug/BertiRubyPrefetcher.hh"

namespace gem5
{

    namespace replacement_policy
    {
        class Base;
    }
    struct BertiRubyPrefetcherParams;

    namespace prefetch
    {

#define ENTANGLING_HASH
#define NO_CROSS_PAGE

/*Berti specific macros*/
#define PC_MASK (0x3FF)
#define TIME_MASK (0xFFFF)
#define LAT_MASK (0xFFF)
#define STRIDE_MASK (12)
// # define LAT_MASK                     (0xFFFF)
#define ADDR_MASK (0xFFFFFF)
#define LANZAR_INT 8
// Confidence
#define CONFIDENCE_MAX (16) // 6 bits
#define CONFIDENCE_INC (1)  // 6 bits
#define CONFIDENCE_INIT (1) // 6 bits
#define CONFIDENCE_L1 (65)  // 6 bits
#define CONFIDENCE_L2 (50)  // 6 bits
#define CONFIDENCE_L2R (35) // 6 bits
#define MSHR_LIMIT (70)

// Stride rpl
// L1, L2, L2 reemplazable y No (reemplazable).
#define R (0x0)
#define L1 (0x1)
#define L2 (0x2)
#define L2R (0x3)

#define SIZE_1X

#if defined(SIZE_1X)
// MICRO Size
#define HISTORY_TABLE_SET (8)
#define HISTORY_TABLE_WAY (16)
#define TABLE_SET_MASK (0x7)

#define BERTI_TABLE_SIZE (16)
#define BERTI_TABLE_STRIDE_SIZE (16)
#define LOG2_BLOCK_SIZE (6)
#endif
/*                          */

        /*****************************************************************************
         *                      General Structs                                      *
         *****************************************************************************/

        typedef struct Delta
        {
            uint64_t conf;
            int64_t delta;
            uint8_t rpl;
            Delta() : conf(0), delta(0), rpl(0) {};
        } delta_t;

        /*****************************************************************************
         *                      Berti structures                                     *
         *****************************************************************************/

        class BertiRubyPrefetcher : public Queued
        {
        private:
            class LatencyTable : public Named
            {
                /* Latency table simulate the modified PQ and MSHR */
            private:
                struct latency_table
                {
                    uint64_t addr = 0; // Addr
                    uint64_t tag = 0;  // IP-Tag
                    uint64_t time = 0; // Event cycle
                    bool pf = false;   // Is the entry accessed by a demand miss
                };
                int size;

                latency_table *latencyt;
                BertiRubyPrefetcher *parent;

            public:
                LatencyTable(const int size, BertiRubyPrefetcher *_parent) : Named("LatencyTable"), size(size), parent(_parent)
                {
                    latencyt = new latency_table[size];
                }
                ~LatencyTable() { delete latencyt; }

                uint8_t add(uint64_t addr, uint64_t tag, bool pf);
                uint64_t get(uint64_t addr);
                uint64_t del(uint64_t addr);
                uint64_t get_tag(uint64_t addr);
            };

            class ShadowCache: public Named
            {
            private:
                struct ShadowCacheEntry : public TaggedEntry
                {
                    ShadowCacheEntry(TagExtractor ext) : TaggedEntry()
                    {
                        registerTagExtractor(ext);
                        invalidate();
                    }
                    uint64_t addr = 0; // Address
                    uint64_t lat = 0;  // Latency
                    bool pf = false;   // Is a prefetch

                    // Required by AssociativeCache
                    bool isValid() const override { return addr != 0; }
                    void invalidate() override
                    {
                        addr = 0;
                        pf = false;
                        lat = 0;
                    }
                    Addr getTag() const { return addr; } // Used for finding entries
                };

                AssociativeCache<ShadowCacheEntry> cache;

            public:
                ShadowCache(uint64_t entries, uint64_t assoc, replacement_policy::Base *repl_policy, TaggedEntry::IndexingPolicy *indexing_policy)
                    :Named("ShadowCache") ,cache("BertiRubyPrefetcher.ShadowCache",
                            entries, assoc, repl_policy, indexing_policy,
                            ShadowCacheEntry(genTagExtractor(indexing_policy)))
                {
                }

                ~ShadowCache()
                {
                }

                bool add(uint64_t addr, bool pf, uint64_t lat);
                bool get(uint64_t addr);
                void set_pf(uint64_t addr, bool pf);
                bool is_pf(uint64_t addr);
                uint64_t get_latency(uint64_t addr);
            };

            class HistoryTable : public Named
            {
                /* History Table */
            private:
                struct history_table
                {
                    uint64_t tag = 0;  // IP Tag
                    uint64_t addr = 0; // IP @ accessed
                    uint64_t time = 0; // Time where the line is accessed
                }; // This struct is the history table

                uint64_t sets;
                uint64_t ways;

                history_table **historyt;
                history_table **history_pointers;
                BertiRubyPrefetcher *parent;

                uint16_t get_aux(uint32_t latency, uint64_t tag, uint64_t act_addr,
                                 std::vector<uint64_t> &tags, std::vector<uint64_t> &addr, uint64_t cycle);

            public:
                HistoryTable(uint64_t sets_in, uint64_t ways_in, BertiRubyPrefetcher *_parent) : Named("HistoryTable"), parent(_parent)
                {
                    sets = sets_in;
                    ways = ways_in;
                    history_pointers = new history_table *[sets];
                    historyt = new history_table *[sets];

                    for (int i = 0; i < sets; i++)
                        historyt[i] = new history_table[ways];
                    for (int i = 0; i < sets; i++)
                        history_pointers[i] = historyt[i];
                }

                ~HistoryTable()
                {
                    for (int i = 0; i < sets; i++)
                        delete historyt[i];
                    delete historyt;

                    delete history_pointers;
                }

                int get_ways();
                void add(uint64_t tag, uint64_t addr);
                uint16_t get(uint32_t latency, uint64_t tag, uint64_t act_addr,
                             std::vector<uint64_t> &tags, std::vector<uint64_t> &addr, uint64_t cycle);
            };

            class Berti :public Named
            {
                /* Berti Table */
            private:
                struct berti
                {
                    std::vector<delta_t> deltas; //         std::array<delta_t, BERTI_TABLE_DELTA_SIZE> deltas;
                    uint64_t conf = 0;
                    uint64_t total_used = 0;
                    berti(size_t delta_size) : deltas(delta_size) {}
                };

                std::map<uint64_t, berti *> bertit;
                std::queue<uint64_t> bertit_queue;

                uint64_t size = 0;

                bool static compare_greater_delta(delta_t a, delta_t b);
                bool static compare_rpl(delta_t a, delta_t b);
                bool static compare_greater_conf(delta_t a, delta_t b);

                void increase_conf_tag(uint64_t tag);
                void add(uint64_t tag, int64_t delta);
                BertiRubyPrefetcher *parent;

            public:
                Berti(uint64_t p_size, BertiRubyPrefetcher *_parent) : Named("BertiTable"), size(p_size), parent(_parent)
                {
                }
                ~Berti()
                {
                    for (auto &pair : bertit)
                    {
                        delete pair.second;
                    }
                }
                void find_and_update(uint64_t latency, uint64_t tag, uint64_t cycle,
                                     uint64_t line_addr, HistoryTable *historyt);
                uint8_t get(uint64_t tag, std::vector<delta_t> &res);
                uint64_t ip_hash(uint64_t ip);
            };

        protected:
            ShadowCache scache;
            LatencyTable *latencyt;
            HistoryTable *historyt;
            Berti *berti;
            int mshr_load = 0; // in %
            Addr last_replaced_addr;

        public:
            int degree;
            void notifyFill(const CacheAccessProbeArg &arg) override;
            BertiRubyPrefetcher(const BertiRubyPrefetcherParams &p);

            void calculatePrefetch(const PrefetchInfo &pfi,
                                   std::vector<AddrPriority> &addresses,
                                   const CacheAccessor &cache) override;
        };

    } // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_STRIDE_HH__
