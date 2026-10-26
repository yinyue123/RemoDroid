# The desktop apps (menu bar / tray icon + pairing window), one per OS; included
# from desktop/desktop.mk. `make desktop-app` builds the one for this OS.
#
#   Windows  RemoDroid.exe (+ remodroid-desktopd.exe gets the same manifest)
#   macOS    RemoDroid.app
#   Linux    remodroid-desktop (GTK 3), icons for the .desktop entry

HOSTCC ?= cc
WINDRES ?= windres
APP_DIR := desktop/app

$(DESK_OUT)/icongen: $(APP_DIR)/icongen.c $(APP_DIR)/icon.c $(APP_DIR)/icon.h
	@mkdir -p $(@D)
	$(HOSTCC) -O2 -o $@ $(APP_DIR)/icongen.c $(APP_DIR)/icon.c

ifeq ($(DESK_OS),windows)
APP := $(DESK_OUT)/RemoDroid.exe

$(DESK_OUT)/remodroid.ico: $(DESK_OUT)/icongen
	$(DESK_OUT)/icongen $@

$(DESK_OUT)/app.res: $(APP_DIR)/win/app.rc $(APP_DIR)/win/app.manifest $(DESK_OUT)/remodroid.ico
	$(WINDRES) -I$(APP_DIR)/win -I$(DESK_OUT) -O coff -i $< -o $@

$(APP): $(APP_DIR)/win/main.c $(APP_DIR)/icon.c $(DESK_OUT)/app.res $(DESK_LIB)
	$(CC) $(OPT) $(ARCHFLAGS) -std=gnu11 -municode -mwindows -I$(APP_DIR) -Idesktop/core $(DESK_WARN) -o $@ \
	    $(APP_DIR)/win/main.c $(APP_DIR)/icon.c $(DESK_OUT)/app.res $(DESK_LINK) -static-libgcc -lcomctl32 -lshell32 -luser32 -lgdi32 -ladvapi32

# remodroid-input.exe: the RemoDroidInput service, for windows of elevated programs (desktop/win/inputpipe.h)
APP_EXTRA := $(DESK_OUT)/remodroid-input.exe
$(APP_EXTRA): $(APP_DIR)/win/inputsvc.c desktop/win/input.c desktop/win/inputpipe.h desktop/core/keymap.c $(DESK_OUT)/app.res
	$(CC) $(DESK_CFLAGS) -DRD_INPUT_HELPER -municode $(DESK_WARN) -o $@ $(APP_DIR)/win/inputsvc.c desktop/win/input.c \
	    desktop/core/keymap.c $(DESK_OUT)/app.res -static -lwtsapi32 -ladvapi32 -luser32 -lshell32
endif

ifeq ($(DESK_OS),macos)
APP := $(DESK_OUT)/RemoDroid.app
VERSION ?= 0.5

$(DESK_OUT)/remodroid.icns: $(DESK_OUT)/icongen
	$(DESK_OUT)/icongen $@

$(DESK_OUT)/RemoDroid: $(APP_DIR)/mac/main.m $(APP_DIR)/icon.c $(DESK_LIB)
	$(CC) $(OPT) $(ARCHFLAGS) -fobjc-arc -I$(APP_DIR) -Idesktop/core $(DESK_WARN) -o $@ $(APP_DIR)/mac/main.m $(APP_DIR)/icon.c \
	    $(DESK_LINK) -Wl,-rpath,@executable_path/../Frameworks -framework AppKit -framework ApplicationServices \
	    -framework ServiceManagement -framework UserNotifications

# the bundle: the app, the agent library, the headless agent, the icon; signed ad hoc (see multi-platform.md §13.4)
$(APP): $(DESK_OUT)/RemoDroid $(DESK_OUT)/remodroid-desktopd $(DESK_OUT)/remodroid.icns $(APP_DIR)/mac/Info.plist
	rm -rf $@ && mkdir -p $@/Contents/MacOS $@/Contents/Frameworks $@/Contents/Resources
	sed 's/@VERSION@/$(VERSION)/g' $(APP_DIR)/mac/Info.plist > $@/Contents/Info.plist
	cp $(DESK_OUT)/RemoDroid $(DESK_OUT)/remodroid-desktopd $@/Contents/MacOS/
	cp $(DESK_LIB) $@/Contents/Frameworks/
	cp $(DESK_OUT)/remodroid.icns $@/Contents/Resources/
	codesign --force --sign - $@/Contents/Frameworks/libremodroid.dylib
	codesign --force --sign - $@/Contents/MacOS/remodroid-desktopd
	codesign --force --sign - --identifier com.remodroid.desktop $@
endif

ifeq ($(DESK_OS),linux)
APP := $(DESK_OUT)/remodroid-desktop
GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS = $(shell pkg-config --libs gtk+-3.0)

$(APP): $(APP_DIR)/linux/main.c $(APP_DIR)/linux/sni.c $(APP_DIR)/linux/sni.h $(APP_DIR)/icon.c $(DESK_LIB)
	$(CC) $(OPT) $(ARCHFLAGS) -std=gnu11 -I$(APP_DIR) -I$(APP_DIR)/linux -Idesktop/core $(GTK_CFLAGS) $(DESK_WARN) \
	    -Wno-deprecated-declarations -o $@ $(APP_DIR)/linux/main.c $(APP_DIR)/linux/sni.c $(APP_DIR)/icon.c \
	    $(DESK_LINK) $(DESK_RPATH) $(GTK_LIBS)

# icons for the .desktop entry and the notifications (hicolor theme sizes)
desktop-icons: $(DESK_OUT)/icongen
	for s in 16 24 32 48 64 128 256 512; do mkdir -p $(DESK_OUT)/icons/$${s}x$${s}/apps; \
	    $(DESK_OUT)/icongen $(DESK_OUT)/icons/$${s}x$${s}/apps/remodroid.png $$s; done
.PHONY: desktop-icons
endif

desktop-app: $(APP) $(APP_EXTRA)
.PHONY: desktop-app
