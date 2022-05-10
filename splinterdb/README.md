SplinterDB
==========

SplinterDB is a key-value store designed for high performance on fast storage devices.

Installation
============
To compile this repository, you need `gawk`, `libaio`, 'libconfig', and 'libjudy' dev headers installed.
You can do this with
> sudo apt update
> sudo apt install libaio-dev libxxhash-dev

Then, to compile:
> make

Benchmarks
==========

The benchmarks for SIGMOD 2023 are run using the run_lookup.py, run_space.py
and run_ycsb.py scripts in the scripts directory.
