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

#ifndef _JOB_QUEUE_H_
#define _JOB_QUEUE_H_


class CJobQueue;
class CWorkerThread;



/////////////////////////////////////////////////////////////////////////////
// This is the base class for a single job.
class CJob : public CRefCountInterface {
public:
    CJob();
    NEWEX_IMPL()

    virtual void ProcessJob(CSimpleThread *pThreadState) = 0;

private:
    friend class CJobQueue;
    friend class CWorkerThread;

    enum JobFlags {
        JOB_IS_BUSY = 0x0001
    };

    ErrVal CheckJobState();

    int16                   m_JobFlags;
    int16                   m_NumWorkRequests;

    CWorkerThread           *m_CurrentThread;

    CQueueHook<CJob>        m_JobList;

    // This is owned by the queue that manages this job.
    // This is always valid, even when the job is not assigned
    // to a busy thread.
    CJobQueue               *m_pOwnerJobQueue;
}; // CJob






/////////////////////////////////////////////////////////////////////////////
// This is a single job queue.
class CJobQueue : public CDebugObject {
public:
    static ErrVal InitializeGlobalJobQueues();
    static ErrVal ShutdownGlobalJobQueues();

#if INCLUDE_REGRESSION_TESTS
    static void TestJobQueue();
#endif

    CJobQueue();
    virtual ~CJobQueue();
    NEWEX_IMPL()

    ErrVal Initialize();
    void Shutdown();
    ErrVal AddThread();

    // This is an async procedure call. The job will
    // eventually be run in a thread pool.
    ErrVal SubmitJob(CJob *jobPtr);

    // This is public so the threadProc can call it. Clients
    // of this module, however, should not call this procedure.
    void RunWorkThread(CWorkerThread *threadInfo);

    // CDebugObject
    virtual ErrVal CheckState();

    int32 GetNumJobs();

private:
    friend class CJob;
    friend class CWorkerThread;

    CRefLock                    *m_pLock;
    CRefEvent                   *m_pNoThreadsLeft;

    CQueueList<CJob>            m_IdleJobs;

    CQueueList<CWorkerThread>   m_BusyThreads;
    CQueueList<CWorkerThread>   m_IdleThreads;

    int32                       m_NumActualThreads;
    int32                       m_NumDesiredThreads;

    int32                       m_TotalActiveRequests;


    ErrVal AssignJobToThread();

    bool DoWaitingJobs(CWorkerThread *threadInfo);

    void FinishJob(CJob *pJob, CWorkerThread *jobThread);

    CJob *GetNextJob(CWorkerThread *jobThread);
}; // CJobQueue





/////////////////////////////////////////////////////////////////////////////
// This describes a thread that is part of a job queue. This
// is a subclass of a thread and simply adds new information
// for job queues.
class CWorkerThread : public CSimpleThread {
public:
    CWorkerThread();
    NEWEX_IMPL()

    // This is public so the OS-specific thread callback can get it.
    CJobQueue                   *m_pOwnerJobQueue;

private:
    friend class CJobQueue;
    friend class CJob;

    CQueueHook<CWorkerThread>   m_WorkThreadQueue;
    CJob                        *m_CurrentJob;
    CRefEvent                   *m_hJobIsWaiting;
}; // CWorkerThread



#endif // _JOB_QUEUE_H_

