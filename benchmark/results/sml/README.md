# SML (Super Mario Land Rev 1) — QEMU bench, 2026-05-17

Same harness as `../README.md` ; the only difference is the embedded ROM
(`port/esp32s3/main/sml.gb`, 64 KB MBC1).

Reproduce:

    source $IDF_PATH/export.sh
    BENCH_ROM=sml ./benchmark/run_bench.sh

Outputs land in this directory (this is the per-ROM subfolder created by
`run_bench.sh` whenever `$BENCH_ROM != blargg_06`).
