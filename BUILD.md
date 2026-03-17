# Building Ocean Eterna (v8.0 / v9.0)

## Prerequisites

```bash
sudo apt install build-essential cmake libzstd-dev liblz4-dev libcurl4-openssl-dev libomp-dev
```

| Package | Used by | Notes |
|---------|---------|-------|
| `libzstd-dev` | bulk_build, server | corpus compression |
| `liblz4-dev` | server | LZ4-frame decompression |
| `libcurl4-openssl-dev` | server | LLM API calls |
| `libomp-dev` | both | parallel indexing/search (optional but strongly recommended) |

Minimum CMake version: **3.16**.  Minimum C++ standard: **C++17**.

## Build

```bash
cd ~/OE_7.77_8.0
mkdir build && cd build
cmake ..
make -j$(nproc)
```

Binaries land in `build/v8.0/` and `build/v9.0/`:

```
build/v8.0/bulk_build_v8.0
build/v8.0/ocean_chat_server_v8.0
build/v9.0/bulk_build_v9.0
build/v9.0/ocean_chat_server_v9.0
```

### Debug build

```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
```

### Build a single target

```bash
make bulk_build_v9.0          # just the v9.0 builder
make ocean_chat_server_v9.0   # just the v9.0 server
```

## Common Issues

**CMake can't find zstd / lz4 / curl**
Install the missing `-dev` package (see Prerequisites).  If you installed to
a non-standard prefix, pass `-DCMAKE_PREFIX_PATH=/your/prefix` to cmake.

**OpenMP warning**
The build works without OpenMP, but indexing and search will be single-threaded
and much slower.  Install `libomp-dev` (or `libomp-devel` on Fedora/RHEL).

**"march=native" fails on cross-compile**
Override with: `cmake -DCMAKE_CXX_FLAGS_RELEASE="-O3" ..`

**Old CMake version**
Ubuntu 20.04 ships CMake 3.16 (our minimum). On older distros, grab a newer
CMake from https://cmake.org/download/ or via `pip install cmake`.
