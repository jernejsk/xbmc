/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDVideoCodecDRMPRIME.h"

#include "ServiceBroker.h"
#include "cores/VideoPlayer/Buffers/VideoBufferDMA.h"
#include "cores/VideoPlayer/Buffers/VideoBufferDRMPRIME.h"
#include "cores/VideoPlayer/DVDCodecs/DVDCodecs.h"
#include "cores/VideoPlayer/DVDCodecs/DVDFactoryCodec.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "threads/SingleLock.h"
#include "utils/CPUInfo.h"
#include "utils/log.h"
#include "windowing/gbm/WinSystemGbm.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

using namespace KODI::WINDOWING::GBM;

namespace
{

constexpr const char* SETTING_VIDEOPLAYER_USEPRIMEDECODERFORHW{"videoplayer.useprimedecoderforhw"};

static void ReleaseBuffer(void* opaque, uint8_t* data)
{
  CVideoBufferDMA* buffer = static_cast<CVideoBufferDMA*>(opaque);
  buffer->Release();
}

static void AlignedSize(AVCodecContext* avctx, int& width, int& height)
{
  int w = width;
  int h = height;
  AVFrame picture;
  int unaligned;
  int stride_align[AV_NUM_DATA_POINTERS];

  avcodec_align_dimensions2(avctx, &w, &h, stride_align);

  do
  {
    // NOTE: do not align linesizes individually, this breaks e.g. assumptions
    // that linesize[0] == 2*linesize[1] in the MPEG-encoder for 4:2:2
    av_image_fill_linesizes(picture.linesize, avctx->pix_fmt, w);
    // increase alignment of w for next try (rhs gives the lowest bit set in w)
    w += w & ~(w - 1);

    unaligned = 0;
    for (int i = 0; i < 4; i++)
      unaligned |= picture.linesize[i] % stride_align[i];
  } while (unaligned);

  width = w;
  height = h;
}

} // namespace

CDVDVideoCodecDRMPRIME::CDVDVideoCodecDRMPRIME(CProcessInfo& processInfo)
  : CDVDVideoCodec(processInfo)
{
  m_pFrame = av_frame_alloc();
  m_pFilterFrame = av_frame_alloc();
  m_videoBufferPool = std::make_shared<CVideoBufferPoolDRMPRIMEFFmpeg>();
}

CDVDVideoCodecDRMPRIME::~CDVDVideoCodecDRMPRIME()
{
  av_frame_free(&m_pFrame);
  av_frame_free(&m_pFilterFrame);
  FilterClose();
  avcodec_free_context(&m_pCodecContext);
}

CDVDVideoCodec* CDVDVideoCodecDRMPRIME::Create(CProcessInfo& processInfo)
{
  if (CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
          CSettings::SETTING_VIDEOPLAYER_USEPRIMEDECODER))
    return new CDVDVideoCodecDRMPRIME(processInfo);
  return nullptr;
}

void CDVDVideoCodecDRMPRIME::Register()
{
  CServiceBroker::GetSettingsComponent()
      ->GetSettings()
      ->GetSetting(CSettings::SETTING_VIDEOPLAYER_USEPRIMEDECODER)
      ->SetVisible(true);
  CDVDFactoryCodec::RegisterHWVideoCodec("drm_prime", CDVDVideoCodecDRMPRIME::Create);
}

static bool IsSupportedHwFormat(const enum AVPixelFormat fmt)
{
  bool hw = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
      SETTING_VIDEOPLAYER_USEPRIMEDECODERFORHW);

  return fmt == AV_PIX_FMT_DRM_PRIME && hw;
}

static bool IsSupportedSwFormat(const enum AVPixelFormat fmt)
{
  return fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P;
}

static const AVCodecHWConfig* FindHWConfig(const AVCodec* codec)
{
  if (!CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
          SETTING_VIDEOPLAYER_USEPRIMEDECODERFORHW))
    return nullptr;

  const AVCodecHWConfig* config = nullptr;
  for (int n = 0; (config = avcodec_get_hw_config(codec, n)); n++)
  {
    if (!IsSupportedHwFormat(config->pix_fmt))
      continue;

    if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
        config->device_type == AV_HWDEVICE_TYPE_DRM)
      return config;

    if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_INTERNAL))
      return config;
  }

  return nullptr;
}

static const AVCodec* FindDecoder(CDVDStreamInfo& hints)
{
  const AVCodec* codec = nullptr;
  void* i = 0;

  if (!(hints.codecOptions & CODEC_FORCE_SOFTWARE))
    while ((codec = av_codec_iterate(&i)))
    {
      if (!av_codec_is_decoder(codec))
        continue;
      if (codec->id != hints.codec)
        continue;

      const AVCodecHWConfig* config = FindHWConfig(codec);
      if (config)
        return codec;
    }

  codec = avcodec_find_decoder(hints.codec);
  if (codec && (codec->capabilities & AV_CODEC_CAP_DR1) == AV_CODEC_CAP_DR1)
    return codec;

  return nullptr;
}

enum AVPixelFormat CDVDVideoCodecDRMPRIME::GetFormat(struct AVCodecContext* avctx,
                                                     const enum AVPixelFormat* fmt)
{
  for (int n = 0; fmt[n] != AV_PIX_FMT_NONE; n++)
  {
    if (IsSupportedHwFormat(fmt[n]) || IsSupportedSwFormat(fmt[n]))
    {
      CDVDVideoCodecDRMPRIME* ctx = static_cast<CDVDVideoCodecDRMPRIME*>(avctx->opaque);
      ctx->UpdateProcessInfo(avctx, fmt[n]);
      return fmt[n];
    }
  }

  CLog::Log(LOGERROR, "CDVDVideoCodecDRMPRIME::{} - unsupported pixel format", __FUNCTION__);
  return AV_PIX_FMT_NONE;
}

int CDVDVideoCodecDRMPRIME::GetBuffer(struct AVCodecContext* avctx, AVFrame* frame, int flags)
{
  if (IsSupportedSwFormat(static_cast<AVPixelFormat>(frame->format)))
  {
    int width = frame->width;
    int height = frame->height;

    AlignedSize(avctx, width, height);

    int size;
    switch (avctx->pix_fmt)
    {
      case AV_PIX_FMT_YUV420P:
      case AV_PIX_FMT_YUVJ420P:
        size = width * height * 3 / 2;
        break;
      default:
        return -1;
    }

    CDVDVideoCodecDRMPRIME* ctx = static_cast<CDVDVideoCodecDRMPRIME*>(avctx->opaque);
    auto buffer = dynamic_cast<CVideoBufferDMA*>(
        ctx->m_processInfo.GetVideoBufferManager().Get(avctx->pix_fmt, size, nullptr));
    if (!buffer)
      return -1;

    frame->opaque = static_cast<void*>(buffer);
    frame->opaque_ref =
        av_buffer_create(nullptr, 0, ReleaseBuffer, frame->opaque, AV_BUFFER_FLAG_READONLY);

    buffer->Export(frame, width, height);
    buffer->SyncStart();

    return 0;
  }

  return avcodec_default_get_buffer2(avctx, frame, flags);
}

bool CDVDVideoCodecDRMPRIME::Open(CDVDStreamInfo& hints, CDVDCodecOptions& options)
{
  const AVCodec* pCodec = FindDecoder(hints);
  if (!pCodec)
  {
    CLog::Log(LOGDEBUG, "CDVDVideoCodecDRMPRIME::{} - unable to find decoder for codec {}",
              __FUNCTION__, hints.codec);
    return false;
  }

  CLog::Log(LOGINFO, "CDVDVideoCodecDRMPRIME::{} - using decoder {}", __FUNCTION__,
            pCodec->long_name ? pCodec->long_name : pCodec->name);

  m_pCodecContext = avcodec_alloc_context3(pCodec);
  if (!m_pCodecContext)
    return false;

  m_hints = hints;

  const AVCodecHWConfig* pConfig = FindHWConfig(pCodec);
  if (pConfig && (pConfig->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
      pConfig->device_type == AV_HWDEVICE_TYPE_DRM)
  {
    CWinSystemGbm* winSystem = dynamic_cast<CWinSystemGbm*>(CServiceBroker::GetWinSystem());
    if (av_hwdevice_ctx_create(&m_pCodecContext->hw_device_ctx, AV_HWDEVICE_TYPE_DRM,
                               drmGetDeviceNameFromFd2(winSystem->GetDrm()->GetFileDescriptor()),
                               nullptr, 0) < 0)
    {
      CLog::Log(LOGINFO, "CDVDVideoCodecDRMPRIME::{} - unable to create hwdevice context",
                __FUNCTION__);
      avcodec_free_context(&m_pCodecContext);
      return false;
    }
  }

  m_pCodecContext->pix_fmt = AV_PIX_FMT_DRM_PRIME;
  m_pCodecContext->opaque = static_cast<void*>(this);
  m_pCodecContext->get_format = GetFormat;
  m_pCodecContext->get_buffer2 = GetBuffer;
  m_pCodecContext->codec_tag = hints.codec_tag;
  m_pCodecContext->coded_width = hints.width;
  m_pCodecContext->coded_height = hints.height;
  m_pCodecContext->bits_per_coded_sample = hints.bitsperpixel;
  m_pCodecContext->time_base.num = 1;
  m_pCodecContext->time_base.den = DVD_TIME_BASE;
  m_pCodecContext->thread_safe_callbacks = 1;
  m_pCodecContext->thread_count = CServiceBroker::GetCPUInfo()->GetCPUCount();

  if (hints.extradata && hints.extrasize > 0)
  {
    m_pCodecContext->extradata_size = hints.extrasize;
    m_pCodecContext->extradata =
        static_cast<uint8_t*>(av_mallocz(hints.extrasize + AV_INPUT_BUFFER_PADDING_SIZE));
    memcpy(m_pCodecContext->extradata, hints.extradata, hints.extrasize);
  }

  for (auto&& option : options.m_keys)
    av_opt_set(m_pCodecContext, option.m_name.c_str(), option.m_value.c_str(), 0);

  if (avcodec_open2(m_pCodecContext, pCodec, nullptr) < 0)
  {
    CLog::Log(LOGINFO, "CDVDVideoCodecDRMPRIME::{} - unable to open codec", __FUNCTION__);
    avcodec_free_context(&m_pCodecContext);
    if (hints.codecOptions & CODEC_FORCE_SOFTWARE)
      return false;

    hints.codecOptions |= CODEC_FORCE_SOFTWARE;
    return Open(hints, options);
  }

  UpdateProcessInfo(m_pCodecContext, m_pCodecContext->pix_fmt);
  m_processInfo.SetVideoInterlaced(false);

  std::list<EINTERLACEMETHOD> methods;
  methods.push_back(EINTERLACEMETHOD::VS_INTERLACEMETHOD_DEINTERLACE);
  m_processInfo.UpdateDeinterlacingMethods(methods);
  m_processInfo.SetDeinterlacingMethodDefault(EINTERLACEMETHOD::VS_INTERLACEMETHOD_DEINTERLACE);
  m_processInfo.SetVideoDeintMethod("none");
  m_processInfo.SetVideoDAR(hints.aspect);

  return true;
}

void CDVDVideoCodecDRMPRIME::UpdateProcessInfo(struct AVCodecContext* avctx,
                                               const enum AVPixelFormat pix_fmt)
{
  const char* pixFmtName = av_get_pix_fmt_name(pix_fmt);
  m_processInfo.SetVideoPixelFormat(pixFmtName ? pixFmtName : "");
  m_processInfo.SetVideoDimensions(avctx->coded_width, avctx->coded_height);

  if (avctx->codec && avctx->codec->name)
    m_name = std::string("ff-") + avctx->codec->name;
  else
    m_name = "ffmpeg";

  m_processInfo.SetVideoDecoderName(m_name + "-drm_prime", IsSupportedHwFormat(pix_fmt));
}

bool CDVDVideoCodecDRMPRIME::AddData(const DemuxPacket& packet)
{
  if (!m_pCodecContext)
    return true;

  if (!packet.pData)
    return true;

  AVPacket avpkt;
  av_init_packet(&avpkt);
  avpkt.data = packet.pData;
  avpkt.size = packet.iSize;
  avpkt.dts = (packet.dts == DVD_NOPTS_VALUE)
                  ? AV_NOPTS_VALUE
                  : static_cast<int64_t>(packet.dts / DVD_TIME_BASE * AV_TIME_BASE);
  avpkt.pts = (packet.pts == DVD_NOPTS_VALUE)
                  ? AV_NOPTS_VALUE
                  : static_cast<int64_t>(packet.pts / DVD_TIME_BASE * AV_TIME_BASE);
  avpkt.side_data = static_cast<AVPacketSideData*>(packet.pSideData);
  avpkt.side_data_elems = packet.iSideDataElems;

  int ret = avcodec_send_packet(m_pCodecContext, &avpkt);
  if (ret == AVERROR(EAGAIN))
    return false;
  else if (ret)
  {
    char err[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(ret, err, AV_ERROR_MAX_STRING_SIZE);
    CLog::Log(LOGERROR, "CDVDVideoCodecDRMPRIME::{} - send packet failed: {} ({})", __FUNCTION__,
              err, ret);
    if (ret != AVERROR_EOF && ret != AVERROR_INVALIDDATA)
      return false;
  }

  return true;
}

void CDVDVideoCodecDRMPRIME::Reset()
{
  if (!m_pCodecContext)
    return;

  Drain();

  do
  {
    int ret = avcodec_receive_frame(m_pCodecContext, m_pFrame);
    if (ret == AVERROR_EOF)
      break;
    else if (ret)
    {
      char err[AV_ERROR_MAX_STRING_SIZE] = {};
      av_strerror(ret, err, AV_ERROR_MAX_STRING_SIZE);
      CLog::Log(LOGERROR, "CDVDVideoCodecDRMPRIME::{} - receive frame failed: {} ({})",
                __FUNCTION__, err, ret);
      break;
    }
    else
      av_frame_unref(m_pFrame);
  } while (true);

  CLog::Log(LOGDEBUG, "CDVDVideoCodecDRMPRIME::{} - flush buffers", __FUNCTION__);
  avcodec_flush_buffers(m_pCodecContext);
}

void CDVDVideoCodecDRMPRIME::Drain()
{
  AVPacket avpkt;
  av_init_packet(&avpkt);
  avpkt.data = nullptr;
  avpkt.size = 0;
  int ret = avcodec_send_packet(m_pCodecContext, &avpkt);
  if (ret && ret != AVERROR_EOF)
  {
    char err[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(ret, err, AV_ERROR_MAX_STRING_SIZE);
    CLog::Log(LOGERROR, "CDVDVideoCodecDRMPRIME::{} - send packet failed: {} ({})", __FUNCTION__,
              err, ret);
  }
}

void CDVDVideoCodecDRMPRIME::SetPictureParams(VideoPicture* pVideoPicture)
{
  pVideoPicture->iWidth = m_pFrame->width;
  pVideoPicture->iHeight = m_pFrame->height;

  double aspect_ratio = 0;
  AVRational pixel_aspect = m_pFrame->sample_aspect_ratio;
  if (pixel_aspect.num)
    aspect_ratio = av_q2d(pixel_aspect) * pVideoPicture->iWidth / pVideoPicture->iHeight;

  if (aspect_ratio <= 0.0)
    aspect_ratio =
        static_cast<float>(pVideoPicture->iWidth) / static_cast<float>(pVideoPicture->iHeight);

  pVideoPicture->iDisplayWidth =
      (static_cast<int>(lrint(pVideoPicture->iHeight * aspect_ratio))) & -3;
  pVideoPicture->iDisplayHeight = pVideoPicture->iHeight;
  if (pVideoPicture->iDisplayWidth > pVideoPicture->iWidth)
  {
    pVideoPicture->iDisplayWidth = pVideoPicture->iWidth;
    pVideoPicture->iDisplayHeight =
        (static_cast<int>(lrint(pVideoPicture->iWidth / aspect_ratio))) & -3;
  }

  pVideoPicture->color_range = m_pFrame->color_range == AVCOL_RANGE_JPEG ||
                               m_pFrame->format == AV_PIX_FMT_YUVJ420P ||
                               m_hints.colorRange == AVCOL_RANGE_JPEG;
  pVideoPicture->color_primaries = m_pFrame->color_primaries == AVCOL_PRI_UNSPECIFIED
                                       ? m_hints.colorPrimaries
                                       : m_pFrame->color_primaries;
  pVideoPicture->color_transfer = m_pFrame->color_trc == AVCOL_TRC_UNSPECIFIED
                                      ? m_hints.colorTransferCharacteristic
                                      : m_pFrame->color_trc;
  pVideoPicture->color_space =
      m_pFrame->colorspace == AVCOL_SPC_UNSPECIFIED ? m_hints.colorSpace : m_pFrame->colorspace;
  pVideoPicture->chroma_position = m_pFrame->chroma_location;

  pVideoPicture->colorBits = 8;
  if (m_pCodecContext->codec_id == AV_CODEC_ID_HEVC &&
      m_pCodecContext->profile == FF_PROFILE_HEVC_MAIN_10)
    pVideoPicture->colorBits = 10;
  else if (m_pCodecContext->codec_id == AV_CODEC_ID_H264 &&
           (m_pCodecContext->profile == FF_PROFILE_H264_HIGH_10 ||
            m_pCodecContext->profile == FF_PROFILE_H264_HIGH_10_INTRA))
    pVideoPicture->colorBits = 10;

  pVideoPicture->hasDisplayMetadata = false;
  AVFrameSideData* sd = av_frame_get_side_data(m_pFrame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
  if (sd)
  {
    pVideoPicture->displayMetadata = *reinterpret_cast<AVMasteringDisplayMetadata*>(sd->data);
    pVideoPicture->hasDisplayMetadata = true;
  }
  else if (m_hints.masteringMetadata)
  {
    pVideoPicture->displayMetadata = *m_hints.masteringMetadata.get();
    pVideoPicture->hasDisplayMetadata = true;
  }

  pVideoPicture->hasLightMetadata = false;
  sd = av_frame_get_side_data(m_pFrame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
  if (sd)
  {
    pVideoPicture->lightMetadata = *reinterpret_cast<AVContentLightMetadata*>(sd->data);
    pVideoPicture->hasLightMetadata = true;
  }
  else if (m_hints.contentLightMetadata)
  {
    pVideoPicture->lightMetadata = *m_hints.contentLightMetadata.get();
    pVideoPicture->hasLightMetadata = true;
  }

  pVideoPicture->iRepeatPicture = 0;
  pVideoPicture->iFlags = 0;
  pVideoPicture->iFlags |= m_pFrame->interlaced_frame ? DVP_FLAG_INTERLACED : 0;
  pVideoPicture->iFlags |= m_pFrame->top_field_first ? DVP_FLAG_TOP_FIELD_FIRST : 0;

  int64_t pts = m_pFrame->best_effort_timestamp;
  pVideoPicture->pts = (pts == AV_NOPTS_VALUE)
                           ? DVD_NOPTS_VALUE
                           : static_cast<double>(pts) * DVD_TIME_BASE / AV_TIME_BASE;
  pVideoPicture->dts = DVD_NOPTS_VALUE;
}

int CDVDVideoCodecDRMPRIME::FilterOpen(const std::string& filters)
{
  int result;

  if (m_pFilterGraph)
    FilterClose();

  if (filters.empty())
    return 0;

  if (!(m_pFilterGraph = avfilter_graph_alloc()))
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecDRMPRIME::FilterOpen - unable to alloc filter graph");
    return -1;
  }

  const AVFilter* srcFilter = avfilter_get_by_name("buffer");
  const AVFilter* outFilter = avfilter_get_by_name("buffersink"); // should be last filter in the graph for now
  enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_DRM_PRIME, AV_PIX_FMT_NONE };

  std::string args = StringUtils::Format("video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:"
                                         "pixel_aspect=%d/%d:sws_param=flags=2",
                                         m_pCodecContext->width,
                                         m_pCodecContext->height,
                                         m_pCodecContext->pix_fmt,
                                         m_pCodecContext->time_base.num ? m_pCodecContext->time_base.num : 1,
                                         m_pCodecContext->time_base.num ? m_pCodecContext->time_base.den : 1,
                                         m_pCodecContext->sample_aspect_ratio.num != 0 ? m_pCodecContext->sample_aspect_ratio.num : 1,
                                         m_pCodecContext->sample_aspect_ratio.num != 0 ? m_pCodecContext->sample_aspect_ratio.den : 1);

  if ((result = avfilter_graph_create_filter(&m_pFilterIn, srcFilter, "src", args.c_str(), NULL, m_pFilterGraph)) < 0)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecDRMPRIME::FilterOpen - avfilter_graph_create_filter: src");
    return result;
  }

  AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
  if (!par)
    return AVERROR(ENOMEM);

  memset(par, 0, sizeof(*par));
  par->format = AV_PIX_FMT_NONE;
  par->hw_frames_ctx = m_pFrame->hw_frames_ctx;

  result = av_buffersrc_parameters_set(m_pFilterIn, par);
  if (result < 0)
  {
    char err[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(result, err, AV_ERROR_MAX_STRING_SIZE);
    CLog::Log(LOGERROR, "CDVDVideoCodecDRMPRIME::FilterOpen - av_buffersrc_parameters_set:  {} ({})", err, result);
    return result;
  }
  av_freep(&par);

  if ((result = avfilter_graph_create_filter(&m_pFilterOut, outFilter, "out", NULL, NULL, m_pFilterGraph)) < 0)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecDRMPRIME::FilterOpen - avfilter_graph_create_filter: out");
    return result;
  }
  if ((result = av_opt_set_int_list(m_pFilterOut, "pix_fmts", &pix_fmts[0],  AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecDRMPRIME::FilterOpen - failed settings pix formats");
    return result;
  }

  AVFilterInOut* outputs = avfilter_inout_alloc();
  AVFilterInOut* inputs  = avfilter_inout_alloc();

  outputs->name = av_strdup("in");
  outputs->filter_ctx = m_pFilterIn;
  outputs->pad_idx = 0;
  outputs->next = nullptr;

  inputs->name = av_strdup("out");
  inputs->filter_ctx = m_pFilterOut;
  inputs->pad_idx = 0;
  inputs->next = nullptr;

  result = avfilter_graph_parse_ptr(m_pFilterGraph, filters.c_str(), &inputs, &outputs, NULL);
  avfilter_inout_free(&outputs);
  avfilter_inout_free(&inputs);

  if (result < 0)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecDRMPRIME::FilterOpen - avfilter_graph_parse");
    return result;
  }

  if (filters.compare(0,11,"deinterlace") == 0)
  {
    m_processInfo.SetVideoDeintMethod(filters);
  }
  else
  {
    m_processInfo.SetVideoDeintMethod("none");
  }

  if ((result = avfilter_graph_config(m_pFilterGraph,  nullptr)) < 0)
  {
    char err[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(result, err, AV_ERROR_MAX_STRING_SIZE);
    CLog::Log(LOGERROR, "CDVDVideoCodecDRMPRIME::FilterOpen - avfilter_graph_config:  {} ({})", err, result);
    return result;
  }

  if (CServiceBroker::GetLogging().CanLogComponent(LOGVIDEO))
  {
    char* graphDump = avfilter_graph_dump(m_pFilterGraph, nullptr);
    if (graphDump)
    {
      CLog::Log(LOGDEBUG, "CDVDVideoCodecDRMPRIME::FilterOpen - Final filter graph:\n%s", graphDump);
      av_freep(&graphDump);
    }
  }

  return result;
}

void CDVDVideoCodecDRMPRIME::FilterClose()
{
  if (m_pFilterGraph)
  {
    CLog::Log(LOGDEBUG, LOGVIDEO, "CDVDVideoCodecDRMPRIME::FilterClose - Freeing filter graph");
    avfilter_graph_free(&m_pFilterGraph);

    // Disposed by above code
    m_pFilterIn = nullptr;
    m_pFilterOut = nullptr;
  }
}

CDVDVideoCodec::VCReturn CDVDVideoCodecDRMPRIME::ProcessFilterIn()
{
  if (!m_pFilterIn)
    return VC_PICTURE;

  int ret = av_buffersrc_add_frame(m_pFilterIn, m_pFrame);
  if (ret < 0)
  {
    char err[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(ret, err, AV_ERROR_MAX_STRING_SIZE);
    CLog::Log(LOGERROR, "CDVDVideoCodecDRMPRIME::{} - buffersrc add frame failed: {} ({})",
              __FUNCTION__, err, ret);
    return VC_ERROR;
  }

  return ProcessFilterOut();
}

CDVDVideoCodec::VCReturn CDVDVideoCodecDRMPRIME::ProcessFilterOut()
{
  if (!m_pFilterOut)
    return VC_EOF;

  int ret = av_buffersink_get_frame(m_pFilterOut, m_pFilterFrame);
  if (ret == AVERROR(EAGAIN))
    return VC_BUFFER;
  else if (ret == AVERROR_EOF)
  {
    if (m_codecControlFlags & DVD_CODEC_CTRL_DRAIN)
    {
      CLog::Log(LOGDEBUG, "CDVDVideoCodecDRMPRIME::{} - flush buffers", __FUNCTION__);
      avcodec_flush_buffers(m_pCodecContext);
      SetCodecControl(m_codecControlFlags & ~DVD_CODEC_CTRL_DRAIN);
    }
    return VC_EOF;
  }
  else if (ret)
  {
    char err[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(ret, err, AV_ERROR_MAX_STRING_SIZE);
    CLog::Log(LOGERROR, "CDVDVideoCodecDRMPRIME::{} - buffersink get frame failed: {} ({})",
              __FUNCTION__, err, ret);
    return VC_ERROR;
  }

  av_frame_unref(m_pFrame);
  av_frame_move_ref(m_pFrame, m_pFilterFrame);

  return VC_PICTURE;
}

CDVDVideoCodec::VCReturn CDVDVideoCodecDRMPRIME::GetPicture(VideoPicture* pVideoPicture)
{
  if (m_codecControlFlags & DVD_CODEC_CTRL_DRAIN)
    Drain();

  if (pVideoPicture->videoBuffer)
  {
    pVideoPicture->videoBuffer->Release();
    pVideoPicture->videoBuffer = nullptr;
  }

  auto result = ProcessFilterOut();
  if (result != VC_PICTURE)
  {
    int ret = avcodec_receive_frame(m_pCodecContext, m_pFrame);
    if (ret == AVERROR(EAGAIN))
      return VC_BUFFER;
    else if (ret == AVERROR_EOF)
      return VC_EOF;
    else if (ret)
    {
      char err[AV_ERROR_MAX_STRING_SIZE] = {};
      av_strerror(ret, err, AV_ERROR_MAX_STRING_SIZE);
      CLog::Log(LOGERROR, "CDVDVideoCodecDRMPRIME::{} - receive frame failed: {} ({})",
                __FUNCTION__, err, ret);
      return VC_ERROR;
    }

    if (m_pFrame->interlaced_frame)
      m_interlaced = true;
    else
      m_interlaced = false;

    if (!m_processInfo.GetVideoInterlaced() && m_interlaced)
      m_processInfo.SetVideoInterlaced(m_interlaced);

    if (!m_interlaced && m_pFilterGraph)
      FilterClose();

    bool need_reopen = false;
    if (m_interlaced && !m_pFilterGraph)
      need_reopen = true;

    if (m_pFilterIn)
    {
      if (m_pFilterIn->outputs[0]->w != m_pCodecContext->width ||
          m_pFilterIn->outputs[0]->h != m_pCodecContext->height)
        need_reopen = true;
    }

    // try to setup new filters
    if (need_reopen)
    {
      if (FilterOpen("deinterlace_v4l2m2m") < 0)
        FilterClose();
    }

    result = ProcessFilterIn();
    if (result != VC_PICTURE)
      return result;
  }

  SetPictureParams(pVideoPicture);

  if (IsSupportedHwFormat(static_cast<AVPixelFormat>(m_pFrame->format)))
  {
    CVideoBufferDRMPRIMEFFmpeg* buffer =
        dynamic_cast<CVideoBufferDRMPRIMEFFmpeg*>(m_videoBufferPool->Get());
    buffer->SetPictureParams(*pVideoPicture);
    buffer->SetRef(m_pFrame);
    pVideoPicture->videoBuffer = buffer;
  }
  else if (m_pFrame->opaque)
  {
    CVideoBufferDMA* buffer = static_cast<CVideoBufferDMA*>(m_pFrame->opaque);
    buffer->SetPictureParams(*pVideoPicture);
    buffer->Acquire();
    buffer->SyncEnd();
    buffer->SetDimensions(m_pFrame->width, m_pFrame->height);

    pVideoPicture->videoBuffer = buffer;
    av_frame_unref(m_pFrame);
  }

  if (!pVideoPicture->videoBuffer)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecDRMPRIME::{} - videoBuffer:nullptr format:{}", __FUNCTION__,
              av_get_pix_fmt_name(static_cast<AVPixelFormat>(m_pFrame->format)));
    av_frame_unref(m_pFrame);
    return VC_ERROR;
  }

  return VC_PICTURE;
}
