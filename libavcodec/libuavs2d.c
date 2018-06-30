/*
 * AVS2 decoding using the uavs2d library
 * Copyright (C) 2016 uavs2d project,
 * National Engineering Laboratory for Video Technology(Shenzhen),
 * Digital Media R&D Center at Peking University Shenzhen Graduate School, China
 *
 * Z.Y. Wang <wangzhenyu at pkusz.edu.cn>
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
#include "uavs2d.h"
#include "libavutil/imgutils.h"
#include "internal.h"


const enum AVPictureType IMGTYPE[8] = {
    AV_PICTURE_TYPE_NONE,
    AV_PICTURE_TYPE_I,
    AV_PICTURE_TYPE_NONE,
    AV_PICTURE_TYPE_NONE,
    AV_PICTURE_TYPE_NONE,
    AV_PICTURE_TYPE_P,
    AV_PICTURE_TYPE_P,
    AV_PICTURE_TYPE_B
};

typedef struct UAVS2DContext {
    AVCodecContext *avctx;
    void *dec_handle;
    avs2_frame_t dec_frame;
    int got_seqhdr;
} UAVS2DContext;


static int find_next_start_code(const unsigned char *bs_data, int bs_len, int *left)
{
    const unsigned char *data_ptr = bs_data + 4;
    int count = bs_len - 4;

    while (count >= 4 &&
        ((*(unsigned int *)data_ptr) != 0xB6010000) && /* P/B picture */
        ((*(unsigned int *)data_ptr) != 0xB3010000) && /* I   picture */
        ((*(unsigned int *)data_ptr) != 0xB0010000) && /* sequence header */
        ((*(unsigned int *)data_ptr) != 0xB1010000)) { /* sequence end */
        data_ptr++;
        count--;
    }

    if (count >= 4) {
        *left = count;
        return 1;
    }

    return 0;
}


static int ff_uavs2d_init(AVCodecContext *avctx)
{
    UAVS2DContext *h = avctx->priv_data;
    h->dec_handle = uavs2d_lib_create(1, 1);
    h->dec_frame.i_output_type = AVS2_OUT_I420;
    h->got_seqhdr = 0;

    avctx->pix_fmt = ff_get_format(avctx, avctx->codec->pix_fmts);
    return 0;
}

static int ff_uavs2d_end(AVCodecContext *avctx)
{
    UAVS2DContext *h = avctx->priv_data;
    uavs2d_lib_destroy(h->dec_handle);
    h->got_seqhdr = 0;
    return 0;
}

static void uavs2d_flush(AVCodecContext * avctx)
{
    UAVS2DContext *h = avctx->priv_data;
    h->dec_frame.bs_len = -1;

    do {
        uavs2d_lib_flush(h->dec_handle, &h->dec_frame);
    } while (h->dec_frame.dec_stats == AVS2_TYPE_DECODED);

    h->got_seqhdr = 0;

    uavs2d_lib_destroy(h->dec_handle);
    h->got_seqhdr = 0;
    h->dec_handle = uavs2d_lib_create(1, 1);
    h->dec_frame.i_output_type = AVS2_OUT_I420;
}

static int uavs2d_decode_frame(AVCodecContext *avctx, void *data, int *got_frame, AVPacket *avpkt)
{
    UAVS2DContext *h = avctx->priv_data;
    const uint8_t *buf = avpkt->data;
    int buf_size          = avpkt->size;
    const uint8_t *buf_end;
    const uint8_t *buf_ptr;
    AVFrame *frm = (AVFrame*)data;
    int left_bytes;
    int ret, finish = 0;

    if (buf_size == 0) {
        if (h->got_seqhdr) {
            if (frm->data[0] == NULL && (ret = ff_get_buffer(avctx, frm, 0)) < 0) {
                return ret;
            }
            h->dec_frame.bs_len = -1;
            h->dec_frame.p_buf_y = frm->data[0];
            h->dec_frame.p_buf_u = frm->data[1];
            h->dec_frame.p_buf_v = frm->data[2];
            h->dec_frame.i_stride = frm->linesize[0];
            h->dec_frame.i_stridec = frm->linesize[1];
            uavs2d_lib_flush(h->dec_handle, &h->dec_frame);
            if (h->dec_frame.dec_stats == AVS2_TYPE_DECODED) {
                *got_frame = 1;
                frm->pts = h->dec_frame.pts;
                frm->pict_type = IMGTYPE[h->dec_frame.frm_type];
            }
        }
        return 0;
    }

    buf_ptr = buf;
    buf_end = buf + buf_size;

    while (finish == 0) {
        if (find_next_start_code(buf_ptr, buf_end - buf_ptr, &left_bytes)) {
            h->dec_frame.bs_len = buf_end - buf_ptr - left_bytes;
        } else {
            h->dec_frame.bs_len = buf_end - buf_ptr;
            finish = 1;
        }
        h->dec_frame.bs_buf = (uint8_t*)buf_ptr;
        h->dec_frame.pts = avpkt->pts;

        buf_ptr += h->dec_frame.bs_len;

        if (h->got_seqhdr && frm->data[0]== NULL && (ret = ff_get_buffer(avctx, frm, 0)) < 0) {
            return ret;
        }
        h->dec_frame.p_buf_y = frm->data[0];
        h->dec_frame.p_buf_u = frm->data[1];
        h->dec_frame.p_buf_v = frm->data[2];
        h->dec_frame.i_stride = frm->linesize[0];
        h->dec_frame.i_stridec = frm->linesize[1];

        uavs2d_lib_decode(h->dec_handle, &h->dec_frame);

        switch (h->dec_frame.dec_stats) {
        case AVS2_TYPE_DECODED:   /* decode one frame */
            *got_frame = 1;
            finish = 1;
            frm->pts = h->dec_frame.pts;
            frm->pict_type = IMGTYPE[h->dec_frame.frm_type];
            break;
        case AVS2_TYPE_ERROR:     /* error, current or next frame was not decoded */
            av_log(h->avctx, AV_LOG_WARNING, "decode error\n");
            break;
        case AVS2_TYPE_DROP:     /* error, current or next frame was not decoded */
            av_log(h->avctx, AV_LOG_WARNING, "DROP non-RA frame\n");
            break;
        case AVS2_TYPE_SEQ:       /* sequence header was decoded */
            if (avctx->width != h->dec_frame.info.img_width || avctx->height != h->dec_frame.info.img_height) {
                static const int avs2_fps_num[8] = { 240000, 24, 25, 30000, 30, 50, 60000, 60 };
                static const int avs2_fps_den[8] = {   1001,  1,  1, 1001,  1,  1,  1001,  1 };
                av_log(avctx, AV_LOG_INFO, "dimension change! %dx%d -> %dx%d\n", avctx->width, avctx->height, h->dec_frame.info.img_width, h->dec_frame.info.img_height);
                ret = ff_set_dimensions(avctx, h->dec_frame.info.img_width, h->dec_frame.info.img_height);

                if (ret < 0) {
                    return ret;
                }
                avctx->framerate.num = avs2_fps_num[h->dec_frame.info.frame_rate_code];
                avctx->framerate.den = avs2_fps_den[h->dec_frame.info.frame_rate_code];
            }
            h->got_seqhdr = 1;
        }
     }

    return buf_ptr - buf;
}

AVCodec ff_libuavs2d_decoder = {
    .name           = "uavs2d",
    .long_name      = NULL_IF_CONFIG_SMALL("Decoder for Chinese AVS2"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_CAVS2,
    .priv_data_size = sizeof(UAVS2DContext),
    .init           = ff_uavs2d_init,
    .close          = ff_uavs2d_end,
    .decode         = uavs2d_decode_frame,
    .capabilities   = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY,
    .flush          = uavs2d_flush,
    .pix_fmts       = (const enum AVPixelFormat[]) { AV_PIX_FMT_YUV420P,
                                                     AV_PIX_FMT_NONE },
};