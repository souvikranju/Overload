# overload — Fake System Overload Generator

A lightweight C console application that generates configurable CPU and/or RAM
overload for testing purposes (stress testing, monitoring validation, scheduler
tuning, etc.).

Supports **Linux x86_64**, **Linux ARM64**, **macOS x86_64**, **macOS ARM64**,
**Windows x86_64**, and **Windows ARM64** — all compiled from a single source file.

---

## Requirements

| Item | Minimum |
|------|---------|
| OS (build host) | Linux x86_64 |
| Compiler | GCC 4.8+ (C99 support) |
| Build tool | `make` (optional — manual compile also works) |

---

## Build

### Using make (recommended)

```bash
# Linux x86_64 (default)
make

# Linux ARM64
make linux-arm64

# Windows x64
make windows-x64

# Windows ARM64  (requires llvm-mingw — see below)
make windows-arm64

# macOS x86_64 (Intel)
make macos-x64

# macOS ARM64 (Apple Silicon)
make macos-arm64

# Build all targets at once
make all-targets

# Remove all binaries
make clean
```

Output binaries:

| Target | Binary name |
|---|---|
| Linux x86_64 | `overload-linux-x64` |
| Linux ARM64 | `overload-linux-arm64` |
| macOS x86_64 | `overload-macos-x64` |
| macOS ARM64 | `overload-macos-arm64` |
| Windows x64 | `overload-windows-x64.exe` |
| Windows ARM64 | `overload-windows-arm64.exe` |

---

## Cross-compiler Setup

### Linux x86_64 (native — no extra packages needed)
Uses the system `gcc`.

### macOS x86_64 / ARM64 (native — no extra packages needed)
Uses the system `clang` (included with Xcode Command Line Tools).

### Linux ARM64
```bash
sudo apt install gcc-aarch64-linux-gnu
```

### Windows x64
```bash
sudo apt install mingw-w64
```

### Windows ARM64
The standard `mingw-w64` apt package does **not** include an ARM64 Windows
compiler. Use the pre-built **llvm-mingw** toolchain:

1. Download the latest release for Linux x86_64 from:  
   <https://github.com/mstorsjo/llvm-mingw/releases>  
   (file: `llvm-mingw-<date>-ucrt-ubuntu-22.04-x86_64.tar.xz`)

2. Extract and add its `bin/` directory to your `PATH`:
   ```bash
   tar -xf llvm-mingw-*.tar.xz -C /opt/
   export PATH="/opt/llvm-mingw-<date>-ucrt-ubuntu-22.04-x86_64/bin:$PATH"
   ```

3. Build:
   ```bash
   make windows-arm64
   ```

---

## Manual Compile Commands

```bash
# Linux x86_64
gcc -Wall -Wextra -pedantic -std=c99 -pthread -O2 \
    -o overload-linux-x64 overload.c

# Linux ARM64
aarch64-linux-gnu-gcc -Wall -Wextra -pedantic -std=c99 -pthread -O2 \
    -o overload-linux-arm64 overload.c

# macOS x86_64 or ARM64 (native)
clang -Wall -Wextra -pedantic -std=c99 -pthread -O2 \
    -o overload-macos-x64 overload.c   # or overload-macos-arm64

# Windows x64 (cross-compile from Linux)
x86_64-w64-mingw32-gcc -Wall -Wextra -pedantic -std=c99 -O2 \
    -o overload-windows-x64.exe overload.c

# Windows ARM64 (cross-compile from Linux, requires llvm-mingw)
aarch64-w64-mingw32-gcc -Wall -Wextra -pedantic -std=c99 -O2 \
    -o overload-windows-arm64.exe overload.c
```

---

## Command-Line Switches

| Switch | Argument | Default | Description |
|--------|----------|---------|-------------|
| `--cpu` | *(none)* | off | Enable CPU overload — spawns one busy-loop thread per core |
| `--ram` | `[MB]` *(optional)* | off | Enable RAM overload — allocate and commit the specified MB. If `MB` is omitted, ~90% of currently available system RAM is used |
| `--time` | `<secs>` | `5` (with flags) / `10` (no args) | How long to run the overload in seconds |
| `--cores` | `<num>` | all logical cores | Override the number of CPU threads spawned (only relevant with `--cpu`) |
| `--help` / `-h` | *(none)* | — | Print usage information and exit |

### Notes on individual switches

**`--cpu`**  
Spawns N threads (POSIX pthreads on Linux, Win32 threads on Windows).
Default N = all logical cores. Each thread runs a `volatile`-protected
prime-number calculation loop, sustaining ~100% utilisation per core for the
requested duration.

**`--ram [MB]`**  
Allocates a contiguous block via `malloc`, then writes one byte per page to
force physical memory commitment (prevents lazy/copy-on-write mapping). The
touch loop is timer-aware: it checks the clock every 1,000 pages and stops
early if the requested duration has already elapsed. The report shows the
amount of RAM **actually committed**, which may be less than requested if the
system cannot touch all pages within the time limit. After the duration expires
the memory is freed before exit.

**`--time <secs>`**  
Controls the total wall-clock duration of the overload. When no arguments are
given at all, the default is **10 seconds**. When at least one flag is
provided but `--time` is omitted, the default is **5 seconds**.

**`--cores <num>`**  
Limits CPU thread count regardless of how many logical cores the system has.
Has no effect when `--cpu` is not specified.

---

## Usage Examples

```bash
# No arguments — run both CPU and RAM overload for 10 seconds (all cores, auto RAM)
./overload-linux-x64

# CPU overload only, 5 seconds, all cores
./overload-linux-x64 --cpu --time 5

# CPU overload only, 5 seconds, limited to 4 cores
./overload-linux-x64 --cpu --time 5 --cores 4

# RAM overload only, allocate 2048 MB, hold for 10 seconds
./overload-linux-x64 --ram 2048 --time 10

# RAM overload only, auto-detect available RAM (~90%), hold for 10 seconds
./overload-linux-x64 --ram --time 10

# Combined CPU + RAM overload, 15 seconds, 4 cores, 1024 MB
./overload-linux-x64 --cpu --ram 1024 --time 15 --cores 4

# Show help
./overload-linux-x64 --help
```

*(Replace `overload-linux-x64` with the appropriate binary for your platform.)*

---

## Sample Output

```
Starting overload...
  CPU overload : 4 core(s) for 15 second(s)
  RAM overload : 1024 MB for 15 second(s)

========================================
         Overload Test Report
========================================
Test Type       : CPU + RAM
Duration        : 15 seconds (actual: 15.1 s)
CPU Cores Used  : 4
RAM Allocated   : 1024 MB
Status          : Test completed successfully
========================================
```

---

## Error Handling

| Condition | Behaviour |
|-----------|-----------|
| Unknown / invalid argument | Prints error + full usage, exits with code `1` |
| `--time` / `--cores` missing value | Prints error + usage, exits with code `1` |
| `--ram` value is not a positive integer | Prints error + usage, exits with code `1` |
| `--ram MB` exceeds available RAM | Prints error with available amount and safe suggestion, exits with code `1` |
| `--cores N` exceeds logical core count | Prints warning, continues — threads are time-sliced by the OS |
| `malloc` failure | Prints descriptive error message, exits with code `1` |
| Thread creation failure | Prints error, joins any already-running threads, exits with code `1` |
| Available RAM undetectable (auto mode) | Falls back to 512 MB with a warning, continues |

On success the process exits with code `0`.

---

## Resource Cleanup

All resources are released before exit regardless of the code path:

- CPU threads are joined before the report is printed.
- Allocated RAM is freed with `free()` after the hold period ends.
- No persistent state is written to disk.

---

## Behaviour Notes

- **Timer accuracy**: actual duration may be slightly longer than requested
  (typically < 200 ms) due to thread scheduling and the 100 ms sleep granularity
  used in the RAM hold loop.
- **Auto RAM on high-memory machines**: when `--ram` is used without an explicit
  MB value on a machine with many tens of GB free, the tool will touch as many
  pages as possible within the requested duration and report the actual amount
  committed. The difference between "available" and "allocated" in the report is
  the amount that could not be touched before the timer expired.
- **Compiler optimisation prevention**: all overload variables are declared
  `volatile` and GCC/MSVC memory-barrier intrinsics are inserted to prevent the
  compiler from eliminating the busy loops or memory writes.
- **Windows threading**: on Windows, Win32 `CreateThread` / `WaitForSingleObject`
  is used instead of pthreads. No extra DLLs are required — MinGW statically
  links the C runtime.
