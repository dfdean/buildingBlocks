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

#ifndef _ASYNC_IO_STREAM_H_
#define _ASYNC_IO_STREAM_H_

class CAsyncIOStream;




/////////////////////////////////////////////////////////////////////////////
// All asynch events are translated into calls on this object.
class CAsyncIOEventHandler : public CRefCountInterface {
public:
    virtual void OnStreamDisconnect(
                    ErrVal err,
                    CAsyncIOStream *pAsyncIOStream,
                    void *pContext) { err = err; pAsyncIOStream = pAsyncIOStream; pContext = pContext; }

    virtual void OnAcceptConnection(
                    CAsyncIOStream *pNewAsyncIOStream,
                    void *pContext) { pNewAsyncIOStream = pNewAsyncIOStream; pContext = pContext; }

    virtual void OnOpenAsyncIOStream(
                    ErrVal err,
                    CAsyncIOStream *pAsyncIOStream,
                    void *pContext) { err = err; pAsyncIOStream = pAsyncIOStream; pContext = pContext; }

    virtual void OnFlush(
                    ErrVal err,
                    CAsyncIOStream *pAsyncIOStream,
                    void *pContext) { err = err; pAsyncIOStream = pAsyncIOStream; pContext = pContext; }

    virtual void OnReadyToRead(
                    ErrVal err,
                    int64 numBytesAvailable,
                    CAsyncIOStream *pAsyncIOStream,
                    void *pContext) { err = err; numBytesAvailable = numBytesAvailable; pAsyncIOStream = pAsyncIOStream; pContext = pContext; }
}; // CAsyncIOEventHandler.





/////////////////////////////////////////////////////////////////////////////
class CAsyncIOEventHandlerSynch : public CAsyncIOEventHandler,
                                       public CRefCountImpl {
public:
    CAsyncIOEventHandlerSynch();
    virtual ~CAsyncIOEventHandlerSynch();
    NEWEX_IMPL()

    ErrVal Initialize();
    ErrVal Wait();

    // CAsyncIOEventHandler
    virtual void OnAcceptConnection(
                    CAsyncIOStream *pNewAsyncIOStream,
                    void *pContext);
    virtual void OnOpenAsyncIOStream(
                    ErrVal err,
                    CAsyncIOStream *pAsyncIOStream,
                    void *pContext);
    virtual void OnFlush(
                    ErrVal err,
                    CAsyncIOStream *pAsyncIOStream,
                    void *pCallbackContext);
    virtual void OnReadyToRead(
                    ErrVal err,
                    int64 totalBytesAvailable,
                    CAsyncIOStream *pAsyncIOStream,
                    void *pCallbackContext);

    // CRefCountInterface
    PASS_REFCOUNT_TO_REFCOUNTIMPL()

    CAsyncIOStream      *m_pAsyncIOStream;

private:
    CRefEvent           *m_pSemaphore;
    ErrVal              m_TestErr;
}; // CAsyncIOEventHandlerSynch






/////////////////////////////////////////////////////////////////////////////
class CAsyncIOStream : public CAsyncBlockIOCallback,
                            public CRefCountImpl {
public:
    enum {
        // Don't use -1 for this flag. There are bug cases where we
        // accidentally pass a small negative value for the length, and
        // we wantto catch that, rather than treat it as going to the
        // end of the stream.
        COPY_TO_EOF             = 0x10000000,
    };

    static ErrVal OpenAsyncIOStream(
                        CParsedUrl *pUrl,
                        int32 openOptions,
                        CAsyncIOEventHandler *pEventHandler,
                        void *pEventHandlerContext);

#if INCLUDE_REGRESSION_TESTS
    static void TestAsyncIOStream();
#endif

    CAsyncIOStream();
    virtual ~CAsyncIOStream();
    NEWEX_IMPL()

    void Close();
    bool IsOpen() {return(NULL != m_pBlockIO);}
    CRefLock *GetLock();

    // Some devices, like network connections, are asynchronous. In those cases,
    // we may have to wait until enough data becomes available to read.
    int64 GetDataLength();

    // For an asynchronous device, these methods allow you to wait until some
    // amount of data is available to read.
    void ListenForNBytes(int64 startPos, int64 numBytes);
    void ListenForMoreBytes();
    void ListenForAllBytesToEOF();
    void Flush();

    // These read and write bytes.
    inline ErrVal GetByte(char *c);
    inline ErrVal PeekByte(char *c);
    inline ErrVal UnGetByte();
    ErrVal PutByte(char c);
    ErrVal Read(char *clientBuffer, int32 bytesToRead);
    ErrVal Write(const char *clientBuffer, int32 bytesToWrite);

    ErrVal printf(const char *format, ...);

    ErrVal GetPtr(
                int64 startPos,
                int32 length,
                char *pTempBuffer,
                int32 maxLength,
                char **pPtr);
    ErrVal GetPtrRef(
                int64 startPos,
                int32 length,
                char **pPtr,
                int32 *pActualLength);

    // Seek functionality.
    int64 GetPosition();
    ErrVal SetPosition(int64 newPos);

    // RemoveNBytes is for HTTP 1.1 clients that need to remove chunk
    // markers from the middle of a stream.
    ErrVal RemoveNBytes(int64 startPos, int32 bytesToRemove);

    // Copy streams
    ErrVal CopyStream(CAsyncIOStream *destStream, int64 numBytes, bool fNoCopy);

    // These read and write characters in a particular alphabet.
    ErrVal SkipWhileCharType(int32 charType);
    ErrVal SkipUntilCharType(int32 charType);
    ErrVal GetChar(
               char *pBuffer,
               int32 maxCharLen,
               int *pCharLen);
    ErrVal PutChar(const char *pChar, int32 charLen);

    // Search functions
    int64 FindString(
                const char *pPattern,
                int32 patternSize,
                int32 flags,
                int64 stopPosition);

    // CDebugObject
    virtual ErrVal CheckState();

    // CAsyncBlockIOCallback
    virtual void OnBlockIOEvent(CIOBuffer *pBuffer);
    virtual void OnBlockIOOpen(ErrVal resultErr, CAsyncBlockIO *pBlockIO);
    virtual void OnBlockIOAccept(ErrVal resultErr, CAsyncBlockIO *pBlockIO);

    // CRefCountInterface
    PASS_REFCOUNT_TO_REFCOUNTIMPL()

private:
    enum AsyncIOStreamPrivateConstants
    {
        // Flags for m_AsyncIOStreamFlags.
        FLUSHING                        = 0x0001,
        WAITING_ON_FLUSH                = 0x0002,
        SERVER_STREAM                   = 0x0004,
        UDP_SERVER_STREAM               = 0x0008,
        UDP_READ_STREAM                 = 0x0010,
        ALL_DATA_IS_IN_BUFFERS          = 0x0020,
        EXPANDING_MEMORY_STREAM         = 0x0040,

        MIN_REASONABLE_NETWORK_PACKET   = 400,

        // These are the states we pass through while parsing format strings.
        PRINTF_FORMAT_NORMAL_CHAR       = 0,
        PRINTF_FORMAT_ESCAPED_CHAR      = 1,
        PRINTF_FORMAT_PERCENT_CHAR      = 2,
        PRINTF_FORMAT_VARIABLE_WIDTH    = 3,

        // States of the data stream
        IDLE                            = 1,
        READING                         = 2,

        NOT_LOADING                     = 0,
        ASYNCH_LOAD_NBYTES              = 1,
        ASYNCH_LOAD_ANY_BYTES           = 2,
        ASYNCH_LOAD_TO_EOF              = 3
    };

    ErrVal StartOpenCommand(
                    CAsyncIOEventHandler *pEventHandler,
                    void *pEventHandlerContext);
    ErrVal FinishOpenCommand();

    void ContinueAsyncLoad(ErrVal resultErr, bool fReceivedNewData);
    void FinishAsyncLoad(ErrVal resultErr);

    CIOBuffer *AllocAsyncIOStreamBuffer(int64 newPos, bool fInputBuffer);

    ErrVal MoveBufferToBackground(CIOBuffer *pBuffer);
    ErrVal WriteBackgroundBuffer(CIOBuffer *pBuffer);
    ErrVal WriteToStreamDevice(const char *clientBuffer, int32 bytesToWrite);
    ErrVal WriteToSeekableDevice(const char *clientBuffer, int32 bytesToWrite);

    void FinishFlush();

    int32                   m_AsyncIOStreamFlags;
    CRefLock                *m_pLock;

    // This is the connection to the media that we are reading/writing.
    CAsyncBlockIO           *m_pBlockIO;
    CIOSystem               *m_pIOSystem;

    int64                   m_TotalAvailableBytes;
    int64                   m_MinPosition;

    // These point into the current buffer of data.
    CIOBuffer               *m_pActiveIOBuffer;
    char                    *m_pFirstValidByte;
    char                    *m_pNextValidByte;
    char                    *m_pEndValidBytes;
    char                    *m_pLastPossibleValidByte;

    // These point into the current buffer of output data.
    // These are only used when the blockIO does not allow
    // you to read what you wrote, so it is NOT seekable.
    // For example, this is true for network IO. In all
    // other cases, like with file IO, then both reading and
    // writing use the above set of buffer pointers.
    CIOBuffer               *m_pActiveOutputIOBuffer;
    char                    *m_pFirstValidOutputByte;
    char                    *m_pNextValidOutputByte;
    char                    *m_pLastPossibleValidOutputByte;
    CQueueList<CIOBuffer>   m_OutputBufferList;

    // This describes the state of asynch reads or writes.
    CQueueList<CIOBuffer>   m_IOBufferList;
    int32                   m_MaxNumIOBuffersForSeekableDevices;

    // This is the state of the current asynch read.
    int32                   m_AsynchLoadType;
    int64                   m_LoadStartPosition;
    int64                   m_LoadStopPosition;
    int64                   m_NextLoadStartPosition;
    int64                   m_NextAsynchBufferPosition;

    // This is for all async events and operations.
    CAsyncIOEventHandler    *m_pEventHandler;
    void                    *m_pEventHandlerContext;

    // This is for flush operations.
    int32                   m_NumFlushWrites;
    ErrVal                  m_FlushErr;
}; // CAsyncIOStream





/////////////////////////////////////////////////////////////////////////////
//
// [GetByte]
//
/////////////////////////////////////////////////////////////////////////////
inline ErrVal
CAsyncIOStream::GetByte(char *pResultChar) {
    ErrVal err;
    AutoLock(m_pLock);

    if ((pResultChar)
        && (m_pNextValidByte)
        && (m_pEndValidBytes)
        && (m_pNextValidByte < m_pEndValidBytes)) {
        *pResultChar = *(m_pNextValidByte++);
        return(ENoErr);
    } else
    {
        err = Read(pResultChar, 1);
        return(err);
    }
} // GetByte.








/////////////////////////////////////////////////////////////////////////////
//
// [PeekByte]
//
/////////////////////////////////////////////////////////////////////////////
inline ErrVal
CAsyncIOStream::PeekByte(char *pResultChar) {
    AutoLock(m_pLock);

    if ((pResultChar)
        && (m_pNextValidByte)
        && (m_pEndValidBytes)
        && (m_pNextValidByte < m_pEndValidBytes)) {
        *pResultChar = *m_pNextValidByte;
        return(ENoErr);
    } else
    {
        ErrVal err = Read(pResultChar, 1);
        (void) UnGetByte();
        return(err);
    }
} // PeekByte





/////////////////////////////////////////////////////////////////////////////
//
// [UnGetByte]
//
/////////////////////////////////////////////////////////////////////////////
inline ErrVal
CAsyncIOStream::UnGetByte() {
    AutoLock(m_pLock);

    if ((m_pNextValidByte)
        && (m_pFirstValidByte)
        && (m_pNextValidByte > m_pFirstValidByte)) {
        m_pNextValidByte = m_pNextValidByte - 1;
        return(ENoErr);
    }
    // Otherwise, the buffer is empty. Move our position back
    // in the file.
    else if ((m_pActiveIOBuffer)
        && (m_pActiveIOBuffer->m_PosInMedia > 0)) {
        return(SetPosition(m_pActiveIOBuffer->m_PosInMedia - 1));
    } else
    {
        return(EFail);
    }
} // UnGetByte



#endif // _ASYNC_IO_STREAM_H_




