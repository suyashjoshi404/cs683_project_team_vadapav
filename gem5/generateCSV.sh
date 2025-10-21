#!/bin/bash

# Initialize variables
input_dir=""
prefetchers=()

# List of benchmark names for replacement
benchmark_list=(
    # "kafka"
    # "tailbench_specjbb"
    # "finagle-http"
    # "finagle-chirper"
    # "media_stream" 
    # "web-search"
    # "tomcat"
    # "wikipedia"
    # "cassandra"
    # "verilator"
    # "tpcc"
    # "tailbench_xapian"
    # "speedometer2.0"
    "akka-uct"
    "als"
    "chi-square"
    "dec-tree"
    "dotty"
    "finagle-chirper"
    "finagle-http"
    "fj-kmeans"
    "future-genetics"
    "gauss-mix"
    "log-regression"
    "movie-lens"
    "naive-bayes"
    "page-rank"
    "reactors"
    "scala-doku"
    "scala-kmeans"
    "scala-stm-bench7"
)

# Function to show usage information
show_usage() {
    echo "Usage: $0 <directory_path>"
    echo "Options:"
    echo "  -prefetcher   Specify which prefetchers to include in the output (l1i, l1d, l2)"
    echo "Examples:"
    echo "  $0 /path/to/directory                     # Include all prefetchers"
    exit 1
}

# Parse command line arguments
if [ $# -lt 1 ]; then
    show_usage
fi

# Get the directory path
input_dir="${1%/}"
shift


prefetchers=("l1i" "l1d" "l2")

# Validate the provided directory
if [ ! -d "$input_dir" ]; then
    echo "Error: $input_dir is not a valid directory"
    exit 1
fi

# Extract the final folder name for the CSV
output_name=$(basename "$input_dir" | awk -F. '{print $NF}')
mkdir -p "stats/data_dump/${output_name}/"
output_folder="stats/data_dump/${output_name}"
prefetcher_suffix=$(IFS=_; echo "${prefetchers[*]}")
output_file_csv="${output_name}_metrics.csv"
output_file_MPKI="${output_name}_MPKI.csv"
output_file_prefetcher="${output_name}_prefetcher.csv"
output_csv="${input_dir}/${output_file_csv}"
output_csv_MPKI="${input_dir}/${output_file_MPKI}"
output_csv_prefetcher="${input_dir}/${output_file_prefetcher}"

tickConversion=250 # Default tick conversion factor

# Function to safely extract a numeric metric from the stats file
extract_metric() {
    local file="$1"
    local metric="$2"
    local fallback_value="0"
    
    # Extract the value, handling lines with comments
    local value=$(grep -m 1 "$metric" "$file" | awk -F'#' '{print $1}' | awk '{print $NF}' | tr -d '()' | xargs)
    
    # If value is empty or not a number, use fallback
    if [[ -z "$value" ]] || ! [[ "$value" =~ ^-?[0-9]+(\.[0-9]+)?$ ]]; then
        echo "$fallback_value"
        return
    fi
    
    echo "$value"
}

# Function to calculate MPKI (Misses Per Thousand Instructions)
calculate_mpki() {
    local misses="$1"
    local instructions="$2"
    
    # Check if instructions is 0 to avoid division by zero
    if [ "$instructions" -eq 0 ] || [ -z "$instructions" ]; then
        echo "0"
        return
    fi
    
    # Calculate MPKI: (Misses / Total Instructions) * 1000
    echo "scale=5; ($misses * 1000) / $instructions" | bc
}

calculate_missrate() {
    local misses="$1"
    local accesses="$2"
    
    # Check if instructions is 0 to avoid division by zero
    if [ "$accesses" -eq 0 ] || [ -z "$accesses" ]; then
        echo "0"
        return
    fi
    
    # Calculate missrate: (Misses / accesses) * 1000
    echo "scale=5; ($misses * 100) / $accesses" | bc
}

# Function to map benchmark name
map_benchmark_name() {
    local benchmark="$1"
    
    # Check if any item in the list is a substring of the benchmark name
    for list_item in "${benchmark_list[@]}"; do
        if [[ "$benchmark" == *"$list_item"* ]]; then
            echo "$list_item"
            return
        fi
    done
    
    # Return original benchmark name if no match found
    echo "$benchmark"
}

# Build CSV header based on selected prefetchers
header_arr=(
    "Benchmark" "Checkpoint" "IPC" "Committed Instructions"
    "L1D Accesses" "L1D Hits" "L1D Misses" "L1D MissLatency" "L1D MissRate" "L1D MPKI"
    "L1I Accesses" "L1I Hits" "L1I Misses" "L1I Prefetcher Misses" "L1I MissLatency" "L1I MissRate" "L1I MPKI" "L1I with pf MPKI"
    "L2 Accesses" "L2 Instr Accesses" "L2: L1I prefetcher Accesses" "L2 Data Accesses" "L2 Hits" "L2 Instr Hits" "L2: L1I prefetcher hits" "L2 Data Hits" "L2 Instr Misses" "L2: L1I Prefetcher Misses" "L2 Data Misses" "L2 Instr MissLatency" "L2 Data MissLatency" "L2 MissRate" "L2 MPKI" "L2 Instr MPKI" "L2 Instr with L1I Pf MPKI" "L2 Data MPKI"
    "L3 Accesses" "L3 Instr Accesses" "L3: L1I prefetcher Accesses" "L3 Data Accesses" "L3 Hits" "L3 Instr Hits" "L3: L1I prefetcher hits" "L3 Data Hits" "L3 Instr Misses" "L3: L1I Prefetcher Misses" "L3 Data Misses" "L3 Instr MissLatency" "L3 Data MissLatency" "L3 MissRate" "L3 MPKI" "L3 Instr MPKI" "L3 Instr with L1I Pf MPKI" "L3 Data MPKI"
    "ITLB Accesses" "ITLB Hits" "ITLB Misses" "ITLB MissRate" "ITLB MPKI"
    "DTLB Accesses" "DTLB Hits" "DTLB Misses" "DTLB MissRate" "DTLB MPKI"
    "Total Cycles" "Decode Idle" "Decode Starvation due to L1 Hit" "Decode Starvation due to L1 Miss" "Decode Starvation due to L2 Miss" "Decode Starvation due to L3 Miss"
    "ROB Stalls due to L1 Hit" "ROB Stalls due to L1 Miss" "ROB Stalls due to L2 Miss" "ROB Stalls due to L3 Miss"
)
header="$(IFS=,; echo "${header_arr[*]}")"

header_MPKI="Benchmark,Checkpoint,L1D Accesses,L1D Hits,L1D Misses,L1D MissLatency,L1D MissRate,L1D MPKI,L1I Accesses,L1I Hits,L1I Misses,L1I Prefetcher Misses,L1I MissLatency,L1I MissRate,L1I MPKI,L1I with pf MPKI,L2 Accesses,L2 Instr Accesses,L2: L1I prefetcher Accesses,L2 Data Accesses,L2 Hits,L2 Instr Hits,L2: L1I prefetcher hits,L2 Data Hits,L2 Instr Misses,L2: L1I Prefetcher Misses,L2 Data Misses,L2 Instr MissLatency,L2 Data MissLatency,L2 MissRate,L2 MPKI,L2 Instr MPKI,L2 Instr with L1I Pf MPKI,L2 Data MPKI,L3 Accesses,L3 Instr Accesses,L3: L1I prefetcher Accesses,L3 Data Accesses,L3 Hits,L3 Instr Hits,L3: L1I prefetcher hits,L3 Data Hits,L3 Instr Misses,L3: L1I Prefetcher Misses,L3 Data Misses,L3 Instr MissLatency,L3 Data MissLatency,L3 MissRate,L3 MPKI,L3 Instr MPKI,L3 Instr with L1I Pf MPKI,L3 Data MPKI,ITLB Accesses,ITLB Hits,ITLB Misses,ITLB MissRate,ITLB MPKI,DTLB Accesses,DTLB Hits,DTLB Misses,DTLB MissRate,DTLB MPKI"

header_prefetcher="Benchmark,Checkpoint,L1I Pf Accuracy,L1I Pf Coverage,L1I Pf Issued,L1D Pf Late,L1I Pf Useful,L1I Pf Unused,L1I Pf Hit in Cache,L1I Pf Hit in MSHR,L1I Pf Hit in Writeback,L1D Pf Accuracy,L1D Pf Coverage,L1D Pf Issued,L1D Pf Late,L1D Pf Useful,L1D Pf Unused,L1D Pf Hit in Cache,L1D Pf Hit in MSHR,L1D Pf Hit in Writeback,L2 Pf Accuracy,L2 Pf Coverage,L2 Pf Issued,L2 Pf Late,L2 Pf Useful,L2 Pf Unused,L2 Pf Hit in Cache,L2 Pf Hit in MSHR,L2 Pf Hit in Writeback"

# Add prefetcher columns based on selection
for pf in "${prefetchers[@]}"; do
    case "$pf" in
        l1i)
            header="$header,L1I Prefetcher Accuracy,L1I Prefetcher Coverage,L1I Prefetcher PfIssued,L1I Prefetcher Late"
            ;;
        l1d)
            header="$header,L1D Prefetcher Accuracy,L1D Prefetcher Coverage,L1D Prefetcher PfIssued,L1D Prefetcher Late"
            ;;
        l2)
            header="$header,L2 Prefetcher Accuracy,L2 Prefetcher Coverage,L2 Prefetcher PfIssued,L2 Prefetcher Late"
            ;;
    esac
done

# Write CSV header
echo "$header" > "$output_csv"
echo "$header_MPKI" > "$output_csv_MPKI"
echo "$header_prefetcher" > "$output_csv_prefetcher"

# Process each subfolder in the input directory
find "$input_dir" -type d | while read -r subfolder; do
    # Extract benchmark name from the folder path
    # benchmark=$(basename "$subfolder" | sed -n 's/.*ckpt\.\(.*\)/\1/p')
    # benchmark=$(basename "$subfolder")
    
    parent_dir=$(basename "$(dirname "$subfolder")")
    benchmark="${parent_dir#m5_out.}"
    checkpoint=$(basename "$subfolder")
    
    # Skip if no benchmark name could be extracted
    # [ -z "$benchmark" ] && continue

    # Map benchmark name to list item if it's a substring
    benchmark=$(map_benchmark_name "$benchmark")
    
    # Find stats_final.txt in this subfolder
    stats_file=$(find "$subfolder" -maxdepth 1 -name "stats_final.txt")
    
    # Skip if no stats file found
    [ -z "$stats_file" ] && continue
    
    # Total instructions
    # total_instructions=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.statIssuedInstType_0::total")
    total_instructions=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.commitStats0.numInsts")
    [ -z "$total_instructions" ] && total_instructions=0
    
    # Debug: print stats file path and total instructions
    echo "Processing: $stats_file"
    echo "Benchmark: $benchmark"
    echo "Total Instructions: $total_instructions"
    
    # IPC Metric
    ipc=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.ipc")

    # L1D Metrics
    l1d_accesses=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.dcache.demandAccesses::total")
    l1d_hits=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.dcache.demandHits::total")
    l1d_misses=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.dcache.demandMisses::total")
    l1d_misslatency=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.dcache.demandAvgMissLatency::total")
    l1d_misslatency=$(echo "scale=5; $l1d_misslatency / $tickConversion" | bc) # Convert to cycles
    l1d_missrate=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.dcache.demandMissRate::total")
    l1d_mpki=$(calculate_mpki "$l1d_misses" "$total_instructions")
    
    # L1I Metrics
    l1i_accesses=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.icache.demandAccesses::total")
    l1i_pfIssued=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.icache.prefetcher.pfIssued")
    l1i_hits=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.icache.demandHits::total")
    l1i_pf_hits=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.icache.prefetcher.pfHitInCache")
    l1i_misses=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.icache.demandMisses::total")
    l1i_pf_misses=$(echo "$l1i_pfIssued - $l1i_pf_hits" | bc)
    l1i_misslatency=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.icache.demandAvgMissLatency::total")
    l1i_misslatency=$(echo "scale=5; $l1i_misslatency / $tickConversion" | bc) # Convert to cycles
    l1i_missrate=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.icache.demandMissRate::total")
    l1i_mpki=$(calculate_mpki "$l1i_misses" "$total_instructions")
    l1i_withpf_mpki=$(calculate_mpki "$l1i_misses + $l1i_pf_misses" "$total_instructions")
    
    # L2 Metrics
    l2_accesses=$(extract_metric "$stats_file" "system.cpu_cluster.l2.demandAccesses::total")
    l2_instr_accesses=$(extract_metric "$stats_file" "system.cpu_cluster.l2.demandAccesses::cpu_cluster.cpus.inst")
    l2_instr_pf_accesses=$(extract_metric "$stats_file" "system.cpu_cluster.l2.demandAccesses::cpu_cluster.cpus.icache.prefetchers")
    l2_data_accesses=$(extract_metric "$stats_file" "system.cpu_cluster.l2.demandAccesses::cpu_cluster.cpus.data")
    l2_hits=$(extract_metric "$stats_file" "system.cpu_cluster.l2.demandHits::total")
    l2_instr_hits=$(extract_metric "$stats_file" "system.cpu_cluster.l2.demandHits::cpu_cluster.cpus.inst")
    l2_instr_pf_hits=$(extract_metric "$stats_file" "system.cpu_cluster.l2.demandHits::cpu_cluster.cpus.icache.prefetcher")
    l2_data_hits=$(extract_metric "$stats_file" "system.cpu_cluster.l2.demandHits::cpu_cluster.cpus.data")
    l2_misses=$(extract_metric "$stats_file" "system.cpu_cluster.l2.demandMisses::total")
    l2_instr_misses=$(extract_metric "$stats_file" "system.cpu_cluster.l2.demandMisses::cpu_cluster.cpus.inst")
    l2_instr_pf_misses=$(extract_metric "$stats_file" "system.cpu_cluster.l2.demandMisses::cpu_cluster.cpus.icache.prefetcher")
    l2_data_misses=$(extract_metric "$stats_file" "system.cpu_cluster.l2.demandMisses::cpu_cluster.cpus.data")
    l2_instr_misslatency=$(extract_metric "$stats_file" "system.cpu_cluster.l2.demandAvgMissLatency::cpu_cluster.cpus.inst")
    l2_instr_misslatency=$(echo "scale=5; $l2_instr_misslatency / $tickConversion" | bc) # Convert to cycles
    l2_data_misslatency=$(extract_metric "$stats_file" "system.cpu_cluster.l2.demandAvgMissLatency::cpu_cluster.cpus.data")
    l2_data_misslatency=$(echo "scale=5; $l2_data_misslatency / $tickConversion" | bc) # Convert to cycles
    l2_missrate=$(extract_metric "$stats_file" "system.cpu_cluster.l2.demandMissRate::total")
    l2_mpki=$(calculate_mpki "$l2_misses" "$total_instructions")
    l2_instr_mpki=$(calculate_mpki "$l2_instr_misses" "$total_instructions")
    l2_instr_withpf_mpki=$(calculate_mpki "$l2_instr_misses + $l2_instr_pf_misses" "$total_instructions")
    l2_data_mpki=$(calculate_mpki "$l2_data_misses" "$total_instructions")

    
    # L3 Metrics
    l3_accesses=$(extract_metric "$stats_file" "system.l3.demandAccesses::total")
    l3_instr_accesses=$(extract_metric "$stats_file" "system.l3.demandAccesses::cpu_cluster.cpus.inst")
    l3_instr_pf_accesses=$(extract_metric "$stats_file" "system.l3.demandAccesses::cpu_cluster.cpus.icache.prefetcher")
    l3_data_accesses=$(extract_metric "$stats_file" "system.l3.demandAccesses::cpu_cluster.cpus.data")
    l3_hits=$(extract_metric "$stats_file" "system.l3.demandHits::total")
    l3_instr_hits=$(extract_metric "$stats_file" "system.l3.demandHits::cpu_cluster.cpus.inst")
    l3_instr_pf_hits=$(extract_metric "$stats_file" "system.l3.demandHits::cpu_cluster.cpus.icache.prefetcher")
    l3_data_hits=$(extract_metric "$stats_file" "system.l3.demandHits::cpu_cluster.cpus.data")
    l3_misses=$(extract_metric "$stats_file" "system.l3.demandMisses::total")
    l3_instr_misses=$(extract_metric "$stats_file" "system.l3.demandMisses::cpu_cluster.cpus.inst")
    l3_instr_pf_misses=$(extract_metric "$stats_file" "system.l3.demandMisses::cpu_cluster.cpus.icache.prefetcher")
    l3_data_misses=$(extract_metric "$stats_file" "system.l3.demandMisses::cpu_cluster.cpus.data")
    l3_instr_misslatency=$(extract_metric "$stats_file" "system.l3.demandAvgMissLatency::cpu_cluster.cpus.inst")
    l3_instr_misslatency=$(echo "scale=5; $l3_instr_misslatency / $tickConversion" | bc) # Convert to cycles
    l3_data_misslatency=$(extract_metric "$stats_file" "system.l3.demandAvgMissLatency::cpu_cluster.cpus.data")
    l3_data_misslatency=$(echo "scale=5; $l3_data_misslatency / $tickConversion" | bc) # Convert to cycles
    l3_missrate=$(extract_metric "$stats_file" "system.l3.demandMissRate::total")
    l3_instr_mpki=$(calculate_mpki "$l3_instr_misses" "$total_instructions")
    l3_instr_withpf_mpki=$(calculate_mpki "$l3_instr_misses + $l3_instr_pf_misses" "$total_instructions")
    l3_data_mpki=$(calculate_mpki "$l3_data_misses" "$total_instructions")
    l3_mpki=$(calculate_mpki "$l3_misses" "$total_instructions")

    # ITLB Metrics
    itlb_accesses=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.mmu.itb.accesses")
    itlb_hits=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.mmu.itb.hits")
    itlb_misses=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.mmu.itb.misses")
    itlb_missrate=$(calculate_missrate "$itlb_misses" "$itlb_accesses")
    itlb_mpki=$(calculate_mpki "$itlb_misses" "$total_instructions")

    # DTLB Metrics
    dtlb_accesses=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.mmu.dtb.accesses")
    dtlb_hits=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.mmu.dtb.hits")
    dtlb_misses=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.mmu.dtb.misses")
    dtlb_missrate=$(calculate_missrate "$dtlb_misses" "$dtlb_accesses")
    dtlb_mpki=$(calculate_mpki "$dtlb_misses" "$total_instructions")
    
    # STLB Metrics
    stlb_accesses=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.mmu.l2_shared.accesses")
    stlb_hits=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.mmu.l2_shared.hits")
    stlb_misses=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.mmu.l2_shared.misses")
    stlb_missrate=$(calculate_missrate "$stlb_misses" "$stlb_accesses")
    stlb_mpki=$(calculate_mpki "$stlb_misses" "$total_instructions")
    
    # Total cycles
    total_cycles=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.numCycles")
    #Stalls metrics
    decode_idle=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.decode.idleCycles")
    # decode_starvation_l1_hit=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.decodeStarvationDueToL1Hit::total")
    # decode_starvation_l1_miss=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.commit.decodeIdleNonSpecPathL2Hit ")
    # decode_starvation_l2_miss=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.commit.decodeIdleNonSpecPathL2Miss")
    # decode_starvation_l3_miss=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.decodeStarvationDueToL3Miss::total")
    # rob_stalls_l1_hit=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.commit.Load_commPathMemStallCostL1Hit")
    # rob_stalls_l1_miss=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.commit.Load_commPathMemStallCostL1Miss")
    # rob_stalls_l2_miss=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.commit.Load_commPathMemStallCostL2Miss")
    # rob_stalls_l3_miss=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.commit.Load_commPathMemStallCostL3Miss")
    
    # Base row without prefetcher metrics
    row="$benchmark,$checkpoint,$ipc,$total_instructions,$l1d_accesses,$l1d_hits,$l1d_misses,$l1d_misslatency,$l1d_missrate,$l1d_mpki,$l1i_accesses,$l1i_hits,$l1i_misses,$l1i_pf_misses,$l1i_misslatency,$l1i_missrate,$l1i_mpki,$l1i_withpf_mpki,$l2_accesses,$l2_instr_accesses,$l2_instr_pf_accesses,$l2_data_accesses,$l2_hits,$l2_instr_hits,$l2_instr_pf_hits,$l2_data_hits,$l2_instr_misses,$l2_instr_pf_misses,$l2_data_misses,$l2_instr_misslatency,$l2_data_misslatency,$l2_missrate,$l2_mpki,$l2_instr_mpki,$l2_instr_withpf_mpki,$l2_data_mpki,$l3_accesses,$l3_instr_accesses,$l3_instr_pf_accesses,$l3_data_accesses,$l3_hits,$l3_instr_hits,$l3_instr_pf_hits,$l3_data_hits,$l3_instr_misses,$l3_instr_pf_misses,$l3_data_misses,$l3_instr_misslatency,$l3_data_misslatency,$l3_missrate,$l3_mpki,$l3_instr_mpki,$l3_instr_withpf_mpki,$l3_data_mpki,$itlb_accesses,$itlb_hits,$itlb_misses,$itlb_missrate,$itlb_mpki,$dtlb_accesses,$dtlb_hits,$dtlb_misses,$dtlb_missrate,$dtlb_mpki,$total_cycles,$decode_idle,$decode_starvation_l1_hit,$decode_starvation_l1_miss,$decode_starvation_l2_miss,$decode_starvation_l3_miss,$rob_stalls_l1_hit,$rob_stalls_l1_miss,$rob_stalls_l2_miss,$rob_stalls_l3_miss"

    row_MPKI="$benchmark,$checkpoint,$l1d_accesses,$l1d_hits,$l1d_misses,$l1d_misslatency,$l1d_missrate,$l1d_mpki,$l1i_accesses,$l1i_hits,$l1i_misses,$l1i_pf_misses,$l1i_misslatency,$l1i_missrate,$l1i_mpki,$l1i_withpf_mpki,$l2_accesses,$l2_instr_accesses,$l2_instr_pf_accesses,$l2_data_accesses,$l2_hits,$l2_instr_hits,$l2_instr_pf_hits,$l2_data_hits,$l2_instr_misses,$l2_instr_pf_misses,$l2_data_misses,$l2_instr_misslatency,$l2_data_misslatency,$l2_missrate,$l2_mpki,$l2_instr_mpki,$l2_instr_withpf_mpki,$l2_data_mpki,$l3_accesses,$l3_instr_accesses,$l3_instr_pf_accesses,$l3_data_accesses,$l3_hits,$l3_instr_hits,$l3_instr_pf_hits,$l3_data_hits,$l3_instr_misses,$l3_instr_pf_misses,$l3_data_misses,$l3_instr_misslatency,$l3_data_misslatency,$l3_missrate,$l3_mpki,$l3_instr_mpki,$l3_instr_withpf_mpki,$l3_data_mpki,$itlb_accesses,$itlb_hits,$itlb_misses,$itlb_missrate,$itlb_mpki,$dtlb_accesses,$dtlb_hits,$dtlb_misses,$dtlb_missrate,$dtlb_mpki"

    # Add prefetcher metrics based on selection
    for pf in "${prefetchers[@]}"; do
        case "$pf" in
            l1i)
                pf_l1i_accuracy=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.icache.prefetcher.accuracy")
                pf_l1i_coverage=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.icache.prefetcher.coverage")
                pf_l1i_issued=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.icache.prefetcher.pfIssued")
                pf_l1i_useful=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.icache.prefetcher.pfUseful")
                pf_l1i_unused=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.icache.prefetcher.pfUnused")
                pf_l1i_hitCache=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.icache.prefetcher.pfHitInCache")
                pf_l1i_hitMSHR=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.icache.prefetcher.pfHitInMSHR")
                pf_l1i_hitWB=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.icache.prefetcher.pfHitInWB")
                pf_l1i_late=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.icache.prefetcher.pfLate")
                row="$row,$pf_l1i_accuracy,$pf_l1i_coverage,$pf_l1i_issued,$pf_l1i_late,$pf_l1i_useful,$pf_l1i_unused,$pf_l1i_hitCache,$pf_l1i_hitMSHR,$pf_l1i_hitWB"
                ;;
            l1d)
                pf_l1d_accuracy=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.dcache.prefetcher.accuracy")
                pf_l1d_coverage=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.dcache.prefetcher.coverage")
                pf_l1d_issued=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.dcache.prefetcher.pfIssued")
                pf_l1d_useful=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.dcache.prefetcher.pfUseful")
                pf_l1d_unused=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.dcache.prefetcher.pfUnused")
                pf_l1d_hitCache=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.dcache.prefetcher.pfHitInCache")
                pf_l1d_hitMSHR=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.dcache.prefetcher.pfHitInMSHR")
                pf_l1d_hitWB=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.dcache.prefetcher.pfHitInWB")
                pf_l1d_late=$(extract_metric "$stats_file" "system.cpu_cluster.cpus.dcache.prefetcher.pfLate")
                row="$row,$pf_l1d_accuracy,$pf_l1d_coverage,$pf_l1d_issued,$pf_l1d_late,$pf_l1d_useful,$pf_l1d_unused,$pf_l1d_hitCache,$pf_l1d_hitMSHR,$pf_l1d_hitWB"
                ;;
            l2)
                pf_l2_accuracy=$(extract_metric "$stats_file" "system.cpu_cluster.l2.prefetcher.accuracy")
                pf_l2_coverage=$(extract_metric "$stats_file" "system.cpu_cluster.l2.prefetcher.coverage")
                pf_l2_issued=$(extract_metric "$stats_file" "system.cpu_cluster.l2.prefetcher.pfIssued")
                pf_l2_useful=$(extract_metric "$stats_file" "system.cpu_cluster.l2.prefetcher.pfUseful")
                pf_l2_unused=$(extract_metric "$stats_file" "system.cpu_cluster.l2.prefetcher.pfUnused")
                pf_l2_hitCache=$(extract_metric "$stats_file" "system.cpu_cluster.l2.prefetcher.pfHitInCache")
                pf_l2_hitMSHR=$(extract_metric "$stats_file" "system.cpu_cluster.l2.prefetcher.pfHitInMSHR")
                pf_l2_hitWB=$(extract_metric "$stats_file" "system.cpu_cluster.l2.prefetcher.pfHitInWB")
                pf_l2_late=$(extract_metric "$stats_file" "system.cpu_cluster.l2.prefetcher.pfLate")
                row="$row,$pf_l2_accuracy,$pf_l2_coverage,$pf_l2_issued,$pf_l2_late,$pf_l2_useful,$pf_l2_unused,$pf_l2_hitCache,$pf_l2_hitMSHR,$pf_l2_hitWB"                ;;
        esac
    done


    row_prefetcher="$benchmark,$checkpoint,$pf_l1i_accuracy,$pf_l1i_coverage,$pf_l1i_issued,$pf_l1i_late,$pf_l1i_useful,$pf_l1i_unused,$pf_l1i_hitCache,$pf_l1i_hitMSHR,$pf_l1i_hitWB,$pf_l1d_accuracy,$pf_l1d_coverage,$pf_l1d_issued,$pf_l1d_late,$pf_l1d_useful,$pf_l1d_unused,$pf_l1d_hitCache,$pf_l1d_hitMSHR,$pf_l1d_hitWB,$pf_l2_accuracy,$pf_l2_coverage,$pf_l2_issued,$pf_l2_late,$pf_l2_useful,$pf_l2_unused,$pf_l2_hitCache,$pf_l2_hitMSHR,$pf_l2_hitWB"

    
    
    # Write metrics to cumulative CSV
    echo "$row" >> "$output_csv"
    echo "$row_MPKI" >> "$output_csv_MPKI"
    echo "$row_prefetcher" >> "$output_csv_prefetcher"
    
    echo "Extracted metrics for $benchmark to $output_csv", "$output_csv_MPKI", "$output_csv_prefetcher"
done

echo "$output_csv  $output_csv_MPKI $output_csv_prefetcher $output_folder" 

cp "$output_csv" "$output_folder"
cp "$output_csv_MPKI" "$output_folder"
cp "$output_csv_prefetcher" "$output_folder"
echo "Cumulative metrics extraction complete. Output file: $output_csv"
echo "Included prefetchers: ${prefetchers[*]}"