#pragma once
/*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://www.xbmc.org
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
#include <queue>
#include <linux/videodev2.h>
#include <imx-mm/vpu/vpu_wrapper.h>
#include "DVDVideoCodec.h"
#include "DVDStreamInfo.h"
#include "threads/CriticalSection.h"


//#define IMX_PROFILE

/* FIXME TODO Develop real proper CVPUBuffer class */
#define VPU_DEC_MAX_NUM_MEM_NUM 20
typedef struct
{
  //virtual mem info
  int nVirtNum;
  unsigned int virtMem[VPU_DEC_MAX_NUM_MEM_NUM];

  //phy mem info
  int nPhyNum;
  unsigned int phyMem_virtAddr[VPU_DEC_MAX_NUM_MEM_NUM];
  unsigned int phyMem_phyAddr[VPU_DEC_MAX_NUM_MEM_NUM];
  unsigned int phyMem_cpuAddr[VPU_DEC_MAX_NUM_MEM_NUM];
  unsigned int phyMem_size[VPU_DEC_MAX_NUM_MEM_NUM];      
} DecMemInfo;

/* Output frame properties */
struct CIMXOutputFrame {
  int v4l2BufferIdx;
  VpuFieldType field;
  VpuRect picCrop;
  unsigned int nQ16ShiftWidthDivHeightRatio;
#ifdef IMX_PROFILE
  unsigned long long pushTS;
#endif
};

class CIMXRenderingFrames
{
public:
  static CIMXRenderingFrames& GetInstance();
  bool AllocateBuffers(const struct v4l2_format *, int);
  void *GetVirtAddr(int idx);
  void *GetPhyAddr(int idx);
  void ReleaseBuffers();
  int FindBuffer(void *);
  int DeQueue(bool wait);
  void Queue(CIMXOutputFrame *, struct v4l2_crop &);

private:
  CIMXRenderingFrames();
  void __ReleaseBuffers();

  static const char  *m_v4lDeviceName;     // V4L2 device Name
  static CIMXRenderingFrames* m_instance;  // Unique instance of the class
  
  CCriticalSection    m_renderingFramesLock; // Lock to ensure multithreading safety for class fields
  bool                m_ready;             // Buffers are allocated and frames can be Queued/Dequeue
  int                 m_v4lfd;             // fd on V4L2 device
  struct v4l2_buffer *m_v4lBuffers;        // Table of V4L buffer info (as returned by VIDIOC_QUERYBUF)
  int                 m_bufferNum;         // Number of allocated V4L2 buffers
  struct v4l2_crop    m_crop;              // Current cropping properties
  bool                m_streamOn;          // Flag that indicates whether streaming in on (from V4L point of view)
  VpuFieldType        m_currentField;      // Current field type
  int                 m_pushedFrames;      // Number of frames queued in V4L2
  void              **m_virtAddr;          // Table holding virtual adresses of mmaped V4L2 buffers
};

class CDVDVideoCodecIMX : public CDVDVideoCodec
{
public:
  CDVDVideoCodecIMX();
  virtual ~CDVDVideoCodecIMX();

  // Methods from CDVDVideoCodec which require overrides
  virtual bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options);
  virtual void Dispose(void);
  virtual int  Decode(BYTE *pData, int iSize, double dts, double pts);
  virtual void Reset(void);
  virtual bool GetPicture(DVDVideoPicture *pDvdVideoPicture);
  virtual void SetDropState(bool bDrop);
  virtual const char* GetName(void) { return (const char*)m_pFormatName; }
  virtual unsigned GetAllowedReferences();

protected:

  bool VpuOpen(void);
  bool VpuAllocBuffers(VpuMemInfo *);
  bool VpuFreeBuffers(void);
  bool VpuAllocFrameBuffers(void);
  bool VpuPushFrame(VpuDecOutFrameInfo*);
  bool VpuDeQueueFrame(bool);
  int GetAvailableBufferNb(void);
  void InitFB(void);
  void RestoreFB(void);
  void FlushOutputFrame(void);

  static const int    m_extraVpuBuffers;   // Number of additional buffers for VPU

  CDVDStreamInfo      m_hints;             // Hints from demuxer at stream opening
  const char         *m_pFormatName;       // Current decoder format name
  VpuDecOpenParam     m_decOpenParam;      // Parameters required to call VPU_DecOpen
  DecMemInfo          m_decMemInfo;        // VPU dedicated memory description
  VpuDecHandle        m_vpuHandle;         // Handle for VPU library calls
  VpuDecInitInfo      m_initInfo;          // Initial info returned from VPU at decoding start
  void               *m_tsm;               // fsl Timestamp manager (from gstreamer implementation)
  bool                m_tsSyncRequired;    // state whether timestamp manager has to be sync'ed
  bool                m_dropState;         // Current drop state
  int                 m_vpuFrameBufferNum; // Total number of allocated frame buffers
  VpuFrameBuffer     *m_vpuFrameBuffers;   // Table of VPU frame buffers description
  VpuMemDesc         *m_extraMem;          // Table of allocated extra Memory
  VpuFrameBuffer    **m_outputBuffers;     // Table of buffer pointers from VPU (index is V4L buf index) (used to call properly VPU_DecOutFrameDisplayed)
  DVDVideoPicture     m_outputFrame;       // Decoded frame ready to be retrieved by GetPicture
  bool                m_outputFrameReady;  // State whether m_outputFrame is available or not


  /* FIXME : Rework is still required for fields below this line */

  /* create a real class and share with openmax ? */
  // bitstream to bytestream (Annex B) conversion support.
  bool bitstream_convert_init(void *in_extradata, int in_extrasize);
  bool bitstream_convert(BYTE* pData, int iSize, uint8_t **poutbuf, int *poutbuf_size);
  static void bitstream_alloc_and_copy( uint8_t **poutbuf, int *poutbuf_size,
  const uint8_t *sps_pps, uint32_t sps_pps_size, const uint8_t *in, uint32_t in_size);  
  typedef struct omx_bitstream_ctx {
      uint8_t  length_size;
      uint8_t  first_idr;
      uint8_t *sps_pps_data;
      uint32_t size;
      omx_bitstream_ctx()
      {
        length_size = 0;
        first_idr = 0;
        sps_pps_data = NULL;
        size = 0;
      }
  } omx_bitstream_ctx;
  uint32_t          m_sps_pps_size;
  omx_bitstream_ctx m_sps_pps_context; 
  bool m_convert_bitstream;

};
