/*****************************************************************************
 * aac.c : Raw aac Stream input module for vlc
 *****************************************************************************
 * Copyright (C) 2001 VideoLAN
 * $Id: aac.c,v 1.7 2003/11/16 21:07:31 gbazin Exp $
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <stdlib.h>                                      /* malloc(), free() */

#include <vlc/vlc.h>
#include <vlc/input.h>

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open    ( vlc_object_t * );
static void Close  ( vlc_object_t * );

vlc_module_begin();
    set_description( _("AAC demuxer" ) );
    set_capability( "demux", 10 );
    set_callbacks( Open, Close );
    add_shortcut( "aac" );
vlc_module_end();

/* TODO:
 * - adif support ?
 *
 */

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int  Demux       ( input_thread_t * );

struct demux_sys_t
{
    mtime_t         i_time;

    es_out_id_t     *p_es;
};

static int i_aac_samplerate[16] =
{
    96000, 88200, 64000, 48000, 44100, 32000, 
    24000, 22050, 16000, 12000, 11025, 8000,
    7350,  0,     0,     0
};

#define AAC_ID( p )          ( ((p)[1]>>3)&0x01 )
#define AAC_SAMPLE_RATE( p ) i_aac_samplerate[((p)[2]>>2)&0x0f]
#define AAC_CHANNELS( p )    ( (((p)[2]&0x01)<<2) | (((p)[3]>>6)&0x03) )
#define AAC_FRAME_SIZE( p )  ( (((p)[3]&0x03) << 11)|( (p)[4] << 3 )|( (((p)[5]) >>5)&0x7 ) )
/* FIXME it's plain wrong */
#define AAC_FRAME_SAMPLES( p )  1024

static inline int HeaderCheck( uint8_t *p )
{
    if( p[0] != 0xff ||
        ( p[1]&0xf6 ) != 0xf0 ||
        AAC_SAMPLE_RATE( p ) == 0 ||
        AAC_CHANNELS( p ) == 0 ||
        AAC_FRAME_SIZE( p ) == 0 )
    {
        return VLC_FALSE;
    }
    return VLC_TRUE;
}


/*****************************************************************************
 * Open: initializes AAC demux structures
 *****************************************************************************/
static int Open( vlc_object_t * p_this )
{
    input_thread_t *p_input = (input_thread_t *)p_this;
    demux_sys_t    *p_sys;
    int            b_forced = VLC_FALSE;

    uint8_t        *p_peek;

    module_t       *p_id3;

    es_format_t    fmt;


    if( p_input->psz_demux && !strncmp( p_input->psz_demux, "aac", 3 ) )
    {
        b_forced = VLC_TRUE;
    }

    if( p_input->psz_name )
    {
        int  i_len = strlen( p_input->psz_name );

        if( i_len > 4 && !strcasecmp( &p_input->psz_name[i_len - 4], ".aac" ) )
        {
            b_forced = VLC_TRUE;
        }
    }

    if( !b_forced )
    {
        /* I haven't find any sure working aac detection so only forced or
         * extention check
         */
        msg_Warn( p_input, "AAC module discarded" );
        return VLC_EGENERIC;
    }

    /* skip possible id3 header */
    p_id3 = module_Need( p_input, "id3", NULL );
    if ( p_id3 )
    {
        module_Unneed( p_input, p_id3 );
    }

    p_input->pf_demux = Demux;
    p_input->pf_demux_control = demux_vaControlDefault;

    p_input->p_demux_data = p_sys = malloc( sizeof( demux_sys_t ) );
    p_sys->i_time = 0;

    /* peek the begining (10 is for adts header) */
    if( stream_Peek( p_input->s, &p_peek, 10 ) < 10 )
    {
        msg_Err( p_input, "cannot peek" );
        goto error;
    }

    if( !strncmp( p_peek, "ADIF", 4 ) )
    {
        msg_Err( p_input, "ADIF file. Not yet supported. (Please report)" );
        goto error;
    }

    es_format_Init( &fmt, AUDIO_ES, VLC_FOURCC( 'm', 'p', '4', 'a' ) );
    if( HeaderCheck( p_peek ) )
    {
        fmt.audio.i_channels = AAC_CHANNELS( p_peek );
        fmt.audio.i_rate = AAC_SAMPLE_RATE( p_peek );

        msg_Dbg( p_input,
                 "adts header: id=%d channels=%d sample_rate=%d",
                 AAC_ID( p_peek ),
                 AAC_CHANNELS( p_peek ),
                 AAC_SAMPLE_RATE( p_peek ) );
    }

    vlc_mutex_lock( &p_input->stream.stream_lock );
    if( input_InitStream( p_input, 0 ) == -1)
    {
        vlc_mutex_unlock( &p_input->stream.stream_lock );
        msg_Err( p_input, "cannot init stream" );
        goto error;
    }
    p_input->stream.i_mux_rate = 0 / 50;
    vlc_mutex_unlock( &p_input->stream.stream_lock );

    p_sys->p_es = es_out_Add( p_input->p_es_out, &fmt );

    return VLC_SUCCESS;

error:
    free( p_sys );
    return VLC_EGENERIC;
}


/*****************************************************************************
 * Demux: reads and demuxes data packets
 *****************************************************************************
 * Returns -1 in case of error, 0 in case of EOF, 1 otherwise
 *****************************************************************************/
static int Demux( input_thread_t * p_input )
{
    demux_sys_t  *p_sys = p_input->p_demux_data;
    pes_packet_t *p_pes;

    uint8_t      h[8];
    uint8_t      *p_peek;

    if( stream_Peek( p_input->s, &p_peek, 8 ) < 8 )
    {
        msg_Warn( p_input, "cannot peek" );
        return 0;
    }

    if( !HeaderCheck( p_peek ) )
    {
        /* we need to resynch */
        vlc_bool_t  b_ok = VLC_FALSE;
        int         i_skip = 0;
        int         i_peek;

        i_peek = stream_Peek( p_input->s, &p_peek, 8096 );
        if( i_peek < 8 )
        {
            msg_Warn( p_input, "cannot peek" );
            return 0;
        }

        while( i_peek >= 8 )
        {
            if( HeaderCheck( p_peek ) )
            {
                b_ok = VLC_TRUE;
                break;
            }

            p_peek++;
            i_peek--;
            i_skip++;
        }

        msg_Warn( p_input, "garbage=%d bytes", i_skip );
        stream_Read( p_input->s, NULL, i_skip );
        return 1;
    }

    memcpy( h, p_peek, 8 );    /* can't use p_peek after stream_*  */

    input_ClockManageRef( p_input,
                          p_input->stream.p_selected_program,
                          p_sys->i_time * 9 / 100 );

    if( ( p_pes = stream_PesPacket( p_input->s, AAC_FRAME_SIZE( h ) ) )==NULL )
    {
        msg_Warn( p_input, "cannot read data" );
        return 0;
    }

    p_pes->i_dts =
    p_pes->i_pts = input_ClockGetTS( p_input,
                                     p_input->stream.p_selected_program,
                                     p_sys->i_time * 9 / 100 );

    es_out_Send( p_input->p_es_out, p_sys->p_es, p_pes );

    p_sys->i_time += (mtime_t)1000000 *
                     (mtime_t)AAC_FRAME_SAMPLES( h ) /
                     (mtime_t)AAC_SAMPLE_RATE( h );
    return( 1 );
}

/*****************************************************************************
 * Close: frees unused data
 *****************************************************************************/
static void Close( vlc_object_t * p_this )
{
    input_thread_t *p_input = (input_thread_t*)p_this;
    demux_sys_t    *p_sys = p_input->p_demux_data;

    free( p_sys );
}

