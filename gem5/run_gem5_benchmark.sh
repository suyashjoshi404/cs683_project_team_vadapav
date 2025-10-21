#!/bin/bash

# Check if required arguments are provided
if [ $# -lt 3 ]; then
    echo "Usage: $0 <benchmark_name> <config> <max_parallel_jobs> [--dry-run]"
    echo "       $0 ALL <config> <max_parallel_jobs> [--dry-run]"
    echo "Example: $0 finagle-chirper LRU 4"
    echo "         $0 ALL LRU 4"
    echo "         $0 ALL LRU 4 --dry-run"
    echo "Available configs: LRU, EMISSARY"
    exit 1
fi

# Set up environment variables
export M5_PATH=$(pwd)/bin/m5
GEM5_HOME=$(pwd)
ALL_CKPT_DIR=/new_disk/NewGem5DatacenterTraces

BENCHMARK_NAME=$1
CONFIG=$2
MAX_JOBS=$3
DRY_RUN=false

# Check for dry-run flag
if [ "$4" = "--dry-run" ]; then
    DRY_RUN=true
fi

# Config mapping - add more configs here as needed
declare -A CONFIG_MAP
CONFIG_MAP["LRU"]="LRU"
CONFIG_MAP["EMISSARY"]="EMISSARY"
CONFIG_MAP["LRU_L1D-Stride"]="LRU_L1D-Stride"
CONFIG_MAP["LRU_L1D-DCPT"]="LRU_L1D-DCPT"
CONFIG_MAP["LRU_BOPPrefetcher"]="LRU_BOPPrefetcher"
CONFIG_MAP["LRU_SignaturePathPrefetcher"]="LRU_SignaturePathPrefetcher"
CONFIG_MAP["LRU_L1D-Berti"]="LRU_L1D-Berti"
CONFIG_MAP["LRU_L1D-IPCP"]="LRU_L1D-IPCP"

# CONFIG_MAP["LRU_L1D-Triage"]="LRU_L1D-Triage"


# Validate config
if [[ ! ${CONFIG_MAP[$CONFIG]} ]]; then
    echo "Error: Invalid config '$CONFIG'"
    echo "Available configs: ${!CONFIG_MAP[@]}"
    exit 1
fi

# Validate max_jobs is a positive integer
if ! [[ "$MAX_JOBS" =~ ^[0-9]+$ ]] || [ "$MAX_JOBS" -le 0 ]; then
    echo "Error: max_parallel_jobs must be a positive integer"
    exit 1
fi

# Check if benchmark directory exists
BENCHMARK_DIR="$ALL_CKPT_DIR/$BENCHMARK_NAME"
if [ "$BENCHMARK_NAME" != "ALL" ] && [ ! -d "$BENCHMARK_DIR" ]; then
    echo "Error: Benchmark directory '$BENCHMARK_DIR' does not exist"
    echo "Available benchmarks:"
    ls -1 "$ALL_CKPT_DIR" | grep -v "\.img$"
    echo "Or use 'ALL' to run all benchmarks"
    exit 1
fi

echo "Benchmark: $BENCHMARK_NAME"
echo "Config: $CONFIG"
if [ "$BENCHMARK_NAME" != "ALL" ]; then
    echo "Benchmark directory: $BENCHMARK_DIR"
fi
echo "Maximum parallel jobs: $MAX_JOBS"
if [ "$DRY_RUN" = true ]; then
    echo "Mode: DRY RUN (will not execute simulations)"
else
    echo "Mode: EXECUTION"
fi
echo "Starting parallel simulation runs..."
echo "=========================="

# Create arrays to store job information
declare -a job_pids=()
declare -a job_names=()
declare -a job_outdirs=()

# Counters for tracking
total_jobs=0
completed_jobs=0
successful_jobs=0
failed_jobs=0

# Function to run all benchmarks with optimal concurrency
run_all_benchmarks() {
    local all_benchmarks=()
    
    # Get list of all benchmark directories
    for dir in "$ALL_CKPT_DIR"/*; do
        if [ -d "$dir" ] && [[ $(basename "$dir") != *.* ]]; then
            benchmark_name=$(basename "$dir")
            # Skip directories that don't contain checkpoint directories
            if ls "$dir"/cpt.* 1> /dev/null 2>&1; then
                all_benchmarks+=("$benchmark_name")
            fi
        fi
    done
    
    echo "Found ${#all_benchmarks[@]} benchmarks to process:"
    printf "  %s\n" "${all_benchmarks[@]}"
    echo "=========================="
    
    if [ "$DRY_RUN" = true ]; then
        run_all_benchmarks_dry_run "${all_benchmarks[@]}"
        return
    fi
    
    # Build global queue of all checkpoints across all benchmarks
    declare -a global_checkpoint_queue=()
    declare -a global_benchmark_names=()
    local overall_total=0
    
    echo "Building global checkpoint queue..."
    for benchmark in "${all_benchmarks[@]}"; do
        local benchmark_dir="$ALL_CKPT_DIR/$benchmark"
        local checkpoint_count=0
        
        for cpt_dir in "$benchmark_dir"/cpt.*; do
            if [ -d "$cpt_dir" ]; then
                local cpt_name=$(basename "$cpt_dir")
                global_checkpoint_queue+=("$cpt_dir")
                global_benchmark_names+=("$benchmark")
                checkpoint_count=$((checkpoint_count + 1))
                overall_total=$((overall_total + 1))
            fi
        done
        
        echo "  $benchmark: $checkpoint_count checkpoints"
    done
    
    echo "Total checkpoints to process: $overall_total"
    echo "=========================="
    
    local overall_start_time=$(date +%s)
    local overall_successful=0
    local overall_failed=0
    local checkpoint_index=0
    
    # Process checkpoints with optimal concurrency
    while [ $checkpoint_index -lt ${#global_checkpoint_queue[@]} ]; do
        # Fill up available slots
        while [ ${#job_pids[@]} -lt $MAX_JOBS ] && [ $checkpoint_index -lt ${#global_checkpoint_queue[@]} ]; do
            local cpt_path="${global_checkpoint_queue[$checkpoint_index]}"
            local benchmark_name="${global_benchmark_names[$checkpoint_index]}"
            local cpt_name=$(basename "$cpt_path")
            
            # Wait for available slot (should be immediate in this loop)
            wait_for_slot
            
            # Start the job in background
            run_simulation "$cpt_name" "$cpt_path" "$benchmark_name" &
            job_pid=$!
            
            # Store job information
            job_pids+=($job_pid)
            job_names+=("$benchmark_name/$cpt_name")
            job_outdirs+=("sim_outs/m5out.$benchmark_name/$cpt_name")
            
            echo "[$(date '+%Y-%m-%d %H:%M:%S')] STARTED: $benchmark_name/$cpt_name (PID: $job_pid) [${#job_pids[@]}/$MAX_JOBS slots used] [$(($checkpoint_index + 1))/$overall_total]"
            
            checkpoint_index=$((checkpoint_index + 1))
        done
        
        # Wait for at least one job to complete before adding more
        if [ ${#job_pids[@]} -ge $MAX_JOBS ]; then
            wait_for_next_completion
        fi
    done
    
    # Wait for all remaining jobs to complete
    wait_for_all_jobs
    
    # Calculate final statistics
    overall_successful=$successful_jobs
    overall_failed=$failed_jobs
    
    local overall_end_time=$(date +%s)
    local overall_duration=$((overall_end_time - overall_start_time))
    
    # Final summary
    echo "=========================="
    echo "OVERALL EXECUTION SUMMARY:"
    echo "Benchmarks processed: ${#all_benchmarks[@]}"
    echo "Total jobs: $overall_total"
    echo "Successful jobs: $overall_successful"
    echo "Failed jobs: $overall_failed"
    echo "Overall completion rate: $(( overall_successful * 100 / overall_total ))%"
    echo "Total execution time: ${overall_duration}s"
    echo "Average jobs running concurrently: $(echo "scale=1; $overall_total * 1.0 / ($overall_duration / $MAX_JOBS)" | bc -l 2>/dev/null || echo "N/A")"
    echo "=========================="
    
    # Exit with appropriate code
    if [ $overall_failed -eq 0 ]; then
        echo "All simulations completed successfully!"
        exit 0
    else
        echo "Some simulations failed. Check the individual output logs for details."
        exit 1
    fi
}

# Function for dry run of all benchmarks
run_all_benchmarks_dry_run() {
    local benchmarks=("$@")
    local total_checkpoints=0
    
    for benchmark in "${benchmarks[@]}"; do
        echo ""
        echo "### BENCHMARK: $benchmark ###"
        local benchmark_dir="$ALL_CKPT_DIR/$benchmark"
        local checkpoint_count=0
        
        echo "DRY RUN: Would process the following checkpoints:"
        for cpt_dir in "$benchmark_dir"/cpt.*; do
            if [ -d "$cpt_dir" ]; then
                local cpt_name=$(basename "$cpt_dir")
                echo "  - $benchmark/$cpt_name"
                checkpoint_count=$((checkpoint_count + 1))
            fi
        done
        echo "Checkpoint count: $checkpoint_count"
        total_checkpoints=$((total_checkpoints + checkpoint_count))
    done
    
    echo ""
    echo "=========================="
    echo "DRY RUN SUMMARY:"
    echo "Total benchmarks: ${#benchmarks[@]}"
    echo "Total checkpoints: $total_checkpoints"
    echo "Max concurrent jobs: $MAX_JOBS"
    echo "Estimated execution batches: $(( (total_checkpoints + MAX_JOBS - 1) / MAX_JOBS ))"
    echo "=========================="
}

# Function to wait for the next job completion (used in rolling strategy)
wait_for_next_completion() {
    while [ ${#job_pids[@]} -ge $MAX_JOBS ]; do
        # Check for completed jobs
        for i in "${!job_pids[@]}"; do
            local pid=${job_pids[$i]}
            if ! kill -0 $pid 2>/dev/null; then
                # Job has completed
                wait $pid
                local exit_code=$?
                local job_name=${job_names[$i]}
                
                completed_jobs=$((completed_jobs + 1))
                
                if [ $exit_code -eq 0 ]; then
                    successful_jobs=$((successful_jobs + 1))
                    echo "[$(date '+%Y-%m-%d %H:%M:%S')] COMPLETED: $job_name (Success) [$completed_jobs jobs completed]"
                else
                    failed_jobs=$((failed_jobs + 1))
                    echo "[$(date '+%Y-%m-%d %H:%M:%S')] COMPLETED: $job_name (Failed) [$completed_jobs jobs completed]"
                fi
                
                # Remove job from arrays
                unset job_pids[$i]
                unset job_names[$i]
                unset job_outdirs[$i]
                
                # Reindex arrays
                job_pids=("${job_pids[@]}")
                job_names=("${job_names[@]}")
                job_outdirs=("${job_outdirs[@]}")
                
                return 0  # Exit after removing one job
            fi
        done
        
        # Small delay to prevent busy waiting
        sleep 0.1
    done
}

# Function to process a single benchmark
process_single_benchmark() {
    local benchmark_name=$1
    local benchmark_dir="$ALL_CKPT_DIR/$benchmark_name"
    
    # Count total number of checkpoint directories
    for cpt_dir in "$benchmark_dir"/cpt.*; do
        if [ -d "$cpt_dir" ]; then
            total_jobs=$((total_jobs + 1))
        fi
    done
    
    echo "Found $total_jobs checkpoint directories in $benchmark_name"
    
    if [ "$DRY_RUN" = true ]; then
        echo "DRY RUN: Would process the following checkpoints:"
        for cpt_dir in "$benchmark_dir"/cpt.*; do
            if [ -d "$cpt_dir" ]; then
                cpt_name=$(basename "$cpt_dir")
                echo "  - $benchmark_name/$cpt_name"
            fi
        done
        return
    fi
    
    # Process all checkpoint directories
    for cpt_dir in "$benchmark_dir"/cpt.*; do
        # Check if it's a directory
        if [ -d "$cpt_dir" ]; then
            # Get the checkpoint directory name without the path
            cpt_name=$(basename "$cpt_dir")
            
            # Wait for available slot
            wait_for_slot
            
            # Start the job in background
            run_simulation "$cpt_name" "$cpt_dir" "$benchmark_name" &
            job_pid=$!
            
            # Store job information
            job_pids+=($job_pid)
            job_names+=("$benchmark_name/$cpt_name")
            job_outdirs+=("sim_outs/m5out.$benchmark_name/$cpt_name")
            
            echo "[$(date '+%Y-%m-%d %H:%M:%S')] STARTED: $benchmark_name/$cpt_name (PID: $job_pid) [${#job_pids[@]}/$MAX_JOBS slots used]"
        fi
    done
    
    # Wait for all jobs to complete
    wait_for_all_jobs
}

# Function to run a single simulation using run_gem5.sh
run_simulation() {
    local cpt_name=$1
    local cpt_path=$2
    local benchmark_name=$3
    
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting: $benchmark_name/$cpt_name (PID: $$)"
    
    # Call run_gem5.sh with appropriate arguments
    # $1 = checkpoint folder name, $2 = benchmark name, $3 = checkpoint path, $4 = config
    if bash run_gem5.sh "$cpt_name" "$benchmark_name" "$cpt_path" "$CONFIG"; then
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] ###--->$benchmark_name/$cpt_name executed successfully <---##### (PID: $$)"
        exit 0
    else
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] ***$benchmark_name/$cpt_name failed*** (PID: $$)"
        exit 1
    fi
}

# Function to wait for a job slot to become available
wait_for_slot() {
    while [ ${#job_pids[@]} -ge $MAX_JOBS ]; do
        # Check for completed jobs
        for i in "${!job_pids[@]}"; do
            local pid=${job_pids[$i]}
            if ! kill -0 $pid 2>/dev/null; then
                # Job has completed
                wait $pid
                local exit_code=$?
                local job_name=${job_names[$i]}
                
                completed_jobs=$((completed_jobs + 1))
                
                if [ $exit_code -eq 0 ]; then
                    successful_jobs=$((successful_jobs + 1))
                    echo "[$(date '+%Y-%m-%d %H:%M:%S')] COMPLETED: $job_name (Success) [$completed_jobs/$total_jobs]"
                else
                    failed_jobs=$((failed_jobs + 1))
                    echo "[$(date '+%Y-%m-%d %H:%M:%S')] COMPLETED: $job_name (Failed) [$completed_jobs/$total_jobs]"
                fi
                
                # Remove job from arrays
                unset job_pids[$i]
                unset job_names[$i]
                unset job_outdirs[$i]
                
                # Reindex arrays
                job_pids=("${job_pids[@]}")
                job_names=("${job_names[@]}")
                job_outdirs=("${job_outdirs[@]}")
                
                break
            fi
        done
        
        # Small delay to prevent busy waiting
        sleep 0.1
    done
}

# Function to wait for all remaining jobs to complete
wait_for_all_jobs() {
    echo "Waiting for all remaining jobs to complete..."
    
    while [ ${#job_pids[@]} -gt 0 ]; do
        for i in "${!job_pids[@]}"; do
            local pid=${job_pids[$i]}
            if ! kill -0 $pid 2>/dev/null; then
                # Job has completed
                wait $pid
                local exit_code=$?
                local job_name=${job_names[$i]}
                
                completed_jobs=$((completed_jobs + 1))
                
                if [ $exit_code -eq 0 ]; then
                    successful_jobs=$((successful_jobs + 1))
                    echo "[$(date '+%Y-%m-%d %H:%M:%S')] COMPLETED: $job_name (Success) [$completed_jobs/$total_jobs]"
                else
                    failed_jobs=$((failed_jobs + 1))
                    echo "[$(date '+%Y-%m-%d %H:%M:%S')] COMPLETED: $job_name (Failed) [$completed_jobs/$total_jobs]"
                fi
                
                # Remove job from arrays
                unset job_pids[$i]
                unset job_names[$i]
                unset job_outdirs[$i]
                
                # Reindex arrays
                job_pids=("${job_pids[@]}")
                job_names=("${job_names[@]}")
                job_outdirs=("${job_outdirs[@]}")
                
                break
            fi
        done
        
        sleep 0.1
    done
}

# Signal handler for cleanup
cleanup() {
    echo "Received interrupt signal. Cleaning up..."
    
    # Kill all running jobs
    for pid in "${job_pids[@]}"; do
        if kill -0 $pid 2>/dev/null; then
            echo "Terminating job with PID: $pid"
            kill -TERM $pid
        fi
    done
    
    # Wait a bit for graceful termination
    sleep 2
    
    # Force kill if necessary
    for pid in "${job_pids[@]}"; do
        if kill -0 $pid 2>/dev/null; then
            echo "Force killing job with PID: $pid"
            kill -KILL $pid
        fi
    done
    
    exit 130
}

# Set up signal handlers
trap cleanup SIGINT SIGTERM

# Main execution logic
if [ "$BENCHMARK_NAME" = "ALL" ]; then
    run_all_benchmarks
else
    # Process single benchmark
    process_single_benchmark "$BENCHMARK_NAME"
    
    # Summary
    echo "=========================="
    echo "EXECUTION SUMMARY:"
    echo "Benchmark: $BENCHMARK_NAME"
    echo "Config: $CONFIG"
    echo "Total jobs: $total_jobs"
    echo "Successful jobs: $successful_jobs"
    echo "Failed jobs: $failed_jobs"
    echo "Completion rate: $(( successful_jobs * 100 / total_jobs ))%"
    echo "=========================="
    
    # Exit with appropriate code
    if [ $failed_jobs -eq 0 ]; then
        echo "All simulations completed successfully!"
        exit 0
    else
        echo "Some simulations failed. Check the individual output logs for details."
        exit 1
    fi
fi
