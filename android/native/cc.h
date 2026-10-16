/* Send-side congestion control (a compact GCC: draft-ietf-rmcat-gcc):
 * a delay-gradient trendline over transport-cc feedback plus a loss-based
 * bound and REMB. The result drives the encoder bitrate ("Auto" quality). */
#ifndef RTC_CC_H
#define RTC_CC_H

#include <stddef.h>
#include <stdint.h>

typedef struct cc cc_t;

cc_t *cc_new(int min_kbps, int start_kbps, int max_kbps);
void  cc_free(cc_t *c);
void  cc_set_limits(cc_t *c, int min_kbps, int max_kbps);
void  cc_on_sent(cc_t *c, uint16_t twcc_seq, size_t bytes, double now);
/* FCI of an RTPFB FMT 15 (transport-cc) feedback packet */
void  cc_on_twcc(cc_t *c, const uint8_t *fci, size_t n, double now);
void  cc_on_loss(cc_t *c, double fraction_lost, double now);
void  cc_on_remb(cc_t *c, double bps, double now);
int   cc_target_kbps(const cc_t *c);
int   cc_acked_kbps(const cc_t *c);
const char *cc_state(const cc_t *c); /* "increase" / "hold" / "decrease" */

#endif
