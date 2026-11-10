# The remote browser (browser/remote-browser-plan.md), included from desktop/desktop.mk. The agent's own
# parts (browser/agent: the inputtest touch screen, Rockchip MPP) are built into libremodroid.so there;
# these are the programs around it, for Linux:
#
#   make browser                  rd-shell: the controller bar, window manager and browser supervisor;
#                                 rd-mirror: Page mode (the page's elements instead of a picture)
#   make browser-mpptest          the MPP encoder on its own (run on the OECT; MPP_INC=…/mpp/inc checks the
#                                 copied API against MPP's real headers)
#   make browser-test             the touch backend against a real Xorg (browser/test/touch_test.sh)

BROWSER_OUT := $(OUT)/browser
BROWSER_CFLAGS := $(OPT) $(ARCHFLAGS) -std=gnu11 -D_GNU_SOURCE $(DESK_WARN)

$(BROWSER_OUT)/rd-shell: browser/rd-shell/rd-shell.c third_party/cjson/cJSON.c
	@mkdir -p $(@D)
	$(CC) $(BROWSER_CFLAGS) -Ithird_party/cjson $$(pkg-config --cflags xft) -o $@ $^ $$(pkg-config --libs xft fontconfig) \
	    -lX11 -lXtst -lXrandr -lm

# Page mode (browser/dom-mirror.md): recorder.js goes into the program as a string
$(BROWSER_OUT)/recorder.h: browser/rd-mirror/recorder.js
	@mkdir -p $(@D)
	(echo 'static const char RECORDER[] = {'; od -An -v -tx1 $< | sed 's/ \([0-9a-f][0-9a-f]\)/0x\1,/g'; echo '0 };') > $@

$(BROWSER_OUT)/rd-mirror: browser/rd-mirror/rd-mirror.c browser/rd-mirror/picture.c browser/rd-mirror/picture.h \
                          third_party/cjson/cJSON.c $(BROWSER_OUT)/recorder.h
	$(CC) $(BROWSER_CFLAGS) -Ithird_party/cjson -I$(BROWSER_OUT) -o $@ browser/rd-mirror/rd-mirror.c browser/rd-mirror/picture.c \
	    third_party/cjson/cJSON.c -lz -lwebp -lpng -ljpeg -lpthread

$(BROWSER_OUT)/mpptest: browser/test/mpptest.c browser/agent/video_mpp.c browser/agent/mpp_api.h browser/agent/mppenc.h
	@mkdir -p $(@D)
	$(CC) $(BROWSER_CFLAGS) $(if $(MPP_INC),-DHAVE_MPP_HEADERS -I$(MPP_INC)) -Ibrowser/agent -Idesktop/core -Icommon \
	    -Ithird_party/cjson -Ithird_party/libev -Ithird_party/inih -o $@ browser/test/mpptest.c \
	    browser/agent/video_mpp.c -ldl -lpthread

browser: $(BROWSER_OUT)/rd-shell $(BROWSER_OUT)/rd-mirror
browser-mpptest: $(BROWSER_OUT)/mpptest
browser-test:
	sh browser/test/touch_test.sh $(CC)

.PHONY: browser browser-mpptest browser-test
