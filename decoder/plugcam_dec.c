/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * plugcam_dec.dll: H.264 → NV12 on the GPU (Direct3D 12 video decoding) with FFmpeg's libavcodec,
 * behind a tiny C API so the app does not depend on FFmpeg's structures. See docs/decoder.md.
 *
 * Built by build.sh: FFmpeg is configured down to the H.264 decoder and its d3d12va hwaccel and
 * linked in statically; the pc_dec_* functions here are the only things the DLL exports. FFmpeg is
 * LGPL 2.1+, so the DLL as a whole is too, and can be swapped for a rebuilt one.
 *
 * Why not the decoder built into Windows: with DXVA the Media Foundation H.264 decoder keeps two
 * of its threads busy for the whole time the GPU decodes, about 330 ms of processor time per
 * second for 1080p30 on an RTX 3080 Ti. This costs about 30.
 *
 * A crop is done on the GPU too (pc_dec_set_crop), with the Direct3D 12 video processor, before
 * the picture is read back: scaling a cropped 1080p picture on the CPU costs about 7 ms a frame.
 */

#define COBJMACROS

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Microsoft's DirectX-Headers (build.sh puts them first on the include path; mingw-w64's lack
 * the video processor), included before FFmpeg's header so its <initguid.h> does not define
 * every interface ID here too: libavcodec already has the ones it uses. */
#include <d3d12.h>
#include <d3d12video.h>
#include <d3d12sdklayers.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d12va.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>

#define API __declspec(dllexport)

/* The interface IDs used here, under names of our own. */
static const IID PC_IID_CommandAllocator = { 0x6102dee4, 0xaf59, 0x4b09, { 0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24 } };
static const IID PC_IID_CommandQueue = { 0x0ec870a6, 0x5d7e, 0x4c22, { 0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed } };
static const IID PC_IID_VideoProcessor = { 0x304fdb32, 0xbede, 0x410a, { 0x85, 0x45, 0x94, 0x3a, 0xc6, 0xa4, 0x61, 0x38 } };
static const IID PC_IID_VideoProcessCommandList = { 0xaeb2543a, 0x167f, 0x4682, { 0xac, 0xc8, 0xd1, 0x59, 0xed, 0x4a, 0x62, 0x09 } };

/* Bumped when the functions below change, so the app can refuse a DLL it does not match. */
#define PC_DEC_API_VERSION 2

typedef struct PcFrame {
    int32_t width, height;
    const uint8_t *y;
    int32_t y_stride;
    const uint8_t *uv;
    int32_t uv_stride;
    int32_t full_range;
    int32_t bt709;
    /* Cropped and scaled as pc_dec_set_crop asked. */
    int32_t prescaled;
} PcFrame;

/* The rectangle x, y, w, h of the decoded picture, stretched to out_w x out_h. */
typedef struct PcCrop {
    int32_t x, y, w, h, out_w, out_h;
} PcCrop;

/* The Direct3D 12 video processor set up for one crop, input size and color space. */
typedef struct Scaler {
    PcCrop crop;
    int in_w, in_h, full_range, bt709;
    ID3D12CommandQueue *queue;
    ID3D12CommandAllocator *allocator;
    ID3D12VideoProcessCommandList *list;
    ID3D12VideoProcessor *processor;
    AVBufferRef *frames; /* pool of out_w x out_h NV12 textures */
    AVFrame *out;
} Scaler;

typedef struct PcDec {
    AVCodecContext *ctx;
    AVBufferRef *device;
    AVPacket *packet;
    AVFrame *gpu;
    AVFrame *cpu;
    PcCrop crop; /* w == 0: none */
    Scaler *scaler;
    int scaler_failed; /* the GPU cannot crop; the app crops on the CPU instead */
} PcDec;

/* The last error FFmpeg logged, or one of ours; read with pc_dec_last_error(). */
static __thread char last_error[512];

static void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_error, sizeof last_error, fmt, ap);
    va_end(ap);
}

static void log_callback(void *avcl, int level, const char *fmt, va_list vl) {
    (void)avcl;
    if (level > AV_LOG_ERROR) return;
    vsnprintf(last_error, sizeof last_error, fmt, vl);
    size_t n = strlen(last_error);
    while (n && (last_error[n - 1] == '\n' || last_error[n - 1] == '\r')) last_error[--n] = 0;
}

static enum AVPixelFormat pick_d3d12(AVCodecContext *ctx, const enum AVPixelFormat *formats) {
    (void)ctx;
    for (const enum AVPixelFormat *f = formats; *f != AV_PIX_FMT_NONE; f++)
        if (*f == AV_PIX_FMT_D3D12) return *f;
    set_error("the GPU cannot decode this stream");
    return AV_PIX_FMT_NONE;
}

#define RELEASE(p, Type)        \
    do {                        \
        if (p) Type##_Release(p); \
        (p) = NULL;             \
    } while (0)

static void scaler_free(Scaler **ps) {
    Scaler *s = *ps;
    if (!s) return;
    av_frame_free(&s->out);
    av_buffer_unref(&s->frames);
    RELEASE(s->processor, ID3D12VideoProcessor);
    RELEASE(s->list, ID3D12VideoProcessCommandList);
    RELEASE(s->allocator, ID3D12CommandAllocator);
    RELEASE(s->queue, ID3D12CommandQueue);
    av_free(s);
    *ps = NULL;
}

static DXGI_COLOR_SPACE_TYPE color_space(int full_range, int bt709) {
    if (bt709) return full_range ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    return full_range ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
}

#define CHECK(call, what)                                              \
    do {                                                               \
        HRESULT hr_ = (call);                                          \
        if (FAILED(hr_)) {                                             \
            set_error("GPU crop: %s failed (0x%08lx)", what, (unsigned long)hr_); \
            goto fail;                                                 \
        }                                                              \
    } while (0)

/* Whether w x h is outside a size range the video processor reported. */
static int c_outside(int w, int h, D3D12_VIDEO_SIZE_RANGE r) {
    return (UINT)w < r.MinWidth || (UINT)w > r.MaxWidth || (UINT)h < r.MinHeight || (UINT)h > r.MaxHeight;
}

static Scaler *scaler_new(PcDec *d, int in_w, int in_h, int full_range, int bt709) {
    AVHWDeviceContext *dev = (AVHWDeviceContext *)d->device->data;
    AVD3D12VADeviceContext *hw = dev->hwctx;
    Scaler *s = av_mallocz(sizeof *s);
    if (!s) return NULL;
    s->crop = d->crop;
    s->in_w = in_w, s->in_h = in_h, s->full_range = full_range, s->bt709 = bt709;

    D3D12_COMMAND_QUEUE_DESC queue_desc = { .Type = D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS };
    CHECK(ID3D12Device_CreateCommandQueue(hw->device, &queue_desc, &PC_IID_CommandQueue, (void **)&s->queue), "command queue");
    CHECK(ID3D12Device_CreateCommandAllocator(hw->device, D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, &PC_IID_CommandAllocator,
                                              (void **)&s->allocator), "command allocator");
    CHECK(ID3D12Device_CreateCommandList(hw->device, 0, D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, s->allocator, NULL,
                                         &PC_IID_VideoProcessCommandList, (void **)&s->list), "command list");
    CHECK(ID3D12VideoProcessCommandList_Close(s->list), "closing the command list");

    /* Same color space in and out: the video processor only crops and scales. */
    DXGI_COLOR_SPACE_TYPE space = color_space(full_range, bt709);
    D3D12_FEATURE_DATA_VIDEO_PROCESS_SUPPORT support = {
        .InputSample = { .Width = in_w, .Height = in_h, .Format = { DXGI_FORMAT_NV12, space } },
        .InputFieldType = D3D12_VIDEO_FIELD_TYPE_NONE,
        .InputStereoFormat = D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE,
        .InputFrameRate = { 30, 1 },
        .OutputFormat = { DXGI_FORMAT_NV12, space },
        .OutputStereoFormat = D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE,
        .OutputFrameRate = { 30, 1 },
    };
    CHECK(ID3D12VideoDevice_CheckFeatureSupport(hw->video_device, D3D12_FEATURE_VIDEO_PROCESS_SUPPORT, &support, sizeof support),
          "asking for video processing support");
    if (!(support.SupportFlags & D3D12_VIDEO_PROCESS_SUPPORT_FLAG_SUPPORTED)) {
        set_error("GPU crop: the video processor cannot work on NV12 (color space %d)", (int)space);
        goto fail;
    }
    D3D12_VIDEO_SCALE_SUPPORT *scale = &support.ScaleSupport;
    if (c_outside(s->crop.w, s->crop.h, scale->OutputSizeRange) || c_outside(s->crop.out_w, s->crop.out_h, scale->OutputSizeRange)) {
        set_error("GPU crop: %dx%d -> %dx%d is outside what the video processor scales (%u..%u x %u..%u)", s->crop.w, s->crop.h,
                  s->crop.out_w, s->crop.out_h, scale->OutputSizeRange.MinWidth, scale->OutputSizeRange.MaxWidth,
                  scale->OutputSizeRange.MinHeight, scale->OutputSizeRange.MaxHeight);
        goto fail;
    }

    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC out_desc = {
        .Format = DXGI_FORMAT_NV12,
        .ColorSpace = space,
        .AlphaFillMode = D3D12_VIDEO_PROCESS_ALPHA_FILL_MODE_OPAQUE,
        .AlphaFillModeSourceStreamIndex = 0,
        .BackgroundColor = { 0.0f, 0.0f, 0.0f, 1.0f },
        .FrameRate = { 30, 1 },
        .EnableStereo = FALSE,
    };
    /* The sizes are exact: the source rectangle and the picture it becomes. */
    D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC in_desc = {
        .Format = DXGI_FORMAT_NV12,
        .ColorSpace = space,
        .SourceAspectRatio = { 1, 1 },
        .DestinationAspectRatio = { 1, 1 },
        .FrameRate = { 30, 1 },
        .SourceSizeRange = { .MaxWidth = s->crop.w, .MaxHeight = s->crop.h, .MinWidth = s->crop.w, .MinHeight = s->crop.h },
        .DestinationSizeRange = { .MaxWidth = s->crop.out_w, .MaxHeight = s->crop.out_h, .MinWidth = s->crop.out_w, .MinHeight = s->crop.out_h },
        .EnableOrientation = FALSE,
        .FilterFlags = D3D12_VIDEO_PROCESS_FILTER_FLAG_NONE,
        .StereoFormat = D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE,
        .FieldType = D3D12_VIDEO_FIELD_TYPE_NONE,
        .DeinterlaceMode = D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_NONE,
        .EnableAlphaBlending = FALSE,
        .LumaKey = { .Enable = FALSE, .Lower = 0.0f, .Upper = 1.0f },
        .NumPastFrames = 0,
        .NumFutureFrames = 0,
        .EnableAutoProcessing = FALSE,
    };
    CHECK(ID3D12VideoDevice_CreateVideoProcessor(hw->video_device, 0, &out_desc, 1, &in_desc, &PC_IID_VideoProcessor,
                                                 (void **)&s->processor), "video processor");

    s->frames = av_hwframe_ctx_alloc(d->device);
    if (!s->frames) goto fail;
    AVHWFramesContext *fc = (AVHWFramesContext *)s->frames->data;
    fc->format = AV_PIX_FMT_D3D12;
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width = s->crop.out_w;
    fc->height = s->crop.out_h;
    fc->initial_pool_size = 1;
    int err = av_hwframe_ctx_init(s->frames);
    if (err < 0) {
        set_error("GPU crop: output frames: %s", av_err2str(err));
        goto fail;
    }
    s->out = av_frame_alloc();
    if (!s->out || (err = av_hwframe_get_buffer(s->frames, s->out, 0)) < 0) {
        set_error("GPU crop: output frame: %s", s->out ? av_err2str(err) : "out of memory");
        goto fail;
    }
    return s;
fail:
    scaler_free(&s);
    return NULL;
}

static D3D12_RESOURCE_BARRIER transition(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    return (D3D12_RESOURCE_BARRIER){
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = { .pResource = r, .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, .StateBefore = from, .StateAfter = to },
    };
}

/* Crops and scales `src` into the scaler's output frame on the GPU. The output frame's fence is
 * signalled when it is done, which av_hwframe_transfer_data waits for. */
static int scale(Scaler *s, const AVFrame *src) {
    AVD3D12VAFrame *in = (AVD3D12VAFrame *)src->data[0];
    AVD3D12VAFrame *out = (AVD3D12VAFrame *)s->out->data[0];

    /* The previous frame's work is done (its picture was read back), so the allocator is free. */
    CHECK(ID3D12CommandAllocator_Reset(s->allocator), "resetting the allocator");
    CHECK(ID3D12VideoProcessCommandList_Reset(s->list, s->allocator), "resetting the command list");

    D3D12_RESOURCE_BARRIER barriers[2] = {
        transition(in->texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ),
        transition(out->texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE),
    };
    ID3D12VideoProcessCommandList_ResourceBarrier(s->list, 2, barriers);

    const PcCrop *c = &s->crop;
    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS out_args = {
        .OutputStream = { { .pTexture2D = out->texture, .Subresource = out->subresource_index } },
        .TargetRectangle = { 0, 0, c->out_w, c->out_h },
    };
    D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS in_args = {
        .InputStream = { { .pTexture2D = in->texture, .Subresource = in->subresource_index } },
        .Transform = {
            .SourceRectangle = { c->x, c->y, c->x + c->w, c->y + c->h },
            .DestinationRectangle = { 0, 0, c->out_w, c->out_h },
            .Orientation = D3D12_VIDEO_PROCESS_ORIENTATION_DEFAULT,
        },
        .Flags = D3D12_VIDEO_PROCESS_INPUT_STREAM_FLAG_NONE,
        .AlphaBlending = { FALSE, 1.0f },
    };
    ID3D12VideoProcessCommandList_ProcessFrames(s->list, s->processor, &out_args, 1, &in_args);

    for (int i = 0; i < 2; i++) {
        D3D12_RESOURCE_STATES t = barriers[i].Transition.StateBefore;
        barriers[i].Transition.StateBefore = barriers[i].Transition.StateAfter;
        barriers[i].Transition.StateAfter = t;
    }
    ID3D12VideoProcessCommandList_ResourceBarrier(s->list, 2, barriers);
    CHECK(ID3D12VideoProcessCommandList_Close(s->list), "closing the command list");

    /* After the decoder has finished the picture; the read-back waits for our signal. */
    CHECK(ID3D12CommandQueue_Wait(s->queue, in->sync_ctx.fence, in->sync_ctx.fence_value), "waiting for the decoder");
    ID3D12CommandQueue_ExecuteCommandLists(s->queue, 1, (ID3D12CommandList **)&s->list);
    CHECK(ID3D12CommandQueue_Signal(s->queue, out->sync_ctx.fence, ++out->sync_ctx.fence_value), "signalling");
    return 0;
fail:
    return -1;
}

API int32_t pc_dec_api_version(void) { return PC_DEC_API_VERSION; }

API const char *pc_dec_version(void) { return av_version_info(); }

API const char *pc_dec_last_error(void) { return last_error; }

API void pc_dec_close(PcDec *d) {
    if (!d) return;
    avcodec_free_context(&d->ctx);
    scaler_free(&d->scaler);
    av_buffer_unref(&d->device);
    av_packet_free(&d->packet);
    av_frame_free(&d->gpu);
    av_frame_free(&d->cpu);
    av_free(d);
}

/* Opens a decoder on the default GPU; NULL (see pc_dec_last_error) when it has no D3D12 video
 * decoding or no H.264 on it. */
API PcDec *pc_dec_open(void) {
    av_log_set_callback(log_callback);
    av_log_set_level(AV_LOG_ERROR);
    last_error[0] = 0;

    PcDec *d = av_mallocz(sizeof *d);
    if (!d) return NULL;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    int err = av_hwdevice_ctx_create(&d->device, AV_HWDEVICE_TYPE_D3D12VA, NULL, NULL, 0);
    if (err < 0) {
        if (!last_error[0]) set_error("no Direct3D 12 video device (%s)", av_err2str(err));
        goto fail;
    }
    d->ctx = avcodec_alloc_context3(codec);
    d->packet = av_packet_alloc();
    d->gpu = av_frame_alloc();
    d->cpu = av_frame_alloc();
    if (!d->ctx || !d->packet || !d->gpu || !d->cpu) goto fail;

    d->ctx->hw_device_ctx = av_buffer_ref(d->device);
    d->ctx->get_format = pick_d3d12;
    /* Every frame out as soon as it is in: phones send no B-frames, and a camera must not lag. */
    d->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    d->ctx->thread_count = 1;
    if ((err = avcodec_open2(d->ctx, codec, NULL)) < 0) {
        if (!last_error[0]) set_error("opening the decoder: %s", av_err2str(err));
        goto fail;
    }
    return d;
fail:
    pc_dec_close(d);
    return NULL;
}

/* Crop and scale the next pictures on the GPU: the rectangle x, y, w, h of the decoded picture,
 * stretched to out_w x out_h (all even). w == 0 turns it off. Returns 0, or -1 when the GPU
 * cannot crop (the pictures then come out whole). */
API int32_t pc_dec_set_crop(PcDec *d, int32_t x, int32_t y, int32_t w, int32_t h, int32_t out_w, int32_t out_h) {
    PcCrop c = { x, y, w, h, out_w, out_h };
    if (w <= 0 || h <= 0 || out_w <= 0 || out_h <= 0) memset(&c, 0, sizeof c);
    d->crop = c;
    return d->scaler_failed ? -1 : 0;
}

/* Feeds one Annex B access unit (SPS/PPS and a frame). 0 when taken; 1 when pictures have to be
 * taken out first (pc_dec_receive), then the same data sent again; negative on error. */
API int32_t pc_dec_send(PcDec *d, const uint8_t *data, int32_t len, int64_t pts) {
    last_error[0] = 0;
    av_packet_unref(d->packet);
    /* The decoder may read a little past the end, so the packet has padding of its own. */
    if (av_new_packet(d->packet, len) < 0) return AVERROR(ENOMEM);
    memcpy(d->packet->data, data, len);
    d->packet->pts = pts;
    int err = avcodec_send_packet(d->ctx, d->packet);
    if (err == AVERROR(EAGAIN)) return 1;
    if (err < 0) {
        if (!last_error[0]) set_error("decoding: %s", av_err2str(err));
        return err;
    }
    return 0;
}

/* Takes the next decoded picture out, copied into system memory as NV12 (cropped and scaled
 * first if asked). 1 when `out` holds one (valid until the next call), 0 when the decoder needs
 * more input, negative on error. */
API int32_t pc_dec_receive(PcDec *d, PcFrame *out) {
    last_error[0] = 0;
    av_frame_unref(d->gpu);
    int err = avcodec_receive_frame(d->ctx, d->gpu);
    if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) return 0;
    if (err < 0) {
        if (!last_error[0]) set_error("decoding: %s", av_err2str(err));
        return err;
    }
    int full_range = d->gpu->color_range != AVCOL_RANGE_MPEG; /* unspecified: phones send full range */
    int bt709 = d->gpu->colorspace == AVCOL_SPC_BT709;

    /* Crop on the GPU when asked and the rectangle fits this picture. */
    const AVFrame *src = d->gpu;
    const PcCrop *c = &d->crop;
    int crop = c->w > 0 && !d->scaler_failed && c->x >= 0 && c->y >= 0 && c->x + c->w <= d->gpu->width &&
               c->y + c->h <= d->gpu->height;
    if (crop) {
        Scaler *s = d->scaler;
        if (!s || memcmp(&s->crop, c, sizeof *c) || s->in_w != d->gpu->width || s->in_h != d->gpu->height ||
            s->full_range != full_range || s->bt709 != bt709) {
            scaler_free(&d->scaler);
            d->scaler = scaler_new(d, d->gpu->width, d->gpu->height, full_range, bt709);
        }
        if (d->scaler && scale(d->scaler, d->gpu) == 0) {
            src = d->scaler->out;
        } else {
            /* The app crops on the CPU from now on; pc_dec_last_error says why. */
            scaler_free(&d->scaler);
            d->scaler_failed = 1;
            crop = 0;
        }
    }

    av_frame_unref(d->cpu);
    d->cpu->format = AV_PIX_FMT_NV12;
    if ((err = av_hwframe_transfer_data(d->cpu, src, 0)) < 0) {
        if (!last_error[0]) set_error("reading the picture back: %s", av_err2str(err));
        return err;
    }
    if (d->cpu->format != AV_PIX_FMT_NV12) {
        set_error("picture came back as %s, not NV12", av_get_pix_fmt_name(d->cpu->format));
        return AVERROR(EINVAL);
    }
    out->width = crop ? c->out_w : d->gpu->width;
    out->height = crop ? c->out_h : d->gpu->height;
    out->y = d->cpu->data[0];
    out->y_stride = d->cpu->linesize[0];
    out->uv = d->cpu->data[1];
    out->uv_stride = d->cpu->linesize[1];
    out->full_range = full_range;
    out->bt709 = bt709;
    out->prescaled = crop;
    return 1;
}
