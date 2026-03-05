# =============================================================================
# Makefile — overload cross-platform build
#
# Targets:
#   make / make all          Linux x86_64  (native gcc)
#   make linux-arm64         Linux ARM64   (aarch64-linux-gnu-gcc)
#   make windows-x64         Windows x64   (x86_64-w64-mingw32-gcc)
#   make windows-arm64       Windows ARM64 (aarch64-w64-mingw32-gcc via llvm-mingw)
#   make macos-x64           macOS x86_64  (clang, native on Intel Mac)
#   make macos-arm64         macOS ARM64   (clang, native on Apple Silicon)
#   make all-targets         Build all platform targets
#   make clean               Remove all binaries
# =============================================================================

# ---------- Toolchains -------------------------------------------------------
CC_LINUX_X64    = gcc
CC_LINUX_ARM64  = aarch64-linux-gnu-gcc
CC_WIN_X64      = x86_64-w64-mingw32-gcc
CC_WIN_ARM64    = aarch64-w64-mingw32-gcc   # requires llvm-mingw in PATH
CC_MACOS        = clang

# ---------- Flags ------------------------------------------------------------
CFLAGS_COMMON   = -Wall -Wextra -pedantic -std=c99 -O2
CFLAGS_LINUX    = $(CFLAGS_COMMON) -pthread
CFLAGS_WIN      = $(CFLAGS_COMMON)
CFLAGS_MACOS    = $(CFLAGS_COMMON) -pthread

# ---------- Source & output names --------------------------------------------
SRC             = overload.c
BIN_LINUX_X64   = overload-linux-x64
BIN_LINUX_ARM64 = overload-linux-arm64
BIN_WIN_X64     = overload-windows-x64.exe
BIN_WIN_ARM64   = overload-windows-arm64.exe
BIN_MACOS_X64   = overload-macos-x64
BIN_MACOS_ARM64 = overload-macos-arm64

# ---------- Default target ---------------------------------------------------
.PHONY: all linux-x64 linux-arm64 windows-x64 windows-arm64 macos-x64 macos-arm64 all-targets clean

all: linux-x64

# ---------- Linux x86_64 (native) --------------------------------------------
linux-x64: $(SRC)
	$(CC_LINUX_X64) $(CFLAGS_LINUX) -o $(BIN_LINUX_X64) $(SRC)
	@echo "Built: $(BIN_LINUX_X64)"

# ---------- Linux ARM64 ------------------------------------------------------
linux-arm64: $(SRC)
	@command -v $(CC_LINUX_ARM64) >/dev/null 2>&1 || \
	  { echo "ERROR: $(CC_LINUX_ARM64) not found."; \
	    echo "  Install with: sudo apt install gcc-aarch64-linux-gnu"; \
	    exit 1; }
	$(CC_LINUX_ARM64) $(CFLAGS_LINUX) -o $(BIN_LINUX_ARM64) $(SRC)
	@echo "Built: $(BIN_LINUX_ARM64)"

# ---------- Windows x64 ------------------------------------------------------
windows-x64: $(SRC)
	@command -v $(CC_WIN_X64) >/dev/null 2>&1 || \
	  { echo "ERROR: $(CC_WIN_X64) not found."; \
	    echo "  Install with: sudo apt install mingw-w64"; \
	    exit 1; }
	$(CC_WIN_X64) $(CFLAGS_WIN) -o $(BIN_WIN_X64) $(SRC)
	@echo "Built: $(BIN_WIN_X64)"

# ---------- Windows ARM64 (requires llvm-mingw) ------------------------------
windows-arm64: $(SRC)
	@command -v $(CC_WIN_ARM64) >/dev/null 2>&1 || \
	  { echo "ERROR: $(CC_WIN_ARM64) not found."; \
	    echo "  llvm-mingw is required for ARM64 Windows cross-compilation."; \
	    echo "  Download from: https://github.com/mstorsjo/llvm-mingw/releases"; \
	    echo "  Extract and add its bin/ directory to your PATH, then retry."; \
	    exit 1; }
	$(CC_WIN_ARM64) $(CFLAGS_WIN) -o $(BIN_WIN_ARM64) $(SRC)
	@echo "Built: $(BIN_WIN_ARM64)"

# ---------- macOS x86_64 (native on Intel Mac) -------------------------------
macos-x64: $(SRC)
	$(CC_MACOS) $(CFLAGS_MACOS) -o $(BIN_MACOS_X64) $(SRC)
	@echo "Built: $(BIN_MACOS_X64)"

# ---------- macOS ARM64 (native on Apple Silicon) ----------------------------
macos-arm64: $(SRC)
	$(CC_MACOS) $(CFLAGS_MACOS) -o $(BIN_MACOS_ARM64) $(SRC)
	@echo "Built: $(BIN_MACOS_ARM64)"

# ---------- Build all targets ------------------------------------------------
all-targets: linux-x64 linux-arm64 windows-x64 windows-arm64 macos-x64 macos-arm64

# ---------- Clean ------------------------------------------------------------
clean:
	rm -f $(BIN_LINUX_X64) $(BIN_LINUX_ARM64) $(BIN_WIN_X64) $(BIN_WIN_ARM64) \
	      $(BIN_MACOS_X64) $(BIN_MACOS_ARM64) overload
	@echo "Cleaned."
