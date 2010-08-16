#include "audio/encoders.h"

#include <assert.h>
#include <stdlib.h>

struct aenc_t
{
    const audio_encoder_t *enc;
    hnd_t handle;
    hnd_t filters;
};

hnd_t x264_audio_encoder_open( const audio_encoder_t *encoder, hnd_t filter_chain, const char *opts )
{
    assert( encoder && filter_chain );
    struct aenc_t *enc = calloc( 1, sizeof( struct aenc_t ) );
    enc->enc           = encoder;
    enc->handle        = encoder->init( filter_chain, opts );
    enc->filters       = filter_chain;

    return enc;
}

const char *x264_audio_encoder_codec_name( hnd_t encoder )
{
    assert( encoder );
    struct aenc_t *enc = encoder;

    return enc->enc->get_codec_name( enc->handle );
}

audio_info_t *x264_audio_encoder_info( hnd_t encoder )
{
    assert( encoder );
    struct aenc_t *enc = encoder;

    return enc->enc->get_info( enc->handle );
}

audio_packet_t *x264_audio_encode_frame( hnd_t encoder )
{
    assert( encoder );
    struct aenc_t *enc = encoder;

    return enc->enc->get_next_packet( enc->handle );
}

void x264_audio_encoder_skip_samples( hnd_t encoder, uint64_t samplecount )
{
    assert( encoder );
    struct aenc_t *enc = encoder;

    return enc->enc->skip_samples( enc->handle, samplecount );
}

audio_packet_t *x264_audio_encoder_finish( hnd_t encoder )
{
    assert( encoder );
    struct aenc_t *enc = encoder;

    return enc->enc->finish( enc->handle );
}

void x264_audio_free_frame( hnd_t encoder, audio_packet_t *frame )
{
    assert( encoder );
    struct aenc_t *enc = encoder;

    return enc->enc->free_packet( enc->handle, frame );
}

void x264_audio_encoder_close( hnd_t encoder )
{
    if( !encoder )
        return;
    struct aenc_t *enc = encoder;

    enc->enc->close( enc->handle );
    x264_af_close( enc->filters );
    free( enc );
}

const audio_encoder_t *x264_encoder_by_name( const char *name )
{
#define IFRET( enc ) if( !strcmp( #enc, name ) ) return &audio_encoder_ ## enc;
#if HAVE_AUDIO
#if HAVE_LAME
    IFRET( mp3 );
#endif
    IFRET( raw );
#endif /* HAVE_AUDIO */
#undef IFRET
    return NULL;
}

const audio_encoder_t *x264_select_audio_encoder( const char *encoder, char* allowed_list[] )
{
    if( !encoder )
        return NULL;
    if( allowed_list )
    {
        if( !strcmp( encoder, "auto" ) )
        {
            const audio_encoder_t *enc;
            for( int i = 0; allowed_list[i] != NULL; i++ )
            {
                enc = x264_encoder_by_name( allowed_list[i] );
                if( enc )
                    return enc;
            }
            return NULL;
        }
        else
        {
            int valid = 0;
            for( int i = 0; allowed_list[i] != NULL; i++ )
                if( !strcmp( encoder, allowed_list[i] ) )
                {
                    valid = 1;
                    break;
                }
            if( !valid )
                return NULL;
        }
    }
    return x264_encoder_by_name( encoder );
}

#include "filters/audio/internal.h"

hnd_t x264_audio_copy_open( hnd_t handle )
{
    assert( handle );
    audio_hnd_t *h = handle;
#define IFRET( dec )                                                                \
        extern const audio_encoder_t audio_copy_ ## dec;                            \
        if( !strcmp( #dec, h->self->name ) )                                        \
            return x264_audio_encoder_open( &( audio_copy_ ## dec ), handle, NULL );
#if HAVE_AUDIO
    IFRET( lavf );
#endif // HAVE_AUDIO
#undef IFRET
    return NULL;
}