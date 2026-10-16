# RemoDroid build.
#
#   make                         host build (for development and tests)
#   make test                    build and run the tests
#   make TARGET=mipsel CC=mipsel-linux-musl-gcc STATIC=1
#                                cross build for a router (static, musl)
#   make dist                    every target inside the build container (build/Dockerfile)
#
# All third-party libraries are compiled from third_party/ and linked in.

TARGET  ?= host
CC      ?= cc
AR      ?= ar
STRIP   ?= strip
STATIC  ?= 0
OUT     := build/$(TARGET)

EV_DEFS := -DEV_STANDALONE=1 -DEV_USE_EPOLL=1 -DEV_USE_POLL=1 -DEV_USE_SELECT=1 \
           -DEV_USE_LINUXAIO=0 -DEV_USE_IOURING=0 -DEV_USE_INOTIFY=0 -DEV_USE_EVENTFD=1 \
           -DEV_USE_SIGNALFD=0 -DEV_USE_TIMERFD=0 -DEV_USE_MONOTONIC=1 -DEV_USE_REALTIME=0 \
           -DEV_USE_NANOSLEEP=1 -DEV_USE_4HEAP=1 -DEV_COMPAT3=0

INCS    := -Icommon -Ithird_party/libev -Ithird_party/inih -Ithird_party/cjson \
           -Ithird_party/picohttpparser -Ithird_party/crypt_blowfish -Ithird_party/mbedtls/include
DEFS    := -D_GNU_SOURCE $(EV_DEFS) -DMBEDTLS_CONFIG_FILE='"rd_mbedtls_config.h"' \
           -DINI_ALLOW_INLINE_COMMENTS=1 -DINI_STOP_ON_FIRST_ERROR=1 -DINI_MAX_LINE=1024
OPT     ?= -Os -g
CFLAGS  += $(OPT) -std=c11 -fno-strict-aliasing -ffunction-sections -fdata-sections $(INCS) $(DEFS)
WARN    := -Wall -Wextra -Wno-unused-parameter -Wshadow -Wformat=2 -Wno-format-truncation
LDFLAGS += -Wl,--gc-sections
ifeq ($(STATIC),1)
LDFLAGS += -static
endif
LIBS    := -lm


# ---- third party ----
MBED_SRC := $(wildcard third_party/mbedtls/library/*.c)
TP_SRC   := third_party/libev/ev.c third_party/inih/ini.c third_party/cjson/cJSON.c \
            third_party/picohttpparser/picohttpparser.c third_party/crypt_blowfish/crypt_blowfish.c \
            $(MBED_SRC)
TP_OBJ   := $(TP_SRC:%.c=$(OUT)/obj/%.o)

COMMON_SRC := $(wildcard common/*.c)
COMMON_OBJ := $(COMMON_SRC:%.c=$(OUT)/obj/%.o)
SERVER_SRC := $(wildcard server/*.c)
SERVER_OBJ := $(SERVER_SRC:%.c=$(OUT)/obj/%.o)
AGENT_SRC  := $(wildcard agent/*.c)
AGENT_OBJ  := $(AGENT_SRC:%.c=$(OUT)/obj/%.o)

all: $(OUT)/remodroid-server $(OUT)/remodroid-agent

$(OUT)/libthird.a: $(TP_OBJ)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(OUT)/obj/third_party/%.o: third_party/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -w -c $< -o $@

# mbedTLS struct layouts follow its config: rebuild the library when it changes
$(filter $(OUT)/obj/third_party/mbedtls/%,$(TP_OBJ)): common/rd_mbedtls_config.h

$(OUT)/obj/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARN) -MMD -MP -c $< -o $@

$(OUT)/remodroid-server: $(SERVER_OBJ) $(COMMON_OBJ) $(OUT)/libthird.a
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS) -lpthread

$(OUT)/remodroid-agent: $(AGENT_OBJ) $(COMMON_OBJ) $(OUT)/libthird.a
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

# Stripped copies for deployment; the unstripped binaries keep debug info for gdb.
strip: all
	@mkdir -p $(OUT)/dist
	$(STRIP) -o $(OUT)/dist/remodroid-server $(OUT)/remodroid-server
	$(STRIP) -o $(OUT)/dist/remodroid-agent $(OUT)/remodroid-agent
	@ls -l $(OUT)/dist

test: all
	node tests/run.mjs $(OUT)
	node tests/tls.mjs $(OUT)   # PEBBLE_DIR=<pebble binaries> adds the ACME tests

# WebRTC suites (need python with aiortc; the browser test also needs playwright + chromium)
PY ?= python3
test-webrtc: all rtc-tests
	$(PY) android/native/test/rtc_interop.py --bin $(OUT)/rtc/rtc_test
	$(PY) android/native/test/rtp_depack.py --bin $(OUT)/rtc/rtp_dump
	$(PY) tests/e2e_webrtc.py --bin $(OUT)
	$(PY) tests/e2e_webrtc.py --bin $(OUT) --relay
	$(PY) tests/e2e_browser.py --bin $(OUT)

dist:
	podman build -t remodroid-build build
	podman run --rm -v $(CURDIR):/src:Z -w /src remodroid-build ./build/dist.sh

clean:
	rm -rf build/host build/x86_64 build/aarch64 build/arm build/mips build/mipsel build/rel-* build/out

-include $(SERVER_OBJ:.o=.d) $(AGENT_OBJ:.o=.d) $(COMMON_OBJ:.o=.d)

include android/native/rtc.mk

.PHONY: all strip test test-webrtc dist clean
