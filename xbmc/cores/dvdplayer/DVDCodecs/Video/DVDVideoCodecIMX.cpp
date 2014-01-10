/*
 *      Copyright (C) 2013 Stephan Rafin
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

#include <linux/mxcfb.h>
#include "DVDVideoCodecIMX.h"

#include <linux/mxc_v4l2.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "threads/SingleLock.h"
#include "utils/log.h"
#include "DVDClock.h"
#include "mfw_gst_ts.h"
#include "threads/Atomics.h"

//#define NO_V4L_RENDERING

#ifdef IMX_PROFILE
static unsigned long long render_ts[30];
static unsigned long long get_time()
{
  struct timespec ts;
  unsigned long long now;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  now = (((unsigned long long)ts.tv_sec) * 1000000000ULL) +
           ((unsigned long long)ts.tv_nsec);

  return now;
}
#endif

/* video device on which the video will be rendered (/dev/video17 => /dev/fb1) */
const char *CIMXRenderingFrames::m_v4lDeviceName = "/dev/video17";
static long sg_singleton_lock_variable = 0;
CIMXRenderingFrames* CIMXRenderingFrames::m_instance = 0;

CIMXRenderingFrames&
CIMXRenderingFrames::GetInstance()
{
  CAtomicSpinLock lock(sg_singleton_lock_variable);
  if( ! m_instance )
  {
    m_instance = new CIMXRenderingFrames();
  }
  return *m_instance;
}

CIMXRenderingFrames::CIMXRenderingFrames()
{
  m_ready = false;
  m_v4lfd = -1;
  m_virtAddr = NULL;
  m_v4lBuffers = NULL;
  memset(&m_crop, 0, sizeof(m_crop));
  m_crop.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
}

bool CIMXRenderingFrames::AllocateBuffers(const struct v4l2_format *format, int nbBuffers)
{
  int ret, i;
  struct v4l2_requestbuffers bufReq;
  struct v4l2_format fmt;
  struct v4l2_buffer v4lBuf;

  CSingleLock lock(m_renderingFramesLock);
  if (m_ready)
  {
    CLog::Log(LOGERROR, "%s - Try to re-allocate buffers while previous buffers were not freed.\n", __FUNCTION__);
    return false;
  }

  m_v4lfd = open(m_v4lDeviceName, O_RDWR|O_NONBLOCK, 0);
  if (m_v4lfd < 0)
  {
    CLog::Log(LOGERROR, "%s - Error while trying to open %s.\n", __FUNCTION__, m_v4lDeviceName);
    __ReleaseBuffers();
    return false;
  }

  ret = ioctl(m_v4lfd, VIDIOC_S_FMT, format);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "%s - Error while setting V4L format (ret %d : %s).\n", __FUNCTION__, ret, strerror(errno));
    __ReleaseBuffers();
    return false;
  }

  fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  ret = ioctl(m_v4lfd, VIDIOC_G_FMT, &fmt);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "%s - Error while getting V4L format (ret %d : %s).\n", __FUNCTION__, ret, strerror(errno));
    __ReleaseBuffers();
    return false;
  }

  m_bufferNum = nbBuffers;
  /* Alloc V4L2 buffers */
  memset(&bufReq, 0, sizeof(bufReq));
  bufReq.count =  m_bufferNum;
  bufReq.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  bufReq.memory = V4L2_MEMORY_MMAP;
  ret = ioctl(m_v4lfd, VIDIOC_REQBUFS, &bufReq);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "%s - %d Hw buffer allocation error (%d)\n", __FUNCTION__, bufReq.count, ret);
    __ReleaseBuffers();
    return false;
  }
  CLog::Log(LOGDEBUG, "%s - %d Hw buffer of %d bytes allocated\n", __FUNCTION__, bufReq.count, fmt.fmt.pix.sizeimage);

  m_virtAddr = new void*[m_bufferNum];
  if (m_virtAddr == NULL)
  {
    CLog::Log(LOGERROR, "%s - Allocation failure (m_virtAddr table of %d elements)\n", __FUNCTION__, m_bufferNum);
    __ReleaseBuffers();
    return false;
  }
  m_v4lBuffers = new v4l2_buffer[m_bufferNum];
  if (m_v4lBuffers == NULL)
  {
    CLog::Log(LOGERROR, "%s - Allocation failure (m_v4lBuffers table of %d elements)\n", __FUNCTION__, m_bufferNum);
    __ReleaseBuffers();
    return false;
  }

  for (i = 0 ; i < m_bufferNum; i++)
  {
    memset(&v4lBuf, 0, sizeof(v4lBuf));
    v4lBuf.index = i;
    v4lBuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    v4lBuf.memory = V4L2_MEMORY_MMAP;
    ret = ioctl (m_v4lfd, VIDIOC_QUERYBUF, &v4lBuf);
    if (ret < 0)
    {
      CLog::Log(LOGERROR, "%s - Error during 1st query of V4L buffer (ret %d : %s)\n", __FUNCTION__, ret, strerror(errno));
      return false;
    }
    m_v4lBuffers[i] = v4lBuf;
    m_virtAddr[i] = NULL;
  }
  memset(&m_crop, 0, sizeof(m_crop));
  m_crop.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  m_pushedFrames = 0;
  m_streamOn = false;
  m_currentField = VPU_FIELD_UNKNOWN;
  m_ready = true;
  return true;
}

void *CIMXRenderingFrames::GetVirtAddr(int idx)
{
  struct v4l2_buffer v4lBuf;
  int ret;

  CSingleLock lock(m_renderingFramesLock);
  if (!m_ready)
    return NULL;
  if ((idx < 0) || (idx >= m_bufferNum))
    return NULL;

  if (m_virtAddr[idx] == NULL)
  {
    v4lBuf = m_v4lBuffers[idx];
    m_virtAddr[idx] = mmap(NULL, v4lBuf.length, PROT_READ | PROT_WRITE, MAP_SHARED, m_v4lfd, v4lBuf.m.offset);

    /* 2nd query to retrieve real Physical address after mmap (iMX6 bug) */
    ret = ioctl (m_v4lfd, VIDIOC_QUERYBUF, &v4lBuf);
    if (ret < 0)
    {
      CLog::Log(LOGERROR, "%s - Error during 2nd query of V4L buffer (ret %d : %s)\n", __FUNCTION__, ret, strerror(errno));
    }
    m_v4lBuffers[idx] = v4lBuf;
  }
  return m_virtAddr[idx];
}

void *CIMXRenderingFrames::GetPhyAddr(int idx)
{

  CSingleLock lock(m_renderingFramesLock);
  if (!m_ready)
    return NULL;
  if ((idx < 0) || (idx >= m_bufferNum))
    return NULL;

  return (void *)m_v4lBuffers[idx].m.offset;
}

int CIMXRenderingFrames::FindBuffer(void *phyAddr)
{
  int i;

  CSingleLock lock(m_renderingFramesLock);
  if (!m_ready)
    return -1;

  for (i = 0; i < m_bufferNum; i++)
  {
    if (m_v4lBuffers[i].m.offset == (unsigned int)phyAddr)
    {
      // CLog::Log(LOGNOTICE, "%s - found buffer OK %d!\n", __FUNCTION__, i);
      return i;
    }
  }
  return -1;
}

int CIMXRenderingFrames::DeQueue(bool wait)
{
  int ret;
  int status;
  struct v4l2_buffer buf;

  CSingleLock lock(m_renderingFramesLock);
  if (!m_ready)
  {
      CLog::Log(LOGNOTICE, "%s - Cannot dequeue frame as buffers were released !\n",
              __FUNCTION__);
    return -1;
  }
  if (!m_streamOn)
  {
    return -1;
  }

  if (wait) {
    status = fcntl(m_v4lfd, F_GETFL);
    fcntl(m_v4lfd, F_SETFL, status & (~O_NONBLOCK));
  }
  buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  buf.memory = V4L2_MEMORY_MMAP;
  ret = ioctl(m_v4lfd, VIDIOC_DQBUF, &buf);
  if (wait)
    fcntl(m_v4lfd, F_SETFL, status | O_NONBLOCK);
  if (ret != 0)
  {
    if (errno != EAGAIN)
      CLog::Log(LOGERROR, "%s - Dequeue buffer error (ret %d : %s)\n",
              __FUNCTION__, ret, strerror(errno));
    return -1;
  }

#ifdef IMX_PROFILE
          CLog::Log(LOGDEBUG, "%s - Time render to dequeue (%d) %llu\n",
              __FUNCTION__, buf.index, get_time() - render_ts[buf.index]);
#endif
  return buf.index;
}

void CIMXRenderingFrames::Queue(CIMXOutputFrame *picture, struct v4l2_crop &destRect)
{
  /* Warning : called from renderer thread
   * Especially do not call any VPU functions as they are not thread safe
   */

  int ret, type;
  struct timeval queue_ts;
  struct v4l2_format fmt;
  int stream_trigger;
  struct v4l2_control ctrl;
  struct v4l2_rect rect;
  bool crop_update = false;

  CSingleLock lock(m_renderingFramesLock);
  if (!m_ready)
  {
    CLog::Log(LOGNOTICE, "%s - Cannot queue frame as buffers were released !\n",
              __FUNCTION__);
    return;
  }

  /*CLog::Log(LOGDEBUG, "%s - queuing frame %d - picture adress : %p\n",
              __FUNCTION__, picture->v4l2BufferIdx, picture);*/

  if ((picture->v4l2BufferIdx < 0) || (picture->v4l2BufferIdx >= m_bufferNum))
  {
    CLog::Log(LOGERROR, "%s - Invalid buffer index : %d - picture adress : %p\n",
              __FUNCTION__, picture->v4l2BufferIdx, picture);
  }
  else
  {
    /* Set field type for each buffer otherwise the mxc_vout driver reverts to progressive */
    switch (picture->field)
    {
    case VPU_FIELD_TB:
      m_v4lBuffers[picture->v4l2BufferIdx].field = V4L2_FIELD_INTERLACED_TB;
      break;
    case VPU_FIELD_BT:
      m_v4lBuffers[picture->v4l2BufferIdx].field= V4L2_FIELD_INTERLACED_BT;
      break;
    case VPU_FIELD_NONE:
    default:
      m_v4lBuffers[picture->v4l2BufferIdx].field = V4L2_FIELD_NONE;
      break;
    }

  /* mxc_vout driver does not display immediatly
   * if timestamp is set to 0
   * (instead this driver expects a 30fps rate)
   * So we explicitly set current time for immediate display
   */
  gettimeofday (&queue_ts, NULL);
  m_v4lBuffers[picture->v4l2BufferIdx].timestamp = queue_ts;
#ifndef NO_V4L_RENDERING
  ret = ioctl(m_v4lfd, VIDIOC_QBUF, &m_v4lBuffers[picture->v4l2BufferIdx]);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "%s - V4L Queue buffer failed (ret %d : %s)\n",
              __FUNCTION__, ret, strerror(errno));
    /* If it fails odds are very high picture is invalid so just exit now */
    return;
  } else
    m_pushedFrames++;

  /* Force cropping dimensions to be aligned */
  destRect.c.top    &= 0xFFFFFFF8;
  destRect.c.left   &= 0xFFFFFFF8;
  destRect.c.width  &= 0xFFFFFFF8;
  destRect.c.height &= 0xFFFFFFF8;
  if ((m_crop.c.top != destRect.c.top) ||
      (m_crop.c.left != destRect.c.left) ||
      (m_crop.c.width != destRect.c.width) ||
      (m_crop.c.height !=  destRect.c.height))
  {
     CLog::Log(LOGNOTICE, "%s - Newcrop : %d % d %d %d\n",
              __FUNCTION__, destRect.c.top, destRect.c.left, destRect.c.width, destRect.c.height);
    m_crop.c = destRect.c;
    crop_update = true;
  }

  if (!m_streamOn)
  {
    if (picture->field != m_currentField)
    {
      CLog::Log(LOGNOTICE, "%s - New field is %d\n",
                __FUNCTION__, picture->field);

      /* Handle interlace field */
      fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
      ret = ioctl (m_v4lfd, VIDIOC_G_FMT, &fmt);
      if (ret < 0) {
        CLog::Log(LOGERROR, "%s - V4L VIDIOC_G_FMT failed (ret %d : %s)\n",
                __FUNCTION__, ret, strerror(errno));
      } else {
        switch (picture->field)
        {
        case VPU_FIELD_TOP:
        case VPU_FIELD_BOTTOM:
          CLog::Log(LOGERROR, "%s - mxc_out driver does not handle this field type :%d\n",
                  __FUNCTION__, picture->field);
          fmt.fmt.pix.field = V4L2_FIELD_NONE;
          break;
        case VPU_FIELD_TB:
          fmt.fmt.pix.field = V4L2_FIELD_INTERLACED_TB;
          break;
        case VPU_FIELD_BT:
          fmt.fmt.pix.field = V4L2_FIELD_INTERLACED_BT;
          break;
        case VPU_FIELD_NONE:
        default:
          fmt.fmt.pix.field = V4L2_FIELD_NONE;
          break;
        }

        /* Take into account cropping from decoded video (for input picture) */
        rect.left = picture->picCrop.nLeft;
        rect.top =  picture->picCrop.nTop;
        rect.width = picture->picCrop.nRight - picture->picCrop.nLeft;
        rect.height = picture->picCrop.nBottom - picture->picCrop.nTop;
        fmt.fmt.pix.priv = (unsigned int)&rect;

        fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        ret = ioctl (m_v4lfd, VIDIOC_S_FMT, &fmt);
        if (ret < 0)
        {
          CLog::Log(LOGERROR, "%s - V4L VIDIOC_S_FMT failed (ret %d : %s)\n",
                  __FUNCTION__, ret, strerror(errno));
        }
        else
          m_currentField = picture->field;
      }
    }
    if (m_currentField  == VPU_FIELD_NONE)
      stream_trigger = 1;
    else {
      stream_trigger = 2; /* should be 3 if V4L2_CID_MXC_MOTION < 2 */

      /* FIXME : How to select the most appropriate motion type ? */
      ctrl.id = V4L2_CID_MXC_MOTION;
      ctrl.value = 2; /* 2 stands for high motion */
      ret = ioctl (m_v4lfd, VIDIOC_S_CTRL, &ctrl);
      if (ret < 0)
      {
        CLog::Log(LOGERROR, "%s - Error while setting V4L motion (ret %d : %s).\n", __FUNCTION__, ret, strerror(errno));
      }
    }
    CLog::Log(LOGDEBUG, "%s - Number of required frames before streaming : %d\n",
              __FUNCTION__, stream_trigger);

    if (m_pushedFrames >= stream_trigger) {
      type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
      ret = ioctl(m_v4lfd, VIDIOC_STREAMON, &type);
      if (ret < 0)
      {
        CLog::Log(LOGERROR, "%s - V4L Stream ON failed (ret %d : %s)\n",
                __FUNCTION__, ret, strerror(errno));
      }
      else
      {
        CLog::Log(LOGDEBUG, "%s - V4L Stream ON OK\n",
                __FUNCTION__);
        m_streamOn = true;
      }
      /* We have to repeat crop command after streamon for some vids
      * FIXME check why in drivers...
      */
      ret = ioctl(m_v4lfd, VIDIOC_S_CROP, &m_crop);
      if (ret < 0)
      {
        CLog::Log(LOGERROR, "%s - S_CROP failed (ret %d : %s)\n",
                __FUNCTION__, ret, strerror(errno));
      }
    }
  }
  else
  {
    if (crop_update)
    {
      ret = ioctl(m_v4lfd, VIDIOC_S_CROP, &m_crop);
      if (ret < 0)
      {
        CLog::Log(LOGERROR, "%s - S_CROP failed (ret %d : %s)\n",
                __FUNCTION__, ret, strerror(errno));
      }
    }
  }
#endif

#ifdef IMX_PROFILE
  render_ts[picture->v4l2BufferIdx] = get_time();
  CLog::Log(LOGDEBUG, "%s - Time push to render (%d) %llu\n",
              __FUNCTION__, picture->v4l2BufferIdx, render_ts[picture->v4l2BufferIdx] - picture->pushTS);
#endif

  delete picture;
  }
}

void CIMXRenderingFrames::ReleaseBuffers()
{
  CSingleLock lock(m_renderingFramesLock);
  if (!m_ready)
  {
      CLog::Log(LOGERROR, "%s - AllocateBuffers was not previously called\n", __FUNCTION__);
      return;
  }
  __ReleaseBuffers();
}

/* Note : Has to be called with m_renderingFramesLock held */
void CIMXRenderingFrames::__ReleaseBuffers()
{
  int type, i;

  if (m_v4lfd >= 0)
  {
    /* stream off */
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ioctl (m_v4lfd, VIDIOC_STREAMOFF, &type);
    m_streamOn = false;
  }

  if (m_virtAddr != NULL)
  {
    for (i = 0; i < m_bufferNum; i++)
    {
      if (m_virtAddr[i] != NULL)
        munmap (m_virtAddr[i], m_v4lBuffers[i].length);
    }
    delete m_virtAddr;
    m_virtAddr = NULL;
  }

  if (m_v4lfd >= 0)
  {
    /* Close V4L2 device */
    close(m_v4lfd);
    m_v4lfd = -1;
  }

  if (m_v4lBuffers != NULL)
  {
    delete m_v4lBuffers;
    m_v4lBuffers = NULL;
  }
  m_bufferNum = 0;
  m_ready = false;
}

/* FIXME get rid of these defines properly */
#define FRAME_ALIGN 16
#define MEDIAINFO 1
#define _4CC(c1,c2,c3,c4) (((uint32_t)(c4)<<24)|((uint32_t)(c3)<<16)|((uint32_t)(c2)<<8)|(uint32_t)(c1))
#define Align(ptr,align)  (((unsigned int)ptr + (align) - 1)/(align)*(align))
#define min(a, b) (a<b)?a:b


/* Experiments show that we need at least one more (+1) V4L buffer than the min value returned by the VPU */
const int CDVDVideoCodecIMX::m_extraVpuBuffers = 8;

void CDVDVideoCodecIMX::FlushOutputFrame(void)
{
  if (m_outputFrameReady)
  {
    VPU_DecOutFrameDisplayed(m_vpuHandle, m_outputBuffers[m_outputFrame.imxOutputFrame->v4l2BufferIdx]);
    m_outputBuffers[m_outputFrame.imxOutputFrame->v4l2BufferIdx] = NULL;
    delete m_outputFrame.imxOutputFrame;
    m_outputFrameReady = false;
  }
}

bool CDVDVideoCodecIMX::VpuAllocBuffers(VpuMemInfo *pMemBlock)
{
  int i, size;
  unsigned char * ptr;
  VpuMemDesc vpuMem;
  VpuDecRetCode ret;
      
  for(i=0; i<pMemBlock->nSubBlockNum; i++)
  {     
    size = pMemBlock->MemSubBlock[i].nAlignment + pMemBlock->MemSubBlock[i].nSize;
    if (pMemBlock->MemSubBlock[i].MemType == VPU_MEM_VIRT)
    { // Allocate standard virtual memory
      ptr = (unsigned char *)malloc(size);
      if(ptr == NULL)
      {
        CLog::Log(LOGERROR, "%s - Unable to malloc %d bytes.\n", __FUNCTION__, size);
        goto AllocFailure;
      }               
      pMemBlock->MemSubBlock[i].pVirtAddr = (unsigned char*)Align(ptr, pMemBlock->MemSubBlock[i].nAlignment);

      m_decMemInfo.virtMem[m_decMemInfo.nVirtNum] = (unsigned int)ptr;
      m_decMemInfo.nVirtNum++;
    }
    else 
    { // Allocate contigous mem for DMA
      vpuMem.nSize = size;
      ret = VPU_DecGetMem(&vpuMem);
      if(ret != VPU_DEC_RET_SUCCESS)
      {
        CLog::Log(LOGERROR, "%s - Unable alloc %d bytes of physical memory (%d).\n", __FUNCTION__, size, ret);
        goto AllocFailure;
      }               
      pMemBlock->MemSubBlock[i].pVirtAddr = (unsigned char*)Align(vpuMem.nVirtAddr, pMemBlock->MemSubBlock[i].nAlignment);
      pMemBlock->MemSubBlock[i].pPhyAddr = (unsigned char*)Align(vpuMem.nPhyAddr, pMemBlock->MemSubBlock[i].nAlignment);

      m_decMemInfo.phyMem_phyAddr[m_decMemInfo.nPhyNum] = (unsigned int)vpuMem.nPhyAddr;
      m_decMemInfo.phyMem_virtAddr[m_decMemInfo.nPhyNum] = (unsigned int)vpuMem.nVirtAddr;
      m_decMemInfo.phyMem_cpuAddr[m_decMemInfo.nPhyNum] = (unsigned int)vpuMem.nCpuAddr;
      m_decMemInfo.phyMem_size[m_decMemInfo.nPhyNum] = size;
      m_decMemInfo.nPhyNum++;                     
    }
  }

  return true;
  
AllocFailure:
        VpuFreeBuffers();
        return false;        
}

bool CDVDVideoCodecIMX::VpuFreeBuffers(void)
{
  int i;
  VpuMemDesc vpuMem;
  VpuDecRetCode vpuRet;
  bool ret = true;

  //free virtual mem
  for(i=0; i<m_decMemInfo.nVirtNum; i++)
  {
    if (m_decMemInfo.virtMem[i]) 
      free((void*)m_decMemInfo.virtMem[i]);
  }
  m_decMemInfo.nVirtNum = 0;

  //free physical mem
  for(i=0; i<m_decMemInfo.nPhyNum; i++)
  {
    vpuMem.nPhyAddr = m_decMemInfo.phyMem_phyAddr[i];
    vpuMem.nVirtAddr = m_decMemInfo.phyMem_virtAddr[i];
    vpuMem.nCpuAddr = m_decMemInfo.phyMem_cpuAddr[i];
    vpuMem.nSize = m_decMemInfo.phyMem_size[i];
    vpuRet = VPU_DecFreeMem(&vpuMem);
    if(vpuRet != VPU_DEC_RET_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s - Errror while trying to free physical memory (%d).\n", __FUNCTION__, ret);
      ret = false;
    }
  }
  m_decMemInfo.nPhyNum = 0;

  return ret;
}  

  
bool CDVDVideoCodecIMX::VpuOpen(void)
{
  VpuDecRetCode  ret;
  VpuVersionInfo vpuVersion;
  VpuMemInfo     memInfo;
  VpuDecConfig config;            
  int param;
  
  memset(&memInfo, 0, sizeof(VpuMemInfo));
  ret = VPU_DecLoad();
  if (ret != VPU_DEC_RET_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s - VPU load failed with error code %d.\n", __FUNCTION__, ret);
    goto VpuOpenError;
  }
  
  ret = VPU_DecGetVersionInfo(&vpuVersion);
  if (ret != VPU_DEC_RET_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s - VPU version cannot be read (%d).\n", __FUNCTION__, ret);
    goto VpuOpenError;
  }
  else
  {
    CLog::Log(LOGDEBUG, "VPU Lib version : major.minor.rel=%d.%d.%d.\n", vpuVersion.nLibMajor, vpuVersion.nLibMinor, vpuVersion.nLibRelease);
  }
  
  ret = VPU_DecQueryMem(&memInfo);
  if (ret != VPU_DEC_RET_SUCCESS)
  {
          CLog::Log(LOGERROR, "%s - iMX VPU query mem error (%d).\n", __FUNCTION__, ret);
          goto VpuOpenError;
  }
  VpuAllocBuffers(&memInfo);

  m_decOpenParam.nReorderEnable = 1;
  m_decOpenParam.nChromaInterleave = 0;
  m_decOpenParam.nMapType = 0;
  m_decOpenParam.nTiled2LinearEnable = 0; 
  m_decOpenParam.nEnableFileMode = 0;

  ret = VPU_DecOpen(&m_vpuHandle, &m_decOpenParam, &memInfo);
  if (ret != VPU_DEC_RET_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s - iMX VPU open failed (%d).\n", __FUNCTION__, ret);
    goto VpuOpenError;
  }

  config = VPU_DEC_CONF_SKIPMODE;
  param = VPU_DEC_SKIPNONE;
  ret = VPU_DecConfig(m_vpuHandle, config, &param);
  if (ret != VPU_DEC_RET_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s - iMX VPU set skip mode failed  (%d).\n", __FUNCTION__, ret);
    goto VpuOpenError;
  }
  /* FIXME TBD
  config = VPU_DEC_CONF_BUFDELAY;
  param = 128 * 1024;
  ret = VPU_DecConfig(m_vpuHandle, config, &param);
  if (ret != VPU_DEC_RET_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s - iMX VPU set buffer delay failed  (%d).\n", __FUNCTION__, ret);
    goto VpuOpenError;

  }
  */
  /* FIXME TODO Is there a need to explicitly configure :
   * - VPU_DEC_CONF_INPUTTYPE
   */

  return true;

VpuOpenError:
  Dispose();
  return false;
}

void CDVDVideoCodecIMX::InitFB(void)
{
  struct mxcfb_gbl_alpha alpha;
  struct mxcfb_color_key colorKey;
  int fd;
  
  fd = open("/dev/fb0",O_RDWR);
  /* set FG/BG semi opaque */
  alpha.alpha = 235;
  alpha.enable = 1;
  ioctl(fd, MXCFB_SET_GBL_ALPHA, &alpha);
  /* Enable color keying */
  colorKey.enable = 1;
  colorKey.color_key = (16 << 16) | (8 << 8) | 16;
  if (ioctl(fd, MXCFB_SET_CLR_KEY, &colorKey) < 0)
    CLog::Log(LOGERROR, "%s - Error while trying to enable color keying %s.\n", __FUNCTION__, strerror(errno));
  close(fd);
}

void CDVDVideoCodecIMX::RestoreFB(void)
{
  struct mxcfb_gbl_alpha alpha;
  struct mxcfb_color_key colorKey;
  int fd;

  fd = open("/dev/fb0",O_RDWR);
  /* set FG as opaque */
  alpha.alpha = 255;
  alpha.enable = 1;
  ioctl(fd, MXCFB_SET_GBL_ALPHA, &alpha);
  /* Disable color keying */
  colorKey.enable = 0;
  colorKey.color_key = 0;
  if (ioctl(fd, MXCFB_SET_CLR_KEY, &colorKey) < 0)
    CLog::Log(LOGERROR, "%s - Error while trying to disable color keying %s.\n", __FUNCTION__, strerror(errno));
  close(fd);
}

bool CDVDVideoCodecIMX::VpuAllocFrameBuffers(void)
{
  /* Alloc frame buffers from V4L2 for efficient rendering through V4L streaming */ 
  struct v4l2_format fmt;
  struct v4l2_rect rect;
  int i, j;
  /*int ret, width, height;
  int video_width, video_height;*/
  int ySize, cSize;
  VpuDecRetCode vpuRet;
  CIMXRenderingFrames &renderingFrames = CIMXRenderingFrames::GetInstance();


  InitFB();

  /* Set V4L2 Format */
  memset(&fmt, 0, sizeof(fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  fmt.fmt.pix.width = Align( m_initInfo.nPicWidth, FRAME_ALIGN);
  fmt.fmt.pix.height = Align(m_initInfo.nPicHeight, FRAME_ALIGN);
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
  /*fmt.fmt.pix.bytesperline = fmt.fmt.pix.width;
  fmt.fmt.pix.sizeimage = fmt.fmt.pix.width * fmt.fmt.pix.height  * 3 / 2;*/
  /* Take into account cropping from decoded video (for input picture) */
  rect.left =  m_initInfo.PicCropRect.nLeft;
  rect.top =  m_initInfo.PicCropRect.nTop;
  rect.width = m_initInfo.PicCropRect.nRight - m_initInfo.PicCropRect.nLeft;
  rect.height = m_initInfo.PicCropRect.nBottom - m_initInfo.PicCropRect.nTop;
  fmt.fmt.pix.priv = (unsigned int)&rect;

  m_vpuFrameBufferNum =  m_initInfo.nMinFrameBufferCount + m_extraVpuBuffers;
  if (!renderingFrames.AllocateBuffers(&fmt, m_vpuFrameBufferNum))
  {
    return false;
  }
  
  m_outputBuffers = new VpuFrameBuffer*[m_vpuFrameBufferNum];
  m_vpuFrameBuffers = new VpuFrameBuffer[m_vpuFrameBufferNum];  
  m_extraMem = new VpuMemDesc[m_vpuFrameBufferNum];
  ySize = fmt.fmt.pix.width * fmt.fmt.pix.height;
  cSize = ySize / 4;
  for (i = 0 ; i < m_vpuFrameBufferNum; i++)
  {
    m_vpuFrameBuffers[i].pbufVirtY = (unsigned char *)renderingFrames.GetVirtAddr(i);
    m_vpuFrameBuffers[i].nStrideY = fmt.fmt.pix.width;
    m_vpuFrameBuffers[i].nStrideC = m_vpuFrameBuffers[i].nStrideY / 2;
    m_vpuFrameBuffers[i].pbufY = (unsigned char *)renderingFrames.GetPhyAddr(i);
    m_vpuFrameBuffers[i].pbufCb = m_vpuFrameBuffers[i].pbufY + ySize;
    m_vpuFrameBuffers[i].pbufCr = m_vpuFrameBuffers[i].pbufCb + cSize;
    m_vpuFrameBuffers[i].pbufVirtCb = m_vpuFrameBuffers[i].pbufVirtY + ySize;
    m_vpuFrameBuffers[i].pbufVirtCr = m_vpuFrameBuffers[i].pbufVirtCb + cSize;    
    /* Dont care about tile */
    m_vpuFrameBuffers[i].pbufY_tilebot = 0;
    m_vpuFrameBuffers[i].pbufCb_tilebot = 0;
    m_vpuFrameBuffers[i].pbufVirtY_tilebot = 0;
    m_vpuFrameBuffers[i].pbufVirtCb_tilebot = 0;
    m_outputBuffers[i] = NULL;
  }
  
  /* Allocate physical extra memory */
  for (i = 0 ; i < m_vpuFrameBufferNum; i++)
  {
    m_extraMem[i].nSize = cSize;
    vpuRet = VPU_DecGetMem(&m_extraMem[i]);
    if (vpuRet != VPU_DEC_RET_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s - Extra memory (%d bytes) allocation failure (%d).\n",
               __FUNCTION__, m_extraMem[i].nSize , vpuRet);
      for (j=i ; j<m_vpuFrameBufferNum; j++)
        m_extraMem[j].nSize = 0;
      return false;
    }
    m_vpuFrameBuffers[i].pbufMvCol = (unsigned char *)m_extraMem[i].nPhyAddr;
    m_vpuFrameBuffers[i].pbufVirtMvCol = (unsigned char *)m_extraMem[i].nVirtAddr;
  }

  return true;
}

bool CDVDVideoCodecIMX::VpuPushFrame(VpuDecOutFrameInfo *frameInfo)
{
  #ifdef IMX_PROFILE
  static unsigned long long previous_ts;
  #endif
  VpuFrameBuffer *frameBuffer = frameInfo->pDisplayFrameBuf;
  CIMXOutputFrame *outputFrame = new CIMXOutputFrame();
  CIMXRenderingFrames &renderingFrames = CIMXRenderingFrames::GetInstance();
  int i;
  //outputFrameType outputFrame;
  double pts;

  pts = (double)TSManagerSend2(m_tsm, frameBuffer) / (double)1000.0;
  if (m_dropState)
  {
    /* If we are dropping frame then release current frame immediatly */
    VPU_DecOutFrameDisplayed(m_vpuHandle, frameBuffer);
    return false;
  }

  /* Find Frame given physical address */
  i = renderingFrames.FindBuffer(frameBuffer->pbufY);
  if (i == -1)
  {
    CLog::Log(LOGERROR, "%s - V4L buffer not found\n", __FUNCTION__);
    return false;
  }
  if (m_outputBuffers[i] != NULL)
  {
    CLog::Log(LOGERROR, "%s - Try to reuse buffer which was not dequeued !\n", __FUNCTION__);
    return false;
  }
  if (m_outputFrameReady)
  {
      CLog::Log(LOGERROR, "%s - Called while GetPicture has not consumed previous frame\n", __FUNCTION__);
  }

  /* Store the pointer to be able to invoke VPU_DecOutFrameDisplayed when the buffer will be dequeued */
  m_outputBuffers[i] = frameBuffer;
  //CLog::Log(LOGDEBUG, "%s - set ouputBuffer idx %d : %x\n", __FUNCTION__, i, frameBuffer);

  outputFrame = new CIMXOutputFrame();
  outputFrame->v4l2BufferIdx = i;
  outputFrame->field = frameInfo->eFieldType;
  outputFrame->picCrop = frameInfo->pExtInfo->FrmCropRect;
  outputFrame->nQ16ShiftWidthDivHeightRatio = frameInfo->pExtInfo->nQ16ShiftWidthDivHeightRatio;
  m_outputFrame.imxOutputFrame = outputFrame;

  m_outputFrame.pts = pts;
  m_outputFrame.dts = DVD_NOPTS_VALUE;
  /*
  m_outputFrame.iWidth = frameInfo->pExtInfo->nFrmWidth;
  m_outputFrame.iHeight  = frameInfo->pExtInfo->nFrmHeight;
  */
  m_outputFrame.iWidth  = frameInfo->pExtInfo->FrmCropRect.nRight - frameInfo->pExtInfo->FrmCropRect.nLeft;
  m_outputFrame.iHeight = frameInfo->pExtInfo->FrmCropRect.nBottom - frameInfo->pExtInfo->FrmCropRect.nTop;
  /* FIXME plug aspect ratio correction here frameInfo->nQ16ShiftWidthDivHeightRatio */
  m_outputFrame.format = RENDER_FMT_IMX;
  m_outputFrameReady = true;

#ifdef IMX_PROFILE
  m_outputFrame.imxOutputFrame->pushTS = get_time();
  CLog::Log(LOGDEBUG, "%s - Time between push %llu\n",
              __FUNCTION__,  m_outputFrame.imxOutputFrame->pushTS - previous_ts);
  previous_ts =m_outputFrame.imxOutputFrame->pushTS;
#endif

  return true;

}

int CDVDVideoCodecIMX::GetAvailableBufferNb(void)
{
  int i, nb;

  nb = 0;
  for (i = 0; i < m_vpuFrameBufferNum; i++)
  {
    if (m_outputBuffers[i] == NULL)
      nb++;
  }
  return nb;
}

bool CDVDVideoCodecIMX::VpuDeQueueFrame(bool wait)
{
  int idx;
  CIMXRenderingFrames &renderingFrames = CIMXRenderingFrames::GetInstance();

  idx = renderingFrames.DeQueue(wait);
  if (idx != -1)
  {
    VPU_DecOutFrameDisplayed(m_vpuHandle, m_outputBuffers[idx]);
    m_outputBuffers[idx] = NULL;
    return true;
  }
  else
  {
#ifdef NO_V4L_RENDERING
      int i;
      for (i = 0; i < m_vpuFrameBufferNum; i++)
      {
        if (m_outputBuffers[i] != NULL)
        {
          VPU_DecOutFrameDisplayed(m_vpuHandle, m_outputBuffers[i]);
          m_outputBuffers[i] = NULL;
        }
      }
#endif
    return false;
  }
}

CDVDVideoCodecIMX::CDVDVideoCodecIMX()
{
  m_pFormatName = "iMX-xxx";
  memset(&m_decMemInfo, 0, sizeof(DecMemInfo));
  m_vpuHandle = 0;
  m_vpuFrameBuffers = NULL;
  m_outputBuffers = NULL;
  m_extraMem = NULL;
  m_vpuFrameBufferNum = 0;
  m_tsSyncRequired = true;
  m_dropState = false;
  m_tsm = NULL;
  m_convert_bitstream = false;
}

CDVDVideoCodecIMX::~CDVDVideoCodecIMX()
{
  Dispose();
}

bool CDVDVideoCodecIMX::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  if (hints.software)
  {
    CLog::Log(LOGNOTICE, "iMX VPU : software decoding requested.\n");
    return false;
  }

  m_outputFrameReady = false;
  m_hints = hints;
  CLog::Log(LOGDEBUG, "Let's decode with iMX VPU\n");    
  
#ifdef MEDIAINFO
  CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: fpsrate %d / fpsscale %d\n", m_hints.fpsrate, m_hints.fpsscale);
  CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: CodecID %d \n", m_hints.codec);
  CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: StreamType %d \n", m_hints.type);
  CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: Level %d \n", m_hints.level);
  CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: Profile %d \n", m_hints.profile);
  CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: PTS_invalid %d \n", m_hints.ptsinvalid);
  CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: Tag %d \n", m_hints.codec_tag);
  CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: %dx%d \n", m_hints.width,  m_hints.height);
  { uint8_t *pb = (uint8_t*)&m_hints.codec_tag;
    if (isalnum(pb[0]) && isalnum(pb[1]) && isalnum(pb[2]) && isalnum(pb[3]))
      CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: Tag fourcc %c%c%c%c\n", pb[0], pb[1], pb[2], pb[3]);
  }
  if (m_hints.extrasize)
  {
    unsigned int  i;
    char buf[4096];

    for (i = 0; i < m_hints.extrasize; i++)
      sprintf(buf+i*2, "%02x", ((uint8_t*)m_hints.extradata)[i]);
    CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: extradata %d %s\n", m_hints.extrasize, buf);
  }
  CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: %d / %d \n", m_hints.width,  m_hints.height);
  CLog::Log(LOGDEBUG, "Decode: aspect %f - forced aspect %d\n", m_hints.aspect, m_hints.forced_aspect);
#endif

  m_convert_bitstream = false;
  switch(m_hints.codec)
  {
  case CODEC_ID_MPEG2VIDEO:
  case CODEC_ID_MPEG2VIDEO_XVMC:
    m_decOpenParam.CodecFormat = VPU_V_MPEG2;
    m_pFormatName = "iMX-mpeg2";
    break;
  case CODEC_ID_H263:
    m_decOpenParam.CodecFormat = VPU_V_H263;   
    m_pFormatName = "iMX-h263";
    break;
  case CODEC_ID_H264:
    m_decOpenParam.CodecFormat = VPU_V_AVC;
    m_pFormatName = "iMX-h264";
    if (hints.extrasize >= 7)
    {
    if ( *(char*)hints.extradata == 1 )
      m_convert_bitstream = bitstream_convert_init(hints.extradata,hints.extrasize);
    }
    break;
  case CODEC_ID_VC1:
    m_decOpenParam.CodecFormat = VPU_V_VC1_AP;
    m_pFormatName = "iMX-vc1";
    break;
/* FIXME TODO
 * => for this type we have to set height, width, nChromaInterleave and nMapType 
  case CODEC_ID_MJPEG:
    m_decOpenParam.CodecFormat = VPU_V_MJPG;
    m_pFormatName = "iMX-mjpg";
    break;*/
  case CODEC_ID_CAVS:
  case CODEC_ID_AVS:
    m_decOpenParam.CodecFormat = VPU_V_AVS;
    m_pFormatName = "iMX-AVS";
    break;
  case CODEC_ID_RV10:
  case CODEC_ID_RV20:
  case CODEC_ID_RV30:
  case CODEC_ID_RV40:
    m_decOpenParam.CodecFormat = VPU_V_RV;
    m_pFormatName = "iMX-RV";
    break;
  case CODEC_ID_KMVC:
    m_decOpenParam.CodecFormat = VPU_V_AVC_MVC;
    m_pFormatName = "iMX-MVC";
    break;  
  case CODEC_ID_VP8:
    m_decOpenParam.CodecFormat = VPU_V_VP8;
    m_pFormatName = "iMX-vp8";
    break;
  case CODEC_ID_MSMPEG4V3:
    m_decOpenParam.CodecFormat = VPU_V_XVID; /* VPU_V_DIVX3 */
    m_pFormatName = "iMX-divx3";
    break;
  case CODEC_ID_MPEG4:
    switch(m_hints.codec_tag)
    {
    case _4CC('D','I','V','X'):
      m_decOpenParam.CodecFormat = VPU_V_XVID; /* VPU_V_DIVX4 */
      m_pFormatName = "iMX-divx4";
      break;
    case _4CC('D','X','5','0'):
    case _4CC('D','I','V','5'):
      m_decOpenParam.CodecFormat = VPU_V_XVID; /* VPU_V_DIVX56 */
      m_pFormatName = "iMX-divx5";
      break;
    case _4CC('X','V','I','D'):
    case _4CC('M','P','4','V'):
    case _4CC('P','M','P','4'):
    case _4CC('F','M','P','4'):
      m_decOpenParam.CodecFormat = VPU_V_XVID;
      m_pFormatName = "iMX-xvid";
      break;
    default:
      CLog::Log(LOGERROR, "iMX VPU : MPEG4 codec tag %d is not (yet) handled.\n", m_hints.codec_tag);
      return false;
    }
    break;
  default:
    CLog::Log(LOGERROR, "iMX VPU : codecid %d is not (yet) handled.\n", m_hints.codec);
    return false;
  }

  m_tsm = createTSManager(0);
  setTSManagerFrameRate(m_tsm, m_hints.fpsrate, m_hints.fpsscale);
  return true;
}

void CDVDVideoCodecIMX::Dispose(void)
{
  VpuDecRetCode  ret;
  int i;
  bool VPU_loaded = m_vpuHandle;
  CIMXRenderingFrames &renderingFrames = CIMXRenderingFrames::GetInstance();
  
  FlushOutputFrame();
  if (m_vpuHandle)
  {    
    ret = VPU_DecFlushAll(m_vpuHandle);
    if (ret != VPU_DEC_RET_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s - VPU flush failed with error code %d.\n", __FUNCTION__, ret);
    }   
    ret = VPU_DecClose(m_vpuHandle);
    if (ret != VPU_DEC_RET_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s - VPU close failed with error code %d.\n", __FUNCTION__, ret);
    }   
    m_vpuHandle = 0;
  }

  VpuFreeBuffers();

  if (m_outputBuffers != NULL)
  {
    while (VpuDeQueueFrame(false));
    renderingFrames.ReleaseBuffers();
    RestoreFB();
    delete m_outputBuffers;
    m_outputBuffers = NULL;
  }

  /* Free extramem */
  if (m_extraMem != NULL)
  {
    for (i = 0; i < m_vpuFrameBufferNum; i++)
    {
      if (m_extraMem[i].nSize > 0)
      {
        ret = VPU_DecFreeMem(&m_extraMem[i]);
        if (ret != VPU_DEC_RET_SUCCESS)
        {
          CLog::Log(LOGERROR, "%s - Release extra mem failed with error code %d.\n", __FUNCTION__, ret);
        }
        m_extraMem[i].nSize = 0;
      }
    }
    delete m_extraMem;
    m_extraMem = NULL;
  }
  m_vpuFrameBufferNum = 0;

  if (m_vpuFrameBuffers != NULL)
  {
    delete m_vpuFrameBuffers;
    m_vpuFrameBuffers = NULL;
  }

  if (VPU_loaded)
  {
    ret = VPU_DecUnLoad();
    if (ret != VPU_DEC_RET_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s - VPU unload failed with error code %d.\n", __FUNCTION__, ret);
    }
  }

  if (m_tsm != NULL)
  {
    destroyTSManager(m_tsm);
    m_tsm = NULL;
  }

  if (m_convert_bitstream)
  {
    if (m_sps_pps_context.sps_pps_data)
    {
      free(m_sps_pps_context.sps_pps_data);
      m_sps_pps_context.sps_pps_data = NULL;
    }
  }
  return;
}

int CDVDVideoCodecIMX::Decode(BYTE *pData, int iSize, double dts, double pts)
{  
  VpuDecFrameLengthInfo frameLengthInfo;
  VpuBufferNode inData;
  VpuDecRetCode  ret;
  VpuDecOutFrameInfo frameInfo;
  int decRet = 0;
  int retSatus = 0;
  int demuxer_bytes = iSize;
  uint8_t *demuxer_content = pData;
  bool bitstream_convered  = false;
  bool retry = false;

#ifdef IMX_PROFILE
  static unsigned long long previous, current;
#endif

  if (!m_vpuHandle)
  {
    VpuOpen();
    if (!m_vpuHandle)
      return VC_ERROR;
  }

#ifdef IMX_PROFILE
  current = get_time();
  CLog::Log(LOGDEBUG, "%s - delta time decode : %llu - demux size : %d  dts : %f - pts : %f\n", __FUNCTION__, current - previous, iSize, dts, pts);
  previous = current;
#endif
/* FIXME tests
  CLog::Log(LOGDEBUG, "%s - demux size : %d  dts : %f - pts : %f - %x %x %x %x\n", __FUNCTION__, iSize, dts, pts, ((unsigned int *)pData)[0], ((unsigned int *)pData)[1], ((unsigned int *)pData)[2], ((unsigned int *)pData)[3]);
  ((unsigned int *)pData)[0] = htonl(iSize-4);
*/

  while (VpuDeQueueFrame(false));

  if (pData && iSize)
  {
    if (m_convert_bitstream)
    {
      // convert demuxer packet from bitstream to bytestream (AnnexB)
      int bytestream_size = 0;
      uint8_t *bytestream_buff = NULL;

      if (!bitstream_convert(demuxer_content, demuxer_bytes, &bytestream_buff, &bytestream_size))
      {
        CLog::Log(LOGERROR, "%s - bitstream convert error...\n", __FUNCTION__);
        return  VC_ERROR;
      }

      if (bytestream_buff && (bytestream_size > 0))
      {
        bitstream_convered = true;
        demuxer_bytes = bytestream_size;
        demuxer_content = bytestream_buff;
      }      
    }

    if (pts != DVD_NOPTS_VALUE)
    {
      if (m_tsSyncRequired)
      {
        m_tsSyncRequired = false;
        resyncTSManager(m_tsm, llrint(pts) * 1000, MODE_AI);
      }
      TSManagerReceive2(m_tsm, llrint(pts) * 1000, iSize);
    }
    else
    {
      //If no pts but dts available (AVI container for instance) then use this one
      if (dts !=  DVD_NOPTS_VALUE)
      {
        if (m_tsSyncRequired)
        {
          m_tsSyncRequired = false;
          resyncTSManager(m_tsm, llrint(dts) * 1000, MODE_AI);
        }
        TSManagerReceive2(m_tsm, llrint(dts) * 1000, iSize);
      }
    }

    //CLog::Log(LOGDEBUG, "%s - Query2 : %lld\n", __FUNCTION__, TSManagerQuery2(m_tsm, NULL));
    TSManagerQuery2(m_tsm, NULL);
    inData.nSize = demuxer_bytes;
    inData.pPhyAddr = NULL;
    inData.pVirAddr = demuxer_content;
    /* FIXME TODO VP8 & DivX3 require specific sCodecData values */
    if ((m_decOpenParam.CodecFormat == VPU_V_MPEG2) ||
        (m_decOpenParam.CodecFormat == VPU_V_VC1_AP)||
        (m_decOpenParam.CodecFormat == VPU_V_XVID))
    {
      inData.sCodecData.pData = (unsigned char *)m_hints.extradata;
      inData.sCodecData.nSize = m_hints.extrasize;
    }
    else
    {
      inData.sCodecData.pData = NULL;
      inData.sCodecData.nSize = 0;
    }

    do // Decode as long as the VPU consumes data
    {
      retry = false;
      ret = VPU_DecDecodeBuf(m_vpuHandle, &inData, &decRet);
      if (ret != VPU_DEC_RET_SUCCESS)
      {
        CLog::Log(LOGERROR, "%s - VPU decode failed with error code %d.\n", __FUNCTION__, ret);
        goto out_error;
      }

      if (decRet & VPU_DEC_INIT_OK)
      /* VPU decoding init OK : We can retrieve stream info */
      {
        ret = VPU_DecGetInitialInfo(m_vpuHandle, &m_initInfo);      
        if (ret == VPU_DEC_RET_SUCCESS)
        {
          CLog::Log(LOGDEBUG, "%s - VPU Init Stream Info : %dx%d (interlaced : %d - Minframe : %d)"\
                    " - Align : %d bytes - crop : %d %d %d %d - Q16Ratio : %x\n", __FUNCTION__, 
            m_initInfo.nPicWidth, m_initInfo.nPicHeight, m_initInfo.nInterlace, m_initInfo.nMinFrameBufferCount,
            m_initInfo.nAddressAlignment, m_initInfo.PicCropRect.nLeft, m_initInfo.PicCropRect.nTop,
            m_initInfo.PicCropRect.nRight, m_initInfo.PicCropRect.nBottom, m_initInfo.nQ16ShiftWidthDivHeightRatio);
          if (VpuAllocFrameBuffers())
          {
            ret = VPU_DecRegisterFrameBuffer(m_vpuHandle, m_vpuFrameBuffers, m_vpuFrameBufferNum);
            if (ret != VPU_DEC_RET_SUCCESS)
            {
              CLog::Log(LOGERROR, "%s - VPU error while registering frame buffers (%d).\n", __FUNCTION__, ret);
              goto out_error;
            }
          }
          else
          {
            goto out_error;
          }
        }
        else
        {
          CLog::Log(LOGERROR, "%s - VPU get initial info failed (%d).\n", __FUNCTION__, ret);
          goto out_error;
        }
      }//VPU_DEC_INIT_OK

      if (decRet & VPU_DEC_ONE_FRM_CONSUMED)
      {
        ret = VPU_DecGetConsumedFrameInfo(m_vpuHandle, &frameLengthInfo);
        if (ret != VPU_DEC_RET_SUCCESS)
        {
          CLog::Log(LOGERROR, "%s - VPU error retireving info about consummed frame (%d).\n", __FUNCTION__, ret);
        }
        TSManagerValid2(m_tsm, frameLengthInfo.nFrameLength + frameLengthInfo.nStuffLength, frameLengthInfo.pFrame);
        //CLog::Log(LOGDEBUG, "%s - size : %d - key consummed : %x\n",  __FUNCTION__, frameLengthInfo.nFrameLength + frameLengthInfo.nStuffLength, frameLengthInfo.pFrame);
      }//VPU_DEC_ONE_FRM_CONSUMED

      if ((decRet & VPU_DEC_OUTPUT_DIS) ||
          (decRet & VPU_DEC_OUTPUT_MOSAIC_DIS))
      /* Frame ready to be displayed */
      {
        if (retSatus & VC_PICTURE)
            CLog::Log(LOGERROR, "%s - Second picture in the same decode call !\n", __FUNCTION__);

        ret = VPU_DecGetOutputFrame(m_vpuHandle, &frameInfo);
        if(ret != VPU_DEC_RET_SUCCESS)
        {
          CLog::Log(LOGERROR, "%s - VPU Cannot get output frame(%d).\n", __FUNCTION__, ret);
          goto out_error;
        }
        if (VpuPushFrame(&frameInfo))
        {
          retSatus |= VC_PICTURE;
        }        
      } //VPU_DEC_OUTPUT_DIS
      
      if (decRet & VPU_DEC_OUTPUT_REPEAT)
      {
        TSManagerSend(m_tsm);
        CLog::Log(LOGDEBUG, "%s - Frame repeat.\n", __FUNCTION__);
      }
      if (decRet & VPU_DEC_OUTPUT_DROPPED)
      {
        TSManagerSend(m_tsm);
        CLog::Log(LOGDEBUG, "%s - Frame dropped.\n", __FUNCTION__);
      }
      if (decRet & VPU_DEC_NO_ENOUGH_BUF)
      {
          CLog::Log(LOGERROR, "%s - No frame buffer available.\n", __FUNCTION__);
      }
      if (decRet & VPU_DEC_SKIP)
      {
        TSManagerSend(m_tsm);
        CLog::Log(LOGDEBUG, "%s - Frame skipped.\n", __FUNCTION__);
      }
      if (decRet & VPU_DEC_FLUSH)
      {
        CLog::Log(LOGNOTICE, "%s - VPU requires a flush.\n", __FUNCTION__);
        ret = VPU_DecFlushAll(m_vpuHandle);
        if (ret != VPU_DEC_RET_SUCCESS)
        {
          CLog::Log(LOGERROR, "%s - VPU flush failed(%d).\n", __FUNCTION__, ret);
        }
        retSatus = VC_FLUSHED;
      }
      if (decRet & VPU_DEC_OUTPUT_EOS)
      {
        CLog::Log(LOGNOTICE, "%s - EOS encountered.\n", __FUNCTION__);
      }
      if (decRet & VPU_DEC_NO_ENOUGH_INBUF)
      {
        // We are done with VPU decoder that time
        break;
      }
      if (!(decRet & VPU_DEC_INPUT_USED))
      {
        CLog::Log(LOGERROR, "%s - input not used : addr %p  size :%d!\n", __FUNCTION__, inData.pVirAddr, inData.nSize);
        TSManagerSend(m_tsm);
      }

      
      if (!(decRet & VPU_DEC_OUTPUT_DIS)  &&
           (inData.nSize != 0))
      {
        /* Let's process again as VPU_DEC_NO_ENOUGH_INBUF was not set
         * and we don't have an image ready if we reach that point
         */
        inData.pVirAddr = NULL;
        inData.nSize = 0;
        retry = true;
      }

    } while (retry == true);
  } //(pData && iSize)


  if (GetAvailableBufferNb() >  (m_vpuFrameBufferNum - m_extraVpuBuffers))
    retSatus |= VC_BUFFER;
  else
    if (retSatus == 0) {
      /* No Picture ready and Not enough VPU buffers.
       * Lets wait for the IPU to free a buffer
       * Anyway we have several decoded frames ready... */
      usleep(5000);
    }


#ifdef IMX_PROFILE
  CLog::Log(LOGDEBUG, "%s - returns %x - duration %lld\n", __FUNCTION__, retSatus, get_time() - previous);
#endif
   
  if (bitstream_convered)
      free(demuxer_content);

  return retSatus;

out_error:
 if (bitstream_convered)
      free(demuxer_content);
 return VC_ERROR;
}

void CDVDVideoCodecIMX::Reset()
{
  int ret;

  CLog::Log(LOGDEBUG, "%s - called\n", __FUNCTION__);

  /* We have to resync timestamp manager */
  m_tsSyncRequired = true;

  /* Flush the output frame */
  FlushOutputFrame();

  /* Flush VPU */
  ret = VPU_DecFlushAll(m_vpuHandle);
  if (ret != VPU_DEC_RET_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s - VPU flush failed with error code %d.\n", __FUNCTION__, ret);
  }

}

unsigned CDVDVideoCodecIMX::GetAllowedReferences()
{
  // Note : It is useless if CLinuxRendererGLES::GetProcessorSize returns 0 for RENDER_FMT_IMX
  return min(3, m_extraVpuBuffers / 2);
}
 
static double GetPlayerPtsSeconds()
{
  double clock_pts = 0.0;
  CDVDClock *playerclock = CDVDClock::GetMasterClock();
  if (playerclock)
    clock_pts = playerclock->GetClock() / DVD_TIME_BASE;

  return clock_pts;
}

bool CDVDVideoCodecIMX::GetPicture(DVDVideoPicture* pDvdVideoPicture)
{ 
  double currentPlayerPts;
  double ts = DVD_NOPTS_VALUE;

  if (!m_outputFrameReady)
  {
    CLog::Log(LOGERROR, "%s called while no picture ready\n", __FUNCTION__);
    return false;
  }

  pDvdVideoPicture->iFlags &= DVP_FLAG_DROPPED;
  if ((pDvdVideoPicture->iFlags != 0) || (m_dropState))
  {
    CLog::Log(LOGERROR, "%s - Flushing video picture\n", __FUNCTION__);
    pDvdVideoPicture->iFlags = DVP_FLAG_DROPPED;
    FlushOutputFrame();
  }
  else
  {
    ts = m_outputFrame.pts;
    currentPlayerPts = GetPlayerPtsSeconds() * (double)DVD_TIME_BASE;
    if (currentPlayerPts > ts)
    {
        CLog::Log(LOGERROR, "%s - player is ahead of time (%f)\n", __FUNCTION__, currentPlayerPts - ts);
    }
    //CLog::Log(LOGINFO, "%s - idx : %d - delta call %f - delta ts %f \n", __FUNCTION__, outputFrame.v4l2_buffer->index,ts - previous, ts - currentPlayerPts);
  }

#ifdef NO_V4L_RENDERING
  VPU_DecOutFrameDisplayed(m_vpuHandle, m_outputBuffers[m_outputFrame.imxOutputFrame->v4l2BufferIdx]);
  m_outputBuffers[m_outputFrame.imxOutputFrame->v4l2BufferIdx] = NULL;
#endif

  pDvdVideoPicture->pts = m_outputFrame.pts;
  pDvdVideoPicture->dts = m_outputFrame.dts;
  pDvdVideoPicture->iWidth = m_outputFrame.iWidth;
  pDvdVideoPicture->iHeight = m_outputFrame.iHeight;
  pDvdVideoPicture->iDisplayWidth = ((pDvdVideoPicture->iWidth * m_outputFrame.imxOutputFrame->nQ16ShiftWidthDivHeightRatio) + 32767) >> 16;
  pDvdVideoPicture->iDisplayHeight = pDvdVideoPicture->iHeight;
  pDvdVideoPicture->format = m_outputFrame.format;
  pDvdVideoPicture->imxOutputFrame = m_outputFrame.imxOutputFrame;
  m_outputFrameReady = false;
  return true;
}

void CDVDVideoCodecIMX::SetDropState(bool bDrop)
{
  /* We are fast enough to continue to really decode every frames
   * and avoid artefacts...
   * (Of course these frames won't be rendered but only decoded !)
   */   
  if (m_dropState != bDrop)
  {
    CLog::Log(LOGNOTICE, "%s : %d.\n", __FUNCTION__, bDrop);
    m_dropState = bDrop;   
  }
}

/* bitstream convert : Shameless copy from openmax */
/* TODO : Have a look at it as  the malloc/copy/free strategy is obviously not the most efficient one */

bool CDVDVideoCodecIMX::bitstream_convert_init(void *in_extradata, int in_extrasize)
{
  // based on h264_mp4toannexb_bsf.c (ffmpeg)
  // which is Copyright (c) 2007 Benoit Fouet <benoit.fouet@free.fr>
  // and Licensed GPL 2.1 or greater

  m_sps_pps_size = 0;
  m_sps_pps_context.sps_pps_data = NULL;
  
  // nothing to filter
  if (!in_extradata || in_extrasize < 6)
    return false;

  uint16_t unit_size;
  uint32_t total_size = 0;
  uint8_t *out = NULL, unit_nb, sps_done = 0;
  const uint8_t *extradata = (uint8_t*)in_extradata + 4;
  static const uint8_t nalu_header[4] = {0, 0, 0, 1};

  // retrieve length coded size
  m_sps_pps_context.length_size = (*extradata++ & 0x3) + 1;
  if (m_sps_pps_context.length_size == 3)
    return false;

  // retrieve sps and pps unit(s)
  unit_nb = *extradata++ & 0x1f;  // number of sps unit(s)
  if (!unit_nb)
  {
    unit_nb = *extradata++;       // number of pps unit(s)
    sps_done++;
  }
  while (unit_nb--)
  {
    unit_size = extradata[0] << 8 | extradata[1];
    total_size += unit_size + 4;
    if ( (extradata + 2 + unit_size) > ((uint8_t*)in_extradata + in_extrasize) )
    {
      free(out);
      return false;
    }
    uint8_t* new_out = (uint8_t*)realloc(out, total_size);
    if (new_out)
    {
      out = new_out;
    }
    else
    {
      CLog::Log(LOGERROR, "bitstream_convert_init failed - %s : could not realloc the buffer out",  __FUNCTION__);
      free(out);
      return false;
    }

    memcpy(out + total_size - unit_size - 4, nalu_header, 4);
    memcpy(out + total_size - unit_size, extradata + 2, unit_size);
    extradata += 2 + unit_size;

    if (!unit_nb && !sps_done++)
      unit_nb = *extradata++;     // number of pps unit(s)
  }

  m_sps_pps_context.sps_pps_data = out;
  m_sps_pps_context.size = total_size;
  m_sps_pps_context.first_idr = 1;

  return true;
}

bool CDVDVideoCodecIMX::bitstream_convert(BYTE* pData, int iSize, uint8_t **poutbuf, int *poutbuf_size)
{
  // based on h264_mp4toannexb_bsf.c (ffmpeg)
  // which is Copyright (c) 2007 Benoit Fouet <benoit.fouet@free.fr>
  // and Licensed GPL 2.1 or greater

  uint8_t *buf = pData;
  uint32_t buf_size = iSize;
  uint8_t  unit_type;
  int32_t  nal_size;
  uint32_t cumul_size = 0;
  const uint8_t *buf_end = buf + buf_size;

  do
  {
    if (buf + m_sps_pps_context.length_size > buf_end)
      goto fail;

    if (m_sps_pps_context.length_size == 1)
      nal_size = buf[0];
    else if (m_sps_pps_context.length_size == 2)
      nal_size = buf[0] << 8 | buf[1];
    else
      nal_size = buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3];

    // FIXME CLog::Log(LOGERROR, "%s - nal_size : %d \n", __FUNCTION__, nal_size);
    buf += m_sps_pps_context.length_size;
    unit_type = *buf & 0x1f;

    if (buf + nal_size > buf_end || nal_size < 0)
      goto fail;

    // prepend only to the first type 5 NAL unit of an IDR picture
    if (m_sps_pps_context.first_idr && unit_type == 5)
    {
      bitstream_alloc_and_copy(poutbuf, poutbuf_size,
        m_sps_pps_context.sps_pps_data, m_sps_pps_context.size, buf, nal_size);
      m_sps_pps_context.first_idr = 0;
    }
    else
    {
      bitstream_alloc_and_copy(poutbuf, poutbuf_size, NULL, 0, buf, nal_size);
      if (!m_sps_pps_context.first_idr && unit_type == 1)
          m_sps_pps_context.first_idr = 1;
    }

    buf += nal_size;
    cumul_size += nal_size + m_sps_pps_context.length_size;
  } while (cumul_size < buf_size);

  return true;

fail:
  free(*poutbuf);
  *poutbuf = NULL;
  *poutbuf_size = 0;
  return false;
}

void CDVDVideoCodecIMX::bitstream_alloc_and_copy(
  uint8_t **poutbuf,      int *poutbuf_size,
  const uint8_t *sps_pps, uint32_t sps_pps_size,
  const uint8_t *in,      uint32_t in_size)
{
  // based on h264_mp4toannexb_bsf.c (ffmpeg)
  // which is Copyright (c) 2007 Benoit Fouet <benoit.fouet@free.fr>
  // and Licensed GPL 2.1 or greater

  #define CHD_WB32(p, d) { \
    ((uint8_t*)(p))[3] = (d); \
    ((uint8_t*)(p))[2] = (d) >> 8; \
    ((uint8_t*)(p))[1] = (d) >> 16; \
    ((uint8_t*)(p))[0] = (d) >> 24; }

  uint32_t offset = *poutbuf_size;
  uint8_t nal_header_size = offset ? 3 : 4;

  *poutbuf_size += sps_pps_size + in_size + nal_header_size;
  *poutbuf = (uint8_t*)realloc(*poutbuf, *poutbuf_size);
  if (sps_pps)
    memcpy(*poutbuf + offset, sps_pps, sps_pps_size);

  memcpy(*poutbuf + sps_pps_size + nal_header_size + offset, in, in_size);
  if (!offset)
  {
    CHD_WB32(*poutbuf + sps_pps_size, 1);
  }
  else
  {
    (*poutbuf + offset + sps_pps_size)[0] = 0;
    (*poutbuf + offset + sps_pps_size)[1] = 0;
    (*poutbuf + offset + sps_pps_size)[2] = 1;
  }
}
