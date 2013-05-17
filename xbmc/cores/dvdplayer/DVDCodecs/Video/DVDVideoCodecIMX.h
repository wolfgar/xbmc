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

class CDTSManager
{
public:
  CDTSManager();
  ~CDTSManager();

  void Register(double pts, int size);
  bool Associate(int size, void *key);
  void Flush();
  double Get(void *key);

protected:

  int FindFree();

  static const int maxEntries = 32;
  struct TEntry
  {
    unsigned int id;
    int  size;
    void *key;
    double pts;
    bool used;
  } m_entries[maxEntries];
  unsigned int m_current;


};



class CDVDVideoCodecIMX : public CDVDVideoCodec
{
public:
  CDVDVideoCodecIMX();
  virtual ~CDVDVideoCodecIMX();

  // Required overrides
  virtual bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options);
  virtual void Dispose(void);
  virtual int  Decode(BYTE *pData, int iSize, double dts, double pts);
  virtual void Reset(void);
  virtual bool GetPicture(DVDVideoPicture *pDvdVideoPicture);
  virtual void SetDropState(bool bDrop);
  virtual const char* GetName(void) { return (const char*)m_pFormatName; }

  void RenderFrame(void);
protected:

  CDVDStreamInfo      m_hints;
  DVDVideoPicture     m_picture;
  const char         *m_pFormatName;
  int                 m_nframes;
  int                 m_displayedFrames;
  
  /* FIXME pure VPU  stuff : TO be moved in a dedicated class ? */
  bool VpuOpen(void);
  bool VpuAllocBuffers(VpuMemInfo *);
  bool VpuFreeBuffers(void);


  VpuDecOpenParam     m_decOpenParam;
  DecMemInfo          m_decMemInfo;
  VpuDecHandle        m_vpuHandle;
  VpuDecInitInfo      m_initInfo;
  CDTSManager         m_ts;
  //void               *m_tsm;    // Timestamp manager
  bool                m_tsSyncRequired;
  /* FIXME V4L rendering stuff & Frame Buffers: To be moved in a dedicated class */
  bool VpuAllocFrameBuffers(void);
  bool VpuPushFrame(VpuFrameBuffer *);
  bool VpuDeQueueFrame(void);
  int GetAvailableBufferNb(void);
  void InitFB(void);
  void RestoreFB(void);
  
  static const int    m_extraVpuBuffers;
  static const char  *m_v4lDeviceName;
  
  struct v4l2_crop    m_crop;
  int                 m_xscreen, m_yscreen;
  int                 m_v4lfd;
  int                 m_vpuFrameBufferNum; // Total number of allocated frame buffers
  VpuFrameBuffer     *m_vpuFrameBuffers;   // Table of VPU frame buffers description
  struct v4l2_buffer *m_v4lBuffers;        // Table of V4L buffer info (as returned by VIDIOC_QUERYBUF)
  VpuFrameBuffer    **m_outputBuffers;     // Output buffer pointers from VPU (table index is V4L buffer index). Enable to call VPU_DecOutFrameDisplayed
  bool                m_streamon;          // Flag that indicates whether streaming in on (from V4L point of view)
  std::queue <struct v4l2_buffer*> m_outputFrames;   // Frames to be displayed  
  VpuMemDesc          m_extraMem;
  
  /* FIXME create a real class and share with openmax */
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


