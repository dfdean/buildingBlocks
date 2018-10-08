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
// Reference Counted Objects Header
//
// This module implements a both pure abstract interface class and a concrete
// base class for reference counted objects. The abstract class may be used with
// any other interface class to make it a reference counted object. A single
// concrete object may multiply inherit from the abstract class. The concrete
// base class implements reference counting with debugging support, such as
// tracking who AddRefs and Releases an object for leak detection, and a pending
// delete list that helps catch problems like double-releases or using an object
// after their final release.
//
//
//
// Many different base classes may support AddRef/Release, so they will all
// inherit from CRefCountInterface. A single concrete class may inherit from
// many base classes, so it may inherit from CRefCountInterface several times.
//
// A concrete class may inherit from CRefCountImpl, which implements a single
// implementation of AddRef/Release. Each concrete class should pass its calls
// to AddRef/Release through to CRefCountImpl.
//
// This gives us the best of both worlds. First, many pure virtual base classes
// can support AddRef/Release by inheriting from CRefCountInterface and a single
// concrete class can subclass many of these base classes. Second, all concrete
// subclasses share a single implementation of refcount objects.
//
// Here is an example:
//
// class B1 : public CRefCountInterface
// {
// };
// class B2 : public CRefCountInterface
// {
// };
// class B3 : public CRefCountInterface
// {
// };
// class CConcrete : public CRefCountImpl,
//                     public B1, // also inherits from CRefCountInterface
//                     public B2, // also inherits from CRefCountInterface
//                     public B3 // also inherits from CRefCountInterface
// {
//     // CRefCountInterface
//     virtual void AddRefImpl() { return(DefaultAddRefImpl()); }
//     virtual void ReleaseImpl() { return(DefaultReleaseImpl()); }
// };
//
// For convenience, the base class can include the macro:
//
//     PASS_REFCOUNT_TO_REFCOUNTIMPL()
//
// Which just expands to the AddRefImpl and ReleaseImpl functions.
//
// Any concrete class must declare CRefCountImpl as its first base class so
// CRefCountImpl's VTable appears first. This ensures that pointers to a
// CRefCountImpl object are also pointers to the concrete object, and so
// we can cast a pointer to a CRefCountImpl object into any other class
// without vtable alignment problems.
//
/////////////////////////////////////////////////////////////////////////////

#include "osIndependantLayer.h"
#include "log.h"
#include "config.h"
#include "debugging.h"
#include "memAlloc.h"
#include "refCount.h"

FILE_DEBUGGING_GLOBALS(LOG_LEVEL_DEFAULT, 0);

extern bool g_ShutdownBuildingBlocks;

#if DD_DEBUG
OSIndependantLock CRefCountImpl::g_GlobalDeleteListLock;
CRefCountImpl *CRefCountImpl::g_pObjectsWaitingRelease = NULL;
CRefCountImpl *CRefCountImpl::g_pLastObjectWaitingRelease = NULL;
// Default to 20 seconds.
uint32 CRefCountImpl::g_MinTimeOnPendingDeleteQueueInMs = 20 * 1000;
#endif



#if LINUX
///////////////////////////////////////////////////////////////
// Warning. I have no idea what I am doing here.
//
// Here is some inline assembler to do an atomic increment and
// decrement of a variable.
//
// For further reading, you may want to check out some of these:
// 1. http://www.ibm.com/developerworks/library/l-ia.html
// 2. http://www.ibiblio.org/gferg/ldp/GCC-Inline-Assembly-HOWTO.html
//
// First, the inline GNU assembler uses AT&T syntax, not Intel syntax.
// So, each instruction has the format:   <op> <src> <dest>
// Register names are prefixed with %, and constants are prefixed with $.
// Op names end with a b,w,l if they are byte,word,long operations.
// Memory indirection uses (), so (p) is the value that p points to.
//
// The inline assembler statement has 4 sections:
//   __asm__ __volatile__(
//                "<A list of instructions>"
//                : <Output operands>        // Optional
//                : <Input operands>         // Optional
//                : "<Clobbered registers>"  // Optional
//                );
//
// In the assembly instructions, operations may take arguments like %0 or %1.
// The %0 refers to the 0th operand, which are declared in the
// input and output operand sections of the asm statement. Operands
// are numbered 0,1,2,.... The input and output operand sections
// are combined for indexing purposes. So if you have 3 outputs
// and 3 inputs, then %5 would refer to the 5th operand, which is
// number 6 in the list (index starts at 0) and this is the last
// input operand. The list starts with the outputs.
//
// The operands lists specify the inputs and outputs for the assembly code.
// Each operand has a constraint, which specifies how the data will be
// accessed in assembly code. This seems to map C variables to registers and
// memory locations in asm. There are lots of constraints, but the two
// most important are:
//    r(x) This means a C variable X is accessed through a register.
//    m(x) This means a C variable X is accessed through memory.
//
// =m() means the value is write-only.
// +m() means the value is both read and written.
//
// The clobber list is a list of registers that are changed by the
// asm. This is tricky since GCC can assign registers for you. In
// my code below, I don't pass through registers, so nothing is
// clobbered. However, "cc" in the clobber list means we can alter
// the condition code register.
// Additionally, "memory" in the clobber list says that unpredictable
// memory may be affected by the inline asm. I use this in the lfence
// instructions. This tells GCC to not cache memory values
// in registers across the assembler instruction.
//
// The volatile keyword is added to the asm statement to tell the compiler
// not to move (loop lift) or otherwise reorder this instruction.
//
// Inside the assembly instructions, I use the "lock" prefix. This tells
// the processor to lock the meory address being manipulated so it cannot
// be cuncurrently used by another processor. This guarantees exclusive
// access in a multi-processor environment.
//
//
// Finally, the lfence is a memory barrier. See these for more information.
// 1. http://en.wikipedia.org/wiki/Memory_barrier
// 2. http://www.linuxjournal.com/article/8211
// 3. http://www.linuxjournal.com/article/8212
// 4. http://lxr.linux.no/source/Documentation/memory-barriers.txt
//
// Any modern processor will change the order of memory operations so
// it is different than the order they appear in the code. This re-ordering
// is done for performance; memory is so much slower than processors so
// accesses are reordered to optimize cache usage.
// A single processor will only reorder memory accesses so it does not affect
// the code. However, two processors may each reorder in such a way that
// they conflict. For example, if processor #1 first writes X, then Y, then
// processor #2 must see the value of X change before Y.
//
// A fence instruction forces a multiprocessor to reset any out-of-order
// memory instructions. A full fence forces all processors to ensure that
// any load or store operation issued before the fence will be committed and
// visible to all processors before any any new load or store operations
// are issued. So, it is a global sync-point.
//
// On x86, the lfence is a memory barrier on loads, and mfence is a memory
// barrier on stores.
///////////////////////////////////////////////////////////////
void
X86AtomicDecrement(int32 *p) {
   // code lifted from linux
   asm volatile (
                "lock; decl %0"
                : "+m" (*p)    // Output operands.
                :              // Input operands
                : "cc"         // Clobbered registers
                );
   asm volatile (
               "mfence"
               :
               :
               : "memory"
               );
}

///////////////////////////////////////////////////////////////
void
X86AtomicIncrement(int32 *p) {
   asm volatile (
                "lock; incl %0"
                : "+m" (*p)    // Output operands.
                :              // Input operands
                : "cc"         // Clobbered registers
                );
   asm volatile (
               "mfence"
               :
               :
               : "memory"
               );
}
#endif // LINUX





/////////////////////////////////////////////////////////////////////////////
//
// [InitializeGlobalState]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CRefCountImpl::InitializeGlobalState() {
    ErrVal err = ENoErr;

#if DD_DEBUG
    err = g_GlobalDeleteListLock.Initialize();
    if (err) {
        return(err);
    }
#endif

    return(err);
} // InitializeGlobalState.






/////////////////////////////////////////////////////////////////////////////
//
// [CRefCountImpl]
//
/////////////////////////////////////////////////////////////////////////////
CRefCountImpl::CRefCountImpl() {
    m_cRef = 1;
    m_RefCountFlags = 0;
} // CRefCountImpl.





/////////////////////////////////////////////////////////////////////////////
//
// [~CRefCountImpl]
//
/////////////////////////////////////////////////////////////////////////////
CRefCountImpl::~CRefCountImpl() {
    if (m_cRef > 0) {
        // We ignore refcount errors when the server shuts down. Global
        // variables may call their destructors without calling Release.
        if (!g_ShutdownBuildingBlocks) {
            DEBUG_WARNING("Deleting a refcounted object with a non-zero refcount");
        }
    }

    if (m_RefCountFlags & DONT_DELETE) {
        // We ignore refcount errors when the server shuts down. Global
        // variables may call their destructors without calling Release.
        if (!g_ShutdownBuildingBlocks) {
            DEBUG_WARNING("Deleting a refcounted object with a non-zero refcount");
        }
    }
} // ~CRefCountImpl.





/////////////////////////////////////////////////////////////////////////////
//
// [DefaultAddRefImpl]
//
/////////////////////////////////////////////////////////////////////////////
void
CRefCountImpl::DefaultAddRefImpl(const char *pFileName, int32 lineNum) {
#if DD_DEBUG
    // Check if we are touching an object after it should have
    // been deleted.
    if (m_RefCountFlags & PENDING_DELETE) {
        DEBUG_WARNING("Using a deleted refcounted object");
    }

    if (m_RefCountFlags & TRACK_REFCOUNT) {
        SignObjectImpl(
            CDebugObject::ADDREF_PROCEDURE,
            NULL,
            pFileName,
            lineNum,
            m_cRef);
    }
#endif

#if WIN32
    InterlockedIncrement((LONG *) &m_cRef);
#elif LINUX
    X86AtomicIncrement(&m_cRef);
#endif
} // DefaultAddRefImpl.





/////////////////////////////////////////////////////////////////////////////
//
// [DefaultReleaseImpl]
//
/////////////////////////////////////////////////////////////////////////////
void
CRefCountImpl::DefaultReleaseImpl(const char *pFileName, int32 lineNum) {
   int32 newRefCount;

#if DD_DEBUG
    // Check if we are touching an object after it should have
    // been deleted.
    if (m_RefCountFlags & PENDING_DELETE) {
        DEBUG_WARNING("Using a deleted refcounted object");
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

    if (0 == newRefCount) {
#if DD_DEBUG
        if (m_RefCountFlags & DONT_DELETE) {
            // We ignore refcount errors when the server shuts down. Global
            // variables may call their destructors without calling Release.
            if (!g_ShutdownBuildingBlocks) {
                DEBUG_WARNING("Deleting a refcounted object with a non-zero refcount");
            }
        }

        if (m_RefCountFlags & TRACK_REFCOUNT) {
            AddToPendingDeleteList(this);
            return;
        } // (m_RefCountFlags & TRACK_REFCOUNT)
#endif

        delete this;
        return;
    } // Release the last reference.
} // DefaultReleaseImpl.







/////////////////////////////////////////////////////////////////////////////
//
// [RecordConstructor]
//
/////////////////////////////////////////////////////////////////////////////
void
CRefCountImpl::RecordConstructor(const char *pFileName, int32 lineNum) {
#if DD_DEBUG
    SignObjectImpl(
        CDebugObject::CONSTRUCTOR,
        NULL,
        pFileName,
        lineNum,
        0);
#endif
} // RecordConstructor.






/////////////////////////////////////////////////////////////////////////////
//
// [TrackRefCount]
//
/////////////////////////////////////////////////////////////////////////////
void
CRefCountImpl::TrackRefCount() {
#if DD_DEBUG
    m_RefCountFlags |= TRACK_REFCOUNT;
#endif
} // TrackRefCount.




/////////////////////////////////////////////////////////////////////////////
//
// [SetDontDelete]
//
/////////////////////////////////////////////////////////////////////////////
void
CRefCountImpl::SetDontDelete(bool fOn) {
    if (fOn) {
        m_RefCountFlags |= DONT_DELETE;
    } else
    {
        m_RefCountFlags &= ~DONT_DELETE;
    }
} // SetDontDelete.





/////////////////////////////////////////////////////////////////////////////
//
// [AddToPendingDeleteList]
//
/////////////////////////////////////////////////////////////////////////////
void
CRefCountImpl::AddToPendingDeleteList(CRefCountImpl *pTarget) {
    if (NULL == pTarget) {
        return;
    }

#if DD_DEBUG
    uint64 nowInMs = GetTimeSinceBootInMs();

    g_GlobalDeleteListLock.BasicLock();

    // Put this on the end of the pending release list.
    pTarget->m_pNextOnPendingDeleteList = NULL;

    if (NULL != g_pLastObjectWaitingRelease) {
        g_pLastObjectWaitingRelease->m_pNextOnPendingDeleteList = pTarget;
    }
    g_pLastObjectWaitingRelease = pTarget;

    if (NULL == g_pObjectsWaitingRelease) {
        g_pObjectsWaitingRelease = pTarget;
    }

    pTarget->m_dwFinalReleaseTime = nowInMs;
    pTarget->m_RefCountFlags |= PENDING_DELETE;

    pTarget = NULL;

    // Prune old items from the pending delete list.
    while (g_pObjectsWaitingRelease) {
        if ((nowInMs - g_pObjectsWaitingRelease->m_dwFinalReleaseTime)
            >= g_MinTimeOnPendingDeleteQueueInMs) {
            pTarget = g_pObjectsWaitingRelease;

            g_pObjectsWaitingRelease = g_pObjectsWaitingRelease->m_pNextOnPendingDeleteList;
            if (g_pLastObjectWaitingRelease == pTarget) {
                g_pLastObjectWaitingRelease = NULL;
            }

            delete pTarget;
        } // Prune one old item.
        else {
            break;
        }
    } // Prune old items from the pending delete list.

    g_GlobalDeleteListLock.BasicUnlock();
#endif // DD_DEBUG
} // AddToPendingDeleteList.






/////////////////////////////////////////////////////////////////////////////
//
// [EmptyPendingDeleteList]
//
/////////////////////////////////////////////////////////////////////////////
void
CRefCountImpl::EmptyPendingDeleteList() {
#if DD_DEBUG
    CRefCountImpl *pTarget;

    g_GlobalDeleteListLock.BasicLock();

    // Prune all items from the pending delete list.
    while (g_pObjectsWaitingRelease) {
        pTarget = g_pObjectsWaitingRelease;
        g_pObjectsWaitingRelease = g_pObjectsWaitingRelease->m_pNextOnPendingDeleteList;
        if (g_pLastObjectWaitingRelease == pTarget) {
            g_pLastObjectWaitingRelease = NULL;
        }

        delete pTarget;
    } // Prune old items from the pending delete list.

    g_GlobalDeleteListLock.BasicUnlock();
#endif // DD_DEBUG
} // EmptyPendingDeleteList.






/////////////////////////////////////////////////////////////////////////////
//
//                       TESTING PROCEDURES
//
// Config testing is a little special. Test modules are built on top
// of config, so config does not use the standard test package.
// It simply returns an error code indicating whether it passed its
// tests or not.
/////////////////////////////////////////////////////////////////////////////
#if INCLUDE_REGRESSION_TESTS

static bool g_fDestructorRan = false;


///////////////////////////////////////
class CTestRefCountObject : public CRefCountInterface,
                              public CRefCountImpl
{
public:
   CTestRefCountObject() { };
   virtual ~CTestRefCountObject() { g_fDestructorRan = true; }

   NEWEX_IMPL()
   PASS_REFCOUNT_TO_REFCOUNTIMPL()
}; // CParsedUrlOpenerSynchCallback




/////////////////////////////////////////////////////////////////////////////
//
// [TestConfig]
//
/////////////////////////////////////////////////////////////////////////////
void
CRefCountImpl::TestRefCounting() {
    ErrVal err = ENoErr;
    CTestRefCountObject *pObject = NULL;
    CTestRefCountObject *pSaveObject = NULL;
    int32 index;
    int32 refCount;

    LowLevelReportTestModule("RefCount");

    /////////////////////////////////////////////////
    LowLevelReportStartTest("Adding and Releasing.");

    pObject = newex CTestRefCountObject;
    if (NULL == pObject) {
       gotoErr(EFail);
    }

    for (index = 1; index < 100; index++) {
       ADDREF_OBJECT(pObject);

       refCount = pObject->GetRefCount();
       if (refCount != (index + 1)) {
          DEBUG_WARNING("Bad RefCount");
       }
    }
    for (index = 100; index > 1; index--) {
       pSaveObject = pObject;
       RELEASE_OBJECT(pObject);
       pObject = pSaveObject;

       refCount = pObject->GetRefCount();
       if (refCount != (index - 1)) {
          DEBUG_WARNING("Bad RefCount");
       }
    }

    if (g_fDestructorRan) {
        DEBUG_WARNING("The Destructor ran prematurely");
    }
    RELEASE_OBJECT(pObject);
    if (!g_fDestructorRan) {
        DEBUG_WARNING("The Destructor never ran");
    }

    return;

abort:
    REPORT_LOW_LEVEL_BUG();
    return;
} // TestRefCounting.


#endif // INCLUDE_REGRESSION_TESTS


