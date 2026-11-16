/*
 * Dolby Vision RPU metadata for the Android MediaCodec HEVC decoder
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/dovi_meta.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"

#include "decode.h"
#include "hevc/hevc.h"
#include "mediacodec_dovi.h"

static void pending_free(MediaCodecDoviPending *p)
{
    av_frame_side_data_free(&p->sd, &p->nb_sd);
}

static void pending_remove(MediaCodecDoviContext *s, int index)
{
    pending_free(&s->pending[index]);
    memmove(s->pending + index, s->pending + index + 1,
            (s->nb_pending - index - 1) * sizeof(*s->pending));
    s->nb_pending--;
}

static void pending_clear(MediaCodecDoviContext *s)
{
    while (s->nb_pending)
        pending_remove(s, s->nb_pending - 1);
}

/* The pts MediaCodec's frame will carry: ff_mediacodec_dec_send() queues the
 * packet's pts in microseconds (0 when unset), and the frame's comes back
 * rescaled to pkt_timebase. */
static int64_t frame_pts(const AVCodecContext *avctx, int64_t pts)
{
    int timebase = avctx->pkt_timebase.num && avctx->pkt_timebase.den;
    if (pts == AV_NOPTS_VALUE)
        pts = 0;
    if (pts && timebase)
        pts = av_rescale_q(pts, avctx->pkt_timebase, AV_TIME_BASE_Q);
    return timebase ? av_rescale_q(pts, AV_TIME_BASE_Q, avctx->pkt_timebase) : pts;
}

void ff_mediacodec_dovi_init(MediaCodecDoviContext *s, AVCodecContext *avctx)
{
    const AVPacketSideData *sd;

    s->dovi.logctx = avctx;
    sd = ff_get_coded_side_data(avctx, AV_PKT_DATA_DOVI_CONF);
    if (sd && sd->size >= sizeof(s->dovi.cfg)) {
        s->dovi.cfg = *(const AVDOVIDecoderConfigurationRecord *) sd->data;
        s->enabled = 1;
    }
}

int ff_mediacodec_dovi_packet(MediaCodecDoviContext *s, AVCodecContext *avctx,
                              const AVPacket *pkt)
{
    const H2645NAL *rpu = NULL;
    MediaCodecDoviPending *p;
    AVFrame *tmp;
    AVBufferRef *raw;
    size_t sd_size;
    const uint8_t *sd;
    int ret;

    if (!pkt->size || avctx->codec_id != AV_CODEC_ID_HEVC)
        return 0;

    sd = av_packet_get_side_data(pkt, AV_PKT_DATA_DOVI_CONF, &sd_size);
    if (sd && sd_size >= sizeof(s->dovi.cfg)) {
        s->dovi.cfg = *(const AVDOVIDecoderConfigurationRecord *) sd;
        s->enabled = 1;
    }

    ret = ff_h2645_packet_split(&s->h2645, pkt->data, pkt->size, avctx, 0,
                                AV_CODEC_ID_HEVC, H2645_FLAG_SMALL_PADDING);
    if (ret < 0)
        return 0; /* MediaCodec still gets the packet; it just carries no metadata */

    /* As the HEVC decoder: the last base-layer RPU of the access unit. */
    for (int i = s->h2645.nb_nals - 1; i > 0; i--) {
        const H2645NAL *nal = &s->h2645.nals[i];
        if (nal->type == HEVC_NAL_UNSPEC62 && nal->size > 2 &&
            !nal->nuh_layer_id && !nal->temporal_id) {
            rpu = nal;
            break;
        }
    }
    if (!rpu)
        return 0;
    s->enabled = 1;

    ret = ff_dovi_rpu_parse(&s->dovi, rpu->data + 2, rpu->size - 2,
                            avctx->err_recognition);
    if (ret < 0) {
        av_log(avctx, AV_LOG_WARNING, "Error parsing DOVI NAL unit.\n");
        return 0;
    }

    /* The metadata as of this access unit, in decode order; later RPUs may
     * reference or replace the context's state before this frame comes out. */
    tmp = av_frame_alloc();
    if (!tmp)
        return AVERROR(ENOMEM);
    raw = av_buffer_alloc(rpu->raw_size - 2);
    if (!raw) {
        av_frame_free(&tmp);
        return AVERROR(ENOMEM);
    }
    memcpy(raw->data, rpu->raw_data + 2, rpu->raw_size - 2);
    if (!av_frame_new_side_data_from_buf(tmp, AV_FRAME_DATA_DOVI_RPU_BUFFER, raw)) {
        av_buffer_unref(&raw);
        av_frame_free(&tmp);
        return AVERROR(ENOMEM);
    }
    ret = ff_dovi_attach_side_data(&s->dovi, tmp);
    if (ret < 0) {
        av_frame_free(&tmp);
        return ret;
    }

    if (s->nb_pending == MEDIACODEC_DOVI_MAX_PENDING)
        pending_remove(s, 0);
    p = &s->pending[s->nb_pending++];
    *p = (MediaCodecDoviPending) { .pts = frame_pts(avctx, pkt->pts) };
    for (int i = 0; i < tmp->nb_side_data; i++) {
        ret = av_frame_side_data_clone(&p->sd, &p->nb_sd, tmp->side_data[i], 0);
        if (ret < 0)
            break;
    }
    av_frame_free(&tmp);
    if (ret < 0) {
        pending_free(p);
        s->nb_pending--;
        return ret;
    }
    return 0;
}

int ff_mediacodec_dovi_frame(MediaCodecDoviContext *s, AVFrame *frame)
{
    int ret = 0;

    if (!s->enabled)
        return 0;

    for (int i = 0; i < s->nb_pending; i++) {
        MediaCodecDoviPending *p = &s->pending[i];
        if (p->pts != frame->pts)
            continue;
        for (int j = 0; j < p->nb_sd && ret >= 0; j++) {
            AVFrameSideData *sd = p->sd[j];
            AVBufferRef *buf = av_buffer_ref(sd->buf);
            if (!buf) {
                ret = AVERROR(ENOMEM);
            } else if (!av_frame_new_side_data_from_buf(frame, sd->type, buf)) {
                av_buffer_unref(&buf);
                ret = AVERROR(ENOMEM);
            }
        }
        pending_remove(s, i);
        break;
    }

    /* Frames come out in presentation order, so an entry of an earlier pts
     * (in any position: entries are in decode order) is a frame the decoder
     * dropped. */
    for (int i = s->nb_pending - 1; i >= 0; i--)
        if (s->pending[i].pts < frame->pts)
            pending_remove(s, i);
    return ret;
}

void ff_mediacodec_dovi_flush(MediaCodecDoviContext *s)
{
    pending_clear(s);
    ff_dovi_ctx_flush(&s->dovi);
}

void ff_mediacodec_dovi_uninit(MediaCodecDoviContext *s)
{
    pending_clear(s);
    ff_dovi_ctx_unref(&s->dovi);
    ff_h2645_packet_uninit(&s->h2645);
}
