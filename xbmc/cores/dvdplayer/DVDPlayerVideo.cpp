/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "system.h"
#include "cores/VideoRenderers/RenderFlags.h"
#include "windowing/WindowingFactory.h"
#include "settings/AdvancedSettings.h"
#include "settings/MediaSettings.h"
#include "settings/Settings.h"
#include "video/VideoReferenceClock.h"
#include "utils/MathUtils.h"
#include "DVDPlayer.h"
#include "DVDPlayerVideo.h"
#include "DVDCodecs/DVDFactoryCodec.h"
#include "DVDCodecs/DVDCodecUtils.h"
#include "DVDCodecs/Video/DVDVideoPPFFmpeg.h"
#include "DVDCodecs/Video/DVDVideoCodecFFmpeg.h"
#include "DVDDemuxers/DVDDemux.h"
#include "DVDOverlayRenderer.h"
#include "guilib/GraphicContext.h"
#include <sstream>
#include <iomanip>
#include <numeric>
#include <iterator>
#include "utils/log.h"

extern "C" {
#include "libavutil/pixdesc.h"
}

using namespace RenderManager;

class CDVDMsgVideoCodecChange : public CDVDMsg
{
public:
  CDVDMsgVideoCodecChange(const CDVDStreamInfo &hints, CDVDVideoCodec* codec)
    : CDVDMsg(GENERAL_STREAMCHANGE)
    , m_codec(codec)
    , m_hints(hints)
  {}
 ~CDVDMsgVideoCodecChange()
  {
    SAFE_DELETE(m_codec);
  }
  CDVDVideoCodec* m_codec;
  CDVDStreamInfo  m_hints;
};


CDVDPlayerVideo::CDVDPlayerVideo( CDVDClock* pClock
                                , CDVDOverlayContainer* pOverlayContainer
                                , CDVDMessageQueue& parent)
: CThread("DVDPlayerVideo")
, m_messageQueue("video")
, m_messageParent(parent)
{
  m_pClock = pClock;
  m_pOverlayContainer = pOverlayContainer;
  m_pTempOverlayPicture = NULL;
  m_pVideoCodec = NULL;
  m_speed = DVD_PLAYSPEED_NORMAL;

  m_bRenderSubs = false;
  m_stalled = false;
  m_paused = false;
  m_syncState = IDVDStreamPlayer::SYNC_STARTING;
  m_iVideoDelay = 0;
  m_iSubtitleDelay = 0;
  m_FlipTimeStamp = 0.0;
  m_FlipTimePts = 0.0f; //silence coverity uninitialized warning, is set elsewhere
  m_iLateFrames = 0;
  m_iDroppedRequest = 0;
  m_fForcedAspectRatio = 0;
  m_messageQueue.SetMaxDataSize(40 * 1024 * 1024);
  m_messageQueue.SetMaxTimeSize(8.0);

  m_iDroppedFrames = 0;
  m_fFrameRate = 25;
  m_bCalcFrameRate = false;
  m_fStableFrameRate = 0.0;
  m_iFrameRateCount = 0;
  m_bAllowDrop = false;
  m_iFrameRateErr = 0;
  m_iFrameRateLength = 0;
  m_bFpsInvalid = false;
  m_bAllowFullscreen = false;
  memset(&m_output, 0, sizeof(m_output));
}

CDVDPlayerVideo::~CDVDPlayerVideo()
{
  m_bAbortOutput = true;
  StopThread();
  g_VideoReferenceClock.Stop();
}

double CDVDPlayerVideo::GetOutputDelay()
{
    double time = m_messageQueue.GetPacketCount(CDVDMsg::DEMUXER_PACKET);
    if( m_fFrameRate )
      time = (time * DVD_TIME_BASE) / m_fFrameRate;
    else
      time = 0.0;

    if( m_speed != 0 )
      time = time * DVD_PLAYSPEED_NORMAL / abs(m_speed);

    return time;
}

bool CDVDPlayerVideo::OpenStream( CDVDStreamInfo &hint )
{
  CRenderInfo info;
  #ifdef HAS_VIDEO_PLAYBACK
  info = g_renderManager.GetRenderInfo();
  #endif

  m_pullupCorrection.ResetVFRDetection();
  if(hint.flags & AV_DISPOSITION_ATTACHED_PIC)
    return false;

  CLog::Log(LOGNOTICE, "Creating video codec with codec id: %i", hint.codec);
  CDVDVideoCodec* codec = CDVDFactoryCodec::CreateVideoCodec(hint, info);
  if(!codec)
  {
    CLog::Log(LOGERROR, "Unsupported video codec");
    return false;
  }

  g_VideoReferenceClock.Start();

  if(m_messageQueue.IsInited())
    m_messageQueue.Put(new CDVDMsgVideoCodecChange(hint, codec), 0);
  else
  {
    OpenStream(hint, codec);
    CLog::Log(LOGNOTICE, "Creating video thread");
    m_messageQueue.Init();
    Create();
  }
  return true;
}

void CDVDPlayerVideo::OpenStream(CDVDStreamInfo &hint, CDVDVideoCodec* codec)
{
  //reported fps is usually not completely correct
  if (hint.fpsrate && hint.fpsscale)
  {
    m_fFrameRate = DVD_TIME_BASE / CDVDCodecUtils::NormalizeFrameduration((double)DVD_TIME_BASE * hint.fpsscale / hint.fpsrate);
    m_bFpsInvalid = false;
  }
  else
  {
    m_fFrameRate = 25;
    m_bFpsInvalid = true;
  }

  m_pullupCorrection.ResetVFRDetection();
  m_bCalcFrameRate = CSettings::GetInstance().GetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK) ||
                     CSettings::GetInstance().GetInt(CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE) != ADJUST_REFRESHRATE_OFF;
  ResetFrameRateCalc();

  m_iDroppedRequest = 0;
  m_iLateFrames = 0;

  if( m_fFrameRate > 120 || m_fFrameRate < 5 )
  {
    CLog::Log(LOGERROR, "CDVDPlayerVideo::OpenStream - Invalid framerate %d, using forced 25fps and just trust timestamps", (int)m_fFrameRate);
    m_fFrameRate = 25;
  }

  // use aspect in stream if available
  if(hint.forced_aspect)
    m_fForcedAspectRatio = hint.aspect;
  else
    m_fForcedAspectRatio = 0.0;

  if (m_pVideoCodec)
  {
    m_pVideoCodec->ClearPicture(&m_picture);
    m_pVideoCodec->Dispose();
    SAFE_DELETE(m_pVideoCodec);
  }

  m_pVideoCodec = codec;
  m_pVideoCodec->SetClock(m_pClock);
  m_hints   = hint;
  m_stalled = m_messageQueue.GetPacketCount(CDVDMsg::DEMUXER_PACKET) == 0;
  m_codecname = m_pVideoCodec->GetName();
  m_packets.clear();
  m_syncState = IDVDStreamPlayer::SYNC_STARTING;
}

void CDVDPlayerVideo::CloseStream(bool bWaitForBuffers)
{
  // wait until buffers are empty
  if (bWaitForBuffers && m_speed > 0)
  {
    m_messageQueue.Put(new CDVDMsg(CDVDMsg::VIDEO_DRAIN), 0);
    m_messageQueue.WaitUntilEmpty();
  }

  m_messageQueue.Abort();

  // wait for decode_video thread to end
  CLog::Log(LOGNOTICE, "waiting for video thread to exit");

  //m_pVideoCodec->Reset();
  m_bAbortOutput = true;
  StopThread();

  m_messageQueue.End();

  CLog::Log(LOGNOTICE, "deleting video codec");
  if (m_pVideoCodec)
  {
    m_pVideoCodec->ClearPicture(&m_picture);
    m_pVideoCodec->Dispose();
    SAFE_DELETE(m_pVideoCodec);
  }

  if (m_pTempOverlayPicture)
  {
    CDVDCodecUtils::FreePicture(m_pTempOverlayPicture);
    m_pTempOverlayPicture = NULL;
  }
}

bool CDVDPlayerVideo::AcceptsData() const
{
  bool full = m_messageQueue.IsFull();
  return !full;
}

void CDVDPlayerVideo::OnStartup()
{
  m_iDroppedFrames = 0;
  m_FlipTimeStamp = m_pClock->GetAbsoluteClock();
  m_FlipTimePts = 0.0;
}

void CDVDPlayerVideo::Process()
{
  CLog::Log(LOGNOTICE, "running thread: video_thread");

  memset(&m_picture, 0, sizeof(DVDVideoPicture));

  double pts = 0;
  double frametime = (double)DVD_TIME_BASE / m_fFrameRate;

  int iDropped = 0; //frames dropped in a row
  bool bRequestDrop = false;
  int iDropDirective;

  m_videoStats.Start();
  m_droppingStats.Reset();
  m_iDroppedFrames = 0;

  while (!m_bStop)
  {
    int iQueueTimeOut = (int)(m_stalled ? frametime : frametime * 10) / 1000;
    int iPriority = (m_speed == DVD_PLAYSPEED_PAUSE && m_syncState == IDVDStreamPlayer::SYNC_INSYNC) ? 1 : 0;

    if (m_syncState == IDVDStreamPlayer::SYNC_WAITSYNC)
      iPriority = 1;

    if (m_paused)
      iPriority = 1;

    CDVDMsg* pMsg;
    MsgQueueReturnCode ret = m_messageQueue.Get(&pMsg, iQueueTimeOut, iPriority);

    if (MSGQ_IS_ERROR(ret))
    {
      CLog::Log(LOGERROR, "Got MSGQ_ABORT or MSGO_IS_ERROR return true");
      break;
    }
    else if (ret == MSGQ_TIMEOUT)
    {
      // if we only wanted priority messages, this isn't a stall
      if( iPriority )
        continue;

      // check if decoder has produced some output
      m_pVideoCodec->SetCodecControl(DVD_CODEC_CTRL_DRAIN);
      int decoderState = m_pVideoCodec->Decode(NULL, 0, DVD_NOPTS_VALUE, DVD_NOPTS_VALUE);
      ProcessDecoderOutput(decoderState, frametime, pts);

      //Okey, start rendering at stream fps now instead, we are likely in a stillframe
      if (!m_stalled)
      {
        if(m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
          CLog::Log(LOGINFO, "CDVDPlayerVideo - Stillframe detected, switching to forced %f fps", m_fFrameRate);
        m_stalled = true;
        pts += frametime * 4;
      }

      //Waiting timed out, output last picture
      if (m_picture.iFlags & DVP_FLAG_ALLOCATED)
      {
        OutputPicture(&m_picture, pts);
        pts += frametime;
      }

      continue;
    }

    if (pMsg->IsType(CDVDMsg::GENERAL_SYNCHRONIZE))
    {
      if(((CDVDMsgGeneralSynchronize*)pMsg)->Wait(100, SYNCSOURCE_VIDEO))
      {
        CLog::Log(LOGDEBUG, "CDVDPlayerVideo - CDVDMsg::GENERAL_SYNCHRONIZE");
      }
      else
        m_messageQueue.Put(pMsg->Acquire(), 1); /* push back as prio message, to process other prio messages */
      m_droppingStats.Reset();
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESYNC))
    {
      pts = static_cast<CDVDMsgDouble*>(pMsg)->m_value;

      m_FlipTimePts = pts -frametime;
      m_syncState = IDVDStreamPlayer::SYNC_INSYNC;
      m_droppingStats.Reset();

      CLog::Log(LOGDEBUG, "CDVDPlayerVideo - CDVDMsg::GENERAL_RESYNC(%f)", pts);
    }
    else if (pMsg->IsType(CDVDMsg::VIDEO_SET_ASPECT))
    {
      CLog::Log(LOGDEBUG, "CDVDPlayerVideo - CDVDMsg::VIDEO_SET_ASPECT");
      m_fForcedAspectRatio = *((CDVDMsgDouble*)pMsg);
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESET))
    {
      if(m_pVideoCodec)
        m_pVideoCodec->Reset();
      m_picture.iFlags &= ~DVP_FLAG_ALLOCATED;
      m_packets.clear();
      m_droppingStats.Reset();
      m_syncState = IDVDStreamPlayer::SYNC_STARTING;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_FLUSH)) // private message sent by (CDVDPlayerVideo::Flush())
    {
      bool sync = static_cast<CDVDMsgBool*>(pMsg)->m_value;
      if(m_pVideoCodec)
        m_pVideoCodec->Reset();
      m_picture.iFlags &= ~DVP_FLAG_ALLOCATED;
      m_packets.clear();
      pts = 0;

      m_pullupCorrection.Flush();
      //we need to recalculate the framerate
      //TODO: this needs to be set on a streamchange instead
      ResetFrameRateCalc();
      m_droppingStats.Reset();

      m_stalled = true;
      if (sync)
        m_syncState = IDVDStreamPlayer::SYNC_STARTING;

      g_renderManager.DiscardBuffer();
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SETSPEED))
    {
      m_speed = static_cast<CDVDMsgInt*>(pMsg)->m_value;
      if (m_pVideoCodec)
        m_pVideoCodec->SetSpeed(m_speed);
      m_droppingStats.Reset();
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_STREAMCHANGE))
    {
      CDVDMsgVideoCodecChange* msg(static_cast<CDVDMsgVideoCodecChange*>(pMsg));

      while (!m_bStop && m_pVideoCodec)
      {
        m_pVideoCodec->SetCodecControl(DVD_CODEC_CTRL_DRAIN);
        int decoderState = m_pVideoCodec->Decode(NULL, 0, DVD_NOPTS_VALUE, DVD_NOPTS_VALUE);

        bool cont = ProcessDecoderOutput(decoderState, frametime, pts);
        if (!cont)
          break;

        if (decoderState & VC_BUFFER)
          break;
      }

      OpenStream(msg->m_hints, msg->m_codec);
      msg->m_codec = NULL;
      m_picture.iFlags &= ~DVP_FLAG_ALLOCATED;
    }
    else if (pMsg->IsType(CDVDMsg::VIDEO_DRAIN))
    {
      while (!m_bStop && m_pVideoCodec)
      {
        m_pVideoCodec->SetCodecControl(DVD_CODEC_CTRL_DRAIN);
        int decoderState = m_pVideoCodec->Decode(NULL, 0, DVD_NOPTS_VALUE, DVD_NOPTS_VALUE);

        bool cont = ProcessDecoderOutput(decoderState, frametime, pts);

        if (!cont)
          break;

        if (decoderState & VC_BUFFER)
          break;
      }
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_PAUSE))
    {
      m_paused = static_cast<CDVDMsgBool*>(pMsg)->m_value;
      CLog::Log(LOGDEBUG, "CDVDPlayerVideo - CDVDMsg::GENERAL_PAUSE: %d", m_paused);
    }
    else if (pMsg->IsType(CDVDMsg::DEMUXER_PACKET))
    {
      DemuxPacket* pPacket = ((CDVDMsgDemuxerPacket*)pMsg)->GetPacket();
      bool bPacketDrop     = ((CDVDMsgDemuxerPacket*)pMsg)->GetPacketDrop();

      if (m_stalled)
      {
        CLog::Log(LOGINFO, "CDVDPlayerVideo - Stillframe left, switching to normal playback");
        m_stalled = false;
      }

      bRequestDrop = false;
      iDropDirective = CalcDropRequirement(pts);
      if (iDropDirective & EOS_VERYLATE)
      {
        if (m_bAllowDrop)
        {
          bRequestDrop = true;
        }
      }
      int codecControl = 0;
      if (iDropDirective & EOS_BUFFER_LEVEL)
        codecControl |= DVD_CODEC_CTRL_HURRY;
      if (m_speed > DVD_PLAYSPEED_NORMAL)
        codecControl |= DVD_CODEC_CTRL_NO_POSTPROC;
      if (bPacketDrop)
        codecControl |= DVD_CODEC_CTRL_DROP;
      if (!g_renderManager.Supports(RENDERFEATURE_ROTATION))
        codecControl |= DVD_CODEC_CTRL_ROTATE;
      m_pVideoCodec->SetCodecControl(codecControl);
      if (iDropDirective & EOS_DROPPED)
      {
        m_iDroppedFrames++;
        iDropped++;
        m_pullupCorrection.Flush();
      }

      if (m_messageQueue.GetDataSize() == 0
      ||  m_speed < 0)
      {
        bRequestDrop = false;
        m_iDroppedRequest = 0;
        m_iLateFrames     = 0;
      }

      // if player want's us to drop this packet, do so nomatter what
      if(bPacketDrop)
        bRequestDrop = true;

      // tell codec if next frame should be dropped
      // problem here, if one packet contains more than one frame
      // both frames will be dropped in that case instead of just the first
      // decoder still needs to provide an empty image structure, with correct flags
      m_pVideoCodec->SetDropState(bRequestDrop);
      int iDecoderState = m_pVideoCodec->Decode(pPacket->pData, pPacket->iSize, pPacket->dts, pPacket->pts);

      // buffer packets so we can recover should decoder flush for some reason
      if(m_pVideoCodec->GetConvergeCount() > 0)
      {
        m_packets.emplace_back(pMsg, 0);
        if(m_packets.size() > m_pVideoCodec->GetConvergeCount()
        || m_packets.size() * frametime > DVD_SEC_TO_TIME(10))
          m_packets.pop_front();
      }

      m_videoStats.AddSampleBytes(pPacket->iSize);

      // reset the request, the following while loop may break before
      // setting the flag to a new value
      bRequestDrop = false;

      // loop while no error and decoder produces pics
      while (!m_bStop)
      {
        int dropped = m_iDroppedFrames;
        bool cont = ProcessDecoderOutput(iDecoderState, frametime, pts);
        iDropped += m_iDroppedFrames - dropped;

        if (!cont)
          break;

        if (iDecoderState & VC_BUFFER)
          break;

        // the decoder didn't need more data, flush the remaning buffer
        iDecoderState = m_pVideoCodec->Decode(NULL, 0, DVD_NOPTS_VALUE, DVD_NOPTS_VALUE);
      }
    }

    // all data is used by the decoder, we can safely free it now
    pMsg->Release();
  }

  // we need to let decoder release any picture retained resources.
  m_pVideoCodec->ClearPicture(&m_picture);
}

bool CDVDPlayerVideo::ProcessDecoderOutput(int &decoderState, double &frametime, double &pts)
{
  std::string sPostProcessType;
  bool bPostProcessDeint = false;
  CDVDVideoPPFFmpeg mPostProcess("");

  if (decoderState & VC_SWFALLBACK)
  {
    CLog::Log(LOGDEBUG, "CDVDPlayerVideo - video decoder failed, fallback to software decode");
    m_hints.software = true;
    CDVDVideoCodec* codec = CDVDFactoryCodec::CreateVideoCodec(m_hints, g_renderManager.GetRenderInfo());
    m_messageQueue.Put(new CDVDMsgVideoCodecChange(m_hints, codec), 20);

    while (!m_packets.empty())
    {
      CDVDMsgDemuxerPacket* msg = (CDVDMsgDemuxerPacket*)m_packets.front().message->Acquire();
      m_packets.pop_front();
      m_messageQueue.Put(msg, 10);
    }
    m_packets.clear();

    return false;
  }

  // if decoder was flushed, we need to seek back again to resume rendering
  if (decoderState & VC_FLUSHED)
  {
    CLog::Log(LOGDEBUG, "CDVDPlayerVideo - video decoder was flushed");
    while (!m_packets.empty())
    {
      CDVDMsgDemuxerPacket* msg = (CDVDMsgDemuxerPacket*)m_packets.front().message->Acquire();
      m_packets.pop_front();

      m_messageQueue.Put(msg, 10);
    }

    m_pVideoCodec->Reset();
    m_packets.clear();
    //picture.iFlags &= ~DVP_FLAG_ALLOCATED;
    g_renderManager.DiscardBuffer();
    return false;
  }

  if (decoderState & VC_REOPEN)
  {
    while (!m_packets.empty())
    {
      CDVDMsgDemuxerPacket* msg = (CDVDMsgDemuxerPacket*)m_packets.front().message->Acquire();
      m_packets.pop_front();
      m_messageQueue.Put(msg, 10);
    }

    m_pVideoCodec->Reopen();
    m_packets.clear();
    //picture.iFlags &= ~DVP_FLAG_ALLOCATED;
    g_renderManager.DiscardBuffer();
    return false;
  }

  // if decoder had an error, tell it to reset to avoid more problems
  if (decoderState & VC_ERROR)
  {
    CLog::Log(LOGDEBUG, "CDVDPlayerVideo - video decoder returned error");
    return false;
  }

  // check for a new picture
  if (decoderState & VC_PICTURE)
  {
    // try to retrieve the picture (should never fail!), unless there is a demuxer bug ofcours
    m_pVideoCodec->ClearPicture(&m_picture);
    if (m_pVideoCodec->GetPicture(&m_picture))
    {
      bool hasTimestamp = true;

      sPostProcessType.clear();

      if (m_picture.iDuration == 0.0)
        m_picture.iDuration = frametime;

      // validate picture timing,
      // if both dts/pts invalid, use pts calulated from picture.iDuration
      // if pts invalid use dts, else use picture.pts as passed
      if (m_picture.dts == DVD_NOPTS_VALUE && m_picture.pts == DVD_NOPTS_VALUE)
      {
        m_picture.pts = pts;
        hasTimestamp = false;
      }
      else if (m_picture.pts == DVD_NOPTS_VALUE)
        m_picture.pts = m_picture.dts;

      /* use forced aspect if any */
      if( m_fForcedAspectRatio != 0.0f )
        m_picture.iDisplayWidth = (int) (m_picture.iDisplayHeight * m_fForcedAspectRatio);

      if (CMediaSettings::GetInstance().GetCurrentVideoSettings().m_PostProcess)
      {
        if (!sPostProcessType.empty())
          sPostProcessType += ",";
        // This is what mplayer uses for its "high-quality filter combination"
        sPostProcessType += g_advancedSettings.m_videoPPFFmpegPostProc;
      }

      if (!sPostProcessType.empty())
      {
        mPostProcess.SetType(sPostProcessType, bPostProcessDeint);
        if (mPostProcess.Process(&m_picture))
          mPostProcess.GetPicture(&m_picture);
      }

      /* if frame has a pts (usually originiating from demux packet), use that */
      if (m_picture.pts != DVD_NOPTS_VALUE)
      {
        pts = m_picture.pts;
      }

      double extraDelay = 0.0;
      if (m_picture.iRepeatPicture)
      {
        extraDelay = m_picture.iRepeatPicture * m_picture.iDuration;
        m_picture.iDuration += extraDelay;
      }

      int iResult = OutputPicture(&m_picture, pts + extraDelay);

      frametime = (double)DVD_TIME_BASE / m_fFrameRate;

      if (m_syncState == IDVDStreamPlayer::SYNC_STARTING && !(m_picture.iFlags & DVP_FLAG_DROPPED))
      {
        m_codecname = m_pVideoCodec->GetName();
        m_syncState = IDVDStreamPlayer::SYNC_WAITSYNC;
        SStartMsg msg;
        msg.player = DVDPLAYER_VIDEO;
        msg.cachetime = DVD_MSEC_TO_TIME(50); // TODO
        msg.cachetotal = DVD_MSEC_TO_TIME(100); // TODO
        msg.timestamp = hasTimestamp ? pts : DVD_NOPTS_VALUE;
        m_messageParent.Put(new CDVDMsgType<SStartMsg>(CDVDMsg::PLAYER_STARTED, msg));
      }

      // guess next frame pts. iDuration is always valid
      if (m_speed != 0)
        pts += m_picture.iDuration * m_speed / abs(m_speed);

      if (iResult & EOS_ABORT)
      {
        //if we break here and we directly try to decode again wihout
        //flushing the video codec things break for some reason
        //i think the decoder (libmpeg2 atleast) still has a pointer
        //to the data, and when the packet is freed that will fail.
        decoderState = m_pVideoCodec->Decode(NULL, 0, DVD_NOPTS_VALUE, DVD_NOPTS_VALUE);
        return false;
      }

      if ((iResult & EOS_DROPPED) && !(m_picture.iFlags & DVP_FLAG_DROPPED))
      {
        m_iDroppedFrames++;
        m_pullupCorrection.Flush();
      }
    }
    else
    {
      CLog::Log(LOGWARNING, "Decoder Error getting videoPicture.");
      m_pVideoCodec->Reset();
    }
  }

  return true;
}

void CDVDPlayerVideo::OnExit()
{
  CLog::Log(LOGNOTICE, "thread end: video_thread");
}

void CDVDPlayerVideo::SetSpeed(int speed)
{
  if(m_messageQueue.IsInited())
    m_messageQueue.Put( new CDVDMsgInt(CDVDMsg::PLAYER_SETSPEED, speed), 1 );
  else
    m_speed = speed;
}

void CDVDPlayerVideo::Flush(bool sync)
{
  /* flush using message as this get's called from dvdplayer thread */
  /* and any demux packet that has been taken out of queue need to */
  /* be disposed of before we flush */
  m_messageQueue.Flush();
  m_messageQueue.Put(new CDVDMsgBool(CDVDMsg::GENERAL_FLUSH, sync), 1);
  m_bAbortOutput = true;
}

#ifdef HAS_VIDEO_PLAYBACK
void CDVDPlayerVideo::ProcessOverlays(DVDVideoPicture* pSource, double pts)
{
  // remove any overlays that are out of time
  if (m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
    m_pOverlayContainer->CleanUp(pts - m_iSubtitleDelay);

  enum EOverlay
  { OVERLAY_AUTO // select mode auto
  , OVERLAY_GPU  // render osd using gpu
  , OVERLAY_BUF  // render osd on buffer
  } render = OVERLAY_AUTO;

  if(pSource->format == RENDER_FMT_YUV420P)
  {
    if(g_Windowing.GetRenderQuirks() & RENDER_QUIRKS_MAJORMEMLEAK_OVERLAYRENDERER)
    {
      // for now use cpu for ssa overlays as it currently allocates and
      // frees textures for each frame this causes a hugh memory leak
      // on some mesa intel drivers

      if(m_pOverlayContainer->ContainsOverlayType(DVDOVERLAY_TYPE_SPU)
      || m_pOverlayContainer->ContainsOverlayType(DVDOVERLAY_TYPE_IMAGE)
      || m_pOverlayContainer->ContainsOverlayType(DVDOVERLAY_TYPE_SSA) )
        render = OVERLAY_BUF;
    }

    if(render == OVERLAY_BUF)
    {
      // rendering spu overlay types directly on video memory costs a lot of processing power.
      // thus we allocate a temp picture, copy the original to it (needed because the same picture can be used more than once).
      // then do all the rendering on that temp picture and finaly copy it to video memory.
      // In almost all cases this is 5 or more times faster!.

      if(m_pTempOverlayPicture && ( m_pTempOverlayPicture->iWidth  != pSource->iWidth
                                 || m_pTempOverlayPicture->iHeight != pSource->iHeight))
      {
        CDVDCodecUtils::FreePicture(m_pTempOverlayPicture);
        m_pTempOverlayPicture = NULL;
      }

      if(!m_pTempOverlayPicture)
        m_pTempOverlayPicture = CDVDCodecUtils::AllocatePicture(pSource->iWidth, pSource->iHeight);
      if(!m_pTempOverlayPicture)
        return;

      CDVDCodecUtils::CopyPicture(m_pTempOverlayPicture, pSource);
      memcpy(pSource->data     , m_pTempOverlayPicture->data     , sizeof(pSource->data));
      memcpy(pSource->iLineSize, m_pTempOverlayPicture->iLineSize, sizeof(pSource->iLineSize));
    }
  }

  if(render == OVERLAY_AUTO)
    render = OVERLAY_GPU;

  VecOverlays overlays;

  {
    CSingleLock lock(*m_pOverlayContainer);

    VecOverlays* pVecOverlays = m_pOverlayContainer->GetOverlays();
    VecOverlaysIter it = pVecOverlays->begin();

    //Check all overlays and render those that should be rendered, based on time and forced
    //Both forced and subs should check timing
    while (it != pVecOverlays->end())
    {
      CDVDOverlay* pOverlay = *it++;
      if(!pOverlay->bForced && !m_bRenderSubs)
        continue;

      double pts2 = pOverlay->bForced ? pts : pts - m_iSubtitleDelay;

      if((pOverlay->iPTSStartTime <= pts2 && (pOverlay->iPTSStopTime > pts2 || pOverlay->iPTSStopTime == 0LL)))
      {
        if(pOverlay->IsOverlayType(DVDOVERLAY_TYPE_GROUP))
          overlays.insert(overlays.end(), static_cast<CDVDOverlayGroup*>(pOverlay)->m_overlays.begin()
                                        , static_cast<CDVDOverlayGroup*>(pOverlay)->m_overlays.end());
        else
          overlays.push_back(pOverlay);

      }
    }

    for(it = overlays.begin(); it != overlays.end(); ++it)
    {
      double pts2 = (*it)->bForced ? pts : pts - m_iSubtitleDelay;

      if (render == OVERLAY_GPU)
        g_renderManager.AddOverlay(*it, pts2);

      if (render == OVERLAY_BUF)
        CDVDOverlayRenderer::Render(pSource, *it, pts2);
    }
  }


}
#endif

static std::string GetRenderFormatName(ERenderFormat format)
{
  switch(format)
  {
    case RENDER_FMT_YUV420P:   return "YV12";
    case RENDER_FMT_YUV420P16: return "YV12P16";
    case RENDER_FMT_YUV420P10: return "YV12P10";
    case RENDER_FMT_NV12:      return "NV12";
    case RENDER_FMT_UYVY422:   return "UYVY";
    case RENDER_FMT_YUYV422:   return "YUY2";
    case RENDER_FMT_VDPAU:     return "VDPAU";
    case RENDER_FMT_VDPAU_420: return "VDPAU_420";
    case RENDER_FMT_DXVA:      return "DXVA";
    case RENDER_FMT_VAAPI:     return "VAAPI";
    case RENDER_FMT_VAAPINV12: return "VAAPI_NV12";
    case RENDER_FMT_OMXEGL:    return "OMXEGL";
    case RENDER_FMT_CVBREF:    return "BGRA";
    case RENDER_FMT_EGLIMG:    return "EGLIMG";
    case RENDER_FMT_BYPASS:    return "BYPASS";
    case RENDER_FMT_MEDIACODEC:return "MEDIACODEC";
    case RENDER_FMT_MEDIACODECSURFACE:return "MEDIACODECSURFACE";
    case RENDER_FMT_IMXMAP:    return "IMXMAP";
    case RENDER_FMT_MMAL:      return "MMAL";
    case RENDER_FMT_NONE:      return "NONE";
  }
  return "UNKNOWN";
}

std::string CDVDPlayerVideo::GetStereoMode()
{
  std::string  stereo_mode;

  switch(CMediaSettings::GetInstance().GetCurrentVideoSettings().m_StereoMode)
  {
    case RENDER_STEREO_MODE_SPLIT_VERTICAL:   stereo_mode = "left_right"; break;
    case RENDER_STEREO_MODE_SPLIT_HORIZONTAL: stereo_mode = "top_bottom"; break;
    default:                                  stereo_mode = m_hints.stereo_mode; break;
  }

  if(CMediaSettings::GetInstance().GetCurrentVideoSettings().m_StereoInvert)
    stereo_mode = GetStereoModeInvert(stereo_mode);
  return stereo_mode;
}

int CDVDPlayerVideo::OutputPicture(const DVDVideoPicture* src, double pts)
{
  m_bAbortOutput = false;

  /* picture buffer is not allowed to be modified in this call */
  DVDVideoPicture picture(*src);
  DVDVideoPicture* pPicture = &picture;

  /* grab stereo mode from image if available */
  if(src->stereo_mode[0])
    m_hints.stereo_mode = src->stereo_mode;

  /* figure out steremode expected based on user settings and hints */
  unsigned int stereo_flags = GetStereoModeFlags(GetStereoMode());

#ifdef HAS_VIDEO_PLAYBACK
  double config_framerate = m_bFpsInvalid ? 0.0 : m_fFrameRate;
  double render_framerate = g_graphicsContext.GetFPS();
  if (CSettings::GetInstance().GetInt(CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE) == ADJUST_REFRESHRATE_OFF)
    render_framerate = config_framerate;
  bool changerefresh = !m_bFpsInvalid &&
                       (m_output.framerate == 0.0 || fmod(m_output.framerate, config_framerate) != 0.0) &&
                       (render_framerate != config_framerate);

  /* check so that our format or aspect has changed. if it has, reconfigure renderer */
  if (!g_renderManager.IsConfigured()
   || ( m_output.width           != pPicture->iWidth )
   || ( m_output.height          != pPicture->iHeight )
   || ( m_output.dwidth          != pPicture->iDisplayWidth )
   || ( m_output.dheight         != pPicture->iDisplayHeight )
   || changerefresh
   || ( m_output.color_format    != (unsigned int)pPicture->format )
   || ( m_output.extended_format != pPicture->extended_format )
   || ( m_output.color_matrix    != pPicture->color_matrix    && pPicture->color_matrix    != 0 ) // don't reconfigure on unspecified
   || ( m_output.chroma_position != pPicture->chroma_position && pPicture->chroma_position != 0 )
   || ( m_output.color_primaries != pPicture->color_primaries && pPicture->color_primaries != 0 )
   || ( m_output.color_transfer  != pPicture->color_transfer  && pPicture->color_transfer  != 0 )
   || ( m_output.color_range     != pPicture->color_range )
   || ( m_output.stereo_flags    != stereo_flags))
  {
    CLog::Log(LOGNOTICE, " fps: %f, pwidth: %i, pheight: %i, dwidth: %i, dheight: %i, colorrange: %s, colorspace: %s, colortransfer: %s"
                       , config_framerate
                       , pPicture->iWidth
                       , pPicture->iHeight
                       , pPicture->iDisplayWidth
                       , pPicture->iDisplayHeight
                       , pPicture->color_range == 1 ? "Full" : "Limited"
                       , av_color_space_name((enum AVColorSpace)pPicture->color_matrix)
                       , av_color_transfer_name((enum AVColorTransferCharacteristic)pPicture->color_transfer));

    unsigned flags = 0;
    if (pPicture->color_range == 1)
      flags |= CONF_FLAGS_YUV_FULLRANGE;
    else if (CSettings::GetInstance().GetBool(CSettings::SETTING_VIDEOPLAYER_DISABLELIMITEDTRESCALING))
      flags |= CONF_FLAGS_YUV_FULLRANGE;

    flags |= GetFlagsChromaPosition(pPicture->chroma_position)
          |  GetFlagsColorMatrix(pPicture->color_matrix, pPicture->iWidth, pPicture->iHeight)
          |  GetFlagsColorPrimaries(pPicture->color_primaries)
          |  GetFlagsColorTransfer(pPicture->color_transfer)
          |  GetFlagsDynamicRange(pPicture->dynamic_range);

    std::string formatstr = GetRenderFormatName(pPicture->format);

    if(m_bAllowFullscreen)
    {
      flags |= CONF_FLAGS_FULLSCREEN;
      m_bAllowFullscreen = false; // only allow on first configure
    }

    flags |= stereo_flags;

    CLog::Log(LOGDEBUG,"%s - change configuration. %dx%d. framerate: %4.2f. format: %s",__FUNCTION__,pPicture->iWidth, pPicture->iHeight, config_framerate, formatstr.c_str());
    if(!g_renderManager.Configure(pPicture->iWidth
                                , pPicture->iHeight
                                , pPicture->iDisplayWidth
                                , pPicture->iDisplayHeight
                                , config_framerate
                                , flags
                                , pPicture->format
                                , pPicture->extended_format
                                , m_hints.orientation
                                , m_pVideoCodec->GetAllowedReferences()))
    {
      CLog::Log(LOGERROR, "%s - failed to configure renderer", __FUNCTION__);
      return EOS_ABORT;
    }

    m_output.width           = pPicture->iWidth;
    m_output.height          = pPicture->iHeight;
    m_output.dwidth          = pPicture->iDisplayWidth;
    m_output.dheight         = pPicture->iDisplayHeight;
    m_output.framerate       = config_framerate;
    m_output.color_format    = pPicture->format;
    m_output.extended_format = pPicture->extended_format;
    m_output.color_matrix    = pPicture->color_matrix;
    m_output.chroma_position = pPicture->chroma_position;
    m_output.color_primaries = pPicture->color_primaries;
    m_output.color_transfer  = pPicture->color_transfer;
    m_output.color_range     = pPicture->color_range;
    m_output.dynamic_range   = pPicture->dynamic_range;
    m_output.stereo_flags    = stereo_flags;
  }

  if (!g_renderManager.IsStarted()) {
    CLog::Log(LOGERROR, "%s - renderer not started", __FUNCTION__);
    return EOS_ABORT;
  }

  int result = 0;

  //correct any pattern in the timestamps
  if (m_output.color_format != RENDER_FMT_BYPASS)
  {
    m_pullupCorrection.Add(pts);
    pts += m_pullupCorrection.GetCorrection();
  }

  //try to calculate the framerate if not stalled
  if (!m_stalled)
    CalcFrameRate();

  // remember original pts, we need it later for overlaying subtitles
  double pts_org = pts;

  // signal to clock what our framerate is, it may want to adjust it's
  // speed to better match with our video renderer's output speed
  double interval;
  int refreshrate = m_pClock->UpdateFramerate(m_fFrameRate, &interval);
  if (refreshrate > 0) //refreshrate of -1 means the videoreferenceclock is not running
  {//when using the videoreferenceclock, a frame is always presented half a vblank interval too late
    pts -= DVD_TIME_BASE * interval;
  }

  if (m_output.color_format != RENDER_FMT_BYPASS)
  {
    // Correct pts by user set delay and rendering delay
    pts += m_iVideoDelay - DVD_SEC_TO_TIME(g_renderManager.GetDisplayLatency());
  }

  // calculate the time we need to delay this picture before displaying
  double iSleepTime, iClockSleep, iFrameSleep, iPlayingClock, iCurrentClock;

  iPlayingClock = m_pClock->GetClock(iCurrentClock, false); // snapshot current clock

  // correct sleep times based on speed
  if(m_speed)
  {
    iClockSleep = (pts - iPlayingClock) * DVD_PLAYSPEED_NORMAL / m_speed;
    iFrameSleep = (pts - m_FlipTimePts) * DVD_PLAYSPEED_NORMAL / m_speed - (iCurrentClock - m_FlipTimeStamp);
  }
  else
  {
    iClockSleep = 0;
    iFrameSleep = 0;
  }

  if (m_syncState != IDVDStreamPlayer::SYNC_INSYNC)
    iSleepTime = 0.0;
  else if (m_stalled)
    iSleepTime = iFrameSleep;
  else
    iSleepTime = iClockSleep;

  // limit sleep time to 2000ms
  if (iSleepTime > DVD_MSEC_TO_TIME(2000))
    iSleepTime = DVD_MSEC_TO_TIME(2000);

  if (m_speed < 0)
  {
    double sleepTime, renderPts;
    int queued, discard;
    double inputPts = m_droppingStats.m_lastPts;
    g_renderManager.GetStats(sleepTime, renderPts, queued, discard);
    if (pts_org > renderPts || queued > 0)
    {
      if (inputPts >= renderPts)
      {
        Sleep(50);
      }
      //CLog::Log(LOGDEBUG, "%s - EOS_DROPPED 1", __FUNCTION__);
      return result | EOS_DROPPED;
    }
    else if (pts_org < iPlayingClock)
    {
      //CLog::Log(LOGDEBUG, "%s - EOS_DROPPED 2", __FUNCTION__);
      return result | EOS_DROPPED;
    }

    if (iSleepTime > DVD_MSEC_TO_TIME(20))
      iSleepTime = DVD_MSEC_TO_TIME(20);
  }
  else if (m_speed > DVD_PLAYSPEED_NORMAL)
  {
    double sleepTime, renderPts;
    int bufferLevel, queued, discard;
    g_renderManager.GetStats(sleepTime, renderPts, queued, discard);
    bufferLevel = queued + discard;

    // estimate the time it will take for the next frame to get rendered
    // drop the frame if it's late in regard to this estimation
    double diff = pts_org - renderPts;
    double mindiff = DVD_SEC_TO_TIME(1/m_fFrameRate) * (bufferLevel + 1);
    if (diff < mindiff)
    {
      m_droppingStats.AddOutputDropGain(pts, 1/m_fFrameRate);
      //CLog::Log(LOGDEBUG, "%s - EOS_DROPPED 3", __FUNCTION__);
      return result | EOS_DROPPED;
    }
  }

  // timestamp when we think next picture should be displayed based on current duration
  m_FlipTimeStamp  = iCurrentClock;
  m_FlipTimeStamp += std::max(0.0, iSleepTime);
  m_FlipTimePts    = pts;

  if ((pPicture->iFlags & DVP_FLAG_DROPPED))
  {
    m_droppingStats.AddOutputDropGain(pts, 1/m_fFrameRate);
    //CLog::Log(LOGDEBUG,"%s - dropped in output", __FUNCTION__);
    //CLog::Log(LOGDEBUG, "%s - EOS_DROPPED 4", __FUNCTION__);
    return result | EOS_DROPPED;
  }

  // set fieldsync if picture is interlaced
  EFIELDSYNC mDisplayField = FS_NONE;
  if( pPicture->iFlags & DVP_FLAG_INTERLACED )
  {
    if( pPicture->iFlags & DVP_FLAG_TOP_FIELD_FIRST )
      mDisplayField = FS_TOP;
    else
      mDisplayField = FS_BOT;
  }

  // make sure waiting time is not negative
  //int maxWaitTime = std::min(std::max(DVD_TIME_TO_MSEC(iSleepTime) + 500, 50), 500);
  int maxWaitTime = std::max(DVD_TIME_TO_MSEC(iSleepTime) + 500, 50);
  // don't wait when going ff
  if (m_speed > DVD_PLAYSPEED_NORMAL)
    maxWaitTime = std::max(DVD_TIME_TO_MSEC(iSleepTime), 0);
  int buffer = g_renderManager.WaitForBuffer(m_bAbortOutput, maxWaitTime);
  if (buffer < 0)
  {
    m_droppingStats.AddOutputDropGain(pts, 1/m_fFrameRate);
    //CLog::Log(LOGDEBUG, "%s - EOS_DROPPED 5", __FUNCTION__);
    return EOS_DROPPED;
  }

  ProcessOverlays(pPicture, pts_org);

  int index = g_renderManager.AddVideoPicture(*pPicture);

  // video device might not be done yet
  while (index < 0 && !m_bAbortOutput &&
         m_pClock->GetAbsoluteClock(false) < iCurrentClock + iSleepTime + DVD_MSEC_TO_TIME(500) )
  {
    Sleep(1);
    index = g_renderManager.AddVideoPicture(*pPicture);
  }

  if (index < 0)
  {
    m_droppingStats.AddOutputDropGain(pts, 1/m_fFrameRate);
    //CLog::Log(LOGDEBUG, "%s - EOS_DROPPED 6", __FUNCTION__);
    return EOS_DROPPED;
  }

  g_renderManager.FlipPage(m_bAbortOutput, (iCurrentClock + iSleepTime) / DVD_TIME_BASE, pts_org, -1, mDisplayField);

  return result;
#else
  // no video renderer, let's mark it as dropped
  return EOS_DROPPED;
#endif
}

std::string CDVDPlayerVideo::GetPlayerInfo()
{
  std::ostringstream s;
  s << "vq:"   << std::setw(2) << std::min(99,GetLevel()) << "%";
  s << ", Mb/s:" << std::fixed << std::setprecision(2) << (double)GetVideoBitrate() / (1024.0*1024.0);
  s << ", fr:"   << std::fixed << std::setprecision(3) << m_fFrameRate;
  s << ", dc:"   << m_codecname;
  s << ", drop:" << m_iDroppedFrames;
  s << ", skip:" << g_renderManager.GetSkippedFrames();

  int pc = m_pullupCorrection.GetPatternLength();
  if (pc > 0)
    s << ", pc:" << pc;
  else
    s << ", pc:none";

  return s.str();
}

int CDVDPlayerVideo::GetVideoBitrate()
{
  return (int)m_videoStats.GetBitrate();
}

void CDVDPlayerVideo::ResetFrameRateCalc()
{
  m_fStableFrameRate = 0.0;
  m_iFrameRateCount  = 0;
  m_iFrameRateLength = 1;
  m_iFrameRateErr    = 0;

  m_bAllowDrop       = (!m_bCalcFrameRate && CMediaSettings::GetInstance().GetCurrentVideoSettings().m_ScalingMethod != VS_SCALINGMETHOD_AUTO) ||
                        g_advancedSettings.m_videoFpsDetect == 0;
}

double CDVDPlayerVideo::GetCurrentPts()
{
  double iSleepTime, iRenderPts;
  int queued, discard;

  // get render stats
  g_renderManager.GetStats(iSleepTime, iRenderPts, queued, discard);

  if (iRenderPts == DVD_NOPTS_VALUE)
    return DVD_NOPTS_VALUE;
  else if (m_stalled)
    return DVD_NOPTS_VALUE;
  else if (m_speed == DVD_PLAYSPEED_NORMAL)
  {
    iRenderPts -= std::max(0.0, iSleepTime);

    if (iRenderPts < 0)
      iRenderPts = 0;
  }
  return iRenderPts;
}

#define MAXFRAMERATEDIFF   0.01
#define MAXFRAMESERR    1000

void CDVDPlayerVideo::CalcFrameRate()
{
  if (m_iFrameRateLength >= 128 || g_advancedSettings.m_videoFpsDetect == 0)
    return; //don't calculate the fps

  //only calculate the framerate if sync playback to display is on, adjust refreshrate is on,
  //or scaling method is set to auto
  if (!m_bCalcFrameRate && CMediaSettings::GetInstance().GetCurrentVideoSettings().m_ScalingMethod != VS_SCALINGMETHOD_AUTO)
  {
    ResetFrameRateCalc();
    return;
  }

  if (!m_pullupCorrection.HasFullBuffer())
    return; //we can only calculate the frameduration if m_pullupCorrection has a full buffer

  //see if m_pullupCorrection was able to detect a pattern in the timestamps
  //and is able to calculate the correct frame duration from it
  double frameduration = m_pullupCorrection.GetFrameDuration();
  if (m_pullupCorrection.VFRDetection())
    frameduration = m_pullupCorrection.GetMinFrameDuration();

  if ((frameduration==DVD_NOPTS_VALUE) ||
      ((g_advancedSettings.m_videoFpsDetect == 1) && ((m_pullupCorrection.GetPatternLength() > 1) && !m_pullupCorrection.VFRDetection())))
  {
    //reset the stored framerates if no good framerate was detected
    m_fStableFrameRate = 0.0;
    m_iFrameRateCount = 0;
    m_iFrameRateErr++;

    if (m_iFrameRateErr == MAXFRAMESERR && m_iFrameRateLength == 1)
    {
      CLog::Log(LOGDEBUG,"%s counted %i frames without being able to calculate the framerate, giving up", __FUNCTION__, m_iFrameRateErr);
      m_bAllowDrop = true;
      m_iFrameRateLength = 128;
    }
    return;
  }

  double framerate = DVD_TIME_BASE / frameduration;

  //store the current calculated framerate if we don't have any yet
  if (m_iFrameRateCount == 0)
  {
    m_fStableFrameRate = framerate;
    m_iFrameRateCount++;
  }
  //check if the current detected framerate matches with the stored ones
  else if (fabs(m_fStableFrameRate / m_iFrameRateCount - framerate) <= MAXFRAMERATEDIFF)
  {
    m_fStableFrameRate += framerate; //store the calculated framerate
    m_iFrameRateCount++;

    //if we've measured m_iFrameRateLength seconds of framerates,
    if (m_iFrameRateCount >= MathUtils::round_int(framerate) * m_iFrameRateLength)
    {
      //store the calculated framerate if it differs too much from m_fFrameRate
      if (fabs(m_fFrameRate - (m_fStableFrameRate / m_iFrameRateCount)) > MAXFRAMERATEDIFF || m_bFpsInvalid)
      {
        CLog::Log(LOGDEBUG,"%s framerate was:%f calculated:%f", __FUNCTION__, m_fFrameRate, m_fStableFrameRate / m_iFrameRateCount);
        m_fFrameRate = m_fStableFrameRate / m_iFrameRateCount;
        m_bFpsInvalid = false;
      }

      //reset the stored framerates
      m_fStableFrameRate = 0.0;
      m_iFrameRateCount = 0;
      m_iFrameRateLength *= 2; //double the length we should measure framerates

      //we're allowed to drop frames because we calculated a good framerate
      m_bAllowDrop = true;
    }
  }
  else //the calculated framerate didn't match, reset the stored ones
  {
    m_fStableFrameRate = 0.0;
    m_iFrameRateCount = 0;
  }
}

int CDVDPlayerVideo::CalcDropRequirement(double pts)
{
  int result = 0;
  double iSleepTime;
  double iDecoderPts, iRenderPts;
  double interval;
  double iGain;
  double iLateness;
  int iSkippedPicture = -1;
  int iDroppedFrames = -1;
  int    iBufferLevel;
  int    queued, discard;

  m_droppingStats.m_lastPts = pts;

  // get decoder stats
  if (!m_pVideoCodec->GetCodecStats(iDecoderPts, iDroppedFrames, iSkippedPicture))
    iDecoderPts = pts;
  if (iDecoderPts == DVD_NOPTS_VALUE)
    iDecoderPts = pts;

  // get render stats
  g_renderManager.GetStats(iSleepTime, iRenderPts, queued, discard);
  iBufferLevel = queued + discard;

  if (iBufferLevel < 0)
    result |= EOS_BUFFER_LEVEL;
  else if (iBufferLevel < 2)
  {
    result |= EOS_BUFFER_LEVEL;
    if (g_advancedSettings.CanLogComponent(LOGVIDEO))
      CLog::Log(LOGDEBUG,"CDVDPlayerVideo::CalcDropRequirement - hurry: %d", iBufferLevel);
  }

  interval = 1/m_fFrameRate*(double)DVD_TIME_BASE;

  if (m_bAllowDrop)
  {
    if (iSkippedPicture > 0)
    {
      iGain = iSkippedPicture*interval;
      CDroppingStats::CGain gain;
      gain.gain = iGain;
      gain.pts = iDecoderPts;
      m_droppingStats.m_gain.push_back(gain);
      m_droppingStats.m_totalGain += gain.gain;
      result |= EOS_DROPPED;
      m_droppingStats.m_dropRequests = 0;
      if (g_advancedSettings.CanLogComponent(LOGVIDEO))
        CLog::Log(LOGDEBUG,"CDVDPlayerVideo::CalcDropRequirement - dropped pictures, Sleeptime: %d, Bufferlevel: %d, Gain: %d", DVD_TIME_TO_MSEC(iSleepTime), iBufferLevel, DVD_TIME_TO_MSEC(iGain));
    }
    if (iDroppedFrames > 0)
    {
      iGain = iDroppedFrames*interval;
      CDroppingStats::CGain gain;
      gain.gain = iGain;
      gain.pts = iDecoderPts;
      m_droppingStats.m_gain.push_back(gain);
      m_droppingStats.m_totalGain += iGain;
      result |= EOS_DROPPED;
      m_droppingStats.m_dropRequests = 0;
      if (g_advancedSettings.CanLogComponent(LOGVIDEO))
        CLog::Log(LOGDEBUG,"CDVDPlayerVideo::CalcDropRequirement - dropped in decoder, Sleeptime: %d, Bufferlevel: %d, Gain: %d", DVD_TIME_TO_MSEC(iSleepTime), iBufferLevel, DVD_TIME_TO_MSEC(iGain));
    }
  }

  // subtract gains
  while (!m_droppingStats.m_gain.empty() &&
         iRenderPts >= m_droppingStats.m_gain.front().pts)
  {
    m_droppingStats.m_totalGain -= m_droppingStats.m_gain.front().gain;
    m_droppingStats.m_gain.pop_front();
  }

  // calculate lateness
  iLateness = iSleepTime + m_droppingStats.m_totalGain;
  if (iLateness < 0 && m_speed)
  {
    m_droppingStats.m_lateFrames++;

    // if lateness is smaller than frametime, we observe this state
    // for 10 cycles
    if (m_droppingStats.m_lateFrames > 10 || iLateness < -2/m_fFrameRate)
    {
      result |= EOS_VERYLATE;
      m_droppingStats.m_dropRequests++;
    }
  }
  else
  {
    m_droppingStats.m_dropRequests = 0;
    m_droppingStats.m_lateFrames = 0;
  }
  return result;
}

void CDroppingStats::Reset()
{
  m_gain.clear();
  m_totalGain = 0;
  m_lateFrames = 0;
  m_dropRequests = 0;
}

void CDroppingStats::AddOutputDropGain(double pts, double frametime)
{
  CDroppingStats::CGain gain;
  gain.gain = frametime;
  gain.pts = pts;
  m_gain.push_back(gain);
  m_totalGain += frametime;
}
