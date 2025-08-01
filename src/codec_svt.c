// Copyright 2020 Cloudinary. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#include "avif/internal.h"

#include "svt-av1/EbSvtAv1.h"

#include "svt-av1/EbSvtAv1Enc.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// The SVT_AV1_VERSION_MAJOR, SVT_AV1_VERSION_MINOR, SVT_AV1_VERSION_PATCHLEVEL, and
// SVT_AV1_CHECK_VERSION macros were added in SVT-AV1 v0.9.0. Define these macros for older
// versions of SVT-AV1.
#ifndef SVT_AV1_VERSION_MAJOR
#define SVT_AV1_VERSION_MAJOR SVT_VERSION_MAJOR
#define SVT_AV1_VERSION_MINOR SVT_VERSION_MINOR
#define SVT_AV1_VERSION_PATCHLEVEL SVT_VERSION_PATCHLEVEL
// clang-format off
#define SVT_AV1_CHECK_VERSION(major, minor, patch)                            \
    (SVT_AV1_VERSION_MAJOR > (major) ||                                       \
     (SVT_AV1_VERSION_MAJOR == (major) && SVT_AV1_VERSION_MINOR > (minor)) || \
     (SVT_AV1_VERSION_MAJOR == (major) && SVT_AV1_VERSION_MINOR == (minor) && \
      SVT_AV1_VERSION_PATCHLEVEL >= (patch)))
// clang-format on
#endif

#if !SVT_AV1_CHECK_VERSION(0, 9, 0)
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#define SVT_FULL_VERSION "v" STR(SVT_AV1_VERSION_MAJOR) "." STR(SVT_AV1_VERSION_MINOR) "." STR(SVT_AV1_VERSION_PATCHLEVEL)
#endif

typedef struct avifCodecInternal
{
    /* SVT-AV1 Encoder Handle */
    EbComponentType * svt_encoder;

    EbSvtAv1EncConfiguration svt_config;
} avifCodecInternal;

static avifBool allocate_svt_buffers(EbBufferHeaderType ** input_buf);
static avifResult dequeue_frame(avifCodec * codec, avifCodecEncodeOutput * output, avifBool done_sending_pics);

static avifResult svtCodecEncodeImage(avifCodec * codec,
                                      avifEncoder * encoder,
                                      const avifImage * image,
                                      avifBool alpha,
                                      int tileRowsLog2,
                                      int tileColsLog2,
                                      int quantizer,
                                      avifEncoderChanges encoderChanges,
                                      avifBool disableLaggedOutput,
                                      uint32_t addImageFlags,
                                      avifCodecEncodeOutput * output)
{
    // SVT-AV1 does not support changing encoder settings.
    if (encoderChanges) {
        return AVIF_RESULT_NOT_IMPLEMENTED;
    }

    // SVT-AV1 does not support changing image dimensions.
    if (codec->internal->svt_encoder != NULL) {
        if ((codec->internal->svt_config.source_width != image->width) || (codec->internal->svt_config.source_height != image->height)) {
            return AVIF_RESULT_NOT_IMPLEMENTED;
        }
    }

    // SVT-AV1 does not support encoding layered image.
    if (encoder->extraLayerCount > 0) {
        return AVIF_RESULT_NOT_IMPLEMENTED;
    }

    // SVT-AV1 does not support disabling lagged output. Ignore this setting.
    (void)disableLaggedOutput;

    avifResult result = AVIF_RESULT_UNKNOWN_ERROR;
    EbColorFormat color_format = EB_YUV420;
    uint8_t * uvPlanes = NULL; // 4:2:0 U and V placeholder for alpha because SVT-AV1 does not support 4:0:0.
    EbBufferHeaderType * input_buffer = NULL;
    EbErrorType res = EB_ErrorNone;

    int y_shift = 0;
    EbColorRange svt_range;
    if (alpha) {
        // AV1-AVIF specification, Section 4 "Auxiliary Image Items and Sequences":
        //   The color_range field in the Sequence Header OBU shall be set to 1.
        svt_range = EB_CR_FULL_RANGE;

        // AV1-AVIF specification, Section 4 "Auxiliary Image Items and Sequences":
        //   The mono_chrome field in the Sequence Header OBU shall be set to 1.
        // Some encoders do not support 4:0:0 and encode alpha as 4:2:0 so it is not always respected.
        y_shift = 1;

        // CICP (CP/TC/MC) does not apply to the alpha auxiliary image.
        // Use Unspecified (2) colour primaries, transfer characteristics, and matrix coefficients below.
    } else {
        // AV1-ISOBMFF specification, Section 2.3.4:
        //   The value of full_range_flag in the 'colr' box SHALL match the color_range
        //   flag in the Sequence Header OBU.
        svt_range = (image->yuvRange == AVIF_RANGE_FULL) ? EB_CR_FULL_RANGE : EB_CR_STUDIO_RANGE;

        // AV1-AVIF specification, Section 2.2.1. "AV1 Item Configuration Property":
        //   The values of the fields in the AV1CodecConfigurationBox shall match those
        //   of the Sequence Header OBU in the AV1 Image Item Data.
        switch (image->yuvFormat) {
            case AVIF_PIXEL_FORMAT_YUV444:
                color_format = EB_YUV444;
                break;
            case AVIF_PIXEL_FORMAT_YUV422:
                color_format = EB_YUV422;
                break;
            case AVIF_PIXEL_FORMAT_YUV420:
                color_format = EB_YUV420;
                y_shift = 1;
                break;
            case AVIF_PIXEL_FORMAT_YUV400:
                // Setting color_format = EB_YUV400; results in "Svt[error]: Instance 1: Only support 420 now".
            case AVIF_PIXEL_FORMAT_NONE:
            case AVIF_PIXEL_FORMAT_COUNT:
            default:
                return AVIF_RESULT_UNKNOWN_ERROR;
        }
    }

    if (codec->internal->svt_encoder == NULL) {
        EbSvtAv1EncConfiguration * svt_config = &codec->internal->svt_config;
        // Zero-initialize svt_config because svt_av1_enc_init_handle() does not set many fields of svt_config.
        // See https://gitlab.com/AOMediaCodec/SVT-AV1/-/issues/1697.
        memset(svt_config, 0, sizeof(EbSvtAv1EncConfiguration));

#if SVT_AV1_CHECK_VERSION(3, 0, 0)
        res = svt_av1_enc_init_handle(&codec->internal->svt_encoder, svt_config);
#else
        res = svt_av1_enc_init_handle(&codec->internal->svt_encoder, NULL, svt_config);
#endif
        if (res != EB_ErrorNone) {
            goto cleanup;
        }
        svt_config->encoder_color_format = color_format;
        svt_config->encoder_bit_depth = (uint8_t)image->depth;

        // Section 2.3.4 of AV1-ISOBMFF says 'colr' with 'nclx' should be present and shall match CICP
        // values in the Sequence Header OBU, unless the latter has 2/2/2 (Unspecified).
        // So set CICP values to 2/2/2 (Unspecified) in the Sequence Header OBU for simplicity.
        // It may also save 3 bytes since the AV1 encoder may set color_description_present_flag to 0
        // (see Section 5.5.2 "Color config syntax" of the AV1 specification).
        svt_config->color_primaries = EB_CICP_CP_UNSPECIFIED;
        svt_config->transfer_characteristics = EB_CICP_TC_UNSPECIFIED;
        svt_config->matrix_coefficients = EB_CICP_MC_UNSPECIFIED;

        svt_config->color_range = svt_range;
#if !SVT_AV1_CHECK_VERSION(0, 9, 0)
        svt_config->is_16bit_pipeline = image->depth > 8;
#endif
        svt_config->source_width = image->width;
        svt_config->source_height = image->height;
#if SVT_AV1_CHECK_VERSION(3, 0, 0)
        svt_config->level_of_parallelism = encoder->maxThreads;
#else
        svt_config->logical_processors = encoder->maxThreads;
#endif
        svt_config->enable_adaptive_quantization = 2;
        // disable 2-pass
#if SVT_AV1_CHECK_VERSION(0, 9, 0)
        svt_config->rc_stats_buffer = (SvtAv1FixedBuf) { NULL, 0 };
#else
        svt_config->rc_firstpass_stats_out = AVIF_FALSE;
        svt_config->rc_twopass_stats_in = (SvtAv1FixedBuf) { NULL, 0 };
#endif

        svt_config->rate_control_mode = 0; // CRF because enable_adaptive_quantization is 2
        if (alpha) {
            svt_config->min_qp_allowed = AVIF_CLAMP(encoder->minQuantizerAlpha, 0, 62);
            svt_config->max_qp_allowed = AVIF_CLAMP(encoder->maxQuantizerAlpha, 0, 63);
        } else {
            svt_config->min_qp_allowed = AVIF_CLAMP(encoder->minQuantizer, 0, 62);
            svt_config->max_qp_allowed = AVIF_CLAMP(encoder->maxQuantizer, 0, 63);
        }
        svt_config->qp = quantizer;

        if (tileRowsLog2 != 0) {
            svt_config->tile_rows = tileRowsLog2;
        }
        if (tileColsLog2 != 0) {
            svt_config->tile_columns = tileColsLog2;
        }
        if (encoder->speed != AVIF_SPEED_DEFAULT) {
#if SVT_AV1_CHECK_VERSION(0, 9, 0)
            svt_config->enc_mode = (int8_t)encoder->speed;
#else
            int speed = AVIF_CLAMP(encoder->speed, 0, 8);
            svt_config->enc_mode = (int8_t)speed;
#endif
        }

        if (color_format == EB_YUV422 || image->depth > 10) {
            svt_config->profile = PROFESSIONAL_PROFILE;
        } else if (color_format == EB_YUV444) {
            svt_config->profile = HIGH_PROFILE;
        }

        // In order for SVT-AV1 to force keyframes by setting pic_type to
        // EB_AV1_KEY_PICTURE on any frame, force_key_frames has to be set.
        svt_config->force_key_frames = true;

        // keyframeInterval == 1 case is handled when encoding each frame by
        // setting pic_type to EB_AV1_KEY_PICTURE. For keyframeInterval > 1,
        // set the intra_period_length. Even though setting intra_period_length
        // to 0 should work in this case, it does not.
        if (encoder->keyframeInterval > 1) {
            svt_config->intra_period_length = encoder->keyframeInterval - 1;
        }

#if SVT_AV1_CHECK_VERSION(0, 9, 1)
        for (uint32_t i = 0; i < codec->csOptions->count; ++i) {
            avifCodecSpecificOption * entry = &codec->csOptions->entries[i];
            if (svt_av1_enc_parse_parameter(svt_config, entry->key, entry->value) < 0) {
                avifDiagnosticsPrintf(codec->diag, "Invalid value for %s: %s.", entry->key, entry->value);
                result = AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
                goto cleanup;
            }
        }
#else
        if (codec->csOptions->count > 0) {
            avifDiagnosticsPrintf(codec->diag, "SVT-AV1 does not support setting options");
            result = AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
            goto cleanup;
        }
#endif

#if SVT_AV1_CHECK_VERSION(3, 0, 0)
        svt_config->lossless = quantizer == AVIF_QUANTIZER_LOSSLESS;
        // TODO: https://gitlab.com/AOMediaCodec/SVT-AV1/-/issues/2245 - Enable when resolved.
        // svt_config->avif = (addImageFlags & AVIF_ADD_IMAGE_FLAG_SINGLE) != 0;
#endif

        res = svt_av1_enc_set_parameter(codec->internal->svt_encoder, svt_config);
        if (res == EB_ErrorBadParameter) {
            goto cleanup;
        }

        res = svt_av1_enc_init(codec->internal->svt_encoder);
        if (res != EB_ErrorNone) {
            goto cleanup;
        }
    }

    if (!allocate_svt_buffers(&input_buffer)) {
        goto cleanup;
    }
    EbSvtIOFormat * input_picture_buffer = (EbSvtIOFormat *)input_buffer->p_buffer;

    const uint32_t bytesPerPixel = image->depth > 8 ? 2 : 1;
    const uint32_t uvHeight = (image->height + y_shift) >> y_shift;
    if (alpha) {
        input_picture_buffer->y_stride = image->alphaRowBytes / bytesPerPixel;
        input_picture_buffer->luma = image->alphaPlane;
        input_buffer->n_filled_len = image->alphaRowBytes * image->height;

#if SVT_AV1_CHECK_VERSION(1, 8, 0)
        // Simulate 4:2:0 UV planes. SVT-AV1 does not support 4:0:0 samples.
        const uint32_t uvWidth = (image->width + y_shift) >> y_shift;
        const uint32_t uvRowBytes = uvWidth * bytesPerPixel;
        const uint32_t uvSize = uvRowBytes * uvHeight;
        uvPlanes = avifAlloc(uvSize);
        if (uvPlanes == NULL) {
            goto cleanup;
        }
        memset(uvPlanes, 0, uvSize);
        input_picture_buffer->cb = uvPlanes;
        input_buffer->n_filled_len += uvSize;
        input_picture_buffer->cr = uvPlanes;
        input_buffer->n_filled_len += uvSize;
        input_picture_buffer->cb_stride = uvWidth;
        input_picture_buffer->cr_stride = uvWidth;
#else
        // This workaround was not needed before SVT-AV1 1.8.0.
        // See https://github.com/AOMediaCodec/libavif/issues/1992.
        (void)uvPlanes;
#endif
    } else {
        input_picture_buffer->y_stride = image->yuvRowBytes[0] / bytesPerPixel;
        input_picture_buffer->luma = image->yuvPlanes[0];
        input_buffer->n_filled_len = image->yuvRowBytes[0] * image->height;
        input_picture_buffer->cb = image->yuvPlanes[1];
        input_buffer->n_filled_len += image->yuvRowBytes[1] * uvHeight;
        input_picture_buffer->cr = image->yuvPlanes[2];
        input_buffer->n_filled_len += image->yuvRowBytes[2] * uvHeight;
        input_picture_buffer->cb_stride = image->yuvRowBytes[1] / bytesPerPixel;
        input_picture_buffer->cr_stride = image->yuvRowBytes[2] / bytesPerPixel;
    }

    input_buffer->flags = 0;
    input_buffer->pts = 0;

    EbAv1PictureType frame_type = EB_AV1_INVALID_PICTURE;
    if ((addImageFlags & AVIF_ADD_IMAGE_FLAG_FORCE_KEYFRAME) || (encoder->keyframeInterval == 1)) {
        frame_type = EB_AV1_KEY_PICTURE;
    }
    input_buffer->pic_type = frame_type;

    res = svt_av1_enc_send_picture(codec->internal->svt_encoder, input_buffer);
    if (res != EB_ErrorNone) {
        goto cleanup;
    }

    result = dequeue_frame(codec, output, AVIF_FALSE);
cleanup:
    if (uvPlanes) {
        avifFree(uvPlanes);
    }
    if (input_buffer) {
        if (input_buffer->p_buffer) {
            avifFree(input_buffer->p_buffer);
        }
        avifFree(input_buffer);
    }
    return result;
}

static avifBool svtCodecEncodeFinish(avifCodec * codec, avifCodecEncodeOutput * output)
{
    EbErrorType ret = EB_ErrorNone;

    EbBufferHeaderType input_buffer;
    input_buffer.n_alloc_len = 0;
    input_buffer.n_filled_len = 0;
    input_buffer.n_tick_count = 0;
    input_buffer.p_app_private = NULL;
    input_buffer.flags = EB_BUFFERFLAG_EOS;
    input_buffer.p_buffer = NULL;
    input_buffer.metadata = NULL;

    // flush
    ret = svt_av1_enc_send_picture(codec->internal->svt_encoder, &input_buffer);

    if (ret != EB_ErrorNone)
        return AVIF_FALSE;

    return (dequeue_frame(codec, output, AVIF_TRUE) == AVIF_RESULT_OK);
}

const char * avifCodecVersionSvt(void)
{
#if SVT_AV1_CHECK_VERSION(0, 9, 0)
    return svt_av1_get_version();
#else
    return SVT_FULL_VERSION;
#endif
}

static void svtCodecDestroyInternal(avifCodec * codec)
{
    if (codec->internal->svt_encoder) {
        svt_av1_enc_deinit(codec->internal->svt_encoder);
        svt_av1_enc_deinit_handle(codec->internal->svt_encoder);
        codec->internal->svt_encoder = NULL;
    }
    avifFree(codec->internal);
}

avifCodec * avifCodecCreateSvt(void)
{
    avifCodec * codec = (avifCodec *)avifAlloc(sizeof(avifCodec));
    if (codec == NULL) {
        return NULL;
    }
    memset(codec, 0, sizeof(struct avifCodec));
    codec->encodeImage = svtCodecEncodeImage;
    codec->encodeFinish = svtCodecEncodeFinish;
    codec->destroyInternal = svtCodecDestroyInternal;

    codec->internal = (struct avifCodecInternal *)avifAlloc(sizeof(avifCodecInternal));
    if (codec->internal == NULL) {
        avifFree(codec);
        return NULL;
    }
    memset(codec->internal, 0, sizeof(struct avifCodecInternal));
    return codec;
}

static avifBool allocate_svt_buffers(EbBufferHeaderType ** input_buf)
{
    *input_buf = avifAlloc(sizeof(EbBufferHeaderType));
    if (!(*input_buf)) {
        return AVIF_FALSE;
    }
    (*input_buf)->p_buffer = avifAlloc(sizeof(EbSvtIOFormat));
    if (!(*input_buf)->p_buffer) {
        return AVIF_FALSE;
    }
    memset((*input_buf)->p_buffer, 0, sizeof(EbSvtIOFormat));
    (*input_buf)->size = sizeof(EbBufferHeaderType);
    (*input_buf)->p_app_private = NULL;
    (*input_buf)->pic_type = EB_AV1_INVALID_PICTURE;
    (*input_buf)->metadata = NULL;

    return AVIF_TRUE;
}

static avifResult dequeue_frame(avifCodec * codec, avifCodecEncodeOutput * output, avifBool done_sending_pics)
{
    EbErrorType res;
    int encode_at_eos = 0;

    do {
        EbBufferHeaderType * output_buf = NULL;

        res = svt_av1_enc_get_packet(codec->internal->svt_encoder, &output_buf, (uint8_t)done_sending_pics);
        if (output_buf != NULL) {
            encode_at_eos = ((output_buf->flags & EB_BUFFERFLAG_EOS) == EB_BUFFERFLAG_EOS);
            if (output_buf->p_buffer && (output_buf->n_filled_len > 0)) {
                const avifResult result = avifCodecEncodeOutputAddSample(output,
                                                                         output_buf->p_buffer,
                                                                         output_buf->n_filled_len,
                                                                         (output_buf->pic_type == EB_AV1_KEY_PICTURE));
                if (result != AVIF_RESULT_OK) {
                    svt_av1_enc_release_out_buffer(&output_buf);
                    return result;
                }
            }
            svt_av1_enc_release_out_buffer(&output_buf);
        }
        output_buf = NULL;
    } while (res == EB_ErrorNone && !encode_at_eos);
    if (!done_sending_pics && ((res == EB_ErrorNone) || (res == EB_NoErrorEmptyQueue)))
        return AVIF_RESULT_OK;
    return (res == EB_ErrorNone ? AVIF_RESULT_OK : AVIF_RESULT_UNKNOWN_ERROR);
}
