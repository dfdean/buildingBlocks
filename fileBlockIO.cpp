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
//
// File Block I/O
//
// This implements a block-device interface for local files.
//
// This implements IO with either synchronous or asynchronous operations.
/////////////////////////////////////////////////////////////////////////////

#if LINUX
#include <aio.h>
#include <signal.h>
#include <unistd.h>
#define USE_LINUX_AIO  0
#endif // LINUX

#include "osIndependantLayer.h"
#include "config.h"
#include "log.h"
#include "debugging.h"
#include "memAlloc.h"
#include "refCount.h"
#include "threads.h"
#include "stringLib.h"
#include "stringParse.h"
#include "queue.h"
#include "jobQueue.h"
#include "rbTree.h"
#include "url.h"
#include "blockIO.h"


FILE_DEBUGGING_GLOBALS(LOG_LEVEL_DEFAULT, 0);




#if WIN32
static HANDLE g_hIOCompletionPort = INVALID_HANDLE_VALUE;
DWORD WINAPI Win32FileIOCompletionProc(LPVOID pvwp);
static int32 g_OffsetFromAsyncIOInfoToIOBlock;
#endif // WIN32

extern int32 g_NumFileCallbacksActive;


#if USE_LINUX_AIO
static void LinuxSignalHandler(int sig, siginfo_t *info, void *unused);
static void LinuxWorkerThreadProc(void *arg, CSimpleThread *threadState);
static int32 g_OffsetFromAsyncIOInfoToIOBlock;
#endif

// SIGRTMIN through SIGRTMAX are signals sent to computer programs for user-defined purposes.
#define SIG_AIO_IO_COMPLETE (SIGRTMIN)




/////////////////////////////////////////////////////////////////////////////
class CFileBlockIO : public CAsyncBlockIO {
public:
    CFileBlockIO();
    virtual ~CFileBlockIO();
    NEWEX_IMPL()

    // CAsyncBlockIO
    virtual void Close();
    virtual ErrVal Flush();
    virtual ErrVal Resize(int64 newLength);

    // CDebugObject
    virtual ErrVal CheckState();

protected:
    friend class CFileIOSystem;

    // CAsyncBlockIO
    virtual void ReadBlockAsyncImpl(CIOBuffer *pBuffer);
    virtual void WriteBlockAsyncImpl(CIOBuffer *pBuffer);

#if WIN32
    HANDLE        m_AsynchFileHandle;
#elif LINUX
    int           m_AsynchFileFD;
#endif // WIN32

    // This is used only for synchronous IO. It is used for systems that do not
    // support asynch IO, or when we explicitly request synch IO.
    CSimpleFile   m_SynchFile;
}; // CFileBlockIO



/////////////////////////////////////////////////////////////////////////////
// This is the interface to the file IO system.
class CFileIOSystem : public CIOSystem {
public:
    CFileIOSystem();
    ~CFileIOSystem() { }
    NEWEX_IMPL()

    // CIOSystem
    virtual ErrVal Shutdown();
    virtual ErrVal OpenBlockIO(
                        CParsedUrl *pUrl,
                        int32 options,
                        CAsyncBlockIOCallback *pCallback);
    virtual int32 GetDefaultBytesPerBlock() { return(BYTES_PER_FILE_BLOCK); }
    virtual int64 GetIOStartPosition(int64 pos) {return(pos & (~(BYTES_PER_FILE_BLOCK - 1)));}
    virtual int32 GetBlockBufferAlignment() { return(BYTES_PER_FILE_BLOCK); }

    // CDebugObject
    virtual ErrVal CheckState();

#if USE_LINUX_AIO
    void WakeWorkerThread(CIOBuffer *pIOBuffer);
    void RunWorkerThread();
#endif

private:
    friend class CFileBlockIO;

    ErrVal InitFileIOSystem();

#if WIN32
    HANDLE          m_hIOCompletionThread;
#endif

#if USE_LINUX_AIO
    bool            m_fShutdown;

    CQueueList<CIOBuffer> m_FinishedIOList;
    CRefEvent       *m_pIOEventSignal;
    CSimpleThread   *m_pWorkerThread;
#endif

    enum CFileIOSystemPrivateConstants  {
        BYTES_PER_FILE_BLOCK        = 4096,
        OFFSET_INTO_FILE_BLOCK_MASK = BYTES_PER_FILE_BLOCK - 1,
        START_BLOCK_MASK            = ~OFFSET_INTO_FILE_BLOCK_MASK
    };
}; // CFileIOSystem

static CFileIOSystem *g_pFileIOSystemImpl = NULL;



/////////////////////////////////////////////////////////////////////////////
//
// [InitializeFileBlockIO]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
InitializeFileBlockIO() {
    g_pFileIOSystemImpl = newex CFileIOSystem;
    if (NULL == g_pFileIOSystemImpl) {
        returnErr(EFail);
    }

    g_pFileIOSystem = g_pFileIOSystemImpl;
    returnErr(ENoErr);
} // InitializeFileBlockIO.




 
/////////////////////////////////////////////////////////////////////////////
//
// [CFileBlockIO]
//
/////////////////////////////////////////////////////////////////////////////
CFileBlockIO::CFileBlockIO() {
    m_MediaType = FILE_MEDIA;
    m_fSeekable = true;

#if WIN32
    m_AsynchFileHandle = INVALID_HANDLE_VALUE;
#elif USE_LINUX_AIO
    m_AsynchFileFD = -1;
#endif // WIN32
} // CFileBlockIO





/////////////////////////////////////////////////////////////////////////////
//
// [~CFileBlockIO]
//
/////////////////////////////////////////////////////////////////////////////
CFileBlockIO::~CFileBlockIO() {
#if WIN32
    if (INVALID_HANDLE_VALUE != m_AsynchFileHandle) {
        BOOL fSuccess;

        fSuccess = CloseHandle(m_AsynchFileHandle);
        if (!fSuccess) {
            int reason = GetLastError();
            fSuccess = fSuccess;
        }

        m_AsynchFileHandle = INVALID_HANDLE_VALUE;
    }
#elif USE_LINUX_AIO
    if (-1 != m_AsynchFileFD) {
        close(m_AsynchFileFD);
        m_AsynchFileFD = -1;
    }
#endif
} // ~CFileBlockIO.






/////////////////////////////////////////////////////////////////////////////
//
// [Resize]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CFileBlockIO::Resize(int64 newLength) {
    ErrVal err = ENoErr;
#if WIN32
    BOOL fSuccess = FALSE;
    DWORD dwResult = 0;
    LARGE_INTEGER largeInt;
#endif // WIN32
    AutoLock(m_pLock);
    RunChecks();

    DEBUG_LOG("CFileBlockIO::Resize: old size = %d, new size = %d",
                m_MediaSize, newLength);

    if (newLength < 0) {
        gotoErr(EFail);
    }

    // Handle the case of a synchronous file specially.
    if (m_fSynchronousDevice) {
        err = m_SynchFile.SetFileLength(newLength);
        DEBUG_LOG("CFileBlockIO::Resize. Call m_SynchFile.SetFileLength. err = %d", err);
        if (err) {
            gotoErr(err);
        }
    } else {
#if WIN32
        if (INVALID_HANDLE_VALUE == m_AsynchFileHandle) {
            gotoErr(EFail);
        }

        largeInt.QuadPart = newLength;
        dwResult = SetFilePointer(
                        m_AsynchFileHandle, // handle of file
                        largeInt.LowPart,  // number of bytes to move file pointer
                        &largeInt.HighPart, // pointer to high-order word of distance
                        CSimpleFile::SEEK_START);
        if (INVALID_SET_FILE_POINTER == dwResult) {
            DWORD dwErr = GetLastError();
            DEBUG_LOG("CFileBlockIO::Resize. SetFilePointer failed. GetLastError = %d", dwErr);
            gotoErr(TranslateWin32ErrorIntoErrVal(dwErr, true));
        }

        fSuccess = SetEndOfFile(m_AsynchFileHandle);
        if (!fSuccess) {
            DWORD dwErr = GetLastError();
            DEBUG_LOG("CFileBlockIO::Resize. SetEndOfFile failed. GetLastError = %d", dwErr);
            gotoErr(TranslateWin32ErrorIntoErrVal(dwErr, true));
        }
#elif USE_LINUX_AIO
        if (-1 == m_AsynchFileFD) {
            gotoErr(EFail);
        }

        // Use truncate if you want to pass in a pathname instead.
        int dwResult = ftruncate(m_AsynchFileFD, newLength);
        if (dwResult < 0) {
            DEBUG_LOG("CFileBlockIO::Resize. SetEndOfFile failed. dwResult = %d, errno = %d",
                        dwResult, errno);
            gotoErr(EFail);
        }
#endif
    } // Asynch IO

    m_MediaSize = newLength;

abort:
    returnErr(err);
} // Resize.





/////////////////////////////////////////////////////////////////////////////
//
// [Close]
//
/////////////////////////////////////////////////////////////////////////////
void
CFileBlockIO::Close() {
#if WIN32
    BOOL fSuccess;
#endif
    AutoLock(m_pLock);
    RunChecksOnce();

    DEBUG_LOG("CFileBlockIO::Close");

    // Close in the base class.
    CAsyncBlockIO::Close();
    m_SynchFile.Close();

#if WIN32
    if (INVALID_HANDLE_VALUE != m_AsynchFileHandle) {
        fSuccess = CloseHandle(m_AsynchFileHandle);
        if (!fSuccess) {
            int reason = GetLastError();
            DEBUG_LOG("CFileBlockIO::Close. CloseHandle failed. GetLastError = %d", reason);
            fSuccess = fSuccess;
        }
        m_AsynchFileHandle = INVALID_HANDLE_VALUE;
    }
#elif USE_LINUX_AIO
    if (-1 != m_AsynchFileFD) {
        close(m_AsynchFileFD);
        m_AsynchFileFD = -1;
    }
#endif
} // Close






/////////////////////////////////////////////////////////////////////////////
//
// [Flush]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CFileBlockIO::Flush() {
    ErrVal err = ENoErr;
    AutoLock(m_pLock);
    RunChecks();

    DEBUG_LOG("CFileBlockIO::Flush");

#if WIN32
    if (INVALID_HANDLE_VALUE == m_AsynchFileHandle) {
        DEBUG_WARNING("File Block IO flushing a NULL handle.");
        gotoErr(EFail);
    }
    // This seems to confuse synchronous writes in WinNT.
    // fSuccess = FlushFileBuffers(m_AsynchFileHandle);
abort:
#elif USE_LINUX_AIO
    if (-1 == m_AsynchFileFD) {
        DEBUG_WARNING("File Block IO flushing a NULL handle.");
        gotoErr(EFail);
    }

    {
        int result = fsync(m_AsynchFileFD);
        if (result < 0) {
            DEBUG_LOG("CFileBlockIO::Flush. fsync failed. result = %d", result);
            err = EFail;
        }
    }
abort:
#endif

    returnErr(err);
} // Flush.





/////////////////////////////////////////////////////////////////////////////
//
// [CheckState]
//
// This does not check checksums, because we do not know
// if the client is currently modifying a buffer (and hence
// the checksum would be invalid). Checksums must be explicitly
// called by the client.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CFileBlockIO::CheckState() {
    ErrVal err = ENoErr;
    AutoLock(m_pLock);

    err = CAsyncBlockIO::CheckState();
    if (err) {
        gotoErr(EFail);
    }

    if ((FILE_MEDIA != m_MediaType) || (!m_fSeekable)) {
        gotoErr(EFail);
    }

abort:
    returnErr(err);
} // CheckState.






/////////////////////////////////////////////////////////////////////////////
//
// [WriteBlockAsyncImpl]
//
// This method has to be overloaded if a subclass really implements
// asynchronous I/O
/////////////////////////////////////////////////////////////////////////////
void
CFileBlockIO::WriteBlockAsyncImpl(CIOBuffer *pBuffer) {
    ErrVal err = ENoErr;
    int32 synchBytesWritten = 0;
#if WIN32
    BOOL fSuccess = FALSE;
    LARGE_INTEGER largeInt;
#endif // WIN32
    RunChecks();


    if (NULL == pBuffer) {
        gotoErr(EFail);
    }
    ADDREF_OBJECT(pBuffer);

    DEBUG_LOG("CFileBlockIO::WriteBlockAsyncImpl.");

    //////////////////////////////////////////
    {
        AutoLock(m_pLock);

        // If the requested IO is too big, then we either expand the
        // file or clip the write.
        if (((pBuffer->m_PosInMedia + pBuffer->m_NumValidBytes) > m_MediaSize)
            &&  !(m_BlockIOFlags & RESIZEABLE)) {
            DEBUG_LOG("CFileBlockIO::WriteBlockAsyncImpl. Failed because buffer exceeds the max length of a fixed-size file.");
            gotoErr(EFail);
        } // handling a too-big IO.
    }
    //////////////////////////////////////////

    // Handle the case of a synchronous file specially.
    if (m_fSynchronousDevice) {
        DEBUG_LOG("CFileBlockIO::WriteBlockAsyncImpl. Synchronous write.");
        DEBUG_LOG("CFileBlockIO::WriteBlockAsyncImpl. pBuffer->m_PosInMedia = " ERRFMT, pBuffer->m_PosInMedia);
        DEBUG_LOG("CFileBlockIO::WriteBlockAsyncImpl. pBuffer->m_NumValidBytes = %d", pBuffer->m_NumValidBytes);

        err = m_SynchFile.Seek(pBuffer->m_PosInMedia);
        if (err) {
            DEBUG_LOG("CFileBlockIO::WriteBlockAsyncImpl. Synchronous write seek failed.");
            gotoErr(err);
        }
        err = m_SynchFile.Write(pBuffer->m_pLogicalBuffer, pBuffer->m_NumValidBytes);
        if (err) {
            DEBUG_LOG("CFileBlockIO::WriteBlockAsyncImpl. Synchronous write failed.");
            gotoErr(err);
        }
        synchBytesWritten = pBuffer->m_NumValidBytes;
    } else
    {
#if WIN32
        largeInt.QuadPart = pBuffer->m_PosInMedia;
        pBuffer->m_NTOverlappedIOInfo.Internal = 0;
        pBuffer->m_NTOverlappedIOInfo.InternalHigh = 0;
        pBuffer->m_NTOverlappedIOInfo.Offset = largeInt.LowPart;
        pBuffer->m_NTOverlappedIOInfo.OffsetHigh = largeInt.HighPart;
        pBuffer->m_NTOverlappedIOInfo.hEvent = NULL;

        fSuccess = WriteFile(
                        m_AsynchFileHandle,
                        pBuffer->m_pLogicalBuffer,
                        pBuffer->m_NumValidBytes,
                        &(pBuffer->m_dwNumBytesTransferred),
                        &(pBuffer->m_NTOverlappedIOInfo));
        if (!fSuccess) {
            DWORD dwError = GetLastError();
            DEBUG_LOG("CFileBlockIO::WriteBlockAsyncImpl. WriteFile failed. dwError = %d", dwError);
            if (ERROR_IO_PENDING == dwError) {
                // Ignore this; it is not a real error.
            }
            else {
                gotoErr(TranslateWin32ErrorIntoErrVal(dwError, true));
            }
        }
#elif USE_LINUX_AIO
        memset(&(pBuffer->m_AIORequest), '\0', sizeof(struct aiocb));
        pBuffer->m_AIORequest.aio_fildes = m_AsynchFileFD;
        pBuffer->m_AIORequest.aio_buf = pBuffer->m_pLogicalBuffer;
        pBuffer->m_AIORequest.aio_nbytes = pBuffer->m_NumValidBytes;
        pBuffer->m_AIORequest.aio_offset = pBuffer->m_PosInMedia;
        pBuffer->m_AIORequest.aio_sigevent.sigev_notify = SIGEV_SIGNAL;
        pBuffer->m_AIORequest.aio_sigevent.sigev_signo = SIG_AIO_IO_COMPLETE;
        pBuffer->m_AIORequest.aio_sigevent.sigev_value.sival_ptr = (void *) &(pBuffer->m_AIORequest);
        aio_write(&(pBuffer->m_AIORequest));
#endif
    } // Asynch case

    if (m_pLock) {
        m_pLock->Lock();
    }

    if ((pBuffer->m_PosInMedia + pBuffer->m_NumValidBytes) > m_MediaSize) {
        m_MediaSize = pBuffer->m_PosInMedia + pBuffer->m_NumValidBytes;
    }

    if (m_pLock) {
        m_pLock->Unlock();
    }

abort:
    DEBUG_LOG("CFileBlockIO::WriteBlockAsyncImpl finished.");

    // If this was synchronous, then report that we are done.
    if ((m_fSynchronousDevice) || (err)) {
        FinishIO(pBuffer, err, synchBytesWritten);
        RELEASE_OBJECT(pBuffer);
    }
} // WriteBlockAsyncImpl.






/////////////////////////////////////////////////////////////////////////////
//
// [ReadBlockAsyncImpl]
//
// This method has to be overloaded if a subclass really implements
// asynchronous I/O
/////////////////////////////////////////////////////////////////////////////
void
CFileBlockIO::ReadBlockAsyncImpl(CIOBuffer *pBuffer) {
    ErrVal err = ENoErr;
    int32 actualIOSize = 0;
    int32 synchBytesRead = 0;
#if WIN32
    BOOL fSuccess = FALSE;
    LARGE_INTEGER largeInt;
#endif // WIN32
    RunChecks();

    if (NULL == pBuffer) {
        return;
    }
    ADDREF_OBJECT(pBuffer);

    DEBUG_LOG("CFileBlockIO::ReadBlockAsyncImpl()");

    { ///////////////////////////////////////////////////
        AutoLock(m_pLock);

        // If the requested IO is too big, then we clip it.
        actualIOSize = pBuffer->m_BufferSize;
        if ((pBuffer->m_PosInMedia + actualIOSize) > m_MediaSize) {
            DEBUG_LOG("CFileBlockIO::ReadBlockAsyncImpl. Clipping IO to file size.");
            DEBUG_LOG("CFileBlockIO::ReadBlockAsyncImpl. pBuffer->m_PosInMedia = " ERRFMT, pBuffer->m_PosInMedia);
            DEBUG_LOG("CFileBlockIO::ReadBlockAsyncImpl. m_MediaSize = " ERRFMT, m_MediaSize);

            actualIOSize = (int32) (m_MediaSize - pBuffer->m_PosInMedia);
            if (actualIOSize <= 0) {
                gotoErr(EEOF);
            }

            DEBUG_LOG("CFileBlockIO::ReadBlockAsyncImpl. actualIOSize = %d", actualIOSize);
        } // handling a too-big I/O.
    } ///////////////////////////////////////////////////

    // Handle the case of a synchronous file specially.
    if (m_fSynchronousDevice) {
        DEBUG_LOG("CFileBlockIO::ReadBlockAsyncImpl. Synchronous write.");
        DEBUG_LOG("CFileBlockIO::ReadBlockAsyncImpl. pBuffer->m_PosInMedia = " ERRFMT, pBuffer->m_PosInMedia);
        DEBUG_LOG("CFileBlockIO::ReadBlockAsyncImpl. pBuffer->m_NumValidBytes = %d", pBuffer->m_NumValidBytes);

        err = m_SynchFile.Seek(pBuffer->m_PosInMedia);
        if (err) {
            DEBUG_LOG("CFileBlockIO::ReadBlockAsyncImpl. Synchronous seek failed.");
            gotoErr(err);
        }

        err = m_SynchFile.Read(
                            pBuffer->m_pLogicalBuffer,
                            actualIOSize,
                            &synchBytesRead);
        if (err) {
            DEBUG_LOG("CFileBlockIO::ReadBlockAsyncImpl. Read seek failed.");
            gotoErr(err);
        }
    } else
    {
#if WIN32
        largeInt.QuadPart = pBuffer->m_PosInMedia;
        pBuffer->m_NTOverlappedIOInfo.Internal = 0;
        pBuffer->m_NTOverlappedIOInfo.InternalHigh = 0;
        pBuffer->m_NTOverlappedIOInfo.Offset = largeInt.LowPart;
        pBuffer->m_NTOverlappedIOInfo.OffsetHigh = largeInt.HighPart;
        pBuffer->m_NTOverlappedIOInfo.hEvent = NULL;

        fSuccess = ReadFile(
                        m_AsynchFileHandle,
                        pBuffer->m_pLogicalBuffer,
                        actualIOSize,
                        &(pBuffer->m_dwNumBytesTransferred),
                        &(pBuffer->m_NTOverlappedIOInfo));
        if (!fSuccess) {
            DWORD dwError = GetLastError();
            DEBUG_LOG("CFileBlockIO::ReadBlockAsyncImpl. ReadFile failed. dwError = %d", dwError);
            if (ERROR_HANDLE_EOF == dwError) {
                gotoErr(EEOF);
            }
            else if (ERROR_IO_PENDING == dwError) {
                // Ignore this; it is not a real error.
            }
            else {
                gotoErr(TranslateWin32ErrorIntoErrVal(dwError, true));
            }
        }
#elif USE_LINUX_AIO
        memset(&(pBuffer->m_AIORequest), '\0', sizeof(struct aiocb));
        pBuffer->m_AIORequest.aio_fildes = m_AsynchFileFD;
        pBuffer->m_AIORequest.aio_buf = pBuffer->m_pLogicalBuffer;
        pBuffer->m_AIORequest.aio_nbytes = pBuffer->m_NumValidBytes;
        pBuffer->m_AIORequest.aio_offset = pBuffer->m_PosInMedia;
        pBuffer->m_AIORequest.aio_sigevent.sigev_notify = SIGEV_SIGNAL;
        pBuffer->m_AIORequest.aio_sigevent.sigev_signo = SIG_AIO_IO_COMPLETE;
        pBuffer->m_AIORequest.aio_sigevent.sigev_value.sival_ptr = (void *) &(pBuffer->m_AIORequest);
        aio_read(&(pBuffer->m_AIORequest));
#endif
    } // Asynch

abort:
    DEBUG_LOG("CFileBlockIO::ReadBlockAsyncImpl finished.");

    // If this was synchronous, then report that we are done.
    if ((m_fSynchronousDevice) || (err)) {
        FinishIO(pBuffer, err, synchBytesRead);
        RELEASE_OBJECT(pBuffer);
    }
} // ReadBlockAsyncImpl.






/////////////////////////////////////////////////////////////////////////////
//
// [CFileIOSystem]
//
/////////////////////////////////////////////////////////////////////////////
CFileIOSystem::CFileIOSystem() {
#if WIN32
    m_hIOCompletionThread = NULL;
#elif USE_LINUX_AIO
    m_fShutdown = false;
    m_pIOEventSignal = NULL;
    m_pWorkerThread = NULL;
#endif
} // CFileIOSystem.






/////////////////////////////////////////////////////////////////////////////
//
// [InitFileIOSystem]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CFileIOSystem::InitFileIOSystem() {
    ErrVal err = ENoErr;
    CIOBuffer *tempBuffer = NULL;

    err = CIOSystem::InitIOSystem();
    if (err) {
        gotoErr(err);
    }

    tempBuffer = newex CIOBuffer;
    if (NULL == tempBuffer) {
        gotoErr(EFail);
    }


#if WIN32
    //////////////////////////////////////////////////////////////////////
    DWORD tid;

    // Compute the offset between a buffer and the Overlapped IO object. We will
    // pass in OVERLAPPED objects that are actually a field within the CIOBuffer
    // struct. When Windows calls our callback, it gives us a pointer to the OVERLAPPED.
    // This offset value will allow us to use that OVERLAPPED pointer to find the
    // beginning of the CIOBuffer.
    g_OffsetFromAsyncIOInfoToIOBlock
        = ((char *) &(tempBuffer->m_NTOverlappedIOInfo)) - ((char *) tempBuffer);

    // Create a new IOCompletion port.
    g_hIOCompletionPort = CreateIoCompletionPort(
                                INVALID_HANDLE_VALUE, // file handle to associate with the I/O completion port
                                NULL, // handle to the I/O completion port
                                0, // per-file completion key for I/O completion packets
                                1); // number of threads allowed to  execute concurrently
    if (NULL == g_hIOCompletionPort) {
        gotoErr(EFail);
    }

    // Start up a thread to service the IOCompletion port.
    m_hIOCompletionThread = CreateThread(
                                NULL,
                                0,
                                Win32FileIOCompletionProc,
                                (LPVOID) this,
                                0,
                                &tid);
    if (NULL == m_hIOCompletionThread) {
        gotoErr(EFail);
    }
#elif USE_LINUX_AIO
    //////////////////////////////////////////////////////////////////////
    struct sigaction sigActionInfo;
    int result;

    // Compute the offset between a buffer and the Overlapped IO object.
    g_OffsetFromAsyncIOInfoToIOBlock
        = ((char *) &(tempBuffer->m_AIORequest)) - ((char *) tempBuffer);

    m_fShutdown = false;

    m_pIOEventSignal = newex CRefEvent;
    if (NULL == m_pIOEventSignal) {
        gotoErr(EFail);
    }
    err = m_pIOEventSignal->Initialize();
    if (err) {
        gotoErr(err);
    }

    // Register a function to be called when an IO signal is sent to
    // this process. That will then signal a thread so we will transfer
    // control to a know thread, not whatever caught the signal.
    memset(&sigActionInfo, '\0', sizeof(struct sigaction));
    sigActionInfo.sa_sigaction = LinuxSignalHandler;
    sigActionInfo.sa_flags = SA_RESTART | SA_SIGINFO;
    //sigActionInfo.sa_flags = SA_SIGINFO;
    sigemptyset(&sigActionInfo.sa_mask);
    sigaddset(&sigActionInfo.sa_mask, SIG_AIO_IO_COMPLETE);
    result = sigaction(SIG_AIO_IO_COMPLETE, &sigActionInfo, NULL);
    if (result <= -1) {
        gotoErr(EFail);
    }

    err = CSimpleThread::CreateThread(
                              "fileIO",
                              &LinuxWorkerThreadProc,
                              NULL,
                              &m_pWorkerThread);
    if (err) {
        gotoErr(err);
    }
#endif

abort:
    RELEASE_OBJECT(tempBuffer);

    returnErr(err);
} // InitFileIOSystem.








/////////////////////////////////////////////////////////////////////////////
//
// [Shutdown]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CFileIOSystem::Shutdown() {
    ErrVal err = ENoErr;

    // If we never lazily initialized the IO system, then there is nothing
    // to do.
    if (!m_fInitialized) {
        returnErr(ENoErr);
    }

    RunChecks();

    err = CIOSystem::Shutdown();
    if (err) {
        gotoErr(err);
    }

#if WIN32
    if (INVALID_HANDLE_VALUE != g_hIOCompletionPort) {
        // Tell the worker thread to stop.
        (void) ::PostQueuedCompletionStatus(
                        g_hIOCompletionPort, // handle to an I/O completion port
                        0, // value to return via GetQueuedCompletionStatus' lpNumberOfBytesTranferred
                        0xFFFFFFFF, // value to return via GetQueuedCompletionStatus' lpCompletionKey
                        NULL); // value to return via GetQueuedCompletionStatus' lpOverlapped

        if (NULL != m_hIOCompletionThread) {
            // Wait for the worker thread to stop
            (void) WaitForSingleObject(m_hIOCompletionThread, INFINITE);
            (void) CloseHandle(m_hIOCompletionThread);
        }

        (void) CloseHandle(g_hIOCompletionPort);
        g_hIOCompletionPort = INVALID_HANDLE_VALUE;
    }
#endif // WIN32

#if USE_LINUX_AIO
    m_fShutdown = true;
    if (NULL != m_pWorkerThread) {
       WakeWorkerThread(NULL);
       m_pWorkerThread->WaitForThreadToStop();

       delete m_pWorkerThread;
       m_pWorkerThread = NULL;
    }

    delete m_pIOEventSignal;
    m_pIOEventSignal = NULL;
#endif // USE_LINUX_AIO


abort:
    returnErr(err);
} // Shutdown.







/////////////////////////////////////////////////////////////////////////////
//
// [OpenBlockIO]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CFileIOSystem::OpenBlockIO(
                    CParsedUrl *pUrl,
                    int32 options,
                    CAsyncBlockIOCallback *pCallback) {
    ErrVal err = ENoErr;
    CFileBlockIO *pBlockIO = NULL;
    char cSaveChar = 0;
    char *pFilePtr = NULL;
    char *pEndFilePtr = NULL;
#if WIN32
    HANDLE fh = INVALID_HANDLE_VALUE;
    HANDLE hPort = NULL;
    DWORD dwCreationDisposition;
    int32 dwOpenOptions = 0;
    int32 accessRights = 0;
    int32 shareOptions = 0;
    LARGE_INTEGER largeInt;
    bool fileIsOpen = false;
    BOOL fSuccess = FALSE;
#endif // WIN32
    bool fHoldingLock = false;
    RunChecks();


    if ((NULL == pUrl)
        || (CParsedUrl::URL_SCHEME_FILE != pUrl->m_Scheme)
        || (NULL == pUrl->m_pPath)
        || (0 == pUrl->m_PathSize)) {
        gotoErr(EFail);
    }

    // Lazily initialize the IO system when we open the first blockIO.
    if (!m_fInitialized) {
        err = InitFileIOSystem();
        if (err) {
            gotoErr(err);
        }
    }

    if (m_pLock) {
        m_pLock->Lock();
        fHoldingLock = true;
    }

#if LINUX && !USE_LINUX_AIO
   options |= CAsyncBlockIO::USE_SYNCHRONOUS_IO;
#endif

    pBlockIO = newex CFileBlockIO;
    if (!pBlockIO) {
        gotoErr(EFail);
    }


    // Handle the case of a synchronous file specially.
    if (options & CAsyncBlockIO::USE_SYNCHRONOUS_IO) {
        pFilePtr = pUrl->m_pPath;
        pEndFilePtr = pFilePtr + pUrl->m_PathSize;
        cSaveChar = *pEndFilePtr;
        *pEndFilePtr = 0;

        if (options & CAsyncBlockIO::CREATE_NEW_STORE) {
            err = pBlockIO->m_SynchFile.OpenOrCreateEmptyFile(pFilePtr, 0);
            DEBUG_LOG("CFileBlockIO::OpenBlockIO. OpenOrCreateEmptyFile for synchronous IO. Path = %s, err = %d",
                        pFilePtr, err);
        } else {
            err = pBlockIO->m_SynchFile.OpenExistingFile(pFilePtr, 0);
            DEBUG_LOG("CFileBlockIO::OpenBlockIO. OpenExistingFile for synchronous IO. Path = %s, err = %d",
                        pFilePtr, err);
        }
        *pEndFilePtr = cSaveChar;
        if (err) {
            DEBUG_WARNING("CFileBlockIO::OpenBlockIO error. Path = %s, err = %d", pFilePtr, err);
            gotoErr(err);
        }

        err = pBlockIO->m_SynchFile.GetFileLength((uint64 *) &(pBlockIO->m_MediaSize));
        if (err) {
            gotoErr(err);
        }
    } else { // if (!(options & CAsyncBlockIO::USE_SYNCHRONOUS_IO))
#if WIN32
        dwOpenOptions = FILE_FLAG_OVERLAPPED;
        dwOpenOptions |= FILE_FLAG_WRITE_THROUGH;
        if (options & CAsyncBlockIO::CREATE_NEW_STORE) {
            dwCreationDisposition = OPEN_ALWAYS;
        } else {
            dwCreationDisposition = OPEN_EXISTING;
        }
        if (options & CAsyncBlockIO::READ_ACCESS) {
            accessRights |= GENERIC_READ;
        }
        if (options & CAsyncBlockIO::WRITE_ACCESS) {
            accessRights |= GENERIC_WRITE;
        }
        shareOptions = FILE_SHARE_READ | FILE_SHARE_DELETE;


        pFilePtr = pUrl->m_pPath;
        pEndFilePtr = pFilePtr + pUrl->m_PathSize;
        cSaveChar = *pEndFilePtr;
        *pEndFilePtr = 0;

        fh = CreateFileA(
                    pFilePtr,
                    accessRights, // access (read-write) mode
                    shareOptions, // share mode
                    NULL, // pointer to security attributes
                    dwCreationDisposition,  // how to create
                    dwOpenOptions,  // file attributes
                    NULL);  // handle to file with attributes to copy

        DEBUG_LOG("CFileBlockIO::OpenBlockIO. OpenExistingFile for assynchronous IO. path = %s",
                    pFilePtr);
        *pEndFilePtr = cSaveChar;

        if (INVALID_HANDLE_VALUE == fh) {
            DWORD dwErr = GetLastError();
            DEBUG_LOG("CFileBlockIO::OpenBlockIO. Open failed. GetLastError = %d",
                        dwErr);
            gotoErr(EFileNotFound);
        }
        fileIsOpen = true;

        if (options & CAsyncBlockIO::CREATE_NEW_STORE) {
            DWORD dwResult;

            DEBUG_LOG("CFileBlockIO::OpenBlockIO. Truncating file");
            dwResult = SetFilePointer(
                            fh, // handle of file
                            0,  // number of bytes to move file pointer
                            NULL, // pointer to high-order word of distance
                            CSimpleFile::SEEK_START);
            if (0xFFFFFFFF == dwResult) {
                DWORD dwErr = GetLastError();
                DEBUG_LOG("CFileBlockIO::OpenBlockIO. Truncating file failed. GetLastError = %d", dwErr);
                gotoErr(TranslateWin32ErrorIntoErrVal(dwErr, true));
            }

            fSuccess = SetEndOfFile(fh);
            if (!fSuccess) {
                DWORD dwErr = GetLastError();
                DEBUG_LOG("CFileBlockIO::OpenBlockIO. Truncating file failed (step 2). GetLastError = %d", dwErr);
                gotoErr(TranslateWin32ErrorIntoErrVal(dwErr, true));
            }
        }

        hPort = CreateIoCompletionPort(
                                fh, // file handle to associate with the I/O completion port
                                g_hIOCompletionPort, // handle to the I/O completion port
                                0, // per-file completion key for I/O completion packets
                                1); // number of threads allowed to execute concurrently
        if (NULL == hPort) {
            gotoErr(EFail);
        }
        pBlockIO->m_AsynchFileHandle = fh;
        fh = INVALID_HANDLE_VALUE;

        fSuccess = GetFileSizeEx(pBlockIO->m_AsynchFileHandle, &largeInt);
        if (!fSuccess) {
            DWORD dwErr = GetLastError();
            gotoErr(TranslateWin32ErrorIntoErrVal(dwErr, true));
        }
        pBlockIO->m_MediaSize = largeInt.QuadPart;
#elif USE_LINUX_AIO
        CSimpleFile file;

        pFilePtr = pUrl->m_pPath;
        pEndFilePtr = pFilePtr + pUrl->m_PathSize;
        cSaveChar = *pEndFilePtr;
        *pEndFilePtr = 0;

        if (options & CAsyncBlockIO::CREATE_NEW_STORE) {
            err = file.OpenOrCreateEmptyFile(pFilePtr, 0);
        } else {
            err = file.OpenExistingFile(pFilePtr, 0);
        }
        *pEndFilePtr = cSaveChar;
        if (err) {
            gotoErr(err);
        }

        err = file.GetFileLength((uint64 *) &(pBlockIO->m_MediaSize));
        if (err) {
            gotoErr(err);
        }

        pBlockIO->m_AsynchFileFD = file.GetFD();
        file.ForgetFD();
#endif
    } // if (options & CAsyncBlockIO::USE_SYNCHRONOUS_IO)

    // These are part of the base class.
    pBlockIO->m_pIOSystem = this;
    pBlockIO->m_MediaType = CAsyncBlockIO::FILE_MEDIA;
    pBlockIO->m_fSeekable = true;

    pBlockIO->ChangeBlockIOCallback(pCallback);
    pBlockIO->m_ActiveBlockIOs.ResetQueue();

    pBlockIO->m_BlockIOFlags = CAsyncBlockIO::RESIZEABLE;
    if (options & CAsyncBlockIO::USE_SYNCHRONOUS_IO) {
        pBlockIO->m_fSynchronousDevice = true;
    }

    pBlockIO->m_pUrl = pUrl;
    ADDREF_OBJECT(pUrl);

    pBlockIO->m_pLock = CRefLock::Alloc();
    if (NULL == pBlockIO->m_pLock) {
        gotoErr(EFail);
    }

    // Add this connection to the list of active connections.
    // This assumes that we are holding the monitor lock.
    m_ActiveBlockIOs.InsertHead(&(pBlockIO->m_ActiveBlockIOs));
    ADDREF_OBJECT(pBlockIO);

    pBlockIO->m_BlockIOFlags |= CAsyncBlockIO::BLOCKIO_IS_OPEN;

    // Do not hold the lock when we call the callback. This
    // can cause a deadlock.
    if (fHoldingLock) {
        m_pLock->Unlock();
        fHoldingLock = false;
    }

    pCallback->OnBlockIOOpen(ENoErr, pBlockIO);

abort:
    if (fHoldingLock) {
        m_pLock->Unlock();
        fHoldingLock = false;
    }

    RELEASE_OBJECT(pBlockIO);

#if WIN32
    if (INVALID_HANDLE_VALUE != fh) {
        BOOL fSuccess = CloseHandle(fh);
        if (!fSuccess) {
            int reason = GetLastError();
            fSuccess = fSuccess;
        }

        fh = INVALID_HANDLE_VALUE;
    }
#endif // WIN32

    returnErr(err);
} // OpenBlockIO.





/////////////////////////////////////////////////////////////////////////////
//
// [CheckState]
//
// This does not check checksums, because we do not know
// if the client is currently modifying a buffer (and hence
// the checksum would be invalid). Checksums must be explicitly
// called by the client.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CFileIOSystem::CheckState() {
    AutoLock(m_pLock);
    returnErr(ENoErr);
} // CheckState.





#if WIN32
//////////////////////////////////////////////////////////////////////////////
//
// [Win32FileIOCompletionProc]
//
// WindowsNT worker thread that monitors an IOCompletion port. This is analogous
// to the select thread in the net block IO.
//////////////////////////////////////////////////////////////////////////////
DWORD WINAPI
Win32FileIOCompletionProc(LPVOID pvwp) {
    ErrVal err = ENoErr;
    DWORD cbTransferred = 0;
    DWORD dwKey = 0;
    BOOL fSuccess = TRUE;
    LPOVERLAPPED pOverlapped = NULL;
    CIOBuffer *pBuffer = NULL;
    char *pBasePtr = NULL;
    CAsyncBlockIO *pBlockIO = NULL;
    int32 osErr = 0;

    (void) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    // THREAD_PRIORITY_TIME_CRITICAL

    while (1) {
        pOverlapped = NULL;
        cbTransferred = 0;
        fSuccess = GetQueuedCompletionStatus(
                        g_hIOCompletionPort, // the I/O completion port of interest
                        &cbTransferred, // to receive number of bytes transferred during I/O
                        &dwKey, // to receive file's completion key
                        &pOverlapped, // to receive pointer to OVERLAPPED structure
                        INFINITE); // optional timeout value

        // This happens when we are closing the IOCompletion port.
        if ((!fSuccess) && (NULL == pOverlapped)) {
            break;
        }

        if (NULL != pOverlapped) {
            g_NumFileCallbacksActive++;

            pBasePtr = (char *) pOverlapped;
            pBasePtr = pBasePtr - g_OffsetFromAsyncIOInfoToIOBlock;
            pBuffer = (CIOBuffer *) pBasePtr;

            pBlockIO = pBuffer->m_pBlockIO;
            if (NULL != pBlockIO) {
                if (fSuccess) {
                    err = ENoErr;
                } else {
                    // These are declared in winerror.h
                    osErr = GetLastError();
                    switch (osErr)
                    {
                    case ERROR_HANDLE_EOF:
                        err = EEOF;
                        break;
                    default:
                        err = TranslateWin32ErrorIntoErrVal(osErr, true);
                        break;
                    }
                }

                // Finish the IO before we send the event, so the blockIO
                // will be valid before it is placed on the queue.
                pBlockIO->FinishIO(pBuffer, err, cbTransferred);
            }
            RELEASE_OBJECT(pBuffer);

            g_NumFileCallbacksActive--;
        } else if (0xFFFFFFFF == dwKey) {
            break;
        }
    } // Infinite work loop.

    return(0);
} // Win32FileIOCompletionProc.
#endif // WIN32




#if USE_LINUX_AIO
//////////////////////////////////////////////////////////////////////////////
//
// [LinuxSignalHandler]
//
//////////////////////////////////////////////////////////////////////////////
static void
LinuxSignalHandler(int sig, siginfo_t *info, void *unused) {
    struct aiocb *completedAIO;
    char *pBasePtr;
    CIOBuffer *pBuffer;

    sig = sig;
    unused = unused;
    if (NULL == info) {
       return;
    }
    completedAIO = (aiocb *) info->si_value.sival_ptr;
    if (NULL == completedAIO) {
       return;
    }

    pBasePtr = (char *) completedAIO;
    pBasePtr = pBasePtr - g_OffsetFromAsyncIOInfoToIOBlock;
    pBuffer = (CIOBuffer *) pBasePtr;

    g_pFileIOSystemImpl->WakeWorkerThread(pBuffer);
} // LinuxSignalHandler




/////////////////////////////////////////////////////////////////////////////
//
// [LinuxWorkerThreadProc]
//
/////////////////////////////////////////////////////////////////////////////
static void
LinuxWorkerThreadProc(void *arg, CSimpleThread *threadState) {
    arg = arg;
    threadState = threadState;

    g_pFileIOSystemImpl->RunWorkerThread();
} // LinuxWorkerThreadProc





/////////////////////////////////////////////////////////////////////////////
//
// [WakeWorkerThread]
//
/////////////////////////////////////////////////////////////////////////////
void
CFileIOSystem::WakeWorkerThread(CIOBuffer *pBuffer) {
    AutoLock(m_pLock);

    if (NULL != pBuffer) {
        m_FinishedIOList.InsertTail(&(pBuffer->m_BlockIOBufferList));
        ADDREF_OBJECT(pBuffer);
    }
    if (NULL != m_pIOEventSignal) {
        m_pIOEventSignal->Signal();
    }
} // WakeWorkerThread






/////////////////////////////////////////////////////////////////////////////
//
// [RunWorkerThread]
//
/////////////////////////////////////////////////////////////////////////////
void
CFileIOSystem::RunWorkerThread() {
    ErrVal err = ENoErr;
    CIOBuffer *pBuffer = NULL;
    bool fShutdown = false;
    CAsyncBlockIO *pBlockIO = NULL;
    ssize_t numResultBytes = 0;

    while (1) {
        m_pIOEventSignal->Wait();

        m_pLock->Lock();
        pBuffer = NULL;
        fShutdown = m_fShutdown;
        if (!fShutdown) {
            pBuffer = m_FinishedIOList.RemoveHead();
        }
        m_pLock->Unlock();

        if (fShutdown) {
            break;
        }

        if (NULL != pBuffer) {
            g_NumFileCallbacksActive++;
            err = ENoErr;

             numResultBytes = aio_return(&(pBuffer->m_AIORequest));
            if (numResultBytes > 0) {
               err = ENoErr;
            } else if (0 == numResultBytes) {
               err = EEOF;
            } else {
               err = EFail;
            }

            pBlockIO = pBuffer->m_pBlockIO;
            if (NULL != pBlockIO) {
                pBlockIO->FinishIO(pBuffer, err, numResultBytes);
            }

            RELEASE_OBJECT(pBuffer);
            g_NumFileCallbacksActive--;
        } // if (NULL != pBuffer)
    } // while (1);
} // RunWorkerThread

#endif // USE_LINUX_AIO


