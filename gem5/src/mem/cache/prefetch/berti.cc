//
// Created by linjiawei on 22-10-31.
//

#include "mem/cache/prefetch/berti.hh"

#include "debug/BertiPrefetcher.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"

#include <cassert>

namespace gem5
{
namespace prefetch
{

BertiPrefetcher::BertiStats::BertiStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(num_train_hit, statistics::units::Count::get(), ""),
      ADD_STAT(num_train_miss, statistics::units::Count::get(), ""),
      ADD_STAT(train_pc, statistics::units::Count::get(), ""),
      ADD_STAT(num_fill_prefetch, statistics::units::Count::get(), ""),
      ADD_STAT(num_fill_miss, statistics::units::Count::get(), ""),
      ADD_STAT(fill_pc, statistics::units::Count::get(), ""),
      ADD_STAT(fill_latency, statistics::units::Count::get(), ""),
      ADD_STAT(pf_delta, statistics::units::Count::get(), "")
{
    train_pc.init(0);
    fill_pc.init(0);
    fill_latency.init(0);
    pf_delta.init(0);
}

BertiPrefetcher::BertiPrefetcher(const BertiPrefetcherParams &p)
    : Queued(p),
      historyTable((name() + ".HistoryTable").c_str(),
                   p.history_table_entries,
                   p.history_table_assoc,
                   p.history_table_replacement_policy,                   
                   p.history_table_indexing_policy,
                   HistoryTableEntry(genTagExtractor(p.history_table_indexing_policy))),
      tableOfDeltas((name() + ".TableOfDeltas").c_str(),
                    p.table_of_deltas_entries, 
                    p.table_of_deltas_entries,
                    p.table_of_deltas_replacement_policy,
                    p.table_of_deltas_indexing_policy,
                    TableOfDeltasEntry(genTagExtractor(p.table_of_deltas_indexing_policy))),
      aggressive_pf(p.aggressive_pf),
      statsBerti(this)
{
}

void
BertiPrefetcher::updateHistoryTable(const PrefetchInfo &pfi)
{
    HistoryTableEntry *entry =
        historyTable.findEntry({pfi.getPC(), pfi.isSecure()});
    HistoryInfo new_info = {blockIndex(pfi.getAddr()), curCycle()};
    if (entry) {
        DPRINTF(BertiPrefetcher,
                "History table hit, ip: [%lx] lineAddr: [%lx]\n", pfi.getPC(),
                new_info.lineAddr);
        if (entry->history.size() == 16) {
            entry->history.erase(entry->history.begin());
        }
        entry->history.push_back(new_info);
    } else {
        DPRINTF(BertiPrefetcher, "History table miss, ip: [%lx]\n",
                pfi.getPC());
        entry = historyTable.findVictim({pfi.getPC(), pfi.isSecure()});
        historyTable.invalidate(entry);
        entry->history.clear();
        entry->history.push_back(new_info);
        historyTable.insertEntry({pfi.getPC(), pfi.isSecure()}, entry);
    }
}

void
BertiPrefetcher::updateTableOfDeltas(
    const Addr pc, const bool isSecure,
    const std::vector<int64_t> &new_deltas)
{
    if (new_deltas.empty())
        return;

    TableOfDeltasEntry *entry =
        tableOfDeltas.findEntry({pc, isSecure});
    if (!entry) {
        entry = tableOfDeltas.findVictim({pc, isSecure});
        tableOfDeltas.invalidate(entry);
        entry->resetConfidence(true);
        tableOfDeltas.insertEntry({pc, isSecure}, entry);
    }

    entry->counter++;
    for (auto &delta : new_deltas) {
        bool miss = true;
        for (auto &delta_info : entry->deltas) {
            if (delta_info.coverageCounter != 0 && delta_info.delta == delta) {
                delta_info.coverageCounter++;
                miss = false;
                break;
            }
        }
        // miss
        if (miss) {
            int replace_idx = 0;
            for (auto i = 1; i < entry->deltas.size(); i++) {
                if (entry->deltas[replace_idx].coverageCounter
                    >= entry->deltas[i].coverageCounter) {
                    replace_idx = i;
                }
            }
            entry->deltas[replace_idx].delta = delta;
            entry->deltas[replace_idx].coverageCounter = 1;
            entry->deltas[replace_idx].status = NO_PREF;
        }
    }

    if (entry->counter >= 8) {
        entry->updateStatus();
        if (entry->counter == 16) {
            /** Start a new learning phase. */
            entry->resetConfidence(false);
        }
    }
    printDeltaTableEntry(*entry);
}



void BertiPrefetcher::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addressed, const CacheAccessor &cache) 
{
    DPRINTF(BertiPrefetcher,
            "Train prefetcher, ip: [%lx] "
            "lineAddr: [%lx] miss: %d last lat: [%d]\n",
            pfi.getPC(), blockIndex(pfi.getAddr()),
            pfi.isCacheMiss(), lastFillLatency);
    if (pfi.isCacheMiss()) {
        statsBerti.num_train_miss++;
    } else {
        statsBerti.num_train_hit++;
    }
    cleanPrefetchLatency(cache);

    // 2. Learning timely deltas

    //A new entry is inserted in the history table either (1) on-demand misses or (2) on hits for prefetched cache lines.
    if(pfi.isCacheMiss()||(!pfi.isCacheMiss()&&cache.hasBeenPrefetched(pfi.getAddr(), pfi.isSecure())))
    {
        updateHistoryTable(pfi);
    }

    // The search for timely deltas is performed either (1) on a fill due to a demand access or (2) on a hit due to a prefetched cache line
    if(!pfi.isCacheMiss() && cache.hasBeenPrefetched(pfi.getAddr(), pfi.isSecure()))
    {
        HistoryTableEntry *hist_entry = historyTable.findEntry({pfi.getPC(), pfi.isSecure()});
        if (hist_entry) {

            bool found = false;
            pfi.isCacheMiss();
            cache.hasBeenPrefetched(pfi.getAddr(), pfi.isSecure());
            std::list<prefetch_fill_latency>::iterator it;
            for (it = prefetch_latency_container.begin(); it != prefetch_latency_container.end() && !found; it++) {
                DPRINTF(BertiPrefetcher, "Checking latency for addr [%lx] pfi addr [%lx] blockAddress [%lx]\n", it->addr, pfi.getAddr() ,blockAddress(pfi.getAddr()));
                found = (it->addr == blockAddress(pfi.getAddr()));
                if(found)
                DPRINTF(BertiPrefetcher, "Found latency %d for addr [%lx]\n", it->latency, it->addr);
            }
            assert(found);
            Cycles PrefetchFillLatency = it->latency;
            std::vector<int64_t> deltas;
            searchTimelyDeltas(*hist_entry, PrefetchFillLatency,
                               curCycle(),
                               blockIndex(pfi.getAddr()), deltas);
            updateTableOfDeltas(pfi.getPC(), pfi.isSecure(), deltas);
        }
    }
    statsBerti.train_pc.sample(pfi.getPC());


    // Issuing prefetch requests

    TableOfDeltasEntry *entry =
        tableOfDeltas.findEntry({pfi.getPC(), pfi.isSecure()});
    if (entry) {
        DPRINTF(BertiPrefetcher, "Delta table hit, ip: [%lx]\n", pfi.getPC());
        tableOfDeltas.accessEntry(entry);
        if (aggressive_pf) {
            for (auto &delta_info : entry->deltas) {
                if (delta_info.status == L2_PREF) {
                    DPRINTF(BertiPrefetcher, "Using delta [%d] to prefetch\n",
                            delta_info.delta);
                    int64_t delta = delta_info.delta;
                    statsBerti.pf_delta.sample(delta);
                    Addr pf_addr =
                        (blockIndex(pfi.getAddr()) + delta) << lBlkSize;
                    addressed.push_back(AddrPriority(pf_addr, 0));
                }
            }
        } else {
            if (entry->best_delta != 0) {
                DPRINTF(BertiPrefetcher, "Using delta [%d] to prefetch\n",
                        entry->best_delta);
                statsBerti.pf_delta.sample(entry->best_delta);
                Addr pf_addr = (blockIndex(pfi.getAddr()) +
                                entry->best_delta) << lBlkSize;
                addressed.push_back(AddrPriority(pf_addr, 0));
            }
        }
    }
}


void BertiPrefetcher::searchTimelyDeltas(
    const HistoryTableEntry &entry,
    const Cycles &latency,
    const Cycles &demand_cycle,
    const Addr &blk_addr,
    std::vector<int64_t> &deltas)
{
    for (auto it = entry.history.rbegin(); it != entry.history.rend(); it++) {
        // if not timely, skip and continue
        if (it->timestamp + latency > demand_cycle)
            continue;
        int64_t delta = blk_addr - it->lineAddr;
        if (delta != 0) {
            deltas.push_back(delta);
            DPRINTF(BertiPrefetcher, "Timely delta found: [%d](%lx - %lx)\n",
                    delta, blk_addr, it->lineAddr);
            // We don't want to many deltas
            if (deltas.size() == 8)
                break;
        }
    }
}

void BertiPrefetcher::notifyFill(const CacheAccessProbeArg &arg)
{
    const PacketPtr& pkt = arg.pkt;
    DPRINTF(BertiPrefetcher, "Receive: %s\n", pkt->print());
    bool isInstFetch = pkt->req->isInstFetch();
    bool hasVaddr = pkt->req->hasVaddr();
    bool hasPC = pkt->req->hasPC();
    if (isInstFetch || !hasVaddr|| !hasPC) {
        DPRINTF(BertiPrefetcher, "Skip packet: %s pf %s hasVaddr %s hasPC %s\n", pkt->print(), 
        pkt->req->isPrefetch()?"yes":"no", hasVaddr?"yes":"no", hasPC?"yes":"no");
        return;
    }
    DPRINTF(BertiPrefetcher,
            "Cache Fill: %s isPF: %d\n",
            pkt->print(), pkt->req->isPrefetch());
    if (pkt->req->isPrefetch()) {
        statsBerti.num_fill_prefetch++;
    } else {
        statsBerti.num_fill_miss++;
    }
    cleanPrefetchLatency(arg.cache);
    Addr addr = pkt->req->getPaddr();
    DPRINTF(BertiPrefetcher, "Debug Fill for addr [%lx] paddr [%lx]\n", addr, pkt->req->getPaddr());
    if( addr == 0x15d5bc80){
        addr = pkt->req->getVaddr();
        assert(true);
    }
    // 1. Measuring fetch latency
    Cycles latency = ticksToCycles(curTick() - pkt->req->time());
    lastFillLatency = latency;
    if (pkt->req->isPrefetch()) {
        prefetch_fill_latency new_latency;
        new_latency.addr = blockAddress(pkt->req->getVaddr());
        new_latency.is_secure = pkt->req->isSecure();
        new_latency.latency = lastFillLatency;
        prefetch_latency_container.push_back(new_latency);
        DPRINTF(BertiPrefetcher, "Adding latency %d for addr [%lx] orig vaddr [%lx]\n", new_latency.latency, new_latency.addr, pkt->req->getVaddr());
    }

    statsBerti.fill_pc.sample(pkt->req->getPC());

    /** Search history table, find deltas. */
    Cycles demand_cycle = ticksToCycles(pkt->req->time());
    Cycles wrappedLatency;

    if (latency % 10 == 0) {
        wrappedLatency = Cycles((latency / 10) * 10);
    } else {
        wrappedLatency = Cycles( ((latency / 10) + 1) * 10 );
    }
    statsBerti.fill_latency.sample(wrappedLatency);

    DPRINTF(BertiPrefetcher, "Updating table of deltas, latency [%d]\n",
            latency);

    

    // 2. Learning timely deltas

    // The search for timely deltas is performed either (1) on a fill due to a demand access or (2) on a hit due to a prefetched cache line
    if (!pkt->req->isPrefetch()) {
        HistoryTableEntry *entry =
                 historyTable.findEntry({pkt->req->getPC(), pkt->req->isSecure()});
        if (!entry)
            return;
        std::vector<int64_t> timely_deltas = std::vector<int64_t>();
        searchTimelyDeltas(*entry, lastFillLatency, demand_cycle,
                       blockIndex(pkt->req->getVaddr()),
                       timely_deltas);
        updateTableOfDeltas(pkt->req->getPC(), pkt->req->isSecure(),
                        timely_deltas);
    }
}



}
}