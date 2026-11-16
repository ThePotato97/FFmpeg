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

#ifndef AVCODEC_MEDIACODEC_DOVI_H
#define AVCODEC_MEDIACODEC_DOVI_H

#include <stdint.h>

#include "libavutil/frame.h"
#include "avcodec.h"
#include "dovi_rpu.h"
#include "h2645_parse.h"

/* Packets whose decoded frame has not come out yet. Frames a decoder drops
 * leave their entry behind until a later frame overtakes it, so this only
 * needs to cover the decoder's reorder depth plus some slack. */
#define MEDIACODEC_DOVI_MAX_PENDING 64

typedef struct MediaCodecDoviPending {
    int64_t pts;           ///< the pts the frame will carry, in pkt_timebase
    AVFrameSideData **sd;  ///< side data to move onto that frame
    int nb_sd;
} MediaCodecDoviPending;

/**
 * MediaCodec decodes inside Android, so FFmpeg's HEVC decoder never sees the
 * Dolby Vision RPU NAL units and the frames carry none of their metadata.
 * This parses each packet's RPU in decode order, as the HEVC decoder does,
 * and attaches the result to the frame that comes out with the packet's pts.
 */
typedef struct MediaCodecDoviContext {
    DOVIContext dovi;
    H2645Packet h2645;
    MediaCodecDoviPending pending[MEDIACODEC_DOVI_MAX_PENDING];
    int nb_pending;
    int enabled;
} MediaCodecDoviContext;

/** Enables parsing when the stream signals Dolby Vision or a packet carries an RPU. */
void ff_mediacodec_dovi_init(MediaCodecDoviContext *s, AVCodecContext *avctx);

/** Parses the RPU of an Annex B access unit and queues its metadata under the pts its frame will have. */
int ff_mediacodec_dovi_packet(MediaCodecDoviContext *s, AVCodecContext *avctx,
                              const AVPacket *pkt);

/** Attaches the queued metadata of the frame's pts, and drops entries of earlier pts. */
int ff_mediacodec_dovi_frame(MediaCodecDoviContext *s, AVFrame *frame);

void ff_mediacodec_dovi_flush(MediaCodecDoviContext *s);
void ff_mediacodec_dovi_uninit(MediaCodecDoviContext *s);

#endif /* AVCODEC_MEDIACODEC_DOVI_H */
