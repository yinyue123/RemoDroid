# The server for Windows (TARGET=win-x86_64 or win-arm64), included from the top Makefile:
#   remodroid-server.exe   the server, its supervisor and the Windows service (server/win.c)
#   RemoDroidServer.exe   the notification-area icon and settings window (server/win/tray.c)
#
#   make TARGET=win-x86_64 CC="zig cc -target x86_64-windows-gnu" WINDRES=llvm-windres
#   make TARGET=win-x86_64 CC=gcc HOSTCC=gcc (MSYS2 UCRT64)

ifneq ($(filter win%,$(TARGET)),)
WIN_DIR := server/win

# the server's icon (RD_ICON_SERVER_APP in desktop/app/icon.h), drawn by the desktop apps' icongen
$(OUT)/remodroid-server.ico: $(DESK_OUT)/icongen
	$(DESK_OUT)/icongen $@ 6

$(OUT)/server.res: $(WIN_DIR)/server.rc $(WIN_DIR)/server.manifest $(OUT)/remodroid-server.ico
	$(WINDRES) -I$(WIN_DIR) -I$(OUT) -O coff -i $< -o $@

$(OUT)/tray.res: $(WIN_DIR)/server.rc $(WIN_DIR)/tray.manifest $(OUT)/remodroid-server.ico
	$(WINDRES) -DRD_TRAY -I$(WIN_DIR) -I$(OUT) -O coff -i $< -o $@

$(OUT)/RemoDroidServer.exe: $(WIN_DIR)/tray.c desktop/app/icon.c third_party/cjson/cJSON.c $(OUT)/tray.res
	$(CC) $(OPT) -std=gnu11 -municode -Wl,--subsystem,windows -Idesktop/app -Ithird_party/cjson $(WARN) -Wno-format-nonliteral \
	    -o $@ $^ -lcomctl32 -lshell32 -luser32 -lgdi32 -ladvapi32
endif
