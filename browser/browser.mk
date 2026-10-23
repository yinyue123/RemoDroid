# The remote browser (browser/remote-browser-plan.md), included from desktop/desktop.mk. The agent's own
# parts (browser/agent: the inputtest touch screen, Rockchip MPP) are built into libremodroid.so there;
# these are the programs around it, for Linux:
#
#   make browser                  rd-shell: the controller bar, window manager and browser supervisor
#   make browser-mpptest          the MPP encoder on its own (run on the OECT; MPP_INC=…/mpp/inc checks the
#                                 copied API against MPP's real headers)
#   make browser-test             the touch backend against a real Xorg (browser/test/touch_test.sh)

BROWSER_OUT := $(OUT)/browser
BROWSER_CFLAGS := $(OPT) $(ARCHFLAGS) -std=gnu11 -D_GNU_SOURCE $(DESK_WARN)

$(BROWSER_OUT)/rd-shell: browser/rd-shell/rd-shell.c
	@mkdir -p $(@D)
	$(CC) $(BROWSER_CFLAGS) $$(pkg-config --cflags xft) -o $@ $< $$(pkg-config --libs xft fontconfig) -lX11 -lXtst

$(BROWSER_OUT)/mpptest: browser/test/mpptest.c browser/agent/video_mpp.c browser/agent/mpp_api.h browser/agent/mppenc.h
	@mkdir -p $(@D)
	$(CC) $(BROWSER_CFLAGS) $(if $(MPP_INC),-DHAVE_MPP_HEADERS -I$(MPP_INC)) -Ibrowser/agent -Idesktop/core -Icommon \
	    -Ithird_party/cjson -Ithird_party/libev -Ithird_party/inih -o $@ browser/test/mpptest.c \
	    browser/agent/video_mpp.c -ldl -lpthread

browser: $(BROWSER_OUT)/rd-shell
browser-mpptest: $(BROWSER_OUT)/mpptest
browser-test:
	sh browser/test/touch_test.sh $(CC)

.PHONY: browser browser-mpptest browser-test
