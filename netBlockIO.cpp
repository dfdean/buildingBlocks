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
// Net Block I/O
//
// This implements the generic block-device interface on top of a network
// sockets layer of the underlying OS.
/////////////////////////////////////////////////////////////////////////////

#if LINUX
#include <fcntl.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#if WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

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

FILE_DEBUGGING_GLOBALS(LOG_LEVEL_DEFAULT, 0);

#define USE_SOCKS  0
#if USE_SOCKS
#include <socks.h>
static bool g_InitedFirewall = false;
extern "C" void SOCKSinit(char *s);
#endif // USE_SOCKS

static void SelectThreadProc(void *arg, CSimpleThread *threadState);

static char g_LocalServerURL[] = "ip://127.0.0.1";
static int32 g_LocalServerURLLength = 14;

static char g_IncomingIPAddressURL[] = "ip://0.0.0.0";
static int32 g_IncomingIPAddressURLLength = 12;

#define MAX_TIMEOUTS_PER_CHECK          64
#define MAX_SYSTEM_CALL_INTERRUPTS      5


#if LINUX
#define SOCKET_ERROR -1
#define IO_WOULD_BLOCK(_lastErr) ((EWOULDBLOCK == _lastErr) || (EINPROGRESS == _lastErr))
#define IO_NOT_CONNECTED_ERROR(_lastErr) ((ENOTCONN == _lastErr))
#define IGNORE_SYSTEMCALL_ERROR() 0
// (EPIPE == GET_LAST_ERROR())
#endif

#if WIN32
#define IO_WOULD_BLOCK(_lastErr) (WSAEWOULDBLOCK == _lastErr)
#define IO_NOT_CONNECTED_ERROR(_lastErr) 0
typedef int32 socklen_t;
#define IGNORE_SYSTEMCALL_ERROR() 0
#define MSG_NOSIGNAL 0
#endif

#define WAKEUP_SELECT_THREAD_MESSAGE_LENGTH     4

extern CConfigSection *g_pBuildingBlocksConfig;
static const char g_NetworkProxyHostConfigValueName[] = "Network Proxy";
static const char g_NetworkProxyPortConfigValueName[] = "Network Proxy Port";


#if WIN32
typedef SOCKET OSSocket;
#define NULL_SOCKET  INVALID_SOCKET
#elif LINUX
typedef int OSSocket;
#define NULL_SOCKET  -1
#endif



/////////////////////////////////////////////////////////////////////////////
// This describes one network connection.
class CNetBlockIO : public CAsyncBlockIO,
                    public CRBTree::CNode {
public:
    CNetBlockIO();
    virtual ~CNetBlockIO();
    NEWEX_IMPL()

    // CAsyncBlockIO
    virtual void Close();
    virtual ErrVal Flush();
    virtual ErrVal Resize(int64 newLength);
    virtual void CancelTimeout(int32 opType);
    virtual void StartTimeout(int32 opType);

    // CDebugObject
    virtual ErrVal CheckState();

    void TestBreakingNetworkConnection();

protected:
    friend class CNetIOSystem;
    friend class CAsyncBlockIO;

    // CAsyncBlockIO
    virtual void ReadBlockAsyncImpl(CIOBuffer *pBuffer);
    virtual void WriteBlockAsyncImpl(CIOBuffer *pBuffer);

    // CNetBlockIO
    virtual ErrVal DoPendingRead(
                        CIOBuffer *pBuffer,
                        bool *pReadFullBuffer,
                        bool *pFinished);
    virtual ErrVal DoPendingWrite(
                        CIOBuffer *pBuffer,
                        bool *pFinished);
    bool CheckTimeout(uint64 value, int32 *pTimeoutOp);
    ErrVal PrepareToDisconnect();


    enum CNetBlockIOPrivateConstants {
        // Be REALLY careful. We subtract time from the timeout
        // every interval, so the first interval may happen immediately
        // after we start waiting. This means each timeout should be padded
        // with one interval duration.
        DEFAULT_CONNECT_TIMEOUT_IN_MS   = 150 * 1000,
        DEFAULT_WRITE_TIMEOUT_IN_MS     = 150 * 1000,
        DEFAULT_READ_TIMEOUT_IN_MS      = 200 * 1000,

        ACCEPT_INCOMING_CONNECTIONS     = 0x00010000,
        DISCONNECT_SOCKET               = 0x00020000,
        WAITING_TO_WRITE                = 0x00040000,
        WAITING_TO_READ                 = 0x00080000,
        WAITING_TO_CONNECT              = 0x00100000,
        SIMULATE_NETWORK_ERROR          = 0x00200000,
        NEVER_TIMEOUT                   = 0x00400000,
        UDP_SOCKET                      = 0x00800000,

        // The type of socket.
        SOCKET_TYPE_TCP                 = 1,
        SOCKET_TYPE_UDP                 = 2,
        SOCKET_TYPE_SSL                 = 3,

        WAITING_FOR_ANYTHING            = (WAITING_TO_WRITE | WAITING_TO_READ | WAITING_TO_CONNECT)
    };


    int32                   m_SocketType;
    OSSocket                m_Socket;

    CQueueHook<CNetBlockIO> m_PendingCloseList;

    // This is a list of buffers for pending IO operations.
    CQueueList<CIOBuffer>   m_PendingWrites;
    CQueueList<CIOBuffer>   m_PendingReads;

    // This is the destination of all UDP datagrams on this blockIO.
    struct sockaddr_in      m_UDPDatagramDest;

    // This is the timeout. A blockIO can only wait for
    // one thing at a time, so it can only timeout for 1
    // thing at a time. m_CurrentTimeoutInMs MUST BE AN INTEGER or else
    // it becomes huge positive numbers when it goes below 0.
    int64                   m_CurrentTimeoutRemainingInMs;
    int32                   m_CurrentTimeoutOp;
    int8                    m_NumTimeouts;
    int32                   m_ReadTimeout;
    int32                   m_ConnectTimeout;
}; // CNetBlockIO




/////////////////////////////////////////////////////////////////////////////
class CNetIOSystem : public CIOSystem {
public:
    CNetIOSystem();
    virtual ~CNetIOSystem();
    NEWEX_IMPL()

    // CIOSystem
    virtual ErrVal Shutdown();
    virtual ErrVal OpenBlockIO(
                        CParsedUrl *pUrl,
                        int32 options,
                        CAsyncBlockIOCallback *pCallback);
    virtual int32 GetDefaultBytesPerBlock() { return(NETWORK_MTU); }
    virtual int64 GetIOStartPosition(int64 pos) { return(pos); }
    virtual int32 GetBlockBufferAlignment() { return(0); }

    // CNetIOSystem
    ErrVal OpenServerBlockIO(
                        bool fUDP,
                        uint16 portNum,
                        bool fUse127001Address,
                        CAsyncBlockIOCallback *pCallback,
                        CAsyncBlockIO **ppResultBlockIO);
    ErrVal ReceiveDataFromAcceptedConnection(
                        CNetBlockIO *pBlockIO,
                        CAsyncBlockIOCallback *pCallback);
    void GetLocalAddr(struct sockaddr_in *addr);
    static ErrVal GetLocalHostName(char *host, int32 maxHostLength);
    void WaitForAllBlockIOsToClose();

    // This is called by the top-level select thread wrapper. It executes all
    // select thread functions, and doesn't return until the select thread exits.
    void SelectThread();

    // CDebugObject
    virtual ErrVal CheckState();

private:
    friend class CNetBlockIO;

    enum CNetIOSystemPrivateConstants
    {
        NETWORK_MTU  = 1400, // 1270, 1452,

        SELECT_TIMEOUT_IN_MS = 5000,
        SELECT_TIMEOUT_IN_MICROSECS = SELECT_TIMEOUT_IN_MS * 1000,
    };

    ErrVal InitNetIOSystem();

    // Creating and deleting a network blockIO.
    CNetBlockIO *AllocNetBlockIO(
                    CParsedUrl *pUrl,
                    OSSocket socketID,
                    int32 connectionFlags,
                    CAsyncBlockIOCallback *pCallback);
    ErrVal SetSocketBufSizeImpl(OSSocket sock, bool writeBuf, int numBytes);
    void DisconnectSocket(CNetBlockIO *netIO);

    // These are functions performed in the main select thread.
    ErrVal ProcessActiveSockets();
    void ProcessReadEvent(CNetBlockIO *connection);
    void ProcessWriteEvent(CNetBlockIO *connection);
    void ProcessExceptionEvent(CNetBlockIO *connection);
    void AdjustBlockIOs();
    void AcceptConnection(
                    CNetBlockIO *serverConnection,
                    OSSocket socketID,
                    struct sockaddr_in *netAddr);
    void ReportSocketIsActive(
                    CNetBlockIO *connection,
                    ErrVal wakeUpErr,
                    int32 op);
    ErrVal DrainNotificationData(OSSocket sock);
    void SafeCloseSocket(OSSocket sock, bool fUDP);
    ErrVal WakeSelectThread();

    ErrVal MakeSocketNonBlocking(OSSocket socket);

    // This is our network address.
    struct sockaddr_in      m_LocalAddr;

    // The state of the connect thread.
    CSimpleThread           *m_pSelectThread;
    bool                    m_SelectThreadIsRunning;
    OSSocket                m_WakeUpThreadSend;
    OSSocket                m_WakeUpThreadListener;
    OSSocket                m_WakeUpThreadReceive;
    struct sockaddr_in      m_wakeUpConnAddr;
    bool                    m_fPendingWakeupMessage;
    bool                    m_StopSelectThread;
    CRefEvent               *m_pSelectThreadStopped;

    // The socket list used by the select thread.
    fd_set                  m_ReadSocks;
    fd_set                  m_WriteSocks;
    fd_set                  m_ExceptionSocks;
    fd_set                  m_ResultReadSocks;
    fd_set                  m_ResultWriteSocks;
    fd_set                  m_ResultExceptionSocks;
    OSSocket                m_FdRange;
    OSSocket                m_ResultFdRange;

    // All active network connections.
    CNameTable              *m_ActiveSocketTable;
    int32                   m_MaxNumBlockIOs;

    // Async close queue
    CQueueList<CNetBlockIO> m_PendingCloseList;
    CRefEvent               *m_pEmptyPendingCloseBlockIOs;

    uint32                  m_NumBytesPerSocketBuffer;
    uint64                  m_PrevCheckForTimeoutsTime;
}; // CNetIOSystem

static CNetIOSystem *g_pNetIOSystemImpl = NULL;






/////////////////////////////////////////////////////////////////////////////
//
// [InitializeFileBlockIO]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
InitializeNetBlockIO() {
    g_pNetIOSystemImpl = newex CNetIOSystem;
    if (NULL == g_pNetIOSystemImpl) {
        returnErr(EFail);
    }

    g_pNetIOSystem = g_pNetIOSystemImpl;
    returnErr(ENoErr);
} // InitializeNetBlockIO.





/////////////////////////////////////////////////////////////////////////////
//
// [CNetBlockIO]
//
/////////////////////////////////////////////////////////////////////////////
CNetBlockIO::CNetBlockIO() : m_PendingCloseList(this) {
    m_MediaType = NETWORK_MEDIA;
    m_fSeekable = false;

    m_Socket = NULL_SOCKET;

    m_PendingWrites.ResetQueue();
    m_PendingReads.ResetQueue();

    m_CurrentTimeoutRemainingInMs = -1;
    m_NumTimeouts = 0;
    m_CurrentTimeoutOp = CIOBuffer::NO_OP;
    m_ReadTimeout = DEFAULT_READ_TIMEOUT_IN_MS;
    m_ConnectTimeout = DEFAULT_CONNECT_TIMEOUT_IN_MS;
} // CNetBlockIO.





/////////////////////////////////////////////////////////////////////////////
//
// [~CNetBlockIO]
//
/////////////////////////////////////////////////////////////////////////////
CNetBlockIO::~CNetBlockIO() {
    if (NULL_SOCKET != m_Socket) {
        DEBUG_WARNING("net Block IO closing a valid handle.");
    }
} // ~CNetBlockIO.





/////////////////////////////////////////////////////////////////////////////
//
// [Resize]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNetBlockIO::Resize(int64 newLength) {
    AutoLock(m_pLock);
    RunChecks();

    if (newLength < 0) {
        returnErr(EFail);
    }

    DEBUG_LOG("CNetBlockIO::Resize: new length = " INT64FMT ", old length = " INT64FMT,
               newLength, m_MediaSize);

    m_MediaSize = newLength;
    returnErr(ENoErr);
} // Resize.





/////////////////////////////////////////////////////////////////////////////
//
// [Flush]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNetBlockIO::Flush() {
    RunChecks();
    returnErr(ENoErr);
} // Flush.




/////////////////////////////////////////////////////////////////////////////
//
// [Close]
//
// Closing a connection will cause the connection to be deleted
// as well. To avoid a race condition with the select thread
// (which has its own copy of the object), this routine marks
// the connection as doomed, and wakes up the select thread.
// Only the select thread actually deletes connections. When
// the select thread checks for timeouts, it also checks for
// doomed connections.
/////////////////////////////////////////////////////////////////////////////
void
CNetBlockIO::Close() {

    AutoLock(m_pLock);
    RunChecksOnce();

    DEBUG_LOG("CNetBlockIO::Close: pBlockIOPtr = %p.", this);

    CAsyncBlockIO::Close();

    // To avoid a race condition with the select thread,
    // we don't delete the socket here. Instead, mark the
    // socket as doomed and let the select thread delete it.
    if (NULL_SOCKET != m_Socket) {
        (void) PrepareToDisconnect();
    }
} // Close






/////////////////////////////////////////////////////////////////////////////
//
// [CheckState]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNetBlockIO::CheckState() {
    ErrVal err = ENoErr;
    AutoLock(m_pLock);

    err = CAsyncBlockIO::CheckState();
    if (err) {
        returnErr(EFail);
    }

    if (NETWORK_MEDIA != m_MediaType) {
        gotoErr(EFail);
    }

    // m_Socket may be NULL_SOCKET. This just means
    // we have disconnected from the socket. If a server sends all the
    // data to us and then disconnects, we will disconnect even though we
    // still have data.

abort:
    returnErr(ENoErr);
} // CheckState.






/////////////////////////////////////////////////////////////////////////////
//
// [DoPendingRead]
//
// This is called in the select thread when there is data ready to read.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNetBlockIO::DoPendingRead(
                CIOBuffer *pBuffer,
                bool *pReadFullBuffer,
                bool *pFinished) {
    ErrVal err = ENoErr;
    int32 bytesRead;
    int32 actualIOSize;
    socklen_t fromLength;
    int32 numRetries = 0;
    bool fIgnoreError = false;
    int32 lastErr = 0;
    AutoLock(m_pLock);
    RunChecks();

    DEBUG_LOG("CNetBlockIO::DoPendingRead: pBlockIOPtr = %p, buffer = %p.",
                this, pBuffer);

    if ((NULL == pBuffer)
        || (NULL == pFinished)
        || (NULL == pReadFullBuffer)) {
        gotoErr(EFail);
    }
    *pFinished = false;
    *pReadFullBuffer = false;

    // Don't read more than 1 block per IO on a network.
    actualIOSize = pBuffer->m_BufferSize;
    if (actualIOSize > g_pNetIOSystemImpl->GetDefaultBytesPerBlock()) {
        actualIOSize = g_pNetIOSystemImpl->GetDefaultBytesPerBlock();
        DEBUG_LOG("CNetBlockIO::DoPendingRead: Clipping actualIOSize to block size. new Size = %d.", actualIOSize);
    }
    DEBUG_LOG("CNetBlockIO::DoPendingRead: actualIOSize = %d.", actualIOSize);


    // Iterate until we have a system call that is not interrupted.
    // This is only an issue on linux.
    while (1) {
        if (m_BlockIOFlags & CNetBlockIO::UDP_SOCKET) {
            int recvFromFlags = 0;
#if WIN32
            // The recvfrom call will complete even if only part of a message has been received.
            recvFromFlags = MSG_PARTIAL;
#endif

            fromLength = sizeof(struct sockaddr_in);
            bytesRead = recvfrom(
                            m_Socket,
                            pBuffer->m_pLogicalBuffer,
                            actualIOSize,
                            recvFromFlags,
                            (struct sockaddr *) &(pBuffer->m_udpDatagramSource),
                            &fromLength);
            // Get the last error immediately after the system call.
            // Anything code, even a DEBUG_LOG, may touch a file and
            // change the last error.
            lastErr = GET_LAST_ERROR();

            DEBUG_LOG("CNetBlockIO::DoPendingRead: Call recvfrom on UDP socket. result = %d.",
                        bytesRead);
        } else {
            bytesRead = recv(m_Socket, pBuffer->m_pLogicalBuffer, actualIOSize, 0);
            // Get the last error immediately after the system call.
            // Anything code, even a DEBUG_LOG, may touch a file and
            // change the last error.
            lastErr = GET_LAST_ERROR();

            DEBUG_LOG("CNetBlockIO::DoPendingRead: Call recvfrom on TCP socket. result = %d.",
                        bytesRead);
        }
#if LINUX
        if ((bytesRead < 0)
            && (IGNORE_SYSTEMCALL_ERROR())
            && (numRetries < MAX_SYSTEM_CALL_INTERRUPTS)) {
            DEBUG_LOG("CNetBlockIO::DoPendingRead: Ignoring EPIPE.");
            numRetries++;
            continue;
        }
#endif
        break;
    } // while (1)

    // Warning.
    // If bytesRead == 0, then this is a peer reset event. That cannot be ignored.
    // It might be a WOULD_BLOCK event (and ok to ignore) only when bytesRead == -1.
    // For example, Linux will return bytesRead = 0, and errno = WOULD_BLOCK when the
    // peer disconnects.
    if (bytesRead < 0) {
        DEBUG_LOG("CNetBlockIO::DoPendingRead: recv returned %d, lastErr = %d",
              bytesRead, lastErr);

        if ((IO_WOULD_BLOCK(lastErr))
                || (0 == actualIOSize)
                || (IO_NOT_CONNECTED_ERROR(lastErr))) {
            //DEBUG_LOG("CNetBlockIO::DoPendingRead: recv returned EWOULDBLOCK");
            fIgnoreError = true;
        } else {
            DEBUG_LOG("CNetBlockIO::DoPendingRead: recv returned error %d", lastErr);
            fIgnoreError = false;
        }
    } // if (bytesRead < 0)


    if ((bytesRead <= 0) && (!fIgnoreError)) {
        // Any error completes a read.
        *pFinished = true;

        // This is not a bad error, web servers may mark the
        // the document by closing the pBlockIO.
        err = EEOF;

        DEBUG_LOG("CNetBlockIO::DoPendingRead: Returning EEOF");

        pBuffer->m_PosInMedia = m_MediaSize;

        FinishIO(pBuffer, EEOF, 0);
    } else if (bytesRead > 0) {
        // Any amount of data counts as a complete read.
        *pFinished = true;

        if (bytesRead == actualIOSize) {
            *pReadFullBuffer = true;
        }

        pBuffer->m_PosInMedia = m_MediaSize;
        m_MediaSize += bytesRead;

        DEBUG_LOG("CNetBlockIO::DoPendingRead: Read %d bytes, *pReadFullBuffer = %d",
                    bytesRead, *pReadFullBuffer);

        FinishIO(pBuffer, ENoErr, bytesRead);
    } // reading data.

abort:
    returnErr(err);
} // DoPendingRead.








/////////////////////////////////////////////////////////////////////////////
//
// [DoPendingWrite]
//
// This is called in the select thread when the socket is ready to
// write more data.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNetBlockIO::DoPendingWrite(CIOBuffer *pBuffer, bool *pFinished) {
    ErrVal err = ENoErr;
    int32 bytesWritten;
    char *dataPtr;
    int32 bufferSize;
    int32 lastErr = 0;
    AutoLock(m_pLock);
    RunChecks();
    int32 numRetries = 0;

    if ((NULL == pBuffer) || (NULL == pFinished)) {
        gotoErr(EFail);
    }
    *pFinished = false;

    // IO does NOT has to be block-aligned for a network device.
    dataPtr = pBuffer->m_pLogicalBuffer + pBuffer->m_StartWriteOffset;
    bufferSize = pBuffer->m_NumValidBytes - pBuffer->m_StartWriteOffset;

    DEBUG_LOG("CNetBlockIO::DoPendingWrite: pBlockIOPtr = %p, buffer = %p.", this, pBuffer);
    DEBUG_LOG("CNetBlockIO::DoPendingWrite: dataPtr = %p, bufferSize = %d.", dataPtr, bufferSize);

    // Write all of the bytes that are currently in the buffer.
    // We may have to loop several times to ignore system call failures on Linux.
    while (1) {
        if (m_BlockIOFlags & CNetBlockIO::UDP_SOCKET) {
            bytesWritten = sendto(
                                m_Socket,
                                dataPtr,
                                bufferSize,
                                // MSG_NOSIGNAL means dont send EPIPE on peer reset.
                                MSG_NOSIGNAL , // Flags
                                (const struct sockaddr *) &m_UDPDatagramDest,
                                sizeof(struct sockaddr_in));
        } else {
            bytesWritten = send(
                            m_Socket,
                            dataPtr,
                            (int) bufferSize,
                            // MSG_NOSIGNAL means dont send EPIPE on peer reset.
                            MSG_NOSIGNAL); // Flags
        }
        // Get the last error immediately after the system call.
        // Anything code, even a DEBUG_LOG, may touch a file and
        // change the last error.
        lastErr = GET_LAST_ERROR();

#if LINUX
        if ((bytesWritten < 0)
            && (IGNORE_SYSTEMCALL_ERROR())
            && (numRetries < MAX_SYSTEM_CALL_INTERRUPTS)) {
            DEBUG_LOG("CNetBlockIO::DoPendingWrite: Ignoring EPIPE.");
            numRetries++;
            continue;
        }
#endif

        break;
    } // while (1)

    if ((SOCKET_ERROR == bytesWritten) && (IO_WOULD_BLOCK(lastErr))) {
        //DEBUG_LOG("CNetBlockIO::DoPendingWrite: send returned EWOULDBLOCK");
        bytesWritten = 0;
    } else if (bytesWritten < 0) {
        DEBUG_LOG("CNetBlockIO::DoPendingWrite: send failed. errno = %d.",
                  lastErr);

        // Any error completes a write.
        *pFinished = true;

        // This is not a bad error, web servers may mark the
        // the document by closing the pBlockIO.
        err = EPeerDisconnected;
        pBuffer->m_Err = EPeerDisconnected;
    } else { // if (bytesWritten > 0)
        DEBUG_LOG("CNetBlockIO::DoPendingWrite: send succeeded. Sent %d bytes", 
                  bytesWritten);
        DEBUG_LOG("CNetBlockIO::DoPendingWrite: pBuffer->m_StartWriteOffset = %d", 
                  pBuffer->m_StartWriteOffset);
        DEBUG_LOG("CNetBlockIO::DoPendingWrite: pBuffer->m_NumValidBytes = %d", 
                  pBuffer->m_NumValidBytes);

        // Do not change m_MediaSize for writing to the socket, we only change
        // the mediaSize when we receive new data from the socket.

        pBuffer->m_Err = ENoErr;
        pBuffer->m_StartWriteOffset += bytesWritten;
        if (pBuffer->m_StartWriteOffset >= pBuffer->m_NumValidBytes) {
            DEBUG_LOG("CNetBlockIO::DoPendingWrite: *pFinished = true");
            *pFinished = true;
        }
    } // writing data.

abort:
    returnErr(err);
} // DoPendingWrite.






/////////////////////////////////////////////////////////////////////////////
//
// [ReadBlockAsyncImpl]
//
// This is called by the client code to read the socket. We try to read
// immediately, and if that fails then we queue the buffer up to be read when
// select fires.
/////////////////////////////////////////////////////////////////////////////
void
CNetBlockIO::ReadBlockAsyncImpl(CIOBuffer *pBuffer) {
    ErrVal err = ENoErr;
    ErrVal err2 = ENoErr;
    bool fFinished = false;
    bool ReadFullBuffer = false;
    AutoLock(m_pLock);
    RunChecks();

    if (NULL == pBuffer) {
        return;
    }

    // Check if we can read right away. This is a fast path, and will avoid a lot
    // of thread jumps.
    err = DoPendingRead(pBuffer, &ReadFullBuffer, &fFinished);
    if ((err) && (EEOF != err)) {
        DEBUG_LOG("ReadBlockAsyncImpl. DoPendingRead returned an error " ERRFMT, err);
        fFinished = false;
        gotoErr(err);
    }

    // If there is more data to read, then add this socket to the list of
    // pending reads. We will complete the read when select fires.
    if (!fFinished) {
        // There is no race condition with the select thread. If the
        // socket becomes readable/writeable while we are in the process
        // of adding it to the select list, then select will simply
        // immediately return.

        // Put this buffer on the queue so the select thread can find
        // it later when we can resume the IO operation.
        m_PendingReads.InsertTail(&(pBuffer->m_BlockIOBufferList));
        ADDREF_OBJECT(pBuffer);

        m_BlockIOFlags |= WAITING_TO_READ;

        // We do not have to tell select() to listen on this socket,
        // since it listens for reads on all sockets.
    } // (!Finished)


    // Close the socket so it is removed from the read select list.
    // Otherwise, the socket will continually select that it is ready
    // to read.
    if (EEOF == err) {
        DEBUG_LOG("ReadBlockAsyncImpl. Socket closed, calling PrepareToDisconnect");
        err2 = PrepareToDisconnect();
        if (err2) {
            DEBUG_LOG("ReadBlockAsyncImpl. PrepareToDisconnect failed. err2 = %d", err2);
            //err = err2;
        }
    } // if (EEOF == err)

abort:
    if ((err) && !(fFinished)) {
        FinishIO(pBuffer, err, 0);
    }
} // ReadBlockAsyncImpl.






/////////////////////////////////////////////////////////////////////////////
//
// [WriteBlockAsyncImpl]
//
// This is called by the client code to write the socket. We try to write
// immediately, and if that fails then we queue the buffer up to be written
// when select fires.
/////////////////////////////////////////////////////////////////////////////
void
CNetBlockIO::WriteBlockAsyncImpl(CIOBuffer *pBuffer) {
    ErrVal err = ENoErr;
    bool fFinished = false;
    AutoLock(m_pLock);
    RunChecks();

    if (NULL == pBuffer) {
        return;
    }

    DEBUG_LOG("CNetBlockIO::WriteBlockAsyncImpl calls DoPendingWrite");
    err = DoPendingWrite(pBuffer, &fFinished);
    if (err) {
        fFinished = true;
    }

    if (fFinished) {
        FinishIO(pBuffer, err, pBuffer->m_NumValidBytes);
    } else { // if (!fFinished)
        // Unlike read, we have to tell the select thread to wait on this
        // socket for writing.
        DEBUG_LOG("CNetBlockIO::WriteBlockAsyncImpl. Schedule a write for later.");

        // There is no race condition with the select thread. If the
        // socket becomes readable/writeable while we are in the process
        // of adding it to the select list, then select will simply
        // immediately return.
        if (g_pNetIOSystemImpl->m_pLock) {
            g_pNetIOSystemImpl->m_pLock->Lock();
        }

#if WIN32
        if (g_pNetIOSystemImpl->m_WriteSocks.fd_count >= FD_SETSIZE) {
            DEBUG_LOG("CNetBlockIO::WriteBlockAsyncImpl. Too many sockets (%d)",
                      g_pNetIOSystemImpl->m_WriteSocks.fd_count);
            err = ETooManySockets;
        } else
#endif
        {
            // Put this buffer on the queue so the select thread can find
            // it later when we can resume the IO operation.
            m_PendingWrites.InsertTail(&(pBuffer->m_BlockIOBufferList));
            ADDREF_OBJECT(pBuffer);

            m_BlockIOFlags |= WAITING_TO_WRITE;
            StartTimeout(CIOBuffer::WRITE);

            // Tell select() to listen on this socket as well.
            if (!(FD_ISSET(m_Socket, &(g_pNetIOSystemImpl->m_WriteSocks)))) {
                FD_SET(m_Socket, &(g_pNetIOSystemImpl->m_WriteSocks));
#if LINUX
                if (m_Socket >= g_pNetIOSystemImpl->m_FdRange) {
                    g_pNetIOSystemImpl->m_FdRange = m_Socket + 1;
                }
#endif // LINUX
            } // if (!(FD_ISSET(m_Socket, &(g_pNetIOSystemImpl->m_WriteSocks))))
        }

        if (g_pNetIOSystemImpl->m_pLock) {
            g_pNetIOSystemImpl->m_pLock->Unlock();
        }

        // Tell the select thread to start listening to this new
        // socket. We added the socket to the fd set, and
        // select uses the latest fd set each time it is called.
        (void) g_pNetIOSystemImpl->WakeSelectThread();
    } // if (!fFinished)

    if ((err) && (!fFinished)) {
        FinishIO(pBuffer, err, 0);
    }
} // WriteBlockAsyncImpl







/////////////////////////////////////////////////////////////////////////////
//
// [TestBreakingNetworkConnection]
//
/////////////////////////////////////////////////////////////////////////////
void
CNetBlockIO::TestBreakingNetworkConnection() {
    AutoLock(m_pLock);

    // Avoid closing the same blockIO twice.
    if (!(m_BlockIOFlags & CNetBlockIO::DISCONNECT_SOCKET)) {
        DEBUG_LOG("CNetBlockIO::TestBreakingNetworkConnection");

        // To avoid a race condition with the select thread,
        // we don't delete the socket here. Instead, mark the
        // socket as doomed and let the select thread delete it.
        if (g_pNetIOSystemImpl->m_pLock) {
            g_pNetIOSystemImpl->m_pLock->Lock();
        }

        m_BlockIOFlags |= CNetBlockIO::DISCONNECT_SOCKET;
        m_BlockIOFlags |= CNetBlockIO::SIMULATE_NETWORK_ERROR;

        g_pNetIOSystemImpl->m_PendingCloseList.InsertTail(&(m_PendingCloseList));
        ADDREF_THIS();

        if (g_pNetIOSystemImpl->m_pLock) {
            g_pNetIOSystemImpl->m_pLock->Unlock();
        }

        // Tell the select thread to delete this socket.
        (void) g_pNetIOSystemImpl->WakeSelectThread();
    }
} // TestBreakingNetworkConnection.






/////////////////////////////////////////////////////////////////////////////
//
// [PrepareToDisconnect]
//
// This tells the main select thread the close this socket. It will eventually
// be closed in the select thread.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNetBlockIO::PrepareToDisconnect() {
    ErrVal err = ENoErr;
    CIOBuffer *pBuffer = NULL;
    AutoLock(m_pLock);
    RunChecks();

    DEBUG_LOG("CNetBlockIO::PrepareToDisconnect: pNetBlockIO = %p", this);

    // Avoid closing the same blockIO twice.
    if (m_BlockIOFlags & CNetBlockIO::DISCONNECT_SOCKET) {
        DEBUG_LOG("CNetBlockIO::PrepareToDisconnect. Socket is already disconnected");
        gotoErr(ENoErr);
    }

    // Discard any buffers that were allocated.
    while (1) {
        pBuffer = m_PendingReads.RemoveHead();
        if (NULL == pBuffer) {
            break;
        }
        m_NumActiveReads -= 1;
        RELEASE_OBJECT(pBuffer);
    }

    // To avoid a race condition with the select thread,
    // we don't delete the socket here. Instead, mark the
    // socket as doomed and let the select thread delete it.
    if (g_pNetIOSystemImpl->m_pLock) {
        g_pNetIOSystemImpl->m_pLock->Lock();
    }

    if ((m_NumActiveReads > 0) || (m_NumActiveWrites > 0)) {
        DEBUG_WARNING("There are active reads or writes.");
    }

    m_BlockIOFlags |= CNetBlockIO::DISCONNECT_SOCKET;
    g_pNetIOSystemImpl->m_PendingCloseList.InsertTail(&(m_PendingCloseList));
    ADDREF_THIS();

    DEBUG_LOG("CNetBlockIO::PrepareToDisconnect: Put pNetBlockIO %p on PendingCloseList", this);

    if (g_pNetIOSystemImpl->m_pLock) {
        g_pNetIOSystemImpl->m_pLock->Unlock();
    }

    // Tell the select thread to delete this socket.
    (void) g_pNetIOSystemImpl->WakeSelectThread();

abort:
    returnErr(err);
} // PrepareToDisconnect.






/////////////////////////////////////////////////////////////////////////////
//
// [CancelTimeout]
//
/////////////////////////////////////////////////////////////////////////////
void
CNetBlockIO::CancelTimeout(int32 opType) {
    AutoLock(m_pLock);

    if ((m_NumTimeouts > 0)
        && (m_CurrentTimeoutOp == opType)) {
        m_NumTimeouts--;
    }
} // CancelTimeout







/////////////////////////////////////////////////////////////////////////////
//
// [StartTimeout]
//
/////////////////////////////////////////////////////////////////////////////
void
CNetBlockIO::StartTimeout(int32 opType) {
    int32 value = 0;
    AutoLock(m_pLock);

    ASSERT(0 == m_NumTimeouts);

    switch (opType) {
    case CIOBuffer::READ:
        value = m_ReadTimeout;
        break;

    case CIOBuffer::WRITE:
        value = DEFAULT_WRITE_TIMEOUT_IN_MS;
        break;

    case CIOBuffer::IO_CONNECT:
        value = m_ConnectTimeout;
        break;

    default:
        break;
    }

    DEBUG_LOG("CNetBlockIO::StartTimeout: opType = %d, time = %d", opType, value);

    m_CurrentTimeoutRemainingInMs = value;
    m_CurrentTimeoutOp = opType;
    m_NumTimeouts++;
} // StartTimeout.






/////////////////////////////////////////////////////////////////////////////
//
// [CheckTimeout]
//
// This is called by the select thread to decide whether a socket has expired
// any timeouts.
/////////////////////////////////////////////////////////////////////////////
bool
CNetBlockIO::CheckTimeout(uint64 elapsedTime, int32 *pTimeoutOp) {
    AutoLock(m_pLock);

    if ((m_BlockIOFlags & CNetBlockIO::DISCONNECT_SOCKET)
        || (m_NumTimeouts <= 0)) {
        return(false);
    }

    if ((m_NumTimeouts > 0)
        && !(m_BlockIOFlags & NEVER_TIMEOUT)) {
        // m_CurrentTimeoutRemainingInMs is an uint64, so it CANNOT
        // become negative.
        if (elapsedTime >= (uint64) m_CurrentTimeoutRemainingInMs) {
            DEBUG_LOG("CNetBlockIO::CheckTimeout: Timing out blockIO %p. m_CurrentTimeoutOp = %d",
                        this, m_CurrentTimeoutOp);

            // Firing a timeout clears all pending activity.
            // This covers a weird case. There should be only one pending action
            // at any single time, so we only record one timeout. To be safe,
            // however, when that timeout fires, it cancels all pending
            // actions.
            m_NumTimeouts = 0;
            if (pTimeoutOp) {
                *pTimeoutOp = m_CurrentTimeoutOp;
            }

            return(true);
        }

        m_CurrentTimeoutRemainingInMs = m_CurrentTimeoutRemainingInMs - elapsedTime;
    }

    return(false);
} // CheckTimeout.






/////////////////////////////////////////////////////////////////////////////
//
// [CNetIOSystem]
//
/////////////////////////////////////////////////////////////////////////////
CNetIOSystem::CNetIOSystem() {
    m_pSelectThread = NULL;
    m_WakeUpThreadSend = NULL_SOCKET;
    m_WakeUpThreadListener = NULL_SOCKET;
    m_WakeUpThreadReceive = NULL_SOCKET;

    m_ActiveSocketTable = NULL;

    m_StopSelectThread = false;
    m_fPendingWakeupMessage = false;
    m_pSelectThreadStopped = NULL;

    m_LocalAddr.sin_addr.s_addr = INADDR_ANY;
    m_wakeUpConnAddr.sin_addr.s_addr = INADDR_ANY;

    FD_ZERO(&m_ReadSocks);
    FD_ZERO(&m_WriteSocks);
    FD_ZERO(&m_ExceptionSocks);
    m_FdRange = 0;

    FD_ZERO(&m_ResultReadSocks);
    FD_ZERO(&m_ResultWriteSocks);
    FD_ZERO(&m_ResultExceptionSocks);

    m_NumBytesPerSocketBuffer = 16000;

    m_PrevCheckForTimeoutsTime = 0;

    m_PendingCloseList.ResetQueue();
    m_pEmptyPendingCloseBlockIOs = NULL;

    m_MaxNumBlockIOs = FD_SETSIZE - 3;
} // CNetIOSystem.





/////////////////////////////////////////////////////////////////////////////
//
// [~CNetIOSystem]
//
/////////////////////////////////////////////////////////////////////////////
CNetIOSystem::~CNetIOSystem() {
    m_ActiveSocketTable = NULL;
    RELEASE_OBJECT(m_pEmptyPendingCloseBlockIOs);
} // ~CNetIOSystem.



/////////////////////////////////////////////////////////////////////////////
//
// [InitNetIOSystem]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNetIOSystem::InitNetIOSystem() {
    ErrVal err = ENoErr;
    int result;
    struct hostent *hostInfo = NULL;
    struct sockaddr_in wakeUpListenAddr;
    socklen_t addrLen;
    int32 lastErr = 0;

#if USE_SOCKS
    if (!g_InitedFirewall) {
        SOCKSinit("myProgram");
        g_InitedFirewall = true;
    }
#endif // USE_SOCKS

    m_NumBytesPerSocketBuffer = 16000;

    // Initialize the base class.
    err = CIOSystem::InitIOSystem();
    if (err) {
        gotoErr(err);
    }

    // Locks and events.
    m_pSelectThreadStopped = newex CRefEvent;
    if (NULL == m_pSelectThreadStopped) {
        gotoErr(EFail);
    }
    err = m_pSelectThreadStopped->Initialize();
    if (err) {
        gotoErr(EFail);
    }

    FD_ZERO(&m_ReadSocks);
    FD_ZERO(&m_WriteSocks);
    FD_ZERO(&m_ExceptionSocks);
    m_FdRange = 0;

    // Find out some standard information about this host.
    // Do this before we initialize the sockets so we can
    // use the host address for them.
    memset(&m_LocalAddr, 0, sizeof(struct sockaddr_in));
    m_LocalAddr.sin_addr.s_addr = INADDR_ANY;


    // Get the local IP address.
    //
    // If the name parameter is to gethostbyname, then this gets
    // the result for the local host.
    // On Windows, it seems that gethostbyname(NULL) works more reliably
    // than gethostbyname(gethostname()) on Windows.
#if WIN32
    DEBUG_LOG("CNetIOSystem::InitNetIOSystem. Calling gethostbyname(NULL)");
    hostInfo = gethostbyname(NULL);
    DEBUG_LOG("CNetIOSystem::InitNetIOSystem. gethostbyname(NULL) returned");
#endif

#if LINUX_DYNAMIC_LINK
    // The Linux version seems to work fine with a dynamically linked libc.
    // However, it fails with a statically linked glibc. It seems that the
    // name resolution library runs different code depending on host-specific
    // preferences, like whether to use DNS or other network infrastructure.
    // So, rather than writing 1 program that can call out to different local
    // name resolver services (like, via a local pipe or system call), the
    // C library is hard-compiled with the diffreent sources. This means that
    // libc cannot call gethostbyname in a portable way. Instead, you must dynamically
    // link with the local glibc on each different machine. If you really
    // need to run the static glibc, then you cannot use this call.
    {
        char buffer[512];
        int nameLength;

        DEBUG_LOG("CNetIOSystem::InitNetIOSystem. Calling gethostname");
        nameLength = gethostname(buffer, sizeof(buffer));
        DEBUG_LOG("CNetIOSystem::InitNetIOSystem. gethostname returned");
        if (nameLength < 0) {
            buffer[0] = 0;
        }
        hostInfo = gethostbyname(buffer);
    }
#endif
    if (hostInfo) {
        memcpy(&(m_LocalAddr.sin_addr), hostInfo->h_addr, hostInfo->h_length);
    }

    if (INADDR_ANY == m_LocalAddr.sin_addr.s_addr) {
#if WIN32
        m_LocalAddr.sin_addr.S_un.S_un_b.s_b1 = 127;
        m_LocalAddr.sin_addr.S_un.S_un_b.s_b2 = 0;
        m_LocalAddr.sin_addr.S_un.S_un_b.s_b3 = 0;
        m_LocalAddr.sin_addr.S_un.S_un_b.s_b4 = 1;
#elif LINUX
        m_LocalAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
#endif
    }
    // NOTE: the sin_family value is not set on WinNT, so explicitly
    // set this to AF_INET
    m_LocalAddr.sin_family = AF_INET;
    m_LocalAddr.sin_port = htons(0);


#if WIN32
    DEBUG_LOG("CNetIOSystem::InitNetIOSystem. Local address = %d.%d.%d.%d",
                m_LocalAddr.sin_addr.S_un.S_un_b.s_b1,
                m_LocalAddr.sin_addr.S_un.S_un_b.s_b2,
                m_LocalAddr.sin_addr.S_un.S_un_b.s_b3,
                m_LocalAddr.sin_addr.S_un.S_un_b.s_b4);
#elif LINUX
    DEBUG_LOG("CNetIOSystem::InitNetIOSystem. Local address = 0x%X",
              m_LocalAddr.sin_addr);
#endif


    // Create the sockets that wake up the select thread.
    m_WakeUpThreadSend = socket(AF_INET, SOCK_STREAM, 0);
    if (NULL_SOCKET == m_WakeUpThreadSend) {
        gotoErr(EFail);
    }
    m_WakeUpThreadListener = socket(AF_INET, SOCK_STREAM, 0);
    if (NULL_SOCKET == m_WakeUpThreadListener) {
        gotoErr(EFail);
    }

    // Make the connect socket asynch before we connect so
    // the connect will be asynch. Otherwise, we block on ourselves.
    // Do not make the listener asynch, however, because we
    // want the accept to be blocking.
    err = MakeSocketNonBlocking(m_WakeUpThreadSend);
    if (err) {
        gotoErr(err);
    }

    // Bind the socket to a name. This is necessary to connect to the socket.
    // Really, we just want the system to assign a port to us.
    // Because we don't rely on any specific port, we won't collide
    // with another application, or suffer potential DoS attacks.
    result = bind(
                m_WakeUpThreadListener,
                (struct sockaddr *) &m_LocalAddr,
                sizeof(struct sockaddr_in));
    // Get the last error immediately after the system call.
    // Anything code, even a DEBUG_LOG, may touch a file and
    // change the last error.
    lastErr = GET_LAST_ERROR();

    if (result < 0) {
        DEBUG_LOG("CNetIOSystem::InitNetIOSystem. bind failed. result = %d, lastErr = %d",
                    result, lastErr);
        gotoErr(EFail);
    }

    // Allow up to 5 connections to be waiting for an
    // accept, which is the most allowed by typical
    // implementations. We can still have any number of
    // active connections that have been accepted.
    result = listen(m_WakeUpThreadListener, 5);

    // Get the port that the wakeup connection socket is
    // listening to. We don't need the address, we just need
    // to know what port we were assigned.
    addrLen = sizeof(struct sockaddr_in);
    result = getsockname(
                m_WakeUpThreadListener,
                (struct sockaddr *) &wakeUpListenAddr,
                &addrLen);
    // Get the last error immediately after the system call.
    // Anything code, even a DEBUG_LOG, may touch a file and
    // change the last error.
    lastErr = GET_LAST_ERROR();

    if (result < 0) {
        DEBUG_LOG("CNetIOSystem::InitNetIOSystem. getsockname failed. result = %d, last err = %d",
                    result, lastErr);
        gotoErr(EFail);
    }

    memset(&m_wakeUpConnAddr, 0, sizeof(struct sockaddr_in));
    m_wakeUpConnAddr = m_LocalAddr;
    m_wakeUpConnAddr.sin_port = wakeUpListenAddr.sin_port;

    // Asynchronously connect to the listener socket. This will return an
    // asynch error until we call accept below.
    result = connect(
                m_WakeUpThreadSend,
                (struct sockaddr *) &m_wakeUpConnAddr,
                sizeof(struct sockaddr_in));
    // Get the last error immediately after the system call.
    // Anything code, even a DEBUG_LOG, may touch a file and
    // change the last error.
    lastErr = GET_LAST_ERROR();

    if ((result != 0) && !(IO_WOULD_BLOCK(lastErr))) {
        DEBUG_LOG("CNetIOSystem::InitNetIOSystem. connect() failed. result = %d, last err = %d",
                    result, lastErr);
        gotoErr(EFail);
    }

    // Accept the connection from ourselves, this will create a new socket
    // and establishing the connection with ourselves.
    addrLen = sizeof(struct sockaddr_in);
    m_WakeUpThreadReceive = accept(
                                m_WakeUpThreadListener,
                                (struct sockaddr*) &m_wakeUpConnAddr,
                                &addrLen);
    // Get the last error immediately after the system call.
    // Anything code, even a DEBUG_LOG, may touch a file and
    // change the last error.
    lastErr = GET_LAST_ERROR();

    if (NULL_SOCKET == m_WakeUpThreadReceive) {
        DEBUG_LOG("CNetIOSystem::InitNetIOSystem. accept() failed. last err = %d",
                    lastErr);
        gotoErr(EFail);
    }

    // This socket is always in the select list.
    ASSERT(!(FD_ISSET(m_WakeUpThreadReceive, &m_ReadSocks)));
    FD_SET(m_WakeUpThreadReceive, &m_ReadSocks);
#if LINUX
    if (m_WakeUpThreadReceive >= g_pNetIOSystemImpl->m_FdRange) {
        g_pNetIOSystemImpl->m_FdRange = m_WakeUpThreadReceive + 1;
    }
#endif // LINUX

    // Make the sockets we use to wake up the select thread non-blocking.
    err = MakeSocketNonBlocking(m_WakeUpThreadReceive);
    if (err) {
        gotoErr(err);
    }
    err = MakeSocketNonBlocking(m_WakeUpThreadListener);
    if (err) {
        gotoErr(err);
    }

    // Create the table of all active connections. This will map sockets to
    // the blockIO data structures.
    m_ActiveSocketTable = newex CNameTable;
    if (NULL == m_ActiveSocketTable) {
        gotoErr(EFail);
    }
    err = m_ActiveSocketTable->Initialize(0, 7);
    if (err) {
        gotoErr(err);
    }

    // Fork the select thread.
    err = CSimpleThread::CreateThread(
                              "NetIOSystemSelectThread",
                              SelectThreadProc,
                              NULL,
                              &m_pSelectThread);
    if (err) {
        gotoErr(err);
    }

abort:
    returnErr(err);
} // InitNetIOSystem.







/////////////////////////////////////////////////////////////////////////////
//
// [Shutdown]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNetIOSystem::Shutdown() {
    ErrVal err = ENoErr;

    // If we never lazily initialized the IO system, then there is nothing
    // to do.
    if (!m_fInitialized) {
        returnErr(ENoErr);
    }

    DEBUG_LOG("CNetIOSystem::Shutdown()");

    // Run any standard debugger checks.
    RunChecks();

    // Tell the select thread to stop.
    m_StopSelectThread = true;
    if ((NULL != m_pSelectThread)
        && (NULL != m_pSelectThreadStopped)
        && (m_pSelectThread->IsRunning())) {
        err = WakeSelectThread();
        // Wait for the select thread to stop.
        m_pSelectThreadStopped->Wait();
    }
    RELEASE_OBJECT(m_pSelectThreadStopped);


    if (NULL_SOCKET != m_WakeUpThreadSend) {
        SafeCloseSocket(m_WakeUpThreadSend, false);
        m_WakeUpThreadSend = NULL_SOCKET;
    }
    if (NULL_SOCKET != m_WakeUpThreadListener) {
       SafeCloseSocket(m_WakeUpThreadListener, false);
       m_WakeUpThreadListener = NULL_SOCKET;
    }
    if (NULL_SOCKET != m_WakeUpThreadReceive) {
        SafeCloseSocket(m_WakeUpThreadReceive, false);
        m_WakeUpThreadReceive = NULL_SOCKET;
    }

#if WIN32
    {
        BOOL result = WSACleanup();
    }
#endif

    if (m_ActiveSocketTable) {
        delete m_ActiveSocketTable;
    }
    m_ActiveSocketTable = NULL;

    err = CIOSystem::Shutdown();

    returnErr(err);
} // Shutdown.






/////////////////////////////////////////////////////////////////////////////
//
// [NetIO_LookupHost]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
NetIO_LookupHost(
                char *pName,
                uint16 portNum,
                struct sockaddr_in *pSockAddr) {
    ErrVal err = ENoErr;
    int result;
    uint32 addrVal;
    char portStr[32];
    struct addrinfo addressHints;
    struct addrinfo *addressResultList = NULL;
    struct addrinfo *currentAddr = NULL;


    if ((NULL == pName) || (NULL == pSockAddr)) {
        gotoErr(EFail);
    }

    DEBUG_LOG("NetIO_LookupHost. pName = %s, portNum = %d",
                pName, portNum);

    // First zero out any padding bytes and the sin_zero field.
    memset(pSockAddr, 0, sizeof(struct sockaddr_in));

    // Winsock does not support passing strings like 127.0.0.1 to
    // gethostbyname. As a result, we use inet_addr to detect and
    // translate such a name into an ip address.
    addrVal = inet_addr(pName);
    if (INADDR_NONE == addrVal) {
        if (0 == strcasecmpex(pName, "localhost")) {
#if WIN32
            pSockAddr->sin_addr.S_un.S_un_b.s_b1 = 127;
            pSockAddr->sin_addr.S_un.S_un_b.s_b2 = 0;
            pSockAddr->sin_addr.S_un.S_un_b.s_b3 = 0;
            pSockAddr->sin_addr.S_un.S_un_b.s_b4 = 1;
#elif LINUX
            pSockAddr->sin_addr.s_addr = inet_addr("127.0.0.1");
#endif
        } else {
            snprintf(portStr, sizeof(portStr), "%d", portNum);
            memset(&addressHints, 0, sizeof(addressHints));
            addressHints.ai_family = AF_INET;
            addressHints.ai_socktype = SOCK_STREAM;

            // getaddrinfo() is both thread-safe and supports IPv6.
            // Don't use gethostbyname, since it is not thread-safe.
#if LINUX_DYNAMIC_LINK
            DEBUG_LOG("NetIO_LookupHost. Calling getaddrinfo");
            result = getaddrinfo(pName, portStr, &addressHints, &addressResultList);
            DEBUG_LOG("NetIO_LookupHost. getaddrinfo returned. result = %d", result);
#else
	        // The Linux version seems to work fine with a dynamically linked libc.
	        // However, it fails with a statically linked glibc. It seems that the
	        // name resolution library runs different code depending on host-specific
	        // preferences, like whether to use DNS or other network infrastructure.
	        // So, rather than writing 1 program that can call out to different local
	        // name resolver services (like, via a local pipe or system call), the
	        // C library is hard-compiled with the diffreent sources. This means that
	        // libc cannot call gethostbyname in a portable way. Instead, you must
	        // dynamically link with the local glibc on each different machine.
	        // If you really need to run the static glibc, then you cannot use this
	        // call.
	        DEBUG_WARNING("Cant resolve network addresses with static linking");
            result = -1;
#endif

            if (0 != result) {
                DEBUG_LOG("NetIO_LookupHost. getaddrinfo failed. result = %d",
                            result);
                gotoErr(ENoHostAddress);
            }
            currentAddr = addressResultList;
            while (NULL != currentAddr) {
                if (AF_INET == currentAddr->ai_family) {
                    DEBUG_LOG("NetIO_LookupHost. getaddrinfo found a valid address");
                    // The address is in network byte order.
                    memcpy(pSockAddr, currentAddr->ai_addr, sizeof(struct sockaddr_in));
                    break;
                }
                currentAddr = currentAddr->ai_next;
            }
            if (NULL == currentAddr) {
                DEBUG_LOG("NetIO_LookupHost. getaddrinfo returned no matching address");
                gotoErr(ENoHostAddress);
            }
        } // calling getaddrinfo.
    } else
    {
        DEBUG_LOG("NetIO_LookupHost. inet_addr succeeded");
        pSockAddr->sin_addr.s_addr = addrVal;
    }

    // NOTE: the sin_family value is invalid on NT, so explicitly
    // set this to AF_INET
    pSockAddr->sin_family = AF_INET;
    pSockAddr->sin_port = htons(portNum);

abort:
    if (NULL != addressResultList) {
       freeaddrinfo(addressResultList);
    }

    returnErr(err);
} // NetIO_LookupHost.








/////////////////////////////////////////////////////////////////////////////
//
// [OpenBlockIO]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNetIOSystem::OpenBlockIO(
                    CParsedUrl *pUrl,
                    int32 options,
                    CAsyncBlockIOCallback *pCallback) {
    ErrVal err = ENoErr;
    CNetBlockIO *pBlockIO = NULL;
    int result = 0;
    bool locked = false;
    bool fIsUDP = false;
    OSSocket newSocketID = NULL_SOCKET;
    char cSaveChar;
    char *pName = NULL;
    char *pEndName = NULL;
    struct sockaddr_in sockAddr;
    int32 numRetries = 0;
    int32 lastErr = 0;
#if WIN32
    int on = -1;
#endif
    RunChecks();

    DEBUG_LOG("CNetIOSystem::OpenBlockIO()");

    options = options;
    if ((NULL == pUrl)
        || (NULL == pCallback)) {
        gotoErr(EFail);
    }

    DEBUG_LOG("CNetIOSystem::OpenBlockIO. URL scheme = %d", pUrl->m_Scheme);

    if ((CParsedUrl::URL_SCHEME_IP_ADDRESS != pUrl->m_Scheme)
        && (!(CParsedUrl::NETWORK_NAME & pUrl->m_Flags)
            || (NULL == pUrl->m_pHostName))) {
        gotoErr(EFail);
    }
    if ((CParsedUrl::URL_SCHEME_IP_ADDRESS == pUrl->m_Scheme)
        && (NULL == pUrl->m_pSockAddr)) {
        gotoErr(EFail);
    }


    // Lazily initialize the IO system when we open the first blockIO.
    if (!m_fInitialized) {
        err = InitNetIOSystem();
        if (err) {
            DEBUG_LOG("CNetIOSystem::OpenBlockIO. InitNetIOSystem failed. err = %d", err);
            gotoErr(err);
        }
    }
    if (NULL == m_ActiveSocketTable) {
        gotoErr(EFail);
    }

    // Look for the IP-address in the URL.
    // If this URL has an IP-address, then just use it.
    // DNS lookups are incredibly expensive.
    if (pUrl->m_pSockAddr) {
        sockAddr = *(pUrl->m_pSockAddr);
    } else {
        // Get the name of the host and temporarily turn it into a C-string.
        pName = pUrl->m_pHostName;
        pEndName = pUrl->m_pHostName + pUrl->m_HostNameSize;
        cSaveChar = *pEndName;
        *pEndName = 0;
        err = NetIO_LookupHost(pName, pUrl->m_Port, &sockAddr);
        *pEndName = cSaveChar;
        if (err) {
            DEBUG_LOG("CNetIOSystem::OpenBlockIO. NetIO_LookupHost failed. err = %d", err);
            gotoErr(err);
        }
    }

    // Create a new socket.
    newSocketID = socket(AF_INET, SOCK_STREAM, 0);
    if (NULL_SOCKET == newSocketID) {
        DEBUG_LOG("CNetIOSystem::OpenBlockIO. socket() failed");
        gotoErr(EFail);
    }
    DEBUG_LOG("CNetIOSystem::OpenBlockIO: Opened socket %d", newSocketID);

    // Make the socket non-blocking.
    // If we do this before connect, then the connect is asynchronous.
    // If we do this after connect, then the connect is synchronous.
    err = MakeSocketNonBlocking(newSocketID);
    if (err) {
        gotoErr(err);
    }

    // Set the read and write buffers.
    err = SetSocketBufSizeImpl(newSocketID, true, m_NumBytesPerSocketBuffer);
    if (err) {
        gotoErr(err);
    }
    err = SetSocketBufSizeImpl(newSocketID, false, m_NumBytesPerSocketBuffer);
    if (err) {
        gotoErr(err);
    }

    // Turn off Nagle's algorithm. This causes packets to be sent
    // immediately, rather than being buffered first on the local
    // host to group them into larger packets. We do the buffering,
    // so Nagle's algorithm would just get in the way. Normally,
    // this defaults to off, but the winsock 1.1 and 2.0 specs let
    // each winsock implementation decide what to default it to.
    //
    // It seems that NODELAY is only in Linux versions Linux 2.5.71
    // and later, so it is not portable.
#if WIN32
        result = setsockopt(
                      newSocketID,
                      IPPROTO_TCP,
                      TCP_NODELAY,
                      (const char *) &on,
                      sizeof(on));
        if (result < 0) {
           gotoErr(EFail);
        }
#endif // WIN32

    // Do not bind the socket, this function is for client connections,
    // not servers.

    // We only hold the lock while we add the connection to
    // the global list.
    if (m_pLock) {
        m_pLock->Lock();
        locked = true;
    }

#if WIN32
    if ((m_WriteSocks.fd_count >= FD_SETSIZE)
        || (m_ExceptionSocks.fd_count >= FD_SETSIZE)
        || (m_ReadSocks.fd_count >= FD_SETSIZE)) {
        DEBUG_LOG("CNetIOSystem::OpenBlockIO. Failing because too many sockets (%d, %d, %d)",
                    m_WriteSocks.fd_count,
                    m_ReadSocks.fd_count,
                    m_ExceptionSocks.fd_count);
        gotoErr(ETooManySockets);
    }
#endif

    pBlockIO = AllocNetBlockIO(pUrl, newSocketID, 0, pCallback);
    if (NULL == pBlockIO) {
        DEBUG_LOG("CNetIOSystem::OpenBlockIO. AllocNetBlockIO failed.");
        gotoErr(ETooManySockets);
    }
    // At this point, the connection is in the select thread list,
    // so the select thread is responsible for finally deleting it.

    pBlockIO->m_BlockIOFlags |= CNetBlockIO::WAITING_TO_CONNECT;
    pBlockIO->StartTimeout(CIOBuffer::IO_CONNECT);

    // Tell select() to report when this socket is connected.
    // Do this before we try to connect to avoid a race
    // condition of the result coming in between the time
    // we try to connect and the time we tell the select
    // thread to notify us.
    ASSERT(!(FD_ISSET(newSocketID, &m_WriteSocks)));
    ASSERT(!(FD_ISSET(newSocketID, &m_ExceptionSocks)));
    ASSERT(!(FD_ISSET(newSocketID, &m_ReadSocks)));

    FD_SET(newSocketID, &m_WriteSocks);
    FD_SET(newSocketID, &m_ExceptionSocks);

    // We always wait for reads on any socket.
    FD_SET(newSocketID, &m_ReadSocks);
#if LINUX
    if (newSocketID >= g_pNetIOSystemImpl->m_FdRange) {
        g_pNetIOSystemImpl->m_FdRange = newSocketID + 1;
    }
#endif // LINUX

    // Add this blockIO to the table that maps sockets to blockIO pointers.
    err = m_ActiveSocketTable->SetValueEx(
                                    (const char *) &(pBlockIO->m_Socket), // pKey
                                    sizeof(OSSocket),
                                    (const char *) pBlockIO, // userData
                                    pBlockIO); // table entry
    if (err) {
        DEBUG_LOG("CNetIOSystem::OpenBlockIO. SetValueEx failed.");
        gotoErr(err);
    }
    ADDREF_OBJECT(pBlockIO);

    if (m_pLock) {
        m_pLock->Unlock();
    }
    locked = false;

    // Tell the select thread to start listening to this
    // new socket. We added the socket to the fd set, and
    // select uses the latest fd set each time it is called.
    (void) WakeSelectThread();

    // TCP ONLY.
    // Connect to the server. This only works if the server
    // process is alive and ready to talk.
#if DD_DEBUG
    {
        char *pStaticBuffer = inet_ntoa(sockAddr.sin_addr);
        DEBUG_LOG("CNetIOSystem::OpenBlockIO connect to: %s", pStaticBuffer);
    }
#endif // DD_DEBUG

    // We may have to loop several times to ignore system call failures on Linux.
    while (1) {
        result = connect(
                    newSocketID,
                    (struct sockaddr *) &sockAddr,
                    sizeof(struct sockaddr_in));
        // Get the last error immediately after the system call.
        // Anything code, even a DEBUG_LOG, may touch a file and
        // change the last error.
        lastErr = GET_LAST_ERROR();

#if LINUX
        if ((result < 0)
            && (IGNORE_SYSTEMCALL_ERROR())
            && (numRetries < MAX_SYSTEM_CALL_INTERRUPTS)) {
            DEBUG_LOG("CNetBlockIO::OpenBlockIO: Ignoring EPIPE.");
            numRetries++;
            continue;
        }
#endif
        break;
    } // while (1)

    DEBUG_LOG("CNetIOSystem::OpenBlockIO connect returned result=%d, errno=%d",
                result, lastErr);

    if ((result != 0) && !(IO_WOULD_BLOCK(lastErr))) {
        DEBUG_LOG("CNetIOSystem::OpenBlockIO connect() failed, giving up");
        pBlockIO->Close();
        err = ENoResponse;
        gotoErr(err);
    }

    if ((pBlockIO)
        && !(pBlockIO->m_BlockIOFlags & CNetBlockIO::DISCONNECT_SOCKET)) {
        pBlockIO->m_BlockIOFlags |= CAsyncBlockIO::BLOCKIO_IS_OPEN;
    }

    RELEASE_OBJECT(pBlockIO);
    returnErr(ENoErr);

abort:
    if ((locked) && (m_pLock)) {
        m_pLock->Unlock();
    }

    if (pBlockIO) {
        (void) pBlockIO->Close();
        RELEASE_OBJECT(pBlockIO);
    } else { // if (!pBlockIO)
        if (NULL_SOCKET != newSocketID) {
            DEBUG_LOG("CNetIOSystem::OpenBlockIO. Call SafeCloseSocket on socket %d", 
                      newSocketID);
            SafeCloseSocket(newSocketID, fIsUDP);
        }
    }

    returnErr(err);
} // OpenBlockIO.







/////////////////////////////////////////////////////////////////////////////
//
// [OpenServerBlockIO]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNetIOSystem::OpenServerBlockIO(
                    bool fIsUDP,
                    uint16 portNum,
                    bool fUse127001Address,
                    CAsyncBlockIOCallback *pCallback,
                    CAsyncBlockIO **ppResultBlockIO) {
    ErrVal err = ENoErr;
    int result = 0;
    OSSocket newSocketID = NULL_SOCKET;
    int on = 1;
    CParsedUrl *pUrl = NULL;
    CNetBlockIO *pBlockIO = NULL;
    int32 lastErr = 0;
    RunChecks();

    if (NULL == ppResultBlockIO) {
        gotoErr(EFail);
    }
    *ppResultBlockIO = NULL;


    // Lazily initialize the IO system when we open the first blockIO.
    if (!m_fInitialized) {
        err = InitNetIOSystem();
        if (err) {
            DEBUG_LOG("CNetIOSystem::OpenServerBlockIO. InitNetIOSystem failed. err = %d", err);
            gotoErr(err);
        }
    }
    if (NULL == m_ActiveSocketTable) {
        DEBUG_LOG("CNetIOSystem::OpenServerBlockIO. NULL == m_ActiveSocketTable");
        gotoErr(EFail);
    }


    // Create a URL that has the name we will be listening on.
    pUrl = CParsedUrl::AllocateUrl(g_LocalServerURL, g_LocalServerURLLength, NULL);
    if (NULL == pUrl) {
        gotoErr(EFail);
    }
    pUrl->m_pSockAddr = (struct sockaddr_in *) memAlloc(sizeof(struct sockaddr_in));
    if (NULL == pUrl->m_pSockAddr) {
        gotoErr(EFail);
    }
    *(pUrl->m_pSockAddr) = m_LocalAddr;


    // Create a new socket.
    if (fIsUDP) {
        newSocketID = socket(AF_INET, SOCK_DGRAM, 0);
    } else {
        newSocketID = socket(AF_INET, SOCK_STREAM, 0);
    }
    if (NULL_SOCKET == newSocketID) {
        gotoErr(EFail);
    }
    DEBUG_LOG("CNetIOSystem::OpenServerBlockIO. Opened socket %d", newSocketID);


    err = MakeSocketNonBlocking(newSocketID);
    if (err) {
        DEBUG_LOG("CNetIOSystem::OpenServerBlockIO. MakeSocketNonBlocking failed, err = %d", err);
        gotoErr(err);
    }

    // Prevent a hacker application from also binding to our port.
#if 0
    on = TRUE;
    result = setsockopt(
                newSocketID,
                SOL_SOCKET,
                SO_EXCLUSIVEADDRUSE,
                (const char *) &on,
                sizeof(on));
    // Ignore any error.
    result = 0;
#endif

    on = 0;
    result = setsockopt(
                newSocketID,
                SOL_SOCKET,
                SO_REUSEADDR,
                (const char *) &on,
                sizeof(on));
    // Ignore any error.
    result = 0;


    // Some applications will try to connect to 127.0.0.1. In those
    // cases, we must bind to that address, not the real address of
    // the machine.
    if (fUse127001Address) {
#if WIN32
        pUrl->m_pSockAddr->sin_addr.S_un.S_un_b.s_b1 = 127;
        pUrl->m_pSockAddr->sin_addr.S_un.S_un_b.s_b2 = 0;
        pUrl->m_pSockAddr->sin_addr.S_un.S_un_b.s_b3 = 0;
        pUrl->m_pSockAddr->sin_addr.S_un.S_un_b.s_b4 = 1;
#elif LINUX
        pUrl->m_pSockAddr->sin_addr.s_addr = inet_addr("127.0.0.1");
#endif
    }
    if (portNum > 0) {
        pUrl->m_pSockAddr->sin_port = htons(portNum);
    } else
    {
        pUrl->m_pSockAddr->sin_port = htons(0);
    }

#if WIN32
    // On Windows, this will cause us to bind to all NIC's.
    pUrl->m_pSockAddr->sin_addr.s_addr = INADDR_ANY;
#endif

    // Bind the socket to a name. This may be very time consuming, so
    // do it outside the lock.
    result = bind(
                newSocketID,
                (struct sockaddr *) pUrl->m_pSockAddr,
                sizeof(struct sockaddr_in));
    // Get the last error immediately after the system call.
    // Anything code, even a DEBUG_LOG, may touch a file and
    // change the last error.
    lastErr = GET_LAST_ERROR();

    if (result < 0) {
        DEBUG_LOG("CNetIOSystem::OpenServerBlockIO. bind() failed, lastErr = %d",
                    lastErr);
        gotoErr(EFail);
    }

    // Allocate the BlockIO.
    { /////////////////////////////////////////////////
        AutoLock(m_pLock);

        pBlockIO = AllocNetBlockIO(pUrl, newSocketID, 0, pCallback);
        if (!pBlockIO) {
            DEBUG_LOG("CNetIOSystem::OpenServerBlockIO. AllocNetBlockIO failed");
            gotoErr(ETooManySockets);
        }

        if (fIsUDP) {
            pBlockIO->m_BlockIOFlags |= CNetBlockIO::UDP_SOCKET;
        } else {
            pBlockIO->m_BlockIOFlags |= CNetBlockIO::ACCEPT_INCOMING_CONNECTIONS;
        }

#if WIN32
        if (m_ReadSocks.fd_count >= FD_SETSIZE) {
            DEBUG_LOG("CNetIOSystem::OpenServerBlockIO. Too many sockets (%d)",
                        m_ReadSocks.fd_count);
            gotoErr(ETooManySockets);
        }
#endif // WIN32

        // Tell select() to listen on this socket for new connections.
        ASSERT(!(FD_ISSET(newSocketID, &m_ReadSocks)));
        FD_SET(newSocketID, &m_ReadSocks);

        ASSERT(!(FD_ISSET(newSocketID, &m_ExceptionSocks)));
        FD_SET(newSocketID, &m_ExceptionSocks);
#if LINUX
        if (newSocketID >= g_pNetIOSystemImpl->m_FdRange) {
            g_pNetIOSystemImpl->m_FdRange = newSocketID + 1;
        }
#endif // LINUX

        // Add this blockIO to the table that maps sockets to blockIO pointers.
        err = m_ActiveSocketTable->SetValueEx(
                                        (const char *) &(pBlockIO->m_Socket), // pKey
                                        sizeof(OSSocket),
                                        (const char *) pBlockIO, // userData
                                        pBlockIO); // table entry
        if (err) {
            DEBUG_LOG("CNetIOSystem::OpenServerBlockIO. SetValueEx failed (%d)", err);
            gotoErr(err);
        }

        ADDREF_OBJECT(pBlockIO);
    } /////////////////////////////////////////////////

    if (!fIsUDP) {
        // Allow up to 5 connections to be waiting for an accept, which is
        // the most allowed by typical implementations. We can still have
        // any number of active connections that have been accepted.
        result = listen(newSocketID, 5);
        if (result) {
            DEBUG_LOG("CNetIOSystem::OpenServerBlockIO. listen() failed, lastErr = %d",
                    lastErr);
            gotoErr(EFail);
        }
    }

    // Tell the select thread to start listening to this new
    // socket. We added the socket to the fd set, and
    // select uses the latest fd set each time it is called.
    (void) WakeSelectThread();

    *ppResultBlockIO = pBlockIO;
    pBlockIO = NULL;
    newSocketID = NULL_SOCKET;

abort:
    if (pBlockIO) {
        (void) pBlockIO->Close();
        RELEASE_OBJECT(pBlockIO);
    }
    if (NULL_SOCKET != newSocketID) {
        DEBUG_LOG("CNetIOSystem::OpenServerBlockIO. Call SafeCloseSocket on socket %d", newSocketID);
        SafeCloseSocket(newSocketID, fIsUDP);
        newSocketID = NULL_SOCKET;
    }

    RELEASE_OBJECT(pUrl);

    returnErr(err);
} // OpenServerBlockIO.






/////////////////////////////////////////////////////////////////////////////
//
// [ReceiveDataFromAcceptedConnection]
//
// This is called when a server wants to start receiving data from a blockIO
// that it accepted.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNetIOSystem::ReceiveDataFromAcceptedConnection(
                        CNetBlockIO *pBlockIO,
                        CAsyncBlockIOCallback *pCallback) {
    ErrVal err = ENoErr;
    bool locked = false;
    RunChecks();

    if ((NULL == m_ActiveSocketTable) || (NULL == m_pLock)) {
        gotoErr(EFail);
    }

    if ((NULL == pBlockIO) || (NULL == pCallback)) {
        gotoErr(EFail);
    }

    // We only hold the lock while we add the connection to
    // the global list.
    m_pLock->Lock();
    locked = true;

    pBlockIO->ChangeBlockIOCallback(pCallback);

#if WIN32
    // Start waiting for a read.
    if ((m_ExceptionSocks.fd_count >= FD_SETSIZE)
        || (m_ReadSocks.fd_count >= FD_SETSIZE)) {
       DEBUG_LOG("CNetIOSystem::ReceiveDataFromAcceptedConnection. Too many sockets (%d, %d)",
                  m_ExceptionSocks.fd_count,
                  m_ReadSocks.fd_count);
        gotoErr(ETooManySockets);
    }
#endif // WIN32

    ASSERT(!(FD_ISSET(pBlockIO->m_Socket, &m_ExceptionSocks)));
    ASSERT(!(FD_ISSET(pBlockIO->m_Socket, &m_ReadSocks)));
    FD_SET(pBlockIO->m_Socket, &m_ExceptionSocks);
    FD_SET(pBlockIO->m_Socket, &m_ReadSocks);
#if LINUX
    if (pBlockIO->m_Socket >= g_pNetIOSystemImpl->m_FdRange) {
        g_pNetIOSystemImpl->m_FdRange = pBlockIO->m_Socket + 1;
    }
#endif // LINUX

    err = m_ActiveSocketTable->SetValueEx(
                                    (const char *) &(pBlockIO->m_Socket), // pKey
                                    sizeof(OSSocket),
                                    (const char *) pBlockIO, // userData
                                    pBlockIO); // table entry
    if (err) {
        DEBUG_LOG("CNetIOSystem::ReceiveDataFromAcceptedConnection. SetValueEx failed (%d)", err);
        gotoErr(err);
    }
    ADDREF_OBJECT(pBlockIO);

    m_pLock->Unlock();
    locked = false;

    // Tell the select thread to start listening to this
    // new socket. We added the socket to the fd set, and
    // select uses the latest fd set each time it is called.
    (void) WakeSelectThread();

abort:
    if ((m_pLock) && (locked)) {
        m_pLock->Unlock();
    }

    returnErr(err);
} // ReceiveDataFromAcceptedConnection.




/////////////////////////////////////////////////////////////////////////////
//
// [NetIO_WaitForAllBlockIOsToClose]
//
/////////////////////////////////////////////////////////////////////////////
void
NetIO_WaitForAllBlockIOsToClose() {
    g_pNetIOSystemImpl->WaitForAllBlockIOsToClose();
}



/////////////////////////////////////////////////////////////////////////////
//
// [WaitForAllBlockIOsToClose]
//
/////////////////////////////////////////////////////////////////////////////
void
CNetIOSystem::WaitForAllBlockIOsToClose() {
    ErrVal err = ENoErr;
    CRefEvent *pSemaphore = NULL;

    DEBUG_LOG("CNetIOSystem::WaitForAllBlockIOsToClose");

    //////////////////////////////////////////////////////
    {
        AutoLock(m_pLock);

        // Discard any semaphore left over from a previous operation.
        RELEASE_OBJECT(m_pEmptyPendingCloseBlockIOs);

        m_pEmptyPendingCloseBlockIOs = newex CRefEvent;
        if (NULL == m_pEmptyPendingCloseBlockIOs) {
            return;
        }

        err = m_pEmptyPendingCloseBlockIOs->Initialize();
        if (err) {
            RELEASE_OBJECT(m_pEmptyPendingCloseBlockIOs);
            return;
        }

        pSemaphore = m_pEmptyPendingCloseBlockIOs;
        ADDREF_OBJECT(pSemaphore);
    }
    //////////////////////////////////////////////////////

    err = WakeSelectThread();
    if (err) {
        return;
    }
    pSemaphore->Wait();
    RELEASE_OBJECT(pSemaphore);

    //////////////////////////////////////////////////////
    {
        AutoLock(m_pLock);
        RELEASE_OBJECT(m_pEmptyPendingCloseBlockIOs);
    }
    //////////////////////////////////////////////////////
} // WaitForAllBlockIOsToClose.





/////////////////////////////////////////////////////////////////////////////
//
// [GetLocalAddr]
//
/////////////////////////////////////////////////////////////////////////////
void
CNetIOSystem::GetLocalAddr(struct sockaddr_in *pAddrResult) {
    if (NULL != pAddrResult) {
        memcpy(pAddrResult, &m_LocalAddr, sizeof(struct sockaddr_in));
    }
} // GetLocalAddr.







/////////////////////////////////////////////////////////////////////////////
//
// [GetLocalHostName]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNetIOSystem::GetLocalHostName(char *host, int32 maxHostLength) {
    int32 result;

    if (NULL == host) {
        returnErr(EFail);
    }

    DEBUG_LOG("CNetIOSystem::GetLocalHostName. Calling gethostname");
    result = gethostname(host, maxHostLength - 1);
    DEBUG_LOG("CNetIOSystem::GetLocalHostName. gethostname returned. result = %d",
                result);

    if (result) {
        returnErr(EFail);
    }

    returnErr(ENoErr);
} // GetLocalHostName.






/////////////////////////////////////////////////////////////////////////////
//
// [NetIO_GetLocalProxySettings]
//
/////////////////////////////////////////////////////////////////////////////
bool
NetIO_GetLocalProxySettings(
                        char **ppProxyServerName,
                        int *pProxyPort) {
    bool fFoundIt = false;
    char *pConfigStr = NULL;
#if WIN32
    HKEY hKey = NULL;
#endif


    if (NULL != ppProxyServerName) {
        *ppProxyServerName = NULL;
    }
    if (NULL != pProxyPort) {
        *pProxyPort = -1;
    }


    // First, look if there is a gateway in the config file.
    pConfigStr = g_pBuildingBlocksConfig->GetString(
                                             g_NetworkProxyHostConfigValueName,
                                             NULL);
    if (NULL != pConfigStr) {
        fFoundIt = true;

        DEBUG_LOG("NetIO_GetLocalProxySettings. Found a proxy (%s)", pConfigStr);

        if (NULL != ppProxyServerName) {
            *ppProxyServerName = strdupex(pConfigStr);
            if (NULL == *ppProxyServerName) {
                goto abort;
            }
        }
        if (NULL != pProxyPort) {
            *pProxyPort = g_pBuildingBlocksConfig->GetInt(g_NetworkProxyPortConfigValueName, 80);
        }

        goto abort;
    } // if (NULL != pConfigStr)
    else
    {
        DEBUG_LOG("NetIO_GetLocalProxySettings. No proxy");
    }

#if WIN32
    LONG lResult;
    DWORD valueType;
    static const char g_SettingsRegistryKey[] = "Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
    DWORD dwIntValue;
    char *pPortStr;
    DWORD length;
    char buffer[1024];

    lResult = RegCreateKeyExA(
                    HKEY_CURRENT_USER,
                    g_SettingsRegistryKey,
                    0, // reserved
                    NULL, // class
                    REG_OPTION_NON_VOLATILE,
                    KEY_READ | KEY_QUERY_VALUE,
                    NULL, // security attributes
                    &hKey,
                    NULL);
    if (ERROR_SUCCESS != lResult) {
        DEBUG_LOG("NetIO_GetLocalProxySettings. RegCreateKeyExA failed");
        goto abort;
    }

    length = sizeof(dwIntValue);
    lResult = RegQueryValueExA(
                  hKey,
                  "ProxyEnable",
                  NULL,
                  &valueType,
                  (uchar *) &dwIntValue,
                  &length);
    if ((ERROR_SUCCESS == lResult) && (!dwIntValue)) {
        DEBUG_LOG("NetIO_GetLocalProxySettings. RegQueryValueEx failed");
        goto abort;
    }

    length = sizeof(buffer);
    lResult = RegQueryValueExA(
                  hKey,
                  "ProxyServer",
                  NULL,
                  &valueType,
                  (uchar *) buffer,
                  &length);
    if ((ERROR_SUCCESS == lResult) && (REG_SZ == valueType)) {
        fFoundIt = true;

        DEBUG_LOG("NetIO_GetLocalProxySettings. Found a proxy (%s)", buffer);

        if (NULL != ppProxyServerName) {
            *ppProxyServerName = strdupex(buffer);
            if (NULL == *ppProxyServerName) {
                goto abort;
            }

            pPortStr = *ppProxyServerName;
            while ((*pPortStr) && (':' != *pPortStr)) {
                pPortStr++;
            }
            if (':' == *pPortStr) {
                *pPortStr = 0;
                pPortStr++;
                if (NULL != pProxyPort) {
                    CStringLib::StringToNumber(pPortStr, strlen(pPortStr), pProxyPort);
                }
            }
        }
    } // if ((ERROR_SUCCESS == lResult) && (REG_SZ == valueType))
    else
    {
        DEBUG_LOG("NetIO_GetLocalProxySettings. No proxy");
    }

    //HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Internet Settings\ProxyHttp1.1
    //HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Internet Settings\ProxyOverride
#endif


abort:
#if WIN32
    if (NULL != hKey) {
        RegCloseKey(hKey);
        hKey = NULL;
    }
#endif

    return(fFoundIt);
} // GetLocalProxySettings







/////////////////////////////////////////////////////////////////////////////
//
// [CheckState]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNetIOSystem::CheckState() {
    ErrVal err = ENoErr;
    AutoLock(m_pLock);

    if ((!m_fInitialized) || (m_StopSelectThread)) {
        returnErr(ENoErr);
    }

    if (NULL == m_ActiveSocketTable) {
        returnErr(EFail);
    }

    returnErr(err);
} // CheckState.




/////////////////////////////////////////////////////////////////////////////
//
// [AllocNetBlockIO]
//
/////////////////////////////////////////////////////////////////////////////
CNetBlockIO *
CNetIOSystem::AllocNetBlockIO(
                    CParsedUrl *pUrl,
                    OSSocket newSocketID,
                    int32 connectionFlags,
                    CAsyncBlockIOCallback *pCallback) {
    ErrVal err = ENoErr;
    CNetBlockIO *pBlockIO = NULL;
    AutoLock(m_pLock);


    if (m_ActiveBlockIOs.GetLength() >= m_MaxNumBlockIOs) {
       DEBUG_LOG("CNetIOSystem::AllocNetBlockIO. Too many sockets (%d)",
                  m_ActiveBlockIOs.GetLength());
        gotoErr(ETooManySockets);
    }

    pBlockIO = newex CNetBlockIO;
    if (!pBlockIO) {
        gotoErr(EFail);
    }

    // These are part of the base class.
    pBlockIO->m_MediaType = CAsyncBlockIO::NETWORK_MEDIA;
    pBlockIO->m_fSeekable = false;
    pBlockIO->m_pUrl = pUrl;
    ADDREF_OBJECT(pUrl);

    pBlockIO->m_MediaSize = 0;

    pBlockIO->m_pLock = CRefLock::Alloc();
    if (NULL == pBlockIO->m_pLock) {
        gotoErr(EFail);
    }

    // These are part of the net Block IO subclass.
    pBlockIO->m_Socket = newSocketID;
    pBlockIO->m_BlockIOFlags = connectionFlags;
    pBlockIO->m_BlockIOFlags &= ~CAsyncBlockIO::RESIZEABLE;

    pBlockIO->m_ActiveBlockIOs.ResetQueue();

    // Add this connection to the list of active connections.
    // This procedure assumes that the caller has already
    // acquired the lock.
    m_ActiveBlockIOs.InsertHead(&(pBlockIO->m_ActiveBlockIOs));
    ADDREF_OBJECT(pBlockIO);
    pBlockIO->m_pIOSystem = this;

    pBlockIO->ChangeBlockIOCallback(pCallback);

    return(pBlockIO);

abort:
    RELEASE_OBJECT(pBlockIO);

    return(NULL);
} // AllocNetBlockIO.






/////////////////////////////////////////////////////////////////////////////
//
// [DisconnectSocket]
//
// This routine is only called by the select thread, so connections are not
// deleted by other threads while select has a pointer to them.
//
// To close a connection, the client sets the delete flag and wakes up the
// select thread. The select thread sees this flag and calls this routine.
//
/////////////////////////////////////////////////////////////////////////////
void
CNetIOSystem::DisconnectSocket(CNetBlockIO *pBlockIO) {
    bool fRemovedItem = false;
    bool fIsUdp = false;

    DEBUG_LOG("CNetIOSystem::DisconnectSocket. pBlockIO = %p", pBlockIO);
    if ((!pBlockIO)
        || (!m_ActiveSocketTable)
        || (NULL_SOCKET == pBlockIO->m_Socket)) {
        return;
    }

    // Hold the lock while we remove the connection from
    // the global list of active connections.
    if (pBlockIO->m_pLock) {
        pBlockIO->m_pLock->Lock();
    }

    /////////////////////////////////////////
    // Get the global IOSystem lock.
    if (m_pLock) {
        m_pLock->Lock();
    }

    if (pBlockIO->m_BlockIOFlags & CNetBlockIO::UDP_SOCKET) {
       fIsUdp = true;
    }

    fRemovedItem = m_ActiveSocketTable->RemoveValue(
                                            (const char *) &(pBlockIO->m_Socket),
                                            sizeof(OSSocket));

    FD_CLR(pBlockIO->m_Socket, &m_ReadSocks);
    FD_CLR(pBlockIO->m_Socket, &m_WriteSocks);
    FD_CLR(pBlockIO->m_Socket, &m_ExceptionSocks);

    if (m_pLock) {
        m_pLock->Unlock();
    }
    /////////////////////////////////////////


    // Handle this specially if we are simulating a network error.
    if (pBlockIO->m_BlockIOFlags & CNetBlockIO::SIMULATE_NETWORK_ERROR) {
        DEBUG_LOG("CNetIOSystem::DisconnectSocket. Call SafeCloseSocket on socket %d",
                  pBlockIO->m_Socket);

        SafeCloseSocket(pBlockIO->m_Socket, fIsUdp);
        pBlockIO->m_Socket = NULL_SOCKET;

        pBlockIO->m_BlockIOFlags &= ~CNetBlockIO::DISCONNECT_SOCKET;

        // Force the error to be reported immediately.
        if (CNetBlockIO::WAITING_TO_WRITE & pBlockIO->m_BlockIOFlags) {
            ReportSocketIsActive(pBlockIO, ENoResponse, CIOBuffer::WRITE);
        } else if (CNetBlockIO::WAITING_TO_READ & pBlockIO->m_BlockIOFlags) {
            ReportSocketIsActive(pBlockIO, ENoResponse, CIOBuffer::READ);
        } else { // if (CNetBlockIO::WAITING_TO_CONNECT & pBlockIO->m_BlockIOFlags)
            ReportSocketIsActive(pBlockIO, ENoResponse, CIOBuffer::IO_CONNECT);
        }
    } else { // simulating a network error.
        DEBUG_LOG("CNetIOSystem::DisconnectSocket. Call SafeCloseSocket on socket %d",
                  pBlockIO->m_Socket);

        // Otherwise, this is just normally closing the socket.
        SafeCloseSocket(pBlockIO->m_Socket, fIsUdp);
        pBlockIO->m_Socket = NULL_SOCKET;
    } // normal close.

    if (pBlockIO->m_pLock) {
        pBlockIO->m_pLock->Unlock();
    }

    // The blockIO was AddRefed when it was put in m_ActiveSocketTable.
    if (fRemovedItem) {
        RELEASE_OBJECT(pBlockIO);
    }
} // DisconnectSocket.






/////////////////////////////////////////////////////////////////////////////
//
// [SelectThreadProc]
//
// This is the main select thread procedure.
/////////////////////////////////////////////////////////////////////////////
static void
SelectThreadProc(void *arg, CSimpleThread *threadState) {
    arg = arg;
    threadState = threadState;
    g_pNetIOSystemImpl->SelectThread();
} // SelectThreadProc.






/////////////////////////////////////////////////////////////////////////////
//
// [SelectThread]
//
// This is the main select thread.
/////////////////////////////////////////////////////////////////////////////
void
CNetIOSystem::SelectThread() {
    int numActiveSockets;
    struct timeval timeoutInterval;
    uint32 msSleepingForSelectError;
    bool fExitLoop = false;
    int32 numBytes;
    int32 lastErr = 0;


    msSleepingForSelectError = 100;

    m_PrevCheckForTimeoutsTime = GetTimeSinceBootInMs();
    numActiveSockets = 0;

    // This is the select thread loop.
    while (1) {
        // Copy the fd-sets into temporary variables. These will be modified
        // by other methods while we wait on select
        //
        // Ugh, this is expensive. There must be a cheaper system call than
        // select. Can I switch to poll? Does that work on windows?
        // I'd rather not have the windows and linux implemntations differ too much,
        // which is why I'm not using IOCompletion ports on windows. Maybe I will have
        // to if this becomes a bottleneck.
        if (m_pLock) {
            m_pLock->Lock();
        }

#if WIN32
        m_ResultReadSocks.fd_count = m_ReadSocks.fd_count;
        if (m_ResultReadSocks.fd_count > 0) {
            numBytes = sizeof(int32) * m_ReadSocks.fd_count;
            memcpy(m_ResultReadSocks.fd_array, m_ReadSocks.fd_array, numBytes);
        }
        m_ResultWriteSocks.fd_count = m_WriteSocks.fd_count;
        if (m_ResultWriteSocks.fd_count > 0) {
            numBytes = sizeof(int32) * m_WriteSocks.fd_count;
            memcpy(m_ResultWriteSocks.fd_array, m_WriteSocks.fd_array, numBytes);
        }
        m_ResultExceptionSocks.fd_count = m_ExceptionSocks.fd_count;
        if (m_ResultExceptionSocks.fd_count > 0) {
            numBytes = sizeof(int32) * m_ExceptionSocks.fd_count;
            memcpy(m_ResultExceptionSocks.fd_array, m_ExceptionSocks.fd_array, numBytes);
        }
        // This param is ignored on windows
        m_ResultFdRange = 0;
#elif LINUX
        // Only copies the part of the bit vector that I actually use.
        // numBytes = sizeof(m_ReadSocks);
        numBytes = (m_FdRange / 8);
        if ((numBytes * 8) < m_FdRange) {
           numBytes += 1;
        }
        ASSERT(numBytes <= (int32) sizeof(m_ReadSocks));

        memcpy(&m_ResultReadSocks, &m_ReadSocks, numBytes);
        memcpy(&m_ResultWriteSocks, &m_WriteSocks, numBytes);
        memcpy(&m_ResultExceptionSocks, &m_ExceptionSocks, numBytes);
        m_ResultFdRange = m_FdRange;
#endif
        if (m_pLock) {
            m_pLock->Unlock();
        }

        ASSERT_WIN32(m_ReadSocks.fd_count > 0);
        ASSERT_WIN32(m_ResultReadSocks.fd_count > 0);

        // This is how often we check for timeouts. This is around 5 seconds.
        // Reset the timeout interval before every select. Select on
        // Linux seems to reset this, so we need to refresh the value.
        timeoutInterval.tv_sec = 0; // seconds
        timeoutInterval.tv_usec = SELECT_TIMEOUT_IN_MICROSECS; // microseconds

        // Block until there is something to do.
        numActiveSockets = select(
                                m_ResultFdRange,
                                &m_ResultReadSocks,
                                &m_ResultWriteSocks,
                                &m_ResultExceptionSocks,
                                &timeoutInterval);
        // Get the last error immediately after the system call.
        // Anything code, even a DEBUG_LOG, may touch a file and
        // change the last error.
        lastErr = GET_LAST_ERROR();

        // DEBUG_LOG("Return from select. m_ResultFdRange = %d, numActiveSockets = %d",  m_ResultFdRange, numActiveSockets);

#if LINUX
      // This happens when a process signal interrupts the system call.
    	if (numActiveSockets == -1 && errno == EINTR) {
            continue;
    	}
#endif

        // After this point, if any other thread wants to change the
        // global state, then they must send a new wake-up message.
        // If a thread saw this was set between the time select() returned
        // and we clear it, this is NOT a race condition. We clear it before
        // we process any pending changes to the global state.
        if (m_pLock) {
            m_pLock->Lock();
        }

        m_fPendingWakeupMessage = false;

        // If we are quitting the entire network server, then exit
        // the main loop of the select thread as soon as we leave the lock.
        if (m_StopSelectThread) {
            fExitLoop = true;
        }

        if (m_pLock) {
            m_pLock->Unlock();
        }

        if (fExitLoop) {
            break;
        }

        // If there was an exception, then select failed. Often this
        // means that an invalid socket is in the fd sets. This can
        // happen if a socket is closed but not FD_CLR'ed. All we can
        // do is raise a warning and adjust the connections in the hope
        // that the offending connection has been cleared.
        //
        // This shouldn't happen and it usually means there is a bug.
        if (numActiveSockets < 0) {
            DEBUG_WARNING("Select returned an error. err = %d", lastErr);

            // Sleep for a while to let other threads run long enough
            // to close their sockets. Otherwise, this select becomes a
            // busy loop.
            OSIndependantLayer::SleepForMilliSecs(msSleepingForSelectError);

            // Adjust the connections, hopefully this will fix the problem.
            AdjustBlockIOs();

            continue;
        } // handling a select error event.

        // In Win32, if numActiveSockets is 0, then the FD_SET's may be invalid.
        // In particular, the exception sockets is what we passed in, not what really
        // has an exception.
        if (0 == numActiveSockets) {
            ASSERT_WIN32(0 == m_ResultReadSocks.fd_count);
            ASSERT_WIN32(0 == m_ResultWriteSocks.fd_count);
            ASSERT_WIN32(0 == m_ResultExceptionSocks.fd_count);

            // Check for timeouts and try again.
            AdjustBlockIOs();
#if WIN32
            continue;
#endif
        }

        // As a special case, check if this is one of the sockets
        // that is just designed to wake up the select thread.
        if (FD_ISSET(m_WakeUpThreadReceive, &m_ResultReadSocks)) {
            FD_CLR(m_WakeUpThreadReceive, &m_ResultReadSocks);
            (void) DrainNotificationData(m_WakeUpThreadReceive);
            numActiveSockets = numActiveSockets - 1;
        }

        if (numActiveSockets > 0) {
            (void) ProcessActiveSockets();
        } else {
            ASSERT_WIN32(0 == m_ResultReadSocks.fd_count);
            ASSERT_WIN32(0 == m_ResultWriteSocks.fd_count);
            ASSERT_WIN32(0 == m_ResultExceptionSocks.fd_count);
        }

        ASSERT_WIN32(m_ReadSocks.fd_count > 0);

        // Always call this, even when there are no active sockets,
        // since it implements timeouts.
        AdjustBlockIOs();
        ASSERT_WIN32(m_ReadSocks.fd_count > 0);
    } // the main server loop.

    if (NULL != m_pSelectThreadStopped) {
        m_pSelectThreadStopped->Signal();
    }
} // SelectThread.







#if WIN32
/////////////////////////////////////////////////////////////////////////////
//
// [ProcessActiveSockets]
//
// This is called in the select thread when we detect activity on a socket.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNetIOSystem::ProcessActiveSockets() {
    ErrVal err = ENoErr;
    OSSocket currentSocket;
    CNetBlockIO *pBlockIO;
    int32 socketNum;

    if (NULL == m_ActiveSocketTable) {
        gotoErr(EFail);
    }

    // Assume there are N connections and S active sockets.
    // We have a table, so finding the connection for a particular
    // socket should be fast: O(1) to O(logN). As a result, it is
    // faster to take all active sockets and find their connections
    // (which costs S * O(1) to S * O(logN)) than it is to take
    // all connections and determine if they are active (which
    // costs N*S).
    //////////////////////////////////////////////
    for (socketNum = 0; (uint32) socketNum < m_ResultReadSocks.fd_count; socketNum++) {
        currentSocket = (m_ResultReadSocks.fd_array)[ socketNum ];

        ////////////////////////////
        if (m_pLock) {
            m_pLock->Lock();
        }
        pBlockIO = (CNetBlockIO *) (m_ActiveSocketTable->GetValue(
                                            (const char *) &currentSocket,
                                            sizeof(OSSocket)));
        if (m_pLock) {
            m_pLock->Unlock();
        }
        ////////////////////////////

        if (pBlockIO) {
            ASSERT(pBlockIO->m_Socket == currentSocket);
            ProcessReadEvent(pBlockIO);
        } // processing one active pBlockIO
    } // looking at sockets ready to read.


    //////////////////////////////////////////////
    for (socketNum = 0; (uint32) socketNum < m_ResultWriteSocks.fd_count; socketNum++) {
        currentSocket = (m_ResultWriteSocks.fd_array)[ socketNum ];

        ////////////////////////////
        if (m_pLock) {
            m_pLock->Lock();
        }

        pBlockIO = (CNetBlockIO *) (m_ActiveSocketTable->GetValue(
                                            (const char *) &currentSocket,
                                            sizeof(OSSocket)));
        if (m_pLock) {
            m_pLock->Unlock();
        }
        ////////////////////////////

        if (pBlockIO) {
            ASSERT(pBlockIO->m_Socket == currentSocket);
            ProcessWriteEvent(pBlockIO);
        } // processing one active pBlockIO
    } // looking at sockets ready to write.


    //////////////////////////////////////////////
    for (socketNum = 0; (uint32) socketNum < m_ResultExceptionSocks.fd_count; socketNum++) {
        currentSocket = (m_ResultExceptionSocks.fd_array)[ socketNum ];

        ////////////////////////////
        if (m_pLock) {
            m_pLock->Lock();
        }
        pBlockIO = (CNetBlockIO *) (m_ActiveSocketTable->GetValue(
                                            (const char *) &currentSocket,
                                            sizeof(OSSocket)));
        if (m_pLock) {
            m_pLock->Unlock();
        }
        ////////////////////////////

        if (pBlockIO) {
            ASSERT(pBlockIO->m_Socket == currentSocket);
            ProcessExceptionEvent(pBlockIO);
        } // processing one active pBlockIO
    } // looking at sockets ready to read.

abort:
    returnErr(err);
} // ProcessActiveSockets.

#elif LINUX

/////////////////////////////////////////////////////////////////////////////
//
// [ProcessActiveSockets]
//
// This is called in the select thread when we detect activity on a socket.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNetIOSystem::ProcessActiveSockets() {
    ErrVal err = ENoErr;
    OSSocket currentSocket;
    CNetBlockIO *pBlockIO;
    CNetBlockIO *pNextBlockIO;

    pBlockIO = (CNetBlockIO *) m_ActiveBlockIOs.GetHead();
    while (NULL != pBlockIO) {
        pNextBlockIO = (CNetBlockIO *) (pBlockIO->m_ActiveBlockIOs.GetNextInQueue());
        currentSocket = pBlockIO->m_Socket;

        if (FD_ISSET(currentSocket, &m_ResultReadSocks)) {
            ProcessReadEvent(pBlockIO);
        }
        if (FD_ISSET(currentSocket, &m_ResultWriteSocks)) {
            ProcessWriteEvent(pBlockIO);
        }
        if (FD_ISSET(currentSocket, &m_ResultExceptionSocks)) {
           ProcessExceptionEvent(pBlockIO);
        }

        pBlockIO = pNextBlockIO;
    } // (NULL != pBlockIO)

    returnErr(err);
} // ProcessActiveSockets.
#endif // LINUX






/////////////////////////////////////////////////////////////////////////////
//
// [AdjustBlockIOs]
//
// This does several things:
//
//   1. Check for timeouts
//   2. Deletes doomed connections
/////////////////////////////////////////////////////////////////////////////
void
CNetIOSystem::AdjustBlockIOs() {
    uint64 now = 0;
    CNetBlockIO *pBlockIO;
    int socketNum;
    uint64 timeSinceLastCheck;
    CAsyncBlockIO * connectionList[MAX_TIMEOUTS_PER_CHECK];
    int32 timeOutList[MAX_TIMEOUTS_PER_CHECK];
    int numTimeouts = 0;
    int32 timeoutOp;

    // We always do this. This only checks the queue of pending-close
    // sockets, not all sockets, so it is not expensive.
    if (m_pLock) {
        while (true) {
            m_pLock->Lock();
            pBlockIO = m_PendingCloseList.RemoveHead();
            m_pLock->Unlock();

            if (NULL == pBlockIO) {
                break;
            }

            DEBUG_LOG("CNetIOSystem::AdjustBlockIOs. Call DisconnectSocket on pBlockIO %p", pBlockIO);

            DisconnectSocket(pBlockIO);
            // The blockIO was AddRefed when it was put on m_PendingCloseList.
            RELEASE_OBJECT(pBlockIO);
        }

        m_pLock->Lock();
        if (m_pEmptyPendingCloseBlockIOs) {
            m_pEmptyPendingCloseBlockIOs->Signal();
        }
        m_pLock->Unlock();
    } // if (m_pLock)


    // Select may be going off a lot when there is a lot of network activity.
    // Don't check for timeouts too often. It's not a cheap operation, and
    // it doesn't have to be done that frequently.
    now = GetTimeSinceBootInMs();
    timeSinceLastCheck = now - m_PrevCheckForTimeoutsTime;

    if (timeSinceLastCheck < SELECT_TIMEOUT_IN_MS) {
        return;
    }
    m_PrevCheckForTimeoutsTime = now;

    /////////////////////////////////////////////////
    // Make a private list of all blockIOs that should timeout.
    {
        AutoLock(m_pLock);

        pBlockIO = (CNetBlockIO *) m_ActiveBlockIOs.GetHead();
        numTimeouts = 0;
        while ((numTimeouts < MAX_TIMEOUTS_PER_CHECK) && (pBlockIO)) {
            if (pBlockIO->CheckTimeout(timeSinceLastCheck, &timeoutOp)) {
                ADDREF_OBJECT(pBlockIO);
                connectionList[numTimeouts] = pBlockIO;
                timeOutList[numTimeouts] = timeoutOp;
                numTimeouts += 1;
            }

            pBlockIO = (CNetBlockIO *) (pBlockIO->m_ActiveBlockIOs.GetNextInQueue());
        }
    }
    /////////////////////////////////////////////////


    for (socketNum = 0; socketNum < numTimeouts; socketNum++) {
        pBlockIO = (CNetBlockIO *) (connectionList[socketNum]);
        timeoutOp = timeOutList[socketNum];
        if (!pBlockIO) {
            continue;
        }

        // Fire a timeout.
        if (CIOBuffer::WRITE == timeoutOp) {
            // Remove the socket. Once we timeout, we cannot report
            // any additional activity.
            FD_CLR(pBlockIO->m_Socket, &m_WriteSocks);
            FD_CLR(pBlockIO->m_Socket, &m_ExceptionSocks);

            DEBUG_LOG("CNetIOSystem::AdjustBlockIOs. ENoResponse for a write on pBlockIO %p", pBlockIO);
            ReportSocketIsActive(pBlockIO, ENoResponse, CIOBuffer::WRITE);
        }
        if (CIOBuffer::READ == timeoutOp) {
            // Remove the socket. Once we timeout, we cannot report
            // any additional activity.
            FD_CLR(pBlockIO->m_Socket, &m_ReadSocks);
            FD_CLR(pBlockIO->m_Socket, &m_WriteSocks);
            FD_CLR(pBlockIO->m_Socket, &m_ExceptionSocks);

            DEBUG_LOG("CNetIOSystem::AdjustBlockIOs. ENoResponse for a read on pBlockIO %p", pBlockIO);
            ReportSocketIsActive(pBlockIO, ENoResponse, CIOBuffer::READ);
        }
        if (CIOBuffer::IO_CONNECT == timeoutOp) {
            // Remove the socket. Once we timeout, we cannot report
            // any additional activity.
            FD_CLR(pBlockIO->m_Socket, &m_ReadSocks);
            FD_CLR(pBlockIO->m_Socket, &m_WriteSocks);
            FD_CLR(pBlockIO->m_Socket, &m_ExceptionSocks);

            DEBUG_LOG("CNetIOSystem::AdjustBlockIOs. ENoResponse for a connect on pBlockIO %p", pBlockIO);
            ReportSocketIsActive(pBlockIO, ENoResponse, CIOBuffer::IO_CONNECT);
        }

        // BlockIO's were AddRef'ed by GetActiveBlockIOs.
        RELEASE_OBJECT(pBlockIO);
    } // finding sockets with activity.
} // AdjustBlockIOs.






/////////////////////////////////////////////////////////////////////////////
//
// [AcceptConnection]
//
/////////////////////////////////////////////////////////////////////////////
void
CNetIOSystem::AcceptConnection(
                    CNetBlockIO *serverConnection,
                    OSSocket socketID,
                    struct sockaddr_in *netAddr) {
    ErrVal err = ENoErr;
    CParsedUrl *pUrl = NULL;
    CNetBlockIO *pBlockIO = NULL;
    CAsyncBlockIOCallback *pCallback = NULL;
#if WIN32
    BOOL result = 0;
    int off = 0;
#endif // WIN32
    RunChecks();

    DEBUG_LOG("CNetIOSystem::AcceptConnection()");
    if ((NULL == serverConnection)
        || (NULL == netAddr)) {
        gotoErr(EFail);
    }

    // Create a URL that describes this accepted connection.
    pUrl = CParsedUrl::AllocateUrl(
                            g_IncomingIPAddressURL,
                            g_IncomingIPAddressURLLength,
                            NULL);
    if (NULL == pUrl) {
        gotoErr(EFail);
    }
    pUrl->m_pSockAddr = (struct sockaddr_in *) memAlloc(sizeof(struct sockaddr_in));
    if (NULL == pUrl->m_pSockAddr) {
        gotoErr(EFail);
    }
    *(pUrl->m_pSockAddr) = *netAddr;


    err = MakeSocketNonBlocking(socketID);
    if (err) {
        gotoErr(err);
    }

    // Set the read and write buffers.
    err = SetSocketBufSizeImpl(socketID, true, m_NumBytesPerSocketBuffer);
    if (err) {
        gotoErr(err);
    }
    err = SetSocketBufSizeImpl(socketID, false, m_NumBytesPerSocketBuffer);
    if (err) {
        gotoErr(err);
    }


    // Turn off Nagle's algorithm. This causes packets to be sent
    // immediately, rather than being buffered first on the local
    // host to group them into larger packets. We do the buffering,
    // so Nagle's algorithm would just get in the way. Normally,
    // this defaults to off, but the winsock 1.1 and 2.0 specs let
    // each winsock implementation decide what to default it to.
    // It seems that NODELAY is only in Linux versions Linux 2.5.71
    // and later, so it is not portable.
#if WIN32
    result = setsockopt(
                  socketID,
                  IPPROTO_TCP,
                  TCP_NODELAY,
                  (const char *) &off,
                  sizeof(off));
    if (result < 0) {
        gotoErr(EFail);
    }
#endif

    // Use the connection BlockIO callback to report the
    // new connection. This will probably be changed so each
    // blockIO has its own callback.
    pCallback = serverConnection->GetBlockIOCallback();

    pBlockIO = AllocNetBlockIO(pUrl, socketID, 0, pCallback);
    if (!pBlockIO) {
        DEBUG_LOG("CNetIOSystem::AcceptConnection. AllocNetBlockIO() failed");
        gotoErr(ETooManySockets);
    }

    // DO NOT START ACCEPTING IO ON THIS BLOCKIO. We must
    // wait until the correct Callback is set on the new blockIO.
    // Otherwise, if a block arrives before we initialize the blockIO,
    // then we will call the wrong callback.

    // Send the new blockIO as a job to pBlockIO->m_ServerJobQueue.
    DEBUG_LOG("CNetIOSystem::AcceptConnection. SendIOEvent IO_ACCEPT");
    err = pBlockIO->SendIOEvent(CIOBuffer::IO_ACCEPT, ENoErr);
    if (err) {
        DEBUG_LOG("CNetIOSystem::AcceptConnection. SendIOEvent() failed");
        gotoErr(err);
    }

    RELEASE_OBJECT(pCallback);
    RELEASE_OBJECT(pBlockIO);
    return;

abort:
    if (pBlockIO) {
        (void) pBlockIO->Close();
    } else if (NULL_SOCKET != socketID) {
        DEBUG_LOG("CNetIOSystem::AcceptConnection. Call SafeCloseSocket on socket %d", socketID);

        SafeCloseSocket(socketID, false);
        socketID = NULL_SOCKET;
    }

    RELEASE_OBJECT(pCallback);
    RELEASE_OBJECT(pBlockIO);
} // AcceptConnection.





/////////////////////////////////////////////////////////////////////////////
//
// [ProcessReadEvent]
//
/////////////////////////////////////////////////////////////////////////////
void
CNetIOSystem::ProcessReadEvent(CNetBlockIO *pBlockIO) {
    ErrVal err = ENoErr;
    CIOBuffer *pBuffer = NULL;
    bool fFinished = false;
    bool fReadFullBuffer = false;
    socklen_t fAddrLength;
    struct sockaddr_in newClientAddress;
    OSSocket newSocket;
    AutoLock(pBlockIO->m_pLock);


    if ((NULL_SOCKET == pBlockIO->m_Socket)
       || (pBlockIO->m_BlockIOFlags & CNetBlockIO::DISCONNECT_SOCKET)) {
        DEBUG_LOG("CNetIOSystem::ProcessReadEvent. Received IO on a disconnected socket %d",
            pBlockIO->m_Socket);
        return;
    }

    if (pBlockIO->m_BlockIOFlags & CNetBlockIO::ACCEPT_INCOMING_CONNECTIONS) {
        // Accept the message, creating a new socket and establishing
        // the pBlockIO with the remote client.
        fAddrLength = sizeof(struct sockaddr_in);
        newSocket = accept(
                       pBlockIO->m_Socket,
                       (struct sockaddr*) &newClientAddress,
                       &fAddrLength);

        DEBUG_LOG("CNetIOSystem::ProcessReadEvent. accepted a socket %d",
                    newSocket);

        // Once we have a new pBlockIO, initialize it.
        if (newSocket > 0) {
           AcceptConnection(pBlockIO, newSocket, &newClientAddress);
           return;
        }
     } // processing a pBlockIO on a listener socket.

    // If we just reported valid activity, then don't timeout.
    // IMPORTANT. Do this before we wake up the thread. Otherwise,
    // in a race codnition, we could clobber the state of the
    // next wait operation.
    pBlockIO->m_BlockIOFlags &= ~CNetBlockIO::WAITING_TO_READ;

    // We read immediately, so it is ok to leave this socket
    // on the select list.
    if (pBlockIO->m_BlockIOFlags & CNetBlockIO::DISCONNECT_SOCKET) {
        goto abort;
    }

    // This loop iterates as long as we are reading full buffers of data.
    while (1) {
        RELEASE_OBJECT(pBuffer);

        // See if the client already provided a buffer for this read.
        pBuffer = pBlockIO->m_PendingReads.RemoveHead();

        // If there is no pending read, then this is an unsolicited buffer.
        // That is ok, just allocate a buffer and do the read.
        if (NULL == pBuffer) {
            // Allocate a new buffer.
            pBuffer = AllocIOBuffer(-1, true);
            if (!pBuffer) {
                gotoErr(EFail);
            }

            pBuffer->m_BufferOp = CIOBuffer::READ;
            pBuffer->m_BufferFlags &= ~CIOBuffer::VALID_DATA;
            pBuffer->m_BufferFlags |= CIOBuffer::INPUT_BUFFER;
            pBuffer->m_Err = ENoErr;
            // buffer and bufferSize are initialized by AllocIOBuffer.
            pBuffer->m_NumValidBytes = 0;
            pBuffer->m_pBlockIO = pBlockIO;
            ADDREF_OBJECT(pBlockIO);

            pBlockIO->m_NumActiveReads += 1;
        }

        DEBUG_LOG("ProcessReadEvent calls DoPendingRead");
        err = pBlockIO->DoPendingRead(pBuffer, &fReadFullBuffer, &fFinished);
        if ((err) && (EEOF != err)) {
            gotoErr(err);
        }

        DEBUG_LOG("ProcessReadEvent. fFinished = %d. m_NumValidBytes = %d",
               fFinished, pBuffer->m_NumValidBytes);

        // If we did not get any data, then save the buffer for a future read.
        if (!fFinished) {
            // Put this buffer on the queue so the select thread can find
            // it later when we can resume the IO operation.
            pBlockIO->m_PendingReads.InsertTail(&(pBuffer->m_BlockIOBufferList));
            ADDREF_OBJECT(pBuffer);

            // Start waiting for the next read.
            pBlockIO->m_BlockIOFlags |= CNetBlockIO::WAITING_TO_READ;
            break;
        }

        if ((!fReadFullBuffer) || (err)) {
            break;
        }
    } // reading as long as there are full buffers of data.


    // Close the socket so it is removed from the read select list.
    // Otherwise, the socket will continually select that it is ready
    // to read.
    if (EEOF == err) {
        DEBUG_LOG("CNetIOSystem::ProcessReadEvent. Hit EOF. PrepareToDisconnect(%d)",
                    pBlockIO->m_Socket);
        (void) pBlockIO->PrepareToDisconnect();
        err = ENoErr;
    }

abort:
    RELEASE_OBJECT(pBuffer);
    if (err) {
        (void) pBlockIO->SendIOEvent(CIOBuffer::READ, err);
    }
} // ProcessReadEvent.







/////////////////////////////////////////////////////////////////////////////
//
// [ProcessWriteEvent]
//
/////////////////////////////////////////////////////////////////////////////
void
CNetIOSystem::ProcessWriteEvent(CNetBlockIO *pBlockIO) {
    ErrVal err = ENoErr;
    CIOBuffer *pBuffer = NULL;
    bool fFinished = false;
    AutoLock(pBlockIO->m_pLock);

    if ((NULL == pBlockIO)
       || (pBlockIO->m_BlockIOFlags & CNetBlockIO::DISCONNECT_SOCKET)
       || (NULL_SOCKET == pBlockIO->m_Socket)) {
        return;
    }

    // Only notify a connection once, otherwise select
    // will busy-loop on this socket until it is serviced
    // and the select busy loop prevents other threads from
    // running and servicing the socket.
    // WARNING. This assumes that the report succeeded.
    // WARNING. Do this BEFORE we report that the socket is active.
    // Otherwise, the socket's thread may block again (like blocking
    // on a read after blocking on a connect and do a second FD_SET
    // before we finish doing the first FD_CLR. Then, the first FD_CLR
    // clobbers the second FD_SET.
    if (m_pLock) {
        m_pLock->Lock();
    }
    FD_CLR(pBlockIO->m_Socket, &m_WriteSocks);
    FD_CLR(pBlockIO->m_Socket, &m_ExceptionSocks);

    if (m_pLock) {
        m_pLock->Unlock();
    }


    if (pBlockIO->m_BlockIOFlags & CNetBlockIO::WAITING_TO_CONNECT) {
        DEBUG_LOG("CNetIOSystem::ProcessWriteEvent. Finished connecting (%d)",
                    pBlockIO->m_Socket);

        pBlockIO->m_BlockIOFlags &= ~CNetBlockIO::WAITING_TO_CONNECT;
        ReportSocketIsActive(pBlockIO, ENoErr, CIOBuffer::IO_CONNECT);
        return;
    }

    // If the block IO is synchronously waiting for this event,
    // then signal it.
    pBuffer = pBlockIO->m_PendingWrites.RemoveHead();

    // If we just reported valid activity, then don't timeout.
    // IMPORTANT. Do this before we wake up the thread. Otherwise,
    // in a race codnition, we could clobber the state of the
    // next wait operation.
    pBlockIO->m_BlockIOFlags &= ~CNetBlockIO::WAITING_TO_WRITE;

    // This happens when we disconnect a socket that is waiting to connect or write.
    if (NULL == pBuffer) {
        gotoErr(ENoErr);
    }

    DEBUG_LOG("CNetBlockIO::ProcessWriteEvent calls DoPendingWrite");
    err = pBlockIO->DoPendingWrite(pBuffer, &fFinished);
    if (err) {
        fFinished = true;
    }

    if (fFinished) {
        if (m_pLock) {
            m_pLock->Lock();
        }

        // Don't select on this again until the pBlockIO is waiting for another
        // write, otherwise select will busy-loop on this socket until it is serviced
        // and the select busy loop prevents other threads from running and
        // servicing the socket.
        // WARNING. Do this BEFORE we report that the socket is active.
        // Otherwise, the socket's thread may block again (like blocking
        // on a read after blocking on a connect and do a second FD_SET
        // before we finish doing the first FD_CLR. Then, the first FD_CLR
        // clobbers the second FD_SET.
        FD_CLR(pBlockIO->m_Socket, &m_WriteSocks);

        pBlockIO->m_BlockIOFlags &= ~CNetBlockIO::WAITING_TO_WRITE;

        if (m_pLock) {
            m_pLock->Unlock();
        }

        // Tell the caller that the buffer I/O is complete.
        (void) pBlockIO->FinishIO(pBuffer, err, pBuffer->m_NumValidBytes);

        RELEASE_OBJECT(pBuffer);
    } else
    {
        pBlockIO->m_PendingWrites.InsertHead(&(pBuffer->m_BlockIOBufferList));
        pBlockIO->m_BlockIOFlags |= CNetBlockIO::WAITING_TO_WRITE;
    }

    pBuffer = NULL;

abort:
    RELEASE_OBJECT(pBuffer);
} // ProcessWriteEvent.








/////////////////////////////////////////////////////////////////////////////
//
// [ProcessExceptionEvent]
//
/////////////////////////////////////////////////////////////////////////////
void
CNetIOSystem::ProcessExceptionEvent(CNetBlockIO *pBlockIO) {
    int32 oldFlags;
    AutoLock(pBlockIO->m_pLock);

    if ((NULL == pBlockIO)
       || (NULL_SOCKET == pBlockIO->m_Socket)
       || (pBlockIO->m_BlockIOFlags & CNetBlockIO::DISCONNECT_SOCKET)) {
       return;
    }

    DEBUG_LOG("CNetIOSystem::ProcessExceptionEvent. socket = %d",
                pBlockIO->m_Socket);

    // Only notify a pBlockIO once, otherwise select
    // will busy-loop on this socket until it is serviced
    // and the select busy loop prevents other threads from
    // running and servicing the socket.
    // WARNING. This assumes that the report succeeded.
    // WARNING. Do this BEFORE we report that the socket is active.
    // Otherwise, the socket's thread may block again (like blocking
    // on a read after blocking on a connect and do a second FD_SET
    // before we finish doing the first FD_CLR. Then, the first FD_CLR
    // clobbers the second FD_SET.
    if (m_pLock) {
        m_pLock->Lock();
    }
    FD_CLR(pBlockIO->m_Socket, &m_WriteSocks);
    FD_CLR(pBlockIO->m_Socket, &m_ExceptionSocks);

    if (m_pLock) {
        m_pLock->Unlock();
    }


    oldFlags = pBlockIO->m_BlockIOFlags;
    pBlockIO->m_BlockIOFlags &= ~CNetBlockIO::WAITING_TO_CONNECT;
    pBlockIO->m_BlockIOFlags &= ~CNetBlockIO::WAITING_TO_WRITE;

    if (CNetBlockIO::WAITING_TO_WRITE & oldFlags) {
        ReportSocketIsActive(pBlockIO, ENoResponse, CIOBuffer::WRITE);
    } else { // if (CNetBlockIO::WAITING_TO_CONNECT & pBlockIO->m_BlockIOFlags) {
         ReportSocketIsActive(pBlockIO, ENoResponse, CIOBuffer::IO_CONNECT);
    }
} // ProcessExceptionEvent.







/////////////////////////////////////////////////////////////////////////////
//
// [ReportSocketIsActive]
//
/////////////////////////////////////////////////////////////////////////////
void
CNetIOSystem::ReportSocketIsActive(
                    CNetBlockIO *pBlockIO,
                    ErrVal wakeUpErr,
                    int32 op) {
    ErrVal err = ENoErr;

    if (!pBlockIO) {
        return;
    }
    if (pBlockIO->m_pLock) {
        pBlockIO->m_pLock->Lock();
    }


    if (pBlockIO->m_BlockIOFlags & CNetBlockIO::DISCONNECT_SOCKET) {
        goto abort;
    }


    if (CIOBuffer::READ == op) {
        pBlockIO->CancelTimeout(CIOBuffer::READ);
    } else if (CIOBuffer::WRITE == op) {
        pBlockIO->CancelTimeout(CIOBuffer::WRITE);
    } else if (CIOBuffer::IO_CONNECT == op) {
        pBlockIO->CancelTimeout(CIOBuffer::IO_CONNECT);
    }

    // If we just reported valid activity, then don't timeout.
    // IMPORTANT. Do this before we wake up the thread. Otherwise,
    // in a race codnition, we could clobber the state of the
    // next wait operation.
    pBlockIO->m_BlockIOFlags &= ~CNetBlockIO::WAITING_FOR_ANYTHING;

    err = pBlockIO->SendIOEvent(op, wakeUpErr);
    ASSERT(ENoErr == err);

abort:
    if (pBlockIO->m_pLock) {
        pBlockIO->m_pLock->Unlock();
    }
} // ReportSocketIsActive.






/////////////////////////////////////////////////////////////////////////////
//
// [DrainNotificationData]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNetIOSystem::DrainNotificationData(OSSocket sock) {
    ErrVal err = ENoErr;
    int32 bytesRead;
    char temp[256];
    int32 maxBytesToRead = sizeof(temp);
    int32 numRetries = 0;
    int32 lastErr = 0;

    if (NULL_SOCKET == sock) {
        gotoErr(EFail);
    }

    while (1) {
        // Read the message.
        bytesRead = recv(sock, temp, maxBytesToRead, 0);
        // Get the last error immediately after the system call.
        // Anything code, even a DEBUG_LOG, may touch a file and
        // change the last error.
        lastErr = GET_LAST_ERROR();

#if LINUX
        if ((bytesRead < 0)
            && (IGNORE_SYSTEMCALL_ERROR())
            && (numRetries < MAX_SYSTEM_CALL_INTERRUPTS)) {
            DEBUG_LOG("CNetBlockIO::DrainNotificationData: Ignoring EPIPE.");
            numRetries++;
            continue;
        }
#endif

        // If we failed, then abort. If we succeeded, then we are done.
        // If nothing happened, then wait until data is ready and try again.
        if (0 == bytesRead) {
            gotoErr(EEOF);
        } else if (bytesRead < 0) {
            if (IO_WOULD_BLOCK(lastErr)) {
                break;
            }

            gotoErr(EEOF);
        }

        // Try to avoid unnecessary work. If we think we have read all
        // there is to read, then stop. This may save us an additional
        // read() call that would return an EWOULDBLOCK error.
        // This could be one system call per iteration of the select
        // thread's main loop, so it adds up.
        //
        // If we somehow guess wrong, it's not a big deal. It just means
        // the control socket can still beread, so it will fire select()
        // again right away. The select thread still gets the same events.
        // So, if a signal spans 2 read() calls, we either do 1 select and
        // 3 reads (2 for data, 1 for EWOULDBLOCK) or 2 selects and 2 reads.
        // And, that's in the worst case. More often, we do 1 select and 1
        // read as opposed to 1 select and 2 reads.
        if (bytesRead < maxBytesToRead) {
            break;
        }
    } // trying to read data.

abort:
    returnErr(err);
} // DrainNotificationData.





/////////////////////////////////////////////////////////////////////////////
//
// [WakeSelectThread]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNetIOSystem::WakeSelectThread() {
    ErrVal err = ENoErr;
    int32 numBytesSent;
    char *dataPtr;
    int32 bufferSize;
    char temp[32];
    bool fPendingWakeupMessage;
    int32 lastErr = 0;
    RunChecks();

    // If somebody else is waking up the select thread, then don't
    // do it again. The select thread hasn't done any work yet, and
    // many wake-ups will only create unnecessary reads and writes
    // on the wake-up socket.
    if (m_pLock) {
        m_pLock->Lock();
    }

    fPendingWakeupMessage = m_fPendingWakeupMessage;
    m_fPendingWakeupMessage = true;

    if (m_pLock) {
        m_pLock->Unlock();
    }

    if (fPendingWakeupMessage) {
        returnErr(ENoErr);
    }


    if (NULL_SOCKET == m_WakeUpThreadSend) {
        returnErr(EFail);
    }

    temp[0] = 'D';
    temp[1] = 'D';
    temp[2] = '3';
    temp[3] = 0;
    dataPtr = temp;
    bufferSize = WAKEUP_SELECT_THREAD_MESSAGE_LENGTH;

    while (bufferSize > 0) {
        // Write all of the bytes that are currently in the buffer.
        numBytesSent = send(m_WakeUpThreadSend, dataPtr, (int) bufferSize, 0);
        // Get the last error immediately after the system call.
        // Anything code, even a DEBUG_LOG, may touch a file and
        // change the last error.
        lastErr = GET_LAST_ERROR();

        if ((SOCKET_ERROR == numBytesSent) && (IO_WOULD_BLOCK(lastErr))) {
            DEBUG_LOG("CNetBlockIO::WakeSelectThread: send returned %d, lastErr = %d",
                      numBytesSent, lastErr);

            OSIndependantLayer::SleepForMilliSecs(100);
            continue;
        }

        if (numBytesSent < 0) {
            DEBUG_LOG("CNetBlockIO::WakeSelectThread: send returned %d, lastErr = %d",
                      numBytesSent, lastErr);
#if WIN32
            DWORD dwErr = GetLastError();
            // This happens when we shutdown. Some apps, like IE, may turn off WSA.
            if (WSANOTINITIALISED == dwErr) {
               err = ENoErr;
               break;
            }
#endif
            err = EFail;
            break;
        } // if (numBytesSent < 0)

        // The data pointer is where we write from next. Update it
        // if we did not write the whole buffer this iteration.
        bufferSize = bufferSize - numBytesSent;
        dataPtr += numBytesSent;
    } // writing the message.

    returnErr(err);
} // WakeSelectThread.





/////////////////////////////////////////////////////////////////////////////
//
// [SafeCloseSocket]
//
// This safely closes a sockets. Some servers will not close
// a pBlockIO until all data has been acknowledged.
/////////////////////////////////////////////////////////////////////////////
void
CNetIOSystem::SafeCloseSocket(OSSocket sock, bool fUDP) {
    int result;
    static char discardBuffer[2000];
    struct linger lingerData;
    struct sockaddr_in fromAddress;
    socklen_t fromLength;
    int32 numRetries = 0;


    DEBUG_LOG("CNetIOSystem::SafeCloseSocket: Closing socket %d", sock);

    // An attacker may not send us a FIN-ACK in order to
    // create a denial-of-service attack. This tells us to close
    // the socket even though there are still unsent-data.
    lingerData.l_onoff = 0;
    lingerData.l_linger = 0;
    result = setsockopt(
                  sock,
                  SOL_SOCKET,
                  SO_LINGER,
                  (const char *) &lingerData,
                  sizeof(lingerData));

    // Half close the pBlockIO to prevent any more data
    // from being sent. 1 == SD_SEND, but that is in winsock2.h
    result = shutdown(sock, 1);

    // Read all remaining data; this just dumps it into a
    // bit bucket that is shared by all threads.
    while (1) {
        if (fUDP) {
           int recvFromFlags = 0;
#if WIN32
           // The recvfrom call will complete even if only part of a message has been received.
           recvFromFlags = MSG_PARTIAL;
#endif

            fromLength = sizeof(struct sockaddr_in);
            result = recvfrom(
                        sock,
                        discardBuffer,
                        sizeof(discardBuffer),
                        recvFromFlags,
                        (struct sockaddr *) &fromAddress,
                        &fromLength);
        } // if (fUDP)
        else {
            result = recv(sock, discardBuffer, sizeof(discardBuffer), 0);
        }

#if LINUX
        if ((result < 0)
            && (IGNORE_SYSTEMCALL_ERROR())
            && (numRetries < MAX_SYSTEM_CALL_INTERRUPTS)) {
            DEBUG_LOG("CNetBlockIO::DrainNotificationData: Ignoring EPIPE.");
            numRetries++;
            continue;
        }
#endif

        if (result <= 0) {
            break;
        }
    } // while (1)

#if LINUX
    ::close(sock);
#elif WIN32
    int sockErr = closesocket(sock);
    if (sockErr) {
        sockErr = WSAGetLastError();
    }
#endif

    DEBUG_LOG("CNetIOSystem::SafeCloseSocket: Finished closing socket %d", sock);
} // SafeCloseSocket.






/////////////////////////////////////////////////////////////////////////////
//
// [SetSocketBufSizeImpl]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNetIOSystem::SetSocketBufSizeImpl(OSSocket sock, bool writeBuf, int numBytes) {
    ErrVal err = ENoErr;
    int result;
    int cmd;
    int trySize;

    if (writeBuf) {
        cmd = SO_SNDBUF;
    } else
    {
        cmd = SO_RCVBUF;
    }

    // Each iteration tries a smaller size until we
    // succeed or get too small.
    for (trySize = numBytes; numBytes > CNetIOSystem::NETWORK_MTU; trySize = trySize >> 1) {
        result = setsockopt(
                    sock,
                    SOL_SOCKET,
                    cmd,
                    (char *) &trySize,
                    sizeof(int));
        if (0 == result) {
            break;
        } else {
#if WIN32
            if (WSAGetLastError() == WSAENOBUFS) {
                continue;
            }
#endif
            gotoErr(EFail);
        }
    } // retry-loop.

abort:
    returnErr(err);
} // SetSocketBufSizeImpl.






/////////////////////////////////////////////////////////////////////////////
//
// [MakeSocketNonBlocking]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNetIOSystem::MakeSocketNonBlocking(OSSocket socket) {
    ErrVal err = ENoErr;
    int result;

    // Make the sockets we use to wake up the select thread non-blocking.
#if WIN32
    int on = 1;

    result = ioctlsocket(socket, (int32) FIONBIO, (u_long *) &on);
    if (result) {
        gotoErr(EFail);
    }
#else
    int flags = fcntl(socket, F_GETFL);
    if (flags < 0) {
        gotoErr(EFail);
    }

    if (!(flags & O_NONBLOCK)) {
        result = fcntl((int) socket, F_SETFL, (long) O_NONBLOCK);
        if (result < 0) {
            gotoErr(EFail);
        }
    }
#endif

abort:
    returnErr(err);
} // MakeSocketNonBlocking.


