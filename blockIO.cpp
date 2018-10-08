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
// Block I/O Base Class Module
//
// This module implements the generic block-device interface; it
// encapsulates both OS-dependencies and the differences between files,
// networks, and memory storage. Different base classes implement
// the different particular I/O systems.
//
// The total block IO layer has several classes, and these will be
// implemented separately for each type of storage.
//
// 1. A block IO, which is a single source/sink stream of blocks.
//    Examples of a block IO include an open file, an active TCP
//    conection, or a stream to a region of memory.
//
// 2. An IO system. This manages block IO's. It's like a file system,
//    or a network interface.
//
// The block IO maintains no state; it reads and writes individual
// blocks. In particular, there is no cache or read-ahead, these
// are implemented by the higher level AsyncIOStream interface.
//
// All IO uses IOBuffers.
// These are also used for other things (like signalling events on a blockIO).
// They are allocated by the backing IOSystem, which can make buffers with
// special properties, like page alignment or locked into physical memory.
//
// All block I/O is asynchronous.
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

#include <math.h> // for ceil

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

extern CJobQueue *g_MainJobQueue;
int32 g_NumFileCallbacksActive = 0;

// The main implementations, one each for net, file and memory.
CIOSystem *g_pMemoryIOSystem = NULL;
CIOSystem *g_pFileIOSystem = NULL;
CIOSystem *g_pNetIOSystem = NULL;



/////////////////////////////////////////////////////////////////////////////
//
// [CIOBuffer]
//
/////////////////////////////////////////////////////////////////////////////
CIOBuffer::CIOBuffer() : m_BlockIOBufferList(this),
                         m_StreamBufferList(this) {
    m_BufferOp = NO_OP;
    m_BufferFlags = 0;

    m_Err = ENoErr;

    m_pPhysicalBuffer = NULL;
    m_pLogicalBuffer = NULL;

    m_BufferSize = 0;
    m_NumValidBytes = 0;
    m_StartWriteOffset = 0;

    m_PosInMedia = 0;

    m_pIOSystem = NULL;
    m_pBlockIO = NULL;

#if WIN32
    m_NTOverlappedIOInfo.Internal = 0;
    m_NTOverlappedIOInfo.InternalHigh = 0;
    m_NTOverlappedIOInfo.Offset = 0;
    m_NTOverlappedIOInfo.OffsetHigh = 0;
    m_NTOverlappedIOInfo.hEvent = (HANDLE) 0;

    m_dwNumBytesTransferred = 0;
#endif // WIN32
} // CIOBuffer




/////////////////////////////////////////////////////////////////////////////
//
// [~CIOBuffer]
//
/////////////////////////////////////////////////////////////////////////////
CIOBuffer::~CIOBuffer() {
    ASSERT(!(m_StreamBufferList.OnAnyQueue()));

    RELEASE_OBJECT(m_pBlockIO);

    if ((m_BufferFlags & CIOBuffer::ALLOCATED_BUFFER)
        && (NULL != m_pPhysicalBuffer)) {
        memFree(m_pPhysicalBuffer);
    }
} // ~CIOBuffer





/////////////////////////////////////////////////////////////////////////////
//
// [ProcessJob]
//
// This is called whenever the main job queue tries to perform some operation
// on an IO buffer.
/////////////////////////////////////////////////////////////////////////////
void
CIOBuffer::ProcessJob(CSimpleThread *pThreadState) {
    CAsyncBlockIO *pBlockIO = NULL;

    pThreadState = pThreadState;

    // Tell our blockIO that we are done.
    pBlockIO = m_pBlockIO;
    if (NULL != pBlockIO) {
        ADDREF_OBJECT(pBlockIO);
        pBlockIO->ProcessAllCompletedJobs();
        RELEASE_OBJECT(pBlockIO);
    }
} // ProcessJob.





/////////////////////////////////////////////////////////////////////////////
//
// [CheckState]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CIOBuffer::CheckState() {
    // Do not check the chain of asynch buffers, since this is
    // done without getting the lock.

    if (NULL == m_pLogicalBuffer) {
        returnErr(EFail);
    }

    if (m_pPhysicalBuffer > m_pLogicalBuffer) {
        returnErr(EFail);
    }

    // Do NOT check if the buffer memory is valid, since it may not be a
    // stand-alone buffer. Rather, it may be a range in a larger buffer.
    // As a result, it won't look like the product of a memAlloc.

    if (m_BufferSize <= 0) {
        returnErr(EFail);
    }

    if ((CIOBuffer::NO_OP != m_BufferOp)
            && (CIOBuffer::READ != m_BufferOp)
            && (CIOBuffer::WRITE != m_BufferOp)
            && (CIOBuffer::IO_CONNECT != m_BufferOp)
            && (CIOBuffer::IO_ACCEPT != m_BufferOp)) {
        returnErr(EFail);
    }

    if (NULL == m_pIOSystem) {
        returnErr(EFail);
    }


    // If the pBuffer is an active IO, then check this.
    if (CIOBuffer::NO_OP != m_BufferOp) {
        if (NULL == m_pBlockIO) {
            returnErr(EFail);
        }

        if (m_PosInMedia < 0L) {
            returnErr(EFail);
        }

        if (CIOBuffer::WRITE == m_BufferOp) {
            if (!(m_BufferFlags & CIOBuffer::VALID_DATA)) {
                returnErr(EFail);
            }
            if (m_NumValidBytes > m_BufferSize) {
                returnErr(EFail);
            }
        } // checking a write.


        if (CIOBuffer::READ == m_BufferOp) {
            if (m_NumValidBytes < 0) {
                returnErr(EFail);
            }
            if ((m_pPhysicalBuffer + m_BufferSize)
                < (m_pLogicalBuffer + m_NumValidBytes)) {
                returnErr(EFail);
            }
        } // checking a read.
    } // doing all checks on an active pBuffer.
    // If the pBuffer is a completed valid IO, then check this.
    else if ((CIOBuffer::NO_OP == m_BufferOp)
        && (m_BufferFlags & CIOBuffer::VALID_DATA)) {
        if (m_PosInMedia < 0L) {
            returnErr(EFail);
        }
        if (m_BufferSize < m_NumValidBytes) {
            returnErr(EFail);
        }
        if (m_NumValidBytes < 0) {
            returnErr(EFail);
        }
        if ((m_pLogicalBuffer < m_pPhysicalBuffer)
            || ((m_pLogicalBuffer + m_NumValidBytes)
                    > (m_pPhysicalBuffer + m_BufferSize))) {
            returnErr(EFail);
        }
    } // doing all checks on a complete inactive pBuffer.

    returnErr(ENoErr);
} // CheckState.







/////////////////////////////////////////////////////////////////////////////
//
// [CAsyncBlockIO]
//
/////////////////////////////////////////////////////////////////////////////
CAsyncBlockIO::CAsyncBlockIO() : m_ActiveBlockIOs(this) {
    m_MediaType = NO_MEDIA;
    m_fSeekable = true;

    m_pUrl = NULL;

    m_BlockIOFlags = 0;
    m_MediaSize = 0;
    m_fSynchronousDevice = false;

    m_pIOSystem = NULL;

    m_NumActiveReads = 0;
    m_NumActiveWrites = 0;

    m_pCallback = NULL;

    m_CompletedBuffers.ResetQueue();

    m_pLock = NULL;
} // CAsyncBlockIO.






/////////////////////////////////////////////////////////////////////////////
//
// [~CAsyncBlockIO]
//
/////////////////////////////////////////////////////////////////////////////
CAsyncBlockIO::~CAsyncBlockIO() {
    if (m_BlockIOFlags & CAsyncBlockIO::BLOCKIO_IS_OPEN) {
        DEBUG_WARNING("Deleting an open blockIO.");
    }
    if ((m_NumActiveReads > 0) || (m_NumActiveWrites > 0)) {
        DEBUG_WARNING("There are active reads or writes.");
    }

    RELEASE_OBJECT(m_pUrl);
    RELEASE_OBJECT(m_pCallback);
    RELEASE_OBJECT(m_pLock);
} // ~CAsyncBlockIO.





/////////////////////////////////////////////////////////////////////////////
//
// [ProcessAllCompletedJobs]
//
// This is called by ProcessJob whenever the main job queue tries to perform
// some operation on an IO buffer.
/////////////////////////////////////////////////////////////////////////////
void
CAsyncBlockIO::ProcessAllCompletedJobs() {
    CIOBuffer *pBuffer = NULL;
    CAsyncBlockIOCallback *pCallback = NULL;
    CAsyncBlockIOCallback *pTempCallback = NULL;
    bool fThisThreadsIsProcessingBuffers = false;

    // This loop iterates as long as there are completed buffers on this
    // blockIO. Normally, this will only iterate once, but if several
    // reads finish at near the same time, then it will iterate for each
    // of them. This ensures that the completed buffers are passed to the
    // callback in a strictly serial order.
    while (1) {
        // Completed buffers are stored on the completed queue.
        // Get the next completed buffer.

        //////////////////////////////////////////
        // ENTER CRITICAL SECTION
        if (m_pLock) {
            m_pLock->Lock();
        }

        // We want to serialize the events for a particular blockIO.
        // If some thread is already processing buffers for this blockIO,
        // and it isn't us, then that means another thread is processing
        // blocks. This happens with a network, for example, when a
        // CONNECT and READ event may happen near each other and two
        // different thread process them. The problem is, due to thread
        // scheduling, the events may then arrive to the callback out of order.
        if (!(m_BlockIOFlags & THREAD_PROCESSING_JOB)) {
            m_BlockIOFlags |= THREAD_PROCESSING_JOB;
            fThisThreadsIsProcessingBuffers = true;
        }

        if (fThisThreadsIsProcessingBuffers) {
            pBuffer = m_CompletedBuffers.RemoveHead();
        } else {
            ASSERT(NULL == pBuffer);
            pBuffer = NULL;
        }

        // Get the next buffer. If there are no more buffers,
        // then we may be done.
        if (NULL != pBuffer) {
            // We may have shutdown on a previous iteration, so
            // only do this if there are buffers to process.
            // Moreover, do this inside the lock. Otherwise, another
            // thread can close us.
            //RunChecksOnce();

            // Get the callback separately for each buffer we process.
            // When we call one method (like IO_CONNECT) it may
            // cause the callback to change.
            //
            // BE CAREFUL. If we are holding the only reference to the
            // callback, then releasing it will make it go away even
            // if we are about to get it and AddRef it again. So get
            // the new one first and release the old one second.
            // <> This doesn't make sense. We should never be holding the
            // last ref to the callback if the buffer is also holding
            // a ref to that callback.
            pTempCallback = GetBlockIOCallback();
            RELEASE_OBJECT(pCallback);
            pCallback = pTempCallback;
        } else { // if (NULL == pBuffer)
            // There are no more buffers left to process, so we are about
            // to break out of the loop as soon as we release the lock.
            if (fThisThreadsIsProcessingBuffers) {
                m_BlockIOFlags &= ~SENT_BLOCKIO_TO_JOBQUEUE;
                m_BlockIOFlags &= ~THREAD_PROCESSING_JOB;

                ASSERT(!(m_BlockIOFlags & SENT_BLOCKIO_TO_JOBQUEUE));
                ASSERT(!(m_BlockIOFlags & THREAD_PROCESSING_JOB));
            }
        } // (NULL == pBuffer)

        if (m_pLock) {
            m_pLock->Unlock();
        }

        // LEAVE CRITICAL SECTION
        //////////////////////////////////////////

        // If there is no more work to do, then we are done. Exit and wait to be
        // called again by ProcessJob where there are new jobs to execute.
        if (NULL == pBuffer) {
            break;
        }

        // Perform an action appropriate to the IO that just completed.
        if (NULL != pCallback) {
            switch (pBuffer->m_BufferOp) {
                case CIOBuffer::READ:
                    DEBUG_LOG("CAsyncBlockIO::ProcessAllCompletedJobs: Receive a completed read buffer.");
                    DEBUG_LOG("CAsyncBlockIO::ProcessAllCompletedJobs: pBuffer = %p, m_Err = %d, m_BufferFlags = %d.", pBuffer, pBuffer->m_Err, pBuffer->m_BufferFlags);
                    DEBUG_LOG("CAsyncBlockIO::ProcessAllCompletedJobs: m_pPhysicalBuffer = %p, m_NumValidBytes = %d, m_PosInMedia = %d.",
                        pBuffer->m_pPhysicalBuffer, pBuffer->m_NumValidBytes, pBuffer->m_PosInMedia);

                    pCallback->OnBlockIOEvent(pBuffer);
                    break;

                case CIOBuffer::WRITE:
                    DEBUG_LOG("CAsyncBlockIO::ProcessAllCompletedJobs: Receive a completed write buffer.");
                    DEBUG_LOG("CAsyncBlockIO::ProcessAllCompletedJobs: pBuffer = %p, m_Err = %d, m_BufferFlags = %d.", pBuffer, pBuffer->m_Err, pBuffer->m_BufferFlags);
                    DEBUG_LOG("CAsyncBlockIO::ProcessAllCompletedJobs: m_pPhysicalBuffer = %p, m_NumValidBytes = %d, m_PosInMedia = %d.",
                        pBuffer->m_pPhysicalBuffer, pBuffer->m_NumValidBytes, pBuffer->m_PosInMedia);

                    pCallback->OnBlockIOEvent(pBuffer);
                    break;

                case CIOBuffer::IO_CONNECT:
                    DEBUG_LOG("CAsyncBlockIO::ProcessAllCompletedJobs: Receive a connect event.");

                    pCallback->OnBlockIOOpen(pBuffer->m_Err, this);
                    break;

                case CIOBuffer::IO_ACCEPT:
                    DEBUG_LOG("CAsyncBlockIO::ProcessAllCompletedJobs: Receive an accept event.");

                    pCallback->OnBlockIOAccept(pBuffer->m_Err, this);
                    break;

                default:
                    DEBUG_WARNING("Unexpected state.");
                    break;
            } // switching on the buffer state.
        } // calling the callback.

        // Buffers are addref'ed when they are put on pBlockIO->m_CompletedBuffers.
        RELEASE_OBJECT(pBuffer);
    } // iterating on several serialized buffers for this blockIO.

    RELEASE_OBJECT(pCallback);
} // ProcessJob.







/////////////////////////////////////////////////////////////////////////////
//
// [WriteBlockAsync]
//
// This is the main function to write a block to a blockIO device.
// It does some standard work, and then passes control to WriteBlockAsyncImpl
// which does the real work. WriteBlockAsyncImpl is implemented differently
// by each subclass of CAsyncBlockIO.
/////////////////////////////////////////////////////////////////////////////
void
CAsyncBlockIO::WriteBlockAsync(CIOBuffer *pBuffer, int32 startOffsetInBuffer) {
    ErrVal err = ENoErr;
    bool fIncrementedIOCount = false;
    RunChecks();

    if (NULL == pBuffer) {
       return;
    }

    // Record the new activity.
    {
        AutoLock(m_pLock);
        m_NumActiveWrites++;
        fIncrementedIOCount = true;
    }

    // Verify the arguments. (NULL == pBuffer->m_pPhysicalBuffer)
    if ((NULL == m_pIOSystem)
        || (CIOBuffer::NO_OP != pBuffer->m_BufferOp)
        || !(pBuffer->m_BufferFlags & CIOBuffer::VALID_DATA)
        || (NULL == pBuffer->m_pLogicalBuffer)
        || (pBuffer->m_NumValidBytes < 0)
        || (pBuffer->m_BufferSize < 0)
        || (pBuffer->m_BufferSize < pBuffer->m_NumValidBytes)
        || (pBuffer->m_PosInMedia != m_pIOSystem->GetIOStartPosition(pBuffer->m_PosInMedia))) {
        gotoErr(EFail);
    }

    // A lot of pBuffer had to be initialized by the caller. Set any other fields
    // for this specific write operation.
    pBuffer->m_BufferOp = CIOBuffer::WRITE;
    pBuffer->m_StartWriteOffset = startOffsetInBuffer;
    pBuffer->m_BufferFlags &= ~CIOBuffer::UNSAVED_CHANGES;

    RELEASE_OBJECT(pBuffer->m_pBlockIO);
    pBuffer->m_pBlockIO = this;
    ADDREF_OBJECT(pBuffer->m_pBlockIO);

    DEBUG_LOG("CAsyncBlockIO::WriteBlockAsync: Start an async write.");
    DEBUG_LOG("CAsyncBlockIO::WriteBlockAsync: m_NumValidBytes = %d, m_PosInMedia = %d.",
          pBuffer->m_NumValidBytes, pBuffer->m_PosInMedia);
    DEBUG_LOG("CAsyncBlockIO::WriteBlockAsync: pBuffer = %p, m_pPhysicalBuffer = %p",
          pBuffer, pBuffer->m_pPhysicalBuffer);

    // Call the virtual function that is implemented differently for each
    // type of blockIO device.
    WriteBlockAsyncImpl(pBuffer);

abort:
    if (err) {
       if (fIncrementedIOCount) {
           AutoLock(m_pLock);
           m_NumActiveWrites--;
       }
       FinishIO(pBuffer, err, 0);
    }
} // WriteBlockAsync.







/////////////////////////////////////////////////////////////////////////////
//
// [ReadBlockAsync]
//
// This is the main function to read a block to a blockIO device.
// It does some standard work, and then passes control to ReadBlockAsyncImpl
// which does the real work. ReadBlockAsyncImpl is implemented differently
// by each subclass of CAsyncBlockIO.
/////////////////////////////////////////////////////////////////////////////
void
CAsyncBlockIO::ReadBlockAsync(CIOBuffer *pBuffer) {
    ErrVal err = ENoErr;
    bool fIncrementedIOCount = false;
    RunChecks();

    if (NULL == pBuffer) {
        return;
    }

    // Record the new activity.
    {
        AutoLock(m_pLock);

        if (0) { //<><><>(m_BlockIOFlags & CNetBlockIO::DISCONNECT_SOCKET)
            DEBUG_WARNING("Start read while disconnecting");
        }

        m_NumActiveReads += 1;
        fIncrementedIOCount = true;
    }

    if ((NULL == m_pIOSystem)
        || (CIOBuffer::NO_OP != pBuffer->m_BufferOp)
        || (NULL == pBuffer->m_pLogicalBuffer)
        || (pBuffer->m_BufferSize < 0)
        || (pBuffer->m_PosInMedia != m_pIOSystem->GetIOStartPosition(pBuffer->m_PosInMedia))) {
        DEBUG_WARNING("IO reading invalid block. IO Buffer %d. Requested Size %d. Buffer Size %d",
                    pBuffer->m_pLogicalBuffer,
                    pBuffer->m_NumValidBytes,
                    pBuffer->m_BufferSize);
        gotoErr(EFail);
    }

    // Call the virtual function that is implemented differently for each
    // type of blockIO device.
    pBuffer->m_NumValidBytes = 0;
    pBuffer->m_BufferOp = CIOBuffer::READ;

    RELEASE_OBJECT(pBuffer->m_pBlockIO);
    pBuffer->m_pBlockIO = this;
    ADDREF_THIS();

    DEBUG_LOG("CAsyncBlockIO::ReadBlockAsync: Start an async read.");
    DEBUG_LOG("CAsyncBlockIO::ReadBlockAsync: m_NumValidBytes = %d, m_PosInMedia = %d.",
          pBuffer->m_NumValidBytes, pBuffer->m_PosInMedia);
    DEBUG_LOG("CAsyncBlockIO::ReadBlockAsync: pBuffer = %p, m_pPhysicalBuffer = %p",
          pBuffer, pBuffer->m_pPhysicalBuffer);

    // Call the virtual function that is implemented differently for each
    // type of blockIO device.
    ReadBlockAsyncImpl(pBuffer);

abort:
    if (err) {
       if (fIncrementedIOCount) {
           AutoLock(m_pLock);
           m_NumActiveReads--;
       }
       FinishIO(pBuffer, err, 0);
    }
} // ReadBlockAsync.








/////////////////////////////////////////////////////////////////////////////
//
// [FinishIO]
//
// This is called by the specific blockIO classes each time they complete
// an asynchronous I/O. It reports completion to the original caller.
/////////////////////////////////////////////////////////////////////////////
void
CAsyncBlockIO::FinishIO(
                CIOBuffer *pBuffer,
                ErrVal resultErr,
                int32 bytesDone) {
    ErrVal err = ENoErr;
    bool fHoldingLock = false;
    RunChecks();

    DEBUG_LOG("CAsyncBlockIO::FinishIO. error %d, bytesDone %d", resultErr, bytesDone);
    if (NULL == pBuffer) {
        return;
    }

#if DD_DEBUG
    // A handy place to put a breakpoint before the thread-jump.
    if (resultErr) {
        DEBUG_LOG("CAsyncBlockIO::FinishIO received an error %d", resultErr);
        resultErr = resultErr;
    }
#endif // DD_DEBUG

    // Store the results of the IO.
    pBuffer->m_Err = resultErr;
    if (CIOBuffer::READ == pBuffer->m_BufferOp) {
        pBuffer->m_NumValidBytes = bytesDone;
        ASSERT(pBuffer->m_NumValidBytes >= 0);
    }

    if (m_pLock) {
        m_pLock->Lock();
        fHoldingLock = true;
    }

    // Update the statistics of the blockIO. This is done whether or
    // not there was an error. Also save the state while we
    // hold the lock. Release the lock BEFORE we do any notification.
    if (CIOBuffer::WRITE == pBuffer->m_BufferOp) {
        m_NumActiveWrites--;
    } else if (CIOBuffer::READ == pBuffer->m_BufferOp) {
        m_NumActiveReads--;
    }


    DEBUG_LOG("CAsyncBlockIO::FinishIO: Finish an IO.");
    DEBUG_LOG("CAsyncBlockIO::FinishIO: pBuffer = %p, m_Err = %d, m_BufferFlags = %d.", pBuffer, pBuffer->m_Err, pBuffer->m_BufferFlags);
    DEBUG_LOG("CAsyncBlockIO::FinishIO: m_pPhysicalBuffer = %p, m_NumValidBytes = %d, m_PosInMedia = %d.",
          pBuffer->m_pPhysicalBuffer, pBuffer->m_NumValidBytes, pBuffer->m_PosInMedia);


    // Immediate IO act synchronously. We don't do a thread jump before calling
    // the callback.
    if (m_fSynchronousDevice) {
        // Perform an action appropriate to the IO that just completed.
        //
        // WARNING!
        //
        // We are holding the lock while we do this. Normally, this would cause
        // a deadlock, because we are about to aquire the AsyncIOStream lock, but
        // we only do this for immediate IO so we are doing it while still on the
        // original caller thread. So, all locks are still being held by this
        // thread, and we can safely re-aquire them recursively.
        if (NULL != m_pCallback) {
            switch (pBuffer->m_BufferOp) {
                case CIOBuffer::READ:
                case CIOBuffer::WRITE:
                    m_pCallback->OnBlockIOEvent(pBuffer);
                    break;

                case CIOBuffer::NO_OP:
                case CIOBuffer::IO_CONNECT:
                case CIOBuffer::IO_ACCEPT:
                default:
                    DEBUG_WARNING("Unexpected state. ");
                    break;
            } // switching on the buffer state.
        } // calling the callback.
    // Otherwise, this is a normal asynch IO.
    } else {
        // Put it on the queue. This ensures that buffers are reported
        // in the same order they are read. This is important for a network.
        // Packets may be continually arriving, and we do not want a race
        // condition to allow packet N+1 to be processed before packet N.
        m_CompletedBuffers.InsertTail(&(pBuffer->m_BlockIOBufferList));
        ADDREF_OBJECT(pBuffer);

        // There is only one outstanding callback per blockIO at a time.
        // If another worker thread is active on this block IO, then it
        // will detect this new buffer on the queue. We do not need an extra
        // thread transition. Moreover, we do not want to risk buffers arriving
        // at worker threads out of order.
        if (!(m_BlockIOFlags & SENT_BLOCKIO_TO_JOBQUEUE)) {
           // Be careful. The job queue may hand this job off to a
           // thread before SubmitJob returns. To avoid a race condition,
           // set SENT_BLOCKIO_TO_JOBQUEUE before calling
           // SubmitJob.
           m_BlockIOFlags |= SENT_BLOCKIO_TO_JOBQUEUE;
           err = g_MainJobQueue->SubmitJob(pBuffer);
           if (err)
           {
               m_BlockIOFlags &= ~SENT_BLOCKIO_TO_JOBQUEUE;
           }
        }
    }

    if ((fHoldingLock) && (m_pLock)) {
        m_pLock->Unlock();
    }
} // FinishIO.






/////////////////////////////////////////////////////////////////////////////
//
// [SendIOEvent]
//
// This raises an event; it sends the blockIO as a job to the jobQueue.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncBlockIO::SendIOEvent(int32 IOEvent, ErrVal eventErr) {
    ErrVal err = ENoErr;
    CIOBuffer *pBuffer = NULL;
    bool fHoldingLock = false;
    RunChecks();

#if DD_DEBUG
    // A handy place to put a breakpoint before the thread-jump.
    if (eventErr) {
        eventErr = eventErr;
    }
#endif

    if (NULL == m_pIOSystem) {
        // If we closed the blockIO just as the peer also closes it,
        // then ignore the peer closing.
        if (EEOF == eventErr) {
            returnErr(ENoErr);
        } else {
            gotoErr(EFail);
        }
    }

    if (m_pLock) {
        m_pLock->Lock();
        fHoldingLock = true;
    }

    // Allocate a new buffer object that is just the object, not the
    // buffer memory. This should be pretty small.
    // There probably won't be enough events to warrant pool-allocating
    // blocks. This may change.
    pBuffer = m_pIOSystem->AllocIOBuffer(-1, false);
    if (NULL == pBuffer) {
        gotoErr(EFail);
    }

    pBuffer->m_BufferFlags = 0;
    pBuffer->m_BufferOp = IOEvent;
    pBuffer->m_Err = eventErr;

    pBuffer->m_pIOSystem = m_pIOSystem;
    pBuffer->m_pBlockIO = this;
    ADDREF_THIS();

    pBuffer->m_StreamBufferList.ResetQueue();

    ASSERT(CIOBuffer::NO_OP != pBuffer->m_BufferOp)

    m_CompletedBuffers.InsertTail(&(pBuffer->m_BlockIOBufferList));
    ADDREF_OBJECT(pBuffer);

    DEBUG_LOG("CAsyncBlockIO::SendIOEvent: Send an IO event.");
    DEBUG_LOG("CAsyncBlockIO::SendIOEvent: pBuffer = %p, state = %d, m_Err = %d, m_BufferFlags = %d.",
       pBuffer, pBuffer->m_BufferOp, pBuffer->m_Err, pBuffer->m_BufferFlags);

    // There is only one outstanding callback per blockIO at a time.
    // If another worker thread is active on this block IO, then it
    // will detect this new buffer on the queue. We do not need an extra
    // thread transition.
    if (!(m_BlockIOFlags & SENT_BLOCKIO_TO_JOBQUEUE)) {
        // Be careful. The job queue may hand this job off to a
        // thread before SubmitJob returns. To avoid a race condition,
        // set SENT_BLOCKIO_TO_JOBQUEUE before calling
        // SubmitJob.
        m_BlockIOFlags |= SENT_BLOCKIO_TO_JOBQUEUE;

        err = g_MainJobQueue->SubmitJob(pBuffer);
        if (err) {
            DEBUG_WARNING("SendIOEvent. SubmitJob failed.");
            m_BlockIOFlags &= ~SENT_BLOCKIO_TO_JOBQUEUE;
        }
    }

abort:
    if ((m_pLock) && (fHoldingLock)) {
        m_pLock->Unlock();
    }

    // We Addref'ed the buffer when we put it on the queue.
    RELEASE_OBJECT(pBuffer);

    returnErr(err);
} // SendIOEvent.






/////////////////////////////////////////////////////////////////////////////
//
// [Flush]
//
// This is the stub. Each subclass of CAsyncBlockIO may implement this differently.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncBlockIO::Flush() {
    returnErr(ENoErr);
} // Flush.





/////////////////////////////////////////////////////////////////////////////
//
// [Close]
//
/////////////////////////////////////////////////////////////////////////////
void
CAsyncBlockIO::Close() {
    CAsyncBlockIO *pThis = this;
    AutoLock(m_pLock);
    RunChecksOnce();

    DEBUG_LOG("CAsyncBlockIO::Close().");

    // It is possible for m_NumActiveReads to be > 0. If we
    // always have an open read, but timeout, then the read
    // will never complete.
    m_NumActiveReads = 0;
    m_NumActiveWrites = 0;

    if (NULL == m_pIOSystem) {
        // This is not an error. The AsyncIOStream may explicitly
        // close a blockIO, but to be safe it also always closes
        // a blockIO when it is released.
        return;
    }

    RELEASE_OBJECT(m_pCallback);

    if (NULL != m_pIOSystem) {
        m_ActiveBlockIOs.RemoveFromQueue();
        m_pIOSystem = NULL;

        // The blockIO was AddRef'ed when it was placed on the active queue.
        RELEASE_OBJECT(pThis);
    }

    m_BlockIOFlags &= ~CAsyncBlockIO::BLOCKIO_IS_OPEN;
} // Close.





/////////////////////////////////////////////////////////////////////////////
//
// [CancelTimeout]
//
// This is the stub. Each subclass of CAsyncBlockIO may implement this differently.
// Currently, this is only impleted in NetBlockIO, since that is the only
// blockIO that can timeout.
/////////////////////////////////////////////////////////////////////////////
void
CAsyncBlockIO::CancelTimeout(int32 opType) {
   opType = opType;
} // CancelTimeout





/////////////////////////////////////////////////////////////////////////////
//
// [StartTimeout]
//
// This is the stub. Each subclass of CAsyncBlockIO may implement this differently.
// Currently, this is only impleted in NetBlockIO, since that is the only
// blockIO that can timeout.
/////////////////////////////////////////////////////////////////////////////
void
CAsyncBlockIO::StartTimeout(int32 opType) {
   opType = opType;
} // StartTimeout.




/////////////////////////////////////////////////////////////////////////////
//
// [GetLock]
//
/////////////////////////////////////////////////////////////////////////////
CRefLock *
CAsyncBlockIO::GetLock() {
    ADDREF_OBJECT(m_pLock);
    return(m_pLock);
} // GetLock.




/////////////////////////////////////////////////////////////////////////////
//
// [CheckState]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncBlockIO::CheckState() {
    ErrVal err = ENoErr;
    AutoLock(m_pLock);

    if (!(m_BlockIOFlags & CAsyncBlockIO::BLOCKIO_IS_OPEN)) {
        returnErr(ENoErr);
    }

    if (NULL == m_pIOSystem) {
        returnErr(EFail);
    }
    if ((m_NumActiveReads < 0) || (m_NumActiveWrites < 0)) {
        returnErr(EFail);
    }
    if (m_MediaSize < 0) {
        returnErr(EFail);
    }
    if (m_NumActiveWrites > 100) {
        returnErr(EFail);
    }

    returnErr(err);
} // CheckState.











/////////////////////////////////////////////////////////////////////////////
//
// [ChangeBlockIOCallback]
//
/////////////////////////////////////////////////////////////////////////////
void
CAsyncBlockIO::ChangeBlockIOCallback(CAsyncBlockIOCallback *pCallback) {
    AutoLock(m_pLock);
    RunChecks();

    RELEASE_OBJECT(m_pCallback);
    m_pCallback = pCallback;
    ADDREF_OBJECT(m_pCallback);
} // ChangeBlockIOCallback.






/////////////////////////////////////////////////////////////////////////////
//
// [GetBlockIOCallback]
//
/////////////////////////////////////////////////////////////////////////////
CAsyncBlockIOCallback *
CAsyncBlockIO::GetBlockIOCallback() {
    AutoLock(m_pLock);
    RunChecks();

    ADDREF_OBJECT(m_pCallback);
    return(m_pCallback);
} // GetBlockIOCallback.





/////////////////////////////////////////////////////////////////////////////
//
// [RemoveNBytes]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CAsyncBlockIO::RemoveNBytes(int64 numBytes) {
    AutoLock(m_pLock);
    RunChecks();

    if ((numBytes >= 0)
        && (m_MediaSize >= numBytes)) {
        m_MediaSize = m_MediaSize - numBytes;
    }

    returnErr(ENoErr);
} // RemoveNBytes.





/////////////////////////////////////////////////////////////////////////////
//
// [CIOSystem]
//
/////////////////////////////////////////////////////////////////////////////
CIOSystem::CIOSystem() {
    m_fInitialized = false;
    m_pLock = NULL;
} // CIOSystem.





/////////////////////////////////////////////////////////////////////////////
//
// [~CIOSystem]
//
/////////////////////////////////////////////////////////////////////////////
CIOSystem::~CIOSystem() {
    // Leak the lock. The IO systems are global variables and so
    // they are released after the memory system is shut down.
    // RELEASE_OBJECT(m_pLock);
    m_pLock = NULL;
} // ~CIOSystem.






/////////////////////////////////////////////////////////////////////////////
//
// [InitIOSystem]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CIOSystem::InitIOSystem() {
    m_pLock = CRefLock::Alloc();
    if (NULL == m_pLock) {
        returnErr(EFail);
    }

    // Initially, there are no active blockIOs.
    m_ActiveBlockIOs.ResetQueue();

    m_fInitialized = true;

    returnErr(ENoErr);
} // InitIOSystem.






/////////////////////////////////////////////////////////////////////////////
//
// [Shutdown]
//
// This is done here, and not in the destructor, because we need
// the memory alloc module and the lock module running, and they
// may be destroyed before the destructor runs.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CIOSystem::Shutdown() {
    ErrVal err = ENoErr;
    returnErr(err);
} // Shutdown.





/////////////////////////////////////////////////////////////////////////////
//
// [AllocIOBuffer]
//
// This used to be really complex, and would keep a free queue of idle
// buffers. Instead, now it just relies on the main memory pool to recycle
// buffers efficiently.
//
// Besides simplicity (only one global allocator cache), and performance
// (all allocations are served by a unified cache), this also allows a
// client to dynamically change the size of buffers. That allows both
// AsyncIOStreams and record files to use the same blockIO and to allocate
// different sized buffers for the same type of blockIO. For example, one
// client reading a file may want to use 2K or 4K buffers, while another may
// want to use 64K buffers.
/////////////////////////////////////////////////////////////////////////////
CIOBuffer *
CIOSystem::AllocIOBuffer(int32 blockBufferSize, bool fAllocBuffer) {
    ErrVal err = ENoErr;
    CIOBuffer *pBuffer;

    // Do NOT take the lock. We don't need to, and that can cause a deadlock.
    // This method is called by blockIO objects.
    // A system may hold its lock and call a blockIO.
    // A blockIO may NOT hold its lock and call the system.
    //AutoLock(m_pLock);

    if (blockBufferSize <= 0) {
       blockBufferSize = GetDefaultBytesPerBlock();
    }

    pBuffer = newex CIOBuffer;
    if (NULL == pBuffer) {
        gotoErr(EFail);
    }

    pBuffer->m_BufferOp = CIOBuffer::NO_OP;
    pBuffer->m_BufferFlags = 0;
    pBuffer->m_Err = ENoErr;

    pBuffer->m_PosInMedia = 0L;
    pBuffer->m_StartWriteOffset = 0;

    pBuffer->m_pLogicalBuffer = NULL;
    pBuffer->m_NumValidBytes = 0;

    pBuffer->m_pIOSystem = this;
    pBuffer->m_pBlockIO = NULL;

    ASSERT(!(pBuffer->m_StreamBufferList.OnAnyQueue()));
    pBuffer->m_StreamBufferList.ResetQueue();

    // Allocate the physical buffer.
    if (fAllocBuffer) {
        // If the data must be aligned, then we allocate it on a
        // page boundary. Otherwise, allocate the data on any alignment.
        // File systems, for example, work faster when doing IO on page
        // boundaries, while network interfaces don't seem to care.
        //
        // This is a bit heavy-handed. If the alignment is anything != 0,
        // then we allocate whole pages. Doing better, however, will
        // require more flexibility from the memory allocator, like
        // malloc(size, arbitraryAlignment)
        if (GetBlockBufferAlignment() > 0) {
            // Make sure the buffer size is a multiple of pages.
            int32 pageSize = g_MainMem.GetBytesPerPage();
            int32 numPages = (int32) ceil((double)blockBufferSize / (double)pageSize);
            blockBufferSize = pageSize * numPages;

            ASSERT(blockBufferSize > 0);
            pBuffer->m_BufferSize = blockBufferSize;
            pBuffer->m_pPhysicalBuffer = (char *) memAllocPages(numPages);
        } else {
            ASSERT(blockBufferSize > 0);
            pBuffer->m_BufferSize = blockBufferSize;
            pBuffer->m_pPhysicalBuffer = (char *) memAlloc(pBuffer->m_BufferSize);
        }

        if (NULL == pBuffer->m_pPhysicalBuffer) {
            RELEASE_OBJECT(pBuffer);
            gotoErr(EFail);
        }
        pBuffer->m_pLogicalBuffer = pBuffer->m_pPhysicalBuffer;

        pBuffer->m_BufferFlags |= CIOBuffer::ALLOCATED_BUFFER;
    } // allocating the buffer.
    else {
        pBuffer->m_BufferSize = 0;
        pBuffer->m_pPhysicalBuffer = NULL;
    }

    return(pBuffer);

abort:
    return(NULL);
} // AllocIOBuffer.






/////////////////////////////////////////////////////////////////////////////
//
// [ReleaseBlockList]
//
// Return buffers to the idle list.
//
// NOTE: This routine takes a reference to the pointer, so
// it can be safely NULL'ed when the buffer is deallocated.
/////////////////////////////////////////////////////////////////////////////
void
CIOSystem::ReleaseBlockList(CIOBuffer *blockList) {
    CIOBuffer *pNextBlock;

    while (blockList) {
        pNextBlock = blockList->m_StreamBufferList.GetNextInQueue();
        RELEASE_OBJECT(blockList);
        blockList = pNextBlock;
    }
} // ReleaseBlockList.





/////////////////////////////////////////////////////////////////////////////
//
// [GetTotalActiveIOJobs]
//
/////////////////////////////////////////////////////////////////////////////
int32
CIOSystem::GetTotalActiveIOJobs() {
    return(g_MainJobQueue->GetNumJobs() + g_NumFileCallbacksActive);
}




/////////////////////////////////////////////////////////////////////////////
//
// [CSynCAsyncBlockIOCallback]
//
/////////////////////////////////////////////////////////////////////////////
CSynCAsyncBlockIOCallback::CSynCAsyncBlockIOCallback() {
    m_Semaphore = NULL;
    m_pLock = NULL;
    m_TestErr = ENoErr;
    m_pBlockIO = NULL;
}




/////////////////////////////////////////////////////////////////////////////
//
// [~CSynCAsyncBlockIOCallback]
//
/////////////////////////////////////////////////////////////////////////////
CSynCAsyncBlockIOCallback::~CSynCAsyncBlockIOCallback() {
    RELEASE_OBJECT(m_Semaphore);
    RELEASE_OBJECT(m_pBlockIO);
    RELEASE_OBJECT(m_pLock);
}




/////////////////////////////////////////////////////////////////////////////
//
// [Initialize]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSynCAsyncBlockIOCallback::Initialize() {
    ErrVal err = ENoErr;

    m_pLock = CRefLock::Alloc();
    if (NULL == m_pLock) {
        gotoErr(EFail);
    }

    m_Semaphore = newex CRefEvent;
    if (NULL == m_Semaphore) {
        gotoErr(EFail);
    }

    err = m_Semaphore->Initialize();
    if (err) {
        gotoErr(err);
    }

abort:
    returnErr(err);
} // Initialize





/////////////////////////////////////////////////////////////////////////////
//
// [OnBlockIOEvent]
//
/////////////////////////////////////////////////////////////////////////////
void
CSynCAsyncBlockIOCallback::OnBlockIOEvent(CIOBuffer *pBuffer) {
    AutoLock(m_pLock);

    if (NULL == pBuffer) {
        return;
    }

    if ((CIOBuffer::READ == pBuffer->m_BufferOp)
       || (CIOBuffer::WRITE == pBuffer->m_BufferOp)) {
        if (m_Semaphore) {
            m_Semaphore->Signal();
        }
    }
} // OnBlockIOEvent




/////////////////////////////////////////////////////////////////////////////
//
// [OnBlockIOOpen]
//
/////////////////////////////////////////////////////////////////////////////
void
CSynCAsyncBlockIOCallback::OnBlockIOOpen(ErrVal resultErr, CAsyncBlockIO *pBlockIO) {
    RELEASE_OBJECT(m_pBlockIO);
    m_pBlockIO = pBlockIO;
    ADDREF_OBJECT(m_pBlockIO);

    m_TestErr = resultErr;

    if (m_Semaphore) {
        m_Semaphore->Signal();
    }
} // OnBlockIOOpen




/////////////////////////////////////////////////////////////////////////////
//
// [Wait]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSynCAsyncBlockIOCallback::Wait() {
    if (m_Semaphore) {
        m_Semaphore->Wait();
    }

    return(m_TestErr);
} // Wait






/////////////////////////////////////////////////////////////////////////////
//
//                          TESTING PROCEDURES
//
/////////////////////////////////////////////////////////////////////////////
#if INCLUDE_REGRESSION_TESTS

static ErrVal TestOpenBlockIO(
                     int32 numBlockIOs,
                     int32 openOptions,
                     int32 storeSize,
                     bool memDevice);

static ErrVal TestWriteBlocks(
                    CAsyncBlockIO *pBlockIO,
                    int32 startByte,
                    int32 numBytes,
                    int16 baseVal);

static ErrVal TestReadBlocks(
                    CAsyncBlockIO *pBlockIO,
                    int32 startByte,
                    int32 numBytes,
                    int16 baseVal);

static void RunBlockIOTests(
                    int32 numBlockIOs,
                    int32 storeSize,
                    int32 growAmount,
                    bool memDevice);

static void TestNet();

static ErrVal TestReadPastEof(CAsyncBlockIO *pBlockIO, int32 startByte);

#define NUM_TEST_BLOCKIOS       1
#define BYTES_IN_STORE          10300
#define TEST_BLOCK_SIZE         100000

static char g_TestPathPrefix[512];
static CAsyncBlockIO *g_BlockIOList[NUM_TEST_BLOCKIOS];
static char g_TestMemoryDevice[TEST_BLOCK_SIZE];
static CSynCAsyncBlockIOCallback *g_TestCallback;

static char *g_TestServerName = (char *) "www.google.com";
static int g_TestServerPort = 80;



/////////////////////////////////////////////////////////////////////////////
//
// [TestBlockIO]
//
/////////////////////////////////////////////////////////////////////////////
void
CIOSystem::TestBlockIO() {
    ErrVal err = ENoErr;
    bool fFoundProxy;


    g_DebugManager.StartModuleTest("Block IO");

    fFoundProxy = NetIO_GetLocalProxySettings(&g_TestServerName, &g_TestServerPort);
    if (!fFoundProxy) {
        g_TestServerName = (char *) "www.google.com";
        g_TestServerPort = 80;
    }

    g_TestCallback = newex CSynCAsyncBlockIOCallback;
    if (NULL == g_TestCallback) {
        DEBUG_WARNING("Error");
        return;
    }
    err = g_TestCallback->Initialize();
    if (err) {
        DEBUG_WARNING("Error");
        return;
    }

    g_DebugManager.AddTestResultsDirectoryPath(
                        "",
                        g_TestPathPrefix,
                        sizeof g_TestPathPrefix);
    printf("\nDood: g_TestPathPrefix=%s\n", g_TestPathPrefix);


    g_pMemoryIOSystem->SetDebugFlags(CDebugObject::CHECK_STATE_ON_EVERY_OP);
    g_pFileIOSystem->SetDebugFlags(CDebugObject::CHECK_STATE_ON_EVERY_OP);
    g_pNetIOSystem->SetDebugFlags(CDebugObject::CHECK_STATE_ON_EVERY_OP);

    g_DebugManager.StartSubTest("Memory Block IO");
    RunBlockIOTests(1, BYTES_IN_STORE, BYTES_IN_STORE, true);
    g_DebugManager.EndSubTest();

    g_DebugManager.StartSubTest("File Block IO");
    RunBlockIOTests(NUM_TEST_BLOCKIOS, BYTES_IN_STORE, BYTES_IN_STORE, false);
    g_DebugManager.EndSubTest();

    g_DebugManager.StartSubTest("Network Block IO");
    TestNet();
    g_DebugManager.EndSubTest();
} // TestBlockIO.





/////////////////////////////////////////////////////////////////////////////
//
// [RunBlockIOTests]
//
/////////////////////////////////////////////////////////////////////////////
static void
RunBlockIOTests(
         int32 numBlockIOs,
         int32 storeSize,
         int32 growAmount,
         bool memDevice) {
    ErrVal err = ENoErr;
    int16 blockIONum;
    int32 grownStoreSize;
    int32 shrunkStoreSize;


    g_DebugManager.StartTest("Creating BlockIOs for new stores");
    err = TestOpenBlockIO(
                     numBlockIOs,
                     CAsyncBlockIO::CREATE_NEW_STORE | CAsyncBlockIO::WRITE_ACCESS | CAsyncBlockIO::READ_ACCESS,
                     storeSize,
                     memDevice);
    if (err) {
       DEBUG_WARNING("Cannot create an store.");
       gotoErr(err);
    }


    g_DebugManager.StartTest("Writing and Reading stores");

    for (blockIONum = 0; blockIONum < numBlockIOs; blockIONum++) {
        err = TestWriteBlocks(g_BlockIOList[blockIONum], 0, storeSize, 12);
        if (err) {
            gotoErr(err);
        }

        err = TestReadBlocks(g_BlockIOList[blockIONum], 0, storeSize, 12);
        if (err) {
            gotoErr(err);
        }
    }



    g_DebugManager.StartTest("Closing and re-opening pBlockIOs");

    for (blockIONum = 0; blockIONum < numBlockIOs; blockIONum++) {
        g_DebugManager.ShowProgress();

        (g_BlockIOList[blockIONum])->Close();
        RELEASE_OBJECT(g_BlockIOList[blockIONum]);
    }

    err = TestOpenBlockIO(
                     numBlockIOs,
                     CAsyncBlockIO::WRITE_ACCESS | CAsyncBlockIO::READ_ACCESS,
                     storeSize,
                     memDevice);
    if (err) {
       DEBUG_WARNING("Cannot create an store.");
       gotoErr(err);
    }

    // Memory devices do not keep their state.
    if (!memDevice) {
         // Read the contents.
         for (blockIONum = 0; blockIONum < numBlockIOs; blockIONum++)
         {
            err = TestReadBlocks(g_BlockIOList[blockIONum], 0, storeSize, 12);
            if (err) {
                  gotoErr(err);
            }
         }
    }



    // Now, read and write the entire stores after they have been
    // overwritten.
    for (blockIONum = 0; blockIONum < numBlockIOs; blockIONum++) {
        err = TestWriteBlocks(g_BlockIOList[blockIONum], 0, storeSize, 31);
        if (err) {
            gotoErr(err);
        }

        err = TestReadBlocks(g_BlockIOList[blockIONum], 0, storeSize, 31);
        if (err) {
            gotoErr(err);
        }

        err = TestReadBlocks(g_BlockIOList[blockIONum], 0, storeSize, 31);
        if (err) {
            gotoErr(err);
        }
    }



    g_DebugManager.StartTest("Reading and Writing Past the End Of A store");

    for (blockIONum = 0; blockIONum < numBlockIOs; blockIONum++) {
        g_DebugManager.ShowProgress();
        err = TestReadPastEof(g_BlockIOList[blockIONum], storeSize);
        if (err) {
            DEBUG_WARNING("Error while reading a store.");
            gotoErr(EFail);
        }
    }

    // Memory devices do not keep their state.
    if (memDevice) {
       gotoErr(ENoErr);
    }


    g_DebugManager.StartTest("Growing stores");
    grownStoreSize = storeSize + growAmount;

    for (blockIONum = 0; blockIONum < numBlockIOs; blockIONum++) {
        g_DebugManager.ShowProgress();

        err = (g_BlockIOList[blockIONum])->Resize(grownStoreSize);
        if (err) {
            DEBUG_WARNING("Cannot grow a store");
            gotoErr(err);
        }
        err = TestReadBlocks(g_BlockIOList[blockIONum], 0, storeSize, 31);
        if (err) {
            gotoErr(err);
        }
    }



    g_DebugManager.StartTest("Reading and Writing Grown Stores");

    for (blockIONum = 0; blockIONum < numBlockIOs; blockIONum++) {
        err = TestWriteBlocks(g_BlockIOList[blockIONum], 0, grownStoreSize, 37);
        if (err) {
            gotoErr(err);
        }

        err = TestReadBlocks(g_BlockIOList[blockIONum], 0, grownStoreSize, 37);
        if (err) {
            gotoErr(err);
        }
    }



    g_DebugManager.StartTest("Reading and Writing Past the End Of A store");

    for (blockIONum = 0; blockIONum < numBlockIOs; blockIONum++) {
        g_DebugManager.ShowProgress();
        err = TestReadPastEof(g_BlockIOList[blockIONum], grownStoreSize);
        if (err) {
            DEBUG_WARNING("Error while reading a store.");
            gotoErr(EFail);
        }
    }



    g_DebugManager.StartTest("Closing and re-opening grown pBlockIOs");

    for (blockIONum = 0; blockIONum < numBlockIOs; blockIONum++) {
        g_DebugManager.ShowProgress();

        (g_BlockIOList[blockIONum])->Close();
        RELEASE_OBJECT(g_BlockIOList[blockIONum]);
    }



    err = TestOpenBlockIO(
                     numBlockIOs,
                     CAsyncBlockIO::WRITE_ACCESS | CAsyncBlockIO::READ_ACCESS,
                     storeSize,
                     memDevice);
    if (err) {
       DEBUG_WARNING("Cannot create an store.");
       gotoErr(err);
    }

    // Read the contents.
    for (blockIONum = 0; blockIONum < numBlockIOs; blockIONum++) {
        err = TestReadBlocks(g_BlockIOList[blockIONum], 0, grownStoreSize, 37);
        if (err)
            gotoErr(err);
    }





    g_DebugManager.StartTest("Shrinking stores");
    shrunkStoreSize = storeSize - (growAmount >> 1);

    for (blockIONum = 0; blockIONum < numBlockIOs; blockIONum++) {
        g_DebugManager.ShowProgress();

        err = (g_BlockIOList[blockIONum])->Resize(shrunkStoreSize);
        if (err) {
            DEBUG_WARNING("Cannot grow a store");
            gotoErr(err);
        }
    }


    g_DebugManager.StartTest("Reading and Writing shrunk Stores");

    for (blockIONum = 0; blockIONum < numBlockIOs; blockIONum++) {
        err = TestWriteBlocks(g_BlockIOList[blockIONum], 0, shrunkStoreSize, 37);
        if (err) {
            gotoErr(err);
        }
    }

    for (blockIONum = 0; blockIONum < numBlockIOs; blockIONum++) {
        err = TestReadBlocks(g_BlockIOList[blockIONum], 0, shrunkStoreSize, 37);
        if (err) {
            gotoErr(err);
        }
    }





    g_DebugManager.StartTest("Reading and Writing Past the End Of A store");

    for (blockIONum = 0; blockIONum < numBlockIOs; blockIONum++) {
        g_DebugManager.ShowProgress();
        err = TestReadPastEof(g_BlockIOList[blockIONum], shrunkStoreSize);
        if (err) {
            DEBUG_WARNING("Error while reading a store.");
            gotoErr(EFail);
        }
    }


    g_DebugManager.StartTest("Closing and re-opening shrunk pBlockIOs");

    for (blockIONum = 0; blockIONum < numBlockIOs; blockIONum++) {
        g_DebugManager.ShowProgress();

        (g_BlockIOList[blockIONum])->Close();
        RELEASE_OBJECT(g_BlockIOList[blockIONum]);
    }


    err = TestOpenBlockIO(
                     numBlockIOs,
                     CAsyncBlockIO::WRITE_ACCESS | CAsyncBlockIO::READ_ACCESS,
                     storeSize,
                     memDevice);
    if (err) {
       DEBUG_WARNING("Cannot create an store.");
       gotoErr(err);
    }


    // Read the contents.
    for (blockIONum = 0; blockIONum < numBlockIOs; blockIONum++) {
        err = TestReadBlocks(g_BlockIOList[blockIONum], 0, shrunkStoreSize, 37);
        if (err) {
            gotoErr(err);
        }
    }



    g_DebugManager.StartTest("Closing stores");

    for (blockIONum = 0; blockIONum < numBlockIOs; blockIONum++) {
        g_DebugManager.ShowProgress();

        if (g_BlockIOList[blockIONum]) {
            g_BlockIOList[blockIONum]->Close();
            RELEASE_OBJECT(g_BlockIOList[blockIONum]);
        }
    }

abort:
    if (err) {
        ::printf("\n\n\nERROR\n\n\n");
    }
    return;
} // RunBlockIOTests.








/////////////////////////////////////////////////////////////////////////////
//
// [TestOpenBlockIO]
//
/////////////////////////////////////////////////////////////////////////////
static ErrVal
TestOpenBlockIO(
         int32 numBlockIOs,
         int32 openOptions,
         int32 storeSize,
         bool memDevice
        ) {
    ErrVal err = ENoErr;
    int16 blockIONum;
    CParsedUrl *pUrl = NULL;
    char buffer[CParsedUrl::MAX_URL_LENGTH];


    for (blockIONum = 0; blockIONum < numBlockIOs; blockIONum++) {
        RELEASE_OBJECT(pUrl);
        pUrl = NULL;
        g_DebugManager.ShowProgress();

        if (memDevice) {
            pUrl = CParsedUrl::AllocateMemoryUrl(0, storeSize, 0);
            if (NULL == pUrl) {
                gotoErr(EFail);
            }

            err = g_pMemoryIOSystem->OpenBlockIO(pUrl, openOptions, g_TestCallback);
        } else {
            snprintf(
                buffer,
                sizeof(buffer),
                "%stestFile%d",
                g_TestPathPrefix,
                blockIONum);
            pUrl = CParsedUrl::AllocateFileUrl(buffer);
            if (NULL == pUrl) {
                gotoErr(EFail);
            }

            err = g_pFileIOSystem->OpenBlockIO(pUrl, openOptions, g_TestCallback);
        }
        if (err) {
            DEBUG_WARNING("Cannot create an store.");
            gotoErr(err);
        }

        err = g_TestCallback->Wait();
        if (err) {
            DEBUG_WARNING("Cannot create an store.");
            gotoErr(err);
        }

        g_BlockIOList[blockIONum] = g_TestCallback->m_pBlockIO;
        g_TestCallback->m_pBlockIO = NULL;

        if (NULL == g_BlockIOList[blockIONum]) {
            DEBUG_WARNING("Cannot open an store.");
            gotoErr(err);
        }

        (g_BlockIOList[blockIONum])->SetDebugFlags(CDebugObject::CHECK_STATE_ON_EVERY_OP);
    }

abort:
    RELEASE_OBJECT(pUrl);
    returnErr(err);
} // TestOpenBlockIO








/////////////////////////////////////////////////////////////////////////////
//
// [TestWriteBlocks]
//
/////////////////////////////////////////////////////////////////////////////
static ErrVal
TestWriteBlocks(
         CAsyncBlockIO *pBlockIO,
         int32 startByte,
         int32 numBytes,
         int16 baseVal
        ) {
    ErrVal err = ENoErr;
    int32 byteNum;
    int32 bytesPerBlock;
    int32 bytesWritten;
    int32 bytesToWrite;
    CIOBuffer *pBuffer = NULL;
    CIOSystem *pIOSystem = NULL;

    if (!pBlockIO) {
        gotoErr(EFail);
    }

    pIOSystem = pBlockIO->GetIOSystem();
    bytesPerBlock = pIOSystem->GetDefaultBytesPerBlock();
    if (bytesPerBlock <= 0) {
        DEBUG_WARNING("Invalid bytes per block.");
        gotoErr(EFail);
    }

    if (bytesPerBlock >= TEST_BLOCK_SIZE) {
        DEBUG_WARNING("Block size too big");
        gotoErr(EFail);
    }

    pBuffer = pIOSystem->AllocIOBuffer(-1, false);
    if (!pBuffer) {
        DEBUG_WARNING("Cannot allocate an IO block");
        gotoErr(EFail);
    }


    bytesWritten = 0;
    while (bytesWritten < numBytes) {
        g_DebugManager.ShowProgress();

        bytesToWrite = bytesPerBlock;
        if ((bytesWritten + bytesToWrite) > numBytes) {
            bytesToWrite = numBytes - bytesWritten;
        }

        // First, initialize the blocks.
        for (byteNum = 0; byteNum < bytesToWrite; byteNum++) {
            g_TestMemoryDevice[byteNum] = (char) (byteNum + baseVal);
        }

        pBuffer->m_BufferOp = CIOBuffer::NO_OP;
        pBuffer->m_BufferFlags |= CIOBuffer::VALID_DATA;
        pBuffer->m_Err = ENoErr;
        pBuffer->m_pPhysicalBuffer = g_TestMemoryDevice;
        pBuffer->m_pLogicalBuffer = pBuffer->m_pPhysicalBuffer;
        pBuffer->m_BufferSize = bytesToWrite;
        ASSERT(bytesToWrite > 0);
        pBuffer->m_NumValidBytes = bytesToWrite;
        ASSERT(pBuffer->m_NumValidBytes >= 0);
        pBuffer->m_PosInMedia = startByte;
        pBuffer->m_StartWriteOffset = 0;

        pBlockIO->WriteBlockAsync(pBuffer, 0);
        g_TestCallback->Wait();
        if ((ENoErr != pBuffer->m_Err)
            || (pBuffer->m_NumValidBytes != bytesToWrite)) {
            DEBUG_WARNING("Error while writing a block.");
            gotoErr(EFail);
        }

        startByte += bytesToWrite;
        bytesWritten += bytesToWrite;
    }

abort:
    CIOSystem::ReleaseBlockList(pBuffer);
    returnErr(ENoErr);
} // TestWriteBlocks.






/////////////////////////////////////////////////////////////////////////////
//
// [TestReadBlocks]
//
/////////////////////////////////////////////////////////////////////////////
static ErrVal
TestReadBlocks(
         CAsyncBlockIO *pBlockIO,
         int32 startByte,
         int32 numBytes,
         int16 baseVal
        ) {
    ErrVal err = ENoErr;
    int32 byteNum;
    int32 bytesRead;
    int32 bytesPerBlock;
    int32 bytesToRead;
    CIOBuffer *pBuffer = NULL;
    char c;
    int16 same;
    CIOSystem *pIOSystem;

    if (!pBlockIO) {
        gotoErr(EFail);
    }

    pIOSystem = pBlockIO->GetIOSystem();
    bytesPerBlock = pIOSystem->GetDefaultBytesPerBlock();
    if (bytesPerBlock <= 0) {
        DEBUG_WARNING("Invalid bytes per block.");
        gotoErr(EFail);
    }

    if (bytesPerBlock >= TEST_BLOCK_SIZE) {
        DEBUG_WARNING("Block size too big");
        gotoErr(EFail);
    }


    pBuffer = pIOSystem->AllocIOBuffer(-1, false);
    if (!pBuffer) {
        DEBUG_WARNING("Cannot allocate an IO block");
        gotoErr(EFail);
    }

    bytesRead = 0;
    while (bytesRead < numBytes) {
        g_DebugManager.ShowProgress();

        bytesToRead = bytesPerBlock;
        if ((bytesRead + bytesToRead) > numBytes) {
            bytesToRead = numBytes - bytesRead;
        }

        // Zero out the memory.
        memset(g_TestMemoryDevice, 0, bytesRead);

        pBuffer->m_BufferOp = CIOBuffer::NO_OP;
        pBuffer->m_Err = ENoErr;
        pBuffer->m_pPhysicalBuffer = g_TestMemoryDevice;
        pBuffer->m_pLogicalBuffer = pBuffer->m_pPhysicalBuffer;
        pBuffer->m_BufferSize = bytesToRead;
        ASSERT(bytesToRead > 0);
        pBuffer->m_NumValidBytes = bytesToRead;
        ASSERT(pBuffer->m_NumValidBytes >= 0);
        pBuffer->m_PosInMedia = startByte;

        pBlockIO->ReadBlockAsync(pBuffer);

        // Be careful; once we grow a file we may get more data than
        // we originally wrote.
        g_TestCallback->Wait();
        if ((ENoErr != pBuffer->m_Err)
            || (pBuffer->m_NumValidBytes < bytesToRead)) {
            DEBUG_WARNING("Error while reading a block.");
            gotoErr(EFail);
        }

        same = true;
        for (byteNum = 0; byteNum < bytesToRead; byteNum++) {
            c = (char) (byteNum + baseVal);
            if (c != g_TestMemoryDevice[byteNum]) {
                same = false;
                break;
            }
        }

        if (!same) {
            DEBUG_WARNING("Data read does not match what was written. byteNum = %d\n",
                    byteNum);
            gotoErr(EFail);
        }

        startByte += bytesToRead;
        bytesRead += bytesToRead;
    }

abort:
    CIOSystem::ReleaseBlockList(pBuffer);
    returnErr(ENoErr);
} // TestReadBlocks.








/////////////////////////////////////////////////////////////////////////////
//
// [TestReadPastEof]
//
/////////////////////////////////////////////////////////////////////////////
static ErrVal
TestReadPastEof(CAsyncBlockIO *pBlockIO, int32 startByte) {
    ErrVal err = ENoErr;
    int32 bytesToRead;
    CIOBuffer *pBuffer = NULL;
    CIOSystem *pIOSystem;
    int64 pos;

    if (!pBlockIO) {
        gotoErr(EFail);
    }

    pIOSystem = pBlockIO->GetIOSystem();

    pBuffer = pIOSystem->AllocIOBuffer(-1, false);
    if (!pBuffer) {
        DEBUG_WARNING("Cannot allocate an IO block");
        gotoErr(EFail);
    }

    bytesToRead = 10;

    pos = pIOSystem->GetIOStartPosition(startByte)
                + pIOSystem->GetDefaultBytesPerBlock();

    pBuffer->m_BufferOp = CIOBuffer::NO_OP;
    pBuffer->m_Err = ENoErr;
    pBuffer->m_pPhysicalBuffer = g_TestMemoryDevice;
    pBuffer->m_pLogicalBuffer = pBuffer->m_pPhysicalBuffer;
    pBuffer->m_BufferSize = bytesToRead;
    ASSERT(bytesToRead > 0);
    pBuffer->m_NumValidBytes = bytesToRead;
    ASSERT(pBuffer->m_NumValidBytes >= 0);
    pBuffer->m_PosInMedia = pos;

    pBlockIO->ReadBlockAsync(pBuffer);
    g_TestCallback->Wait();
    if (EEOF == pBuffer->m_Err) {
        err = ENoErr;
    } else {
        err = EFail;
    }

    if (err) {
        DEBUG_WARNING("Error while reading a block.");
        gotoErr(EFail);
    }

    if (pBuffer->m_NumValidBytes != 0) {
        DEBUG_WARNING("Error. No error from reading past eof.");
        gotoErr(EFail);
    }

abort:
    RELEASE_OBJECT(pBuffer);
    returnErr(ENoErr);
} // TestReadPastEof.






/////////////////////////////////////////////////////////////////////////////
//
// [TestNet]
//
/////////////////////////////////////////////////////////////////////////////
static void
TestNet() {
    ErrVal err = ENoErr;
    CAsyncBlockIO * pBlockIO;
    int32 numBytes;
    CIOBuffer *pBuffer;
    CIOSystem *pIOSystem;
    CParsedUrl *pUrl = NULL;
    const char *ptr;
    char buffer[CParsedUrl::MAX_URL_LENGTH];

    g_DebugManager.StartTest("Connect and send HTTP GET");



    snprintf(
        buffer,
        sizeof(buffer),
        "http://%s:%d",
        g_TestServerName,
        g_TestServerPort);
    RELEASE_OBJECT(pUrl);
    pUrl = CParsedUrl::AllocateUrl(buffer);
    if (NULL == pUrl) {
        gotoErr(EFail);
    }

    err = g_pNetIOSystem->OpenBlockIO(pUrl, CAsyncBlockIO::READ_ACCESS | CAsyncBlockIO::WRITE_ACCESS, g_TestCallback);
    if (err) {
        DEBUG_WARNING("Cannot open a pBlockIO to a host");
        gotoErr(EFail);
    }

    err = g_TestCallback->Wait();
    if (err) {
        DEBUG_WARNING("Cannot resolve a host");
        gotoErr(EFail);
    }

    pBlockIO = g_TestCallback->m_pBlockIO;
    g_TestCallback->m_pBlockIO = NULL;


    pIOSystem = pBlockIO->GetIOSystem();
    if (!pIOSystem) {
        DEBUG_WARNING("Cannot get pIOSystem");
        gotoErr(EFail);
    }
    pBuffer = pIOSystem->AllocIOBuffer(-1, false);
    if (!pBuffer) {
        DEBUG_WARNING("Cannot allocate an IO block");
        gotoErr(EFail);
    }

    // char *ptr = "GET www.google.com/ HTTP/1.0\n\r"
    ptr = "GET / HTTP/1.1\r\n"
            "Accept: */*\r\n"
            "TestHeader: 1\r\n"
            "Accept-Language: en-us\r\n"
            //"Accept-Encoding: gzip, deflate\r\n"
            "User-Agent: dawsonsBrowser\r\n" // Mozilla/4.0 (compatible; MSIE 6.0; Windows NT 5.0; .NET CLR 1.1.4322)\r\n"
            "Host: www.google.com\r\n"
            //"Connection: Keep-Alive\r\n"
            "\r\n";

    numBytes = strlen(ptr);
    snprintf(g_TestMemoryDevice, TEST_BLOCK_SIZE, ptr);

    pBuffer->m_BufferOp = CIOBuffer::NO_OP;
    pBuffer->m_BufferFlags |= CIOBuffer::VALID_DATA;
    pBuffer->m_Err = ENoErr;
    pBuffer->m_pPhysicalBuffer = g_TestMemoryDevice;
    pBuffer->m_pLogicalBuffer = pBuffer->m_pPhysicalBuffer;
    pBuffer->m_BufferSize = numBytes;
    ASSERT(numBytes > 0);
    pBuffer->m_NumValidBytes = numBytes;
    ASSERT(pBuffer->m_NumValidBytes >= 0);
    pBuffer->m_PosInMedia = 0;
    pBuffer->m_StartWriteOffset= 0;

    pBlockIO->WriteBlockAsync(pBuffer, 0);
    g_TestCallback->Wait();
    if ((ENoErr != pBuffer->m_Err)
        || (pBuffer->m_NumValidBytes != numBytes)) {
        DEBUG_WARNING("Error while writing a block.");
        gotoErr(EFail);
    }

    // Buffers should just arrive.
    err = g_TestCallback->Wait();
    if (err) {
        DEBUG_WARNING("Error while writing a block.");
        gotoErr(EFail);
    }

    CIOSystem::ReleaseBlockList(pBuffer);

    pBlockIO->Close();

abort:
    RELEASE_OBJECT(pBlockIO);
    RELEASE_OBJECT(pUrl);
} // TestNet.



#endif // INCLUDE_REGRESSION_TESTS


