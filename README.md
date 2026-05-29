# Market Data Ingestion Task — C++17

This project implements the standard and hard versions of the market data ingestion task.

## What it does

- Reads Databento XEUR.EOBI MBO JSON/NDJSON files line by line.
- Creates `MarketDataEvent` objects.
- Prints the first 10 and last 10 events.
- Prints summary statistics: total messages, invalid/skipped lines, first timestamp, last timestamp, wall-clock time, throughput.
- Supports a single daily file for the standard task.
- Supports a folder of daily `.mbo.json` files for the hard task.
- Implements two merging strategies:
  - Flat k-way merge with one priority queue.
  - Hierarchical binary-tree merge.
- Uses producer threads, one per input file, and a dispatcher consumer.

## Build

```bash
g++ -std=c++17 -O3 -pthread main.cpp -o market_data_app
```

or with CMake:

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

## Run standard task on one file

```bash
./market_data_app sample_data/xeur-eobi-20260310.mbo.json
```

## Run hard task on a folder

```bash
./market_data_app sample_data --mode both
```

For the real Databento folders, use for example:

```bash
./market_data_app /content/drive/MyDrive/XEUR-20260409-HJTR7RCAKT --mode both
./market_data_app /content/drive/MyDrive/XEUR-20260409-HTT6HHLT6R --mode both
```

Use `--mode flat` or `--mode hierarchy` to run only one benchmark.

## Notes

The full market data files are intentionally not included because they are large. The implementation is streaming-based and processes one line at a time. The included sample files are small synthetic NDJSON files for functional testing.
