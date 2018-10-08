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
// Memory Block I/O
//
// This implements a generic block-device interface that reads and
// writes memory buffers.
//
// This always implements IO with synchronous operations.
/////////////////////////////////////////////////////////////////////////////

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

#define MAX_SANE_MEMORY_BLOCK_IO_SIZE   10000000

/////////////////////////////////////////////////////////////////////////////
class CMemoryBlockIO : public CAsyncBlockIO {
public:
    CMemoryBlockIO();
    virtual ~CMemoryBlockIO();
    NEWEX_IMPL()

    // CAsyncBlockIO
    virtual void Close();
    virtual ErrVal Resize(int64 newLength);

    // CDebugObject
    virtual ErrVal CheckState();

protected:
    friend class CMemoryIOSystem;

    // CAsyncBlockIO
    virtual void ReadBlockAsyncImpl(CIOBuffer *pBuffer);
    virtual void WriteBlockAsyncImpl(CIOBuffer *pBuffer);

    bool    m_fBufferAllocatedFromMemory;
    int32   m_BufferPhysicalSize;

    char    *m_pBuffer;
}; // CMemoryBlockIO




/////////////////////////////////////////////////////////////////////////////
class CMemoryIOSystem : public CIOSystem {
public:
    CMemoryIOSystem();
    virtual ~CMemoryIOSystem();
    NEWEX_IMPL()

    // CIOSystem
    virtual ErrVal Shutdown();
    virtual ErrVal OpenBlockIO(
                        CParsedUrl *pUrl,
                        int32 options,
                        CAsyncBlockIOCallback *pCallback);
    virtual int32 GetDefaultBytesPerBlock() { return(1024); }
    virtual int64 GetIOStartPosition(int64 pos) { return(pos); }
    virtual int32 GetBlockBufferAlignment() { return(0); }

    // CDebugObject
    virtual ErrVal CheckState();
}; // CMemoryIOSystem




/////////////////////////////////////////////////////////////////////////////
//
// [InitializeMemoryBlockIO]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
InitializeMemoryBlockIO() {
    g_pMemoryIOSystem = newex CMemoryIOSystem;
    if (NULL == g_pMemoryIOSystem) {
        returnErr(EFail);
    }
    returnErr(ENoErr);
} // InitializeMemoryBlockIO.




/////////////////////////////////////////////////////////////////////////////
//
// [CMemoryBlockIO]
//
/////////////////////////////////////////////////////////////////////////////
CMemoryBlockIO::CMemoryBlockIO() {
    m_MediaType = MEMORY_MEDIA;
    m_fSeekable = true;

    m_pBuffer = NULL;
    m_BufferPhysicalSize = 0;
    m_fBufferAllocatedFromMemory = false;
} // CMemoryBlockIO.




/////////////////////////////////////////////////////////////////////////////
//
// [~CMemoryBlockIO]
//
/////////////////////////////////////////////////////////////////////////////
CMemoryBlockIO::~CMemoryBlockIO() {
    // Do NOT deallocate the memory buffer. That is done as
    // part of an explicit delete call.
    m_pBuffer = NULL;
} // ~CMemoryBlockIO.






/////////////////////////////////////////////////////////////////////////////
//
// [Resize]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CMemoryBlockIO::Resize(int64 newLength) {
    ErrVal err = ENoErr;
    AutoLock(m_pLock);
    RunChecks();

    DEBUG_LOG("CMemoryBlockIO::Resize: new length = %ld, old maxLength = %ld.",
        newLength, m_BufferPhysicalSize);

    if (newLength >= MAX_SANE_MEMORY_BLOCK_IO_SIZE) {
        gotoErr(EFail);
    }

    if (newLength <= m_BufferPhysicalSize) {
        m_MediaSize = newLength;
        err = ENoErr;
        gotoErr(err);
    }

    if ((!(m_BlockIOFlags & CAsyncBlockIO::RESIZEABLE))
        || (!m_fBufferAllocatedFromMemory)) {
        gotoErr(EFail);
    }

    DEBUG_LOG("CMemoryBlockIO::Resize: Allocating new larger buffer.");

    m_pBuffer = (char *) g_MainMem.Realloc(m_pBuffer, (int32) newLength);
    if (NULL == m_pBuffer) {
        gotoErr(EFail);
    }

    m_MediaSize = newLength;
    m_BufferPhysicalSize = (int32) newLength;

abort:
    returnErr(err);
} // Resize.








/////////////////////////////////////////////////////////////////////////////
//
// [Close]
//
/////////////////////////////////////////////////////////////////////////////
void
CMemoryBlockIO::Close() {
    AutoLock(m_pLock);
    RunChecksOnce();

    DEBUG_LOG("CMemoryBlockIO::Close.");

    CAsyncBlockIO::Close();

    if ((m_pBuffer) && (m_fBufferAllocatedFromMemory)) {
        g_MainMem.Free(m_pBuffer);
        m_pBuffer = NULL;
    }
} // Close.






/////////////////////////////////////////////////////////////////////////////
//
// [WriteBlockAsyncImpl]
//
// Writing may expand the buffer if that is allowed.
/////////////////////////////////////////////////////////////////////////////
void
CMemoryBlockIO::WriteBlockAsyncImpl(CIOBuffer *pBuffer) {
    ErrVal err = ENoErr;
    char *destPtr;
    int32 newLength;
    AutoLock(m_pLock);
    RunChecks();

    DEBUG_LOG("CMemoryBlockIO::WriteBlockAsyncImpl.");

    if (NULL == m_pBuffer) {
        return;
    }

    // If the requested IO is too big, then we either
    // expand the buffer or clip the IO.
    if ((pBuffer->m_PosInMedia + pBuffer->m_NumValidBytes) > m_MediaSize) {
        DEBUG_LOG("CMemoryBlockIO::WriteBlockAsyncImpl. Expanding the memory buffer.");
        DEBUG_LOG("CMemoryBlockIO::WriteBlockAsyncImpl. m_MediaSize = %d", m_MediaSize);
        DEBUG_LOG("CMemoryBlockIO::WriteBlockAsyncImpl. pBuffer->m_NumValidBytes = %d", pBuffer->m_NumValidBytes);
        DEBUG_LOG("CMemoryBlockIO::WriteBlockAsyncImpl. pBuffer->m_PosInMedia = %d", (int32) pBuffer->m_PosInMedia);

        if ((pBuffer->m_PosInMedia + pBuffer->m_NumValidBytes) <= m_BufferPhysicalSize) {
            m_MediaSize = pBuffer->m_PosInMedia + pBuffer->m_NumValidBytes;
        } else if (m_BlockIOFlags & RESIZEABLE) {
            newLength = (int32) (pBuffer->m_PosInMedia + pBuffer->m_NumValidBytes);
            if (newLength >= MAX_SANE_MEMORY_BLOCK_IO_SIZE) {
                gotoErr(EFail);
            }

            m_pBuffer = (char *) (g_MainMem.Realloc(m_pBuffer, newLength));
            if (NULL == m_pBuffer) {
                gotoErr(EFail);
            }

            m_BufferPhysicalSize = newLength;
            m_MediaSize = pBuffer->m_PosInMedia + pBuffer->m_NumValidBytes;
        }    // resizing the buffer
        else {
            gotoErr(EFail);
        } // clipping the IO.
    } // handling a too-big IO.

    // Do the actual IO.
    destPtr = m_pBuffer + pBuffer->m_PosInMedia;
    memcpy(
        destPtr,
        pBuffer->m_pLogicalBuffer,
        pBuffer->m_NumValidBytes);

abort:
    // Mark the block as complete.
    FinishIO(pBuffer, err, pBuffer->m_NumValidBytes);
} // WriteBlockAsyncImpl.







/////////////////////////////////////////////////////////////////////////////
//
// [ReadBlockAsyncImpl]
//
/////////////////////////////////////////////////////////////////////////////
void
CMemoryBlockIO::ReadBlockAsyncImpl(CIOBuffer *pBuffer) {
    ErrVal err = ENoErr;
    char *srcPtr;
    int32 actualIOSize;
    AutoLock(m_pLock);
    RunChecks();

    DEBUG_LOG("CMemoryBlockIO::ReadBlockAsyncImpl.");

    if ((NULL == m_pBuffer) || (NULL == m_pIOSystem)) {
        gotoErr(EFail);
    }

    DEBUG_LOG("CMemoryBlockIO::ReadBlockAsyncImpl. pBuffer->m_PosInMedia = %d", pBuffer->m_PosInMedia);
    DEBUG_LOG("CMemoryBlockIO::ReadBlockAsyncImpl. pBuffer->m_BufferSize = %d", pBuffer->m_BufferSize);
    DEBUG_LOG("CMemoryBlockIO::ReadBlockAsyncImpl. m_MediaSize = %d", m_MediaSize);

    // If the requested IO is too big, then we clip the io.
    actualIOSize = pBuffer->m_BufferSize;
    if ((pBuffer->m_PosInMedia + actualIOSize) > m_MediaSize) {
        actualIOSize = (int32) (m_MediaSize - pBuffer->m_PosInMedia);
        DEBUG_LOG("CMemoryBlockIO::ReadBlockAsyncImpl. Clipping IO. actualIOSize = %d", actualIOSize);
    }

    // Don't read more than 1 block per IO.
    if (actualIOSize > m_pIOSystem->GetDefaultBytesPerBlock()) {
        actualIOSize = m_pIOSystem->GetDefaultBytesPerBlock();
        DEBUG_LOG("CMemoryBlockIO::ReadBlockAsyncImpl. Growing to blocksize. actualIOSize = %d", actualIOSize);
    }

    if (actualIOSize < 0) {
        actualIOSize = 0;
        gotoErr(EEOF);
    }

    // Do the actual IO.
    srcPtr = m_pBuffer + pBuffer->m_PosInMedia;
    memcpy(pBuffer->m_pLogicalBuffer, srcPtr, actualIOSize);

abort:
    // Mark the block as complete.
    FinishIO(pBuffer, err, actualIOSize);
} // ReadBlockAsyncImpl.







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
CMemoryBlockIO::CheckState() {
    ErrVal err = ENoErr;
    AutoLock(m_pLock);

    err = CAsyncBlockIO::CheckState();
    if (err) {
        returnErr(EFail);
    }

    if ((MEMORY_MEDIA != m_MediaType)
        || (NULL == m_pIOSystem)
        || (!m_fSeekable)
        || (NULL == m_pBuffer)
        || (m_BufferPhysicalSize < 0)
        || (m_MediaSize > m_BufferPhysicalSize)) {
        gotoErr(EFail);
    }

    // Don't check the active connection list, since we don't hold
    // any lock.

    if (m_fBufferAllocatedFromMemory) {
        int32 realBufSize;

        realBufSize = g_MainMem.GetPtrSize(m_pBuffer);
        if ((realBufSize < 0)
            || (realBufSize < m_MediaSize)
            || (realBufSize != m_BufferPhysicalSize)) {
            gotoErr(EFail);
        }

        err = g_MainMem.CheckPtr(m_pBuffer);
        if (err) {
            gotoErr(err);
        }
    }

abort:
    returnErr(err);
} // CheckState.





/////////////////////////////////////////////////////////////////////////////
//
// [CMemoryIOSystem]
//
/////////////////////////////////////////////////////////////////////////////
CMemoryIOSystem::CMemoryIOSystem() {
} // CMemoryIOSystem.




/////////////////////////////////////////////////////////////////////////////
//
// [~CMemoryIOSystem]
//
/////////////////////////////////////////////////////////////////////////////
CMemoryIOSystem::~CMemoryIOSystem() {
} // ~CMemoryIOSystem.




/////////////////////////////////////////////////////////////////////////////
//
// [Shutdown]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CMemoryIOSystem::Shutdown() {
    ErrVal err = ENoErr;

    // If we never lazily initialized the IO system, then there is nothing
    // to do.
    if (!m_fInitialized) {
        returnErr(ENoErr);
    }

    // Run any standard debugger checks.
    RunChecks();

    err = CIOSystem::Shutdown();
    if (err) {
        gotoErr(err);
    }

abort:
    returnErr(err);
} // Shutdown.





/////////////////////////////////////////////////////////////////////////////
//
// [OpenBlockIO]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CMemoryIOSystem::OpenBlockIO(
                        CParsedUrl *pUrl,
                        int32 options,
                        CAsyncBlockIOCallback *pCallback) {
    ErrVal err = ENoErr;
    char *ptr = NULL;
    CMemoryBlockIO *pBlockIO = NULL;
    char cSaveChar;
    char *pFilePtr;
    char *pEndFilePtr;
    char *pBuffer;
    int32 bufferLength;
    int32 numFieldsRead;
    AutoLock(m_pLock);
    RunChecks();

    DEBUG_LOG("CMemoryIOSystem::OpenBlockIO");

    if ((NULL == pUrl)
        || (CParsedUrl::URL_SCHEME_MEMORY != pUrl->m_Scheme)
        || (NULL == pUrl->m_pPath)
        || (0 == pUrl->m_PathSize)
        || (NULL == pCallback)) {
        gotoErr(EFail);
    }


    // Lazily initialize the IO system when we open the first blockIO.
    if (!m_fInitialized) {
        err = CIOSystem::InitIOSystem();
        if (err) {
            gotoErr(err);
        }
    }

    pBlockIO = newex CMemoryBlockIO;
    if (NULL == pBlockIO) {
        gotoErr(EFail);
    }

    // Parse the URL.
    pFilePtr = pUrl->m_pPath;
    pEndFilePtr = pFilePtr + pUrl->m_PathSize;
    cSaveChar = *pEndFilePtr;
    *pEndFilePtr = 0;

    DEBUG_LOG("CMemoryIOSystem::OpenBlockIO: Url Path = %s", pUrl->m_pPath);
    numFieldsRead = sscanf(pFilePtr, "%p/%d/", &pBuffer, &bufferLength);
    *pEndFilePtr = cSaveChar;
    if (2 != numFieldsRead) {
        gotoErr(EFail);
    }

    if ((options & CAsyncBlockIO::CREATE_NEW_STORE) || (NULL == pBuffer)) {
        DEBUG_LOG("CMemoryIOSystem::OpenBlockIO: Creating a new store");

        ptr = (char *) memAlloc(bufferLength);
        if (NULL == ptr) {
            gotoErr(EFail);
        }

        // These are part of the memory block IO subclass.
        pBlockIO->m_pBuffer = ptr;
        pBlockIO->m_BufferPhysicalSize = bufferLength;
        pBlockIO->m_fBufferAllocatedFromMemory = true;
        ptr = NULL;
    } else
    {
        DEBUG_LOG("CMemoryIOSystem::OpenBlockIO: Opening an existing buffer");

        pBlockIO->m_pBuffer = pBuffer;
        pBlockIO->m_BufferPhysicalSize = bufferLength;
        pBlockIO->m_fBufferAllocatedFromMemory = false;
    }

    // These are part of the base class.
    pBlockIO->m_MediaType = CAsyncBlockIO::MEMORY_MEDIA;
    pBlockIO->m_fSeekable = true;
    pBlockIO->m_pIOSystem = this;

    pBlockIO->m_BlockIOFlags = 0;
    pBlockIO->m_MediaSize = bufferLength;
    pBlockIO->m_ActiveBlockIOs.ResetQueue();

    pBlockIO->ChangeBlockIOCallback(pCallback);
    pBlockIO->m_ActiveBlockIOs.ResetQueue();

    pBlockIO->m_pUrl = pUrl;
    ADDREF_OBJECT(pUrl);

    pBlockIO->m_pLock = CRefLock::Alloc();
    if (NULL == pBlockIO->m_pLock) {
        gotoErr(EFail);
    }

    // Add this connection to the list of active connections.
    m_ActiveBlockIOs.InsertHead(&(pBlockIO->m_ActiveBlockIOs));
    ADDREF_OBJECT(pBlockIO);

    pBlockIO->m_BlockIOFlags |= CAsyncBlockIO::BLOCKIO_IS_OPEN;

    //<>
    pBlockIO->m_fSynchronousDevice = true;
    //<>

    pCallback->OnBlockIOOpen(ENoErr, pBlockIO);

abort:
    // The callback owns pBlockIO now.
    RELEASE_OBJECT(pBlockIO);
    if (ptr) {
        memFree(ptr);
    }

    returnErr(err);
} // OpenBlockIO.





/////////////////////////////////////////////////////////////////////////////
//
// [CheckState]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CMemoryIOSystem::CheckState() {
    AutoLock(m_pLock);
    returnErr(ENoErr);
} // CheckState.







