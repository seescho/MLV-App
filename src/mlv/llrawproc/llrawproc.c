﻿/*
 * Copyright (C) 2017 Bouncyball
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "llrawproc.h"
#include "pixelproc.h"
#include "stripes.h"
#include "patternnoise.h"
#include "dualiso.h"
#include "hist.h"
#include "darkframe.h"
#include "../../processing/raw_processing.h"

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))
#define COERCE(x,lo,hi) MAX(MIN((x),(hi)),(lo))
#define ABS(a) ((a) > 0 ? (a) : -(a))

/* this is DNG feature only */
static void deflicker(mlvObject_t * video, uint16_t * raw_image_buff, size_t raw_image_size)
{
    uint16_t black = video->RAWI.raw_info.black_level;
    uint16_t white = (1 << video->RAWI.raw_info.bits_per_pixel) - 1;

    struct histogram * hist = hist_create(white);
    hist_add(hist, raw_image_buff + 1, (uint32_t)((raw_image_size - 1) / 2), 1);
    uint16_t median = hist_median(hist);
    double correction = log2((double) (video->llrawproc->deflicker_target - black) / (median - black));
    video->RAWI.raw_info.exposure_bias[0] = correction * 10000;
    video->RAWI.raw_info.exposure_bias[1] = 10000;
}

/* convert uncompressed 10/12bit raw data to 14bit for subsequent processing */
static void make_14bit(uint16_t * raw_image_buff, size_t raw_image_size, struct raw_info * raw_info)
{
    uint32_t pixel_count = raw_image_size / 2;
    int bits_shift = 14 - raw_info->bits_per_pixel;
    raw_info->black_level <<= bits_shift;
    raw_info->white_level <<= bits_shift;
    raw_info->bits_per_pixel = 14;
    raw_info->frame_size = raw_info->width * raw_info->height * 14 / 8;

    #pragma omp parallel for
    for(uint32_t i = 0; i < pixel_count; ++i)
    {
        raw_image_buff[i] <<= bits_shift;
    }
}

/* undo 14bit conversion to initial bit depth with rounding error minimizing */
static void undo_14bit(uint16_t * raw_image_buff, size_t raw_image_size, uint32_t bpp)
{
    uint32_t pixel_count = raw_image_size / 2;
    int bits_shift = 14 - bpp;
    /* calculate rounding number to be added to the raw value before shifting right to minimize rounding error */
    uint32_t rounding_number = (uint32_t)pow(2, bits_shift - 1);

    #pragma omp parallel for
    for(uint32_t i = 0; i < pixel_count; ++i)
    {
        raw_image_buff[i] = (raw_image_buff[i] + rounding_number) >> bits_shift;
    }
}

/* rescale restricted to imaginary 10-12bit levels of lossless raw data to about real 14bit range */
static void scale_restricted_range(struct raw_info * raw_info, uint16_t * image_data)
{
    uint32_t pixel_count = raw_info->width * raw_info->height;
    /* find min and max level values in the currecnt raw frame */
    int32_t min_level = image_data[0];
    int32_t max_level = image_data[0];
    for(uint32_t i = 1; i < pixel_count; ++i)
    {
        if(image_data[i] < min_level) min_level = image_data[i];
        if(image_data[i] > max_level) max_level = image_data[i];
    }
#ifndef STDOUT_SILENT
    printf("min_level = %d, max_level = %d\n", min_level, max_level);
#endif
    raw_info->black_level = MAX(min_level, raw_info->black_level);
    raw_info->white_level = MAX(max_level, raw_info->white_level);

    int32_t scaled_white_level = 16200;
    double scale_ratio = (double)(scaled_white_level - raw_info->black_level) / (double)(raw_info->white_level - raw_info->black_level);
    raw_info->white_level = scaled_white_level;

#pragma omp parallel for
    for(uint32_t i = 0; i < pixel_count; ++i)
    {
        image_data[i] = MIN( (uint16_t)((double)((image_data[i] - raw_info->black_level) * scale_ratio + raw_info->black_level) + 0.5), 16383);
    }
}

/* initialise low level raw processing struct */
llrawprocObject_t * initLLRawProcObject()
{
    llrawprocObject_t * llrawproc = calloc(1, sizeof(llrawprocObject_t));

    /* set defaults */
    llrawproc->vertical_stripes = 1;
    llrawproc->focus_pixels = 0;
    llrawproc->fpi_method = 0;
    llrawproc->bad_pixels = 1;
    llrawproc->bps_method = 0;
    llrawproc->bpi_method = 0;
    llrawproc->chroma_smooth = 0;
    llrawproc->pattern_noise = 0;
    llrawproc->deflicker_target = 0;
    llrawproc->fpm_status = 0;
    llrawproc->bpm_status = 0;
    llrawproc->compute_stripes = 0;
    llrawproc->first_time = 1;
    llrawproc->dual_iso = 0;
    llrawproc->diso_averaging = 0;
    llrawproc->diso_alias_map = 1;
    llrawproc->diso_frblending = 1;
    llrawproc->dark_frame = 0;

    llrawproc->dark_frame_filename = NULL;
    llrawproc->dark_frame_data = NULL;
    llrawproc->dark_frame_size = 0;

    llrawproc->raw2ev = NULL;
    llrawproc->ev2raw = NULL;

    llrawproc->focus_pixel_map.type = PIX_FOCUS;
    llrawproc->focus_pixel_map.pixels = NULL;
    llrawproc->bad_pixel_map.type = PIX_BAD;
    llrawproc->bad_pixel_map.pixels = NULL;

    return llrawproc;
}

void freeLLRawProcObject(mlvObject_t * video)
{
    df_free_filename(video);
    df_free(video);
    free_luts(video->llrawproc->raw2ev, video->llrawproc->ev2raw);
    free_pixel_maps(&(video->llrawproc->focus_pixel_map), &(video->llrawproc->bad_pixel_map));
    free(video->llrawproc);
}

/* all low level raw processing takes place here */
void applyLLRawProcObject(mlvObject_t * video, uint16_t * raw_image_buff, size_t raw_image_size)
{
    /* if 'fix_raw == false' skip raw processing alltogether */
    if(!video->llrawproc->fix_raw) return;

    /* subtruct dark frame if Ext or Int mode specified and df_init is successful */
    if (!df_init(video))
    {
#ifndef STDOUT_SILENT
        printf("Subtracting Dark Frame... ");
#endif
        df_subtract(video, raw_image_buff, raw_image_size);
#ifndef STDOUT_SILENT
        printf("Done\n\n");
#endif
    }

    /* make copy of 'RAWI.raw_info' struct for subsequent modification */
    struct raw_info raw_info = video->RAWI.raw_info;

    /* convert uncompressed 10/12bit raw data to 14bits for correct processing */
    if(video->RAWI.raw_info.bits_per_pixel < 14)
    {
        make_14bit(raw_image_buff, raw_image_size, &raw_info);
    }

    /* do one time stuff */
    if(video->llrawproc->first_time)
    {
        /* initialize dual iso black and white levels */
        llrpResetDngBWLevels(video);

        /* initialise LUTs */
        video->llrawproc->raw2ev = get_raw2ev(raw_info.black_level);
        video->llrawproc->ev2raw = get_ev2raw(raw_info.black_level);

        video->llrawproc->first_time = 0;
    }

    /* fix vertical stripes */
    if (video->llrawproc->vertical_stripes)
    {
        fix_vertical_stripes(&video->llrawproc->stripe_corrections,
                             raw_image_buff,
                             raw_info.black_level,
                             raw_info.white_level,
                             raw_info.frame_size,
                             video->RAWI.xRes,
                             video->RAWI.yRes,
                             video->llrawproc->vertical_stripes,
                             &video->llrawproc->compute_stripes);
    }

    /* fix focus pixels */
    if (video->llrawproc->focus_pixels && video->llrawproc->fpm_status < 3)
    {
        /* detect crop_rec mode */
        int crop_rec = (llrpDetectFocusDotFixMode(video) == 2) ? 1 : (video->llrawproc->focus_pixels == 2);
        /* if raw data is lossless set unified mode */
        int unified_mode = (video->MLVI.videoClass & MLV_VIDEO_CLASS_FLAG_LJ92) ? 5 : 0;
        fix_focus_pixels(&video->llrawproc->focus_pixel_map,
                         &video->llrawproc->fpm_status,
                         raw_image_buff,
                         video->IDNT.cameraModel,
                         video->RAWI.xRes,
                         video->RAWI.yRes,
                         video->VIDF.panPosX,
                         video->VIDF.panPosY,
                         raw_info.width,
                         raw_info.height,
                         crop_rec,
                         unified_mode,
                         video->llrawproc->fpi_method,
                         (video->llrawproc->dual_iso),
                         video->llrawproc->raw2ev,
                         video->llrawproc->ev2raw);
    }

    /* fix bad pixels */
    if (video->llrawproc->bad_pixels && video->llrawproc->bpm_status < 3)
    {
        fix_bad_pixels(&video->llrawproc->bad_pixel_map,
                       &video->llrawproc->bpm_status,
                       raw_image_buff,
                       video->IDNT.cameraModel,
                       video->RAWI.xRes,
                       video->RAWI.yRes,
                       video->VIDF.panPosX,
                       video->VIDF.panPosY,
                       raw_info.width,
                       raw_info.height,
                       raw_info.black_level,
                       (video->llrawproc->bad_pixels == 2),
                       video->llrawproc->bps_method,
                       video->llrawproc->bpi_method,
                       (video->llrawproc->dual_iso),
                       video->llrawproc->raw2ev,
                       video->llrawproc->ev2raw);
    }

    /* fix pattern noise */
    if (!video->llrawproc->diso_validity && video->llrawproc->pattern_noise)
    {
#ifndef STDOUT_SILENT
        printf("Fixing pattern noise... ");
#endif
        fix_pattern_noise((int16_t *)raw_image_buff, video->RAWI.xRes, video->RAWI.yRes, raw_info.white_level, 0);
#ifndef STDOUT_SILENT
        printf("Done\n\n");
#endif
    }

    /* if dual iso valid/forced and processing is turned on */
    if(video->llrawproc->diso_validity && video->llrawproc->dual_iso)
    {
        raw_info.width = video->RAWI.xRes;
        raw_info.height = video->RAWI.yRes;
        raw_info.pitch = video->RAWI.xRes;
        raw_info.active_area.x1 = 0;
        raw_info.active_area.y1 = 0;
        raw_info.active_area.x2 = raw_info.width;
        raw_info.active_area.y2 = raw_info.height;
        /* detect if lossless raw data is restricted to imaginary 8-12bit levels */
        int restricted_lossless = (video->MLVI.videoClass & MLV_VIDEO_CLASS_FLAG_LJ92) && raw_info.white_level < 15000;
        if(restricted_lossless)
        {
#ifndef STDOUT_SILENT
            printf("\nScaling raw data range...\n");
            printf("Raw_Black = %d, Raw_White = %d <= BEFORE SCALING\n", raw_info.black_level, raw_info.white_level);
#endif
            scale_restricted_range(&raw_info, raw_image_buff);
#ifndef STDOUT_SILENT
            printf("Raw_Black = %d, Raw_White = %d <= AFTER SCALING\n", raw_info.black_level, raw_info.white_level);
            printf("\nChanging processing B/W levels...\n");
            printf("Proc_Black = %d, Proc_White = %d <= BEFORE CHANGING\n", video->processing->black_level, video->processing->white_level);
#endif
            processingSetBlackAndWhiteLevel(video->processing, raw_info.black_level, raw_info.white_level, 14); // black and white levels are 14bit after scaling above
#ifndef STDOUT_SILENT
            printf("Proc_Black = %d, Proc_White = %d <= AFTER CHANGING\n", video->processing->black_level, video->processing->white_level);
#endif
        }

        /* dual iso processing */
        if (video->llrawproc->dual_iso == 1) // Full 20bit processing mode
        {
            diso_get_full20bit(raw_info,
                               raw_image_buff,
                               video->llrawproc->diso_averaging,
                               video->llrawproc->diso_alias_map,
                               video->llrawproc->diso_frblending,
                               video->llrawproc->chroma_smooth);

            /* for full20bit set diso levels and bit depth to 16 bit, needed for cDNG export */
            int bits_shift = 16 - raw_info.bits_per_pixel;
            video->llrawproc->dng_black_level = raw_info.black_level << bits_shift;
            video->llrawproc->dng_white_level = raw_info.white_level << bits_shift;
            video->llrawproc->dng_bit_depth = 16;
        }
        else if (video->llrawproc->dual_iso == 2) // Preview mode
        {
            diso_get_preview(raw_image_buff,
                             raw_info.width,
                             raw_info.height,
                             raw_info.black_level,
                             raw_info.white_level,
                             0); // dual iso check mode is off
        }
    }

    /* do chroma smoothing */
    if (video->llrawproc->chroma_smooth && video->llrawproc->dual_iso != 1) // do not smooth 20bit dualiso raw
    {
#ifndef STDOUT_SILENT
            printf("\nUsing chroma smooth method: '%dx%d'\n\n", video->llrawproc->chroma_smooth, video->llrawproc->chroma_smooth);
#endif
        chroma_smooth(video->llrawproc->chroma_smooth,
                      raw_image_buff,
                      video->RAWI.xRes,
                      video->RAWI.yRes,
                      raw_info.black_level,
                      raw_info.white_level,
                      video->llrawproc->raw2ev,
                      video->llrawproc->ev2raw);
    }

    /* undo 14bit conversion of uncompressed 10/12bit raw data, except when 20bit dual iso processing is active */
    if(video->RAWI.raw_info.bits_per_pixel < 14 && video->llrawproc->dual_iso != 1)
    {
        undo_14bit(raw_image_buff, raw_image_size, video->RAWI.raw_info.bits_per_pixel);
    }

    /* deflicker RAW data by changing 'tcBaselineExposure' tag in the exported DNG */
    if (video->llrawproc->deflicker_target)
    {
#ifndef STDOUT_SILENT
        printf("Per-frame exposure compensation: 'ON'\nDeflicker target: '%d'\n\n", video->llrawproc->deflicker_target);
#endif
        deflicker(video, raw_image_buff, raw_image_size);
    }

#ifndef STDOUT_SILENT
    printf("raw_image_buff[1000] = %u, Proc_Black = %d, Proc_White = %d, Raw_Black = %d, Raw_White = %d <= THE END OF LLRAWPROC\n", raw_image_buff[1000], video->processing->black_level, video->processing->white_level, video->RAWI.raw_info.black_level, video->RAWI.raw_info.white_level);
#endif
}

/* Detect focus dot fix mode according to RAWC block info (binning + skipping) and camera ID
   Return value 0 = off, 1 = On, 2 = CropRec */
int llrpDetectFocusDotFixMode(mlvObject_t * video)
{
    switch(video->IDNT.cameraModel)
    {
        case 0x80000331: // EOSM
        case 0x80000355: // EOSM2
        case 0x80000346: // 100D
        case 0x80000301: // 650D
        case 0x80000326: // 700D
            if(video->RAWC.blockType[0])
            {
                int sampling_x = video->RAWC.binning_x + video->RAWC.skipping_x;
                int sampling_y = video->RAWC.binning_y + video->RAWC.skipping_y;
                if( (video->RAWI.raw_info.height < 900) && !(sampling_y == 5 && sampling_x == 3) )
                {
                    return 2;
                }
            }
            return 1;

        default: // All other cameras
            return 0;
    }
}

/* LLRawProcObject variable handling */
int llrpGetFixRawMode(mlvObject_t * video)
{
    return video->llrawproc->fix_raw;
}

void llrpSetFixRawMode(mlvObject_t * video, int value)
{
    video->llrawproc->fix_raw = value;
}

int llrpGetVerticalStripeMode(mlvObject_t * video)
{
    return video->llrawproc->vertical_stripes;
}

void llrpSetVerticalStripeMode(mlvObject_t * video, int value)
{
    video->llrawproc->vertical_stripes = value;
}

void llrpComputeStripesOn(mlvObject_t * video)
{
    video->llrawproc->compute_stripes = 1;
}

int llrpGetFocusPixelMode(mlvObject_t * video)
{
    return video->llrawproc->focus_pixels;
}

void llrpSetFocusPixelMode(mlvObject_t * video, int value)
{
    video->llrawproc->focus_pixels = value;
}

int llrpGetFocusPixelInterpolationMethod(mlvObject_t * video)
{
    return video->llrawproc->fpi_method;
}

void llrpSetFocusPixelInterpolationMethod(mlvObject_t * video, int value)
{
    video->llrawproc->fpi_method = value;
}

int llrpGetBadPixelMode(mlvObject_t * video)
{
    return video->llrawproc->bad_pixels;
}

void llrpSetBadPixelMode(mlvObject_t * video, int value)
{
    video->llrawproc->bad_pixels = value;
}

int llrpGetBadPixelSearchMethod(mlvObject_t *video)
{
    return video->llrawproc->bps_method;
}

void llrpSetBadPixelSearchMethod(mlvObject_t * video, int value)
{
    video->llrawproc->bps_method = value;
}

int llrpGetBadPixelInterpolationMethod(mlvObject_t * video)
{
    return video->llrawproc->bpi_method;
}

void llrpSetBadPixelInterpolationMethod(mlvObject_t * video, int value)
{
    video->llrawproc->bpi_method = value;
}

int llrpGetChromaSmoothMode(mlvObject_t * video)
{
    return video->llrawproc->chroma_smooth;
}

void llrpSetChromaSmoothMode(mlvObject_t * video, int value)
{
    video->llrawproc->chroma_smooth = value;
}

int llrpGetPatternNoiseMode(mlvObject_t * video)
{
    return video->llrawproc->pattern_noise;
}

void llrpSetPatternNoiseMode(mlvObject_t * video, int value)
{
    video->llrawproc->pattern_noise = value;
}

int llrpGetDeflickerTarget(mlvObject_t * video)
{
    return video->llrawproc->deflicker_target;
}

void llrpSetDeflickerTarget(mlvObject_t * video, int value)
{
    video->llrawproc->deflicker_target = value;
}

int llrpGetDualIsoMode(mlvObject_t * video)
{
    return video->llrawproc->dual_iso;
}

void llrpSetDualIsoMode(mlvObject_t * video, int value)
{
    video->llrawproc->dual_iso = value;
}

int llrpGetDualIsoInterpolationMethod(mlvObject_t * video)
{
    return video->llrawproc->diso_averaging;
}

void llrpSetDualIsoInterpolationMethod(mlvObject_t * video, int value)
{
    video->llrawproc->diso_averaging = value;
}

int llrpGetDualIsoAliasMapMode(mlvObject_t * video)
{
    return video->llrawproc->diso_alias_map;
}

void llrpSetDualIsoAliasMapMode(mlvObject_t * video, int value)
{
    video->llrawproc->diso_alias_map = value;
}

int llrpGetDualIsoFullResBlendingMode(mlvObject_t * video)
{
    return video->llrawproc->diso_frblending;
}

void llrpSetDualIsoFullResBlendingMode(mlvObject_t * video, int value)
{
    video->llrawproc->diso_frblending = value;
}

int llrpGetDualIsoValidity(mlvObject_t * video)
{
    return video->llrawproc->diso_validity;
}

void llrpSetDualIsoValidity(mlvObject_t * video, int diso_force)
{
    if(diso_force)
    {
        video->llrawproc->diso_validity = DISO_FORCED;
    }
    else if(video->DISO.blockType[0] && video->DISO.dualMode)
    {
        video->llrawproc->diso_validity = DISO_VALID;
    }
    else
    {
        video->llrawproc->diso_validity = DISO_INVALID;
    }
}

int llrpHQDualIso(mlvObject_t * video)
{
    return (video->llrawproc->dual_iso == 1) && video->llrawproc->diso_validity && (llrpGetFixRawMode(video));
}

void llrpResetDngBWLevels(mlvObject_t * video)
{
    video->llrawproc->dng_bit_depth = video->RAWI.raw_info.bits_per_pixel;
    video->llrawproc->dng_black_level = video->RAWI.raw_info.black_level;
    video->llrawproc->dng_white_level = video->RAWI.raw_info.white_level;
}

void llrpResetFpmStatus(mlvObject_t * video)
{
    reset_fpm_status(&(video->llrawproc->focus_pixel_map), &(video->llrawproc->fpm_status));
}

void llrpResetBpmStatus(mlvObject_t * video)
{
    reset_bpm_status(&(video->llrawproc->bad_pixel_map), &(video->llrawproc->bpm_status));
}

/* dark frame stuff */
void llrpInitDarkFrameExtFileName(mlvObject_t * video, char * df_filename)
{
    df_free_filename(video);
    df_init_filename(video, df_filename);
}

void llrpFreeDarkFrameExtFileName(mlvObject_t * video)
{
    df_free_filename(video);
}

int llrpGetDarkFrameMode(mlvObject_t * video)
{
    return video->llrawproc->dark_frame;
}

void llrpSetDarkFrameMode(mlvObject_t * video, int value)
{
    video->llrawproc->dark_frame = value;
}

int llrpGetDarkFrameExtStatus(mlvObject_t * video)
{
    if(video->llrawproc->dark_frame_filename) return 1;
    return 0;
}

int llrpGetDarkFrameIntStatus(mlvObject_t * video)
{
    if(video->DARK.blockType[0]) return 1;
    return 0;
}

int llrpValidateExtDarkFrame(mlvObject_t * video, char * df_filename, char * error_message)
{
    return df_validate(video, df_filename, error_message);
}
