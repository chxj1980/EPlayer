/*
 * ****************************************************************
 * This software is a media player SDK implementation
 * GPL:
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details. You should
 * have received a copy of the GNU Library General Public License
 * along with this library; if not, write to the Free Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Project: EC < Enjoyable Coding >
 *
 * VideoRender.cpp
 * This file for Media Video Render class APIs impelement.
 *
 * Eamil:   epengao@126.com
 * Author:  Gao Peng
 * Version: Intial first version.
 * ****************************************************************
 */

#include "ECLog.h"
#include "ECUtil.h"
#include "ECOSDef.h"
#include "ECMemOP.h"
#include "ECAutoLock.h"
#include "MessageHub.h"
#include "VideoRender.h"
#include "VideoDecPort.h"

#define V_RND_RETRY_WAIT     10
#define V_RND_SEEK_MAX_TRY  512
#define V_RND_FORWARD_TIME  100
#define DROP_FRAM_WAIT_TIME 200

#ifdef EC_OS_Win32
#include "Win_VideoDevice.h"
#elif defined EC_OS_Linux
/* TODO */
#elif defined EC_OS_MacOS
#include "Mac_VideoDevice.h"
#elif defined EC_OS_iOS
#include "iOS_VideoDevice.h"
#elif defined EC_OS_Android
#include "Android_VideoDevice.h"
#endif

VideoRender::VideoRender(MediaClock* pClock)
:m_bEOS(false)
,m_pDecoderPort(NULL)
,m_pVideoDevice(NULL)
,m_pMedaiClock(pClock)
{
#ifdef EC_OS_Win32
    m_pVideoDevice = new Win_VideoDevice();
#elif defined EC_OS_Linux
    /* TODO */
#elif defined EC_OS_MacOS
    m_pVideoDevice = new Mac_VideoDevice();
#elif defined EC_OS_iOS
    m_pVideoDevice = new iOS_VideoDevice();
#elif defined EC_OS_Android
    m_pVideoDevice = new Android_VideoDevice();
#endif
    m_pThreadDriver = new ECThreadDriver(this);
}

VideoRender::~VideoRender()
{
    if (m_pThreadDriver)
    {
        m_pThreadDriver->Exit();
        delete m_pThreadDriver;
    }
    if (m_pVideoDevice)
    {
        delete m_pVideoDevice;
    }
}

void VideoRender::Run()
{
    m_pThreadDriver->Run();
}

void VideoRender::Pause()
{
    m_pThreadDriver->Pause();
}

void VideoRender::Flush()
{
    ECAutoLock Lock(&m_mtxVideoDev);
    m_pVideoDevice->Flush();
    m_VFrame.nTimestamp = 0;
}

void VideoRender::Seek(EC_U32 nPos, bool fastSeek)
{
    Flush();

    EC_U32 nRet = 0;
    bool checkAgain = false;
    EC_U32 nMaxTry = V_RND_SEEK_MAX_TRY;
    do
    {
        nMaxTry--;
        nRet = m_pDecoderPort->GetVideoFrame(&m_VFrame);
        if(fastSeek)
        {
            checkAgain = (nRet != EC_Err_None);
        }
        else
        {
            checkAgain = (m_VFrame.nTimestamp < (nPos - 500));
        }
    } while (nMaxTry > 0 && checkAgain);

    if (nRet == Video_Render_Err_None)
    {
        //m_pThreadDriver->RunSteps(1);
        ECAutoLock Lock(&m_mtxVideoDev);
        m_pVideoDevice->DrawFrame(&m_VFrame);
    }
}

EC_U32 VideoRender::OpenDevice(void* pVideoWindow,
                               int nWindowWidth,
                               int nWindowHeight,
                               MediaContext* pMediaContext,
                               VideoDecPort* pVideoDecPort)
{
    m_bEOS = false;
    m_VFrame.nTimestamp = 0;
    m_VFrame.pAVFrame = NULL;

    m_pDecoderPort = pVideoDecPort;
    ECAutoLock Lock(&m_mtxVideoDev);
    return m_pVideoDevice->Init(pVideoWindow, nWindowWidth,
                                nWindowHeight, pMediaContext);
}

void VideoRender::CloseDevice()
{
    m_pThreadDriver->Pause();
    m_bEOS = false;
    {
        ECAutoLock Lock(&m_mtxVideoDev);
        m_pVideoDevice->Uninit();
    }
    m_VFrame.nTimestamp = 0;
    m_VFrame.pAVFrame = NULL;
}

void VideoRender::UpdateVideoWindow(void* pWindow, int nWidth, int nHeight)
{
    ECAutoLock Lock(&m_mtxVideoDev);
    m_pVideoDevice->UpdateVideoWindow(pWindow, nWidth, nHeight);
}

void DebugPrintf(char* lpszFormat, ...);

void VideoRender::DoRunning()
{
    EC_U32 nRet = m_pDecoderPort->GetVideoFrame(&m_VFrame);
    if(nRet == Video_Render_Err_None)
    {
        if(m_pMedaiClock)
        {
            bool renderFrame = true;
            TimeStamp nClockTime = m_pMedaiClock->GetClockTime();
            int nForwardTime = int(m_VFrame.nTimestamp - nClockTime);
            {
                if(nForwardTime > 0)
                {
                    if (nForwardTime > V_RND_FORWARD_TIME)
                    {
                        EC_U32 nWaitTime = nForwardTime - V_RND_FORWARD_TIME;
                        if (nWaitTime < 1000)
                            ECSleep(nWaitTime);
                        else
							ECSleep(1000);
                    }
                }
                else if(0 - nForwardTime > DROP_FRAM_WAIT_TIME)
                {
                    renderFrame = false;
                }

                if(renderFrame)
                {
                    ECAutoLock Lock(&m_mtxVideoDev);
                    m_pVideoDevice->DrawFrame(&m_VFrame);
                }
                else
                {
                    //ecLogW("Drop Frame,timstamp:%d, currPlayTime:%d, timeAfter:%d\n", (int)m_VFrame.nTimestamp, (int)nClockTime, nForwardTime);
                    return;
                }
            }
        }
    }
    else if(Source_Err_ReadEOS == nRet)
    {
        if(!m_bEOS)
        {
            m_bEOS = true;
            MessageHub* pMsgHub = MessageHub::GetInstance();
            pMsgHub->SendMessage(PlayerMessage_Video_PlayFinished);
        }
        else
        {
            ECSleep(V_RND_RETRY_WAIT);
        }
    }
    else
    {
        ECSleep(V_RND_RETRY_WAIT);
    }
}