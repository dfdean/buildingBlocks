/////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2005-2017 Dawson Dean
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
// CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
// TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
// SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
/////////////////////////////////////////////////////////////////////////////
// See the corresponding .cpp file for a description of this module.
/////////////////////////////////////////////////////////////////////////////

#ifndef _BLOCK_IO_H_
#define _BLOCK_IO_H_

#if LINUX
#include <netinet/in.h>
#include <aio.h>
#endif

class CIOBuffer;
class CAsyncBlockIO;
class CIOSystem;
class CAsyncIOStream;
class CNetIOSystem;
class CParsedUrl;


/////////////////////////////////////////////////////////////////////////////
// Clients of a BlockIO must implement this callback interface.
//
// There is one CAsyncBlockIOCallback for each CAsyncBlockIO, it spans all operations
// on that block IO. This is different than the normal asynchronous call
// pattern. Normally, you pass a different callback to each asynch operation.
// BlockIO objects, however, may invoke the callback at any time, not just
// in response to an asynch call. For example, a network socket may accept a
// new connection, or receive an incoming packet at any time. Rather than
// making devices like networks behave differently than other devices, I decided
// the cleanest way is to change the model for all block IO devices. So, you
// provide a callback when you open the block IO, and that callback will
// be invoked when any asynchronous operation on that block IO completes.
/////////////////////////////////////////////////////////////////////////////
class CAsyncBlockIOCallback : public CRefCountInterface {
public:
    virtual void OnBlockIOEvent(CIOBuffer *pBuffer) = 0;
    virtual void OnBlockIOOpen(ErrVal err, CAsyncBlockIO *pBlockIO) = 0;
    virtual void OnBlockIOAccept(ErrVal err, CAsyncBlockIO *pBlockIO) = 0;
}; // CAsyncBlockIOCallback






/////////////////////////////////////////////////////////////////////////////
// This allows us to do synchronous block IO.
/////////////////////////////////////////////////////////////////////////////
class CSynCAsyncBlockIOCallback : public CAsyncBlockIOCallback,
                                    public CRefCountImpl {
public:
    CSynCAsyncBlockIOCallback();
    virtual ~CSynCAsyncBlockIOCallback();
    NEWEX_IMPL()

    ErrVal Initialize();
    ErrVal Wait();

    // CAsyncBlockIOCallback
    virtual void OnBlockIOEvent(CIOBuffer *pBuffer);
    virtual void OnBlockIOOpen(ErrVal err, CAsyncBlockIO *pBlockIO);
    virtual void OnBlockIOAccept(ErrVal err, CAsyncBlockIO *pBlockIO) { err = err; pBlockIO = pBlockIO; return; }

    // CRefCountInterface
    PASS_REFCOUNT_TO_REFCOUNTIMPL()

    CAsyncBlockIO       *m_pBlockIO;

private:
    CRefEvent           *m_Semaphore;
    CRefLock            *m_pLock;
    ErrVal              m_TestErr;
}; // CSynCAsyncBlockIOCallback





/////////////////////////////////////////////////////////////////////////////
// This describes one block I/O operation.
/////////////////////////////////////////////////////////////////////////////
class CIOBuffer : public CRefCountImpl,
                  public CJob {
public:
    enum CIOBufferOp {
        NO_OP               = 0,
        READ                = 1,
        WRITE               = 2,
        IO_CONNECT          = 3,
        IO_ACCEPT           = 4,
    };

    enum {
        // These are the flags for a single block.
        VALID_DATA          = 0x01,
        ALLOCATED_BUFFER    = 0x02,
        INPUT_BUFFER        = 0x04,
        OUTPUT_BUFFER       = 0x08,
        DISCARD_WHEN_IDLE   = 0x10,
        UNSAVED_CHANGES     = 0x20,
    };

    int32                       m_BufferOp;
    int32                       m_BufferFlags;

    ErrVal                      m_Err;

    // This is the correctly aligned. The actual data will be
    // some subset of this. For example, we may ignore a packet header.
    char                        *m_pPhysicalBuffer;
    int32                       m_BufferSize;

    // This is the valid data. It is a subset of m_pPhysicalBuffer.
    char                        *m_pLogicalBuffer;
    int32                       m_NumValidBytes;

    // This is only used for seekable media.
    int64                       m_PosInMedia;

    // This is the CAsyncBlockIO that allocated this data block.
    // The m_pBlockIO may be NULL, but m_pIOSystem never should be.
    CIOSystem                   *m_pIOSystem;

    // This is the CAsyncBlockIO that is currently using this data block.
    // This is NULL if the block is not in the middle of a read/write.
    CAsyncBlockIO               *m_pBlockIO;

    // This lets a bunch of buffers be queued to read or write
    // a pipe-IO device.
    CQueueHook<CIOBuffer>       m_BlockIOBufferList;

    // This lets us hang this block on a list in the owner. This holds
    // free blocks in a IO system, and active blocks in a AsyncIOStream.
    CQueueHook<CIOBuffer>       m_StreamBufferList;

    // This lets us do several different non-blocking writes from one
    // buffer. For example, a UDP socket may not write an entire
    // buffer in one operation.
    int32                       m_StartWriteOffset;

    struct sockaddr_in          m_udpDatagramSource;

#if WIN32
    // This implements asynch file I/O on windows 32.
    OVERLAPPED                  m_NTOverlappedIOInfo;
    DWORD                       m_dwNumBytesTransferred;
#endif // WIN32

#if LINUX
    struct aiocb                m_AIORequest;
#endif

    CIOBuffer();
    ~CIOBuffer();
    NEWEX_IMPL()

    // CDebugObject
    virtual ErrVal CheckState();

    // CRefCountInterface
    PASS_REFCOUNT_TO_REFCOUNTIMPL()

    // CJob
    virtual void ProcessJob(CSimpleThread *pThreadState);
}; // CIOBuffer






/////////////////////////////////////////////////////////////////////////////
// This represents one IO store, like a single file or a network socket.
/////////////////////////////////////////////////////////////////////////////
class CAsyncBlockIO : public CRefCountImpl,
                        public CRefCountInterface {
public:
    enum CAsyncBlockIOConstants {
        // These are the media types.
        NO_MEDIA                        = 0,
        MEMORY_MEDIA                    = 1,
        FILE_MEDIA                      = 2,
        NETWORK_MEDIA                   = 3,

        // These are the options to open.
        // They are *also* stored as flags in a block IO.
        READ_ACCESS                     = 0x0001,
        WRITE_ACCESS                    = 0x0002,
        RESIZEABLE                      = 0x0004,
        CREATE_NEW_STORE                = 0x0008,
        USE_SYNCHRONOUS_IO              = 0x0010,

        // These are set internally.
        BLOCKIO_IS_OPEN                 = 0x1000,
        SENT_BLOCKIO_TO_JOBQUEUE        = 0x2000,
        THREAD_PROCESSING_JOB           = 0x4000,
    }; // CAsyncBlockIOConstants


    CAsyncBlockIO();
    virtual ~CAsyncBlockIO();
    NEWEX_IMPL()

    virtual void Close();
    virtual ErrVal Flush();

    virtual void CancelTimeout(int32 opType);
    virtual void StartTimeout(int32 opType);

    virtual void ReadBlockAsync(CIOBuffer *pBuffer);
    virtual void WriteBlockAsync(CIOBuffer *pBuffer, int32 startOffsetInBuffer);

    // Resize removes from the end. RemoveNBytes removes from the current position.
    virtual ErrVal Resize(int64 newLength) = 0;
    ErrVal RemoveNBytes(int64 numBytes);

    // Various accessor functions.
    int64 GetMediaSize() { return(m_MediaSize); }
    CIOSystem *GetIOSystem() { return(m_pIOSystem); }
    CRefLock *GetLock();

    void ChangeBlockIOCallback(CAsyncBlockIOCallback *pCallback);
    CAsyncBlockIOCallback *GetBlockIOCallback();
    void ProcessAllCompletedJobs();

    // This is public so async worker threads can call it.
    void FinishIO(
            CIOBuffer *pBuffer,
            ErrVal err,
            int32 bytesDone);

    // CDebugObject
    virtual ErrVal CheckState();

    // CRefCountInterface
    PASS_REFCOUNT_TO_REFCOUNTIMPL()

protected:
    friend class CIOSystem;
    friend class CNetIOSystem;
    friend class CAsyncIOStream;

    ErrVal SendIOEvent(int32 IOEvent, ErrVal err);

    virtual void ReadBlockAsyncImpl(CIOBuffer *pBuffer) = 0;
    virtual void WriteBlockAsyncImpl(CIOBuffer *pBuffer) = 0;

    int8                        m_MediaType;
    bool                        m_fSeekable;
    bool                        m_fSynchronousDevice;

    CParsedUrl                  *m_pUrl;

    int32                       m_BlockIOFlags;
    int64                       m_MediaSize;

    int16                       m_NumActiveReads;
    int16                       m_NumActiveWrites;

    // This links active block IOs together.
    CQueueHook<CAsyncBlockIO>   m_ActiveBlockIOs;

    CRefLock                    *m_pLock;

    // This allows us to process completed reads and writes in
    // the order that they actually occurr. This is important
    // for a PIPE blockIO, because it sends us unsolicited read
    // events and those events (sent to a job queue) do not preserve
    // order. This also allows a single transition to a worker thread to
    // process several completed operations.
    CQueueList<CIOBuffer>       m_CompletedBuffers;

    CAsyncBlockIOCallback       *m_pCallback;

    CIOSystem                   *m_pIOSystem;
}; // CAsyncBlockIO.






/////////////////////////////////////////////////////////////////////////////
// This describes a file system, a network controller, or other I/O system.
/////////////////////////////////////////////////////////////////////////////
class CIOSystem : public CDebugObject
{
public:
    CIOSystem();
    virtual ~CIOSystem();
    NEWEX_IMPL()

    static int32 GetTotalActiveIOJobs();
    static void ReleaseBlockList(CIOBuffer *freeOp);

#if INCLUDE_REGRESSION_TESTS
    static void TestBlockIO();
#endif

    ErrVal InitIOSystem();
    virtual ErrVal Shutdown();

    virtual ErrVal OpenBlockIO(
                        CParsedUrl *pUrl,
                        int32 options,
                        CAsyncBlockIOCallback *pCallback) = 0;

    // The size parameters for blockIO for each class of device.
    virtual int32 GetDefaultBytesPerBlock() { return(2048); }
    virtual int64 GetIOStartPosition(int64 pos) { return(pos); }
    virtual int32 GetBlockBufferAlignment() { return(0); }

    CIOBuffer *AllocIOBuffer(int32 bufferSize, bool allocBuffer);

protected:
    friend class CAsyncBlockIO;

    bool                        m_fInitialized;
    CRefLock                    *m_pLock;

    CQueueList<CAsyncBlockIO>   m_ActiveBlockIOs;
}; // CIOSystem.

extern CIOSystem *g_pMemoryIOSystem;
extern CIOSystem *g_pFileIOSystem;
extern CIOSystem *g_pNetIOSystem;

ErrVal InitializeMemoryBlockIO();
ErrVal InitializeFileBlockIO();
ErrVal InitializeNetBlockIO();

bool NetIO_GetLocalProxySettings(char **ppProxyServerName, int *pProxyPort);
ErrVal NetIO_LookupHost(char *name, uint16 portNum, struct sockaddr_in *addr);
void NetIO_WaitForAllBlockIOsToClose(); // This is just for leak checking.

#endif // _BLOCK_IO_H_

