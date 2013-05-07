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
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "utils/log.h"
#include "DVDClock.h"

/* FIXME get rid of these defines properly */
#define FRAME_ALIGN 16
#define MEDIAINFO 1
#define _4CC(c1,c2,c3,c4) (((uint32_t)(c4)<<24)|((uint32_t)(c3)<<16)|((uint32_t)(c2)<<8)|(uint32_t)(c1))
#define Align(ptr,align)  (((unsigned int)ptr + (align) - 1)/(align)*(align))

/* video device on which the video will be rendered (/dev/video17 => /dev/fb1) */
const char *CDVDVideoCodecIMX::m_v4lDeviceName = "/dev/video17";
/* Experiments show that we need at least one more (+1) V4L buffer than the min value returned by the VPU */
const int CDVDVideoCodecIMX::m_extraVpuBuffers = 4;

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
  m_decOpenParam.nChromaInterleave = 0;   // No YUV chroma interleave
  m_decOpenParam.nMapType = 0;            // Linear
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

  /* FIXME TODO Is there a need to explicitly configure :
   * - VPU_DEC_CONF_BUFDELAY
   * - VPU_DEC_CONF_INPUTTYPE
   */
  return true;

VpuOpenError:
  Dispose();
  return false;
}

void CDVDVideoCodecIMX::initFB(void)
{
  struct fb_var_screeninfo screen;
  struct mxcfb_gbl_alpha alpha;
  struct mxcfb_color_key colorKey;
  int fd;
  
  fd = open("/dev/fb0",O_RDWR);
  /* set FG/BG semi opaque */
  alpha.alpha = 220;
  alpha.enable = 1;  
  ioctl(fd, MXCFB_SET_GBL_ALPHA, &alpha);
  /* Enable color keying */
  colorKey.enable = 1;
  colorKey.color_key = (1 << 16) | (2 << 8) | 3;
  if (ioctl(fd, MXCFB_SET_CLR_KEY, &colorKey) < 0)
    CLog::Log(LOGERROR, "%s - Error while trying to enable color keying %s.\n", __FUNCTION__, strerror(errno));
  /* Retrieve screen resolution */
  ioctl(fd, FBIOGET_VSCREENINFO, &screen);
  close(fd);

  /* Keep resolution */
  m_xscreen = screen.xres;
  m_yscreen = screen.yres;  
}

void CDVDVideoCodecIMX::restoreFB(void)
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
  struct v4l2_requestbuffers bufReq;
  struct v4l2_format fmt;
  int ret, i;
  int width, height;
  int ySize, cSize;
  int video_width, video_height;
  VpuDecRetCode vpuRet;
  struct v4l2_buffer v4lBuf;

  m_v4lfd = open(m_v4lDeviceName, O_RDWR|O_NONBLOCK, 0);
  if (m_v4lfd < 0)
  {
    CLog::Log(LOGERROR, "%s - Error while trying to open %s.\n", __FUNCTION__, m_v4lDeviceName);
    return false;
  }
  
  video_width = Align(m_initInfo.nPicWidth, FRAME_ALIGN);
  video_height = Align(m_initInfo.nPicHeight, FRAME_ALIGN);
  
  /* Set crop to keep aspect ratio */
  m_crop.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  m_crop.c.top = 0;
  m_crop.c.left = 0;
  m_crop.c.width = m_xscreen;
  m_crop.c.height = m_yscreen;  
  if (m_crop.c.width * video_height > m_crop.c.height * video_width)
  {
    width = video_width * m_crop.c.height / video_height;
    width = width & 0xFFFFFFF8;
    m_crop.c.left = m_crop.c.left + (m_crop.c.width - width) / 2;
    m_crop.c.width = width;
  }
  else if (m_crop.c.width * video_height < m_crop.c.height * video_width)
  {
    height = video_height * m_crop.c.width / video_width;
    height = height & 0xFFFFFFF8;
    m_crop.c.top = m_crop.c.top + (m_crop.c.height - height) / 2;
    m_crop.c.height = height;
  }
  ret = ioctl(m_v4lfd, VIDIOC_S_CROP, &m_crop);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "%s - Error while setting crop (ret %d : %s).\n", __FUNCTION__, ret, strerror(errno));
    return false;
  }
  CLog::Log(LOGDEBUG, "%s - V4L crop : %d %d %d %d\n", __FUNCTION__, m_crop.c.top, m_crop.c.left, m_crop.c.width, m_crop.c.height);

  /* Set V4L2  Format */
  memset(&fmt, 0, sizeof(fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  fmt.fmt.pix.width = video_width;
  fmt.fmt.pix.height = video_height; 
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
  fmt.fmt.pix.priv = 0;    
  ret = ioctl(m_v4lfd, VIDIOC_S_FMT, &fmt);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "%s - Error while setting V4L format (ret %d : %s).\n", __FUNCTION__, ret, strerror(errno));
    return false;
  }
  ret = ioctl(m_v4lfd, VIDIOC_G_FMT, &fmt);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "%s - Error while getting V4L format (ret %d : %s).\n", __FUNCTION__, ret, strerror(errno));
    return false;
  }
  
  /* Alloc V4L2 buffers */
  memset(&bufReq, 0, sizeof(bufReq));
  bufReq.count =  m_initInfo.nMinFrameBufferCount + m_extraVpuBuffers;
  bufReq.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  bufReq.memory = V4L2_MEMORY_MMAP;
  ret = ioctl(m_v4lfd, VIDIOC_REQBUFS, &bufReq);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "%s - %d Hw buffer allocation error (%d)\n", __FUNCTION__, bufReq.count, ret);
    return false;
  }
  CLog::Log(LOGDEBUG, "%s - %d Hw buffer of %d bytes allocated\n", __FUNCTION__, bufReq.count, fmt.fmt.pix.sizeimage);  
  
  m_vpuFrameBufferNum = bufReq.count;
  m_outputBuffers = new VpuFrameBuffer*[m_vpuFrameBufferNum];
  m_vpuFrameBuffers = new VpuFrameBuffer[m_vpuFrameBufferNum];  
  m_v4lBuffers = new v4l2_buffer[m_vpuFrameBufferNum];
  ySize = fmt.fmt.pix.width * fmt.fmt.pix.height;
  cSize = ySize / 4;
  for (i=0 ; i<m_vpuFrameBufferNum; i++)
  {
    m_outputBuffers[i] = NULL;
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
    m_vpuFrameBuffers[i].pbufVirtY = (unsigned char *)mmap(NULL, v4lBuf.length, PROT_READ | PROT_WRITE, MAP_SHARED, m_v4lfd, v4lBuf.m.offset);    
    /* 2nd query to retrieve real Physical address (iMX6 bug) */
    ret = ioctl (m_v4lfd, VIDIOC_QUERYBUF, &v4lBuf);
    if (ret < 0)
    {
      CLog::Log(LOGERROR, "%s - Error during 2nd query of V4L buffer (ret %d : %s)\n", __FUNCTION__, ret, strerror(errno));
      return false;
    }
    m_v4lBuffers[i] = v4lBuf;
    m_vpuFrameBuffers[i].nStrideY = fmt.fmt.pix.width;
    m_vpuFrameBuffers[i].nStrideC = m_vpuFrameBuffers[i].nStrideY / 2;
    m_vpuFrameBuffers[i].pbufY = (unsigned char *)v4lBuf.m.offset;
    m_vpuFrameBuffers[i].pbufCb = m_vpuFrameBuffers[i].pbufY + ySize;
    m_vpuFrameBuffers[i].pbufCr = m_vpuFrameBuffers[i].pbufCb + cSize;
    m_vpuFrameBuffers[i].pbufVirtCb = m_vpuFrameBuffers[i].pbufVirtY + ySize;
    m_vpuFrameBuffers[i].pbufVirtCr = m_vpuFrameBuffers[i].pbufVirtCb + cSize;    
    /* Dont care about tile */
    m_vpuFrameBuffers[i].pbufY_tilebot = 0;
    m_vpuFrameBuffers[i].pbufCb_tilebot = 0;
    m_vpuFrameBuffers[i].pbufVirtY_tilebot = 0;
    m_vpuFrameBuffers[i].pbufVirtCb_tilebot = 0;
  }
  
  /* Allocate physical extra memory */
  m_extraMem.nSize = cSize * m_vpuFrameBufferNum;
  vpuRet = VPU_DecGetMem(&m_extraMem);
  if (vpuRet != VPU_DEC_RET_SUCCESS)
  {
    m_extraMem.nSize = 0;
    CLog::Log(LOGERROR, "%s - Extra memory (%d bytes) allocation failure (%d).\n",
               __FUNCTION__, m_extraMem.nSize , vpuRet);
    return false;
  }
  m_vpuFrameBuffers[0].pbufMvCol = (unsigned char *)m_extraMem.nPhyAddr;
  m_vpuFrameBuffers[0].pbufVirtMvCol = (unsigned char *)m_extraMem.nVirtAddr;
  for (i=1 ; i<m_vpuFrameBufferNum; i++)
  {
    m_vpuFrameBuffers[i].pbufMvCol =  m_vpuFrameBuffers[i-1].pbufMvCol + cSize;
    m_vpuFrameBuffers[i].pbufVirtMvCol = m_vpuFrameBuffers[i-1].pbufVirtMvCol + cSize;
  }
  
  return true;
}

bool CDVDVideoCodecIMX::VpuPushFrame(VpuFrameBuffer *frameBuffer)
{
  int i, ret, type;
  
  /* Find Frame */
  for (i=0; i<m_vpuFrameBufferNum; i++)
    if ((unsigned int)frameBuffer->pbufY == m_v4lBuffers[i].m.offset)
      break;
  if (i >= m_vpuFrameBufferNum)
  {
    CLog::Log(LOGERROR, "%s - V4L buffer not found\n", __FUNCTION__);
    return false;
  }

  if (m_outputBuffers[i] != NULL)
  {
    CLog::Log(LOGERROR, "%s - Try to enqueue bufer which was not dequeued !\n", __FUNCTION__);
    return false;
  }
     
  /* Store the pointer to be able to invoke VPU_DecOutFrameDisplayed when the buffer will be dequeued*/
  m_outputBuffers[i] = frameBuffer;
  
  /* Make sure the buffer is immediatly displayed 
    FIXME : Should not be done here but in the renderer...*/
  m_v4lBuffers[i].timestamp.tv_sec = 0;
  m_v4lBuffers[i].timestamp.tv_usec = 0;  
  ret = ioctl(m_v4lfd, VIDIOC_QBUF, &m_v4lBuffers[i]);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "%s - V4L Queue buffer failed (ret %d : %s)\n", 
               __FUNCTION__, ret, strerror(errno));      
    return false;
  }

  if (m_nframes == 0)
  {
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ret = ioctl(m_v4lfd, VIDIOC_STREAMON, &type);
    if (ret < 0)
    {
      CLog::Log(LOGERROR, "%s - V4L Stream ON failed (ret %d : %s)\n",
               __FUNCTION__, ret, strerror(errno));
    }
    m_streamon = true;
    ioctl(m_v4lfd, VIDIOC_S_CROP, &m_crop);       
  }

  return true;
}


bool CDVDVideoCodecIMX::VpuDeQueueFrame(void)
{
  int ret;
  struct v4l2_buffer buf;
  
  if ((m_v4lfd == -1) ||
      (!m_streamon))
  {
    return false;
  }
  buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  buf.memory = V4L2_MEMORY_MMAP;
  ret = ioctl(m_v4lfd, VIDIOC_DQBUF, &buf);
  if (ret != 0)
  {
    if (errno != EAGAIN)
      CLog::Log(LOGERROR, "%s - Dequeue buffer error (ret %d : %s)\n",
              __FUNCTION__, ret, strerror(errno));
    return false;
  }
  /*CLog::Log(LOGDEBUG, "%s - Dequeue buffer %d\n",
              __FUNCTION__, buf.index);*/
  // Mark frame as displayed for VPU
  VPU_DecOutFrameDisplayed(m_vpuHandle, m_outputBuffers[buf.index]);
  m_outputBuffers[buf.index] = NULL;
  return true;  
}


CDVDVideoCodecIMX::CDVDVideoCodecIMX()
{
  m_pFormatName = "iMX-xxx";
  memset(&m_decMemInfo, 0, sizeof(DecMemInfo));
  m_vpuHandle = 0;
  m_v4lfd = -1;
  m_vpuFrameBuffers = NULL;
  m_outputBuffers = NULL;
  m_v4lBuffers = NULL;
  m_vpuFrameBufferNum = 0;
  m_extraMem.nSize = 0;
  m_streamon = false;   
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

  m_nframes = 0;
  m_hints = hints;
  CLog::Log(LOGNOTICE, "Let's decode with iMX VPU\n");    
  
#ifdef MEDIAINFO
  CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: fpsrate %d / fpsscale %d\n", m_hints.fpsrate, m_hints.fpsscale);
  CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: CodecID %d \n", m_hints.codec);
  CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: StreamType %d \n", m_hints.type);
  CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: Level %d \n", m_hints.level);
  CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: Profile %d \n", m_hints.profile);
  CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: PTS_invalid %d \n", m_hints.ptsinvalid);
  CLog::Log(LOGDEBUG, "Decode: MEDIAINFO: Tag %d \n", m_hints.codec_tag);
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
#endif

  m_convert_bitstream = false;
  switch(m_hints.codec)
  {
  case CODEC_ID_H263:
    m_decOpenParam.CodecFormat = VPU_V_H263;   
    m_pFormatName = "iMX-h263";
    break;
  case CODEC_ID_H264:
    m_decOpenParam.CodecFormat = VPU_V_AVC;
    m_pFormatName = "iMX-h264";
    if (hints.extrasize < 7 || hints.extradata == NULL)
    {
      CLog::Log(LOGNOTICE, "%s - avcC data too small or missing", __func__);
      return false;
    }
    if ( *(char*)hints.extradata == 1 )
      m_convert_bitstream = bitstream_convert_init(hints.extradata, hints.extrasize);
    break;
  case CODEC_ID_VC1:
    m_decOpenParam.CodecFormat = VPU_V_VC1;
    m_pFormatName = "iMX-vc1";
    break;
/* FIXME TODO
 * => for this type we have to set height, width, nChromaInterleave and nMapType 
  case CODEC_ID_MJPEG:
    m_decOpenParam.CodecFormat = VPU_V_MJPG;
    m_pFormatName = "iMX-mjpg";
    break;
*/
  case CODEC_ID_VP8:
    m_decOpenParam.CodecFormat = VPU_V_VP8;
    m_pFormatName = "iMX-vp8";
    break;
  case CODEC_ID_MPEG4:
    switch(m_hints.codec_tag)
    {
    case _4CC('D','I','V','X'):
      m_decOpenParam.CodecFormat = VPU_V_DIVX4;
      m_pFormatName = "iMX-divx4";
      break;
    case _4CC('D','X','5','0'):
    case _4CC('D','I','V','5'):
      m_decOpenParam.CodecFormat = VPU_V_DIVX56;
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

  initFB();
  return VpuOpen();
}

void CDVDVideoCodecIMX::Dispose(void)
{
  VpuDecRetCode  ret;
  int i, type;
  
  if (m_vpuHandle)
  {
    ret = VPU_DecClose(m_vpuHandle);
    if (ret != VPU_DEC_RET_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s - VPU close failed with error code %d.\n", __FUNCTION__, ret);
    }   
    
  }

  VpuFreeBuffers();

  if (m_v4lfd >= 0)
  {
    while (VpuDeQueueFrame());
    /* stream off */
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ioctl (m_v4lfd, VIDIOC_STREAMOFF, &type);
    m_streamon = false;
    /* unmap vpuFrameBuffers  */
    if ((m_vpuFrameBuffers != NULL) && (m_v4lBuffers != NULL))
    {
      for (i = 0; i < m_vpuFrameBufferNum; i++)
      {
        munmap (m_vpuFrameBuffers[i].pbufVirtY, m_v4lBuffers[i].length);
      }
    }
    if (m_vpuFrameBuffers != NULL)
    {
      delete m_vpuFrameBuffers;
      m_vpuFrameBuffers = NULL;
    }
    if (m_v4lBuffers != NULL)
    {
      delete m_v4lBuffers;
      m_v4lBuffers = NULL;
    }    
    if (m_outputBuffers != NULL)
    {
      delete m_outputBuffers;
      m_outputBuffers = NULL;
    }    
    m_vpuFrameBufferNum = 0;

    /* vpuFrameBuffers (V4L buffers) will be released by this close ? */
    close(m_v4lfd);
    m_v4lfd = -1;   

    /* Free extramem */
    if (m_extraMem.nSize > 0)
    {
      ret = VPU_DecFreeMem(&m_extraMem);
      if (ret != VPU_DEC_RET_SUCCESS)
      {
        CLog::Log(LOGERROR, "%s - Release extra mem failed with error code %d.\n", __FUNCTION__, ret);
      }
      m_extraMem.nSize = 0;
    }
  }

  ret = VPU_DecUnLoad();
  if (ret != VPU_DEC_RET_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s - VPU unload failed with error code %d.\n", __FUNCTION__, ret);
  }

  restoreFB();
  return;
}

int CDVDVideoCodecIMX::Decode(BYTE *pData, int iSize, double dts, double pts)
{  
//  VpuDecFrameLengthInfo frameLengthInfo;
  VpuBufferNode inData;
  VpuDecRetCode  ret;
  VpuDecOutFrameInfo frameInfo;
  int decRet = 0;
  int retSatus = 0;
  int demuxer_bytes = iSize;
  uint8_t *demuxer_content = pData;
  bool bitstream_convered  = false;

  //FIXME a virer static int file_raw  = -1;
  
  if (!m_vpuHandle)
    return VC_ERROR;

  while (VpuDeQueueFrame());

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
    /* FIXME a virer
    if (file_raw == -1)
    {
      file_raw = open("/tmp/stream.raw", O_RDWR | O_TRUNC |O_CREAT, S_IREAD|S_IWRITE);
    }
    if (file_raw != -1)
    {
      write(file_raw, demuxer_content, demuxer_bytes);
      fsync(file_raw);
    }
    */
    inData.nSize = demuxer_bytes;
    inData.pPhyAddr = NULL;
    inData.pVirAddr = demuxer_content;
    /* FIXME TODO VP8 & DivX3 require specific sCodecData values */
    inData.sCodecData.pData = NULL;
    inData.sCodecData.nSize = 0;
    ret = VPU_DecDecodeBuf(m_vpuHandle, &inData, &decRet);
    if (bitstream_convered)
      free(demuxer_content);
    if (ret != VPU_DEC_RET_SUCCESS)
    {
      CLog::Log(LOGERROR, "%s - VPU decode failed with error code %d.\n", __FUNCTION__, ret);
      return VC_ERROR;
    }
    else
    {
      //CLog::Log(LOGDEBUG, "%s - frames : %d VPU decode success : %x.\n", __FUNCTION__, m_nframes, decRet);
      retSatus |= VC_BUFFER;
    }
    
    if (decRet & VPU_DEC_INIT_OK)
    /* VPU decoding init OK : We can retrieve stream info */
    {
      ret = VPU_DecGetInitialInfo(m_vpuHandle, &m_initInfo);      
      if (ret == VPU_DEC_RET_SUCCESS)
      {
        CLog::Log(LOGDEBUG, "%s - VPU Init Stream Info : %dx%d (interlaced : %d - Minframe : %d)"\
                  " - Align : %d bytes - crop : %d %d %d %d\n", __FUNCTION__, 
          m_initInfo.nPicWidth, m_initInfo.nPicHeight, m_initInfo.nInterlace, m_initInfo.nMinFrameBufferCount,
          m_initInfo.nAddressAlignment, m_initInfo.PicCropRect.nLeft, m_initInfo.PicCropRect.nTop,
           m_initInfo.PicCropRect.nRight, m_initInfo.PicCropRect.nBottom);
        if (VpuAllocFrameBuffers())
        {
          ret = VPU_DecRegisterFrameBuffer(m_vpuHandle, m_vpuFrameBuffers, m_vpuFrameBufferNum);
          if (ret != VPU_DEC_RET_SUCCESS)
          {
            CLog::Log(LOGERROR, "%s - VPU error while registering frame buffers (%d).\n", __FUNCTION__, ret);
            return VC_ERROR;
          }
        }
        else
        {
          return VC_ERROR;
        }
      }
      else
      {
        CLog::Log(LOGERROR, "%s - VPU get initial info failed (%d).\n", __FUNCTION__, ret);
        return VC_ERROR;
      }
    }//VPU_DEC_INIT_OK

    if (decRet & VPU_DEC_ONE_FRM_CONSUMED)
    {/*
      ret = VPU_DecGetConsumedFrameInfo(m_vpuHandle, &frameLengthInfo);
      if (ret != VPU_DEC_RET_SUCCESS)
      {
        CLog::Log(LOGERROR, "%s - VPU error retireving info about consummed frame (%d).\n", __FUNCTION__, ret);
      }
      */
      /* FIXME TS */
    }//VPU_DEC_ONE_FRM_CONSUMED
    
    if (decRet & VPU_DEC_OUTPUT_DIS)
    /* Frame ready to be displayed */
    {
      ret = VPU_DecGetOutputFrame(m_vpuHandle, &frameInfo);
      if(ret != VPU_DEC_RET_SUCCESS)
      {
        CLog::Log(LOGERROR, "%s - VPU Cannot get output frame(%d).\n", __FUNCTION__, ret);
        return VC_ERROR;
      } 
      
      VpuPushFrame(frameInfo.pDisplayFrameBuf);
      m_nframes++;
      retSatus |= VC_PICTURE;
    } //VPU_DEC_OUTPUT_DIS    
    
    if (decRet & VPU_DEC_OUTPUT_MOSAIC_DIS)
    {
        CLog::Log(LOGERROR, "%s - Unexpected OUTPUT_MOSAIC_DIS.\n", __FUNCTION__);
    }
    if (decRet & VPU_DEC_OUTPUT_REPEAT)
    {
      CLog::Log(LOGDEBUG, "%s - Frame repeat.\n", __FUNCTION__);
      /* FIXME TS*/
    }    
    if (decRet & VPU_DEC_OUTPUT_DROPPED)
    {
       CLog::Log(LOGDEBUG, "%s - Frame dropped.\n", __FUNCTION__);
       /* FIXME TS*/
    }
    if (decRet & VPU_DEC_NO_ENOUGH_BUF)
    {
        CLog::Log(LOGERROR, "%s - No frame buffer available.\n", __FUNCTION__);
    }
    if (decRet & VPU_DEC_SKIP)
    {
       CLog::Log(LOGDEBUG, "%s - Frame skipped.\n", __FUNCTION__);
       /* FIXME TS*/
    }
    if (decRet & VPU_DEC_FLUSH)
    {
      CLog::Log(LOGNOTICE, "%s - VPU requires a flush.\n", __FUNCTION__);
      ret = VPU_DecFlushAll(m_vpuHandle);
      if (ret != VPU_DEC_RET_SUCCESS)
      {
        CLog::Log(LOGERROR, "%s - VPU flush failed(%d).\n", __FUNCTION__, ret);
      }
    }
    if (decRet & VPU_DEC_OUTPUT_EOS)
    {
       CLog::Log(LOGNOTICE, "%s - EOS encountered.\n", __FUNCTION__);
    }    
    if (decRet & VPU_DEC_NO_ENOUGH_INBUF)
    {
       CLog::Log(LOGNOTICE, "%s - Not enough input buffer.\n", __FUNCTION__);
    }    
    if (!(decRet & VPU_DEC_INPUT_USED))
    {
      CLog::Log(LOGERROR, "%s - input not used !\n", __FUNCTION__);
    }
    
   /* FIXME To be implemented with latest version of VPU firmware 
    if (decRet & VPU_DEC_RESOLUTION_CHANGED)
    {
      CLog::Log(LOGERROR, "%s - VPU error : Change resolution not handled for now\n", __FUNCTION__);
    }*/
   
  } //(pData && iSize)

  return retSatus;
}

void CDVDVideoCodecIMX::Reset()
{
  /* FIXME TODO
   * At least Flush the decoder and the video frames
   */
}

bool CDVDVideoCodecIMX::GetPicture(DVDVideoPicture* pDvdVideoPicture)
{

  /* FIXME TODO  :
    - Regarding pts computation  (integrate fsl TSManager libary used by gstreamer)
    - Handle V4L in the renderer 
  */
  pDvdVideoPicture->pts = DVD_NOPTS_VALUE;
  pDvdVideoPicture->dts = DVD_NOPTS_VALUE;
  pDvdVideoPicture->iFlags = 0;
  pDvdVideoPicture->iWidth = m_initInfo.nPicWidth;
  pDvdVideoPicture->iHeight = m_initInfo.nPicHeight;
  pDvdVideoPicture->iDisplayWidth = m_initInfo.nPicWidth;
  pDvdVideoPicture->iDisplayHeight = m_initInfo.nPicHeight;
  pDvdVideoPicture->format = RENDER_FMT_BYPASS;
  
  return true;   
}


void CDVDVideoCodecIMX::SetDropState(bool bDrop)
{
  static int lastState = -1;
  VpuDecRetCode  ret; 
  VpuDecConfig config;    
  int param;
    
  //CLog::Log(LOGERROR, "%s - SetDropState : %d.\n", __FUNCTION__, bDrop);
  
  if (lastState != bDrop)
  {
    config = VPU_DEC_CONF_SKIPMODE;
    if (bDrop)
      param = VPU_DEC_SKIPALL;
    else 
      param = VPU_DEC_SKIPNONE;
    ret = VPU_DecConfig(m_vpuHandle, config, &param);
    if (ret != VPU_DEC_RET_SUCCESS)
      CLog::Log(LOGERROR, "%s - Error while setting Skip mode : %d.\n", __FUNCTION__, ret);  
    lastState = bDrop;
  }
}



/* bitstreal convertion : Shameless copy from openmax */

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

