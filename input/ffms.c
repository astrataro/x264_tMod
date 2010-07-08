/*****************************************************************************
 * ffms.c: x264 ffmpegsource input module
 *****************************************************************************
 * Copyright (C) 2009 x264 project
 *
 * Authors: Mike Gurlitz <mike.gurlitz@gmail.com>
 *          Steven Walters <kemuri9@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *****************************************************************************/

#include "input.h"
#include <ffms.h>
#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, "ffms", __VA_ARGS__ )

#undef DECLARE_ALIGNED
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#ifdef _WIN32
#include <windows.h>
#else
#define SetConsoleTitle(t)
#endif

#if HAVE_AUDIO
#include "audio/audio.h"
#endif

typedef struct
{
    char *filename;
    FFMS_VideoSource *video_source;
    FFMS_Track *track;
    int total_frames;
    struct SwsContext *scaler;
    int pts_offset_flag;
    int64_t pts_offset;
    int reduce_pts;
    int vfr_input;

    int init_width;
    int init_height;

    int cur_width;
    int cur_height;
    int cur_pix_fmt;
} ffms_hnd_t;

static int FFMS_CC update_progress( int64_t current, int64_t total, void *private )
{
    if( current % 10 )
        return 0;
    char buf[200];
    sprintf( buf, "ffms [info]: indexing input file [%.1f%%]", 100.0 * current / total );
    fprintf( stderr, "%s  \r", buf+5 );
    SetConsoleTitle( buf );
    fflush( stderr );
    return 0;
}

static int open_file( char *psz_filename, hnd_t *p_handle, video_info_t *info, cli_input_opt_t *opt )
{
    ffms_hnd_t *h = calloc( 1, sizeof(ffms_hnd_t) );
    if( !h )
        return -1;
    FFMS_Init( 0 );
    FFMS_ErrorInfo e;
    e.BufferSize = 0;
    int seekmode = opt->seek ? FFMS_SEEK_NORMAL : FFMS_SEEK_LINEAR_NO_RW;

    FFMS_Index *idx = NULL;
    if( opt->index_file )
    {
        struct stat index_s, input_s;
        if( !stat( opt->index_file, &index_s ) && !stat( psz_filename, &input_s ) &&
            input_s.st_mtime < index_s.st_mtime )
            idx = FFMS_ReadIndex( opt->index_file, &e );
    }
    if( !idx )
    {
        idx = FFMS_MakeIndex( psz_filename, 0, 0, NULL, NULL, 0, update_progress, NULL, &e );
        fprintf( stderr, "                                            \r" );
        FAIL_IF_ERROR( !idx, "could not create index\n" )
        if( opt->index_file && FFMS_WriteIndex( opt->index_file, idx, &e ) )
            x264_cli_log( "ffms", X264_LOG_WARNING, "could not write index file\n" );
    }

    int trackno = FFMS_GetFirstTrackOfType( idx, FFMS_TYPE_VIDEO, &e );
    FAIL_IF_ERROR( trackno < 0, "could not find video track\n" )

    h->filename = strdup( psz_filename );
    h->video_source = FFMS_CreateVideoSource( psz_filename, trackno, idx, 1, seekmode, &e );
    FAIL_IF_ERROR( !h->video_source, "could not create video source\n" )

    h->track = FFMS_GetTrackFromVideo( h->video_source );

    FFMS_DestroyIndex( idx );
    const FFMS_VideoProperties *videop = FFMS_GetVideoProperties( h->video_source );
    h->total_frames    = videop->NumFrames;
    info->sar_height   = videop->SARDen;
    info->sar_width    = videop->SARNum;
    info->fps_den      = videop->FPSDenominator;
    info->fps_num      = videop->FPSNumerator;
    h->vfr_input       = info->vfr;

    const FFMS_Frame *frame = FFMS_GetFrame( h->video_source, 0, &e );
    FAIL_IF_ERROR( !frame, "could not read frame 0\n" )

    h->init_width  = h->cur_width  = info->width  = frame->EncodedWidth;
    h->init_height = h->cur_height = info->height = frame->EncodedHeight;
    h->cur_pix_fmt = frame->EncodedPixelFormat;
    info->interlaced = frame->InterlacedFrame;
    info->tff        = frame->TopFieldFirst;

    if( h->cur_pix_fmt != PIX_FMT_YUV420P )
        x264_cli_log( "ffms", X264_LOG_WARNING, "converting from %s to YV12\n",
                       avcodec_get_pix_fmt_name( h->cur_pix_fmt ) );

    /* ffms timestamps are in milliseconds. ffms also uses int64_ts for timebase,
     * so we need to reduce large timebases to prevent overflow */
    if( h->vfr_input )
    {
        const FFMS_TrackTimeBase *timebase = FFMS_GetTimeBase( h->track );
        int64_t timebase_num = timebase->Num;
        int64_t timebase_den = timebase->Den * 1000;
        h->reduce_pts = 0;

        while( timebase_num > UINT32_MAX || timebase_den > INT32_MAX )
        {
            timebase_num >>= 1;
            timebase_den >>= 1;
            h->reduce_pts++;
        }
        info->timebase_num = timebase_num;
        info->timebase_den = timebase_den;
    }

    *p_handle = h;
    return 0;
}

static int get_frame_total( hnd_t handle )
{
    return ((ffms_hnd_t*)handle)->total_frames;
}

static int check_swscale( ffms_hnd_t *h, const FFMS_Frame *frame, int i_frame )
{
    if( h->scaler && h->cur_width == frame->EncodedWidth && h->cur_height == frame->EncodedHeight &&
        h->cur_pix_fmt == frame->EncodedPixelFormat )
        return 0;
    if( h->scaler )
    {
        sws_freeContext( h->scaler );
        x264_cli_log( "ffms", X264_LOG_WARNING, "stream properties changed to %dx%d, %s at frame %d  \n", frame->EncodedWidth,
                      frame->EncodedHeight, avcodec_get_pix_fmt_name( frame->EncodedPixelFormat ), i_frame );
        h->cur_width   = frame->EncodedWidth;
        h->cur_height  = frame->EncodedHeight;
        h->cur_pix_fmt = frame->EncodedPixelFormat;
    }
    h->scaler = sws_getContext( h->cur_width, h->cur_height, h->cur_pix_fmt, h->init_width, h->init_height,
                                PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL );
    FAIL_IF_ERROR( !h->scaler, "could not open swscale context\n" )
    return 0;
}

static int read_frame( x264_picture_t *p_pic, hnd_t handle, int i_frame )
{
    ffms_hnd_t *h = handle;
    FFMS_ErrorInfo e;
    e.BufferSize = 0;
    const FFMS_Frame *frame = FFMS_GetFrame( h->video_source, i_frame, &e );
    FAIL_IF_ERROR( !frame, "could not read frame %d\n", i_frame )

    if( check_swscale( h, frame, i_frame ) )
        return -1;
    /* FFMS_VideoSource has a single FFMS_Frame buffer for all calls to GetFrame.
     * With threaded input, copying the pointers would result in the data changing during encoding.
     * FIXME: don't do redundant sws_scales for singlethreaded input, or fix FFMS to allow
     * multiple FFMS_Frame buffers. */
    sws_scale( h->scaler, (uint8_t**)frame->Data, (int*)frame->Linesize, 0,
               frame->EncodedHeight, p_pic->img.plane, p_pic->img.i_stride );

    const FFMS_FrameInfo *info = FFMS_GetFrameInfo( h->track, i_frame );

    if( h->vfr_input )
    {
        FAIL_IF_ERROR( info->PTS == AV_NOPTS_VALUE, "invalid timestamp. "
                       "Use --force-cfr and specify a framerate with --fps\n" )

        if( !h->pts_offset_flag )
        {
            h->pts_offset = info->PTS;
            h->pts_offset_flag = 1;
        }

        p_pic->i_pts = (info->PTS - h->pts_offset) >> h->reduce_pts;
    }
    return 0;
}

static int close_file( hnd_t handle )
{
    ffms_hnd_t *h = handle;
    sws_freeContext( h->scaler );
    FFMS_DestroyVideoSource( h->video_source );
    free( h->filename );
    free( h );
    return 0;
}

#if HAVE_AUDIO
static hnd_t open_audio( hnd_t handle, int track )
{
    ffms_hnd_t *h = handle;
    return audio_open_from_file( NULL, h->filename, track );
}
#endif

const cli_input_t ffms_input = { open_file, get_frame_total, x264_picture_alloc, read_frame, NULL, x264_picture_clean, close_file,
#if HAVE_AUDIO
                                 open_audio
#endif
};
