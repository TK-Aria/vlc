/*****************************************************************************
 * speex.c: speex decoder/packetizer module making use of libspeex.
 *****************************************************************************
 * Copyright (C) 1999-2001 VideoLAN
 * $Id: speex.c,v 1.3 2003/11/16 21:07:30 gbazin Exp $
 *
 * Authors: Gildas Bazin <gbazin@netcourrier.com>
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
#include <string.h>                                    /* memcpy(), memset() */

#include <vlc/vlc.h>
#include <vlc/decoder.h>
#include <vlc/input.h>

#include <ogg/ogg.h>
#include <speex.h>
#include "speex_header.h"
#include "speex_stereo.h"
#include "speex_callbacks.h"

/*****************************************************************************
 * decoder_sys_t : speex decoder descriptor
 *****************************************************************************/
struct decoder_sys_t
{
    /* Module mode */
    vlc_bool_t b_packetizer;

    /*
     * Input properties
     */
    int i_headers;
    int i_frame_in_packet;

    /*
     * Speex properties
     */
    SpeexBits bits;
    SpeexHeader *p_header;
    SpeexStereoState stereo;
    void *p_state;

    /*
     * Common properties
     */
    audio_date_t end_date;

};

static int pi_channels_maps[6] =
{
    0,
    AOUT_CHAN_CENTER,   AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT,
    AOUT_CHAN_CENTER | AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT,
    AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_REARLEFT
     | AOUT_CHAN_REARRIGHT,
    AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_CENTER
     | AOUT_CHAN_REARLEFT | AOUT_CHAN_REARRIGHT
};

/****************************************************************************
 * Local prototypes
 ****************************************************************************/
static int  OpenDecoder   ( vlc_object_t * );
static int  OpenPacketizer( vlc_object_t * );
static void CloseDecoder  ( vlc_object_t * );

static void *DecodeBlock  ( decoder_t *, block_t ** );
static int  ProcessHeader ( decoder_t *, ogg_packet * );
static void *ProcessPacket( decoder_t *, ogg_packet *, block_t ** );

static aout_buffer_t *DecodePacket( decoder_t *, ogg_packet * );
static block_t *SendPacket( decoder_t *, ogg_packet *, block_t * );

static void ParseSpeexComments( decoder_t *, ogg_packet * );

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
vlc_module_begin();
    set_description( _("Speex audio decoder") );
    set_capability( "decoder", 100 );
    set_callbacks( OpenDecoder, CloseDecoder );

    add_submodule();
    set_description( _("Speex audio packetizer") );
    set_capability( "packetizer", 100 );
    set_callbacks( OpenPacketizer, CloseDecoder );
vlc_module_end();

/*****************************************************************************
 * OpenDecoder: probe the decoder and return score
 *****************************************************************************/
static int OpenDecoder( vlc_object_t *p_this )
{
    decoder_t *p_dec = (decoder_t*)p_this;
    decoder_sys_t *p_sys = p_dec->p_sys;

    if( p_dec->fmt_in.i_codec != VLC_FOURCC('s','p','x',' ') )
    {
        return VLC_EGENERIC;
    }

    /* Allocate the memory needed to store the decoder's structure */
    if( ( p_dec->p_sys = p_sys =
          (decoder_sys_t *)malloc(sizeof(decoder_sys_t)) ) == NULL )
    {
        msg_Err( p_dec, "out of memory" );
        return VLC_EGENERIC;
    }
    p_dec->p_sys->b_packetizer = VLC_FALSE;

    aout_DateSet( &p_sys->end_date, 0 );

    /* Set output properties */
    p_dec->fmt_out.i_cat = AUDIO_ES;
    p_dec->fmt_out.i_codec = AOUT_FMT_S16_NE;

    /* Set callbacks */
    p_dec->pf_decode_audio = (aout_buffer_t *(*)(decoder_t *, block_t **))
        DecodeBlock;
    p_dec->pf_packetize    = (block_t *(*)(decoder_t *, block_t **))
        DecodeBlock;

    p_sys->i_headers = 0;
    p_sys->p_state = NULL;
    p_sys->p_header = NULL;
    p_sys->i_frame_in_packet = 0;

    return VLC_SUCCESS;
}

static int OpenPacketizer( vlc_object_t *p_this )
{
    decoder_t *p_dec = (decoder_t*)p_this;

    int i_ret = OpenDecoder( p_this );

    if( i_ret == VLC_SUCCESS )
    {
        p_dec->p_sys->b_packetizer = VLC_TRUE;
        p_dec->fmt_out.i_codec = VLC_FOURCC('s','p','x',' ');
    }

    return i_ret;
}

/****************************************************************************
 * DecodeBlock: the whole thing
 ****************************************************************************
 * This function must be fed with ogg packets.
 ****************************************************************************/
static void *DecodeBlock( decoder_t *p_dec, block_t **pp_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    ogg_packet oggpacket;

    if( !pp_block ) return NULL;

    if( *pp_block )
    {
        /* Block to Ogg packet */
        oggpacket.packet = (*pp_block)->p_buffer;
        oggpacket.bytes = (*pp_block)->i_buffer;
    }
    else
    {
        if( p_sys->b_packetizer ) return NULL;

        /* Block to Ogg packet */
        oggpacket.packet = NULL;
        oggpacket.bytes = 0;
    }

    oggpacket.granulepos = -1;
    oggpacket.b_o_s = 0;
    oggpacket.e_o_s = 0;
    oggpacket.packetno = 0;

    if( p_sys->i_headers == 0 )
    {
        /* Take care of the initial Speex header */
        if( ProcessHeader( p_dec, &oggpacket ) != VLC_SUCCESS )
        {
            msg_Err( p_dec, "Initial Speex header is corrupted" );
            block_Release( *pp_block );
            return NULL;
        }

        p_sys->i_headers++;

        return ProcessPacket( p_dec, &oggpacket, pp_block );
    }

    if( p_sys->i_headers == 1 )
    {
        /* The next packet in order is the comments header */
        ParseSpeexComments( p_dec, &oggpacket );
        p_sys->i_headers++;

        return ProcessPacket( p_dec, &oggpacket, pp_block );
    }

    return ProcessPacket( p_dec, &oggpacket, pp_block );
}

/*****************************************************************************
 * ProcessHeader: processes the inital Speex header packet.
 *****************************************************************************/
static int ProcessHeader( decoder_t *p_dec, ogg_packet *p_oggpacket )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    void *p_state;
    SpeexHeader *p_header;
    SpeexMode *p_mode;
    SpeexCallback callback;

    p_sys->p_header = p_header =
        speex_packet_to_header( p_oggpacket->packet, p_oggpacket->bytes );
    if( !p_header )
    {
        msg_Err( p_dec, "Cannot read Speex header" );
        return VLC_EGENERIC;
    }
    if( p_header->mode >= SPEEX_NB_MODES )
    {
        msg_Err( p_dec, "Mode number %d does not (yet/any longer) exist in "
                 "this version of libspeex", p_header->mode );
        return VLC_EGENERIC;
    }

    p_mode = speex_mode_list[p_header->mode];

    if( p_header->speex_version_id > 1 )
    {
        msg_Err( p_dec, "This file was encoded with Speex bit-stream "
                 "version %d, which I don't know how to decode",
                 p_header->speex_version_id );
        return VLC_EGENERIC;
    }

    if( p_mode->bitstream_version < p_header->mode_bitstream_version )
    {
        msg_Err( p_dec, "File encoded with a newer version of Speex" );
        return VLC_EGENERIC;
    }
    if( p_mode->bitstream_version > p_header->mode_bitstream_version ) 
    {
        msg_Err( p_dec, "File encoded with an older version of Speex" );
        return VLC_EGENERIC;
    }
   
    msg_Dbg( p_dec, "Speex %d Hz audio using %s mode %s%s", 
             p_header->rate, p_mode->modeName,
             ( p_header->nb_channels == 1 ) ? " (mono" : " (stereo",
             p_header->vbr ? ", VBR)" : ")" );

    /* Take care of speex decoder init */
    speex_bits_init( &p_sys->bits );
    p_sys->p_state = p_state = speex_decoder_init( p_mode );
    if( !p_state )
    {
        msg_Err( p_dec, "Decoder initialization failed" );
        return VLC_EGENERIC;
    }

    if( p_header->nb_channels == 2 )
    {
        SpeexStereoState stereo = SPEEX_STEREO_STATE_INIT;
        p_sys->stereo = stereo;
        callback.callback_id = SPEEX_INBAND_STEREO;
        callback.func = speex_std_stereo_request_handler;
        callback.data = &p_sys->stereo;
        speex_decoder_ctl( p_state, SPEEX_SET_HANDLER, &callback );
    }

    /* Setup the format */
    p_dec->fmt_out.audio.i_physical_channels =
        p_dec->fmt_out.audio.i_original_channels =
            pi_channels_maps[p_header->nb_channels];
    p_dec->fmt_out.audio.i_channels = p_header->nb_channels;
    p_dec->fmt_out.audio.i_rate = p_header->rate;

    aout_DateInit( &p_sys->end_date, p_header->rate );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * ProcessPacket: processes a Speex packet.
 *****************************************************************************/
static void *ProcessPacket( decoder_t *p_dec, ogg_packet *p_oggpacket,
                            block_t **pp_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    block_t *p_block = *pp_block;

    /* Date management */
    if( p_block && p_block->i_pts > 0 &&
        p_block->i_pts != aout_DateGet( &p_sys->end_date ) )
    {
        aout_DateSet( &p_sys->end_date, p_block->i_pts );
    }

    if( !aout_DateGet( &p_sys->end_date ) )
    {
        /* We've just started the stream, wait for the first PTS. */
        if( p_block ) block_Release( p_block );
        return NULL;
    }

    *pp_block = NULL; /* To avoid being fed the same packet again */

    if( p_sys->b_packetizer )
    {
         return SendPacket( p_dec, p_oggpacket, p_block );
    }
    else
    {
        aout_buffer_t *p_aout_buffer;

        if( p_sys->i_headers >= p_sys->p_header->extra_headers + 2 )
            p_aout_buffer = DecodePacket( p_dec, p_oggpacket );
        else
            p_aout_buffer = NULL; /* Skip headers */

        if( p_block )
        {
            block_Release( p_block );
        }
        return p_aout_buffer;
    }
}

/*****************************************************************************
 * DecodePacket: decodes a Speex packet.
 *****************************************************************************/
static aout_buffer_t *DecodePacket( decoder_t *p_dec, ogg_packet *p_oggpacket )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    if( p_oggpacket->bytes )
    {
        /* Copy Ogg packet to Speex bitstream */
        speex_bits_read_from( &p_sys->bits, p_oggpacket->packet,
                              p_oggpacket->bytes );
        p_sys->i_frame_in_packet = 0;
    }

    /* Decode one frame at a time */
    if( p_sys->i_frame_in_packet < p_sys->p_header->frames_per_packet )
    {
        aout_buffer_t *p_aout_buffer;
        int i_ret;

        p_aout_buffer =
            p_dec->pf_aout_buffer_new( p_dec, p_sys->p_header->frame_size );
        if( !p_aout_buffer )
        {
            return NULL;
        }

        i_ret = speex_decode( p_sys->p_state, &p_sys->bits,
                              (int16_t *)p_aout_buffer->p_buffer );
        if( i_ret == -1 ) return NULL; /* End of stream */
        if( i_ret== -2 )
        {
            msg_Warn( p_dec, "Decoding error: corrupted stream?" );
            return NULL;
        }

        if( speex_bits_remaining( &p_sys->bits ) < 0 )
        {
            msg_Warn( p_dec, "Decoding overflow: corrupted stream?" );
        }

        if( p_sys->p_header->nb_channels == 2 )
            speex_decode_stereo( (int16_t *)p_aout_buffer->p_buffer,
                                 p_sys->p_header->frame_size, &p_sys->stereo );

        /* Date management */
        p_aout_buffer->start_date = aout_DateGet( &p_sys->end_date );
        p_aout_buffer->end_date =
            aout_DateIncrement( &p_sys->end_date, p_sys->p_header->frame_size);

        p_sys->i_frame_in_packet++;

        return p_aout_buffer;
    }
    else
    {
        return NULL;
    }
}

/*****************************************************************************
 * SendPacket: send an ogg packet to the stream output.
 *****************************************************************************/
static block_t *SendPacket( decoder_t *p_dec, ogg_packet *p_oggpacket,
                            block_t *p_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    /* Date management */
    p_block->i_dts = p_block->i_pts = aout_DateGet( &p_sys->end_date );

    if( p_sys->i_headers >= p_sys->p_header->extra_headers + 2 )
        p_block->i_length =
            aout_DateIncrement( &p_sys->end_date,
                                p_sys->p_header->frame_size ) -
            p_block->i_pts;
    else
        p_block->i_length = 0;

    return p_block;
}

/*****************************************************************************
 * ParseSpeexComments: FIXME should be done in demuxer
 *****************************************************************************/
#define readint(buf, base) (((buf[base+3]<<24)&0xff000000)| \
                           ((buf[base+2]<<16)&0xff0000)| \
                           ((buf[base+1]<<8)&0xff00)| \
                            (buf[base]&0xff))

static void ParseSpeexComments( decoder_t *p_dec, ogg_packet *p_oggpacket )
{
    input_thread_t *p_input = (input_thread_t *)p_dec->p_parent;
    decoder_sys_t *p_sys = p_dec->p_sys;

    input_info_category_t *p_cat =
        input_InfoCategory( p_input, _("Speex Comment") );

    char *p_buf = (char *)p_oggpacket->packet;
    SpeexMode *p_mode;
    int i_len;

    p_mode = speex_mode_list[p_sys->p_header->mode];
    input_AddInfo( p_cat, _("Mode"), "%s%s",
                   p_mode->modeName, p_sys->p_header->vbr ? " VBR" : "" );

    if( p_oggpacket->bytes < 8 )
    {
        msg_Warn( p_dec, "Invalid/corrupted comments" );
        return;
    }

    i_len = readint( p_buf, 0 ); p_buf += 4;
    if( i_len > p_oggpacket->bytes - 4 )
    {
        msg_Warn( p_dec, "Invalid/corrupted comments" );
        return;
    }

    input_AddInfo( p_cat, p_buf, "" );

    /* TODO: finish comments parsing */
}

/*****************************************************************************
 * CloseDecoder: speex decoder destruction
 *****************************************************************************/
static void CloseDecoder( vlc_object_t *p_this )
{
    decoder_t * p_dec = (decoder_t *)p_this;
    decoder_sys_t *p_sys = p_dec->p_sys;

    if( p_sys->p_state )
    {
        speex_decoder_destroy( p_sys->p_state );
        speex_bits_destroy( &p_sys->bits );
    }

    if( p_sys->p_header ) free( p_sys->p_header );
    free( p_sys );
}
