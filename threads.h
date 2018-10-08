/////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2005-2017 Dawson Dean
//
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

#ifndef _BUILDING_BLOCKS_THREADS_H_
#define _BUILDING_BLOCKS_THREADS_H_


#if LINUX
#include <pthread.h>
#endif // if LINUX

class CSimpleThread;


/////////////////////////////////////////////////////////////////////////////
// This is the type of the user procedure that is called as the
// main procedure in a thread.
typedef void (*ThreadProcType)(void *arg, CSimpleThread *pThread);



/////////////////////////////////////////////////////////////////////////////
// This is the state of a single thread.
class CSimpleThread : public CDebugObject {
public:
#if INCLUDE_REGRESSION_TESTS
    static void TestThreads();
#endif

    static ErrVal InitializeModule();
    static ErrVal CreateThread(
                     const char *pName,
                     ThreadProcType clientProc,
                     void *arg,
                     CSimpleThread **ppResultThread);

    CSimpleThread();
    virtual ~CSimpleThread();
    NEWEX_IMPL()

    bool IsRunning();
    void WaitForThreadToStop();

    // CDebugObject
    ErrVal CheckState();

    // This is public, but may only be called by the thread module.
    void OSCallback();

private:
    friend class CJobQueue;

    enum ThreadConstants {
        // These describe the state of one thread.
        ORIGINAL_PROCESS_THREAD  = 0x0001,
        THREAD_STARTED           = 0x0002,
        THREAD_STOPPED           = 0x0004,
    };

    ErrVal RunThread(
                const char *name,
                ThreadProcType clientProc,
                void *threadArgArg);


    int32           m_ThreadFlags;

    ThreadProcType  m_pThreadProc;
    void            *m_pThreadArg;

    CSimpleThread   *m_pNextRunningThread;

    // These are assigned by the operating system.
#if WIN32
    HANDLE          m_hThreadHandle;
    DWORD           m_OSThreadId;
#elif LINUX
    pthread_t       m_hThread;
#endif
}; // CSimpleThread





/////////////////////////////////////////////////////////////////////////////
class CRefLock : public CRefCountImpl,
                  public CRefCountInterface {
public:
    CRefLock();
    virtual ~CRefLock();
    NEWEX_IMPL()

    static CRefLock *Alloc();

    ErrVal Initialize();

    void Lock();
    void Unlock();
    bool IsLocked();

    // CRefCountInterface
    virtual void AddRefImpl(const char *pFileName, int32 lineNum) { DefaultAddRefImpl(pFileName, lineNum); }
    virtual void ReleaseImpl(const char *pFileName, int32 lineNum);

private:
    enum lockConstants {
        LOCK_INITIALIZED            = 0x01,
        LOCKED                      = 0x02,
        FINAL_RELEASE_ON_UNLOCK     = 0X04,
    };

    int16               m_RecursionDepth;
    uint16              m_LockFlags;

    OSIndependantLock   m_hLockMutex;
}; // CRefLock






/////////////////////////////////////////////////////////////////////////////
class CRefEvent : public CRefCountImpl,
                        public CRefCountInterface {
public:
    CRefEvent();
    virtual ~CRefEvent();
    NEWEX_IMPL()

    ErrVal Initialize();

    void Wait();
    void Signal();
    void Clear();

    // CRefCountInterface
    PASS_REFCOUNT_TO_REFCOUNTIMPL();

private:
    bool                m_fInitialized;

#if WIN32
    HANDLE              m_hEvent;
#elif LINUX
    pthread_cond_t      m_hEvent;
    pthread_mutex_t     m_EventLock;
    int32               m_NumWaiters;
    int32               m_NumSignals;
#endif
}; // CRefEvent





/////////////////////////////////////////////////////////////////////////////
//
//                            AUTO LOCK
//
// This will aquire and release a lock when a variable enters and leaves scope.
// It's like a monitor lock, except it applies to any C scope, including the
// body of a procedure, a loop body, an if statement, and more.
//
// Normally, we should AddRef the lock while it is held, but a CRefLock will not
// be released while the lock is held, so this is not required. It also makes
// a lot of unnecessary AddRef/Release calls, which is expensive. Each AddRef/Release
// is an atomic operation, which locks the memory bus.
/////////////////////////////////////////////////////////////////////////////
class CAutoLockImpl {
public:
    /////////////////////////////
    CAutoLockImpl(CRefLock *pLockPtr) {
        m_pLock = pLockPtr;
        if (NULL != m_pLock) {
            m_pLock->Lock();
        }
    }

    /////////////////////////////
    ~CAutoLockImpl() {
        if (NULL != m_pLock) {
            m_pLock->Unlock();
            m_pLock = NULL;
        }
    }

private:
    CRefLock        *m_pLock;
}; // CAutoLockImpl


// This is the line that is actually inserted in procedures.
#define AutoLock(pLockPtr) CAutoLockImpl _autoLockVar(pLockPtr)



#endif // _BUILDING_BLOCKS_THREADS_H_




