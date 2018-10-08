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
// Threads Module
//
// This wraps the basic mechanisms of a threading library: threads, locks,
// semaphores. They are all in one module because they are usually implemented
// as a single package (like pthreads) and they also have inter-dependencies
// (like sometimes a semaphore grabs the lock when it wakes a thread).
//
// The lock is a reference counted lock, which allows several different related
// data structures to share a single lock. This reduces the number of locks
// required by higher level modules, which saves resources (each lock can
// create a kernel object) and also simplifies lock dependencies, which reduces
// deadlock opportunities. This is a recursive lock, so a single thread may
// reacquire the same lock multiple times.
//
// This is built on top of the lower level basic lock in OSIndependentLayer.
// It adds a few additional features, such as it will not implement the final release
// and delete the lock until the lock has been completely unlocked.
//
// This also defines the AutoLock object, which will aquire and release a lock
// when a variable enters and leaves scope. It's like a monitor lock, except it
// applies to any C scope, including the body of a procedure, a loop body, an
// if statement, and more.
//
// This also implements CRefEvent, a cross-platform binary semaphore.
/////////////////////////////////////////////////////////////////////////////

#if LINUX
#include "time.h"
#include "signal.h"
#endif // LINUX

#include "osIndependantLayer.h"
#include "log.h"
#include "config.h"
#include "debugging.h"
#include "refCount.h"
#include "memAlloc.h"
#include "threads.h"

FILE_DEBUGGING_GLOBALS(LOG_LEVEL_DEFAULT, 0);

static CRefLock *g_ThreadStateLock = NULL;
static CSimpleThread *g_GlobalThreadList;

#if WIN32
static void Win32ThreadStart(void *arg);
#elif LINUX
static void *LinuxThreadStart(void *arg);
#endif

#if LINUX
extern void X86AtomicDecrement(int32 *p);
extern void X86AtomicIncrement(int32 *p);
//#define PTHREAD_COND_INITIALIZER 0
#endif // LINUX



/////////////////////////////////////////////////////////////////////////////
//
// [InitializeModule]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleThread::InitializeModule() {
    ErrVal err = ENoErr;
    CSimpleThread *pThread;

    g_ThreadStateLock = CRefLock::Alloc();
    if (NULL == g_ThreadStateLock) {
        gotoErr(EFail);
    }

    // Allocate the state for the main thread in the process.
    // This does NOT spin up a new thread. It simply makes a thread state
    // for the thread that is currently executing.
    pThread = newex CSimpleThread;
    if (NULL == pThread) {
        gotoErr(EFail);
    }
    pThread->m_ThreadFlags |= CSimpleThread::ORIGINAL_PROCESS_THREAD;
    err = pThread->RunThread("main", NULL, NULL);
    if (err) {
        gotoErr(err);
    }

#if LINUX
    // Ignore SIGPIPE signals.
    signal(SIGPIPE, SIG_IGN);
#endif // LINUX

abort:
    returnErr(err);
} // InitializeModule.






/////////////////////////////////////////////////////////////////////////////
//
// [CreateThread]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleThread::CreateThread(
                     const char *pThreadName,
                     ThreadProcType clientProc,
                     void *pThreadArg,
                     CSimpleThread **ppResultThread) {
    ErrVal err = ENoErr;
    CSimpleThread *pThread = NULL;

    if (NULL != ppResultThread) {
       *ppResultThread = NULL;
    }

    pThread = newex CSimpleThread;
    if (NULL == pThread) {
        gotoErr(EFail);
    }

    err = pThread->RunThread(pThreadName, clientProc, pThreadArg);
    if (err) {
       gotoErr(err);
    }

    if (NULL != ppResultThread) {
       *ppResultThread = pThread;
    }

abort:
    returnErr(err);
} // CreateThread.





/////////////////////////////////////////////////////////////////////////////
//
// [CSimpleThread]
//
/////////////////////////////////////////////////////////////////////////////
CSimpleThread::CSimpleThread() {
    m_ThreadFlags = 0;
    m_pThreadProc = NULL;
    m_pThreadArg = NULL;

#if WIN32
    m_hThreadHandle = 0;
    m_OSThreadId = 0;
#elif LINUX
    m_hThread = 0;
#endif

    m_pNextRunningThread = NULL;
} // CSimpleThread.






/////////////////////////////////////////////////////////////////////////////
//
// [~CSimpleThread]
//
/////////////////////////////////////////////////////////////////////////////
CSimpleThread::~CSimpleThread() {
    m_pThreadProc = NULL;
    m_pThreadArg = NULL;
} // CSimpleThread.






/////////////////////////////////////////////////////////////////////////////
//
// [RunThread]
//
// This initializes a thread: it assigns a thread ID, adds
// the thread to the global state of all threads, and then
// runs the thread.
//
// NOTE: The thread may have been allocated either in this
// module, or some higher level module that subclasses thread.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleThread::RunThread(
                  const char *pName,
                  ThreadProcType clientProc,
                  void *threadArgArg) {
    ErrVal err = ENoErr;

    RunChecks();

    pName = pName;

    // Add thread to active list.
    g_ThreadStateLock->Lock();
    m_pNextRunningThread = g_GlobalThreadList;
    g_GlobalThreadList = this;
    g_ThreadStateLock->Unlock();

    // Initialize the state of this thread.
    m_pThreadProc = clientProc;
    m_pThreadArg = threadArgArg;

    // If this is the main thread for the process, then it is
    // already running. Otherwise, start the thread.
    if (m_ThreadFlags & ORIGINAL_PROCESS_THREAD) {
#if WIN32
        m_hThreadHandle = GetCurrentThread();
        m_OSThreadId = 0;
#endif
    } else
    {
#if WIN32
       m_hThreadHandle = ::CreateThread(
                              NULL, // pointer to thread security attributes
                              0, // initial thread stack size, in bytes. 0 Uses default size
                              (LPTHREAD_START_ROUTINE) Win32ThreadStart, // pointer to thread function
                              (LPVOID) this, // argument for new thread
                              0, // creation flags, CREATE_SUSPENDED
                              &m_OSThreadId); // pointer to returned thread identifier
        if (NULL == m_hThreadHandle) {
            DWORD dwErr = GetLastError();
            DEBUG_LOG("CSimpleThread::RunThread. CreateThread failed (step 2). GetLastError = %d", dwErr);
            gotoErr(TranslateWin32ErrorIntoErrVal(dwErr, true));
        }
#elif LINUX
       int result;
       pthread_attr_t attr;

       pthread_attr_init(&attr);

       result = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
       if (0 != result) {
            gotoErr(EFail);
       }

       result = pthread_create(&m_hThread, &attr, LinuxThreadStart, this);
       if (0 != result) {
            gotoErr(EFail);
       }
#endif

        m_ThreadFlags |= THREAD_STARTED;
    }

abort:
    returnErr(err);
} // RunThread.







/////////////////////////////////////////////////////////////////////////////
//
// [CheckState]
//
// This checks the state of the object itself; it is part of
// the debugObject class.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleThread::CheckState() {
    ErrVal err = ENoErr;
    CSimpleThread *currentThread;

    // Get exclusive access to the global thread state.
    g_ThreadStateLock->Lock();

    currentThread = g_GlobalThreadList;
    while (currentThread) {
        if ((NULL == currentThread->m_pThreadProc)
            && !(currentThread->m_ThreadFlags & ORIGINAL_PROCESS_THREAD)) {
            gotoErr(EFail);
        }

        currentThread = currentThread->m_pNextRunningThread;
    }

abort:
    g_ThreadStateLock->Unlock();

    returnErr(err);
} // CheckState.






/////////////////////////////////////////////////////////////////////////////
//
// These are the OS-specific thread stubs.
/////////////////////////////////////////////////////////////////////////////
#if WIN32
static void
Win32ThreadStart(void *arg) {
    CSimpleThread *pThreadState = (CSimpleThread *) arg;

    DEBUG_LOG("CSimpleThread: Starting thread");

    if (NULL != pThreadState) {
       // Run the thread.
       // This call won't return until the thread has exited.
       __try {
           pThreadState->OSCallback();
       } __except (EXCEPTION_EXECUTE_HANDLER) {
           DEBUG_WARNING("Caught thread exception.");
       }
    }

    DEBUG_LOG("CSimpleThread: Thread exits");

    // Don't delete the actual thread descriptor. We keep that around
    // to do things like examine the exit code or check if the thread is still
    // running.

    // Kill the thread.
    ExitThread(0);
    // We never reach here.
} // Win32ThreadStart.
#endif






/////////////////////////////////////////////////////////////////////////////
//
// These are the OS-specific thread stubs.
/////////////////////////////////////////////////////////////////////////////
#if LINUX
static void *
LinuxThreadStart(void *arg) {
    CSimpleThread *pThreadState;

    // Ignore SIGPIPE signals.
    signal(SIGPIPE, SIG_IGN);

    DEBUG_LOG("CSimpleThread: Starting thread");

    pThreadState = (CSimpleThread *) arg;
    if (NULL != pThreadState) {
       // Run the thread.
       // This call won't return until the thread has exited.
       try {
           pThreadState->OSCallback();
       } catch (void *p) {
           DEBUG_WARNING("Caught thread exception.");
       }
    }

    DEBUG_LOG("CSimpleThread: Thread exits");

    // Don't delete the actual thread descriptor. We keep that around
    // to do things like examine the exit code or check if the thread is still
    // running.

    // Kill the thread.
    pthread_exit(pThreadState);
    // We never reach here.

   return(NULL);
} // LinuxThreadStart.
#endif






/////////////////////////////////////////////////////////////////////////////
//
// [OSCallback]
//
// This runs as part of the new thread.
/////////////////////////////////////////////////////////////////////////////
void
CSimpleThread::OSCallback() {
    CSimpleThread *prevThread;
    CSimpleThread *currentThread;

    // This is the thread body.
    if (m_pThreadProc) {
        (*m_pThreadProc)(m_pThreadArg, this);
    }

    // When this routine returns, the thread is done.
    m_ThreadFlags |= THREAD_STOPPED;

    // Get exclusive access to the global thread state.
    g_ThreadStateLock->Lock();

    // Remove the thread from the active list.
    prevThread = NULL;
    currentThread = g_GlobalThreadList;
    while (currentThread) {
        if (currentThread == this) {
           if (g_GlobalThreadList == currentThread)
           {
              g_GlobalThreadList = m_pNextRunningThread;
           } else
           {
              prevThread->m_pNextRunningThread = m_pNextRunningThread;
           }
           break;
        }

        prevThread = currentThread;
        currentThread = currentThread->m_pNextRunningThread;
    }

    g_ThreadStateLock->Unlock();

    // Close up the thread kernel object. This tells ExitThread that
    // there are no more references to the thread object so it can be deleted.
#if WIN32
    if (NULL != m_hThreadHandle) {
        (void) CloseHandle(m_hThreadHandle);
        m_hThreadHandle = NULL;
    }
#endif
} // OSCallback.







/////////////////////////////////////////////////////////////////////////////
//
// [IsRunning]
//
/////////////////////////////////////////////////////////////////////////////
bool
CSimpleThread::IsRunning() {
    if (!(m_ThreadFlags & THREAD_STARTED)
         || (m_ThreadFlags & THREAD_STOPPED)) {
        return(false);
    }

#if WIN32
    {
        BOOL fSuccess;
        DWORD dwExitCode;

        fSuccess = GetExitCodeThread(m_hThreadHandle, &dwExitCode);
        if (!fSuccess) {
            return(false);
        }

        if (STILL_ACTIVE == dwExitCode) {
            return(true);
        }
    }
#endif

   return(true);
} // IsRunning.





/////////////////////////////////////////////////////////////////////////////
//
// [WaitForThreadToStop]
//
/////////////////////////////////////////////////////////////////////////////
void
CSimpleThread::WaitForThreadToStop() {
    if (!(m_ThreadFlags & THREAD_STARTED)
         || (m_ThreadFlags & THREAD_STOPPED)) {
        return;
    }

    DEBUG_LOG("CSimpleThread::WaitForThreadToStop: Starting to wait");

#if WIN32
    WaitForSingleObject(m_hThreadHandle, INFINITE);
#elif LINUX
    pthread_join(m_hThread, NULL);
#endif

    DEBUG_LOG("CSimpleThread::WaitForThreadToStop: Finished waiting");
} // WaitForThreadToStop.






/////////////////////////////////////////////////////////////////////////////
//
// [Alloc]
//
/////////////////////////////////////////////////////////////////////////////
CRefLock *
CRefLock::Alloc() {
    ErrVal err = ENoErr;
    CRefLock *pLock = NULL;

    pLock = newex CRefLock;
    if (NULL == pLock) {
        gotoErr(EFail);
    }

    err = pLock->Initialize();
    if (err) {
        RELEASE_OBJECT(pLock);
        gotoErr(err);
    }

abort:
    return(pLock);
} // Alloc






/////////////////////////////////////////////////////////////////////////////
//
// [CRefLock]
//
/////////////////////////////////////////////////////////////////////////////
CRefLock::CRefLock() {
    m_LockFlags = 0;
    m_RecursionDepth = 0;
} // CRefLock.






/////////////////////////////////////////////////////////////////////////////
//
// [~CRefLock]
//
/////////////////////////////////////////////////////////////////////////////
CRefLock::~CRefLock() {
    if (m_LockFlags & LOCK_INITIALIZED) {
        m_hLockMutex.Shutdown();

        m_LockFlags &= ~LOCK_INITIALIZED;
        m_RecursionDepth = 0;
    }
} // ~CRefLock.






/////////////////////////////////////////////////////////////////////////////
//
// [Initialize]
//
// This initializes a lock. In some OS'es, this can return an error.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CRefLock::Initialize() {
    ErrVal err = ENoErr;

    if (m_LockFlags & LOCK_INITIALIZED) {
        DEBUG_WARNING("CRefLock::Initialize. Multiple initializations of a single lock.");
    } else { // if (!(m_LockFlags & LOCK_INITIALIZED))
        err = m_hLockMutex.Initialize();
        if (err) {
            DEBUG_WARNING("Cannot initialize an os lock.");
            gotoErr(err);
        }

        m_LockFlags = LOCK_INITIALIZED;
        m_RecursionDepth = 0;
    }

abort:
    returnErr(err);
} // Initialize.







/////////////////////////////////////////////////////////////////////////////
//
// [Lock]
//
// This acquires a lock, it blocks until it has the lock.
/////////////////////////////////////////////////////////////////////////////
void
CRefLock::Lock() {
    if (!(m_LockFlags & LOCK_INITIALIZED)) {
        DEBUG_WARNING(" Lock is not initialized");
        return;
    }

    m_hLockMutex.BasicLock();

    // Once we hold the lock, update the lock state.
    m_LockFlags |= LOCKED;
    m_RecursionDepth += 1;
} // Lock.






/////////////////////////////////////////////////////////////////////////////
//
// [Unlock]
//
// This releases a lock.
/////////////////////////////////////////////////////////////////////////////
void
CRefLock::Unlock() {
    bool fDeleteThis = false;

    if (!(m_LockFlags & LOCK_INITIALIZED)) {
        DEBUG_WARNING("CRefLock::Unlock. Lock is not initialized");
        return;
    }

    // Update the lock state while we still hold the lock.
    m_RecursionDepth = m_RecursionDepth - 1;
    if (m_RecursionDepth < 0) {
        DEBUG_WARNING("CRefLock::Unlock. More Unlocks than locks.");
        m_RecursionDepth = 0;
    }
    if (0 == m_RecursionDepth) {
        m_LockFlags &= ~LOCKED;
        if (m_LockFlags & FINAL_RELEASE_ON_UNLOCK) {
            fDeleteThis = true;
        }
    }

    m_hLockMutex.BasicUnlock();

    // We may have waited until the lock is unlocked until we can delete it.
    if (fDeleteThis) {
        DefaultReleaseImpl("Delayed release of a lock", 0);
    }
} // Unlock.






/////////////////////////////////////////////////////////////////////////////
//
// [IsLocked]
//
/////////////////////////////////////////////////////////////////////////////
bool
CRefLock::IsLocked() {
    if (m_LockFlags & LOCKED) {
        return(true);
    } else
    {
        return(false);
    }
} // IsLocked





/////////////////////////////////////////////////////////////////////////////
//
// [ReleaseImpl]
//
// This is part of the CRefCountInterface class. We implement the added feature
// here that we do not do a final release until the lock is unlocked.
/////////////////////////////////////////////////////////////////////////////
void
CRefLock::ReleaseImpl(const char *pFileName, int32 lineNum) {
   int32 newRefCount;

#if DD_DEBUG
    // Check if we are touching an object after it should have
    // been deleted.
    if (m_RefCountFlags & PENDING_DELETE) {
        DEBUG_WARNING("CRefLock::ReleaseImpl. Using a deleted refcounted object");
    }

    if (m_RefCountFlags & TRACK_REFCOUNT) {
        SignObjectImpl(
            CDebugObject::RELEASE_PROCEDURE,
            NULL,
            pFileName,
            lineNum,
            m_cRef);
    }
#endif

#if WIN32
    newRefCount = InterlockedDecrement((LONG *) &m_cRef);
#elif LINUX
    X86AtomicDecrement(&m_cRef);
    newRefCount = m_cRef;
#endif

    // Decrement the refcount. If this is the last reference to the lock,
    // then we *may* delete it, but only if it is unlocked. If it is locked,
    // then we wait until it is unlocked before deleting it.
    if (0 == newRefCount) {
#if WIN32
       InterlockedIncrement((LONG *) &m_cRef);
#elif LINUX
       X86AtomicIncrement(&m_cRef);
#endif
       if (!(m_LockFlags & LOCKED)) {
         DefaultReleaseImpl("Fake second release of a lock", -1);
       } else {
          m_LockFlags |= FINAL_RELEASE_ON_UNLOCK;
       }
    } // if (0 == newRefCount)
} // ReleaseImpl






/////////////////////////////////////////////////////////////////////////////
//
// [CRefEvent]
//
/////////////////////////////////////////////////////////////////////////////
CRefEvent::CRefEvent() {
#if WIN32
    m_hEvent = NULL;
#elif LINUX
    //m_hEvent = PTHREAD_COND_INITIALIZER;
    //m_EventLock = xxxx;
    m_NumWaiters = 0;
    m_NumSignals = 0;
#endif

    m_fInitialized = false;
} // CRefEvent.






/////////////////////////////////////////////////////////////////////////////
//
// [~CRefEvent]
//
/////////////////////////////////////////////////////////////////////////////
CRefEvent::~CRefEvent() {
    bool fSuccess = true;

    if (m_fInitialized) {
#if WIN32
        BOOL fWinSuccess = CloseHandle(m_hEvent);
        if (!fWinSuccess) {
            fSuccess = false;
        }
        m_hEvent = NULL;
#elif LINUX
        int result;
        result = pthread_cond_destroy(&m_hEvent);
        if (0 != result) {
            fSuccess = false;
        }
        pthread_mutex_destroy(&m_EventLock);
#endif
    }

    if (!fSuccess) {
        DEBUG_WARNING("Error while shutting down a CRefEvent.");
    }
} // ~CRefEvent.






/////////////////////////////////////////////////////////////////////////////
//
// [Initialize]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CRefEvent::Initialize() {
    if (m_fInitialized) {
        DEBUG_WARNING("CRefEvent::Initialize. Initializing a CRefEvent twice.");
        return(ENoErr);
    }

#if WIN32
    m_hEvent = CreateEvent(
                    NULL,    // pointer to security attributes
                    false,   // flag for manual-reset event
                    false,   // flag for initial state
                    NULL);     // pointer to event-object name
    if (NULL != m_hEvent) {
        m_fInitialized = true;
    }
#elif LINUX
    // pthread_condattr_t attr;
    pthread_mutexattr_t mutexAttr;
    int result;

    result = pthread_cond_init(&m_hEvent, NULL);
    if (0 != result) {
        goto abort;
    }

    // From the pthreads doc:
    //
    //      The pthread_cond_wait() and pthread_cond_timedwait() functions are used
    //      to block on a condition variable. They are called with mutex locked by
    //      the calling thread or undefined behaviour will result.
    //
    // So, I have to pass in a mutex or else pthread_cond_wait crashes.
    //
    // Moreover, condition variables do not store their value, so doing
    // a signal before a wait will leave the wait blocked. So, I use the lock
    // and maintain my own count of signals.
    pthread_mutexattr_init(&mutexAttr);
    result = pthread_mutexattr_settype(&mutexAttr, PTHREAD_MUTEX_RECURSIVE_NP);
    if (0 != result) {
        goto abort;
    }
    result = pthread_mutex_init(&m_EventLock, &mutexAttr);
    if (0 != result) {
       goto abort;
    }

    m_fInitialized = true;
abort:
#endif

    if (!m_fInitialized) {
        DEBUG_WARNING("CRefEvent::Initialize. Cannot initialize a CRefEvent");
        returnErr(EFail);
    }

    returnErr(ENoErr);
} // Initialize.







/////////////////////////////////////////////////////////////////////////////
//
// [Wait]
//
/////////////////////////////////////////////////////////////////////////////
void
CRefEvent::Wait() {
    if (!m_fInitialized) {
        DEBUG_WARNING("CRefEvent::Wait. Uninitialized semaphore.");
        return;
    }

#if WIN32
    DWORD dwResult = WaitForSingleObject(m_hEvent, INFINITE);
    if (WAIT_FAILED == dwResult) {
        DEBUG_WARNING("CRefEvent::Wait. Bad lock error.");
    }
#elif LINUX
    int result = 0;

    pthread_mutex_lock(&m_EventLock);
    // Loop until we have success. Several waiting threads may be
    // competing for the same signal.
    while (1) {
        if (m_NumSignals > 0) {
           m_NumSignals--;
           break;
        } else {
            m_NumWaiters += 1;
            result = pthread_cond_wait(&m_hEvent, &m_EventLock);
            m_NumWaiters--;
        }
    } // while (1)
    pthread_mutex_unlock(&m_EventLock);

    if (0 != result) {
        DEBUG_WARNING("CRefEvent::Wait. Bad lock error.");
    }
#endif
} // Wait





/////////////////////////////////////////////////////////////////////////////
//
// [Signal]
//
/////////////////////////////////////////////////////////////////////////////
void
CRefEvent::Signal() {
    if (!m_fInitialized) {
        DEBUG_WARNING("CRefEvent::Signal. Uninitialized semaphore.");
        return;
    }

#if WIN32
    BOOL fSuccess;

    fSuccess = SetEvent(m_hEvent);
    if (!fSuccess) {
        DEBUG_WARNING("CRefEvent::Signal. Error in CRefEvent::Signal.");
    }
#elif LINUX
    int result = 0;

    pthread_mutex_lock(&m_EventLock);
    m_NumSignals++;
    // If some thread is sleeping for this signal, then wake it up.
    if (m_NumWaiters > 0) {
       result = pthread_cond_signal(&m_hEvent);
    }
    pthread_mutex_unlock(&m_EventLock);

    if (0 != result) {
        DEBUG_WARNING("CRefEvent::Signal. Error");
    }
#endif
} // Signal






/////////////////////////////////////////////////////////////////////////////
//
// [Clear]
//
/////////////////////////////////////////////////////////////////////////////
void
CRefEvent::Clear() {
    if (!m_fInitialized) {
        DEBUG_WARNING("Uninitialized semaphore.");
        return;
    }

#if WIN32
    BOOL fSuccess = ResetEvent(m_hEvent);
    if (!fSuccess) {
        DEBUG_WARNING("CRefEvent::Clear. Error");
    }
#elif LINUX
    pthread_mutex_lock(&m_EventLock);
    m_NumSignals = 0;
    // If threads are waiting on the signal, then don't wake them. We
    // are clearing the signal, so they will have to wait until it is set
    // again.
    pthread_mutex_unlock(&m_EventLock);
#endif
} // Clear




/////////////////////////////////////////////////////////////////////////////
//
//                           TESTING PROCEDURES
//
/////////////////////////////////////////////////////////////////////////////
#if INCLUDE_REGRESSION_TESTS

#define NUM_TEST_LOCKS         100
#define NUM_RETRIES            2
#define NUM_MULTIPLE_LOCKS     3
#define NUM_TRIALS             2
#define REPEAT_ALL_TESTS       2

static void RunThreadTests();
static void RunLockTests();
static void TestThreadProc(void *arg, CSimpleThread *threadState);

CRefLock * locks[NUM_TEST_LOCKS];

#define NUM_TEST_THREADS 30

static CRefEvent * g_WorkerThreadReady[NUM_TEST_THREADS];
static CRefEvent * g_WorkerAllowedToFinishSems[NUM_TEST_THREADS];
static int32 g_WorkerIDs[NUM_TEST_THREADS];
static int32 g_TestValues[NUM_TEST_THREADS];



/////////////////////////////////////////////////////////////////////////////
//
// [TestThreads]
//
/////////////////////////////////////////////////////////////////////////////
void
CSimpleThread::TestThreads() {
    int32 allTestNum;
    int32 lockNum;

    g_DebugManager.StartModuleTest("Threads");

    for (lockNum = 0; lockNum < NUM_TEST_LOCKS; lockNum++) {
        locks[lockNum] = CRefLock::Alloc();
        if (NULL == locks[lockNum]) {
            DEBUG_WARNING("Error from Initialize");
        }
    }

    for (allTestNum = 0; allTestNum < REPEAT_ALL_TESTS; allTestNum++) {
        RunLockTests();
    }

    RunThreadTests();
} // TestThreads.




/////////////////////////////////////////////////////////////////////////////
//
// [RunLockTests]
//
/////////////////////////////////////////////////////////////////////////////
static void RunLockTests() {
    int32 lockNum;
    int32 trial;

    g_DebugManager.SetProgressIncrement(1);

    //////////////////////////////////////////
    g_DebugManager.StartTest("Get exclusive locks several times");

    for (trial = 0; trial < NUM_TRIALS; trial++) {
        g_DebugManager.ShowProgress();

        for (lockNum = 0; lockNum < NUM_TEST_LOCKS; lockNum++) {
            if (locks[lockNum]) {
                (locks[lockNum])->Lock();
            }
        }

        for (lockNum = 0; lockNum < NUM_TEST_LOCKS; lockNum++) {
            g_DebugManager.ShowProgress();

            if (locks[lockNum]) {
                (locks[lockNum])->Unlock();
            }
        }
    } // testing exclusive locks.
} // RunLockTests.





/////////////////////////////////////////////////////////////////////////////
//
// [TestThreadProc]
//
/////////////////////////////////////////////////////////////////////////////
static void
TestThreadProc(void *arg, CSimpleThread *threadState) {
    int32 threadNum;

    threadState = threadState;
    threadNum = *((int32 *) arg);
    g_TestValues[threadNum] = 5;

    if (g_WorkerThreadReady[threadNum]) {
        g_WorkerThreadReady[threadNum]->Signal();
    }

    if (g_WorkerAllowedToFinishSems[threadNum]) {
        g_WorkerAllowedToFinishSems[threadNum]->Wait();
    }

    if (99 != g_TestValues[threadNum]) {
        DEBUG_WARNING("g_TestValues was not set correctly.");
    }

    g_TestValues[threadNum] = 13;
    if (g_WorkerThreadReady[threadNum]) {
        g_WorkerThreadReady[threadNum]->Signal();
    }
} // TestThreadProc.







/////////////////////////////////////////////////////////////////////////////
//
// [RunThreadTests]
//
/////////////////////////////////////////////////////////////////////////////
void
RunThreadTests() {
    ErrVal err = ENoErr;
    int16 threadNum;


    for (threadNum = 0; threadNum < NUM_TEST_THREADS; threadNum++) {
        g_WorkerThreadReady[threadNum] = newex CRefEvent;
        if (NULL == g_WorkerThreadReady[threadNum]) {
            DEBUG_WARNING("Error from newex");
            return;
        }

        g_WorkerAllowedToFinishSems[threadNum] = newex CRefEvent;
        if (NULL == g_WorkerAllowedToFinishSems[threadNum]) {
            DEBUG_WARNING("Error from newex");
            return;
        }

        err = g_WorkerThreadReady[threadNum]->Initialize();
        if (err) {
            DEBUG_WARNING("Error from event->Initialize");
            return;
        }

        err = g_WorkerAllowedToFinishSems[threadNum]->Initialize();
        if (err) {
            DEBUG_WARNING("Error from event->Initialize");
            return;
        }

        g_TestValues[threadNum] = 0;
    }


    g_DebugManager.StartTest("Fork some threads");
    for (threadNum = 0; threadNum < NUM_TEST_THREADS; threadNum++) {
        g_WorkerIDs[threadNum] = threadNum;
        err = CSimpleThread::CreateThread("thread", &TestThreadProc, (void *) &(g_WorkerIDs[threadNum]), NULL);
        if (err) {
            DEBUG_WARNING("Error from CSimpleThread::CreateThread");
        }
    }



    g_DebugManager.StartTest("Wait for threads to do some work");
    for (threadNum = 0; threadNum < NUM_TEST_THREADS; threadNum++) {
        g_WorkerThreadReady[threadNum]->Wait();

        if (5 != g_TestValues[threadNum]) {
            DEBUG_WARNING("A thread did not set the right result value.");
        }

        g_TestValues[threadNum] = 99;
        g_WorkerThreadReady[threadNum]->Clear();
        g_WorkerAllowedToFinishSems[threadNum]->Signal();

        g_WorkerThreadReady[threadNum]->Wait();
        if (13 != g_TestValues[threadNum]) {
            DEBUG_WARNING("A thread did not set the right result value.");
        }
    }
} // RunThreadTests.


#endif // INCLUDE_REGRESSION_TESTS




