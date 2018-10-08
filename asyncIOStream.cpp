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
// AsyncIOStream Module
//
// This implements basic stream interface on top of block IO devices like
// files, networks, and memory regions. It presents the data as a continuous
// stream of bytes, so it hides packet, sector and block boundaries.
//
// An AsyncIOStream keeps the data in a queue of buffers. The number of buffers
// will vary depending on the underlying block device. If the device is seekable,
// like a file, then the AsyncIOStream keeps only a few memory buffers, and will
// synchronously reads buffers from the file as they are needed. This allows
// a data stream to keep a small memory footprint for a huge file. It also
// means the AsyncIOStream acts like a cache, so it orders the buffers in LRU
// order and implements a replacement policy.
//
// If the underlying blockIO is not seekable, like a network connection, then
// the AsyncIOStream keeps all buffers in memory. If this becomes a problem,
// like using HTTP to download huge documents, then a higher level of the code,
// the HTTP stream module, will transfer data from a http AsyncIOStream to
// a file AsyncIOStream.
//
// Finally, some asyncIO streams can just keep data in buffers, like a growing
// bag of bytes in memory. This is useful to first format and write some data
// to a temporary stream and then transfer that to another network or file
// stream.
//
// This module provides asynchrony with the "listen" and "flush" functions.
// Reading and writing a stream never blocks, and is never asynchronous. Instead,
// it always reads or writes a memory buffer. This is important because it means
// that stream reading code, like a grammar parser, never has the complexity of
// asynchrony. Instead, clients first ask the IO stream to asynchronously report
// when some data is available, either a specific number of bytes or any bytes.
// This will signal a callback function (the event handler), and then the client
// can do the reads.
//
// Similarly, when you write to a stream, the data is internally written to a
// buffer and may be flushed to the backing store asynchronously. There is
// an asynchronous flush command which will notify the client when all data
// in the stream has been comitted to the backing store.
//
// There is one callback function associated with each stream. This receives
// all notifications that data is available or a write has completed. Normally,
// this is provided when the stream is opened, but it can also be dynamically
// changed when the "owner" of a stream is changed. Originally, I wanted a
// different callback for each operation, but this had several problems. For
// example, some events (like a network peer disconnects) may happen at any
// time, so you really need a constant listener callback. Individual callbacks
// also adds complications when several asynch-listens are active at once; you
// must match each callback to each listen.
//
// Internally, the buffers never overlap, and each corresponds to a single buffer
// that we would read or write in the underlying block device.
// There is one active foreground buffer where the stream is currently reading and
// possibly writing. Other buffers are unused, or valid but
// idle (cache), or in transit with an asynchronous read or write.
//
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
#include "nameTable.h"
#include "url.h"
#include "blockIO.h"
#include "asyncIOStream.h"

FILE_DEBUGGING_GLOBALS(LOG_LEVEL_DEFAULT, 0);





/////////////////////////////////////////////////////////////////////////////
//
// [CAsyncIOStream]
//
// This procedure leaves the AsyncIOStream in an initial state so
// ohter methods can tell that init has not been called yet.
/////////////////////////////////////////////////////////////////////////////
CAsyncIOStream::CAsyncIOStream() {
    m_AsyncIOStreamFlags = 0;
    m_pLock = NULL;

    m_pBlockIO = NULL;
    m_pIOSystem = NULL;

    m_TotalAvailableBytes = 0;
    m_MinPosition = 0;

    m_pActiveIOBuffer = NULL;
    m_pFirstValidByte = NULL;
    m_pNextValidByte = NULL;
    m_pEndValidBytes = NULL;
    m_pLastPossibleValidByte = NULL;

    m_pActiveOutputIOBuffer = NULL;
    m_pFirstValidOutputByte = NULL;
    m_pNextValidOutputByte = NULL;
    m_pLastPossibleValidOutputByte = NULL;

    m_IOBufferList.ResetQueue();
    m_MaxNumIOBuffersForSeekableDevices = 1;

    m_AsynchLoadType = NOT_LOADING;
    m_NextAsynchBufferPosition = 0;
    m_LoadStartPosition = 0;
    m_LoadStopPosition = 0;
    m_NextLoadStartPosition = 0;

    m_OutputBufferList.ResetQueue();

    m_pEventHandler = NULL;
    m_pEventHandlerContext = NULL;
} // CAsyncIOStream.







/////////////////////////////////////////////////////////////////////////////
//
// [~CAsyncIOStream]
//
/////////////////////////////////////////////////////////////////////////////
CAsyncIOStream::~CAsyncIOStream() {
    if (NULL != m_pBlockIO) {
       Close();
    }

    RELEASE_OBJECT(m_pEventHandler);
    RELEASE_OBJECT(m_pLock);
} // ~CAsyncIOStream.







/////////////////////////////////////////////////////////////////////////////
//
// [OpenAsyncIOStream]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOStream::OpenAsyncIOStream(
                        CParsedUrl *pUrl,
                        int32 openOptions,
                        CAsyncIOEventHandler *pEventHandler,
                        void *pEventHandlerContext
                       ) {
    ErrVal err = ENoErr;
    CAsyncIOStream *pAsyncIOStream = NULL;

    if ((NULL == pUrl) || (NULL == pEventHandler)) {
        gotoErr(EFail);
    }

    pAsyncIOStream = newex CAsyncIOStream;
    if (NULL == pAsyncIOStream) {
        gotoErr(EFail);
    }

    DEBUG_LOG("CAsyncIOStream::OpenAsyncIOStream. url = %s",
                pUrl->GetPrintableURL());

    // Don't call RunChecks because we do not have an open stream yet.
    err = pAsyncIOStream->StartOpenCommand(pEventHandler, pEventHandlerContext);
    if (err) {
        gotoErr(err);
    }

    if ((CParsedUrl::URL_SCHEME_HTTP == pUrl->m_Scheme)
        || (CParsedUrl::URL_SCHEME_URN == pUrl->m_Scheme)
        || (CParsedUrl::URL_SCHEME_FTP == pUrl->m_Scheme)
        || (CParsedUrl::URL_SCHEME_IP_ADDRESS == pUrl->m_Scheme)) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::OpenAsyncIOStream. Call g_pNetIOSystem->OpenBlockIO");
        err = g_pNetIOSystem->OpenBlockIO(pUrl, openOptions, pAsyncIOStream);
    } else if (CParsedUrl::URL_SCHEME_FILE == pUrl->m_Scheme) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::OpenAsyncIOStream. Call g_pFileIOSystem->OpenBlockIO");
        openOptions |= CAsyncBlockIO::USE_SYNCHRONOUS_IO;
        err = g_pFileIOSystem->OpenBlockIO(pUrl, openOptions, pAsyncIOStream);
    } else if (CParsedUrl::URL_SCHEME_MEMORY == pUrl->m_Scheme) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::OpenAsyncIOStream. Call g_pMemoryIOSystem->OpenBlockIO");
        openOptions |= CAsyncBlockIO::USE_SYNCHRONOUS_IO;
        err = g_pMemoryIOSystem->OpenBlockIO(pUrl, openOptions, pAsyncIOStream);
    } else if (CParsedUrl::URL_SCHEME_EMPTY == pUrl->m_Scheme) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::OpenAsyncIOStream. Call g_EmptyIOSystem.OpenBlockIO");
        ASSERT(0);
    } else {
        err = EFail;
    }
    DEBUG_LOG_VERBOSE("CAsyncIOStream::OpenAsyncIOStream. OpenBlockIO returns err=%d", err);

    // DO NOT DO ANY WORK HERE.
    // OpenBlockIO is asynch, so it may have already signalled
    // completion by now.

abort:
    RELEASE_OBJECT(pAsyncIOStream);

    returnErr(err);
} // OpenAsyncIOStream.








/////////////////////////////////////////////////////////////////////////////
//
// [StartOpenCommand]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOStream::StartOpenCommand(
                        CAsyncIOEventHandler *pEventHandler,
                        void *pEventHandlerContext) {
    ErrVal err = ENoErr;

    // If this stream is already open, then we cannot re-open it.
    if (m_pBlockIO) {
        gotoErr(EFail);
    }

    m_AsyncIOStreamFlags = 0;

    // It is OK for pEventHandler to be NULL.
    m_pEventHandler = pEventHandler;
    ADDREF_OBJECT(m_pEventHandler);
    m_pEventHandlerContext = pEventHandlerContext;

    // Initially, there are no bytes in the buffer.
    m_pActiveIOBuffer = NULL;
    m_pFirstValidByte = NULL;
    m_pNextValidByte = NULL;
    m_pEndValidBytes = NULL;
    m_pLastPossibleValidByte = NULL;

    m_pActiveOutputIOBuffer = NULL;
    m_pFirstValidOutputByte = NULL;
    m_pNextValidOutputByte = NULL;
    m_pLastPossibleValidOutputByte = NULL;

    m_IOBufferList.ResetQueue();
    m_OutputBufferList.ResetQueue();

    m_MaxNumIOBuffersForSeekableDevices = 1;
    m_TotalAvailableBytes = 0;

abort:
    returnErr(err);
} // StartOpenCommand.







/////////////////////////////////////////////////////////////////////////////
//
// [FinishOpenCommand]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOStream::FinishOpenCommand() {
    ErrVal err = ENoErr;
    char cSaveChar;
    char *pFilePtr;
    char *pEndFilePtr;
    char *pBuffer;
    int32 bufferLength;
    int32 numValidBytes;
    int32 numFieldsRead;

    DEBUG_LOG_VERBOSE("CAsyncIOStream::FinishOpenCommand");
    if (NULL == m_pBlockIO) {
        returnErr(EFail);
    }

    m_pIOSystem = m_pBlockIO->GetIOSystem();
    m_TotalAvailableBytes = m_pBlockIO->GetMediaSize();

    // The lock may have been held by a previous blockIO.
    // This can happen when we accept a network connection.
    RELEASE_OBJECT(m_pLock);
    m_pLock = m_pBlockIO->GetLock();
    if (NULL == m_pLock) {
        gotoErr(EFail);
    }

    // If this is a memory blockIO, then make one buffer entry that
    // references the contents of the blockIO.
    if ((NULL != m_pBlockIO->m_pUrl)
        && (CParsedUrl::URL_SCHEME_MEMORY == m_pBlockIO->m_pUrl->m_Scheme)) {
        numValidBytes = 0;

        // Parse the URL.
        pFilePtr = m_pBlockIO->m_pUrl->m_pPath;
        pEndFilePtr = pFilePtr + m_pBlockIO->m_pUrl->m_PathSize;
        cSaveChar = *pEndFilePtr;
        *pEndFilePtr = 0;
        numFieldsRead = sscanf(
                            m_pBlockIO->m_pUrl->m_pPath,
                            "%p/%d/%d",
                            &pBuffer,
                            &bufferLength,
                            &numValidBytes);
        *pEndFilePtr = cSaveChar;

        if (3 != numFieldsRead) {
            gotoErr(EFail);
        }

        DEBUG_LOG_VERBOSE("CAsyncIOStream::FinishOpenCommand. Memory stream. numValidBytes = %d", numValidBytes);
        if (numValidBytes > 0) {
            m_pActiveIOBuffer = g_pMemoryIOSystem->AllocIOBuffer(-1, false);
            if (NULL == m_pActiveIOBuffer) {
                gotoErr(EFail);
            }

            m_pActiveIOBuffer->m_BufferOp = CIOBuffer::NO_OP;
            m_pActiveIOBuffer->m_BufferFlags |= CIOBuffer::VALID_DATA;
            m_pActiveIOBuffer->m_BufferFlags |= CIOBuffer::INPUT_BUFFER;
            m_pActiveIOBuffer->m_BufferFlags |= CIOBuffer::OUTPUT_BUFFER;
            m_pActiveIOBuffer->m_Err = ENoErr;

            // buffer and bufferSize are initialized by AllocIOBuffer.
            m_pActiveIOBuffer->m_pPhysicalBuffer = pBuffer;
            m_pActiveIOBuffer->m_pLogicalBuffer = m_pActiveIOBuffer->m_pPhysicalBuffer;
            m_pActiveIOBuffer->m_BufferSize = numValidBytes;
            m_pActiveIOBuffer->m_NumValidBytes = numValidBytes;

            m_pActiveIOBuffer->m_PosInMedia = 0;

            // The buffer was AddRef'ed by AllocIOBuffer.
            // Keep m_IOBufferList in LRU order. New buffers are the most recently
            // used, so they are added to the head of the list.
            m_IOBufferList.InsertHead(&(m_pActiveIOBuffer->m_StreamBufferList));

            m_pFirstValidByte = m_pActiveIOBuffer->m_pLogicalBuffer;
            m_pNextValidByte = m_pFirstValidByte;
            m_pEndValidBytes = m_pActiveIOBuffer->m_pLogicalBuffer + numValidBytes;
            m_pLastPossibleValidByte = m_pActiveIOBuffer->m_pPhysicalBuffer + bufferLength;

            m_TotalAvailableBytes = numValidBytes;
        } // (numValidBytes > 0)
    } // (CParsedUrl::URL_SCHEME_MEMORY == m_pUrl->m_Scheme)

abort:
    returnErr(err);
} // FinishOpenCommand.






/////////////////////////////////////////////////////////////////////////////
//
// [Close]
//
/////////////////////////////////////////////////////////////////////////////
void
CAsyncIOStream::Close() {
    CIOBuffer *pBuffer;
    CAsyncBlockIO *pBlockIO = NULL;
    RunChecksOnce();

    DEBUG_LOG_VERBOSE("CAsyncIOStream::Close. stream = %p", this);

    { /////////////////////////////////////////////////
        AutoLock(m_pLock);

        // Remove all buffers.
        m_pActiveIOBuffer = NULL;
        m_pFirstValidByte = NULL;
        m_pNextValidByte = NULL;
        m_pEndValidBytes = NULL;
        m_pLastPossibleValidByte = NULL;

        m_AsyncIOStreamFlags = 0;
        m_MaxNumIOBuffersForSeekableDevices = 1;
        m_AsynchLoadType = NOT_LOADING;

        m_pActiveOutputIOBuffer = NULL;
        m_pFirstValidOutputByte = NULL;
        m_pNextValidOutputByte = NULL;
        m_pLastPossibleValidOutputByte = NULL;

        // Close this outside the lock.
        pBlockIO = m_pBlockIO;
        m_pBlockIO = NULL;
        m_pIOSystem = NULL;

        while (1) {
            pBuffer = m_IOBufferList.RemoveHead();
            if (NULL == pBuffer) {
                break;
            }
            DEBUG_LOG_VERBOSE("CAsyncIOStream::Close. Release buffer %p", pBuffer);
            RELEASE_OBJECT(pBuffer);
        }
        while (1) {
            pBuffer = m_OutputBufferList.RemoveHead();
            if (NULL == pBuffer) {
                break;
            }
            DEBUG_LOG_VERBOSE("CAsyncIOStream::Close. Release buffer %p", pBuffer);
            RELEASE_OBJECT(pBuffer);
        }
        m_IOBufferList.ResetQueue();
    } /////////////////////////////////////////////////

    if (pBlockIO) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::Close. Close blockIO %p", pBlockIO);
        pBlockIO->Close();
        RELEASE_OBJECT(pBlockIO);
    }
} // Close





/////////////////////////////////////////////////////////////////////////////
//
// [GetLock]
//
/////////////////////////////////////////////////////////////////////////////
CRefLock *
CAsyncIOStream::GetLock() {
    if (m_pBlockIO) {
        ADDREF_OBJECT(m_pLock);
        return(m_pLock);
    }

    return(NULL);
} // GetLock.





/////////////////////////////////////////////////////////////////////////////
//
// [SetPosition]
//
// This moves the cursor to a block of data that covers the desired position.
//
// If this stream uses a seekable device, then this effects both the read
// and write position. If this stream has a non-seekable device, then this
// only effects the read position.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOStream::SetPosition(int64 newPos) {
    ErrVal err = ENoErr;
    int64 offset;
    CIOBuffer *pBuffer;
    bool fFoundBuffer;
    AutoLock(m_pLock);
    RunChecks();

    DEBUG_LOG_VERBOSE( "CAsyncIOStream::SetPosition. newPos = " INT64FMT, newPos );

    // The stream must have been initialized.
    if ((NULL == m_pBlockIO) || (newPos < 0)) {
        gotoErr(EFail);
    }

    // Check if the buffer already covers this byte position.
    // This is (hopefully) the most common case.
    if ((m_pActiveIOBuffer)
        && (m_pFirstValidByte)
        && (newPos >= m_pActiveIOBuffer->m_PosInMedia)) {
        if (newPos < (m_pActiveIOBuffer->m_PosInMedia + m_pActiveIOBuffer->m_NumValidBytes)) {
            offset = newPos - m_pActiveIOBuffer->m_PosInMedia;
            m_pNextValidByte = m_pFirstValidByte + offset;
            gotoErr(ENoErr);
        }

        // As a special case, handle seeking to the end of the file.
        if ((newPos == (m_pActiveIOBuffer->m_PosInMedia + m_pActiveIOBuffer->m_NumValidBytes))
            && (newPos >= m_TotalAvailableBytes)) {
            offset = newPos - m_pActiveIOBuffer->m_PosInMedia;
            m_pNextValidByte = m_pFirstValidByte + offset;
            gotoErr(EEOF);
        }
    } // seeking within the current buffer.

    DEBUG_LOG_VERBOSE("CAsyncIOStream::SetPosition. Need to change the current buffer");

    // Look for the buffer, it may be lying around if we are
    // seeking back in the media, or if the media is a string
    // and all data is in the buffer.
    fFoundBuffer = false;
    pBuffer = m_IOBufferList.GetHead();
    while (NULL != pBuffer) {
        if ((newPos >= pBuffer->m_PosInMedia)
            && (newPos < (pBuffer->m_PosInMedia + pBuffer->m_NumValidBytes))
            && (pBuffer->m_BufferFlags & CIOBuffer::INPUT_BUFFER)) {
            fFoundBuffer = true;
        } else if ((newPos >= pBuffer->m_PosInMedia)
            && (newPos == (pBuffer->m_PosInMedia + pBuffer->m_NumValidBytes))
            && (newPos >= m_TotalAvailableBytes)
            && (pBuffer->m_BufferFlags & CIOBuffer::INPUT_BUFFER)) {
            fFoundBuffer = true;
        }

        if (fFoundBuffer) {
            DEBUG_LOG_VERBOSE("CAsyncIOStream::SetPosition. Found a new buffer (err = %d)", pBuffer->m_Err);
            if (ENoErr != pBuffer->m_Err) {
                gotoErr(pBuffer->m_Err);
            }

            m_pActiveIOBuffer = pBuffer;
            offset = newPos - m_pActiveIOBuffer->m_PosInMedia;

            m_pFirstValidByte = m_pActiveIOBuffer->m_pLogicalBuffer;
            m_pNextValidByte = m_pFirstValidByte + offset;
            m_pEndValidBytes = m_pFirstValidByte + m_pActiveIOBuffer->m_NumValidBytes;
            m_pLastPossibleValidByte = m_pActiveIOBuffer->m_pPhysicalBuffer + m_pActiveIOBuffer->m_BufferSize;

            // Keep m_IOBufferList in LRU order. If we are touching this buffer, then
            // move it to the head of the list.
            pBuffer->m_StreamBufferList.RemoveFromQueue();
            m_IOBufferList.InsertHead(&(pBuffer->m_StreamBufferList));

            gotoErr(ENoErr);
        } // looking at an IO that has the buffer we need.

        pBuffer = pBuffer->m_StreamBufferList.GetNextInQueue();
    } // looking at all completed IO ops for the buffer we need.

    // You can always seek to byte 0 even in an empty stream.
    if ((0 == newPos) && (0 == m_TotalAvailableBytes)) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::SetPosition. Seek to the start of an empty stream");
        gotoErr(ENoErr);
    }

    if ((newPos > m_TotalAvailableBytes)
       && !(m_AsyncIOStreamFlags & EXPANDING_MEMORY_STREAM)) {
        DEBUG_LOG("CAsyncIOStream::SetPosition. Failed to seek past EOF. m_TotalAvailableBytes = " INT64FMT,
                  m_TotalAvailableBytes);
        gotoErr(EEOF);
    }
    // If the device is not seekable, then there is nowhere to read it
    // from (this will change in the future when we cache to temp files).
    // For now, this means we cannot find the block.
    if (!(m_pBlockIO->m_fSeekable)
       && !(m_AsyncIOStreamFlags & EXPANDING_MEMORY_STREAM)
       && !(m_AsyncIOStreamFlags & ALL_DATA_IS_IN_BUFFERS)) {
        DEBUG_LOG("CAsyncIOStream::SetPosition. Failed to seek past loaded buffers for a non-seekable stream");
        gotoErr(EEOF);
    }

    // We are about to change the current buffer.
    DEBUG_LOG_VERBOSE("CAsyncIOStream::SetPosition. Set the current buffer to the background");
    err = MoveBufferToBackground(m_pActiveIOBuffer);
    if (err) {
        gotoErr(err);
    }

    // We will either allocate a new block, or else recycle an existing one.
    m_pActiveIOBuffer = AllocAsyncIOStreamBuffer(newPos, true);
    if (NULL == m_pActiveIOBuffer) {
        gotoErr(EFail);
    }

    m_pFirstValidByte = m_pActiveIOBuffer->m_pLogicalBuffer;
    m_pNextValidByte = m_pActiveIOBuffer->m_pLogicalBuffer;
    m_pEndValidBytes = m_pActiveIOBuffer->m_pLogicalBuffer;
    m_pLastPossibleValidByte = m_pActiveIOBuffer->m_pPhysicalBuffer + m_pActiveIOBuffer->m_BufferSize;

    if ((m_pActiveIOBuffer->m_PosInMedia < m_TotalAvailableBytes)
       && !(m_AsyncIOStreamFlags & EXPANDING_MEMORY_STREAM)) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::SetPosition. Starting a new read");
        m_pBlockIO->ReadBlockAsync(m_pActiveIOBuffer);
    }

    // This is seeking to the end of a stream. We do this when we want to start
    // writing at the end. This is not an EOF error.
    if ((EEOF == err) && (newPos == m_TotalAvailableBytes)) {
        err = ENoErr;
    }

abort:
    returnErr(err);
} // SetPosition.







/////////////////////////////////////////////////////////////////////////////
//
// [GetPosition]
//
// This always returns the current read position. If we read and write
// in the same buffers, then this is also the write position. If we write
// and read to separate IO channels (like in a network) then there is
// no write position, since it is a non-seekable device.
/////////////////////////////////////////////////////////////////////////////
inline int64
CAsyncIOStream::GetPosition() {
    AutoLock(m_pLock);

    if ((NULL == m_pActiveIOBuffer)
        || (NULL == m_pNextValidByte)
        || (NULL == m_pFirstValidByte)) {
        return(m_TotalAvailableBytes);
    } else {
        return(m_pActiveIOBuffer->m_PosInMedia + (m_pNextValidByte - m_pFirstValidByte));
    }
} // GetPosition.






/////////////////////////////////////////////////////////////////////////////
//
// [GetDataLength]
//
/////////////////////////////////////////////////////////////////////////////
int64
CAsyncIOStream::GetDataLength() {
    int64 maxBufferPos;
    int64 eof;
    AutoLock(m_pLock);

    eof = m_TotalAvailableBytes;

    // There may be unsaved data at the end of the buffer.
    if (m_pActiveIOBuffer) {
        maxBufferPos = m_pActiveIOBuffer->m_PosInMedia
                            + m_pActiveIOBuffer->m_NumValidBytes;
        if (maxBufferPos > eof) {
            eof = maxBufferPos;
        }
    }

#if DD_DEBUG
    CIOBuffer *pBuffer;
    pBuffer = m_IOBufferList.GetHead();
    while (NULL != pBuffer) {
        maxBufferPos = pBuffer->m_PosInMedia + pBuffer->m_NumValidBytes;
        if (maxBufferPos > eof) {
            DEBUG_LOG("CAsyncIOStream::GetDataLength ERROR. maxBufferPos > eof");
            ASSERT(0);
            eof = maxBufferPos;
        }

        pBuffer = pBuffer->m_StreamBufferList.GetNextInQueue();
    } // looking at all completed IO ops for the buffer we need.
#endif

    return(eof);
} // GetDataLength.







/////////////////////////////////////////////////////////////////////////////
//
// [GetPtr]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOStream::GetPtr(
                int64 startPos,
                int32 length,
                char *pTempBuffer,
                int32 maxLength,
                char **pPtr) {
    ErrVal err = ENoErr;
    int64 offset;
    int64 stopPos;
    AutoLock(m_pLock);
    RunChecks();

    DEBUG_LOG_VERBOSE("CAsyncIOStream::GetPtr. startPos = " INT64FMT ", length = %d",
                startPos, length);

    // The stream must have been initialized.
    if ((NULL == pPtr)
        || (startPos < 0)
        || (length < 0)) {
        returnErr(EFail);
    }
    *pPtr = NULL;
    stopPos = startPos + length;

    err = SetPosition(startPos);
    if (err) {
        gotoErr(err);
    }

    // Check if the buffer contains the entire range.
    if ((m_pActiveIOBuffer)
        && (m_pFirstValidByte)
        && (stopPos < (m_pActiveIOBuffer->m_PosInMedia + m_pActiveIOBuffer->m_NumValidBytes))) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::GetPtr. Using a region in an existing buffer");
        offset = startPos - m_pActiveIOBuffer->m_PosInMedia;

        m_pNextValidByte = m_pFirstValidByte + offset;
        *pPtr = m_pNextValidByte;
    } else if (pTempBuffer) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::GetPtr. Reading the data into a temporary buffer");
        if ((int64) maxLength < (stopPos - startPos)) {
            gotoErr(EFail);
        }

        // Otherwise, read the data into the temporary buffer.
        err = Read(pTempBuffer, (int32) (stopPos - startPos));
        if (err) {
            gotoErr(err);
        }
        *pPtr = pTempBuffer;
    } else
    {
        gotoErr(EFail);
    }

abort:
    returnErr(err);
} // GetPtr.







/////////////////////////////////////////////////////////////////////////////
//
// [GetPtrRef]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOStream::GetPtrRef(
                     int64 startPos,
                     int32 length,
                     char **pPtr,
                     int32 *pActualLength) {
    ErrVal err = ENoErr;
    int64 offset;
    AutoLock(m_pLock);
    RunChecks();

    DEBUG_LOG_VERBOSE("CAsyncIOStream::GetPtrRef. startPos = " INT64FMT ", length = %d",
                startPos, length);

    // The stream must have been initialized.
    // Length < 0 means get as much as you can.
    if ((NULL == pPtr)
        || (startPos < 0)
        || (NULL == pActualLength)) {
        returnErr(EFail);
    }
    *pPtr = NULL;
    *pActualLength = 0;

    err = SetPosition(startPos);
    if (err) {
        gotoErr(err);
    }

    if ((m_pActiveIOBuffer) && (m_pFirstValidByte)) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::GetPtrRef. Using a region in an existing buffer");

        offset = startPos - m_pActiveIOBuffer->m_PosInMedia;
        *pPtr = m_pFirstValidByte + offset;
        *pActualLength = (int32) ((m_pActiveIOBuffer->m_PosInMedia + m_pActiveIOBuffer->m_NumValidBytes)
                              - startPos);

        // Length < 0 means get as much as you can.
        // Length > 0 means to clip the result to that length.
        if ((length > 0) && (*pActualLength > length)) {
            *pActualLength = length;
        }
    }

abort:
    returnErr(err);
} // GetPtrRef.









/////////////////////////////////////////////////////////////////////////////
//
// [Read]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOStream::Read(char *clientBuffer, int32 bytesToRead) {
    ErrVal err = ENoErr;
    int32 bytesInBuffer;
    int32 numBytes;
    AutoLock(m_pLock);
    RunChecks();

    DEBUG_LOG_VERBOSE("CAsyncIOStream::Read. bytesToRead = %d", bytesToRead);

    // The stream must have been initialized.
    if ((NULL == m_pBlockIO)
        || (bytesToRead < 0)
        || (NULL == clientBuffer)) {
        gotoErr(EFail);
    }

    // Each loop reads from one buffer.
    while (bytesToRead > 0) {
        // Find out how many bytes are available in this buffer.
        numBytes = bytesToRead;
        if (NULL == m_pActiveIOBuffer) {
            numBytes = 0;
        } else if ((m_pEndValidBytes)
            && (m_pNextValidByte)
            && ((m_pNextValidByte + numBytes) >= m_pEndValidBytes)) {
            numBytes = m_pEndValidBytes - m_pNextValidByte;
        }

        if (numBytes > 0) {
            memcpy(clientBuffer, m_pNextValidByte, numBytes);

            clientBuffer += numBytes;
            if (m_pNextValidByte) {
                m_pNextValidByte += numBytes;
            }
            bytesToRead = bytesToRead - numBytes;
        }

        // If we did not read all of the bytes we need, then we must
        // have hit the end of the buffer. In that case, read the
        // next buffer-full of data.
        if (bytesToRead > 0) {
            int64 currentPosition = 0;

            if (NULL != m_pActiveIOBuffer) {
                currentPosition = m_pActiveIOBuffer->m_PosInMedia;
            }

            if ((m_pEndValidBytes) && (m_pFirstValidByte)) {
                bytesInBuffer = m_pEndValidBytes - m_pFirstValidByte;
            } else {
                bytesInBuffer = 0;
            }

            DEBUG_LOG_VERBOSE("CAsyncIOStream::Read. Call SetPosition for new position" INT64FMT,
                        currentPosition + bytesInBuffer);

            err = SetPosition(currentPosition + bytesInBuffer);
            if (err) {
                gotoErr(err);
            }

            // Be careful.
            // You can always seek to 0 in an empty stream, but you cannot read.
            if (0 == m_TotalAvailableBytes) {
                DEBUG_LOG_VERBOSE("CAsyncIOStream::SetPosition. Read an empty stream");
                gotoErr(EEOF);
            }
        } // reading the next buffer of data.
    } // while (bytesToRead > 0)

abort:
    returnErr(err);
} // Read





/////////////////////////////////////////////////////////////////////////////
//
// [Write]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOStream::Write(const char *clientBuffer, int32 bytesToWrite) {
    ErrVal err = ENoErr;
    AutoLock(m_pLock);
    RunChecks();

    DEBUG_LOG_VERBOSE("CAsyncIOStream::Write. bytesToWrite = %d",
                bytesToWrite);

    // The stream must have been initialized.
    if ((NULL == m_pBlockIO)
        || (bytesToWrite < 0)
        || (!clientBuffer)) {
        gotoErr(EFail);
    }

    // This happens when a stream has closed.
    if (NULL == m_pIOSystem) {
        gotoErr(EEOF);
    }

    if (m_pBlockIO->m_fSeekable) {
        err = WriteToSeekableDevice(clientBuffer, bytesToWrite);
    } else
    {
        err = WriteToStreamDevice(clientBuffer, bytesToWrite);
    }

abort:
    returnErr(err);
} // Write.







/////////////////////////////////////////////////////////////////////////////
//
// [WriteToSeekableDevice]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOStream::WriteToSeekableDevice(const char *clientBuffer, int32 bytesToWrite) {
    ErrVal err = ENoErr;
    int64 writePos;
    int32 bytesCopiedToCurrentBuffer;
    int32 numFreeBytes;


    if ((m_pActiveIOBuffer)
        && (m_pNextValidByte)
        && (m_pFirstValidByte)) {
        writePos = m_pActiveIOBuffer->m_PosInMedia + (m_pNextValidByte - m_pFirstValidByte);
    } else
    {
        writePos = 0;
    }

    DEBUG_LOG_VERBOSE("CAsyncIOStream::WriteToSeekableDevice. bytesToWrite = %d, writePos =" INT64FMT,
                bytesToWrite, writePos);

    // Each iteration of this loop writes some data.
    while (bytesToWrite > 0) {
        // If there is no current buffer, or we are at the end of
        // a buffer, then allocate a new empty buffer.
        if ((NULL == m_pFirstValidByte)
            || ((m_pNextValidByte)
                && (m_pLastPossibleValidByte)
                && (m_pNextValidByte >= m_pLastPossibleValidByte))) {
            // We are writing into the middle of the stream or at the end of the stream.
            // First, handle the case of writing into the middle of the stream.
            if ((writePos < GetDataLength())
                  && !(m_AsyncIOStreamFlags & EXPANDING_MEMORY_STREAM)) {
                err = SetPosition(writePos);
                if (err) {
                    gotoErr(err);
                }
            }
            // Now, handle the case of writing to the end of the stream.
            else {
                DEBUG_LOG_VERBOSE("CAsyncIOStream::WriteToSeekableDevice. Add a new buffer to end of stream. writePos =" INT64FMT,
                            writePos);

                // Save the current buffer.
                err = MoveBufferToBackground(m_pActiveIOBuffer);
                if (err) {
                    gotoErr(err);
                }

                // Allocate a new empty buffer.
                m_pActiveIOBuffer = AllocAsyncIOStreamBuffer(writePos, true);
                if (!m_pActiveIOBuffer) {
                     gotoErr(EFail);
                }

                // This will be adjusted for block alignment.
                writePos = m_pActiveIOBuffer->m_PosInMedia;

                m_pFirstValidByte = m_pActiveIOBuffer->m_pLogicalBuffer;
                m_pNextValidByte = m_pActiveIOBuffer->m_pLogicalBuffer;
                m_pEndValidBytes = m_pActiveIOBuffer->m_pLogicalBuffer;
                m_pLastPossibleValidByte = m_pActiveIOBuffer->m_pLogicalBuffer + m_pActiveIOBuffer->m_BufferSize;
            }
        } // allocating an empty buffer.

        // Now that we know this is the buffer we will be writing to,
        // record that we have written to it.
        m_pActiveIOBuffer->m_BufferFlags |= CIOBuffer::UNSAVED_CHANGES;
        m_pActiveIOBuffer->m_BufferFlags |= CIOBuffer::VALID_DATA;

        if ((m_pLastPossibleValidByte) && (m_pNextValidByte)) {
            numFreeBytes = m_pLastPossibleValidByte - m_pNextValidByte;
        } else {
            numFreeBytes = 0;
        }

        if ((numFreeBytes <= 0)
            || (NULL == m_pNextValidByte)
            || (NULL == m_pEndValidBytes)) {
            gotoErr(EFail);
        }

        bytesCopiedToCurrentBuffer = bytesToWrite;
        if (numFreeBytes < bytesCopiedToCurrentBuffer) {
            bytesCopiedToCurrentBuffer = numFreeBytes;
        }

        // Now, at this point we know there is enough room in the buffer
        // for the value. Copy the value to the buffer. We save it in
        // the buffer in the same format as it will appear in the file.
        memcpy(m_pNextValidByte, clientBuffer, bytesCopiedToCurrentBuffer);

        // The next value goes just after the value we just wrote.
        clientBuffer += bytesCopiedToCurrentBuffer;
        bytesToWrite = bytesToWrite - bytesCopiedToCurrentBuffer;
        writePos += bytesCopiedToCurrentBuffer;
        m_pNextValidByte += bytesCopiedToCurrentBuffer;

        // The data we just wrote is also readable.
        if (m_pNextValidByte > m_pEndValidBytes) {
            m_pEndValidBytes = m_pNextValidByte;
        }

        // Record the changes to the buffer.
        if (m_pActiveIOBuffer) {
            m_pActiveIOBuffer->m_NumValidBytes = m_pEndValidBytes - m_pFirstValidByte;
            if ((m_pActiveIOBuffer->m_PosInMedia + m_pActiveIOBuffer->m_NumValidBytes)
                     > m_TotalAvailableBytes) {
                  m_TotalAvailableBytes = m_pActiveIOBuffer->m_PosInMedia + m_pActiveIOBuffer->m_NumValidBytes;
            }
        }
    } // while (bytesToWrite > 0)

abort:
    returnErr(err);
} // WriteToSeekableDevice.







/////////////////////////////////////////////////////////////////////////////
//
// [WriteToStreamDevice]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOStream::WriteToStreamDevice(const char *clientBuffer, int32 bytesToWrite) {
    ErrVal err = ENoErr;
    int32 bytesCopiedToCurrentBuffer;

    DEBUG_LOG_VERBOSE("CAsyncIOStream::WriteToStreamDevice. bytesToWrite = %d", bytesToWrite);

    // Each iteration of this loop writes some data.
    while (bytesToWrite > 0) {
        // If there is no current buffer, or we are at the end of
        // a buffer, then allocate a new empty buffer.
        if ((!m_pFirstValidOutputByte)
            || ((m_pNextValidOutputByte)
                && (m_pLastPossibleValidOutputByte)
                && (m_pNextValidOutputByte >= m_pLastPossibleValidOutputByte))) {
            DEBUG_LOG_VERBOSE("CAsyncIOStream::WriteToStreamDevice. Add a new buffer to end of stream.");

            // Save the current buffer.
            err = MoveBufferToBackground(m_pActiveOutputIOBuffer);
            if (err) {
                gotoErr(err);
            }

            // Allocate a new empty buffer. It doesn't matter what
            // the position in the media is for a write to a PIPE.
            m_pActiveOutputIOBuffer = AllocAsyncIOStreamBuffer(0, false);
            if (!m_pActiveOutputIOBuffer) {
                gotoErr(EFail);
            }

            m_pFirstValidOutputByte = m_pActiveOutputIOBuffer->m_pLogicalBuffer;
            m_pNextValidOutputByte = m_pActiveOutputIOBuffer->m_pLogicalBuffer;
            m_pLastPossibleValidOutputByte = m_pActiveOutputIOBuffer->m_pPhysicalBuffer + m_pActiveOutputIOBuffer->m_BufferSize;
        } // allocating an empty buffer.

        // Now that we know this is the buffer we will be writing to,
        // record that we have written to it.
        m_pActiveOutputIOBuffer->m_BufferFlags |= CIOBuffer::UNSAVED_CHANGES;
        m_pActiveOutputIOBuffer->m_BufferFlags |= CIOBuffer::VALID_DATA;

        if ((m_pLastPossibleValidOutputByte)
            && (m_pNextValidOutputByte)) {
            bytesCopiedToCurrentBuffer = m_pLastPossibleValidOutputByte - m_pNextValidOutputByte;
        } else {
            bytesCopiedToCurrentBuffer = 0;
        }
        if ((bytesCopiedToCurrentBuffer <= 0) || (!m_pNextValidOutputByte)) {
            gotoErr(EFail);
        }
        if (bytesCopiedToCurrentBuffer >= bytesToWrite) {
            bytesCopiedToCurrentBuffer = bytesToWrite;
        }

        // Now, at this point we know there is enough room in the buffer
        // for the value. Copy the value to the buffer. We save it in
        // the buffer in the same format as it will appear in the file.
        memcpy(m_pNextValidOutputByte, clientBuffer, bytesCopiedToCurrentBuffer);

        // The next value goes just after the value we just wrote.
        clientBuffer += bytesCopiedToCurrentBuffer;
        m_pNextValidOutputByte += bytesCopiedToCurrentBuffer;
        bytesToWrite = bytesToWrite - bytesCopiedToCurrentBuffer;
        m_pActiveOutputIOBuffer->m_NumValidBytes = m_pNextValidOutputByte - m_pFirstValidOutputByte;
    } // while (bytesToWrite > 0)

abort:
    returnErr(err);
} // WriteToStreamDevice.








/////////////////////////////////////////////////////////////////////////////
//
// [RemoveNBytes]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOStream::RemoveNBytes(int64 startPos, int32 bytesToRemove) {
    ErrVal err = ENoErr;
    CIOBuffer *pBuffer;
    CIOBuffer *pNewBuffer = NULL;
    CIOBuffer *pNextBufferToCheck;
    int32 bytesToRemoveFromThisBuffer;
    int32 offset;
    int32 bytesInNewBuffer;
    AutoLock(m_pLock);
    RunChecks();

    DEBUG_LOG_VERBOSE("CAsyncIOStream::RemoveNBytes. startPos = " INT64FMT ", bytesToRemove = %d",
                startPos, bytesToRemove);

    // The stream must have been initialized.
    if ((NULL == m_pBlockIO)
        || (startPos < 0)
        || (bytesToRemove < 0)) {
        gotoErr(EFail);
    }

    err = MoveBufferToBackground(m_pActiveIOBuffer);
    if (err) {
        gotoErr(err);
    }

    pBuffer = m_IOBufferList.GetHead();
    while (pBuffer) {
        // We may delete this buffer, so get the next buffer now,
        // while the current buffer is still valid.
        pNextBufferToCheck = pBuffer->m_StreamBufferList.GetNextInQueue();

        // Adjust any buffers effected by this. There are several possible
        // different cases, depending on how the burrer overlaps with the
        // region we are removing.
        //
        // pBuffer:         [xxxxxxxxxxxxxxxxxxxxxxxxx]
        // Case 1: [.....]
        // Case 2: [....................]
        // Case 3: [.......................................]
        // Case 4:                [..............]
        // Case 5:                [........................]
        //
        if ((pBuffer->m_BufferFlags & CIOBuffer::INPUT_BUFFER)
            // Don't adjust a split buffer that we just created.
            && (pNewBuffer != pBuffer)
            && (startPos < (pBuffer->m_PosInMedia + pBuffer->m_NumValidBytes))) {
            ////////////////////////////////
            // Case 1.
            // Check if the range occurs entirely before the beginning
            // of the buffer. This is an easy case, just change the start position
            // of this buffer.
            if ((startPos + bytesToRemove) <= pBuffer->m_PosInMedia) {
                pBuffer->m_PosInMedia = pBuffer->m_PosInMedia - bytesToRemove;
            }
            ////////////////////////////////
            // Case 3 and Case 5.
            // Otherwise, check if the range partly overlaps the end of the buffer.
            // This is usually an easy case, just shorten the buffer. It may
            // span the entire buffer.
            else if ((startPos + bytesToRemove)
                        >= (pBuffer->m_PosInMedia + pBuffer->m_NumValidBytes)) {
                bytesToRemoveFromThisBuffer
                    = (int32) ((pBuffer->m_PosInMedia + pBuffer->m_NumValidBytes) - startPos);

                pBuffer->m_NumValidBytes = pBuffer->m_NumValidBytes - bytesToRemoveFromThisBuffer;
                // It may be 0 if the removed bytes cover this entire buffer and
                // part of at least one adjacent buffer.
                if (pBuffer->m_NumValidBytes <= 0) {
                    pBuffer->m_StreamBufferList.RemoveFromQueue();
                    // If this is waiting on an asynch operation, then
                    // let the operation finish before we close the file.
                    if ((CIOBuffer::READ == pBuffer->m_BufferOp)
                        || (CIOBuffer::WRITE == pBuffer->m_BufferOp))
                    {
                        pBuffer->m_BufferFlags |= CIOBuffer::DISCARD_WHEN_IDLE;
                    }
                    else
                    {
                        // Discard this block.
                        RELEASE_OBJECT(pBuffer);
                    }
                }
            } // The range overlaps the end of the buffer.
            ////////////////////////////////
            // Case 2.
            // Otherwise, check if the range overlaps the beginning of the buffer.
            // This is an easy case, just trim from the front of the buffer.
            else if (startPos <= pBuffer->m_PosInMedia) {
                bytesToRemoveFromThisBuffer
                    = (int32) ((startPos + bytesToRemove) - pBuffer->m_PosInMedia);

                pBuffer->m_NumValidBytes = pBuffer->m_NumValidBytes
                    - bytesToRemoveFromThisBuffer;

                pBuffer->m_PosInMedia = startPos;

                pBuffer->m_pLogicalBuffer += bytesToRemoveFromThisBuffer;
            } // Remove data from the front of the buffer.

            ////////////////////////////////
            // Case 4.
            // Otherwise, we have the worst case. Bytes are removed from
            // the middle of a buffer.
            else {
                bytesInNewBuffer
                    = (int32) ((pBuffer->m_PosInMedia + pBuffer->m_NumValidBytes)
                                    - (startPos + bytesToRemove));
                offset = (int32) ((startPos + bytesToRemove) - pBuffer->m_PosInMedia);

                // Allocate a new empty IO buffer for the data after the range.
                pNewBuffer = AllocAsyncIOStreamBuffer(bytesInNewBuffer, true);
                if (!pNewBuffer) {
                    gotoErr(EFail);
                }
                pNewBuffer->m_PosInMedia = startPos;
                pNewBuffer->m_NumValidBytes = bytesInNewBuffer;

                memcpy(
                    pNewBuffer->m_pLogicalBuffer,
                    pBuffer->m_pLogicalBuffer + offset,
                    pNewBuffer->m_NumValidBytes);

                // Now, remove the data from the first buffer.
                pBuffer->m_NumValidBytes = (int32) (startPos - pBuffer->m_PosInMedia);
            } // Remove data from the middle of the buffer.
        } // // Adjust any buffers effected by this.

        pBuffer = pNextBufferToCheck;
    } // Updating all buffers.


    m_pActiveIOBuffer = NULL;
    m_pFirstValidByte = NULL;
    m_pNextValidByte = NULL;
    m_pEndValidBytes = NULL;
    m_pLastPossibleValidByte = NULL;

    m_TotalAvailableBytes = m_TotalAvailableBytes - bytesToRemove;

    m_NextLoadStartPosition    = m_NextLoadStartPosition - bytesToRemove;
    m_NextAsynchBufferPosition = m_NextAsynchBufferPosition - bytesToRemove;

    if (m_pBlockIO) {
        err = m_pBlockIO->RemoveNBytes(bytesToRemove);
        if (err) {
            gotoErr(err);
        }
    }

abort:
    returnErr(err);
} // RemoveNBytes.









/////////////////////////////////////////////////////////////////////////////
//
// [CopyStream]
//
// This writes without copying. The reader (the source of the
// copy) retains the blocks, so they do not appear in the cache
// of the writer.
//
// Read may return an error if the server suddenly disconnects
// due to a crash.
//
// Write may return an error, if the remote client closes the
// connection. That happens when we return an error response
// or an old object that the client already has in cache.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOStream::CopyStream(
                  CAsyncIOStream *destStream,
                  int64 numBytesToCopy,
                  bool fTransferOwnerShip) {
    ErrVal err = ENoErr;
    ErrVal err2 = ENoErr;
    int64 srcStartPos;
    int32 numBytes;
    CIOBuffer *pBuffer;
    int32 startOffsetInBuffer;
    RunChecks();

    DEBUG_LOG_VERBOSE("CAsyncIOStream::CopyStream. numBytesToCopy = " INT64FMT ", fTransferOwnerShip = %d",
                numBytesToCopy, fTransferOwnerShip);

    if ((!destStream)
        || !(destStream->m_pLock)
        || !(destStream->m_pBlockIO)
        || (numBytesToCopy < 0)) {
        returnErr(EFail);
    }

    m_pLock->Lock();
    destStream->m_pLock->Lock();

    // Save where we left off so we can restore our position.
    srcStartPos = GetPosition();

    DEBUG_LOG_VERBOSE("CAsyncIOStream::CopyStream. srcStartPos = " INT64FMT, srcStartPos);

    // The user can specify the number of bytes to copy, or else
    // we just copy from here to the end of the stream.
    if (CAsyncIOStream::COPY_TO_EOF == numBytesToCopy) {
        numBytesToCopy = GetDataLength() - srcStartPos;
    }
    DEBUG_LOG_VERBOSE("CAsyncIOStream::CopyStream. numBytesToCopy = %d", numBytesToCopy);

    if (fTransferOwnerShip) {
        // Write any unsaved output buffer.
        err = MoveBufferToBackground(m_pActiveIOBuffer);
        if (err) {
            gotoErr(err);
        }
        err = destStream->MoveBufferToBackground(destStream->m_pActiveIOBuffer);
        if (err) {
            gotoErr(err);
        }
        err = MoveBufferToBackground(m_pActiveOutputIOBuffer);
        if (err) {
            gotoErr(err);
        }
        err = destStream->MoveBufferToBackground(destStream->m_pActiveOutputIOBuffer);
        if (err) {
            gotoErr(err);
        }

        // We are writing data without buffering, so
        // remove any previously accumulated buffers.
        destStream->m_pActiveIOBuffer = NULL;
        destStream->m_pFirstValidByte = NULL;
        destStream->m_pNextValidByte = NULL;
        destStream->m_pEndValidBytes = NULL;
        destStream->m_pLastPossibleValidByte = NULL;

        destStream->m_pActiveOutputIOBuffer = NULL;
        destStream->m_pFirstValidOutputByte = NULL;
        destStream->m_pNextValidOutputByte = NULL;
        destStream->m_pLastPossibleValidOutputByte = NULL;
        destStream->m_TotalAvailableBytes = 0;
    }

    // Each loop transfers from one buffer.
    while (numBytesToCopy > 0) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::CopyStream. srcStartPos = " INT64FMT, srcStartPos);
        err = SetPosition(srcStartPos);
        if (err) {
            if (EEOF == err) {
                err = ENoErr;
            }
            gotoErr(err);
        }

        pBuffer = m_pActiveIOBuffer;
        if (fTransferOwnerShip) {
            // Remove the buffer from the source stream.
            m_IOBufferList.RemoveFromQueue(&(pBuffer->m_StreamBufferList));
            m_pActiveIOBuffer = NULL;
            m_pFirstValidByte = NULL;
            m_pNextValidByte = NULL;
            m_pEndValidBytes = NULL;
            m_pLastPossibleValidByte = NULL;
            m_pActiveOutputIOBuffer = NULL;
            m_pFirstValidOutputByte = NULL;
            m_pNextValidOutputByte = NULL;
            m_pLastPossibleValidOutputByte = NULL;

            // Insert the buffer into this stream. We own the buffer now.
            destStream->m_IOBufferList.InsertHead(&(pBuffer->m_StreamBufferList));

            // Find out how many bytes are available in this buffer.
            startOffsetInBuffer = (int32) (srcStartPos - pBuffer->m_PosInMedia);
            pBuffer->m_BufferFlags |= CIOBuffer::VALID_DATA;
            pBuffer->m_BufferFlags |= CIOBuffer::OUTPUT_BUFFER;

            destStream->m_pBlockIO->WriteBlockAsync(pBuffer, startOffsetInBuffer);
        } // if (fTransferOwnerShip)
        else {
            if ((m_pEndValidBytes) && (m_pNextValidByte)) {
                numBytes = m_pEndValidBytes - m_pNextValidByte;
            } else {
                break;
            }
            if (numBytes > numBytesToCopy) {
                numBytes = (int32) numBytesToCopy;
            }

            DEBUG_LOG_VERBOSE("CAsyncIOStream::CopyStream. Writing bytes, numBytes = %d", numBytes);
            err = destStream->Write(m_pNextValidByte, numBytes);
            if (err) {
                gotoErr(err);
            }

            if (m_pNextValidByte) {
                m_pNextValidByte += numBytes;
            }
        } // if (!fTransferOwnerShip)

        srcStartPos += numBytes;
        numBytesToCopy = numBytesToCopy - numBytes;
    } // while (numBytesToCopy > 0)

abort:
    // Restore the position in the stream.
    DEBUG_LOG_VERBOSE("CAsyncIOStream::CopyStream. srcStartPos = " INT64FMT, srcStartPos);
    err2 = SetPosition(srcStartPos);
    if ((!err) && (err2)) {
        if (EEOF == err2) {
            err2 = ENoErr;
        }
        err = err2;
    }

    m_pLock->Unlock();
    destStream->m_pLock->Unlock();

    returnErr(err);
} // CopyStream.







/////////////////////////////////////////////////////////////////////////////
//
// [ListenForNBytes]
//
/////////////////////////////////////////////////////////////////////////////
void
CAsyncIOStream::ListenForNBytes(int64 startPos, int64 nBytes) {
    ErrVal err = ENoErr;
    int64 bytesAvalilableNow;
    AutoLock(m_pLock);
    RunChecks();

    DEBUG_LOG_VERBOSE("CAsyncIOStream::ListenForNBytes. startPos = " INT64FMT ", nBytes = " INT64FMT,
                startPos, nBytes);

    // The stream must have been initialized.
    if ((NULL == m_pBlockIO) || (nBytes < 0)) {
        gotoErr(EFail);
    }
    if (startPos < 0) {
        startPos = m_NextLoadStartPosition;
        DEBUG_LOG_VERBOSE("CAsyncIOStream::ListenForNBytes. startPos = " INT64FMT ", nBytes = " INT64FMT,
                    startPos, nBytes);
    }

    // If this stream is in the middle of an asynch op, then we cannot
    // start a new one.
    if (NOT_LOADING != m_AsynchLoadType) {
        gotoErr(EFail);
    }

    m_AsynchLoadType = ASYNCH_LOAD_NBYTES;
    m_LoadStartPosition = startPos;
    m_LoadStopPosition = startPos + nBytes;
    DEBUG_LOG_VERBOSE("CAsyncIOStream::ListenForNBytes. m_LoadStopPosition = " INT64FMT,
                m_LoadStopPosition);

    if ((m_AsyncIOStreamFlags & ALL_DATA_IS_IN_BUFFERS)
       || (m_AsyncIOStreamFlags & EXPANDING_MEMORY_STREAM)) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::ListenForNBytes. ALL_DATA_IS_IN_BUFFERS");
        FinishAsyncLoad(ENoErr);
        goto abort;
    }

    // If we have all the data we need, then we are done.
    bytesAvalilableNow = GetDataLength();
    if (bytesAvalilableNow >= m_LoadStopPosition) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::ListenForNBytes. All bytes are already available. bytesAvalilableNow = " INT64FMT,
                    bytesAvalilableNow);
        FinishAsyncLoad(ENoErr);
        goto abort;
    }

    ContinueAsyncLoad(ENoErr, false);

abort:
    if (err) {
        FinishAsyncLoad(err);
    }
} // ListenForNBytes.







/////////////////////////////////////////////////////////////////////////////
//
// [ListenForMoreBytes]
//
/////////////////////////////////////////////////////////////////////////////
void
CAsyncIOStream::ListenForMoreBytes() {
    ErrVal err = ENoErr;
    int64 bytesAvalilableNow;
    AutoLock(m_pLock);
    RunChecks();

    DEBUG_LOG_VERBOSE("CAsyncIOStream::ListenForMoreBytes");

    // The stream must have been initialized.
    if (NULL == m_pBlockIO) {
        gotoErr(EFail);
    }
    // If this stream is in the middle of an asynch op, then don't
    // start a new one. This is not an error case.
    // The KeepAlives may call this twice, once when the KeepAlive
    // is set, and again when a read with keepAlive is completed. Neither
    // can assume the other will always run, so both must run to be safe.
    // So, don't freak out if the other has already started the next read.
    if (ASYNCH_LOAD_ANY_BYTES == m_AsynchLoadType) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::ListenForMoreBytes. ASYNCH_LOAD_ANY_BYTES");
        gotoErr(ENoErr);
    } else if ((ASYNCH_LOAD_ANY_BYTES != m_AsynchLoadType)
            && (NOT_LOADING != m_AsynchLoadType)) {
        gotoErr(EFail);
    }

    m_AsynchLoadType = ASYNCH_LOAD_ANY_BYTES;
    m_LoadStartPosition = m_NextLoadStartPosition;
    m_LoadStopPosition = -1;

    if ((m_AsyncIOStreamFlags & ALL_DATA_IS_IN_BUFFERS)
       || (m_AsyncIOStreamFlags & EXPANDING_MEMORY_STREAM)) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::ListenForNBytes. ALL_DATA_IS_IN_BUFFERS");
        FinishAsyncLoad(ENoErr);
        goto abort;
    }

    bytesAvalilableNow = GetDataLength();
    if (bytesAvalilableNow > m_LoadStartPosition) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::ListenForMoreBytes. All bytes are already available. bytesAvalilableNow = " INT64FMT,
                    bytesAvalilableNow);
        FinishAsyncLoad(ENoErr);
    } else {
        ContinueAsyncLoad(ENoErr, false);
    }

abort:
    if (err) {
        FinishAsyncLoad(err);
    }
} // ListenForMoreBytes.






/////////////////////////////////////////////////////////////////////////////
//
// [ListenForAllBytesToEOF]
//
/////////////////////////////////////////////////////////////////////////////
void
CAsyncIOStream::ListenForAllBytesToEOF() {
    ErrVal err = ENoErr;
    AutoLock(m_pLock);
    RunChecks();

    DEBUG_LOG_VERBOSE("CAsyncIOStream::ListenForAllBytesToEOF");

    if (NULL == m_pBlockIO) {
        gotoErr(EFail);
    }
    // If this stream is in the middle of an asynch op, then we cannot
    // start a new one.
    if (NOT_LOADING != m_AsynchLoadType) {
        gotoErr(EFail);
    }

    m_AsynchLoadType = ASYNCH_LOAD_TO_EOF;
    m_LoadStartPosition = m_NextLoadStartPosition;
    m_LoadStopPosition = -1;

    if ((m_AsyncIOStreamFlags & ALL_DATA_IS_IN_BUFFERS)
       || (m_AsyncIOStreamFlags & EXPANDING_MEMORY_STREAM)) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::ListenForAllBytesToEOF. ALL_DATA_IS_IN_BUFFERS");
        FinishAsyncLoad(ENoErr);
        goto abort;
    }

    ContinueAsyncLoad(ENoErr, false);

abort:
    if (err) {
        FinishAsyncLoad(err);
    }
} // ListenForAllBytesToEOF.







/////////////////////////////////////////////////////////////////////////////
//
// [ContinueAsyncLoad]
//
// This method cannot hold the lock while it calls a callback, since that may
// cause a deadlock. The AsyncIOStream object and the callback object may have
// separate locks. The callback object may run at any time, such as in response
// to a user action or a timer, or an I/O event on another data stream. This
// method and a few others are indirectly called by the selectThread, so they
// start a new call-chain. The callback object must be able to hold its lock while
// calling the AsyncIOStream, so the AsyncIOStream cannot hold its lock while calling
// the callback.
//
// This only effects OnBlockIOEvent, ContinueAsyncLoad and FinishAsyncLoad
// since these are the only methods that are called indirectly
// by the select thread.
/////////////////////////////////////////////////////////////////////////////
void
CAsyncIOStream::ContinueAsyncLoad(ErrVal resultErr, bool fReceivedNewData) {
    ErrVal err = ENoErr;
    int64 numAvailBytes;
    CIOBuffer *pBuffer = NULL;
    ErrVal finishAsyncLoadErr = ENoErr;
    bool fCallFinishAsyncLoad = false;

    DEBUG_LOG_VERBOSE("CAsyncIOStream::ContinueAsyncLoad: resultErr = %d, fReceivedNewData = %d",
            resultErr, fReceivedNewData);

    ////////////////////////////////////////////////
    {
        AutoLock(m_pLock);

        // Run any standard debugger checks.
        RunChecks();

        // The stream must have been initialized.
        if ((NULL == m_pBlockIO) || (NOT_LOADING == m_AsynchLoadType)) {
            gotoErr(EFail);
        }
        if (resultErr) {
            fCallFinishAsyncLoad = true;
            finishAsyncLoadErr = resultErr;
            fReceivedNewData = false;
            goto finishOutsideCritSec;
        }

        numAvailBytes = GetDataLength();
        DEBUG_LOG_VERBOSE("CAsyncIOStream::ContinueAsyncLoad: numAvailBytes = %d", numAvailBytes);
        DEBUG_LOG_VERBOSE("CAsyncIOStream::ContinueAsyncLoad: m_LoadStartPosition = %d", m_LoadStartPosition);
        DEBUG_LOG_VERBOSE("CAsyncIOStream::ContinueAsyncLoad: m_AsynchLoadType = %d", m_AsynchLoadType);

        if (((ASYNCH_LOAD_ANY_BYTES == m_AsynchLoadType)
                && (numAvailBytes > m_LoadStartPosition))
            || ((ASYNCH_LOAD_NBYTES == m_AsynchLoadType)
                && (numAvailBytes >= m_LoadStopPosition))) {
            DEBUG_LOG_VERBOSE("CAsyncIOStream::ContinueAsyncLoad: Call FinishAsyncLoad");

            fCallFinishAsyncLoad = true;
            finishAsyncLoadErr = ENoErr;
            fReceivedNewData = true;
            goto finishOutsideCritSec;
        }

        // This check is important. If we read a partial block from a file
        // then the next read may be rounded down to a previous block.
        // As a result, we could read the last block several times.
        // This check avoids that.
        if ((m_pBlockIO->m_fSeekable)
            && (numAvailBytes >= m_pBlockIO->GetMediaSize())) {
            DEBUG_LOG_VERBOSE("CAsyncIOStream::ContinueAsyncLoad: Call FinishAsyncLoad");

            fCallFinishAsyncLoad = true;
            finishAsyncLoadErr = ENoErr;
            fReceivedNewData = true;
            goto finishOutsideCritSec;
        }

        // At this point, we have decided that we need more data.
        DEBUG_LOG_VERBOSE("CAsyncIOStream::ContinueAsyncLoad: we need more data");

        // If this is a stream (non-seekable) blockIO, then buffers will
        // be allocated by the blockIO when they arrive. If this
        // is a seekable, however, then we need to request the read.
        if (m_pBlockIO->m_fSeekable) {
            // Allocate a new empty IO buffer.
            pBuffer = AllocAsyncIOStreamBuffer(numAvailBytes, true);
            if (!pBuffer) {
                gotoErr(EFail);
            }
            pBuffer->m_PosInMedia = m_NextAsynchBufferPosition;

            // Now, fill the new buffer. We always do this if we are
            // reading, or if we are either reading or writing and
            // there is data in the backing store.
            DEBUG_LOG_VERBOSE("CAsyncIOStream::ContinueAsyncLoad: Call ReadBlockAsync. pos = %d", m_NextAsynchBufferPosition);
            m_pBlockIO->ReadBlockAsync(pBuffer);
        }
        // Otherwise, if this is a stream (non-seekable) blockIO, then
        // new data may never arrive. Start a timeout, which will trigger
        // an I/O error if data does not arrive in a reasonable time.
        else { // if (!(m_pBlockIO->m_fSeekable))
            DEBUG_LOG_VERBOSE("CAsyncIOStream::ContinueAsyncLoad: Call StartTimeout");
            m_pBlockIO->StartTimeout(CIOBuffer::READ);
        }
    } ////////////////////////////////////////////////

finishOutsideCritSec:
    if (fCallFinishAsyncLoad) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::ContinueAsyncLoad: Calling FinishAsyncLoad");
        FinishAsyncLoad(finishAsyncLoadErr);
    }
    return;

abort:
    if (pBuffer) {
        pBuffer->m_StreamBufferList.RemoveFromQueue();
        RELEASE_OBJECT(pBuffer);
    }

    // This is actually a success case.
    if ((EEOF == err)
        && ((ASYNCH_LOAD_TO_EOF == m_AsynchLoadType)
            || (ASYNCH_LOAD_ANY_BYTES == m_AsynchLoadType))) {
        err = ENoErr;
    }

    FinishAsyncLoad(err);
} // ContinueAsyncLoad.







/////////////////////////////////////////////////////////////////////////////
//
// [FinishAsyncLoad]
//
// This method cannot hold the lock while it calls a callback, since that may
// cause a deadlock. The AsyncIOStream object and the callback object may have
// separate locks. The callback object may run at any time, such as in response
// to a user action or a timer, or an I/O event on another data stream. This
// method and a few others are indirectly called by the selectThread, so they
// start a new call-chain. The callback object must be able to hold its lock while
// calling the AsyncIOStream, so the AsyncIOStream cannot hold its lock while calling
// the callback.
//
// This only effects OnBlockIOEvent, ContinueAsyncLoad and FinishAsyncLoad
// since these are the only methods that are called by the select thread.
/////////////////////////////////////////////////////////////////////////////
void
CAsyncIOStream::FinishAsyncLoad(ErrVal resultErr) {
    int64 totalAvailBytes;
    int32 oldAsyncLoadType;
    CAsyncIOEventHandler *pEventHandler = NULL;

    DEBUG_LOG_VERBOSE("CAsyncIOStream::FinishAsyncLoad. resultErr= %d", resultErr);

    ///////////////////////////////////////////////////////
    {
        AutoLock(m_pLock);

        // Run any standard debugger checks.
        RunChecks();

        totalAvailBytes = GetDataLength();

        DEBUG_LOG_VERBOSE("CAsyncIOStream::FinishAsyncLoad. totalAvailBytes = %d, m_AsynchLoadType = %d",
                    totalAvailBytes, m_AsynchLoadType);

        // Update the state before calling the callback; the callback may
        // re-entrantly start a new load.
        oldAsyncLoadType = m_AsynchLoadType;
        m_AsynchLoadType = NOT_LOADING;

        if (!resultErr) {
            // Get the returned data.
            if ((ASYNCH_LOAD_NBYTES == oldAsyncLoadType)
                || (ASYNCH_LOAD_ANY_BYTES == oldAsyncLoadType)) {
                DEBUG_LOG_VERBOSE("CAsyncIOStream::FinishAsyncLoad. m_LoadStartPosition= " INT64FMT,
                            m_LoadStartPosition);

                (void) SetPosition(m_LoadStartPosition);
            }
            // Advance the cursor.
            if (ASYNCH_LOAD_NBYTES == oldAsyncLoadType) {
                m_NextLoadStartPosition = m_LoadStopPosition;
            } else if (ASYNCH_LOAD_ANY_BYTES == oldAsyncLoadType) {
                m_NextLoadStartPosition = totalAvailBytes;
            } else if (ASYNCH_LOAD_TO_EOF == oldAsyncLoadType) {
                m_NextLoadStartPosition = totalAvailBytes;
            }

            DEBUG_LOG_VERBOSE("CAsyncIOStream::FinishAsyncLoad. m_NextLoadStartPosition= " INT64FMT,
                       m_NextLoadStartPosition);
        } // if (!resultErr)

        // Prepare to call the callback.
        if (NULL != m_pEventHandler) {
            if ((ASYNCH_LOAD_NBYTES == oldAsyncLoadType)
                || (ASYNCH_LOAD_ANY_BYTES == oldAsyncLoadType)
                || (ASYNCH_LOAD_TO_EOF == oldAsyncLoadType)) {
                pEventHandler = m_pEventHandler;
                ADDREF_OBJECT(m_pEventHandler);
            }
        }
    } /////////////////////////////////////////////////


    // Call the callback outside the lock.
    if (NULL != pEventHandler) {
        pEventHandler->OnReadyToRead(
                              resultErr,
                              totalAvailBytes,
                              this,
                              m_pEventHandlerContext);
        RELEASE_OBJECT(pEventHandler);
    }
} // FinishAsyncLoad.






/////////////////////////////////////////////////////////////////////////////
//
// [OnBlockIOEvent]
//
// This is called when a blockIO completes a read or a write.
// It is part of the CAsyncBlockIOCallback base class.
//
// This method cannot hold the lock while it calls a callback, since that may
// cause a deadlock. The AsyncIOStream object and the callback object may have
// separate locks. The callback object may run at any time, such as in response
// to a user action or a timer, or an I/O event on another data stream. This
// method and a few others are indirectly called by the selectThread, so they
// start a new call-chain. The callback object must be able to hold its lock while
// calling the AsyncIOStream, so the AsyncIOStream cannot hold its lock while calling
// the callback.
//
// This only effects OnBlockIOEvent, ContinueAsyncLoad and FinishAsyncLoad
// since these are the only methods that are called by the select thread.
/////////////////////////////////////////////////////////////////////////////
void
CAsyncIOStream::OnBlockIOEvent(CIOBuffer *pBuffer) {
    ErrVal err = ENoErr;
    ErrVal bufferErr = ENoErr;
    ErrVal continueAsyncLoadErr = ENoErr;
    ErrVal finishAsyncLoadErr = ENoErr;
    ErrVal eventErr = ENoErr;
    bool fCallContinueAsyncLoad = false;
    bool fCallFinishAsyncLoad = false;
    bool fCallOnFlush = false;
    bool fCallOnError = false;
    CIOBuffer *pPrevBuffer = NULL;

    DEBUG_LOG_VERBOSE("CAsyncIOStream::OnBlockIOEvent: this = %p, pBuffer = %p.", this, pBuffer);

    ///////////////////////////////////////////////////
    {
        AutoLock(m_pLock);

        if (NULL == pBuffer) {
            gotoErr(ENoErr);
        }

        // A handy place to put a breakpoint.
#if DD_DEBUG
        if (pBuffer->m_Err) {
            pBuffer = pBuffer;
        }
#endif // DD_DEBUG

        // Check if we are closed; this can happen if
        // a read comes in after we have times out and closed
        // a connection.
        if ((NULL == m_pBlockIO) || (NULL == m_pIOSystem)) {
            // We don't have to release this; the AsyncIOStream
            // is shutdown, so it has not addRef'ed the buffer.
            DEBUG_LOG_VERBOSE("CAsyncIOStream::OnBlockIOEvent: NULL == m_pBlockIO");
            gotoErr(ENoErr);
        } // discarding the buffer.

        // Run any standard debugger checks.
        // Do this AFTER we check whether we are closed, since
        // it treats closure as an error.
        RunChecks();


        // Some operations, like a Write(), do not need to
        // hold onto the buffer. In this case, discard the buffer as
        // soon as the IO operation completes.
        if (pBuffer->m_BufferFlags & CIOBuffer::DISCARD_WHEN_IDLE) {
            DEBUG_LOG_VERBOSE("CAsyncIOStream::OnBlockIOEvent: DISCARD_WHEN_IDLE");

            if (m_pBlockIO->m_fSeekable) {
                pBuffer->m_StreamBufferList.RemoveFromQueue();
                RELEASE_OBJECT(pBuffer);
            }

            if (m_pActiveIOBuffer == pBuffer) {
                m_pActiveIOBuffer = NULL;
                m_pFirstValidByte = NULL;
                m_pNextValidByte = NULL;
                m_pEndValidBytes = NULL;
                m_pLastPossibleValidByte = NULL;
            }

            gotoErr(ENoErr);
        } // discarding the buffer.


        //////////////////////////////////////////////
        // A read completed.
        if (CIOBuffer::READ == pBuffer->m_BufferOp) {
            DEBUG_LOG_VERBOSE("CAsyncIOStream::OnBlockIOEvent: Finished a read. err = %d", pBuffer->m_Err);
            DEBUG_LOG_VERBOSE("CAsyncIOStream::OnBlockIOEvent: pBuffer->m_NumValidBytes = %d", pBuffer->m_NumValidBytes);

            // A read error on a network means the peer disconnected.
            // Report this as a special case.
            if ((pBuffer->m_Err)
               && (CAsyncBlockIO::NETWORK_MEDIA == m_pBlockIO->m_MediaType)
               && !(m_AsyncIOStreamFlags & UDP_SERVER_STREAM)) {
                fCallOnError = true;
                eventErr = pBuffer->m_Err;
            }

            // If this is a stream blockIO, then buffers will be allocated
            // by the block IO when they are available. So, we have to add some
            // of the additional flags that are normally set by this module when
            // we allocate the buffers.
            if (!(m_pBlockIO->m_fSeekable)) {
                pBuffer->m_BufferFlags |= CIOBuffer::INPUT_BUFFER;

                // If this is just an error, we don't need to keep the buffer.
                if (!(pBuffer->m_Err)) {
                    // Keep m_IOBufferList in LRU order. New buffers are the most recently
                    // used, so they are added to the head of the list.
                    m_IOBufferList.InsertHead(&(pBuffer->m_StreamBufferList));
                    ADDREF_OBJECT(pBuffer);
                }

                m_pBlockIO->CancelTimeout(CIOBuffer::READ);
            } // (!(m_pBlockIO->m_fSeekable))

            pBuffer->m_BufferOp = CIOBuffer::NO_OP;

            // If we just read a UDP buffer, then create a new data stream
            // for this new data.
            if (m_AsyncIOStreamFlags & UDP_SERVER_STREAM) {
                DEBUG_LOG_VERBOSE("CAsyncIOStream::OnBlockIOEvent: Finished a UDP read.");
                //OnUDPRead(pBuffer);
                gotoErr(ENoErr);
            }

            if (pBuffer->m_NumValidBytes > 0) {
                // If this is a stream device, then adjust m_PosInMedia.
                // It was set in the select thread, which can't grab the stream's lock.
                // If this was set at the same time that we were removing bytes,
                // then it is not accurate.
                if (!(m_pBlockIO->m_fSeekable)) {
                    pBuffer->m_PosInMedia = m_TotalAvailableBytes;
                    m_TotalAvailableBytes += pBuffer->m_NumValidBytes;

                    DEBUG_LOG_VERBOSE("CAsyncIOStream::OnBlockIOEvent: New m_TotalAvailableBytes = %d", m_TotalAvailableBytes);
                    DEBUG_LOG_VERBOSE("CAsyncIOStream::OnBlockIOEvent: pBuffer->m_PosInMedia = %d", pBuffer->m_PosInMedia);
                }

                if (pBuffer == m_pActiveIOBuffer) {
                    m_pEndValidBytes = m_pFirstValidByte + pBuffer->m_NumValidBytes;
                }
            } // (pBuffer->m_NumValidBytes > 0)


            // If we are currently waiting for more readable data, then this may provide
            // enough data and we can stop waiting.
            if (NOT_LOADING != m_AsynchLoadType) {
                DEBUG_LOG_VERBOSE("CAsyncIOStream::OnBlockIOEvent: We are currently loading new bytes");

                if (pBuffer->m_NumValidBytes > 0) {
                    m_NextAsynchBufferPosition += pBuffer->m_NumValidBytes;
                }

                if (pBuffer->m_Err) {
                    fCallFinishAsyncLoad = true;
                    finishAsyncLoadErr = pBuffer->m_Err;
                } else if (pBuffer->m_NumValidBytes > 0) {
                    fCallContinueAsyncLoad = true;
                    continueAsyncLoadErr = pBuffer->m_Err;
                }
            } // (NOT_LOADING != m_AsynchLoadType)

            // Some network connections may be slow. If this is the case,
            // then check if we can combine this buffer with the previous buffer.
            if ((!(m_pBlockIO->m_fSeekable))
                && (!(pBuffer->m_Err))
                && (pBuffer->m_NumValidBytes < MIN_REASONABLE_NETWORK_PACKET)) {
                // Look for the previous buffer.
                pPrevBuffer = m_IOBufferList.GetHead();
                while (pPrevBuffer) {
                    if ((pPrevBuffer->m_PosInMedia + pPrevBuffer->m_NumValidBytes)
                            == pBuffer->m_PosInMedia)
                    {
                        break;
                    }
                    pPrevBuffer = pPrevBuffer->m_StreamBufferList.GetNextInQueue();
                } // Look for the previous buffer.

                // Check if we found a previous buffer that is small enough
                // to fit into the new buffer.
                if (NULL != pPrevBuffer) {
                    int32 offset = pPrevBuffer->m_pLogicalBuffer - pPrevBuffer->m_pPhysicalBuffer;

                    if ((offset + pPrevBuffer->m_NumValidBytes + pBuffer->m_NumValidBytes)
                        < (pPrevBuffer->m_BufferSize))
                    {
                        DEBUG_LOG_VERBOSE("CAsyncIOStream::OnBlockIOEvent: Combine adjacent buffers");

                        // Copy the new buffer into the previous buffer.
                        memcpy(
                            pPrevBuffer->m_pLogicalBuffer + pPrevBuffer->m_NumValidBytes,
                            pBuffer->m_pLogicalBuffer,
                            pBuffer->m_NumValidBytes);
                        pPrevBuffer->m_NumValidBytes += pBuffer->m_NumValidBytes;

                        // Discard the new buffer.
                        pBuffer->m_StreamBufferList.RemoveFromQueue();
                        RELEASE_OBJECT(pBuffer);

                        pBuffer = pPrevBuffer;
                        if (m_pActiveIOBuffer == pBuffer)
                        {
                            m_pEndValidBytes = m_pFirstValidByte + m_pActiveIOBuffer->m_NumValidBytes;
                        }

                        // Keep m_IOBufferList in LRU order. If we are touching this buffer, then
                        // move it to the head of the list.
                        pPrevBuffer->m_StreamBufferList.RemoveFromQueue();
                        m_IOBufferList.InsertHead(&(pPrevBuffer->m_StreamBufferList));
                    }
                    // If the new is too large to fit in the prev, then the
                    // prev will be too large to fit in the new. Both the
                    // new and the prev are normally the same size.
                } // if (NULL != pPrevBuffer)
            } // if (pBuffer->m_NumValidBytes < MIN_REASONABLE_NETWORK_PACKET)

            gotoErr(ENoErr);
        } // (CIOBuffer::READ == pBuffer->m_BufferOp)


        //////////////////////////////////////////////
        // A write completed.
        if (CIOBuffer::WRITE == pBuffer->m_BufferOp) {
            DEBUG_LOG_VERBOSE("CAsyncIOStream::OnBlockIOEvent: Finished a write. err = %d", pBuffer->m_Err);

            pBuffer->m_BufferOp = CIOBuffer::NO_OP;
            bufferErr = pBuffer->m_Err;

            // If this is a stream, then we are done with write buffers as soon
            // as they are complete. We do not read data from these buffers, so
            // we no longer need them.
            if (!(m_pBlockIO->m_fSeekable)) {
                pBuffer->m_StreamBufferList.RemoveFromQueue();
                RELEASE_OBJECT(pBuffer);
            }

            if (m_pActiveIOBuffer == pBuffer) {
                m_pActiveIOBuffer = NULL;
                m_pFirstValidByte = NULL;
                m_pNextValidByte = NULL;
                m_pEndValidBytes = NULL;
                m_pLastPossibleValidByte = NULL;
            }

            if ((m_AsyncIOStreamFlags & FLUSHING)
               || (m_AsyncIOStreamFlags & WAITING_ON_FLUSH)) {
                m_NumFlushWrites--;
                DEBUG_LOG_VERBOSE("CAsyncIOStream::OnBlockIOEvent: New m_NumFlushWrites = %d", m_NumFlushWrites);

                if (bufferErr) {
                    m_FlushErr = bufferErr;
                }

                // Check if there are any buffers still being written.
                // If no blocks are being written, then the flush is done.
                if ((m_NumFlushWrites <= 0) && (m_AsyncIOStreamFlags & WAITING_ON_FLUSH)) {
                    DEBUG_LOG_VERBOSE("CAsyncIOStream::OnBlockIOEvent: Call flush");
                    fCallOnFlush = true;
                    gotoErr(err);
                } // (NULL == pBuffer)
            } // (m_AsyncIOStreamFlags & FLUSHING)
        } // (CIOBuffer::WRITE == pBuffer->m_BufferOp)
    } ///////////////////////////////////////////////////////

abort:
    if (fCallContinueAsyncLoad) {
        ContinueAsyncLoad(continueAsyncLoadErr, true);
    }
    if (fCallFinishAsyncLoad) {
        FinishAsyncLoad(finishAsyncLoadErr);
    }
    if (fCallOnFlush) {
        FinishFlush();
    }
    if (fCallOnError) {
        AutoLock(m_pLock);
        if (NULL != m_pEventHandler) {
            CAsyncIOEventHandler *pEventHandler = m_pEventHandler;
            ADDREF_OBJECT(pEventHandler);
            pEventHandler->OnStreamDisconnect(eventErr, this, m_pEventHandlerContext);
            RELEASE_OBJECT(pEventHandler);
        } // (NULL != m_pEventHandler)
    }
} // OnBlockIOEvent.







/////////////////////////////////////////////////////////////////////////////
//
// [OnBlockIOOpen]
//
// CAsyncBlockIOCallback
/////////////////////////////////////////////////////////////////////////////
void
CAsyncIOStream::OnBlockIOOpen(ErrVal resultErr, CAsyncBlockIO *pBlockIO) {
    DEBUG_LOG_VERBOSE("CAsyncIOStream::OnBlockIOOpen, err = %d, blockIO = %p", resultErr, pBlockIO);

    RELEASE_OBJECT(m_pBlockIO);
    m_pBlockIO = pBlockIO;
    ADDREF_OBJECT(m_pBlockIO);

    if (!resultErr) {
        FinishOpenCommand();
    }

    if (m_pEventHandler) {
        CAsyncIOEventHandler *pEventHandler = m_pEventHandler;
        ADDREF_OBJECT(pEventHandler);
        pEventHandler->OnOpenAsyncIOStream(resultErr, this, m_pEventHandlerContext);
        RELEASE_OBJECT(pEventHandler);
    }
} // OnBlockIOOpen.






/////////////////////////////////////////////////////////////////////////////
//
// [OnBlockIOAccept]
//
// This is a CAsyncBlockIOCallback method. It is called when we accept a new
// network connection from a remote client. Wrap this in a CAsyncIOStream and
// pass it to out callback.
/////////////////////////////////////////////////////////////////////////////
void
CAsyncIOStream::OnBlockIOAccept(ErrVal resultErr, CAsyncBlockIO *pBlockIO) {
    ErrVal err = ENoErr;
    CAsyncIOStream *pAsyncIOStream = NULL;
    CAsyncIOEventHandler *pEventHandler = NULL;

    DEBUG_LOG_VERBOSE("CAsyncIOStream::OnBlockIOAccept, err = %d, blockIO = %p", resultErr, pBlockIO);

    if ((resultErr) || (NULL == pBlockIO)) {
        gotoErr(resultErr);
    }

    // Create a new stream.
    pAsyncIOStream = newex CAsyncIOStream;
    if (NULL == pAsyncIOStream) {
        gotoErr(EFail);
    }
    err = pAsyncIOStream->StartOpenCommand(NULL, NULL);
    if (err) {
        gotoErr(err);
    }

    // Connect the AsyncIOStream to the block IO. This will allow the
    // AsyncIOStream to read and buffer all incoming data.
    pAsyncIOStream->m_pBlockIO = pBlockIO;
    ADDREF_OBJECT(pBlockIO);

    err = pAsyncIOStream->FinishOpenCommand();
    if (err) {
        gotoErr(err);
    }

    // Notify our callback.
    pEventHandler = m_pEventHandler;
    ADDREF_OBJECT(pEventHandler);
    pEventHandler->OnAcceptConnection(pAsyncIOStream, m_pEventHandlerContext);
    RELEASE_OBJECT(pEventHandler);

abort:
    RELEASE_OBJECT(pAsyncIOStream);
    return;
} // OnBlockIOAccept.






/////////////////////////////////////////////////////////////////////////////
//
// [MoveBufferToBackground]
//
// MoveBufferToBackground starts to not write buffers for seekable and
// synchronous streams.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOStream::MoveBufferToBackground(CIOBuffer *pBuffer) {
    ErrVal err = ENoErr;
    AutoLock(m_pLock);

    if (NULL == m_pBlockIO) {
        gotoErr(EFail);
    }

    // If this buffer is not active, then ignore it.
    if ((NULL == pBuffer)
         || ((m_pActiveOutputIOBuffer != pBuffer)
            && (m_pActiveIOBuffer != pBuffer))) {
        gotoErr(ENoErr);
    }

    // Adjust the data size in the buffer.
    if (m_pActiveOutputIOBuffer == pBuffer) {
        m_pActiveOutputIOBuffer = NULL;
        m_pFirstValidOutputByte = NULL;
        m_pNextValidOutputByte = NULL;
        m_pLastPossibleValidOutputByte = NULL;
    } else if (m_pActiveIOBuffer == pBuffer) {
        m_pActiveIOBuffer = NULL;
        m_pFirstValidByte = NULL;
        m_pNextValidByte = NULL;
        m_pEndValidBytes = NULL;
        m_pLastPossibleValidByte = NULL;
    } // (m_pBlockIO->m_fSeekable)

    if (pBuffer->m_BufferFlags & CIOBuffer::UNSAVED_CHANGES) {
        err = WriteBackgroundBuffer(pBuffer);
    }

abort:
    returnErr(err);
} // MoveBufferToBackground.







/////////////////////////////////////////////////////////////////////////////
//
// [WriteBackgroundBuffer]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOStream::WriteBackgroundBuffer(CIOBuffer *pBuffer) {
    ErrVal err = ENoErr;
    AutoLock(m_pLock);

    if (NULL == m_pBlockIO) {
        gotoErr(EFail);
    }

    if (!(pBuffer->m_BufferFlags & CIOBuffer::UNSAVED_CHANGES)) {
        gotoErr(ENoErr);
    }

    pBuffer->m_BufferFlags &= ~CIOBuffer::UNSAVED_CHANGES;
    pBuffer->m_StartWriteOffset = 0;

    if (!(m_AsyncIOStreamFlags & EXPANDING_MEMORY_STREAM)) {
        m_pBlockIO->WriteBlockAsync(pBuffer, 0);
    }

abort:
    returnErr(err);
} // WriteBackgroundBuffer.







/////////////////////////////////////////////////////////////////////////////
//
// [AllocAsyncIOStreamBuffer]
//
// This allocates a new buffer.
/////////////////////////////////////////////////////////////////////////////
CIOBuffer *
CAsyncIOStream::AllocAsyncIOStreamBuffer(int64 bufferStartPos, bool fInputBuffer) {
    ErrVal err = ENoErr;
    CIOBuffer *pBuffer = NULL;
    bool fAllocBuffer = true;
    AutoLock(m_pLock);


    if ((NULL == m_pBlockIO) || (NULL == m_pIOSystem)) {
        gotoErr(EFail);
    }


    // We will either allocate a new block, or else recycle an existing one.
    // m_IOBufferList is in LRU order, so try to remove a buffer from the end.
    // The most recently used buffer is at the head. Start at the tail and
    // use the prev pointers to go towards the head.
    if ((m_pBlockIO->m_fSeekable)
            && !(m_AsyncIOStreamFlags & EXPANDING_MEMORY_STREAM)
            && !(m_AsyncIOStreamFlags & ALL_DATA_IS_IN_BUFFERS)
            && (m_IOBufferList.GetLength() >= m_MaxNumIOBuffersForSeekableDevices)) {
        pBuffer = m_IOBufferList.GetTail();
        while (NULL != pBuffer) {
            if (CIOBuffer::NO_OP == pBuffer->m_BufferOp) {
                DEBUG_LOG_VERBOSE("CAsyncIOStream::AllocAsyncIOStreamBuffer. Recycle a buffer");
                break;
            }
            pBuffer = pBuffer->m_StreamBufferList.GetPreviousInQueue();
        } // while (NULL != pBuffer)
    } // Recycle a buffer.


    // If we cannot recycle a buffer, then allocate a new one.
    if (NULL == pBuffer) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::AllocAsyncIOStreamBuffer. Allocate a new buffer");

        pBuffer = m_pIOSystem->AllocIOBuffer(-1, fAllocBuffer);
        if ((NULL == pBuffer)
            || (NULL == pBuffer->m_pPhysicalBuffer)
            || (pBuffer->m_BufferSize < 0)) {
            gotoErr(EFail);
        }

        // The buffer was AddRef'ed by AllocIOBuffer.
        if (fInputBuffer) {
            // Keep m_IOBufferList in LRU order. New buffers are the most recently
            // used, so they are added to the head of the list.
            m_IOBufferList.InsertHead(&(pBuffer->m_StreamBufferList));
        } else {
            // The output list is FIFO, so new buffers are inserted at the tail.
            m_OutputBufferList.InsertTail(&(pBuffer->m_StreamBufferList));
        }
    } // Allocate a new bufer.

    pBuffer->m_BufferOp = CIOBuffer::NO_OP;
    pBuffer->m_BufferFlags &= ~CIOBuffer::VALID_DATA;
    pBuffer->m_Err = ENoErr;
    if (fInputBuffer) {
        pBuffer->m_BufferFlags |= CIOBuffer::INPUT_BUFFER;
    }
    pBuffer->m_BufferFlags |= CIOBuffer::OUTPUT_BUFFER;

    // buffer and bufferSize are initialized by AllocIOBuffer.
    pBuffer->m_pLogicalBuffer = pBuffer->m_pPhysicalBuffer;
    pBuffer->m_NumValidBytes = 0;

    pBuffer->m_PosInMedia = m_pIOSystem->GetIOStartPosition(bufferStartPos);

    return(pBuffer);

abort:
    return(NULL);
} // AllocAsyncIOStreamBuffer.







/////////////////////////////////////////////////////////////////////////////
//
// [Flush]
//
/////////////////////////////////////////////////////////////////////////////
void
CAsyncIOStream::Flush() {
    ErrVal err = ENoErr;
    CIOBuffer *pBuffer;
    AutoLock(m_pLock);
    RunChecks();

    DEBUG_LOG_VERBOSE("CAsyncIOStream::Flush");

    // The stream must have been initialized.
    if (NULL == m_pBlockIO) {
        gotoErr(EFail);
    }

    m_AsyncIOStreamFlags |= FLUSHING;
    m_NumFlushWrites = 0;
    m_FlushErr = ENoErr;


    // In some cases, we never write data to the backing store,
    // so there is nothing to do.
    if ((m_AsyncIOStreamFlags & EXPANDING_MEMORY_STREAM)
            || (m_AsyncIOStreamFlags & ALL_DATA_IS_IN_BUFFERS)) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::Flush is a no-op on a memory stream");
        gotoErr(ENoErr);
    }

    pBuffer = m_IOBufferList.GetHead();
    while (pBuffer) {
        if ((CIOBuffer::NO_OP == pBuffer->m_BufferOp)
            && (pBuffer->m_BufferFlags & CIOBuffer::UNSAVED_CHANGES)) {
            DEBUG_LOG_VERBOSE("CAsyncIOStream::Flush writes one buffer (%p)", pBuffer);
            err = WriteBackgroundBuffer(pBuffer);
            if (!err) {
                m_NumFlushWrites += 1;
            }
        } else if (CIOBuffer::WRITE == pBuffer->m_BufferOp) {
            DEBUG_LOG_VERBOSE("CAsyncIOStream::Flush found a buffer already being written");
            m_NumFlushWrites += 1;
        }
        pBuffer = pBuffer->m_StreamBufferList.GetNextInQueue();
    } // while (pBuffer)

    pBuffer = m_OutputBufferList.GetHead();
    while (pBuffer) {
        if ((CIOBuffer::NO_OP == pBuffer->m_BufferOp)
            && (pBuffer->m_BufferFlags & CIOBuffer::UNSAVED_CHANGES)) {
            DEBUG_LOG_VERBOSE("CAsyncIOStream::Flush writes one buffer (%p)", pBuffer);
            err = WriteBackgroundBuffer(pBuffer);
            if (!err) {
                m_NumFlushWrites += 1;
            }
        } else if (CIOBuffer::WRITE == pBuffer->m_BufferOp) {
            DEBUG_LOG_VERBOSE("CAsyncIOStream::Flush found a buffer already being written");
            m_NumFlushWrites += 1;
        }
        pBuffer = pBuffer->m_StreamBufferList.GetNextInQueue();
    } // while (pBuffer)

    DEBUG_LOG_VERBOSE("CAsyncIOStream::Flush m_NumFlushWrites = %d", m_NumFlushWrites);

    m_pActiveIOBuffer = NULL;
    m_pFirstValidByte = NULL;
    m_pNextValidByte = NULL;
    m_pEndValidBytes = NULL;
    m_pLastPossibleValidByte = NULL;

    m_pActiveOutputIOBuffer = NULL;
    m_pFirstValidOutputByte = NULL;
    m_pNextValidOutputByte = NULL;
    m_pLastPossibleValidOutputByte = NULL;

abort:
    // If no blocks are being written, then the flush is done.
    // Be careful. Don't do this if some other methods has
    // already called OnFlush.
    m_AsyncIOStreamFlags &= ~FLUSHING;
    m_AsyncIOStreamFlags |= WAITING_ON_FLUSH;
    if (m_NumFlushWrites <= 0) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::Flush calls FinishFlush");
        FinishFlush();
    }
} // Flush








/////////////////////////////////////////////////////////////////////////////
//
// [FinishFlush]
//
/////////////////////////////////////////////////////////////////////////////
void
CAsyncIOStream::FinishFlush() {
    AutoLock(m_pLock);
    RunChecks();

    DEBUG_LOG_VERBOSE("CAsyncIOStream::FinishFlush");
    if (m_pBlockIO->m_NumActiveWrites > 0) {
       DEBUG_WARNING("There are active reads or writes.");
    }

    m_NumFlushWrites = 0;
    m_AsyncIOStreamFlags &= ~FLUSHING;
    m_AsyncIOStreamFlags &= ~WAITING_ON_FLUSH;

    if (NULL != m_pEventHandler) {
        CAsyncIOEventHandler *pEventHandler = m_pEventHandler;
        ADDREF_OBJECT(pEventHandler);
        pEventHandler->OnFlush(m_FlushErr, this, m_pEventHandlerContext);
        RELEASE_OBJECT(pEventHandler);
     } // (NULL != m_pEventHandler)
} // FinishFlush.







/////////////////////////////////////////////////////////////////////////////
//
// [CheckState]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOStream::CheckState() {
    ErrVal err = ENoErr;
    CIOBuffer *pBuffer;
    CIOBuffer *pNextBuffer;
    bool fFoundLargerBuffer;
    bool foregroundIsOK;
    int64 activeBufferStartPos;
    int64 activeBufferStopPos;
    AutoLock(m_pLock);

    // This means the stream has closed. This happens all the
    // time with net connections to web servers, and it is not
    // an error.
    if (NULL == m_pIOSystem) {
        gotoErr(ENoErr);
    }

    err = m_pBlockIO->CheckState();
    if (err) {
        gotoErr(EFail);
    }
    err = m_pIOSystem->CheckState();
    if (err) {
        gotoErr(EFail);
    }

    if (NULL == m_pActiveIOBuffer) {
        if ((m_pFirstValidByte)
            || (m_pNextValidByte)
            || (m_pEndValidBytes)) {
            gotoErr(EFail);
        }
    }
    if (NULL == m_pActiveOutputIOBuffer) {
        if ((m_pFirstValidOutputByte)
            || (m_pNextValidOutputByte)) {
            gotoErr(EFail);
        }
    }

    if (m_pActiveIOBuffer) {
        // Check m_pFirstValidByte
        if ((m_pFirstValidByte )
            && (m_pActiveIOBuffer)
            && (m_pActiveIOBuffer->m_pLogicalBuffer)
            && (m_pFirstValidByte != m_pActiveIOBuffer->m_pLogicalBuffer)) {
            gotoErr(EFail);
        }

        // Check m_pNextValidByte
        if ((m_pNextValidByte)
                && (m_pActiveIOBuffer)
                && (m_pActiveIOBuffer->m_pLogicalBuffer)) {
            if (m_pNextValidByte > (m_pActiveIOBuffer->m_pLogicalBuffer + m_pActiveIOBuffer->m_NumValidBytes)) {
                gotoErr(EFail);
            }
            if (m_pNextValidByte < m_pActiveIOBuffer->m_pLogicalBuffer) {
                gotoErr(EFail);
            }
        }

        // Check m_pEndValidBytes
        if ((m_pEndValidBytes)
            && (m_pActiveIOBuffer)
            && (m_pActiveIOBuffer->m_pLogicalBuffer)) {
            if (m_pEndValidBytes < m_pActiveIOBuffer->m_pLogicalBuffer) {
                gotoErr(EFail);
            }
            // While we are in the iddle of a read, m_pEndValidBytes may still
            // point to the beginning of the buffer. This is not an error.
            if (m_pEndValidBytes > (m_pActiveIOBuffer->m_pLogicalBuffer + m_pActiveIOBuffer->m_NumValidBytes)) {
                gotoErr(EFail);
            }
        }

        // Check m_pLastPossibleValidByte
        if ((m_pLastPossibleValidByte)
            && (m_pActiveIOBuffer)
            && (m_pActiveIOBuffer->m_pPhysicalBuffer)) {
            if (m_pLastPossibleValidByte < m_pActiveIOBuffer->m_pPhysicalBuffer) {
                gotoErr(EFail);
            }
            if (m_pLastPossibleValidByte > (m_pActiveIOBuffer->m_pPhysicalBuffer + m_pActiveIOBuffer->m_BufferSize)) {
                gotoErr(EFail);
            }
        }

        if ((m_pFirstValidByte)
            && (m_pEndValidBytes)
            && (m_pFirstValidByte > m_pEndValidBytes)) {
            gotoErr(EFail);
        }

        if ((m_pNextValidByte)
            && (m_pFirstValidByte)
            && (m_pEndValidBytes)
            && ((m_pNextValidByte < m_pFirstValidByte)
                || (m_pNextValidByte > m_pEndValidBytes))) {
            gotoErr(EFail);
        }
    } // if (m_pActiveIOBuffer)


    if (m_pActiveOutputIOBuffer) {
        if ((m_pFirstValidOutputByte)
            && (m_pActiveOutputIOBuffer)
            && (m_pActiveOutputIOBuffer->m_pLogicalBuffer)
            && (m_pFirstValidOutputByte != m_pActiveOutputIOBuffer->m_pLogicalBuffer)) {
            gotoErr(EFail);
        }

        // Check m_pNextValidOutputByte
        if ((m_pNextValidOutputByte)
            && (m_pActiveOutputIOBuffer)
            && (m_pActiveOutputIOBuffer->m_pLogicalBuffer)) {
            if (m_pNextValidOutputByte
                > (m_pActiveOutputIOBuffer->m_pLogicalBuffer + m_pActiveOutputIOBuffer->m_NumValidBytes)) {
                gotoErr(EFail);
            }
            if (m_pNextValidOutputByte < m_pActiveOutputIOBuffer->m_pLogicalBuffer) {
                gotoErr(EFail);
            }
        }

        // Check m_pLastPossibleValidOutputByte
        if ((m_pLastPossibleValidOutputByte)
            && (m_pActiveOutputIOBuffer)
            && (m_pActiveOutputIOBuffer->m_pPhysicalBuffer)) {
            if (m_pLastPossibleValidOutputByte < m_pActiveOutputIOBuffer->m_pPhysicalBuffer) {
                gotoErr(EFail);
            }
            if (m_pLastPossibleValidOutputByte
                > (m_pActiveOutputIOBuffer->m_pPhysicalBuffer + m_pActiveOutputIOBuffer->m_BufferSize)) {
                gotoErr(EFail);
            }
        }

        if ((m_pFirstValidOutputByte)
            && (m_pNextValidOutputByte)
            && (m_pFirstValidOutputByte > m_pNextValidOutputByte)) {
            gotoErr(EFail);
        }
    } // if (m_pActiveOutputIOBuffer)


    // Now, check every buffer in the list.
    pBuffer = m_IOBufferList.GetHead();
    while (pBuffer) {
        err = pBuffer->CheckState();
        if (err) {
            gotoErr(EFail);
        }

        // Look for the next larger buffer.
        fFoundLargerBuffer = false;
        pNextBuffer = m_IOBufferList.GetHead();
        while (pNextBuffer) {
            if (pNextBuffer->m_PosInMedia > pBuffer->m_PosInMedia) {
                fFoundLargerBuffer = true;
                if ((pBuffer->m_PosInMedia + pBuffer->m_NumValidBytes)
                    == pNextBuffer->m_PosInMedia) {
                    break;
                }
            }

            pNextBuffer = pNextBuffer->m_StreamBufferList.GetNextInQueue();
        }

        if ((fFoundLargerBuffer) && (!pNextBuffer)) {
            gotoErr(EFail);
        }

        pBuffer = pBuffer->m_StreamBufferList.GetNextInQueue();
    } // while (pBuffer)


    pBuffer = m_OutputBufferList.GetHead();
    while (pBuffer) {
        err = pBuffer->CheckState();
        if (err) {
            gotoErr(EFail);
        }

        pBuffer = pBuffer->m_StreamBufferList.GetNextInQueue();
    }

    // Make sure the foreground buffer doesn't overlap with
    // any background buffer.
    if (NULL == m_pActiveIOBuffer) {
        foregroundIsOK = true;
    } else
    {
        activeBufferStartPos = m_pActiveIOBuffer->m_PosInMedia;
        if (0 == m_pActiveIOBuffer->m_NumValidBytes) {
            activeBufferStopPos = m_pActiveIOBuffer->m_PosInMedia;
        } else {
            activeBufferStopPos = m_pActiveIOBuffer->m_PosInMedia + m_pActiveIOBuffer->m_NumValidBytes - 1;
        }
        foregroundIsOK = false;

        pBuffer = m_IOBufferList.GetHead();
        while (pBuffer) {
            if (pBuffer == m_pActiveIOBuffer) {
                foregroundIsOK = true;
            }
            // Make sure this buffer does not overlap any other buffers.
            else if (m_pActiveIOBuffer) {
                if ((activeBufferStartPos >= pBuffer->m_PosInMedia)
                    && (activeBufferStartPos < (pBuffer->m_PosInMedia + pBuffer->m_NumValidBytes))) {
                    gotoErr(EFail);
                }
                if ((activeBufferStopPos >= pBuffer->m_PosInMedia)
                    && (activeBufferStopPos < (pBuffer->m_PosInMedia + pBuffer->m_NumValidBytes))) {
                    gotoErr(EFail);
                }
            }

            pBuffer = pBuffer->m_StreamBufferList.GetNextInQueue();
        }

        if (!foregroundIsOK) {
            gotoErr(EFail);
        }
    } // if (NULL != m_pActiveIOBuffer)

    if ((NULL != m_pActiveOutputIOBuffer)
        && (m_pBlockIO->m_fSeekable)) {
        gotoErr(EFail);
    }

abort:
    returnErr(err);
} // CheckState.







//////////////////////////////////////////////////////////////////////////////
//
// [GetChar]
//
// Get a single UTF-8 character. This may be 1 byte or several bytes, depending
// on the character.
//////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOStream::GetChar(
                  char *pBuffer,
                  int32 maxCharLen,
                  int *pCharLen) {
    ErrVal err = ENoErr;
    AutoLock(m_pLock);

    if ((NULL == pBuffer) || (NULL == pCharLen) || (maxCharLen < 0)) {
        gotoErr(EFail);
    }

    // Read the next character from the buffer.
    // Depending on the alphabet, this may be several bytes, so it
    // could straddle buffers.
    *pCharLen = 0;
    while (*pCharLen < maxCharLen) {
        if ((m_pNextValidByte)
            && (m_pEndValidBytes)
            && (m_pNextValidByte < m_pEndValidBytes)) {
            *pBuffer = *(m_pNextValidByte++);
        } else {
            err = Read(pBuffer, 1);
            DEBUG_LOG_VERBOSE("CAsyncIOStream::GetChar. Call Read() err = %d", err);
            if (err) {
                gotoErr(err);
            }
        }

        pBuffer++;
        *pCharLen += 1;

        break;
        // For now, no explicit Unicode support at the stream level.
        // if (CStringLib::IsCompleteChar(pBuffer, *pCharLen)) { break; }
    } // while (*pCharLen < maxCharLen)

abort:
    returnErr(err);
} // GetChar




//////////////////////////////////////////////////////////////////////////////
//
// [PutChar]
//
// Write a single UTF-8 character. This may be 1 byte or several bytes, depending
// on the character.
//////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOStream::PutChar(const char *pChar, int32 charLen) {
    if (NULL == pChar) {
       returnErr(EFail);
    }
    charLen = charLen;
    return(PutByte(*pChar));
} // PutChar





/////////////////////////////////////////////////////////////////////////////
//
// [PutByte]
//
// Write a single octet (1 byte). This may or may not be a complete character
// in UTF-8.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOStream::PutByte(char c) {
    ErrVal err = ENoErr;
    AutoLock(m_pLock);

    if (NULL == m_pBlockIO) {
        returnErr(EFail);
    }


    // Fast case #1.
    // We have room in the buffer, and we maintain separate buffers for input and
    // output.
    if ((!(m_pBlockIO->m_fSeekable))
        && (m_pNextValidOutputByte)
        && (m_pLastPossibleValidOutputByte)
        && (m_pNextValidOutputByte < m_pLastPossibleValidOutputByte)) {
        // Now, at this point we know there is enough room in the buffer
        // for the value. Copy the value to the buffer. We save it in
        // the buffer in the same format as it will appear in the file.
        *(m_pNextValidOutputByte++) = c;

        // Now that we know this is the buffer we will be writing to,
        // record that we have written to it.
        if (m_pActiveOutputIOBuffer) {
           m_pActiveOutputIOBuffer->m_NumValidBytes = m_pNextValidOutputByte - m_pFirstValidOutputByte;
           m_pActiveOutputIOBuffer->m_BufferFlags |= CIOBuffer::UNSAVED_CHANGES;
           m_pActiveOutputIOBuffer->m_BufferFlags |= CIOBuffer::VALID_DATA;
        }
    }
    // Fast case #1.
    // We have room in the buffer, and we use the same buffer for input and
    // output.
    else if ((m_pBlockIO->m_fSeekable)
            && (m_pNextValidByte)
            && (m_pLastPossibleValidByte)
            && (m_pNextValidByte < m_pLastPossibleValidByte)) {
        // Now, at this point we know there is enough room in the buffer
        // for the value. Copy the value to the buffer. We save it in
        // the buffer in the same format as it will appear in the file.
        *(m_pNextValidByte++) = c;

        // If you write, then seek backwards, then write again, the second
        // write overwrites new data with even newer data. It does not,
        // however, extend the modified part of the block.
        if ((m_pEndValidBytes) && (m_pNextValidByte > m_pEndValidBytes)) {
            m_pEndValidBytes = m_pNextValidByte;
        }

        // Now that we know this is the buffer we will be writing to,
        // record that we have written to it.
        if (m_pActiveIOBuffer) {
           m_pActiveIOBuffer->m_NumValidBytes = m_pEndValidBytes - m_pFirstValidByte;
           m_pActiveIOBuffer->m_BufferFlags |= CIOBuffer::UNSAVED_CHANGES;
           m_pActiveIOBuffer->m_BufferFlags |= CIOBuffer::VALID_DATA;
        }
    }
    // The slow case. Write() may allocate a new empty buffer.
    else
    {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::PutByte. Call Write()");
        err = Write((char *) &c, 1);
        if (err) {
            gotoErr(err);
        }
    }

abort:
    returnErr(err);
} // PutByte








//////////////////////////////////////////////////////////////////////////////
//
// [printf]
//
//////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOStream::printf(const char *format, ...) {
    ErrVal err = ENoErr;
    va_list argList;
    char formatChar;
    int intValue;
    char charValue;
    char *pCharPtrValue;
    int32 state = PRINTF_FORMAT_NORMAL_CHAR;
    char widthString[32];
    char numberBuffer[32];
    char convertedCharBuffer[16];
    int32 convertedCharSize;
    int widthStringLength = 0;
    char *pEndPtr;
    int32 numDigits = -1;
    int32 charsWritten;
    int numActualDigits;
    AutoLock(m_pLock);


    va_start(argList, format);


    // This is the main loop; it examines every character in the format string.
    while ((*format) && (!err)) {
        formatChar = *format;
        format++;

        switch (state) {
        ///////////////////////////
        // Print the next character from the buffer. If this is special (like \ or %) then it
        // will change the state.
        case PRINTF_FORMAT_NORMAL_CHAR:
            if ('\\' == formatChar) {
                state = PRINTF_FORMAT_ESCAPED_CHAR;
            } else if ('%' == formatChar) {
                // Prepare to process a new variable descriptor.
                state = PRINTF_FORMAT_PERCENT_CHAR;
                widthStringLength = 0;
                numDigits = -1;
            } else {
                // Write the character.
                err = PutByte(formatChar);
                if (err) {
                    gotoErr(err);
                }
            }
            break;


        ///////////////////////////
        case PRINTF_FORMAT_ESCAPED_CHAR:
            state = PRINTF_FORMAT_NORMAL_CHAR;
            // Just write the character without interpreting it.
            err = PutByte(formatChar);
            if (err) {
                gotoErr(err);
            }
            break;


        ///////////////////////////
        case PRINTF_FORMAT_VARIABLE_WIDTH:
            if (CStringLib::IsByte(formatChar, CStringLib::NUMBER_CHAR)) {
                if (widthStringLength < 32) {
                    widthString[widthStringLength] = formatChar;
                    widthStringLength += 1;
                } else {
                    gotoErr(EInvalidArg);
                }
            } else {
                state = PRINTF_FORMAT_PERCENT_CHAR;
                widthStringLength = 0;
                numDigits = -1;

                format--;
            } // read the variable type char
            break;


        ///////////////////////////
        case PRINTF_FORMAT_PERCENT_CHAR:
            // We just read %x, for some x, so now switch on the value of x.
            if (CStringLib::IsByte(formatChar, CStringLib::NUMBER_CHAR)) {
                widthString[0] = formatChar;
                widthStringLength = 1;
                state = PRINTF_FORMAT_VARIABLE_WIDTH;
                // The rest of the number characters will be read in the
                // PRINTF_FORMAT_VARIABLE_WIDTH state, which appends to widthString.
            } else {
                // If there was a width string that was part of this
                // format, then convert that to a number.
                if (widthStringLength >  0) {
                    err = CStringLib::StringToNumber(widthString, widthStringLength, &numDigits);
                    if (err)
                    {
                        numDigits = -1;
                        err = ENoErr;
                    }
                } else {
                    numDigits = -1;
                } // convert width string to a number.


                state = PRINTF_FORMAT_NORMAL_CHAR;
                switch(formatChar) {
                ////////////////////////////////////////
                case 'l': // long
                case 'd': // integer
                case 'i': // integer
                    //<><><>Cleanup
                    intValue = va_arg(argList, int32);
                    snprintf(numberBuffer, sizeof(numberBuffer), "%d", intValue);
                    pEndPtr = numberBuffer + strlen(numberBuffer);

                    // WARNING.
                    // numActualDigits puts a limit on the total formatted size, not the number
                    // of digits. So, it includes chars like ',' and '.'.
                    // In UTF-8, all of these characters are 1 byte.
                    numActualDigits = (pEndPtr - numberBuffer);

                    if (numDigits > 0) {
                        // Prefix this with 0's.
                        while (numActualDigits < numDigits) {
                            err = PutByte('0');
                            if (err) {
                                gotoErr(err);
                            }
                            numActualDigits++;
                        }

                        // Truncate, if necessary.
                        while (numActualDigits > numDigits) {
                            DEBUG_WARNING("Truncating number.");
                            pEndPtr = pEndPtr - 1;
                            numActualDigits--;
                        }
                    } // (numDigits > 0)

                    // Write the number.
                    err = Write(numberBuffer, pEndPtr - numberBuffer);
                    if (err) {
                        gotoErr(err);
                    }
                    break;

                ////////////////////////////////////////
                case 'C': // char
                case 'c': // char
#if LINUX
                    charValue = va_arg(argList, int);
#else
                    charValue = va_arg(argList, char);
#endif

                    if ('c' == formatChar) {
                        err = PutByte(charValue);
                        if (err) {
                            gotoErr(err);
                        }
                    } else {
                        err = CStringLib::ConvertUTF8ToUTF16(
                                            &charValue,
                                            1,
                                            (WCHAR *) (&convertedCharBuffer[0]),
                                            sizeof(convertedCharBuffer),
                                            &convertedCharSize);
                        if (err) {
                            gotoErr(err);
                        }
                        err = Write(convertedCharBuffer, convertedCharSize);
                        if (err) {
                            gotoErr(err);
                        }
                    }
                    break;


                ////////////////////////////////////////
                case 'Q': // string
                case 'q': // string
                case 'S': // string
                case 's': // string
                    pCharPtrValue = va_arg(argList, char *);
                    charsWritten = 0;
                    if (NULL != pCharPtrValue) {
                        // Preface it with a quote.
                        if (('Q' == formatChar) || ('q' == formatChar)) {
                            err = PutByte('\"');
                            if (err) {
                                gotoErr(err);
                            }
                            // Optionally make this a UTF-16 character.
                            if ('Q' == formatChar) {
                                err = PutByte(0);
                                if (err) {
                                    gotoErr(err);
                                }
                            }
                        } // if (('Q' == formatChar) || ('q' == formatChar))

                        while (*pCharPtrValue) {
                            if (('s' == formatChar) || ('q' == formatChar)) {
                                err = PutByte(*pCharPtrValue);
                                if (err) {
                                    gotoErr(err);
                                }
                            } else {
                                err = CStringLib::ConvertUTF8ToUTF16(
                                                    pCharPtrValue,
                                                    1,
                                                    (WCHAR *) (&convertedCharBuffer[0]),
                                                    sizeof(convertedCharBuffer),
                                                    &convertedCharSize);
                                if (err) {
                                    gotoErr(err);
                                }
                                err = Write(convertedCharBuffer, convertedCharSize);
                                if (err) {
                                    gotoErr(err);
                                }
                            }

                            pCharPtrValue++;
                            charsWritten++;
                            if ((numDigits > 0) && (charsWritten >= numDigits))
                            {
                                break;
                            }
                        } // while (*pCharPtrValue)


                        // Pad the string.
                        if (numDigits > 0)
                        {
                            while (charsWritten < numDigits)
                            {
                                err = PutByte(' ');
                                if (err)
                                {
                                    gotoErr(err);
                                }
                                // Optionally make this a UTF-16 character.
                                if (('S' == formatChar) || ('Q' == formatChar))
                                {
                                    err = PutByte(' ');
                                    if (err)
                                    {
                                        gotoErr(err);
                                    }
                                }

                                charsWritten++;
                            } // while (charsWritten < numDigits)
                        } // if (numDigits > 0)


                        // Close with a quote.
                        if (('Q' == formatChar) || ('q' == formatChar)) {
                            err = PutByte('\"');
                            if (err) {
                                gotoErr(err);
                            }
                            // Optionally make this a UTF-16 character.
                            if ('Q' == formatChar) {
                                err = PutByte(0);
                                if (err)
                                {
                                    gotoErr(err);
                                }
                            }
                        } // if (('Q' == formatChar) || ('q' == formatChar))
                    } // (NULL != pCharPtrValue)
                    break;

                ////////////////////////////////////////
                default:
                    // If a percent sign (%) is followed by a character that has no
                    // meaning as a format-control character, that character and the
                    // following characters (up to the next percent sign) are treated
                    // as an ordinary sequence of characters, that is, a sequence of
                    // characters that must match the input. For example, to specify
                    // that a percent-sign character is to be input, use %%.
                    err = PutByte(formatChar);
                    if (err) {
                        gotoErr(err);
                    }
                    break;
                } // switch on variable type.
            } // read the variable type char
            break;

        default:
            break;
        } // switch on the state.
    } // main loop

abort:
    va_end(argList);
    returnErr(err);
} // printf






/////////////////////////////////////////////////////////////////////////////
//
// [SkipWhileCharType]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOStream::SkipWhileCharType(int32 charType) {
    ErrVal err = ENoErr;
    char streamCharBuffer[32];
    int32 streamCharSize;
    int32 streamCharProperties;
    int64 lastCharPosition;
    AutoLock(m_pLock);
    RunChecks();

    // Make sure we start at a valid buffer. Otherwise, all pointers are bogus.
    if (NULL == m_pActiveIOBuffer) {
        err = SetPosition(GetDataLength());
        if (err) {
            if (EEOF == err) {
                err = ENoErr;
            }
            gotoErr(err);
        }
    }

    // This outer loop reads complete characters. Each iteration
    // reads one character, which may span buffers.
    while (1) {
        lastCharPosition = GetPosition();

        // Read the next character from the buffer.
        // GetChar() can handle multi-byte characters that might straddle buffers.
        err = GetChar(
                    streamCharBuffer,
                    sizeof(streamCharBuffer),
                    &streamCharSize);
        if (err) {
            if (EEOF == err) {
                err = ENoErr;
            }
            gotoErr(err);
        }

        streamCharProperties = CStringLib::GetCharProperties(streamCharBuffer, streamCharSize);
        if (!(streamCharProperties & charType)) {
            // We skip up to just BEFORE the character with this type.
            err = SetPosition(lastCharPosition);
            gotoErr(err);
        }
    } // reading the next buffer of data.

abort:
    returnErr(err);
} // SkipWhileCharType.






/////////////////////////////////////////////////////////////////////////////
//
// [SkipUntilCharType]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOStream::SkipUntilCharType(int32 charType) {
    ErrVal err = ENoErr;
    char streamCharBuffer[32];
    int32 streamCharSize;
    int32 streamCharProperties;
    int64 lastCharPosition;
    AutoLock(m_pLock);
    RunChecks();

    // Make sure we start at a valid buffer. Otherwise, all pointers are bogus.
    if (NULL == m_pActiveIOBuffer) {
        err = SetPosition(GetDataLength());
        if (err) {
            gotoErr(err);
        }
    }

    // This outer loop reads complete characters. Each iteration
    // reads one character, which may span buffers.
    while (1) {
        lastCharPosition = GetPosition();
        // Read the next character from the buffer.
        // Depending on the alphabet, this may be several bytes, so it
        // could straddle buffers.
        err = GetChar(
                  streamCharBuffer,
                  sizeof(streamCharBuffer),
                  &streamCharSize);
        if (err) {
            gotoErr(err);
        }
        streamCharProperties = CStringLib::GetCharProperties(streamCharBuffer, 1);
        if (streamCharProperties & charType) {
            // We skip up to just BEFORE the character with this type.
            err = SetPosition(lastCharPosition);
            gotoErr(err);
        }
    } // reading the next buffer of data.

abort:
    returnErr(err);
} // SkipUntilCharType






/////////////////////////////////////////////////////////////////////////////
//
// [FindString]
//
// This procedure searches a stream for a pattern. A search may span several
// buffers and an instance of the pattern may straddle two buffers.
/////////////////////////////////////////////////////////////////////////////
int64
CAsyncIOStream::FindString(
                const char *pPattern,
                int32 patternLength,
                int32 searchOptions,
                int64 stopPosition) {
    ErrVal err = ENoErr;
    int64 currentPos;
    int64 savePos = -1;
    int64 matchPosition = -1;
    int32 numBytesInBuffer;
    int32 numBytesToSearch;
    char straddleBuffer[ (CStringLib::MAX_SEARCH_PATTERN_SIZE * 2) + 8 ];
    int32 numStraddleBytesFromPrevBuffer;
    int32 numStraddleBytesFromCurrentBuffer;
    char *pStartStraddleText;
    int64 straddleBufferPos;
    const char *pMatchStr;
    bool fHitStopPosition = false;
    AutoLock(m_pLock);
    RunChecks();

    searchOptions = searchOptions; // Unused.
    if ((NULL == pPattern)
        || (patternLength > CStringLib::MAX_SEARCH_PATTERN_SIZE)
        || (patternLength < 0)) {
        gotoErr(EFail);
    }
    DEBUG_LOG_VERBOSE("CAsyncIOStream::FindString. pattern = %s", pPattern);

    // This is not an error, just a degenerate case.
    if (0 == *pPattern) {
        DEBUG_LOG_VERBOSE("CAsyncIOStream::FindString. Giving up on an empty pattern");
        gotoErr(ENoErr);
    }

    // Make sure we start at a valid buffer. Otherwise, all pointers are bogus.
    if (NULL == m_pActiveIOBuffer) {
        err = SetPosition(0);
        if (err) {
            DEBUG_LOG("CAsyncIOStream::FindString. SetPosition failed. err = %d", err);
            gotoErr(err);
        }
    }
    if ((NULL == m_pEndValidBytes)
        || (NULL == m_pNextValidByte)
        || (NULL == m_pFirstValidByte)
        || (NULL == m_pActiveIOBuffer)) {
        gotoErr(EFail);
    }

    savePos = m_pActiveIOBuffer->m_PosInMedia + (m_pNextValidByte - m_pFirstValidByte);
    currentPos = savePos;


    // Each iteration of this loop searches one buffer and its
    // staddle with the next buffer.
    numStraddleBytesFromPrevBuffer = 0;
    while (1) {
        numBytesInBuffer = m_pEndValidBytes - m_pNextValidByte;

        // If there is a straddle buffer from the previous buffer, then
        // search that now. This finds instances of the pattern that straddled
        // the previous and current buffer.
        if (numStraddleBytesFromPrevBuffer > 0) {
            // Copy the first patternLength bytes of the current buffer into
            // the straddle buffer.
            numStraddleBytesFromCurrentBuffer = patternLength;
            if (numStraddleBytesFromCurrentBuffer > numBytesInBuffer) {
                numStraddleBytesFromCurrentBuffer = numBytesInBuffer;
            }
            memcpy(
                straddleBuffer + numStraddleBytesFromPrevBuffer,
                m_pNextValidByte,
                numStraddleBytesFromCurrentBuffer);

            // Look for the pattern in the straddle buffer.
            numBytesToSearch = numStraddleBytesFromPrevBuffer + numStraddleBytesFromCurrentBuffer;
            if ((stopPosition > 0) && ((straddleBufferPos + numBytesToSearch) > stopPosition)) {
               numBytesToSearch = (int32) (stopPosition - straddleBufferPos);
               fHitStopPosition = true;
            }

            pMatchStr = CStringLib::FindPatternInBuffer(
                                                    straddleBuffer,
                                                    numBytesToSearch,
                                                    pPattern,
                                                    patternLength);
            if (NULL != pMatchStr) {
                matchPosition = straddleBufferPos + (pMatchStr - straddleBuffer);
                break;
            }
            numStraddleBytesFromPrevBuffer = 0;

            if (fHitStopPosition) {
               gotoErr(err);
            }
        } // if (numStraddleBytesFromPrevBuffer > 0)

        if ((stopPosition > 0) && ((currentPos + numBytesInBuffer) > stopPosition)) {
            numBytesInBuffer = (int32) (stopPosition - currentPos);
            fHitStopPosition = true;
        }

        // Look for the pattern in this buffer.
        pMatchStr = CStringLib::FindPatternInBuffer(
                                                m_pNextValidByte,
                                                numBytesInBuffer,
                                                pPattern,
                                                patternLength);
        if (NULL != pMatchStr) {
            matchPosition = currentPos + (pMatchStr - m_pNextValidByte);
            break;
        }

        if (fHitStopPosition) {
           break;
        }

        // Copy the last patternLength bytes of this buffer into
        // the straddle buffer.
        numStraddleBytesFromPrevBuffer = patternLength;
        if (numStraddleBytesFromPrevBuffer > numBytesInBuffer) {
            numStraddleBytesFromPrevBuffer = numBytesInBuffer;
        }
        if (numStraddleBytesFromPrevBuffer < 0) {
            numStraddleBytesFromPrevBuffer = 0;
        }

        pStartStraddleText = m_pNextValidByte
                              + numBytesInBuffer
                              - numStraddleBytesFromPrevBuffer;
        memcpy(
            straddleBuffer,
            pStartStraddleText,
            numStraddleBytesFromPrevBuffer);
        straddleBufferPos = currentPos + (pStartStraddleText - m_pNextValidByte);


        // Go to the next buffer.
        currentPos += numBytesInBuffer;
        err = SetPosition(currentPos);
        if ((err)
            || (NULL == m_pEndValidBytes)
            || (NULL == m_pNextValidByte)) {
            if (EEOF == err) {
               err = ENoErr;
            }
            DEBUG_LOG("CAsyncIOStream::FindString. SetPosition failed. err = %d", err);
            gotoErr(err);
        }
    } // searching in every buffer.

abort:
    // Set the position in the stream.
    if (matchPosition >= 0) {
        err = SetPosition(matchPosition);
    } else if (savePos >= 0) {
        err = SetPosition(savePos);
    }

    return(matchPosition);
} // FindString.





/////////////////////////////////////////////////////////////////////////////
//
/////////////////////////////////////////////////////////////////////////////
CAsyncIOEventHandlerSynch::CAsyncIOEventHandlerSynch() {
    m_pSemaphore = NULL;

    m_pAsyncIOStream = NULL;
    m_TestErr = ENoErr;
} // CAsyncIOEventHandlerSynch






/////////////////////////////////////////////////////////////////////////////
//
/////////////////////////////////////////////////////////////////////////////
CAsyncIOEventHandlerSynch::~CAsyncIOEventHandlerSynch() {
    RELEASE_OBJECT(m_pSemaphore);
    RELEASE_OBJECT(m_pAsyncIOStream);
} // ~CAsyncIOEventHandlerSynch




/////////////////////////////////////////////////////////////////////////////
//
// [Initialize]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOEventHandlerSynch::Initialize() {
    ErrVal err = ENoErr;

    m_pSemaphore = newex CRefEvent;
    if (NULL == m_pSemaphore) {
        gotoErr(EFail);
    }

    err = m_pSemaphore->Initialize();
    if (err) {
        gotoErr(err);
    }

abort:
    returnErr(err);
} // Initialize





/////////////////////////////////////////////////////////////////////////////
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncIOEventHandlerSynch::Wait() {
    if (m_pSemaphore) {
        m_pSemaphore->Wait();
    }
    return(m_TestErr);
} // Wait







/////////////////////////////////////////////////////////////////////////////
//
/////////////////////////////////////////////////////////////////////////////
void
CAsyncIOEventHandlerSynch::OnReadyToRead(
                                ErrVal err,
                                int64 totalBytesAvailable,
                                CAsyncIOStream *pAsyncIOStream,
                                void *pCallbackContext) {
    // Unused parameters
    totalBytesAvailable = totalBytesAvailable;
    pAsyncIOStream = pAsyncIOStream;
    pCallbackContext = pCallbackContext;

    m_TestErr = err;
    if (m_pSemaphore) {
        m_pSemaphore->Signal();
    }
} // OnReadyToRead





/////////////////////////////////////////////////////////////////////////////
//
/////////////////////////////////////////////////////////////////////////////
void
CAsyncIOEventHandlerSynch::OnFlush(
                                ErrVal err,
                                CAsyncIOStream *pAsyncIOStream,
                                void *pCallbackContext) {
    // Unused parameters
    pAsyncIOStream = pAsyncIOStream;
    pCallbackContext = pCallbackContext;

    m_TestErr = err;
    if (m_pSemaphore) {
        m_pSemaphore->Signal();
    }
} // OnFlush







/////////////////////////////////////////////////////////////////////////////
//
/////////////////////////////////////////////////////////////////////////////
void
CAsyncIOEventHandlerSynch::OnOpenAsyncIOStream(
                               ErrVal resultErr,
                               CAsyncIOStream *pAsyncIOStream,
                               void *pCallbackContext) {
    // Unused parameters
    pCallbackContext = pCallbackContext;

    RELEASE_OBJECT(m_pAsyncIOStream);
    m_pAsyncIOStream = pAsyncIOStream;
    ADDREF_OBJECT(m_pAsyncIOStream);

    m_TestErr = resultErr;
    if (m_pSemaphore) {
        m_pSemaphore->Signal();
    }
} // OnOpenAsyncIOStream







/////////////////////////////////////////////////////////////////////////////
//
/////////////////////////////////////////////////////////////////////////////
void
CAsyncIOEventHandlerSynch::OnAcceptConnection(
                                CAsyncIOStream *pNewAsyncIOStream,
                                void *pContext) {
    // Unused parameters
    pNewAsyncIOStream = pNewAsyncIOStream;
    pContext = pContext;

    if (m_pSemaphore) {
        m_pSemaphore->Signal();
    }
} // OnAcceptConnection







/////////////////////////////////////////////////////////////////////////////
//
//                     TESTING PROCEDURES
//
/////////////////////////////////////////////////////////////////////////////
#if INCLUDE_REGRESSION_TESTS

#if LINUX
#include <unistd.h>
#endif

static CAsyncIOEventHandlerSynch *g_TestCallback = NULL;

ErrVal TestCompareStreams(CAsyncIOStream *reader, CAsyncIOStream *writer, bool clipToDestSize);
ErrVal TestCopyStreams(CAsyncIOStream *reader, CAsyncIOStream *writer, bool fBucketIO);

// Watch out, google now has a chunked main page. The higher levels of the
// http code can handle chunking, but not this simple test code.
//static char *g_TestServerName = "www.google.com";
#define DEFAULT_TEST_SERVER     "www.uky.edu"
static char *g_TestServerName = (char *) DEFAULT_TEST_SERVER;
static int g_TestServerPort = 80;





/////////////////////////////////////////////////////////////////////////////
//
// [TestAsyncIOStream]
//
/////////////////////////////////////////////////////////////////////////////
void
CAsyncIOStream::TestAsyncIOStream() {
    ErrVal err = ENoErr;
    CAsyncIOStream *reader = NULL;
    CAsyncIOStream *writer = NULL;
    int i;
    char c;
    CDebugObject timer;
    int64 strOffset;
    char path[512];
    CParsedUrl *pUrl = NULL;
    bool fFoundProxy;
    const char *ptr = "GET / HTTP/1.1\r\n"
            "Accept: */*\r\n"
            "TestHeader: 1\r\n"
            "Accept-Language: en-us\r\n"
            //"Accept-Encoding: gzip, deflate\r\n"
            "User-Agent: dawsonsBrowser\r\n" // Mozilla/4.0 (compatible; MSIE 6.0; Windows NT 5.0; .NET CLR 1.1.4322)\r\n"
            "Host: www.altavista.com\r\n"
            //"Connection: Keep-Alive\r\n"
            "\r\n";



    g_DebugManager.StartModuleTest("Async IO Stream");

    g_TestCallback = newex CAsyncIOEventHandlerSynch;
    if (NULL == g_TestCallback) {
        gotoErr(EFail);
    }
    err = g_TestCallback->Initialize();
    if (err) {
        gotoErr(err);
    }

    fFoundProxy = NetIO_GetLocalProxySettings(&g_TestServerName, &g_TestServerPort);
    if (!fFoundProxy) {
        g_TestServerName = (char *) DEFAULT_TEST_SERVER;
        g_TestServerPort = 80;
    }


    //////////////////////////////////////////////////////////
    g_DebugManager.StartTest("Synchronously copy one file into another");


    RELEASE_OBJECT(pUrl);
    g_DebugManager.AddTestDataDirectoryPath("sample.html", path, 512);
    pUrl = CParsedUrl::AllocateFileUrl(path);
    if (NULL == pUrl) {
        gotoErr(EFail);
    }

    err = CAsyncIOStream::OpenAsyncIOStream(
                                 pUrl,
                                 CAsyncBlockIO::READ_ACCESS | CAsyncBlockIO::WRITE_ACCESS,
                                 g_TestCallback,
                                 NULL);
    if (err) {
        gotoErr(err);
    }

    err = g_TestCallback->Wait();
    if (err) {
        gotoErr(err);
    }
    reader = g_TestCallback->m_pAsyncIOStream;
    g_TestCallback->m_pAsyncIOStream = NULL;



    // Make sure that old data doesn't confuse us.
    g_DebugManager.AddTestResultsDirectoryPath("asyncIOStreamTestOutput.txt", path, 512);
    (void) CSimpleFile::DeleteFile(path);

    RELEASE_OBJECT(pUrl);
    pUrl = CParsedUrl::AllocateFileUrl(path);
    if (NULL == pUrl) {
        gotoErr(EFail);
    }
    err = CAsyncIOStream::OpenAsyncIOStream(
                              pUrl,
                              CAsyncBlockIO::CREATE_NEW_STORE | CAsyncBlockIO::READ_ACCESS | CAsyncBlockIO::WRITE_ACCESS,
                              g_TestCallback,
                              NULL);
    if (err) {
        gotoErr(err);
    }


    err = g_TestCallback->Wait();
    if (err) {
        gotoErr(err);
    }
    writer = g_TestCallback->m_pAsyncIOStream;
    g_TestCallback->m_pAsyncIOStream = NULL;




    reader->ListenForAllBytesToEOF();
    err = g_TestCallback->Wait();
    if (err) {
        gotoErr(err);
    }

    //streamSize = reader->GetDataLength();
    reader->SetDebugFlags(CDebugObject::CHECK_STATE_ON_EVERY_OP);
    writer->SetDebugFlags(CDebugObject::CHECK_STATE_ON_EVERY_OP);

    err = TestCopyStreams(reader, writer, true);
    if (err) {
        gotoErr(err);
    }



    writer->Flush();
    err = g_TestCallback->Wait();
    if (err) {
        gotoErr(err);
    }

    reader->Close();
    writer->Close();

    //////////////////////////////////////////////////////////
    g_DebugManager.StartTest("Read a copied file and read until we get an EOF");

    RELEASE_OBJECT(pUrl);
    g_DebugManager.AddTestDataDirectoryPath("sample.html", path, 512);
    pUrl = CParsedUrl::AllocateFileUrl(path);
    if (NULL == pUrl) {
        gotoErr(EFail);
    }

    err = CAsyncIOStream::OpenAsyncIOStream(
                                pUrl,
                                CAsyncBlockIO::READ_ACCESS | CAsyncBlockIO::WRITE_ACCESS,
                                g_TestCallback,
                                NULL);
    if (err) {
        gotoErr(err);
    }


    err = g_TestCallback->Wait();
    if (err) {
        gotoErr(err);
    }
    reader = g_TestCallback->m_pAsyncIOStream;
    g_TestCallback->m_pAsyncIOStream = NULL;


    g_DebugManager.AddTestResultsDirectoryPath("asyncIOStreamTestOutput.txt", path, 512);
    RELEASE_OBJECT(pUrl);
    pUrl = CParsedUrl::AllocateFileUrl(path);
    if (NULL == pUrl) {
        gotoErr(EFail);
    }

    err = CAsyncIOStream::OpenAsyncIOStream(
                        pUrl,
                        CAsyncBlockIO::READ_ACCESS | CAsyncBlockIO::WRITE_ACCESS,
                        g_TestCallback,
                        NULL);
    if (err) {
        gotoErr(err);
    }

    err = g_TestCallback->Wait();
    if (err) {
        gotoErr(err);
    }
    writer = g_TestCallback->m_pAsyncIOStream;
    g_TestCallback->m_pAsyncIOStream = NULL;


    reader->ListenForAllBytesToEOF();
    err = g_TestCallback->Wait();
    if (err) {
        gotoErr(err);
    }


    writer->ListenForAllBytesToEOF();
    err = g_TestCallback->Wait();
    if (err) {
        gotoErr(err);
    }


    err = TestCompareStreams(reader, writer, false);
    if (err) {
        gotoErr(err);
    }
    err = TestCompareStreams(writer, reader, false);
    if (err) {
        gotoErr(err);
    }



    //////////////////////////////////////////////////////////
    g_DebugManager.StartTest("Read at EOF several times");
    for (i = 0; i < 10; i++) {
        err = reader->GetByte(&c);
        if (EEOF != err) {
            DEBUG_WARNING("Missed an EOF");
        }
    }


    writer->Close();
    reader->Close();


    //////////////////////////////////////////////////////////
    g_DebugManager.StartTest("Synchronously copy a network link into a file");

    {
        bool fFoundProxy;
        char buffer[CParsedUrl::MAX_URL_LENGTH];

        fFoundProxy = NetIO_GetLocalProxySettings(&g_TestServerName, &g_TestServerPort);
        if (!fFoundProxy) {
            g_TestServerName = (char *) DEFAULT_TEST_SERVER;
            g_TestServerPort = 80;
        }
        snprintf(
            buffer,
            CParsedUrl::MAX_URL_LENGTH,
            "http://%s:%d",
            g_TestServerName,
            g_TestServerPort);

        RELEASE_OBJECT(pUrl);
        pUrl = CParsedUrl::AllocateUrl(buffer);
        if (NULL == pUrl) {
            gotoErr(EFail);
        }
    }


    err = CAsyncIOStream::OpenAsyncIOStream(pUrl, 0, g_TestCallback, NULL);
    if (err) {
        gotoErr(err);
    }
    err = g_TestCallback->Wait();
    if (err) {
        gotoErr(err);
    }
    reader = (CAsyncIOStream *) (g_TestCallback->m_pAsyncIOStream);
    g_TestCallback->m_pAsyncIOStream = NULL;



    err = reader->Write(ptr, strlen(ptr));
    if (err) {
        gotoErr(err);
    }

    reader->Flush();
    err = g_TestCallback->Wait();
    if (err) {
        gotoErr(err);
    }


    // Make sure that old data doesn't confuse us.
    g_DebugManager.AddTestResultsDirectoryPath("asyncIOStreamTestOutput.txt", path, 512);
    ::unlink(path);
    RELEASE_OBJECT(pUrl);
    pUrl = CParsedUrl::AllocateFileUrl(path);
    if (NULL == pUrl) {
        gotoErr(EFail);
    }

    err = CAsyncIOStream::OpenAsyncIOStream(
                        pUrl,
                        CAsyncBlockIO::CREATE_NEW_STORE | CAsyncBlockIO::READ_ACCESS | CAsyncBlockIO::WRITE_ACCESS,
                        g_TestCallback,
                        NULL);
    if (err) {
        gotoErr(err);
    }

    err = g_TestCallback->Wait();
    if (err) {
        gotoErr(err);
    }
    writer = g_TestCallback->m_pAsyncIOStream;
    g_TestCallback->m_pAsyncIOStream = NULL;



    reader->ListenForNBytes(-1, 100);
    err = g_TestCallback->Wait();
    if (err) {
        gotoErr(err);
    }


    reader->SetDebugFlags(CDebugObject::CHECK_STATE_ON_EVERY_OP);
    writer->SetDebugFlags(CDebugObject::CHECK_STATE_ON_EVERY_OP);

    err = TestCopyStreams(reader, writer, false);
    if (err) {
        gotoErr(err);
    }

    writer->Flush();
    err = g_TestCallback->Wait();
    if (err) {
        gotoErr(err);
    }



    //////////////////////////////////////////////////////////
    g_DebugManager.StartTest("Read a copied stream and read until we get an EOF");


    writer->Close();

    g_DebugManager.AddTestResultsDirectoryPath("asyncIOStreamTestOutput.txt", path, 512);
    RELEASE_OBJECT(pUrl);
    pUrl = CParsedUrl::AllocateFileUrl(path);
    if (NULL == pUrl) {
        gotoErr(EFail);
    }

    err = CAsyncIOStream::OpenAsyncIOStream(
                        pUrl,
                        CAsyncBlockIO::READ_ACCESS | CAsyncBlockIO::WRITE_ACCESS,
                        g_TestCallback,
                        NULL);
    if (err) {
        gotoErr(err);
    }


    err = g_TestCallback->Wait();
    if (err) {
        gotoErr(err);
    }
    writer = g_TestCallback->m_pAsyncIOStream;
    g_TestCallback->m_pAsyncIOStream = NULL;


    writer->ListenForAllBytesToEOF();
    err = g_TestCallback->Wait();
    if (err) {
        gotoErr(err);
    }


    err = TestCompareStreams(reader, writer, true);
    if (err) {
        gotoErr(err);
    }
    err = TestCompareStreams(writer, reader, true);
    if (err) {
        gotoErr(err);
    }


    //////////////////////////////////////////////////////////
    g_DebugManager.StartTest("Read at EOF several times");
    {
        int64 streamSize = 0;
        int64 streamPos = 0;
        streamSize = writer->GetDataLength();
        streamPos = writer->GetPosition();
        ::printf("\n>>streamSize = %d, stremPos=%d", ((int32)streamSize), ((int32) streamPos));
    }
    // BE Careful. Use the writer stream. This is a local file, which won't change
    // size. The reader is still pulling in packets from the network, so it may
    // no longer be at the end of the stream if new packets arrived.
    for (i = 0; i < 10; i++) {
        err = writer->GetByte(&c);
        if (EEOF != err) {
            DEBUG_WARNING("Missed an EOF");
        }
    }



    //////////////////////////////////////////////////////////
    g_DebugManager.StartTest("Search for a string in a file");

    RELEASE_OBJECT(pUrl);
    g_DebugManager.AddTestDataDirectoryPath("sample.html", path, 512);
    pUrl = CParsedUrl::AllocateFileUrl(path);
    if (NULL == pUrl) {
        gotoErr(EFail);
    }

    err = CAsyncIOStream::OpenAsyncIOStream(
                        pUrl,
                        CAsyncBlockIO::READ_ACCESS | CAsyncBlockIO::WRITE_ACCESS,
                        g_TestCallback,
                        NULL);
    if (err) {
        gotoErr(err);
    }

    err = g_TestCallback->Wait();
    if (err) {
        gotoErr(err);
    }
    reader = g_TestCallback->m_pAsyncIOStream;
    g_TestCallback->m_pAsyncIOStream = NULL;

    strOffset = reader->FindString(
                           "CAsyncIOStream::",
                           13, //patternSize,
                           0, // flags
                           -1);
    strOffset = strOffset; // Unused (for now)

    strOffset = reader->FindString(
                           "&*()MissingPattern!@#$",
                           13, //patternSize,
                           0, // flags
                           -1);
    strOffset = strOffset; // Unused (for now)

abort:
    if (err) {
        ::printf("\nError: (%d)\n", ERROR_CODE(err));
    }

    if (reader) {
        reader->Close();
    }
    if (writer) {
        writer->Close();
    }
    RELEASE_OBJECT(reader);
    RELEASE_OBJECT(writer);
    RELEASE_OBJECT(pUrl);
} // TestAsyncIOStream.








/////////////////////////////////////////////////////////////////////////////
//
// [TestCopyStreams]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
TestCopyStreams(
            CAsyncIOStream *reader,
            CAsyncIOStream *writer,
            bool fBucketIO) {
    ErrVal err = ENoErr;
    char c;
    char preReadC;
    char postReadC;
    int32 trialNum;
    int32 byteNum = 0;
    int64 streamSize = 0;

    err = reader->SetPosition(0);
    if (err) {
        gotoErr(err);
    }

    streamSize = reader->GetDataLength();
    byteNum = 0;
    while (1) {
        for (trialNum = 0; trialNum < 3; trialNum++) {
            err = reader->GetByte(&preReadC);
            // An error may only mean EOF.
            if (EEOF == err) {
                gotoErr(ENoErr);
            } else if (err) {
                gotoErr(err);
            }

            err = reader->UnGetByte();
            if (err) {
                gotoErr(err);
            }
        }

        err = reader->GetByte(&c);
        if (err) {
            gotoErr(EFail);
        }

        if (preReadC != c) {
            gotoErr(EFail);
        }

        err = writer->PutByte(c);
        if (err) {
            gotoErr(err);
        }


        for (trialNum = 0; trialNum < 3; trialNum++) {
            err = reader->UnGetByte();
            if (err) {
                gotoErr(err);
            }

            err = reader->GetByte(&postReadC);
            if (err) {
                gotoErr(err);
            }

            if (postReadC != c) {
                gotoErr(EFail);
            }
        }

        byteNum++;
    }

abort:
    // This test does not make sense if this is a pipe IO, since new packets
    // may be arriving throughout the test.
    if ((fBucketIO) && (byteNum != streamSize)) {
        DEBUG_WARNING("Bad copy");
    }

    returnErr(err);
} // TestCopyStreams.








/////////////////////////////////////////////////////////////////////////////
//
// [TestCompareStreams]
//
// Use fClipToCopiedSize when we are comparing a network read to a local 
// file copy. If the network is slow, then packets may arrive after we 
// finish copying to a local file, which means the original file from the 
// network will be longer than the local copy.
/////////////////////////////////////////////////////////////////////////////
ErrVal
TestCompareStreams(CAsyncIOStream *reader, CAsyncIOStream *writer, bool fClipToCopiedSize) {
    ErrVal err = ENoErr;
    char c1;
    char c2;
    char preReadC;
    int32 trialNum;
    int32 byteNum = 0;
    int64 streamSize = 0;

    err = reader->SetPosition(0);
    if (err) {
        gotoErr(err);
    }

    err = writer->SetPosition(0);
    if (err) {
        gotoErr(err);
    }

    streamSize = writer->GetDataLength();


    byteNum = 0;
    while ((!fClipToCopiedSize) || (byteNum < streamSize)) {
        for (trialNum = 0; trialNum < 3; trialNum++) {
            err = reader->GetByte(&preReadC);
            // An error may only mean EOF.
            if (EEOF == err) {
                err = ENoErr;
                gotoErr(ENoErr);
            } else if (err) {
                gotoErr(err);
            }

            err = reader->UnGetByte();
            if (err) {
                gotoErr(err);
            }
        }

        err = reader->GetByte(&c1);
        if (err) {
            gotoErr(err);
        }

        if (preReadC != c1) {
            gotoErr(EFail);
        }

        err = writer->GetByte(&c2);
        if (err) {
            gotoErr(err);
        }

        if (c2 != c1) {
            gotoErr(EFail);
        }

        byteNum++;
        if ((fClipToCopiedSize) && (byteNum >= streamSize)) {
            break;
        }
    }

abort:
    byteNum = byteNum;
    returnErr(err);
} // TestCompareStreams.



#endif // INCLUDE_REGRESSION_TESTS


