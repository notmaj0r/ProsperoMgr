# ProsperoMgr — PS5 build entry point.
#
# Usage:
#   export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
#   make                      # -> build/ProsperoMgr.elf
#   make test PS5_HOST=<ip>   # stream to elfldr on :9021
#   make frontend             # npm build + embed into backend/src/assets.cpp
#   make payloads             # embed source/payloads/*.elf into bundled_payloads.cpp
#   make tile-pkg             # rebuild dist/ProsperoMgr.pkg + embed into bundled_tile_pkg.cpp
#   make clean

PS5_HOST ?= ps5
PS5_PORT ?= 9021

SRC_DIR := source/backend/src
INC_DIR := source/backend/include
THIRD_PARTY_DIR := source/backend/third_party
BUILD_DIR := build
PKG_HELPER_SRC := source/helpers/prospero_pkgd.c
PKG_HELPER_POLICY := $(INC_DIR)/pkg_install_policy.h
PKG_HELPER_ELF := source/payloads/prospero-pkgd.elf
BUNDLED_PAYLOADS_CPP := $(SRC_DIR)/bundled_payloads.cpp
# Excludes the helper: it is listed separately so it can be built on demand.
PAYLOAD_ELFS := $(filter-out $(PKG_HELPER_ELF),$(wildcard source/payloads/*.elf))
PS5_HTTP_BRIDGE_SRC := $(SRC_DIR)/ps5_http_bridge.c

SOURCES := $(wildcard $(SRC_DIR)/*.cpp)

TARGET_PS5 := $(BUILD_DIR)/ProsperoMgr.elf

# UnRAR (.rar extraction) vendored under third_party/unrar/ — see its license.txt.
# Only these files are real compilation units; the rest of that directory is pulled
# in via #include and must not also be compiled on its own (duplicate symbols).
UNRAR_DIR := $(THIRD_PARTY_DIR)/unrar
UNRAR_SOURCES := archive.cpp arcread.cpp blake2s.cpp cmddata.cpp consio.cpp crc.cpp crypt.cpp \
                 dll.cpp encname.cpp errhnd.cpp extinfo.cpp extract.cpp filcreat.cpp file.cpp \
                 filefn.cpp filestr.cpp find.cpp getbits.cpp global.cpp hash.cpp headers.cpp \
                 largepage.cpp match.cpp options.cpp pathfn.cpp qopen.cpp rar.cpp rarpch.cpp \
                 rarvm.cpp rawread.cpp rdwrfn.cpp rijndael.cpp rs16.cpp rs.cpp scantree.cpp \
                 secpassword.cpp sha1.cpp sha256.cpp smallfn.cpp strfn.cpp strlist.cpp system.cpp \
                 threadpool.cpp timefn.cpp ui.cpp unicode.cpp unpack.cpp volume.cpp
UNRAR_OBJS := $(patsubst %.cpp,$(BUILD_DIR)/obj-ps5/unrar/%.o,$(UNRAR_SOURCES))

# sqlite3.c (app.db reads) and miniz.c (save-data zip backups) are vendored
# public-domain C libraries, not C++, so they get their own rules below.
OBJ_PS5 := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/obj-ps5/%.o,$(SOURCES)) $(BUILD_DIR)/obj-ps5/ps5_http_bridge.o $(BUILD_DIR)/obj-ps5/sqlite3.o $(BUILD_DIR)/obj-ps5/miniz.o $(UNRAR_OBJS)

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

VERSION_TAG := $(shell git describe --abbrev=6 --dirty --always --tags 2>/dev/null || echo dev)

# DEBUG_NET=1 make  ->  compiles in the /api/debug/net-sink and
# /api/debug/net-source transfer-pipeline instrumentation routes (see
# api_routes.cpp). Off by default — never shipped in a normal build.
DEBUG_NET_FLAGS :=
ifeq ($(DEBUG_NET),1)
    DEBUG_NET_FLAGS := -DPROSPEROMGR_DEBUG_NET
endif

# -Wframe-larger-than: a background job whose frame outgrows its worker
# thread's stack takes the whole process down, with no signal left to report it.
CXXFLAGS_PS5 := -std=c++20 -O2 -Wall -Wframe-larger-than=16384 -I$(INC_DIR) -I$(THIRD_PARTY_DIR) \
                -I$(PS5_PAYLOAD_SDK)/target/include \
                -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include \
                -DPROSPEROMGR_PS5 -DVERSION_TAG=\"$(VERSION_TAG)\" $(DEBUG_NET_FLAGS)
# Only mbedcrypto (mbedtls_sha256_* for repository checksum verification) —
# libcurl/libmbedtls/libmbedx509 were the old download transport and are no
# longer linked; downloads go through native sceHttp (SceHttp/SceSsl/SceNet
# below) instead. All four still come from the same build_deps.sh bundle.
# libSceSystemService/libSceUserService/libSceRegMgr/libSceAppInstUtil are
# PS5 system-service stubs (games library launch, foreground-user lookup,
# offline account activation registry writes, PKG install).
LDFLAGS_PS5 := $(PS5_PAYLOAD_SDK)/target/lib/libmbedcrypto.a $(PS5_PAYLOAD_SDK)/target/lib/libmicrohttpd.a \
                -L$(PS5_PAYLOAD_SDK)/target/lib -lpthread -lkernel_sys \
                -lSceSystemService -lSceUserService -lSceRegMgr -lSceAppInstUtil \
                -lSceNetCtl -lSceHttp -lSceHttp2 -lSceSsl -lSceNet -lSceNotification -lSceIpmi

# Trimmed amalgamation build — read-only usage, no extension/window-function
# support needed (and this amalgamation doesn't compile with windowfunc half-on).
CFLAGS_SQLITE := -O2 -w -DSQLITE_OMIT_LOAD_EXTENSION=1 -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_WINDOWFUNC=1
CFLAGS_MINIZ := -O2 -w

.PHONY: all frontend payloads pkg-helper tile-pkg clean test check-ps5-sdk check-network-deps

all: check-ps5-sdk check-network-deps $(TARGET_PS5)

check-ps5-sdk:
ifndef PS5_PAYLOAD_SDK
	$(error PS5_PAYLOAD_SDK is undefined — see /opt/ps5-payload-sdk)
endif

check-network-deps:
	@test -f "$(PS5_PAYLOAD_SDK)/target/lib/libmbedcrypto.a" || \
		( echo "Missing PS5 mbedTLS dependency (libmbedcrypto.a, used for checksums); run PS5-Projects/ps5-payload-manager-main/build_deps.sh"; exit 1 )

$(TARGET_PS5): $(OBJ_PS5)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS_PS5) -o $@ $^ $(LDFLAGS_PS5)

$(BUILD_DIR)/obj-ps5/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_PS5) -MMD -MP -c -o $@ $<

# stb's decoders keep large scratch buffers on the stack. They only ever run
# on request threads, never on a job worker, so the limit above doesn't apply.
$(BUILD_DIR)/obj-ps5/stb_impl.o: CXXFLAGS_PS5 += -Wno-frame-larger-than

# _UNIX switches unrar/dll.hpp's HANDLE/LPARAM/PASCAL aliases to their Unix forms.
$(BUILD_DIR)/obj-ps5/file_jobs.o: CXXFLAGS_PS5 += -D_UNIX

$(BUILD_DIR)/obj-ps5/ps5_http_bridge.o: $(PS5_HTTP_BRIDGE_SRC)
	@mkdir -p $(dir $@)
	$(CC) -Os -Wall -I$(INC_DIR) -I$(PS5_PAYLOAD_SDK)/target/include \
		-DPROSPEROMGR_PS5 -MMD -MP -c -o $@ $<

pkg-helper: check-ps5-sdk $(PKG_HELPER_ELF)

$(PKG_HELPER_ELF): $(PKG_HELPER_SRC) $(PKG_HELPER_POLICY) Makefile
	$(CC) -Os -Wall -Werror -I$(INC_DIR) -o $@ $< \
		-lSceSystemService -lSceUserService -lSceAppInstUtil -lSceNetCtl \
		-lSceNotification -lkernel_sys -lpthread -lSceNet
	$(STRIP) --strip-all $@

$(BUILD_DIR)/obj-ps5/sqlite3.o: $(THIRD_PARTY_DIR)/sqlite3.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_SQLITE) -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include -DPROSPEROMGR_PS5 -c -o $@ $<

$(BUILD_DIR)/obj-ps5/miniz.o: $(THIRD_PARTY_DIR)/miniz.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_MINIZ) -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include -DPROSPEROMGR_PS5 -c -o $@ $<

# RARDLL: builds the library API (RAROpenArchive/RARReadHeader/...), not the CLI —
# excludes rar.cpp's main(). SILENT: no console output from a background worker.
$(BUILD_DIR)/obj-ps5/unrar/%.o: $(UNRAR_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++20 -O2 -w -DRARDLL -D_UNIX -DSILENT -I$(UNRAR_DIR) \
		-I$(PS5_PAYLOAD_SDK)/target/include -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include \
		-DPROSPEROMGR_PS5 -c -o $@ $<

frontend:
	# node_modules/.bin/vite is a symlink npm doesn't always relink (e.g. node_modules
	# copied instead of freshly installed) — run vite's real entry point instead.
	cd source/frontend && npm install && node node_modules/vite/bin/vite.js build
	python3 source/tools/gen_assets.py

payloads: $(BUNDLED_PAYLOADS_CPP)

$(BUNDLED_PAYLOADS_CPP): $(PAYLOAD_ELFS) $(PKG_HELPER_ELF) source/tools/gen_payloads.py
	python3 source/tools/gen_payloads.py

tile-pkg:
	pkg-source/build-pkg.sh
	python3 source/tools/gen_tile_pkg.py

test: $(TARGET_PS5)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

clean:
	rm -rf $(BUILD_DIR)

# Auto-generated by -MMD above — makes header changes trigger a rebuild of
# every .cpp that includes them, instead of only the .cpp that directly changed.
-include $(OBJ_PS5:.o=.d)
