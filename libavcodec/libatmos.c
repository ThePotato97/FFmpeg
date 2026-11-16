/*
 * Dolby Atmos objects for spatial audio, through atmos-render
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

/*
 * TrueHD and E-AC-3 (JOC) decoded to the objects Windows' own DD+ decoder hands Dolby Atmos for
 * Headphones, for a player that sends them to Windows' spatial audio: 256-sample frames of
 * ATMOS_OBJECTS_CHANNELS unnamed planar channels, each with AV_FRAME_DATA_ATMOS_OBJECTS saying
 * what every channel is (a static bed channel, a positioned dynamic object, or unused). These are
 * not speaker channels: a player that doesn't read the side data must not use these decoders.
 *
 * For E-AC-3 FFmpeg's eac3 decoder decodes the core, set up as Windows' decoder's RF mode, and
 * atmos-render rebuilds the objects from it and the bitstream's EMDF (JOC, OAMD).
 */

#include <atmos_objects.h>
#include <string.h>

#include "libavutil/atmos_objects.h"
#include "libavutil/channel_layout.h"
#include "libavutil/dict.h"
#include "libavutil/frame.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"

#define RATE 48000

_Static_assert(sizeof(AVAtmosObject) == sizeof(AtmosObject), "atmos-render's AtmosObject is AVAtmosObject");

typedef struct LibAtmosContext {
    AtmosObjects *objects;
    int codec;
    AVCodecContext *core;
    AVFrame *core_frame;
    AVPacket *pkt;
    float *interleaved;
    int interleaved_size;
    float planes[ATMOS_OBJECTS_CHANNELS][ATMOS_OBJECTS_BLOCK];
    int64_t origin;
    int has_origin;
    int eof;
} LibAtmosContext;

static int open_core(AVCodecContext *avctx, LibAtmosContext *s)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_EAC3);
    AVDictionary *opts = NULL;
    int ret;

    if (!codec)
        return AVERROR_DECODER_NOT_FOUND;
    s->core = avcodec_alloc_context3(codec);
    s->core_frame = av_frame_alloc();
    if (!s->core || !s->core_frame)
        return AVERROR(ENOMEM);
    s->core->request_sample_fmt = AV_SAMPLE_FMT_FLTP;
    s->core->pkt_timebase = avctx->pkt_timebase;
    /* Windows' DD+ decoder's RF mode, as atmos-render's Loudness::Windows expects */
    av_dict_set(&opts, "drc_scale", "1", 0);
    av_dict_set(&opts, "heavy_compr", "1", 0);
    av_dict_set(&opts, "target_level", "-31", 0);
    ret = avcodec_open2(s->core, codec, &opts);
    av_dict_free(&opts);
    return ret;
}

static av_cold int libatmos_init(AVCodecContext *avctx)
{
    LibAtmosContext *s = avctx->priv_data;
    int ret;

    if (avctx->sample_rate && avctx->sample_rate != RATE) {
        av_log(avctx, AV_LOG_ERROR, "Only %d Hz is supported, not %d\n", RATE, avctx->sample_rate);
        return AVERROR(ENOSYS);
    }
    s->codec = avctx->codec_id == AV_CODEC_ID_TRUEHD ? ATMOS_OBJECTS_TRUEHD : ATMOS_OBJECTS_EAC3;
    s->objects = atmos_objects_create(s->codec);
    s->pkt = av_packet_alloc();
    if (!s->objects || !s->pkt)
        return AVERROR(ENOMEM);
    if (s->codec == ATMOS_OBJECTS_EAC3 && (ret = open_core(avctx, s)) < 0)
        return ret;

    avctx->sample_rate = RATE;
    avctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_uninit(&avctx->ch_layout);
    avctx->ch_layout.order = AV_CHANNEL_ORDER_UNSPEC;
    avctx->ch_layout.nb_channels = ATMOS_OBJECTS_CHANNELS;
    return 0;
}

static av_cold int libatmos_close(AVCodecContext *avctx)
{
    LibAtmosContext *s = avctx->priv_data;

    atmos_objects_destroy(s->objects);
    s->objects = NULL;
    avcodec_free_context(&s->core);
    av_frame_free(&s->core_frame);
    av_packet_free(&s->pkt);
    av_freep(&s->interleaved);
    return 0;
}

/* The core's decode so far, interleaved, into atmos-render. */
static int push_core(AVCodecContext *avctx, LibAtmosContext *s)
{
    int ret;

    while ((ret = avcodec_receive_frame(s->core, s->core_frame)) >= 0) {
        AVFrame *f = s->core_frame;
        int channels = f->ch_layout.nb_channels;
        int size = f->nb_samples * channels;
        uint64_t mask = f->ch_layout.order == AV_CHANNEL_ORDER_NATIVE ? f->ch_layout.u.mask : 0;

        if (f->format != AV_SAMPLE_FMT_FLTP || !mask || mask >> 32) {
            av_log(avctx, AV_LOG_ERROR, "Unexpected E-AC-3 core output\n");
            av_frame_unref(f);
            return AVERROR_BUG;
        }
        if (size > s->interleaved_size) {
            av_freep(&s->interleaved);
            s->interleaved = av_malloc_array(size, sizeof(float));
            if (!s->interleaved)
                return AVERROR(ENOMEM);
            s->interleaved_size = size;
        }
        for (int c = 0; c < channels; c++) {
            const float *plane = (const float *)f->extended_data[c];
            for (int n = 0; n < f->nb_samples; n++)
                s->interleaved[n * channels + c] = plane[n];
        }
        atmos_objects_push_pcm(s->objects, channels, (uint32_t)mask, s->interleaved, f->nb_samples);
        av_frame_unref(f);
    }
    return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF ? 0 : ret;
}

static int take_packet(AVCodecContext *avctx, LibAtmosContext *s)
{
    int ret = ff_decode_get_packet(avctx, s->pkt);

    if (ret == AVERROR_EOF) {
        if (s->core) {
            avcodec_send_packet(s->core, NULL);
            if ((ret = push_core(avctx, s)) < 0)
                return ret;
        }
        atmos_objects_end(s->objects);
        s->eof = 1;
        return 0;
    }
    if (ret < 0)
        return ret;

    if (!s->has_origin) {
        s->origin = s->pkt->pts != AV_NOPTS_VALUE ? s->pkt->pts : 0;
        s->has_origin = 1;
    }
    /* The bitstream first, for its frames' EMDF, then the core's PCM, which atmos-render lines up
     * with them (as atmos-render's decoder package does). */
    atmos_objects_push(s->objects, s->pkt->data, s->pkt->size);
    if (s->core) {
        ret = avcodec_send_packet(s->core, s->pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN))
            av_log(avctx, AV_LOG_WARNING, "E-AC-3 core decode failed\n");
        ret = push_core(avctx, s);
    }
    av_packet_unref(s->pkt);
    return ret;
}

static int libatmos_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    LibAtmosContext *s = avctx->priv_data;
    AVAtmosObject objects[ATMOS_OBJECTS_CHANNELS];
    float *planes[ATMOS_OBJECTS_CHANNELS];
    uint64_t start;
    int ret;

    for (int c = 0; c < ATMOS_OBJECTS_CHANNELS; c++)
        planes[c] = s->planes[c];

    for (;;) {
        if (atmos_objects_receive(s->objects, planes, (AtmosObject *)objects, &start))
            break;
        if (s->eof)
            return AVERROR_EOF;
        if ((ret = take_packet(avctx, s)) < 0)
            return ret;
    }

    frame->nb_samples = ATMOS_OBJECTS_BLOCK;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;
    for (int c = 0; c < ATMOS_OBJECTS_CHANNELS; c++)
        memcpy(frame->extended_data[c], s->planes[c], ATMOS_OBJECTS_BLOCK * sizeof(float));
    frame->pts = s->origin;
    if (avctx->pkt_timebase.num && avctx->pkt_timebase.den)
        frame->pts += av_rescale_q(start, (AVRational){ 1, RATE }, avctx->pkt_timebase);
    if (!av_frame_new_side_data_from_buf(frame, AV_FRAME_DATA_ATMOS_OBJECTS,
                                         av_buffer_alloc(sizeof(objects))))
        return AVERROR(ENOMEM);
    memcpy(av_frame_get_side_data(frame, AV_FRAME_DATA_ATMOS_OBJECTS)->data, objects, sizeof(objects));
    return 0;
}

static void libatmos_flush(AVCodecContext *avctx)
{
    LibAtmosContext *s = avctx->priv_data;

    atmos_objects_destroy(s->objects);
    s->objects = atmos_objects_create(s->codec);
    if (s->core)
        avcodec_flush_buffers(s->core);
    s->has_origin = 0;
    s->eof = 0;
}

#define LIBATMOS_DECODER(short_, long_, id_)                                     \
const FFCodec ff_libatmos_##short_##_decoder = {                                    \
    .p.name         = "libatmos_" #short_,                                          \
    CODEC_LONG_NAME(long_ " to Dolby Atmos objects (atmos-render)"),          \
    .p.type         = AVMEDIA_TYPE_AUDIO,                                         \
    .p.id           = id_,                                                         \
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY,                      \
    .p.wrapper_name = "libatmos",                                                 \
    .priv_data_size = sizeof(LibAtmosContext),                                    \
    .init           = libatmos_init,                                              \
    .close          = libatmos_close,                                             \
    FF_CODEC_RECEIVE_FRAME_CB(libatmos_receive_frame),                            \
    .flush          = libatmos_flush,                                             \
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,                                  \
    CODEC_SAMPLEFMTS(AV_SAMPLE_FMT_FLTP),                                         \
};

LIBATMOS_DECODER(truehd, "TrueHD", AV_CODEC_ID_TRUEHD)
LIBATMOS_DECODER(eac3, "E-AC-3", AV_CODEC_ID_EAC3)
