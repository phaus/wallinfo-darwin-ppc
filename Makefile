# wallinfo-darwin-ppc — wallpaper info overlay for Mac OS X 10.5 (PowerPC)
#
# Cross builds only — the code needs Apple frameworks, there is no native
# (Linux) build.
#
#   make cross32    Mach-O ppc_7400 via osxcross (PowerPC G4) -> bin/wallinfo-ppc
#   make cross64    Mach-O ppc64 via osxcross (G5 only)       -> bin/wallinfo-ppc64
#   make all-cross  both
#
# Prerequisites:
#   - osxcross with macports-legacy ppc targets (oppc32-gcc / oppc64-gcc)
#   - no TLS libs needed (kopia-darwin-ppc needs mbedTLS, this one does not)

CROSS32 ?= /opt/osxcross/target/bin/oppc32-gcc
CROSS64 ?= /opt/osxcross/target/bin/oppc64-gcc

CFLAGS  ?= -O2 -Wall -Wextra -std=gnu99

# ImageIO is a sub-umbrella of ApplicationServices on 10.5 — its headers are
# not on the default include path, so add them explicitly. Linking the
# ApplicationServices umbrella pulls in CoreGraphics + ImageIO.
SDKROOT ?= /opt/osxcross/target/SDK/MacOSX10.5.sdk
IMGIO_INC := -I$(SDKROOT)/System/Library/Frameworks/ApplicationServices.framework/Versions/A/Frameworks/ImageIO.framework/Headers
FRW := -framework CoreFoundation -framework ApplicationServices -framework CoreServices

SRC := src/wallinfo.c
BIN := bin

.PHONY: all cross32 cross64 all-cross clean

all: all-cross

cross32:
	@mkdir -p $(BIN)
	$(CROSS32) $(CFLAGS) $(IMGIO_INC) $(SRC) -o $(BIN)/wallinfo-ppc $(FRW)

cross64:
	@mkdir -p $(BIN)
	$(CROSS64) $(CFLAGS) $(IMGIO_INC) $(SRC) -o $(BIN)/wallinfo-ppc64 $(FRW)

all-cross: cross32 cross64

clean:
	rm -rf $(BIN)
