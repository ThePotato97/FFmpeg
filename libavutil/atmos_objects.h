/*
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

#ifndef AVUTIL_ATMOS_OBJECTS_H
#define AVUTIL_ATMOS_OBJECTS_H

#include <stdint.h>

/**
 * What one channel of a frame carrying AV_FRAME_DATA_ATMOS_OBJECTS is, as Windows' spatial audio
 * takes it (ISpatialAudioObjectRenderStream).
 */
typedef struct AVAtmosObject {
    /**
     * Windows' AudioObjectType: 0 unused (the channel is silent), 1 dynamic (positioned by x/y/z),
     * else one static bed channel's bit (FrontLeft 0x2 .. TopBackRight 0x1000).
     */
    uint32_t object_type;
    /** Windows' position for a dynamic object: within |x| <= 0.5, |y| <= 1, |z| <= 0.5. */
    float x, y, z;
    /** 0 when the object is inactive (its samples are silent). */
    uint32_t active;
} AVAtmosObject;

#endif /* AVUTIL_ATMOS_OBJECTS_H */
