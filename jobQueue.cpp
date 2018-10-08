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
// Job Queue Module
//
// This implements a job queue, which contains a pool of threads and a
// list of jobs. At any moment, there may be threads waiting on jobs,
// or jobs waiting on threads.
//
// A single job may be resubmitted many times, and the job is maintained
// in a higher level in the code. For example, the blockIO will
// use jobs for pending block IO reads and writes. The higher
// level code will also decide when to delete a job.
//
// This is built on top of the basic thread module. It implements
// threads that can sleep until there is a job waiting to be done.
//
// There are three classes that make up this abstraction:
//
//      CJobQueue - A queue of jobs
//      CJob - A single job
//      CWorkerThread - A single worker thread
//
// The job class is a pure virtual class; clients will define subclasses
// that contain information specific to a type of job. Each job contains
// a run method, which actually does the work of the job.
//
// A single job may have several pending requests. New requests may be
// submitted when the job is idle, or busy, including submitting requests
// as part of finishing a previous request.
//
// A particular job may only be on ONE job queue at a time. There is
// support for multiple requests on a job within one job queue, but NO
// support for coordination between job queues.
//
/////////////////////////////////////////////////////////////////////////////

#include "osIndependantLayer.h"
#include "log.h"
#include "config.h"
#include "debugging.h"
#include "refCount.h"
#include "memAlloc.h"
#include "threads.h"
#include "queue.h"
#include "jobQueue.h"


FILE_DEBUGGING_GLOBALS(LOG_LEVEL_DEFAULT, 0);

static void JobQueueThreadProc(void *arg, CSimpleThread *pThreadState);

CJobQueue *g_MainJobQueue = NULL;

extern CConfigSection *g_pBuildingBlocksConfig;
static char MaxWorkerThreadsValueName[]         = "Max Worker Threads";



/////////////////////////////////////////////////////////////////////////////
//
// [InitializeGlobalJobQueues]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CJobQueue::InitializeGlobalJobQueues() {
    ErrVal err = ENoErr;
    int32 threadNum;
    int32 numThreads;
    int32 maxNumThreads;


    // This must be allocated (or static) so the test
    // procedure and module can quit while the threads are still
    // using it.
    g_MainJobQueue = newex CJobQueue;
    if (NULL == g_MainJobQueue) {
        returnErr(EFail);
    }

    err = g_MainJobQueue->Initialize();
    if (err) {
        returnErr(err);
    }

    // Add aditional threads. We already put one thread in the queue when we
    // created the job queue.
#if WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    numThreads = si.dwNumberOfProcessors + 1;
#else
    numThreads = 2;
#endif

    // By default, don't create a bunch of threads.
    maxNumThreads = g_pBuildingBlocksConfig->GetInt(MaxWorkerThreadsValueName, 1);
    if (numThreads > maxNumThreads) {
        numThreads = maxNumThreads;
    }

    for (threadNum = 0; threadNum < numThreads; threadNum++) {
        err = g_MainJobQueue->AddThread();
        if (err) {
            returnErr(err);
        }
    }

    returnErr(err);
} // InitializeGlobalJobQueues.





/////////////////////////////////////////////////////////////////////////////
//
// [ShutdownGlobalJobQueues]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CJobQueue::ShutdownGlobalJobQueues() {
    ErrVal err = ENoErr;

    DEBUG_LOG("CJobQueue::ShutdownGlobalJobQueues");
    if (NULL != g_MainJobQueue) {
        g_MainJobQueue->Shutdown();

        delete g_MainJobQueue;
        g_MainJobQueue = NULL;
    }

    returnErr(err);
} // ShutdownGlobalJobQueues.






/////////////////////////////////////////////////////////////////////////////
//
// [CJobQueue]
//
/////////////////////////////////////////////////////////////////////////////
CJobQueue::CJobQueue() {
    m_IdleJobs.ResetQueue();
    m_BusyThreads.ResetQueue();
    m_IdleThreads.ResetQueue();

    m_NumActualThreads = 0;
    m_NumDesiredThreads = 0;

    m_TotalActiveRequests = 0;

    m_pLock = NULL;
    m_pNoThreadsLeft = NULL;
} // CJobQueue.





/////////////////////////////////////////////////////////////////////////////
//
// [~CJobQueue]
//
/////////////////////////////////////////////////////////////////////////////
CJobQueue::~CJobQueue() {
    RELEASE_OBJECT(m_pNoThreadsLeft);
    RELEASE_OBJECT(m_pLock);
} // ~CJobQueue.






/////////////////////////////////////////////////////////////////////////////
//
// [Initialize]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CJobQueue::Initialize() {
    ErrVal err = ENoErr;

    // Initialize the lock.
    m_pLock = CRefLock::Alloc();
    if (!m_pLock) {
        gotoErr(EFail);
    }

    m_pNoThreadsLeft = newex CRefEvent;
    if (!m_pNoThreadsLeft) {
        gotoErr(EFail);
    }
    err = m_pNoThreadsLeft->Initialize();
    if (err) {
        gotoErr(err);
    }

    // Initially, there are no threads.
    m_IdleJobs.ResetQueue();
    m_BusyThreads.ResetQueue();
    m_IdleThreads.ResetQueue();

    m_NumActualThreads = 0;
    m_NumDesiredThreads = 0;

    m_TotalActiveRequests = 0;

abort:
    returnErr(err);
} // Initialize.






/////////////////////////////////////////////////////////////////////////////
//
// [Shutdown]
//
// We never delete the threads. Instead, we simply let them all complete and
// exit by themselves.
/////////////////////////////////////////////////////////////////////////////
void
CJobQueue::Shutdown() {
    CWorkerThread *pVictim;
    CWorkerThread *pNextVictim;
    bool foundActiveThreads;

    // Run any standard debugger checks.
    RunChecksOnce();

    // Get exclusive access to the entire job queue state.
    if (m_pLock) {
        m_pLock->Lock();
    }

    DEBUG_LOG("CJobQueue::Shutdown: Start shutting down");
    m_NumDesiredThreads = 0;

    // Wake up all threads and tell them to quit.
    while (1) {
        pVictim = m_IdleThreads.GetHead();
        if (!pVictim) {
            break;
        }

        // Remove it from the idle queue.
        m_IdleThreads.RemoveFromQueue(&(pVictim->m_WorkThreadQueue));

        // Add it to the busy list.
        m_BusyThreads.InsertHead(&(pVictim->m_WorkThreadQueue));

        // There is nothing to do, just wake up and and exit.
        pVictim->m_CurrentJob = NULL;
        if (pVictim->m_hJobIsWaiting) {
            pVictim->m_hJobIsWaiting->Signal();
        }
    }


    // Remove any threads that have already exited.
    pVictim = m_BusyThreads.GetHead();
    foundActiveThreads = false;
    while (NULL != pVictim) {
        pNextVictim = pVictim->m_WorkThreadQueue.GetNextInQueue();

        if (pVictim->IsRunning()) {
           foundActiveThreads = true;
        } else {
           DEBUG_LOG("CJobQueue::Shutdown: Found a busyThread that is no longer running.");
           m_BusyThreads.RemoveFromQueue(&(pVictim->m_WorkThreadQueue));
        }

        pVictim = pNextVictim;
    } // while (NULL != pVictim)


    if (m_pLock) {
        m_pLock->Unlock();
    }

    DEBUG_LOG("CJobQueue::Shutdown: foundActiveThreads = %d", foundActiveThreads);
    if ((foundActiveThreads) && (NULL != m_pNoThreadsLeft)) {
        DEBUG_LOG("CJobQueue::Shutdown: Wait on m_pNoThreadsLeft.");
        m_pNoThreadsLeft->Wait();
        DEBUG_LOG("CJobQueue::Shutdown: m_pNoThreadsLeft was signalled, time to leave.");
    } else
    {
        DEBUG_LOG("CJobQueue::Shutdown: No threads, so exit without waiting.");
    }

    RELEASE_OBJECT(m_pLock);
    RELEASE_OBJECT(m_pNoThreadsLeft);
} // Shutdown






/////////////////////////////////////////////////////////////////////////////
//
// [AddThread]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CJobQueue::AddThread() {
    ErrVal err = ENoErr;
    CWorkerThread *pNewThread = NULL;

    // Run any standard debugger checks.
    RunChecks();

    if (NULL == m_pLock) {
        returnErr(EFail);
    }
    m_pLock->Lock();

    DEBUG_LOG("CJobQueue::AddThread");

    // Create a new thread descriptor. This does not fork
    // a thread, it only creates a descriptor data structure.
    pNewThread = newex CWorkerThread;
    if (!pNewThread) {
        gotoErr(EFail);
    }

    // Initialize the pJobThread part of the thread. The
    // base class will be initialized below when we call the
    // method to fork the thread.
    pNewThread->m_CurrentJob = NULL;
    pNewThread->m_pOwnerJobQueue = this;
    pNewThread->m_hJobIsWaiting = newex CRefEvent;
    if (NULL == pNewThread->m_hJobIsWaiting) {
        gotoErr(EFail);
    }
    err = pNewThread->m_hJobIsWaiting->Initialize();
    if (err) {
        gotoErr(err);
    }

    // Add the new thread to the list of idle threads for this
    // job queue. If there is a job to do, then the thread
    // will be explicitly assigned to it below.
    m_IdleThreads.InsertHead(&(pNewThread->m_WorkThreadQueue));
    m_NumActualThreads += 1;
    m_NumDesiredThreads += 1;

    // Now, actually fork the thread. The thread has nothing
    // to do, so it will immediately sleep until we assign
    // it a new job.
    err = pNewThread->RunThread("", JobQueueThreadProc, NULL);
    if (err) {
        gotoErr(err);
    }

    // If there were jobs waiting for threads, then assign a job
    // to the new thread.
    if (m_IdleJobs.GetLength() > 0) {
        DEBUG_LOG("CJobQueue::AddThread. Assigning a job to a new thread");
        err = AssignJobToThread();
        if (err) {
            gotoErr(err);
        }
    }

abort:
    m_pLock->Unlock();
    returnErr(err);
} // AddThread.





/////////////////////////////////////////////////////////////////////////////
//
// [SubmitJob]
//
// The client does not have to initialize any of the fields
// of the job base class. These are all initialized here. The
// client does, however, have to initialize any job-specific
// information.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CJobQueue::SubmitJob(CJob *pJobPtr) {
    ErrVal err = ENoErr;

    // Run any standard debugger checks.
    // Do NOT check the new job argument, since it has not yet
    // been initialized.
    RunChecks();

    if ((NULL == m_pLock) || (NULL == pJobPtr)) {
        returnErr(EFail);
    }

    // Record the new job before we get the lock. We do not want
    // to hold the job queue lock while we log data. We also do not
    // want to dispatch the job before we log that it is being added
    // to the queue. BE CAREFUL. We are reading the queue sizes outside
    // of the lock, so they may be inconsistent.

    // Get exclusive access to the entire job queue state.
    m_pLock->Lock();

    ADDREF_OBJECT(pJobPtr);

    m_TotalActiveRequests += 1;

    // If this is the first time this job has been submitted, then
    // add it to the queue.
    // NOTE: if pJobPtr->m_NumWorkRequests >= 1, then this job may
    // be EITHER busy or idle. It cannot change state, however,
    // as long as we hold the lock.
    if (0 == pJobPtr->m_NumWorkRequests) {
        // Add the job to the idle list; it is idle until we can
        // match it to a thread.
        m_IdleJobs.InsertTail(&(pJobPtr->m_JobList));

        // Initialize the base class; the caller does not have to
        // do this, since the caller may not know about *any* of
        // these fields.
        pJobPtr->m_JobFlags = 0;
        pJobPtr->m_CurrentThread = NULL;
        pJobPtr->m_pOwnerJobQueue = this;
    }

    pJobPtr->m_NumWorkRequests += 1;

    // If there were threads waiting for jobs, then assign the
    // job to a thread.
    if (m_IdleThreads.GetLength() > 0) {
        err = AssignJobToThread();
        if (err) {
            gotoErr(err);
        }
    }

abort:
    m_pLock->Unlock();

    returnErr(err);
} // SubmitJob.







/////////////////////////////////////////////////////////////////////////////
//
// [CheckState]
//
// This is part of the debugObject class.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CJobQueue::CheckState() {
    ErrVal err = ENoErr;
    CJob *pJob;
    CWorkerThread *thread;
    int count;
    int32 totalNumPendingRequests;

    if (!m_pLock) {
        returnErr(EFail);
    }
    m_pLock->Lock();


    if (m_TotalActiveRequests < 0) {
        gotoErr(EFail);
    }


    totalNumPendingRequests = 0;


    // CHECK IDLE JOBS
    pJob = m_IdleJobs.GetHead();
    count = 0;
    while (pJob) {
        if (pJob->m_JobFlags & CJob::JOB_IS_BUSY) {
            gotoErr(EFail);
        }
        if (pJob->m_CurrentThread) {
            gotoErr(EFail);
        }
        if (pJob->m_pOwnerJobQueue != this) {
            gotoErr(EFail);
        }

        err = pJob->CheckJobState();
        if (err) {
            gotoErr(EFail);
        }

        totalNumPendingRequests += pJob->m_NumWorkRequests;
        count += 1;
        pJob = pJob->m_JobList.GetNextInQueue();
    } // inspecting every idle job.

    if (count != m_IdleJobs.GetLength()) {
        gotoErr(EFail);
    }



    // CHECK IDLE THREADS
    thread = m_IdleThreads.GetHead();
    count = 0;
    while (thread) {
        if (thread->m_CurrentJob) {
            gotoErr(EFail);
        }
        if (thread->m_pOwnerJobQueue != this) {
            gotoErr(EFail);
        }

        count += 1;
        thread = thread->m_WorkThreadQueue.GetNextInQueue();
    }

    if (count != m_IdleThreads.GetLength()) {
        gotoErr(EFail);
    }



    // CHECK BUSY THREADS
    thread = m_BusyThreads.GetHead();
    count = 0;
    while (thread) {
        if (!(thread->m_CurrentJob)) {
            gotoErr(EFail);
        }
        if (thread->m_pOwnerJobQueue != this) {
            gotoErr(EFail);
        }

        err = thread->m_CurrentJob->CheckJobState();
        if (err) {
            gotoErr(EFail);
        }

        totalNumPendingRequests += thread->m_CurrentJob->m_NumWorkRequests;

        count += 1;
        thread = thread->m_WorkThreadQueue.GetNextInQueue();
    }


    if (count != m_BusyThreads.GetLength()) {
        gotoErr(EFail);
    }



    // Check the queues.
    err = m_IdleJobs.CheckState();
    if (err) {
        gotoErr(EFail);
    }
    err = m_BusyThreads.CheckState();
    if (err) {
        gotoErr(EFail);
    }
    err = m_IdleThreads.CheckState();
    if (err) {
        gotoErr(EFail);
    }


    if (totalNumPendingRequests != m_TotalActiveRequests) {
        gotoErr(EFail);
    }


abort:
    m_pLock->Unlock();
    returnErr(err);
} // CheckState.






/////////////////////////////////////////////////////////////////////////////
//
// [AssignJobToThread]
//
// This assigns idle jobs to idle threads. It transfers threads and jobs
// from the idle to busy lists and wakes threads up.
//
// NOTE: This assumes that it has exclusive access to the job queue. This
// means that whoever called this routine is holding the lock to the queue.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CJobQueue::AssignJobToThread() {
    CWorkerThread *pNewThread;
    CJob *pNewJob;

    // Run any standard debugger checks.
    RunChecks();

    if (NULL == m_pLock) {
        returnErr(EFail);
    }
    ASSERT(m_pLock->IsLocked());

    // Keep working as long as there are idle threads
    // and idle jobs. Each iteration assigns 1 job to 1 thread.
    while ((m_IdleJobs.GetLength() > 0) && (m_IdleThreads.GetLength() > 0)) {
        // Get the oldest idle job and move it to the busy list.
        // IMPORTANT. Get the oldest or else we can starve a job.
        pNewJob = m_IdleJobs.GetHead();

        // Remove it from the idle queue.
        m_IdleJobs.RemoveFromQueue(&(pNewJob->m_JobList));

        // Get the first idle thread and move it to the busy list.
        // It does not matter whether we use tyhe oldest or newest.
        // All threads use the same code, so there is no difference
        // in cache hits.
        pNewThread = m_IdleThreads.GetHead();

        // Remove it from the idle queue.
        m_IdleThreads.RemoveFromQueue(&(pNewThread->m_WorkThreadQueue));

        // Add it to the busy list.
        m_BusyThreads.InsertHead(&(pNewThread->m_WorkThreadQueue));

        // Associate this job with this thread.
        pNewJob->m_CurrentThread = pNewThread;
        pNewThread->m_CurrentJob = pNewJob;

        // Mark the job as busy.
        pNewJob->m_JobFlags |= CJob::JOB_IS_BUSY;

        // Wake the thread up.
        pNewThread->m_hJobIsWaiting->Signal();
    } // assigning idle jobs to idle threads.

    returnErr(ENoErr);
} // AssignJobToThread.





/////////////////////////////////////////////////////////////////////////////
//
// [JobQueueThreadProc]
//
// This runs 1 thread. It is called by the thread library and passes control
// to the work thread method. This routine only returns when the thread is
// exiting.
/////////////////////////////////////////////////////////////////////////////
void
JobQueueThreadProc(void *arg, CSimpleThread *pThreadState) {
    CWorkerThread *pWorkThreadState;
    CJobQueue *ownerQueue;

    arg = arg;

    // Get the thread and job queue for this routine. This
    // procedure will be called once for each busy thread.
    if (NULL == pThreadState) {
        return;
    }

    pWorkThreadState = (CWorkerThread *) pThreadState;
    ownerQueue = pWorkThreadState->m_pOwnerJobQueue;
    if (NULL == ownerQueue) {
        return;
    }

    // When this routine exits, the work thread is done.
    ownerQueue->RunWorkThread(pWorkThreadState);
} // JobQueueThreadProc.





/////////////////////////////////////////////////////////////////////////////
//
// [RunWorkThread]
//
// This runs 1 thread. It is called by the thread library when the thread
// is first forked. It loops until the thread is asked to quit. Each
// iteration will sleep until there is one or more jobs to do or the
// thread should quit. Each iteration does one or several jobs and then
// (when there are no jobs left to do) it goes back to sleep. This routine
// is woken up by the AssignJobToThread method each time it is assigned
// a new job. It is also woken up by the remove threads library when it
// is asked to quit.
//
// This routine only returns when the thread is being deleted.
/////////////////////////////////////////////////////////////////////////////
void
CJobQueue::RunWorkThread(CWorkerThread *pThreadInfo) {
    bool quitNow;

    if ((NULL == m_pLock) || (NULL == pThreadInfo)) {
        return;
    }

    // This is the main loop. Each iteration runs one job.
    // We iterate this loop for the life of the thread, when
    // we exit this loop, the thread exits.
    while (true) {
        pThreadInfo->m_hJobIsWaiting->Wait();

        // Important, we may have both a job to do and be told to quit.
        // Always do the job first before being told to quit. The job
        // processing logic will only do the assigned job (it won't find
        // more work to do on its own) if we have been told to quit.

        // Run while there are jobs left to do. This will return the
        // thread to the idle queue.
        quitNow = DoWaitingJobs(pThreadInfo);
        if (quitNow) {
            break;
        }
    } // running jobs until we quit.


    // We were left on the idle queue. Now, remove the thread from
    // the idle queue and clean up its state.
    m_pLock->Lock();

    // Remove the thread from the idle queue. Now, the thread is not
    // on any queues, so it has effectively disappeared from the
    // job queue.
    m_IdleThreads.RemoveFromQueue(&(pThreadInfo->m_WorkThreadQueue));

    // If this is the last thread to quit, then tell the
    // queue that all threads have quit.
    if ((m_IdleThreads.GetLength() == 0) && (m_BusyThreads.GetLength() == 0)) {
        CRefEvent *pNoThreadsLeft = m_pNoThreadsLeft;

        m_pLock->Unlock();
        if (pNoThreadsLeft) {
            DEBUG_LOG("CJobQueue::RunWorkThread Signalling m_pNoThreadsLeft.");
            pNoThreadsLeft->Signal();
        }
    } else
    {
        m_pLock->Unlock();
    }
} // RunWorkThread.







/////////////////////////////////////////////////////////////////////////////
//
// [DoWaitingJobs]
//
// This is called by each worker thread, so there may be several instances
// of this method running on different threads at the same time.
// This is called by the worker thread, but it manipulates the global
// job queue.
//
// This runs all jobs for a thread while it is awake; it returns when
// there are no more jobs to do and the thread should go back to sleep.
//
// This only holds the queue lock while it gets a job for a new thread.
// It releases the lock when it is actually executing a job, or just before
// the thread is about to go back to sleep.
//
// When this routine has been called, the means this thread has been placed
// on the busy list and assigned a job. It may find more jobs on its own
// after it does the first job.
//
/////////////////////////////////////////////////////////////////////////////
bool
CJobQueue::DoWaitingJobs(CWorkerThread *pThreadInfo) {
    CJob *pCurrentJob;
    bool quitNow = false;

    if ((NULL == m_pLock) || (NULL == pThreadInfo)) {
        return(true);
    }

    // If there is not a job to do, then we may have been just
    // woken up so we can quit. Grab the queue lock so we can
    // check whether we should quit and either quit or become
    // idle again.
    pCurrentJob = pThreadInfo->m_CurrentJob;
    if (NULL == pCurrentJob) {
        m_pLock->Lock();
        goto noJobsToDo;
    }

    // Loop as long as there are jobs to do. When there are
    // no more idle jobs, then exit the loop and the thread goes
    // back to sleep.
    while (1) {
        // This does the entire job.
        pCurrentJob->ProcessJob(pThreadInfo);

        // We have to update the queue, so get exclusive
        // access to the entire job queue state.
        m_pLock->Lock();

        // This will dispose of the job we just finished, and look
        // for a new job to do.
        FinishJob(pCurrentJob, pThreadInfo);

        // IMPORTANT. If we quit, hold onto the lock so we can remove
        // ourselves from the busy queue.
        pCurrentJob = GetNextJob(pThreadInfo);
        if (NULL == pCurrentJob) {
            break; // quitting while holding onto the lock.
        }

        // If we have more jobs to do, then we are done updating
        // the queue for now so release the lock while we do the
        // next job.
        m_pLock->Unlock();
    } // doing jobs.

noJobsToDo:
    // When we quit the loop, we are still holding onto the lock.
    // This is important, since we need it to remove ourselves
    // from the busy queue.
    ASSERT(m_pLock->IsLocked());

    // Remove the thread from the busy queue.
    m_BusyThreads.RemoveFromQueue(&(pThreadInfo->m_WorkThreadQueue));

    // WARNING! This may be a race condition on some platforms.
    // If somebody else tries to wake this thread up by
    // calling it before we sleep on the event, then the
    // wake-up call may be lost. On Windows this is not
    // a problem, and I think Linux is safe with this as well.

    // Add the thread to the idle list.
    m_IdleThreads.InsertHead(&(pThreadInfo->m_WorkThreadQueue));

    // If there are more threads than needed, then signal
    // that we can quit this thread.
    if (m_NumActualThreads > m_NumDesiredThreads) {
        DEBUG_LOG("CJobQueue::DoWaitingJobs: (m_NumActualThreads > m_NumDesiredThreads), so this thread is quitting");
        m_NumActualThreads = m_NumActualThreads - 1;
        quitNow = true;
    }

    m_pLock->Unlock();

    return(quitNow);
} // DoWaitingJobs.







/////////////////////////////////////////////////////////////////////////////
//
// [FinishJob]
//
// This is called by the job thread routine when it completes one job.
/////////////////////////////////////////////////////////////////////////////
void
CJobQueue::FinishJob(CJob *pJob, CWorkerThread *pJobThread) {
    if (NULL != m_pLock) {
        ASSERT(m_pLock->IsLocked());
    }

    if ((NULL == pJob) || (NULL == pJobThread)) {
        return;
    }

    pJobThread->m_CurrentJob = NULL;

    // Mark the job as no longer busy.
    pJob->m_JobFlags &= ~CJob::JOB_IS_BUSY;

    pJob->m_CurrentThread = NULL;
    pJob->m_pOwnerJobQueue = NULL;
    pJob->m_NumWorkRequests = pJob->m_NumWorkRequests - 1;

    // If the job had several work requests, the put it back on
    // the idle queue. Put it at the end of the queue, however,
    // so it doesn't starve other jobs.
    if (pJob->m_NumWorkRequests > 0) {
        // Add the job to the idle list; it is idle until we can
        // match it to a thread.
        m_IdleJobs.InsertTail(&(pJob->m_JobList));

        pJob->m_JobFlags = 0;
        pJob->m_CurrentThread = NULL;
        pJob->m_pOwnerJobQueue = this;
    }

    // A job is AddRef'ed every time it is submitted.
    // WARNING. This may delete the job, so do this AFTER we no
    // longer need the job data structure.
    RELEASE_OBJECT(pJob);

    // Decrement the count AFTER we Release the job. This lets us know
    // when it is safe to start looking for cell leaks.
    m_TotalActiveRequests = m_TotalActiveRequests - 1;
} // FinishJob.





/////////////////////////////////////////////////////////////////////////////
//
// [GetNextJob]
//
// This is called by the job thread routine when it completes one job.
/////////////////////////////////////////////////////////////////////////////
CJob *
CJobQueue::GetNextJob(CWorkerThread *pJobThread) {
    CJob *pJob;

    if (NULL != m_pLock) {
        ASSERT(m_pLock->IsLocked());
    }

    // Look on the idle job list for a new job to do.
    pJob = m_IdleJobs.GetHead();
    if (NULL == pJob) {
        return(NULL);
    }

    // Remove it from the idle queue.
    m_IdleJobs.RemoveFromQueue(&(pJob->m_JobList));

    // Associate this job with this thread.
    pJob->m_CurrentThread = pJobThread;
    pJobThread->m_CurrentJob = pJob;

    pJob->m_JobFlags |= CJob::JOB_IS_BUSY;

    return(pJob);
} // GetNextJob





/////////////////////////////////////////////////////////////////////////////
//
// [GetNumJobs]
//
/////////////////////////////////////////////////////////////////////////////
int32
CJobQueue::GetNumJobs() {
    int32 value;

    m_pLock->Lock();
    value = m_TotalActiveRequests;
    m_pLock->Unlock();

    return(value);
} // GetNumJobs.







/////////////////////////////////////////////////////////////////////////////
//
// [CWorkerThread]
//
/////////////////////////////////////////////////////////////////////////////
CWorkerThread::CWorkerThread() : m_WorkThreadQueue(this) {
    m_hJobIsWaiting = NULL;

    m_pOwnerJobQueue = NULL;
    m_CurrentJob = NULL;
} // CWorkerThread







/////////////////////////////////////////////////////////////////////////////
//
// [CJob]
//
/////////////////////////////////////////////////////////////////////////////
CJob::CJob() : m_JobList(this) {
    m_JobFlags = 0;
    m_NumWorkRequests = 0;

    m_CurrentThread = NULL;
    m_pOwnerJobQueue = NULL;
} // CJob





/////////////////////////////////////////////////////////////////////////////
//
// [CheckJobState]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CJob::CheckJobState() {
    ErrVal err = ENoErr;
    CJob *pJob;
    CWorkerThread *pThread;


    if (!m_pOwnerJobQueue) {
        returnErr(EFail);
    }

    // Get exclusive access to the entire job queue state.
    (m_pOwnerJobQueue->m_pLock)->Lock();

    // If the job is running, then there should be a current thread.
    if (m_JobFlags & CJob::JOB_IS_BUSY) {
        if (!m_CurrentThread) {
            gotoErr(EFail);
        }

        // Check the thread
        if (m_CurrentThread->m_CurrentJob != this) {
            gotoErr(EFail);
        }
        if (m_CurrentThread->m_pOwnerJobQueue != m_pOwnerJobQueue) {
            gotoErr(EFail);
        }

        // Look for the job's thread in the busy threads queue.
        pThread = m_pOwnerJobQueue->m_BusyThreads.GetHead();
        while (pThread) {
            if (pThread == m_CurrentThread) {
                break;
            }
            pThread = pThread->m_WorkThreadQueue.GetNextInQueue();
        }
        if (NULL == pThread) {
            gotoErr(EFail);
        }

        if (m_NumWorkRequests <= 0) {
            gotoErr(EFail);
        }
    } // checking a busy job's state.




    if (!(m_JobFlags & CJob::JOB_IS_BUSY)) {
        if (m_CurrentThread) {
            gotoErr(EFail);
        }

        // Look for the job on the idle queue.
        pJob = m_pOwnerJobQueue->m_IdleJobs.GetHead();
        while (pJob) {
            if (pJob == this) {
                break;
            }

            pJob = pJob->m_JobList.GetNextInQueue();
        }

        if (NULL == pJob) {
            gotoErr(EFail);
        }

        if (0 == m_NumWorkRequests) {
            gotoErr(EFail);
        }
    } // checking an idle job's state.

abort:
    (m_pOwnerJobQueue->m_pLock)->Unlock();

    returnErr(err);
} // CheckJobState.








/////////////////////////////////////////////////////////////////////////////
//
//                   TESTING PROCEDURES
//
/////////////////////////////////////////////////////////////////////////////
#if INCLUDE_REGRESSION_TESTS

#define NUM_TEST_JOBS 10



class CTestJob : public CRefCountImpl, public CJob
{
    virtual void ProcessJob(CSimpleThread *pThreadState);

    // CRefCountInterface
    PASS_REFCOUNT_TO_REFCOUNTIMPL()
};




static CJobQueue *g_TestJobQueue;
static CTestJob * g_TestJobs[NUM_TEST_JOBS];
static bool g_TestJobsDone[NUM_TEST_JOBS];



/////////////////////////////////////////////////////////////////////////////
//
// [ProcessJob]
//
/////////////////////////////////////////////////////////////////////////////
void
CTestJob::ProcessJob(CSimpleThread *pThreadState) {
    int32 jobNum;

    pThreadState = pThreadState; // Compiler warnings.

    for (jobNum = 0; jobNum < NUM_TEST_JOBS; jobNum++) {
        if (this == g_TestJobs[jobNum]) {
            break;
        }
    }

    if (jobNum >= NUM_TEST_JOBS) {
        DEBUG_WARNING("Bad job");
        return;
    }

    g_TestJobsDone[jobNum] = true;
} // ProcessJob.








/////////////////////////////////////////////////////////////////////////////
//
// [TestJobQueue]
//
/////////////////////////////////////////////////////////////////////////////
void
CJobQueue::TestJobQueue() {
    ErrVal err = ENoErr;
    int jobNum;

    g_DebugManager.StartModuleTest("Job Queue");

    for (jobNum = 0; jobNum < NUM_TEST_JOBS; jobNum++) {
        g_TestJobsDone[jobNum] = false;
    }

    // WARNING. this must be allocated (or static) so the test
    // procedure and module can quit while the threads are still
    // using it.
    g_TestJobQueue = newex CJobQueue;
    if (NULL == g_TestJobQueue) {
        DEBUG_WARNING("Cannot create an event");
        return;
    }

    g_TestJobQueue->SetDebugFlags(CDebugObject::CHECK_STATE_ON_EVERY_OP);


#if WIN32
    g_DebugManager.StartTest("Event Race Conditions");
    HANDLE     event;

    event = CreateEvent(NULL,    // pointer to security attributes
                        false,    // flag for manual-reset event
                        false,    // flag for initial state
                        NULL);     // pointer to event-object name
    if (!event)
        DEBUG_WARNING("Cannot create an event");

    (void) SetEvent(event);
    (void) WaitForSingleObject(event, INFINITE);
#endif


    g_DebugManager.StartTest("Fork some threads");

    err = g_TestJobQueue->Initialize();
    if (err) {
        DEBUG_WARNING("Error from init");
    }

    err = g_TestJobQueue->AddThread();
    if (err) {
        DEBUG_WARNING("Error from init");
    }
    err = g_TestJobQueue->AddThread();
    if (err) {
        DEBUG_WARNING("Error from init");
    }
    err = g_TestJobQueue->AddThread();
    if (err) {
        DEBUG_WARNING("Error from init");
    }
    // Each queue starts with 1 thread.


    g_DebugManager.StartTest("Run jobs");

    for (jobNum = 0; jobNum < NUM_TEST_JOBS; jobNum++) {
        g_TestJobs[jobNum] = NULL;
    }


    for (jobNum = 0; jobNum < NUM_TEST_JOBS; jobNum++) {
        g_TestJobs[jobNum] = newex CTestJob;

        err = g_TestJobQueue->SubmitJob(g_TestJobs[jobNum]);
        if (err) {
            DEBUG_WARNING("Error from SubmitJob.");
        }
    }


    for (jobNum = 0; jobNum < NUM_TEST_JOBS; jobNum++) {
        while (!(g_TestJobsDone[jobNum])) {
           OSIndependantLayer::SleepForMilliSecs(100);
        }
    }


    g_DebugManager.StartTest("Stop all threads");
    g_TestJobQueue->Shutdown();
    delete g_TestJobQueue;
} // TestJobQueue.


#endif // INCLUDE_REGRESSION_TESTS

