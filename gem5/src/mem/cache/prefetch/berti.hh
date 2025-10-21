//
// Created by linjiawei on 22-10-31.
//

#ifndef __MEM_CACHE_PREFETCH_BERTI_HH__
#define __MEM_CACHE_PREFETCH_BERTI_HH__

#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "debug/BertiPrefetcher.hh"
#include "base/cache/associative_cache.hh"
#include "mem/packet.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/cache/tags/tagged_entry.hh"
#include "params/BertiPrefetcher.hh"

namespace gem5
{

struct BertiPrefetcherParams;

namespace prefetch
{

class BertiPrefetcher : public Queued
{
  protected:
    struct HistoryInfo
    {
        Addr lineAddr;
        Cycles timestamp;
    };

    class HistoryTableEntry : public TaggedEntry
    {
      public:
        /** FIFO of demand miss history. */
        std::vector<HistoryInfo> history;

        HistoryTableEntry(TagExtractor ext): TaggedEntry() { registerTagExtractor(ext);history = std::vector<HistoryInfo>(); }
    };

    AssociativeCache<HistoryTableEntry> historyTable;

    enum DeltaStatus { L1_PREF, L2_PREF, NO_PREF };

    struct DeltaInfo
    {
        uint8_t coverageCounter;
        int64_t delta;
        DeltaStatus status;
    };

    class TableOfDeltasEntry : public TaggedEntry
    {
      public:
        std::vector<DeltaInfo> deltas;
        uint8_t counter;
        int64_t best_delta;

        void resetConfidence(bool reset_status)
        {
            counter = 0;
            for (auto &info : deltas) {
                info.coverageCounter = 0;
                if (reset_status) {
                    info.status = NO_PREF;
                }
            }
            if (reset_status) {
                best_delta = 0;
            }
        }

        // void updateStatus()
        // {
        //     uint8_t min_cov = 0;
        //     for (auto &info : deltas) {
        //         info.status = info.coverageCounter >=
        //                               counter * 0.35 ? L2_PREF : NO_PREF;
        //         if (info.status == L2_PREF && info.coverageCounter > min_cov) {
        //             min_cov = info.coverageCounter;
        //             best_delta = info.delta;
        //         }
        //     }
        //     if (min_cov == 0) {
        //         best_delta = 0;
        //     }
        // }

        void updateStatus()
        {
            for (auto &info : deltas) {
                info.status = info.coverageCounter >=
                                      counter * 0.35 ? L2_PREF : NO_PREF;
                info.coverageCounter = 0;
            }
            counter = 0;
        }

        TableOfDeltasEntry(TagExtractor ext)
            : TaggedEntry(), counter(0), best_delta(0)
        {
            registerTagExtractor(ext);
            deltas = std::vector<DeltaInfo>(16);
            resetConfidence(true);
            best_delta = 0;
        }
    };

    AssociativeCache<TableOfDeltasEntry> tableOfDeltas;

    Cycles lastFillLatency;
    bool aggressive_pf;

    struct prefetch_fill_latency
    {
        Addr addr;
        bool is_secure;
        Cycles latency;
    };
    std::list<prefetch_fill_latency> prefetch_latency_container;


    struct BertiStats : public statistics::Group
    {
        BertiStats(statistics::Group *parent);
        // train
        statistics::Scalar num_train_hit;
        statistics::Scalar num_train_miss;
        statistics::SparseHistogram train_pc;
        // fill
        statistics::Scalar num_fill_prefetch;
        statistics::Scalar num_fill_miss;
        statistics::SparseHistogram fill_pc;
        statistics::SparseHistogram fill_latency;
        // prefetch
        statistics::SparseHistogram pf_delta;
    } statsBerti;


    /** Update history table on demand miss. */
    void updateHistoryTable(const PrefetchInfo &pfi);

    /** Update table of deltas on cache fill. */
    void updateTableOfDeltas(const Addr pc, const bool isSecure,
                             const std::vector<int64_t> &new_deltas);

    /** Search for timely deltas. */
    void searchTimelyDeltas(const HistoryTableEntry &entry,
                            const Cycles &latency,
                            const Cycles &demand_cycle,
                            const Addr &blk_addr,
                            std::vector<int64_t> &deltas);

    void notifyFill(const CacheAccessProbeArg &arg) override;


    void printDeltaTableEntry(const TableOfDeltasEntry &entry) {
        DPRINTF(BertiPrefetcher, "Entry Counter: %d\n", entry.counter);
        for (auto &info : entry.deltas) {
            DPRINTF(BertiPrefetcher,
                    "=>[delta: %d coverage: %d status: %d]\n",
                    info.delta, info.coverageCounter, info.status);
        }
    }

    void cleanPrefetchLatency(const CacheAccessor &cache)
    {
        std::list<prefetch_fill_latency>::iterator it;
        for (it = prefetch_latency_container.begin(); it != prefetch_latency_container.end();) 
        {
            if(!cache.inCache(it->addr,it->is_secure))
            {
                DPRINTF(BertiPrefetcher, "Cleaned latency %d for addr [%lx]\n", it->latency, it->addr);
                it = prefetch_latency_container.erase(it);
            }
            else
            {
                it++;
            }
        }
    }

  public:
    BertiPrefetcher(const BertiPrefetcherParams &p);

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addressed,const CacheAccessor &cache) override;
};

}

}


#endif