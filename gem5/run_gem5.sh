export M5_PATH=$(pwd)/bin/m5
GEM5_HOME=$(pwd)
GEM5_CFG=$GEM5_HOME/configs/example/arm/suyash_fs_simulation.py
ALL_CKPT_DIR=/new_disk/NewGem5DatacenterTraces

CONFIG=$4

#CKPT_DIR=$ALL_CKPT_DIR/$1
DISK_DIR=$ALL_CKPT_DIR/finagle-chirper
CKPT_DIR=$3
echo $DISK_DIR

OUTDIR=sim_outs/m5out.$2/$1
mkdir -p $OUTDIR
touch ${OUTDIR}

echo $OUTDIR

run_script() {
  # script="$GEM5_HOME/build/ARM/gem5.opt  --outdir=${OUTDIR} --debug-file=trace.out.gz  $GEM5_CFG --m1 -W 1000000 -I 2000000 --disk-image="${CKPT_DIR}/ubuntu-image.img" --ftqSize=24 --bootloader="${M5_PATH}/binaries/boot_v2_qemu_virt.arm64" --caches "$@" --cpu-type O3CPU --fdip --bp-type TAGE --restore "${CKPT_DIR}" --num-cores 1 -n 1 --mem-size 16384MiB > ${OUTDIR}/out.txt 2>&1"
  # 5M+100M
  script="$GEM5_HOME/build/ARM/gem5.opt  \
  --outdir=${OUTDIR} \
  $GEM5_CFG  \
  --disk-image="${DISK_DIR}/ubuntu-image.img"  \
  --cpu o3 \
  --restore $CKPT_DIR \
  --warmup-insts 5000000 \
  --maxinsts 100000000 \
  --bp-type LTAGE \
  --indirect-bp-type SimpleIndirectPredictor \
  --l1i-hwp-type FetchDirectedPrefetcher \
  "$@" \
  > ${OUTDIR}/out.txt 2>&1"
  
  # 5M+100M
  # script="$GEM5_HOME/build/ARM/gem5.opt  --outdir=${OUTDIR} --debug-file=trace.out.gz  $GEM5_CFG --m1 -W 5000000 -I 100000000 --disk-image="${CKPT_DIR}/ubuntu-image.img" --ftqSize=24 --bootloader="${M5_PATH}/binaries/boot_v2_qemu_virt.arm64" --caches "$@" --cpu-type O3CPU --fdip --bp-type TAGE --restore "${CKPT_DIR}" --num-cores 1 -n 1 --mem-size 16384MiB > ${OUTDIR}/out.txt 2>&1"

   if eval "$script"; then
       echo "###--->$CKPT_DIR executed succesfully <---#####"
      #  rm -rf ${OUTDIR}/debug.insts
      #  rm -rf ${OUTDIR}/trace.out.gz
   else 
       echo "***$CKPT_DIR not executed***"
   fi
}

case "$CONFIG" in
  "LRU")
    echo "CONFIG: LRU"
    run_script ""
    ;;
  "Triage")
    echo "CONFIG: Triage"
    run_script --triage
    ;;
  "Triangel")
    echo "CONFIG: Triage"
    run_script --triangel
    ;;
  "LRU_L1D-Stride")
    echo "CONFIG: LRU_L1D-Stride"
    run_script --l1d-hwp-type=StridePrefetcher
    ;;
  "LRU_L1D-DCPT")
    echo "CONFIG: LRU_L1D-Stride"
    run_script --l1d-hwp-type=DCPTPrefetcher
    ;;
  "LRU_L1D-BOPPrefetcher")
    echo "CONFIG: LRU_BOPPrefetcher"
    run_script --l1d-hwp-type=BOPPrefetcher
    ;;
  "LRU_L1D-SignaturePathPrefetcher")
    echo "CONFIG: LRU_SignaturePathPrefetcher"
    run_script --l1d-hwp-type=SignaturePathPrefetcher
    ;;
  "LRU_L1D-Berti")
    echo "CONFIG: LRU_L1D-Berti"
    run_script --l1d-hwp-type=BertiPrefetcher
    ;;
  "LRU_L1D-IPCP")
    echo "CONFIG: LRU_L1D-IPCP"
    run_script --l1d-hwp-type=IPCPPrefetcher
    ;;
  
  "LRU_L2-Stride")
    echo "CONFIG: LRU_L2-Stride"
    run_script --l2-hwp-type=StridePrefetcher
    ;;
  "LRU_PDIP_L1D-Stride")
    echo "CONFIG: LRU_PDIP_L1D-Stride"
    run_script --l1i-hwp-type=PDIP --l1d-hwp-type=StridePrefetcher
    ;;
  "LRU_PDIP_L1D-Stride_L2-Stride")
    echo "CONFIG: LRU_PDIP_L1D-Stride_L2-Stride"
    run_script --l1i-hwp-type=PDIP --l1d-hwp-type=StridePrefetcher --l2-hwp-type=StridePrefetcher
    ;;
  "LRU_PDIP_L1D-Stride_L2-BOP")
    echo "CONFIG: LRU_PDIP_L1D-Stride_L2-BOP"
    run_script --l1i-hwp-type=PDIP --l1d-hwp-type=StridePrefetcher --l2-hwp-type=BOPPrefetcher
    ;;
  "LRU_PDIP_L1D-Stride_L2-SPP")
    echo "CONFIG: LRU_PDIP_L1D-Stride_L2-SPP"
    run_script --l1i-hwp-type=PDIP --l1d-hwp-type=StridePrefetcher --l2-hwp-type=SignaturePathPrefetcherV2
    ;;
  "EMISSARY")
    echo "CONFIG: EMISSARY"
    run_script  --starve-randomness="3.125" --preserve-ways=8 --l2-rp-type=LRUEmissaryRP
    ;;
  "EMISSARY_PDIP")
    echo "CONFIG: EMISSARY_PDIP"
    run_script  --starve-randomness="3.125" --preserve-ways=8 --l2-rp-type=LRUEmissaryRP --l1i-hwp-type=PDIP
    ;;
  "EMISSARY_L1D-Stride")
    echo "CONFIG: EMISSARY_L1D-Stride"
    run_script  --starve-randomness="3.125" --preserve-ways=8 --l2-rp-type=LRUEmissaryRP --l1d-hwp-type=StridePrefetcher
    ;;
  "EMISSARY_L2-Stride")
    echo "CONFIG: EMISSARY_L2-Stride"
    run_script  --starve-randomness="3.125" --preserve-ways=8 --l2-rp-type=LRUEmissaryRP --l2-hwp-type=StridePrefetcher
    ;;
  "EMISSARY_PDIP_L1D-Stride")
    echo "CONFIG: EMISSARY_PDIP_L1D-Stride"
    run_script  --starve-randomness="3.125" --preserve-ways=8 --l2-rp-type=LRUEmissaryRP --l1i-hwp-type=PDIP --l1d-hwp-type=StridePrefetcher
    ;;
  "EMISSARY_PDIP_L1D-Stride_L2-Stride")
    echo "CONFIG: EMISSARY_PDIP_L1D-Stride_L2-Stride"
    run_script  --starve-randomness="3.125" --preserve-ways=8 --l2-rp-type=LRUEmissaryRP --l1i-hwp-type=PDIP --l1d-hwp-type=StridePrefetcher --l2-hwp-type=StridePrefetcher
    ;;
  "EMISSARY_PDIP_L1D-Stride_L2-BOP")
    echo "CONFIG: EMISSARY_PDIP_L1D-Stride_L2-BOP"
    run_script  --starve-randomness="3.125" --preserve-ways=8 --l2-rp-type=LRUEmissaryRP --l1i-hwp-type=PDIP --l1d-hwp-type=StridePrefetcher --l2-hwp-type=BOPPrefetcher
    ;;
  "EMISSARY_PDIP_L1D-Stride_L2-SPP")
    echo "CONFIG: EMISSARY_PDIP_L1D-Stride_L2-SPP"
    run_script  --starve-randomness="3.125" --preserve-ways=8 --l2-rp-type=LRUEmissaryRP --l1i-hwp-type=PDIP --l1d-hwp-type=StridePrefetcher --l2-hwp-type=SignaturePathPrefetcherV2
    ;;    
  # "CLIP")
  #   echo "CONFIG: CLIP"
  #   run_script --l2-rp-type=BRRIP
  #   ;;
  # "CLIP_PDIP")
  #   echo "CONFIG: CLIP_PDIP"
  #   run_script --l1i-hwp-type=PDIP --l2-rp-type=BRRIP
  #   ;;
  # "CLIP_L1D-Stride")
  #   echo "CONFIG: CLIP_L1D-Stride"
  #   run_script --l1d-hwp-type=StridePrefetcher --l2-rp-type=BRRIP
  #   ;;
  # "CLIP_L2-Stride")
  #   echo "CONFIG: CLIP_L2-Stride"
  #   run_script --l2-hwp-type=StridePrefetcher --l2-rp-type=BRRIP
  #   ;;
  # "CLIP_PDIP_L1D-Stride")
  #   echo "CONFIG: CLIP_PDIP_L1D-Stride"
  #   run_script --l1i-hwp-type=PDIP --l1d-hwp-type=StridePrefetcher --l2-rp-type=BRRIP
  #   ;;
  # "CLIP_PDIP_L1D-Stride_L2-Stride")
  #   echo "CONFIG: CLIP_PDIP_L1D-Stride_L2-Stride"
  #   run_script --l1i-hwp-type=PDIP --l1d-hwp-type=StridePrefetcher --l2-hwp-type=StridePrefetcher --l2-rp-type=BRRIP
  #   ;;
  # "CLIP_PDIP_L1D-Stride_L2-BOP")
  #   echo "CONFIG: CLIP_PDIP_L1D-Stride_L2-BOP"
  #   run_script --l1i-hwp-type=PDIP --l1d-hwp-type=StridePrefetcher --l2-hwp-type=BOPPrefetcher --l2-rp-type=BRRIP
  #   ;;
  # "CLIP_PDIP_L1D-Stride_L2-SPP")
  #   echo "CONFIG: CLIP_PDIP_L1D-Stride_L2-SPP"
  #   run_script --l1i-hwp-type=PDIP --l1d-hwp-type=StridePrefetcher --l2-hwp-type=SignaturePathPrefetcherV2 --l2-rp-type=BRRIP
  #   ;;
  
  # "ICARUS")
  #   echo "CONFIG: ICARUS"
  #   run_script --l2-rp-type=CRIPR
  #   ;;
  # "ICARUS_PDIP")
  #   echo "CONFIG: ICARUS_PDIP"
  #   run_script --l1i-hwp-type=PDIP --l2-rp-type=CRIPR
  #   ;;
  # "ICARUS_L1D-Stride")
  #   echo "CONFIG: ICARUS_L1D-Stride"
  #   run_script --l1d-hwp-type=StridePrefetcher --l2-rp-type=CRIPR
  #   ;;
  # "ICARUS_L2-Stride")
  #   echo "CONFIG: ICARUS_L2-Stride"
  #   run_script --l2-hwp-type=StridePrefetcher --l2-rp-type=CRIPR
  #   ;;
  # "ICARUS_PDIP_L1D-Stride")
  #   echo "CONFIG: ICARUS_PDIP_L1D-Stride"
  #   run_script --l1i-hwp-type=PDIP --l1d-hwp-type=StridePrefetcher --l2-rp-type=CRIPR
  #   ;;
  # "ICARUS_PDIP_L1D-Stride_L2-Stride")
  #   echo "CONFIG: ICARUS_PDIP_L1D-Stride_L2-Stride"
  #   run_script --l1i-hwp-type=PDIP --l1d-hwp-type=StridePrefetcher --l2-hwp-type=StridePrefetcher --l2-rp-type=CRIPR
  #   ;;
  # "ICARUS_PDIP_L1D-Stride_L2-BOP")
  #   echo "CONFIG: ICARUS_PDIP_L1D-Stride_L2-BOP"
  #   run_script --l1i-hwp-type=PDIP --l1d-hwp-type=StridePrefetcher --l2-hwp-type=BOPPrefetcher --l2-rp-type=CRIPR
  #   ;;
  # "ICARUS_PDIP_L1D-Stride_L2-SPP")
  #   echo "CONFIG: ICARUS_PDIP_L1D-Stride_L2-SPP"
  #   run_script --l1i-hwp-type=PDIP --l1d-hwp-type=StridePrefetcher --l2-hwp-type=SignaturePathPrefetcherV2 --l2-rp-type=CRIPR
  #   ;;
  *) # Catch-all for any other configurations
    echo "Invalid CONFIG! Please use start, stop, status, or restart."
    ;;
esac
