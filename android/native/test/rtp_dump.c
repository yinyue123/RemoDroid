/* Packetizes a clip with the real RTP sender and writes the plaintext packets,
 * so tests can depacketize them independently.
 *   rtp_dump h265|h264 in.frames out.rtp      (out: u16 BE length + packet, repeated) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rd.h"
#include "rtp.h"
#include "sdp.h"

static FILE *out;
static int npk;

static void on_send(void *ud, const uint8_t *p, size_t n, int is_rtcp)
{
    if (is_rtcp)
        return;
    uint8_t h[2];
    rd_put_be16(h, (uint16_t)n);
    fwrite(h, 1, 2, out);
    fwrite(p, 1, n, out);
    npk++;
}

int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    int codec = !strcmp(argv[1], "h265") ? CODEC_H265 : CODEC_H264_BASELINE;
    FILE *in = fopen(argv[2], "rb");
    out = fopen(argv[3], "wb");
    if (!in || !out)
        return 1;
    rtp_conf_t c = { .codec = codec, .pt = codec == CODEC_H265 ? PT_H265 : PT_H264B, .ssrc = 0x1234,
                     .ext_abs = EXT_ABS_SEND_TIME, .ext_twcc = EXT_TWCC, .ext_playout = EXT_PLAYOUT_DELAY };
    rtp_t *r = rtp_new(c, NULL, (rtp_cb_t){ on_send, NULL }, NULL);
    uint8_t h[5];
    int frames = 0;
    while (fread(h, 1, 5, in) == 5) {
        size_t len = rd_get_be32(h);
        uint8_t *d = malloc(len);
        if (fread(d, 1, len, in) != len)
            return 1;
        rtp_send_frame(r, d, len, (uint32_t)(frames * 3000), h[4], frames / 30.0);
        free(d);
        frames++;
    }
    fclose(out);
    printf("{\"frames\":%d,\"packets\":%d}\n", frames, npk);
    rtp_free(r);
    return 0;
}
