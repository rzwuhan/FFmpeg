/*
 * AVS2 decoding using the davs2 library
 * Copyright (C) 2018 Yiqun Xu, <yiqun.xu@vipl.ict.ac.cn>
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

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/avutil.h"
#include "avcodec.h"
#include "libavutil/imgutils.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <davs2.h>

typedef struct DAVS2Context {
    AVCodecContext *avctx;
    void *dec_handle;
    int got_seqhdr;

    void *decoder;

    AVFrame *frame;
    davs2_param_t param;      // decoding parameters
    davs2_packet_t packet;     // input bitstream
    int ret;

    int img_width[3];
    int img_height[3];
    int out_flag;
    int decoded_frames;

    davs2_picture_t out_frame;  // output data, frame data
    davs2_seq_info_t headerset;  // output data, sequence header

} DAVS2Context;

/* ---------------------------------------------------------------------------
 */
static __inline
const uint8_t *
find_start_code(const uint8_t *data, int len) {
    while (len >= 4 && (*(int *) data & 0x00FFFFFF) != 0x00010000) {
        ++data;
        --len;
    }

    return len >= 4 ? data : 0;
}

static int ff_davs2_init(AVCodecContext *avctx) {
    DAVS2Context *avs2ctx = avctx->priv_data;

    /* init the decoder */
    avs2ctx->param.threads = 0;
    avs2ctx->param.i_info_level = 0;
    avs2ctx->decoder = davs2_decoder_open(&avs2ctx->param);
    avctx->flags |= AV_CODEC_FLAG_TRUNCATED;

    av_log(avctx, AV_LOG_WARNING, "[davs2] decoder created. 0x%llx\n", avs2ctx->decoder);
    return 0;
}

/* ---------------------------------------------------------------------------
 */
static int
output_decoded_frame(AVCodecContext *avctx, davs2_picture_t *pic, davs2_seq_info_t *headerset, int ret_type,
                     AVFrame *frame) {
    DAVS2Context *avs2ctx = avctx->priv_data;
    avctx->flags |= AV_CODEC_FLAG_TRUNCATED;

    if (headerset == NULL) {
        return 0;
    }

    if (pic == NULL || ret_type == DAVS2_GOT_HEADER) {
        avctx->frame_size =
                (headerset->horizontal_size * headerset->vertical_size * 3 * headerset->bytes_per_sample) >> 1;
        avctx->coded_width = headerset->horizontal_size;
        avctx->coded_height = headerset->vertical_size;
        avctx->width = headerset->horizontal_size;
        avctx->height = headerset->vertical_size;
        avctx->pix_fmt = (headerset->output_bitdepth == 10 ? AV_PIX_FMT_YUV420P10 : AV_PIX_FMT_YUV420P);
        avctx->framerate.num = (int) (headerset->frame_rate);
        avctx->framerate.den = 1;
        return 0;
    }

    const int bytes_per_sample = pic->bytes_per_sample;
    int i;

    for (i = 0; i < 3; ++i) {
        int size_plane = pic->widths[i] * pic->lines[i] * bytes_per_sample;
        frame->buf[i] = av_buffer_alloc(size_plane);
        frame->data[i] = frame->buf[i]->data;
        frame->linesize[i] = pic->widths[i] * bytes_per_sample;
        memcpy(frame->data[i], pic->planes[i], size_plane);
    }

    frame->width = avs2ctx->headerset.horizontal_size;
    frame->height = avs2ctx->headerset.vertical_size;
    frame->pts = avs2ctx->out_frame.pts;

    frame->key_frame = 1;
    frame->pict_type = AV_PICTURE_TYPE_I;
    frame->format = avctx->pix_fmt;
    avs2ctx->out_flag = 1;

    avs2ctx->decoded_frames++;
    return 1;
}

static int ff_davs2_end(AVCodecContext *avctx) {
    DAVS2Context *avs2ctx = avctx->priv_data;

    /* close the decoder */
    if (avs2ctx->decoder != NULL) {
        davs2_decoder_close(avs2ctx->decoder);
        av_log(avctx, AV_LOG_WARNING, "[davs2] decoder destroyed. 0x%llx; frames %d\n", avs2ctx->decoder,
               avs2ctx->decoded_frames);
        avs2ctx->decoder = NULL;
    }

    return 0;
}

static int ff_davs2_decode(AVCodecContext *avctx, void *outdata, int *got_frame, AVPacket *avpkt) {
    DAVS2Context *avs2ctx = avctx->priv_data;
    const uint8_t *data = avpkt->data;
    int data_size = avpkt->size;
    const uint8_t *data_next_start_code;
    AVFrame *frame = data;
    int ret_type = -1;

    *got_frame = 0;
    avs2ctx->out_flag = 0;
    avs2ctx->frame = frame;
    avctx->flags |= AV_CODEC_FLAG_TRUNCATED;

    if (data_size == 0) {
        avs2ctx->packet.data = data;
        avs2ctx->packet.len = data_size;
        avs2ctx->packet.pts = avpkt->pts;
        avs2ctx->packet.dts = avpkt->dts;

        for (;;) {
            avs2ctx->ret = davs2_decoder_flush(avs2ctx->decoder, &avs2ctx->headerset, &avs2ctx->out_frame);

            if (avs2ctx->ret < 0) {
                return 0;
            }

            if (ret_type != DAVS2_DEFAULT) {
                *got_frame = output_decoded_frame(avctx, &avs2ctx->out_frame, &avs2ctx->headerset, ret_type, frame);
                davs2_decoder_frame_unref(avs2ctx->decoder, &avs2ctx->out_frame);
            }
            if (avs2ctx->out_flag == 1) {
                break;
            }
        }
        return 0;
    } else {
        for (;;) {

            int len;

//            data_next_start_code = find_start_code(data + 4, data_size - 4);
//
//            if (data_next_start_code) {
//                len = data_next_start_code - data;
//            } else {
                len = data_size;
//            }

            avs2ctx->packet.data = data;
            avs2ctx->packet.len = len;
            avs2ctx->packet.pts = avpkt->pts;
            avs2ctx->packet.dts = avpkt->dts;

            ret_type = davs2_decoder_send_packet(avs2ctx->decoder, &avs2ctx->packet);
            if (ret_type == DAVS2_ERROR) {
                av_log(NULL, AV_LOG_ERROR, "An decoder error counted\n");
                return -1;
            }

            ret_type = davs2_decoder_recv_frame(avs2ctx->decoder, &avs2ctx->headerset, &avs2ctx->out_frame);

            if (ret_type != DAVS2_DEFAULT) {
                *got_frame = output_decoded_frame(avctx, &avs2ctx->out_frame, &avs2ctx->headerset, ret_type, frame);
                davs2_decoder_frame_unref(avs2ctx->decoder, &avs2ctx->out_frame);
            }

            data += len;
            data_size -= len;

            if (!data_size) break;
        }
    }

    // 剩余未解码数据长度
    data_size = (data - avpkt->data);

    return data_size;
}

AVCodec ff_libdavs2_decoder = {
        .name           = "libdavs2",
        .long_name      = NULL_IF_CONFIG_SMALL("Decoder for Chinese AVS2"),
        .type           = AVMEDIA_TYPE_VIDEO,
        .id             = AV_CODEC_ID_AVS2,
        .priv_data_size = sizeof(DAVS2Context),
        .init           = ff_davs2_init,
        .close          = ff_davs2_end,
        .decode         = ff_davs2_decode,
        .capabilities   =  AV_CODEC_CAP_DELAY,//AV_CODEC_CAP_DR1 |
        .pix_fmts       = (const enum AVPixelFormat[]) {AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P10,
                                                        AV_PIX_FMT_NONE},
};