# Phone-side WebRTC library (android/native), included from the top Makefile.
# Host build for tests:  make rtc-test
RTC_DIR  := android/native
RTC_OUT  := $(OUT)/rtc
RTC_SRC  := $(filter-out $(RTC_DIR)/jni.c,$(wildcard $(RTC_DIR)/*.c))
RTC_INCS := -I$(RTC_DIR) -Icommon -Ithird_party/libev -Ithird_party/cjson -Ithird_party/mbedtls/include \
            -Ithird_party/libsrtp/include -Ithird_party/libsrtp/crypto/include -Ithird_party/usrsctp/usrsctplib
RTC_DEFS := -D_GNU_SOURCE $(EV_DEFS) -DMBEDTLS_CONFIG_FILE='"mbedtls_rtc_config.h"'
RTC_CFLAGS := $(OPT) -std=gnu11 -fno-strict-aliasing -ffunction-sections -fdata-sections -fPIC $(RTC_INCS) $(RTC_DEFS)

SRTP_SRC := third_party/libsrtp/srtp/srtp.c $(wildcard third_party/libsrtp/crypto/*/*.c)
SCTP_SRC := $(wildcard third_party/usrsctp/usrsctplib/*.c) $(wildcard third_party/usrsctp/usrsctplib/netinet/*.c) \
            $(wildcard third_party/usrsctp/usrsctplib/netinet6/*.c)
SCTP_DEFS := -D__Userspace__ -DSCTP_SIMPLE_ALLOCATOR -DSCTP_PROCESS_LEVEL_LOCKS -DHAVE_SYS_QUEUE_H \
             -DHAVE_STDATOMIC_H -Wno-address-of-packed-member
RTC_TP_SRC := third_party/libev/ev.c third_party/cjson/cJSON.c $(MBED_SRC) $(SRTP_SRC) $(SCTP_SRC)
RTC_COMMON := common/util.c common/tunnel.c common/msg.c common/stun.c

RTC_OBJ    := $(RTC_SRC:%.c=$(RTC_OUT)/%.o) $(RTC_COMMON:%.c=$(RTC_OUT)/%.o)
RTC_TP_OBJ := $(RTC_TP_SRC:%.c=$(RTC_OUT)/%.o)

$(RTC_OUT)/third_party/libsrtp/%.o: third_party/libsrtp/%.c
	@mkdir -p $(@D)
	$(CC) $(RTC_CFLAGS) -DHAVE_CONFIG_H -Ithird_party/libsrtp -w -c $< -o $@

$(RTC_OUT)/third_party/usrsctp/%.o: third_party/usrsctp/%.c
	@mkdir -p $(@D)
	$(CC) $(RTC_CFLAGS) $(SCTP_DEFS) -w -c $< -o $@

$(RTC_OUT)/third_party/%.o: third_party/%.c
	@mkdir -p $(@D)
	$(CC) $(RTC_CFLAGS) -w -c $< -o $@

$(RTC_OUT)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(RTC_CFLAGS) $(WARN) -MMD -MP -c $< -o $@

$(RTC_OUT)/librtc.a: $(RTC_OBJ) $(RTC_TP_OBJ)
	$(AR) rcs $@ $^

$(RTC_OUT)/rtc_test: $(RTC_DIR)/test/rtc_test.c $(RTC_OUT)/librtc.a
	$(CC) $(RTC_CFLAGS) $(WARN) $(LDFLAGS) -o $@ $^ -lm -lpthread

rtc-test: $(RTC_OUT)/rtc_test

-include $(RTC_OBJ:.o=.d)
.PHONY: rtc-test

$(RTC_OUT)/rtp_dump: $(RTC_DIR)/test/rtp_dump.c $(RTC_OUT)/librtc.a
	$(CC) $(RTC_CFLAGS) $(WARN) $(LDFLAGS) -o $@ $^ -lm -lpthread

rtc-tests: $(RTC_OUT)/rtc_test $(RTC_OUT)/rtp_dump
.PHONY: rtc-tests
