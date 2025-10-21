#include "mem/cache/prefetch/vberti.hh"

namespace gem5
{
namespace prefetch
{

BertiRubyPrefetcher::BertiRubyPrefetcher(const BertiRubyPrefetcherParams &p)
    : Queued(p), scache(p.shadow_table_entries, p.shadow_table_assoc, p.shadow_table_repl_policy, p.shadow_table_indexing_policy), degree(p.degree)
{
    latencyt = new LatencyTable(p.latency_table_size, this);
    historyt = new HistoryTable(p.history_table_sets, p.history_table_ways, this);
    berti = new Berti(p.berti_table_delta_size, this);
}

void BertiRubyPrefetcher::notifyFill(const CacheAccessProbeArg &arg)
{
    if(!arg.pkt->req->hasVaddr())
        return;
    Addr line_addr = arg.pkt->req->getVaddr();
    line_addr = blockIndex(line_addr);
    // Remove @ from latency table
    uint64_t tag = latencyt->get_tag(line_addr);
    uint64_t cycle = latencyt->get(line_addr); latencyt->del(line_addr);
    uint64_t latency = 0;
    uint64_t curr_cycle = curCycle() & TIME_MASK;

    if (cycle != 0 && (curr_cycle > cycle))
        latency = curr_cycle - cycle;

    if (latency > LAT_MASK)
        latency = 0;
    bool prefetch = arg.pkt->req->isPrefetch();
    DPRINTF(BertiRubyPrefetcher, "Notify Fill: Tag %x Addr %x PF %d Curr Cycle %d Enq Cycle %d Lat %d\n",tag, line_addr, prefetch, curr_cycle, cycle, latency);

    // Add to the shadow cache
    scache.add(line_addr, prefetch, latency);

    if (latency != 0 && !prefetch)
    {
        berti->find_and_update(latency, tag, cycle, line_addr, historyt);
    }
}

void BertiRubyPrefetcher::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, const CacheAccessor &cache)
{
    Addr lineAddr = blockIndex(pfi.getAddr());
    Addr pc = pfi.getPC();

    pc = ((pc >> 1) ^ (pc >> 4));
    pc = pc & PC_MASK;
    bool cache_hit = !pfi.isCacheMiss();
    DPRINTF(BertiRubyPrefetcher, "Calculate Prefetch: PC %x LineAddr %x Hit %d\n", pc, lineAddr, cache_hit);
    if (!cache_hit)
    {
        latencyt->add(lineAddr, pc, 1);
        historyt->add(pc, lineAddr);
    }
    else if (cache_hit && scache.is_pf(lineAddr))
    {
        scache.set_pf(lineAddr, false);
        uint64_t latency = scache.get_latency(lineAddr);
        uint64_t cycle = curCycle() & TIME_MASK;
        berti->find_and_update(latency, pc, cycle, lineAddr, historyt);
        historyt->add(pc, lineAddr);
    }
    else
    {
        scache.set_pf(lineAddr, false);
    }
    std::vector<delta_t> deltas(BERTI_TABLE_STRIDE_SIZE);
    for (int i = 0; i < degree; i++)
    {
        deltas[i].conf = 0;
        deltas[i].delta = 0;
        deltas[i].rpl = R;
    }
    auto berti_result = berti->get(pc, deltas);

    if (berti_result == 0)
    {
        DPRINTF(BertiRubyPrefetcher, "Calculate Prefetch: No deltas found for PC %x\n", pc);
        return;
    }
    int launched = 0;
    for (auto &i : deltas)
    {
        Addr pAddr = (lineAddr + i.delta) << LOG2_BLOCK_SIZE;
        if (!latencyt->get(pAddr) && i.delta != 0) // Avoids redundant prefetches with stride 0
        {
            DPRINTF(BertiRubyPrefetcher, "Calculate Prefetch: Enqueue PF Addr %x\n", pAddr);
            addresses.push_back(AddrPriority(pAddr, 0));
            launched++;
            if(launched == degree)
                break;
        }
    }
}

/******************************************************************************/
/*                      Latency table functions                               */
/******************************************************************************/
uint8_t BertiRubyPrefetcher::LatencyTable::add(uint64_t addr, uint64_t tag, bool pf)
{
    /*
     * Save if possible the new miss into the pqmshr (latency) table
     *
     * Parameters:
     *  - addr: address without cache offset
     *  - access: is theh entry accessed by a demand request
     *  - cycle: time to use in the latency table
     *
     * Return: pf
     */

    latency_table *free = nullptr;
    uint64_t curr_cycle = parent->curCycle();
    curr_cycle &= TIME_MASK;
    DPRINTF(BertiRubyPrefetcher, "Latency Table Add: Tag %x Addr %x Pf %d\n", tag, addr, pf);
    Cycles cycle(curr_cycle);
    for (int i = 0; i < size; i++)
    {
        // Search if the addr already exists. If it exist we does not have
        // to do nothing more
        if (latencyt[i].addr == addr)
        {
            // latencyt[i].time = cycle;
            latencyt[i].pf = pf;
            latencyt[i].tag = tag;
            DPRINTF(BertiRubyPrefetcher, "Latency Table Add: Entry exists\n");
            return latencyt[i].pf;
        }

        // We discover a free space into the latency table, save it for later
        if (latencyt[i].tag == 0)
            free = &latencyt[i];
    }

    if (free == nullptr){
        DPRINTF(BertiRubyPrefetcher, "Latency Table Add: No free space\n");
        return 0;
    }

    // We save the new entry into the latency table
    // DPRINTF(BertiRubyPrefetcher, "New entry LatencyTable %#x\n", addr);
    free->addr = addr;
    free->time = cycle;
    free->tag = tag;
    free->pf = pf;

    return free->pf;
}

uint64_t BertiRubyPrefetcher::LatencyTable::get(uint64_t addr)
{
    /*
     * Return time or 0 if the addr is or is not in the pqmshr (latency) table
     *
     * Parameters:
     *  - addr: address without cache offset
     *
     * Return: time if the line is in the latency table, otherwise 0
     */

    for (int i = 0; i < size; i++)
    {
        // Search if the addr already exists
        if (latencyt[i].addr == addr)
        {
            DPRINTF(BertiRubyPrefetcher, "Latency Table Get: IP %x Addr %x Lat %d\n", latencyt[i].tag, addr, latencyt[i].time);
            return latencyt[i].time;
        }
    }
    DPRINTF(BertiRubyPrefetcher, "Latency Table Get: Addr %x not found\n",addr);
    return 0;
}

uint64_t BertiRubyPrefetcher::LatencyTable::del(uint64_t addr)
{
    /*
     * Remove the address from the latency table
     *
     * Parameters:
     *  - addr: address without cache offset
     *
     *  Return: the latency of the address
     */

    for (int i = 0; i < size; i++)
    {
        // Line already in the table
        if (latencyt[i].addr == addr)
        {
            // Calculate latency
            uint64_t time = latencyt[i].time;
            DPRINTF(BertiRubyPrefetcher, "Latency Table Del: IP %x Addr %x\n", latencyt[i].tag, addr);
            latencyt[i].addr = 0; // Free the entry
            latencyt[i].tag = 0;  // Free the entry
            latencyt[i].time = 0; // Free the entry
            latencyt[i].pf = 0;   // Free the entry
            // Return the latency
            return time;
        }
    }
    DPRINTF(BertiRubyPrefetcher, "Latency Table Del: Addr %x not present\n", addr);
    // We should always track the misses
    return 0;
}

uint64_t BertiRubyPrefetcher::LatencyTable::get_tag(uint64_t addr)
{
    /*
     * Return IP-Tag or 0 if the addr is or is not in the pqmshr (latency) table
     *
     * Parameters:
     *  - addr: address without cache offset
     *
     * Return: ip-tag if the line is in the latency table, otherwise 0
     */

    for (int i = 0; i < size; i++)
    {
        if (latencyt[i].addr == addr && latencyt[i].tag) // This is the address
        {
            DPRINTF(BertiRubyPrefetcher, "Latency Table Get IP: IP %x Addr %x\n", latencyt[i].tag, addr);
            return latencyt[i].tag;
        }
    }

    return 0;
}

/******************************************************************************/
/*                       History Table functions                               */
/******************************************************************************/

uint16_t BertiRubyPrefetcher::HistoryTable::get_aux(uint32_t latency, uint64_t tag, uint64_t act_addr, std::vector<uint64_t> &tags, std::vector<uint64_t> &addr, uint64_t cycle)
{
    /*
     * Return an array (by parameter) with all the possible PC that can launch
     * an on-time and late prefetch
     *
     * Parameters:
     *  - tag: PC tag
     *  - latency: latency of the processor
     */

    uint16_t num_on_time = 0;
    uint16_t set = tag & TABLE_SET_MASK;
    assert(set < sets);

    // This is the begin of the simulation
    if (cycle < latency)
        return num_on_time;

    // The IPs that is launch in this cycle will be able to launch this prefetch
    cycle -= latency;

    // Pointer to guide
    history_table *pointer = history_pointers[set];

    do
    {
        // Look for the IPs that can launch this prefetch
        if (pointer->tag == tag && pointer->time <= cycle)
        {
            DPRINTF(BertiRubyPrefetcher, "History Table Get Aux: Found Match for Tag %x\n", tag);
            // Test that addr is not duplicated
            if (pointer->addr == act_addr)
                return num_on_time;

            for (int i = 0; i < num_on_time; i++)
            {
                if (pointer->addr == addr[i]) return num_on_time;
            }
            assert(num_on_time < tags.size());
            assert(num_on_time < addr.size());

            // This IP can launch the prefetch
            tags[num_on_time] = pointer->tag;
            addr[num_on_time] = pointer->addr;
            DPRINTF(BertiRubyPrefetcher, "History Table Get Aux: Found Timely Deltas IP %x Addr %x\n",pointer->tag, pointer->addr);
            num_on_time++;
        }

        if (pointer == historyt[set])
        {
            // We get at the end of the history, we start again
            DPRINTF(BertiRubyPrefetcher, "History Table Get Aux: Reached end of table, restart\n");
            pointer = &historyt[set][ways - 1];
        }
        else
            pointer--;
    } while (pointer != history_pointers[set]);

    return num_on_time;
}

void BertiRubyPrefetcher::HistoryTable::add(uint64_t tag, uint64_t addr)
{
    /*
     * Save the new information into the history table
     *
     * Parameters:
     *  - tag: PC tag
     *  - addr: addr access
     */
    uint16_t set = tag & TABLE_SET_MASK;
    assert(set < sets);
    addr &= ADDR_MASK;
    Cycles cycle = Cycles(parent->curCycle() & TIME_MASK);
    DPRINTF(BertiRubyPrefetcher, "History Table Add: Tag %x Addr %x Cycle %d Set %d\n", tag, addr, cycle, set);
    // Save new element into the history table
    history_pointers[set]->tag = tag;
    history_pointers[set]->time = cycle;
    history_pointers[set]->addr = addr;

    if (history_pointers[set] == &historyt[set][ways - 1])
    {
        history_pointers[set] = &historyt[set][0]; // End the cycle
    }
    else
        history_pointers[set]++; // Pointer to the next (oldest) entry
}

uint16_t BertiRubyPrefetcher::HistoryTable::get(uint32_t latency, uint64_t tag, uint64_t act_addr, std::vector<uint64_t> &tags, std::vector<uint64_t> &addr, uint64_t cycle)
{
    /*
     * Return an array (by parameter) with all the possible PC that can launch
     * an on-time and late prefetch
     *
     * Parameters:
     *  - tag: PC tag
     *  - latency: latency of the processor
     *  - on_time_ip (out): ips that can launch an on-time prefetch
     *  - on_time_addr (out): addr that can launch an on-time prefetch
     *  - num_on_time (out): number of ips that can launch an on-time prefetch
     */

    act_addr &= ADDR_MASK;

    uint16_t num_on_time = get_aux(latency, tag, act_addr, tags, addr, cycle & ADDR_MASK);
    DPRINTF(BertiRubyPrefetcher, "History Table Get: Timely ips %d\n",num_on_time);
    // We found on-time prefetchs
    return num_on_time;
}

/******************************************************************************/
/*                       Shadow Cache functions                               */
/******************************************************************************/

bool BertiRubyPrefetcher::ShadowCache::get(uint64_t addr)
{
    /*
     * Parameters:
     *      - addr: cache block v_addr
     *
     * Return: true if the addr is in the l1d cache, false otherwise
     */
    auto entry = cache.findEntry({addr, false});
    if(entry != nullptr)
        DPRINTF(BertiRubyPrefetcher,"Shadow Cache get Addr: %x Hit\n", addr);
    else
        DPRINTF(BertiRubyPrefetcher,"Shadow Cache get Addr: %x Miss\n", addr);
    return entry != nullptr;
}

bool BertiRubyPrefetcher::ShadowCache::add(uint64_t addr, bool pf, uint64_t lat)
{
    // Create a new entry
    ShadowCacheEntry *new_entry = cache.findVictim({addr, false});
    new_entry->pf = pf;
    new_entry->lat = lat;

    // Let the cache handle insertion and replacement
    cache.insertEntry({addr, false}, new_entry);
    DPRINTF(BertiRubyPrefetcher, "Shadow Cache Add entry Addr: %x Latency: %d PF: %d\n", addr, lat, pf);
    return new_entry->pf;
}

void BertiRubyPrefetcher::ShadowCache::set_pf(uint64_t addr, bool pf)
{
    ShadowCacheEntry *entry = cache.findEntry({addr, false});
    if(entry != nullptr){
        DPRINTF(BertiRubyPrefetcher, "Shadow Cache setting pf %d Addr %x\n", pf, addr);
        entry->pf = pf;
    }
}

bool BertiRubyPrefetcher::ShadowCache::is_pf(uint64_t addr)
{
    ShadowCacheEntry *entry = cache.findEntry({addr, false});
    if(entry != nullptr){
        DPRINTF(BertiRubyPrefetcher, "Shadow Cache is pf %d Addr %x\n", entry->pf, addr);
        return entry->pf;
    }
    DPRINTF(BertiRubyPrefetcher, "Shadow Cache is pf: Addr %x not present\n", addr);
    return 0;
}

uint64_t BertiRubyPrefetcher::ShadowCache::get_latency(uint64_t addr)
{
    ShadowCacheEntry *entry = cache.findEntry({addr, false});
    assert(entry != nullptr && "Address must be in shadow cache");
    DPRINTF(BertiRubyPrefetcher, "Shadow Cache Get Latency: Addr %x Latency %x\n", addr, entry->lat);
    return entry->lat;
}

/******************************************************************************/
/*                       Berti functions                               */
/******************************************************************************/

bool BertiRubyPrefetcher::Berti::compare_greater_delta(delta_t a, delta_t b)
{
    // Sorted stride when the confidence is full
    if (a.rpl == L1 && b.rpl != L1)
        return 1;
    else if (a.rpl != L1 && b.rpl == L1)
        return 0;
    else
    {
        if (a.rpl == L2 && b.rpl != L2)
            return 1;
        else if (a.rpl != L2 && b.rpl == L2)
            return 0;
        else
        {
            if (a.rpl == L2 && b.rpl != L2)
                return 1;
            if (a.rpl != L2 && b.rpl == L2)
                return 0;
            else
            {
                if (std::abs(a.delta) < std::abs(b.delta))
                    return 1;
                return 0;
            }
        }
    }
}

bool BertiRubyPrefetcher::Berti::compare_greater_conf(delta_t a, delta_t b)
{
    if (a.conf > b.conf) return 1;
    else
    {
        if (std::abs(a.delta) < std::abs(b.delta)) return 1;
        return 0;
    }
}

bool BertiRubyPrefetcher::Berti::compare_rpl(delta_t a, delta_t b)
{
    if (a.rpl == R && b.rpl != R)
        return 1;
    else if (b.rpl == R && a.rpl != R)
        return 0;
    else if (a.rpl == L2 && b.rpl != L2)
        return 1;
    else if (b.rpl == L2 && a.rpl != L2)
        return 0;
    else
    {
        if (a.conf < b.conf)
            return 1;
        else
            return 0;
    }
}

void BertiRubyPrefetcher::Berti::increase_conf_tag(uint64_t tag)
{
    /*
     * Increase the global confidence of the deltas associated to the tag
     *
     * Parameters:
     *  tag : tag to find
     */
    if (bertit.find(tag) == bertit.end())
    {
        // Tag not found
        DPRINTF(BertiRubyPrefetcher, "Berti Tag %x Not Found\n", tag);
        return;
    }

    // Get the entries and the deltas

    bertit[tag]->conf += CONFIDENCE_INC;
    DPRINTF(BertiRubyPrefetcher, "Berti confidence increase: Tag %x Conf %d\n", tag, bertit[tag]->conf);

    if (bertit[tag]->conf == CONFIDENCE_MAX)
    {
        DPRINTF(BertiRubyPrefetcher, "Berti Max confidence achieved: Tag %x\n", tag);
        // Max confidence achieve
        for (auto &i : bertit[tag]->deltas)
        {
            // Set bits to prefetch level
            if (i.conf > CONFIDENCE_L1)
                i.rpl = L1;
            else if (i.conf > CONFIDENCE_L2)
                i.rpl = L2;
            else if (i.conf > CONFIDENCE_L2R)
                i.rpl = L2R;
            else
                i.rpl = R;

            i.conf = 0; // Reset confidence
        }

        bertit[tag]->conf = 0; // Reset global confidence
    }
}
// TODO: Recheck the logic
void BertiRubyPrefetcher::Berti::add(uint64_t tag, int64_t delta)
{
    /*
     * Save the new information into the history table
     *
     * Parameters:
     *  - tag: PC tag
     *  - cpu: actual cpu
     *  - stride: actual cpu
     */
    auto add_delta = [this](auto delta, auto entry)
    {
        // Lambda function to add a new element
        delta_t new_delta;
        new_delta.delta = delta;
        new_delta.conf = CONFIDENCE_INIT;
        new_delta.rpl = R;
        auto it = std::find_if(std::begin(entry->deltas), std::end(entry->deltas), [](const auto i)
                               { return (i.delta == 0); });
        assert(it != std::end(entry->deltas));
        *it = new_delta;
    };

    if (bertit.find(tag) == bertit.end())
    {
        DPRINTF(BertiRubyPrefetcher, "Berti Add: Encountered New Tag %x\n", tag);
        // We are not tracking this tag
        if (bertit_queue.size() > BERTI_TABLE_SIZE)
        {
            // FIFO replacent algorithm
            uint64_t key = bertit_queue.front();
            berti *entry = bertit[key];
            DPRINTF(BertiRubyPrefetcher, "Berti Add: Remove entry with Key %x\n", key);
            delete entry; // Free previous entry

            bertit.erase(bertit_queue.front());
            bertit_queue.pop();
        }

        bertit_queue.push(tag); // Add new tag
        assert((bertit.size() <= BERTI_TABLE_SIZE) && "Tracking too much tags");

        // Confidence IP
        berti *entry = new berti(BERTI_TABLE_STRIDE_SIZE);
        entry->conf = CONFIDENCE_INC;
        ;

        // Saving the new stride
        add_delta(delta, entry);

        // Save the new tag
        bertit.insert(std::make_pair(tag, entry));
        return;
    }
    // Get the delta
    berti *entry = bertit[tag];
    DPRINTF(BertiRubyPrefetcher, "Berti Add: Existing Tag %x\n", tag);
    for (auto &i : entry->deltas)
    {
        if (i.delta == delta)
        {
            // We already track the delta
            i.conf += CONFIDENCE_INC;

            if (i.conf > CONFIDENCE_MAX)
                i.conf = CONFIDENCE_MAX;
            DPRINTF(BertiRubyPrefetcher, "Berti Add: Increased confidence %d\n", i.conf);
            return;
        }
    }

    uint8_t dx_conf = 100;
    int dx_remove = -1;
    for (int i = 0; i < BERTI_TABLE_STRIDE_SIZE; i++)
    {
        if (entry->deltas[i].rpl == R && entry->deltas[i].conf < dx_conf)
        {
            dx_conf = entry->deltas[i].conf;
            dx_remove = i;
        }
    }

    if (dx_remove > -1)
    {
        DPRINTF(BertiRubyPrefetcher, "Berti Add: Overwriting delta entry at index %d Delta %d Conf 1 Repl R\n", dx_remove, delta);
        entry->deltas[dx_remove].delta = delta;
        entry->deltas[dx_remove].conf   = CONFIDENCE_INIT;
        entry->deltas[dx_remove].rpl    = R;
        return;
    } else
    {
        DPRINTF(BertiRubyPrefetcher, "Berti Add: Find Entry with L2R repl\n");
        for (int i = 0; i < BERTI_TABLE_STRIDE_SIZE; i++)
        {
            if (entry->deltas[i].rpl == L2R && entry->deltas[i].conf < dx_conf)
            {
                dx_conf = entry->deltas[i].conf;
                dx_remove = i;
            }
        }
        if (dx_remove > -1)
        {
            entry->deltas[dx_remove].delta = delta;
            entry->deltas[dx_remove].conf   = CONFIDENCE_INIT;
            entry->deltas[dx_remove].rpl    = R;
            DPRINTF(BertiRubyPrefetcher, "Berti Add: Overwriting delta entry at index %d Delta %d Conf 1 Repl R\n", dx_remove, delta);
            return;
        }
    }
}

void BertiRubyPrefetcher::Berti::find_and_update(uint64_t latency, uint64_t tag, uint64_t cycle, uint64_t line_addr, HistoryTable *historyt)
{
    // We were tracking this miss
    std::vector<uint64_t> tags(16);
    std::vector<uint64_t> addr(16);
    uint16_t num_on_time = 0;

    // Get the IPs that can launch a prefetch
    num_on_time = historyt->get(latency, tag, line_addr, tags, addr, cycle);
    DPRINTF(BertiRubyPrefetcher, "Berti Find and Update: Timely Deltas %d\n", num_on_time);
    for (uint32_t i = 0; i < num_on_time; i++)
    {
        // Increase conf tag
        if (i == 0)
            increase_conf_tag(tag);

        // Add information into berti table
        int64_t stride;
        line_addr &= ADDR_MASK;

        // Usually applications go from lower to higher memory position.
        // The operation order is important (mainly because we allow
        // negative strides)
        stride = (int64_t)(line_addr - addr[i]);

        if ((std::abs(stride) < (1 << STRIDE_MASK))){
            add(tags[i], stride);
        }
        else{
            DPRINTF(BertiRubyPrefetcher, "Berti Find and Update: Delta %d not added\n",stride);
        }
    }
}

uint8_t BertiRubyPrefetcher::Berti::get(uint64_t tag, std::vector<delta_t> &res)
{
    /*
     * Save the new information into the history table
     *
     * Parameters:
     *  - tag: PC tag
     *
     * Return: the stride to prefetch
     */
    if (!bertit.count(tag))
    {
        DPRINTF(BertiRubyPrefetcher, "Berti get: Tag %x Miss\n",tag );
        return 0;
    }

    // We found the tag
    berti *entry = bertit[tag];

    uint16_t dx = 0;
    
    for (int i = 0; i < BERTI_TABLE_STRIDE_SIZE; i++)
    {
        if (entry->deltas[i].delta != 0 && entry->deltas[i].rpl)
        {
            // Substitue min confidence for the next one
            res[dx].delta = entry->deltas[i].delta;
            res[dx].rpl = entry->deltas[i].rpl;
            dx++;
        }
    }

    if (dx == 0 && entry->conf >= LANZAR_INT)
    {
        DPRINTF(BertiRubyPrefetcher, "Berti get: No deltas Found\n");
        // We do not find any delta, so we will try  to launch with small confidence
        int dx = 0;
        for (auto &i : entry->deltas)
        {
            if (i.delta != 0)
            {
                res[dx].delta = i.delta;
                float temp = (float) i.conf / (float) entry->conf;
                uint64_t aux_conf   = (uint64_t) (temp * 100);
                res[dx].conf = aux_conf;
                dx++;
            }
        }
        std::sort(std::begin(res), std::end(res), compare_greater_conf);
        for (int i = 0; i < parent->degree; i++)
        {
            if (res[i].conf > 80) res[i].rpl = L1;
            else if (res[i].conf > 35) res[i].rpl = L2;
            //if (res[i].per > 80) res[i].rpl = L2;
            else res[i].rpl = R;
        }
        std::sort(std::begin(res), std::end(res), compare_greater_delta);
        return 1;
    }

    // Sort the entries
    std::sort(std::begin(res), std::end(res), compare_greater_delta);
    return 1;
}

uint64_t BertiRubyPrefetcher::Berti::ip_hash(uint64_t ip)
{
    /*
     * IP hash function
     */
#ifdef HASH_ORIGINAL
    ip = ((ip >> 1) ^ (ip >> 4)); // Original one
#endif
    // IP hash from here: http://burtleburtle.net/bob/hash/integer.html
#ifdef THOMAS_WANG_HASH_1
    ip = (ip ^ 61) ^ (ip >> 16);
    ip = ip + (ip << 3);
    ip = ip ^ (ip >> 4);
    ip = ip * 0x27d4eb2d;
    ip = ip ^ (ip >> 15);
#endif
#ifdef THOMAS_WANG_HASH_2
    ip = (ip + 0x7ed55d16) + (ip << 12);
    ip = (ip ^ 0xc761c23c) ^ (ip >> 19);
    ip = (ip + 0x165667b1) + (ip << 5);
    ip = (ip + 0xd3a2646c) ^ (ip << 9);
    ip = (ip + 0xfd7046c5) + (ip << 3);
    ip = (ip ^ 0xb55a4f09) ^ (ip >> 16);
#endif
#ifdef THOMAS_WANG_HASH_3
    ip -= (ip << 6);
    ip ^= (ip >> 17);
    ip -= (ip << 9);
    ip ^= (ip << 4);
    ip -= (ip << 3);
    ip ^= (ip << 10);
    ip ^= (ip >> 15);
#endif
#ifdef THOMAS_WANG_HASH_4
    ip += ~(ip << 15);
    ip ^= (ip >> 10);
    ip += (ip << 3);
    ip ^= (ip >> 6);
    ip += ~(ip << 11);
    ip ^= (ip >> 16);
#endif
#ifdef THOMAS_WANG_HASH_5
    ip = (ip + 0x479ab41d) + (ip << 8);
    ip = (ip ^ 0xe4aa10ce) ^ (ip >> 5);
    ip = (ip + 0x9942f0a6) - (ip << 14);
    ip = (ip ^ 0x5aedd67d) ^ (ip >> 3);
    ip = (ip + 0x17bea992) + (ip << 7);
#endif
#ifdef THOMAS_WANG_HASH_6
    ip = (ip ^ 0xdeadbeef) + (ip << 4);
    ip = ip ^ (ip >> 10);
    ip = ip + (ip << 7);
    ip = ip ^ (ip >> 13);
#endif
#ifdef THOMAS_WANG_HASH_7
    ip = ip ^ (ip >> 4);
    ip = (ip ^ 0xdeadbeef) + (ip << 5);
    ip = ip ^ (ip >> 11);
#endif
#ifdef THOMAS_WANG_NEW_HASH
    ip ^= (ip >> 20) ^ (ip >> 12);
    ip = ip ^ (ip >> 7) ^ (ip >> 4);
#endif
#ifdef THOMAS_WANG_HASH_HALF_AVALANCHE
    ip = (ip + 0x479ab41d) + (ip << 8);
    ip = (ip ^ 0xe4aa10ce) ^ (ip >> 5);
    ip = (ip + 0x9942f0a6) - (ip << 14);
    ip = (ip ^ 0x5aedd67d) ^ (ip >> 3);
    ip = (ip + 0x17bea992) + (ip << 7);
#endif
#ifdef THOMAS_WANG_HASH_FULL_AVALANCHE
    ip = (ip + 0x7ed55d16) + (ip << 12);
    ip = (ip ^ 0xc761c23c) ^ (ip >> 19);
    ip = (ip + 0x165667b1) + (ip << 5);
    ip = (ip + 0xd3a2646c) ^ (ip << 9);
    ip = (ip + 0xfd7046c5) + (ip << 3);
    ip = (ip ^ 0xb55a4f09) ^ (ip >> 16);
#endif
#ifdef THOMAS_WANG_HASH_INT_1
    ip -= (ip << 6);
    ip ^= (ip >> 17);
    ip -= (ip << 9);
    ip ^= (ip << 4);
    ip -= (ip << 3);
    ip ^= (ip << 10);
    ip ^= (ip >> 15);
#endif
#ifdef THOMAS_WANG_HASH_INT_2
    ip += ~(ip << 15);
    ip ^= (ip >> 10);
    ip += (ip << 3);
    ip ^= (ip >> 6);
    ip += ~(ip << 11);
    ip ^= (ip >> 16);
#endif
#ifdef ENTANGLING_HASH
    ip = ip ^ (ip >> 2) ^ (ip >> 5);
#endif
#ifdef FOLD_HASH
    uint64_t hash = 0;
    while (ip)
    {
        hash ^= (ip & ip_mask);
        ip >>= size_ip_mask;
    }
    ip = hash;
#endif
    return ip; // No IP hash
}


}
}