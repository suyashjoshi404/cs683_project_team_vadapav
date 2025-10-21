# run_gem5_benchmark.sh - Concurrent Benchmark Execution

This script runs `run_gem5.sh` for all checkpoint directories within a specified benchmark, executing simulations concurrently for improved performance.

## Usage

```bash
./run_gem5_benchmark.sh <benchmark_name> <config> <max_parallel_jobs> [--dry-run]
./run_gem5_benchmark.sh ALL <config> <max_parallel_jobs> [--dry-run]
```

### Arguments

1. **benchmark_name**: Name of the benchmark directory from `/home/msd/NewGem5DatacenterTraces/` or "ALL" to run all benchmarks
2. **config**: Configuration to use (currently supported: LRU, EMISSARY)
3. **max_parallel_jobs**: Maximum number of concurrent simulations to run
4. **--dry-run** (optional): Show what would be executed without actually running simulations

### Examples

```bash
# Run finagle-chirper benchmark with LRU config using 4 parallel jobs
./run_gem5_benchmark.sh finagle-chirper LRU 4

# Run scala-kmeans benchmark with EMISSARY config using 2 parallel jobs  
./run_gem5_benchmark.sh scala-kmeans EMISSARY 2

# Run ALL benchmarks with LRU config using 8 parallel jobs
./run_gem5_benchmark.sh ALL LRU 8

# Run ALL benchmarks with EMISSARY config using 4 parallel jobs
./run_gem5_benchmark.sh ALL EMISSARY 4

# Dry run to see what would be executed without actually running
./run_gem5_benchmark.sh finagle-chirper LRU 4 --dry-run
./run_gem5_benchmark.sh ALL LRU 2 --dry-run
```

## Available Benchmarks

To see available benchmarks, run the script with an invalid benchmark name:

```bash
./run_gem5_benchmark.sh invalid LRU 1
```

Current available benchmarks:
- akka-uct
- als
- chi-square
- dec-tree
- dotty
- finagle-chirper
- finagle-http
- fj-kmeans
- future-genetics
- gauss-mix
- log-regression
- movie-lens
- naive-bayes
- page-rank
- reactors
- scala-doku
- scala-kmeans
- scala-stm-bench7

## Key Optimizations

### Concurrency Optimization (ALL mode)
The script implements an intelligent rolling concurrency strategy:

- **Problem**: Some benchmarks have fewer checkpoints than the maximum concurrent jobs, leading to underutilized CPU resources
- **Solution**: Build a global queue of all checkpoints from all benchmarks and process them optimally
- **Benefit**: Maintains maximum concurrency throughout execution, significantly reducing total runtime

**Example**: With 4 max concurrent jobs:
- **Old approach**: benchmark A (2 checkpoints) → only 2 jobs running → wait → benchmark B (6 checkpoints) → 4 jobs running
- **New approach**: Start 2 from A + 2 from B immediately → maintain 4 jobs continuously

This can improve overall execution time by 20-50% depending on the distribution of checkpoints across benchmarks.

## Configuration Map

The script uses a configuration map that can be easily extended:

```bash
CONFIG_MAP["LRU"]="LRU"
CONFIG_MAP["EMISSARY"]="EMISSARY"
# Add new configs here:
# CONFIG_MAP["NEW_CONFIG"]="NEW_CONFIG"
```

## How It Works

### Single Benchmark Mode
1. **Validation**: Checks if the benchmark directory exists and config is valid
2. **Discovery**: Finds all `cpt.*` directories within the benchmark directory
3. **Execution**: Calls `run_gem5.sh` for each checkpoint directory with parameters:
   - `$1` = checkpoint folder name (e.g., "cpt.12007952063750")
   - `$2` = benchmark name (e.g., "scala-kmeans")
   - `$3` = full checkpoint path
   - `$4` = config name (e.g., "LRU")
4. **Concurrency**: Maintains specified number of parallel jobs using job slot management
5. **Monitoring**: Provides real-time status updates and completion statistics

### All Benchmarks Mode (ALL)
1. **Discovery**: Automatically finds all valid benchmark directories in `/home/msd/NewGem5DatacenterTraces/`
2. **Global Queue**: Builds a unified queue of all checkpoints across all benchmarks
3. **Optimized Concurrency**: Maintains maximum concurrent jobs by rolling across benchmarks
   - If a benchmark has fewer checkpoints than max concurrency, immediately starts checkpoints from the next benchmark
   - Ensures all available CPU slots are utilized continuously
4. **Progress Tracking**: Shows per-checkpoint progress and overall statistics
5. **Comprehensive Reporting**: Provides detailed summary across all benchmarks with efficiency metrics

## Output

### Single Benchmark Mode
- Real-time job status with timestamps
- Completion statistics including success/failure rates
- Individual simulation outputs stored in `sim_outs/m5out.<benchmark_name>/<checkpoint_name>/`

### All Benchmarks Mode
- Global checkpoint queue processing with optimal resource utilization
- Real-time progress tracking across all benchmarks  
- Overall execution summary with total counts across all benchmarks
- Individual simulation outputs stored in `sim_outs/m5out.<benchmark_name>/<checkpoint_name>/`
- Total execution time tracking and concurrency efficiency metrics

## Signal Handling

The script handles SIGINT (Ctrl+C) and SIGTERM gracefully:
- Terminates all running jobs
- Provides cleanup before exit
- Prevents orphaned processes

## Adding New Configurations

To add new configurations, simply update the `CONFIG_MAP` in the script:

```bash
CONFIG_MAP["NEW_CONFIG"]="NEW_CONFIG"
```

And ensure the corresponding case is present in `run_gem5.sh`.
