# ==========================================================================
#  IntelXeKext Makefile
#  Builds IntelXeDriver.kext for x86_64 macOS (XNU kernel extension)
#
#  Three build targets:
#    make                - native build with xcrun (requires macOS + Xcode)
#    make CC=<osxcross>  - cross-build with osxcross on Linux
#    make docker         - builds inside an osxcross Docker container
# ==========================================================================

# ------------------------------------------------------------------
#  Configuration
# ------------------------------------------------------------------

KEXT_NAME       := IntelXeDriver
BUNDLE_ID       := com.intelxe.driver.IntelXeDriver
SRC_DIR         := .
BUILD_DIR       := build
KEXT_BUNDLE     := $(BUILD_DIR)/$(KEXT_NAME).kext
EXECUTABLE      := $(KEXT_BUNDLE)/Contents/MacOS/$(KEXT_NAME)

SOURCES         := $(SRC_DIR)/IntelXeDriver.cpp
HEADERS         := $(SRC_DIR)/IntelXeDriver.hpp
PLIST_SRC      := $(SRC_DIR)/Info.plist
PLIST_DST      := $(KEXT_BUNDLE)/Contents/Info.plist

# ------------------------------------------------------------------
#  Toolchain detection
# ------------------------------------------------------------------

# If CC is set (e.g. o64-clang++), use cross-compilation mode
ifdef CC
CROSS           := 1
CXX             := $(CC)
LD              := $(shell echo "$(CXX)" | sed 's/clang++$$/ld64.lld/')
STRIP           := x86_64-apple-darwin-strip
LIBTOOL         := x86_64-apple-darwin-libtool
SDK             ?= $(shell dirname $(shell which $(CXX) | xargs dirname))/../SDK/MacOSX.sdk
SDK_FLAGS       := -isysroot $(SDK)
else
CROSS           := 0
CXX             := $(shell xcrun --find clang++ 2>/dev/null || echo clang++)
LD              := $(CXX)
STRIP           := strip
LIBTOOL         := libtool
SDK_FLAGS       := $(shell xcrun --sdk macosx --show-sdk-path 2>/dev/null | xargs -I{} echo -isysroot {})
endif

# ------------------------------------------------------------------
#  Compiler / linker flags for a macOS KEXT (kernel C++)
# ------------------------------------------------------------------

# Architecture
ARCH            := x86_64
ARCH_FLAGS      := -arch $(ARCH)

# Kernel C++ subset: no exceptions, no RTTI, no standard library
CXXFLAGS_COMMON := \
	-fno-exceptions                        \
	-fno-rtti                             \
	-fno-common                           \
	-nostdinc++                          \
	-fno-strict-aliasing                  \
	-fno-inline-functions                 \
	-ffreestanding                        \
	-mkernel                              \
	-Wall -Wextra -Wno-unused-parameter   \
	-DKERNEL                              \
	-D__APPLE__                           \
	-D__DARWIN__                          \
	-Wno-deprecated-declarations          \
	-Os

# SDK framework include paths
ifeq ($(CROSS),1)
# osxcross: SDK layout has frameworks under SDK/System/Library/Frameworks
FWK_INCLUDE    := \
	$(SDK_FLAGS)                                    \
	-I$(SDK)/System/Library/Frameworks/Kernel.framework/Headers     \
	-I$(SDK)/System/Library/Frameworks/IOKit.framework/Headers      \
	-I$(SDK)/System/Library/Frameworks/System.framework/Headers
else
# Native macOS: use -iframework for framework header search
FWK_INCLUDE    := \
	$(SDK_FLAGS)                                    \
	-iframework /System/Library/Frameworks/Kernel.framework       \
	-iframework /System/Library/Frameworks/IOKit.framework
endif

CXXFLAGS        := $(ARCH_FLAGS) $(CXXFLAGS_COMMON) $(FWK_INCLUDE)

# KEXT linker flags
#   -static               no dynamic linking against userland libs
#   -Wl,-kext             mark output as MH_KEXT_BUNDLE
#   -Wl,-undefined,dynamic_lookup  allow unresolved symbols (resolved at kext load)
#   -Wl,-no_data_const      avoid data-const relocation issues in kernel
LDFLAGS         := \
	$(ARCH_FLAGS)                       \
	-static                             \
	-nostdlib                           \
	-Wl,-kext                           \
	-Wl,-undefined,dynamic_lookup       \
	-Wl,-no_data_const                  \
	-lcc_kext

# ------------------------------------------------------------------
#  Build rules
# ------------------------------------------------------------------

.PHONY: all clean bundle docker kext sign

all: bundle

# Create the .kext bundle directory structure
bundle: $(KEXT_BUNDLE)

$(KEXT_BUNDLE): $(EXECUTABLE) $(PLIST_DST)
	@echo "========================================"
	@echo " KEXT built: $(KEXT_BUNDLE)"
	@echo "========================================"

# Link the KEXT object into a Mach-O KEXT bundle
$(EXECUTABLE): $(BUILD_DIR)/IntelXeDriver.o | $(KEXT_BUNDLE)/Contents/MacOS/
	@echo "[LD]   $@"
	$(CXX) $(LDFLAGS) -o $@ $<

# Compile
$(BUILD_DIR)/IntelXeDriver.o: $(SOURCES) $(HEADERS) | $(BUILD_DIR)/
	@echo "[CXX]  $<"
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Copy Info.plist into the bundle
$(PLIST_DST): $(PLIST_SRC) | $(KEXT_BUNDLE)/Contents/
	@echo "[PLIST] $<"
	cp $< $@

# Directory scaffolding
$(BUILD_DIR)/:
	mkdir -p $(BUILD_DIR)

$(KEXT_BUNDLE)/Contents/MacOS/: | $(KEXT_BUNDLE)/Contents/
	mkdir -p $@

$(KEXT_BUNDLE)/Contents/:
	mkdir -p $(KEXT_BUNDLE)/Contents

# ------------------------------------------------------------------
#  Helpers
# ------------------------------------------------------------------

kext: bundle
	@echo ""
	@echo "To load:  sudo kextutil $(KEXT_BUNDLE)"
	@echo "To check: kextstat | grep $(BUNDLE_ID)"
	@echo "To unload: sudo kextunload -b $(BUNDLE_ID)"
	@echo ""

sign: bundle
	@echo "[SIGN] Ad-hoc signing the KEXT bundle..."
	codesign --force --deep --sign - $(KEXT_BUNDLE)
	@echo "Done. Signed: $(KEXT_BUNDLE)"

clean:
	rm -rf $(BUILD_DIR)

# ------------------------------------------------------------------
#  Docker-based osxcross build
# ------------------------------------------------------------------

docker:
	docker build -t intelxe-kext-builder -f Dockerfile.osxcross .
	docker run --rm -v "$(CURDIR):/src" intelxe-kext-builder

# ------------------------------------------------------------------
#  Show resolved toolchain info (useful for debugging)
# ------------------------------------------------------------------

info:
	@echo "CXX          = $(CXX)"
	@echo "LD           = $(LD)"
	@echo "ARCH         = $(ARCH)"
	@echo "CROSS        = $(CROSS)"
	@echo "SDK_FLAGS    = $(SDK_FLAGS)"
	@echo "CXXFLAGS     = $(CXXFLAGS)"
	@echo "LDFLAGS      = $(LDFLAGS)"
	@echo "KEXT_BUNDLE  = $(KEXT_BUNDLE)"
