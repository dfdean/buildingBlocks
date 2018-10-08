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

#ifndef _BUILDING_BLOCKS_REFCOUNT_H_
#define _BUILDING_BLOCKS_REFCOUNT_H_



/////////////////////////////////////////////////////////////////////////////
// This is the VIRTUAL base class for all refcounted objects.
// See the corresponding .cpp file for a description of this module.
class CRefCountInterface {
public:
    virtual void AddRefImpl(const char *pFileName, int32 lineNum) = 0;
    virtual void ReleaseImpl(const char *pFileName, int32 lineNum) = 0;
}; // CRefCountInterface.





/////////////////////////////////////////////////////////////////////////////
// This is the base class for CONCRETE objects that are refcounted objects.
// See the corresponding .cpp file for a description of this module.
class CRefCountImpl : public CDebugObject {
public:
#if INCLUDE_REGRESSION_TESTS
    static void TestRefCounting();
#endif

    static ErrVal InitializeGlobalState();
    static void EmptyPendingDeleteList();

    CRefCountImpl();

    int32 GetRefCount() { return(m_cRef); }

#if DD_DEBUG
    // Record who refcounts and releases an object objects to
    // debug memory leaks.
    virtual void TrackRefCount();
    void RecordConstructor(const char *pFileName, int32 lineNum);

    // This also debugs memory leaks. The caller can say that he holds
    // a reference and does not expect an object to be rleleased. This helps
    // spot problems like an object is released too many times and somebody's
    // reference suddenly becomes invalid out from under them.
    void SetDontDelete(bool fOn);
#endif // DD_DEBUG

protected:
    enum CRefCountImplPrivateConstants {
        // Flags
        TRACK_REFCOUNT          = 0x01,
        DONT_DELETE             = 0x02,
        PENDING_DELETE          = 0x04,
    };

    // Prevent calls to delete on a refcounted object. This must be
    // protected, not private, so subclasses can access it.
    virtual ~CRefCountImpl();

    // Make these virtual, so the "this" pointer will reference the
    // allocated object. Otherwise, "this" points to the CRefCountImpl
    // class in the objects vtable, which is not what we allocated.
    virtual void DefaultAddRefImpl(const char *pFileName, int32 lineNum);
    virtual void DefaultReleaseImpl(const char *pFileName, int32 lineNum);

    // We can manage a list of objects that have refcount 0 and
    // will be deleted. This delayed deletion is helpful when tracking
    // problems like an object is used after its final release.
    static void AddToPendingDeleteList(CRefCountImpl *pTarget);

    int32                       m_cRef;
    int8                        m_RefCountFlags;

#if DD_DEBUG
    CRefCountImpl               *m_pNextOnPendingDeleteList;
    int64                       m_dwFinalReleaseTime;

    // The global pending release list.
    static OSIndependantLock    g_GlobalDeleteListLock;
    static CRefCountImpl        *g_pObjectsWaitingRelease;
    static CRefCountImpl        *g_pLastObjectWaitingRelease;
    static uint32               g_MinTimeOnPendingDeleteQueueInMs;
#endif // DD_DEBUG
}; // CRefCountImpl


// These macros perform 3 functions:
// 1. They both check that a pointer is non-NULL before calling AddRef or Release on it.
// 2. The release call will NULL out a pointer after calling release on it.
// 3. They both pass in the fileName and line number to track who addRef's and releases an object.
#define ADDREF_OBJECT(pObject) if (NULL != (pObject)) { (pObject)->AddRefImpl(__FILE__, __LINE__); }
#define RELEASE_OBJECT(pObject) if (NULL != (pObject)) { (pObject)->ReleaseImpl(__FILE__, __LINE__); pObject = NULL; }
#define ADDREF_THIS() AddRefImpl(__FILE__, __LINE__)
#define RELEASE_THIS() ReleaseImpl(__FILE__, __LINE__)

// This is what should appear in every concrete class.
#define PASS_REFCOUNT_TO_REFCOUNTIMPL() \
            virtual void AddRefImpl(const char *pFileName, int32 lineNum) { DefaultAddRefImpl(pFileName, lineNum); } \
            virtual void ReleaseImpl(const char *pFileName, int32 lineNum) { DefaultReleaseImpl(pFileName, lineNum); } //



#endif // _BUILDING_BLOCKS_REFCOUNT_H_


