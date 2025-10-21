#!/bin/bash

# Check if required arguments are provided
if [ $# -lt 2 ]; then
    echo "Usage: $0 <checkpoint_directory> <max_parallel_jobs>"
    echo "Example: $0 checkpoints 4"
    exit 1
fi

# Set up environment variables
export M5_PATH=$(pwd)/bin/m5
GEM5_HOME=$(pwd)
GEM5_CFG=$GEM5_HOME/configs/example/arm/arbiter_fs_simulation.py
DISK_DIR="../stock-gem5-fdp/gem5-fdp/imgs/"
CKPT_DIR=$1
MAX_JOBS=$2

# Validate max_jobs is a positive integer
if ! [[ "$MAX_JOBS" =~ ^[0-9]+$ ]] || [ "$MAX_JOBS" -le 0 ]; then
    echo "Error: max_parallel_jobs must be a positive integer"
    exit 1
fi

# Check if checkpoint directory exists
if [ ! -d "$CKPT_DIR" ]; then
    echo "Error: Checkpoint directory '$CKPT_DIR' does not exist"
    exit 1
fi

echo "Disk directory: $DISK_DIR"
echo "Checkpoint directory: $CKPT_DIR"
echo "Maximum parallel jobs: $MAX_JOBS"
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

# Function to run a single simulation
run_simulation() {
    local dir_name=$1
    local outdir=$2
    
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting: $subdir_name (PID: $$)"
    
    # Construct and execute the command
    local script="$GEM5_HOME/build/ARM/gem5.opt \
  --outdir=${outdir} \
  $GEM5_CFG \
  --disk-image=\"${DISK_DIR}/ubuntu-image.img\" \
  --cpu o3 \
  --restore $dir_name \
  --warmup-insts 5000000 \
  --maxinsts 100000000 \
  --bp-type LTAGE \
  --indirect-bp-type SimpleIndirectPredictor \
  --l1i-hwp-type FetchDirectedPrefetcher \
  > ${outdir}/out.txt 2>&1"
#   --disable-decoupled-frontend \
    
    if eval "$script"; then
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] ###--->$subdir_name executed successfully <---##### (PID: $$)"
        exit 0
    else
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] ***$subdir_name failed*** (PID: $$)"
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

# Count total number of subdirectories
for subdir in "$CKPT_DIR"/*; do
    if [ -d "$subdir" ]; then
        total_jobs=$((total_jobs + 1))
    fi
done

echo "Found $total_jobs subdirectories to process"
echo "=========================="

# Process all subdirectories
for subdir in "$CKPT_DIR"/*; do
    # Check if it's a directory
    if [ -d "$subdir" ]; then
        # Get the subdirectory name without the path
        subdir_name=$(basename "$subdir")
        
        # Create output directory
        OUTDIR="sim_outs/refine_checkpoints/m5out.$(basename "$CKPT_DIR")/$subdir_name"
        mkdir -p "$OUTDIR"
        
        # Wait for available slot
        wait_for_slot
        
        # Start the job in background
        run_simulation "$subdir" "$OUTDIR" &
        job_pid=$!
        
        # Store job information
        job_pids+=($job_pid)
        job_names+=("$subdir_name")
        job_outdirs+=("$OUTDIR")
        
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] STARTED: $subdir_name (PID: $job_pid) [${#job_pids[@]}/$MAX_JOBS slots used]"
    fi
done

# Wait for all jobs to complete
wait_for_all_jobs

# Summary
echo "=========================="
echo "EXECUTION SUMMARY:"
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