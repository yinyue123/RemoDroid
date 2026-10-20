# Desktop agents (multi-platform.md), included from the top Makefile.
#
#   make desktop                                        this computer (Linux): build/host/desktop/
#   make desktop TARGET=win-x86_64 CC="zig cc -target x86_64-windows-gnu" AR="zig ar"
#   make desktop TARGET=mac (on macOS; ARCHFLAGS="-arch arm64 -arch x86_64" for universal2)
#
# Every target produces libremodroid (the WebRTC stack shared with the phone, plus the desktop
# agent) as a shared library, remodroid-desktopd (no UI) and the platform's app:
#   Linux    libremodroid.so     remodroid-desktopd  remodroid-desktop (GTK 3, `make desktop-app`)
#   Windows  remodroid.dll       remodroid-desktopd.exe  RemoDroid.exe
#   macOS    libremodroid.dylib  remodroid-desktopd  RemoDroid.app

DESK_OUT := $(OUT)/desktop
ifneq ($(filter win%,$(TARGET)),)
DESK_OS := windows
else ifneq ($(filter mac%,$(TARGET)),)
DESK_OS := macos
else
DESK_OS ?= linux
endif
DESK_SRC := desktop/$(if $(filter windows,$(DESK_OS)),win,$(if $(filter macos,$(DESK_OS)),mac,linux))

DESK_INCS := -Idesktop -Idesktop/core -I$(DESK_SRC) -Irtc -Icommon -Ithird_party/libev -Ithird_party/cjson \
             -Ithird_party/inih -Ithird_party/picohttpparser -Ithird_party/mbedtls/include \
             -Ithird_party/libsrtp/include -Ithird_party/libsrtp/crypto/include -Ithird_party/usrsctp/usrsctplib \
             -Ithird_party/minih264
DESK_DEFS := -DRD_DESKTOP -DMBEDTLS_CONFIG_FILE='"mbedtls_desktop_config.h"' \
             -DINI_ALLOW_INLINE_COMMENTS=1 -DINI_STOP_ON_FIRST_ERROR=1 -DINI_MAX_LINE=1024 -DEV_STANDALONE=1 \
             -DINI_API=
DESK_SCTP := -D__Userspace__ -DSCTP_SIMPLE_ALLOCATOR -DSCTP_PROCESS_LEVEL_LOCKS -Wno-address-of-packed-member

ifeq ($(DESK_OS),windows)
DESK_LIB   := $(DESK_OUT)/remodroid.dll
DESK_EXE   := .exe
DESK_DEFS  += -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE -DNOMINMAX \
              -DEV_USE_SELECT=1 -DEV_SELECT_IS_WINSOCKET=1 -DEV_USE_POLL=0 -DEV_USE_EPOLL=0 -DEV_USE_KQUEUE=0 \
              -DEV_USE_EVENTFD=0 -DEV_USE_INOTIFY=0 -DEV_USE_SIGNALFD=0 -DEV_USE_TIMERFD=0 -DEV_USE_LINUXAIO=0 \
              -DEV_USE_IOURING=0 -DEV_USE_MONOTONIC=0 -DEV_USE_REALTIME=0 -DEV_USE_NANOSLEEP=0 -DEV_USE_4HEAP=1 \
              -DEV_COMPAT3=0 -D'EV_FD_TO_WIN32_HANDLE(fd)=(fd)' -D'EV_WIN32_HANDLE_TO_FD(h)=(h)' \
              -D'EV_WIN32_CLOSE_FD(fd)=closesocket(fd)' -DFD_SETSIZE=1024
DESK_LIBS  := -lws2_32 -liphlpapi -lbcrypt -lcrypt32 -ld3d11 -ldxgi -lmfplat -lmf -lmfuuid -lole32 -loleaut32 \
              -luser32 -lgdi32 -lshell32 -lshlwapi -luuid -lwinmm -lpowrprof
DESK_SHARED := -shared -static-libgcc -Wl,--out-implib,$(DESK_OUT)/libremodroid.dll.a
DESK_TEST_LIBS = $(DESK_LIBS) -static-libgcc
DESK_PIC   :=
DESK_HEADLESS_EXTRA := $(DESK_OUT)/app.res   # the manifest: UTF-8 file names, DPI awareness
else ifeq ($(DESK_OS),macos)
DESK_LIB   := $(DESK_OUT)/libremodroid.dylib
DESK_EXE   :=
DESK_DEFS  += -D_DARWIN_C_SOURCE -DEV_USE_SELECT=1 -DEV_USE_POLL=1 -DEV_USE_KQUEUE=0 -DEV_USE_EPOLL=0 \
              -DEV_USE_EVENTFD=0 -DEV_USE_MONOTONIC=1 -DEV_USE_REALTIME=0 -DEV_USE_NANOSLEEP=1 -DEV_USE_4HEAP=1 \
              -DEV_COMPAT3=0 -DHAVE_SA_LEN -DHAVE_SIN_LEN -DHAVE_SIN6_LEN -DHAVE_SCONN_LEN -D__APPLE_USE_RFC_2292
DESK_SCTP  += -DHAVE_SYS_QUEUE_H -DHAVE_STDATOMIC_H
DESK_LIBS  := -framework ApplicationServices -framework CoreFoundation -framework CoreGraphics -framework CoreMedia \
              -framework CoreVideo -framework VideoToolbox -framework ScreenCaptureKit -framework Security \
              -framework AppKit -framework UserNotifications -framework IOKit -lobjc
DESK_SHARED := -dynamiclib -install_name @rpath/libremodroid.dylib
DESK_TEST_LIBS := -framework CoreFoundation -framework Security
DESK_PIC   := -fPIC
else
DESK_LIB   := $(DESK_OUT)/libremodroid.so
DESK_EXE   :=
DESK_DEFS  += -D_GNU_SOURCE $(EV_DEFS)
DESK_SCTP  += -DHAVE_SYS_QUEUE_H -DHAVE_STDATOMIC_H
DESK_LIBS  := -lX11 -lXext -lXtst -lXrandr -lXdamage -lXfixes -lm -lpthread
DESK_SHARED := -shared -Wl,-soname,libremodroid.so -Wl,--gc-sections -Wl,--exclude-libs,ALL
DESK_TEST_LIBS := -lm -lpthread
DESK_PIC   := -fPIC
endif

DESK_CFLAGS := $(OPT) $(ARCHFLAGS) -std=gnu11 -fno-strict-aliasing -ffunction-sections -fdata-sections \
               -fvisibility=hidden $(DESK_PIC) $(DESK_INCS) $(DESK_DEFS)
DESK_WARN   := -Wall -Wextra -Wno-unused-parameter -Wshadow -Wno-missing-field-initializers -Wno-format-truncation

DESK_OWN := $(wildcard desktop/core/*.c) $(wildcard $(DESK_SRC)/*.c) $(wildcard $(DESK_SRC)/*.m) \
            $(wildcard rtc/*.c) \
            common/util.c common/tunnel.c common/msg.c common/stun.c common/plat.c common/wsc.c common/ws.c \
            common/httpc.c common/enroll.c common/cas.c common/conf.c
DESK_TP  := third_party/libev/ev.c third_party/cjson/cJSON.c third_party/inih/ini.c \
            third_party/picohttpparser/picohttpparser.c $(wildcard third_party/mbedtls/library/*.c) \
            third_party/libsrtp/srtp/srtp.c $(wildcard third_party/libsrtp/crypto/*/*.c) \
            $(wildcard third_party/usrsctp/usrsctplib/*.c) $(wildcard third_party/usrsctp/usrsctplib/netinet/*.c) \
            $(wildcard third_party/usrsctp/usrsctplib/netinet6/*.c)
DESK_OBJ := $(patsubst %,$(DESK_OUT)/obj/%.o,$(DESK_OWN) $(DESK_TP))

$(DESK_OUT)/obj/third_party/libsrtp/%.c.o: third_party/libsrtp/%.c
	@mkdir -p $(@D)
	$(CC) $(DESK_CFLAGS) -DHAVE_CONFIG_H -Ithird_party/libsrtp -w -c $< -o $@

$(DESK_OUT)/obj/third_party/usrsctp/%.c.o: third_party/usrsctp/%.c
	@mkdir -p $(@D)
	$(CC) $(DESK_CFLAGS) $(DESK_SCTP) -UNOMINMAX -w -c $< -o $@  # usrsctp uses windows.h's min/max

$(DESK_OUT)/obj/third_party/%.c.o: third_party/%.c
	@mkdir -p $(@D)
	$(CC) $(DESK_CFLAGS) -w -c $< -o $@

# usrsctp's headers also shape rtc/sctp.c
$(DESK_OUT)/obj/rtc/sctp.c.o: rtc/sctp.c
	@mkdir -p $(@D)
	$(CC) $(DESK_CFLAGS) $(DESK_SCTP) $(DESK_WARN) -MMD -MP -c $< -o $@

$(DESK_OUT)/obj/%.c.o: %.c
	@mkdir -p $(@D)
	$(CC) $(DESK_CFLAGS) $(DESK_WARN) -MMD -MP -c $< -o $@

$(DESK_OUT)/obj/%.m.o: %.m
	@mkdir -p $(@D)
	$(CC) $(DESK_CFLAGS) $(DESK_WARN) -fobjc-arc -MMD -MP -c $< -o $@

$(DESK_LIB): $(DESK_OBJ)
	$(CC) $(ARCHFLAGS) $(DESK_SHARED) -o $@ $^ $(DESK_LIBS)

ifeq ($(DESK_OS),windows)
DESK_LINK := $(DESK_OUT)/libremodroid.dll.a
DESK_RPATH :=
else ifeq ($(DESK_OS),macos)
DESK_LINK := $(DESK_LIB)
DESK_RPATH := -Wl,-rpath,@executable_path -Wl,-rpath,@executable_path/../Frameworks
else
DESK_LINK := $(DESK_LIB)
DESK_RPATH := -Wl,-rpath,'$$ORIGIN' -Wl,-rpath,'$$ORIGIN/../lib/remodroid'
endif

$(DESK_OUT)/remodroid-desktopd$(DESK_EXE): desktop/app/headless.c $(DESK_LIB) $(DESK_HEADLESS_EXTRA)
	$(CC) $(OPT) $(ARCHFLAGS) -std=gnu11 -Idesktop/core $(DESK_WARN) -o $@ desktop/app/headless.c $(DESK_HEADLESS_EXTRA) \
	    $(DESK_LINK) $(DESK_RPATH)

# Unit tests of the shared desktop code (no display needed)
$(DESK_OUT)/desk_test$(DESK_EXE): desktop/test/desk_test.c $(filter-out $(DESK_OUT)/obj/$(DESK_SRC)/%,$(DESK_OBJ))
	$(CC) $(DESK_CFLAGS) $(DESK_WARN) -o $@ $^ desktop/test/stubs.c $(DESK_TEST_LIBS)

desktop: $(DESK_LIB) $(DESK_OUT)/remodroid-desktopd$(DESK_EXE)
desktop-test: $(DESK_OUT)/desk_test$(DESK_EXE)

-include $(patsubst %.o,%.d,$(filter $(DESK_OUT)/obj/desktop/% $(DESK_OUT)/obj/rtc/% $(DESK_OUT)/obj/common/%,$(DESK_OBJ)))

.PHONY: desktop desktop-test

include desktop/app/app.mk
