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

#ifndef _MEM_ALLOC_H_
#define _MEM_ALLOC_H_



/////////////////////////////////////////////////////////////////////////////
// This is the state for a single buffer allocation pool.
class CMemAlloc : public CDebugObject {
public:
#if INCLUDE_REGRESSION_TESTS
    static void TestAlloc();
#endif
    enum CMemAllocConstants {
        // These are the options passed to PrintAllocations
        PRINT_VERBOSE           = 0x1000,
        PRINT_ONLY_CHANGES      = 0x2000,
    };


    CMemAlloc();
    virtual ~CMemAlloc();

    virtual ErrVal Initialize(int32 initialFlags);

    void *Alloc(int32 numDataBytes) { return(Alloc(numDataBytes, NULL, 0)); }
    void *Alloc(int32 numDataBytes, const char *pFileName, int lineNum);
    void Free(const void *block);
    void *Calloc(int32 numDataBytes, const char *pFileName, int lineNum);
    void *Realloc(void *pOldPtr, int32 numDataBytes);

    void *AllocPages(int32 numPages, const char *pFileName, int lineNum);
    int32 GetBytesPerPage() { return(m_PageSize); }
    int32 GetPtrSize(const void *ptr);

    // CDebugObject
    virtual ErrVal CheckState();
    ErrVal CheckPtr(const void *ptr);

    // Memory Leak detection
    virtual void PrintAllocations(
                        int32 flags, 
                        char *pDestPtr, 
                        int32 maxSize, 
                        int32 *pResultSize);
    void CollectMemoryStats(int32 flags);
    int32 GetNumAllocations(int32 flags);
    void DontCountCurrentAllocations();
    void DontCountMemoryAsLeaked(const void *ptr);

protected:
    enum PrivateMemAllocConstants {
        ILLEGAL_ADDR_MASK           = 0x00000003,

        LOG_MIN_BUFFER_SIZE         = 5,
        LOG_MAX_BUFFER_SIZE         = 30,
        MIN_BUFFER_SIZE             = (1 << LOG_MIN_BUFFER_SIZE),
        MAX_BUFFER_SIZE             = (1 << LOG_MAX_BUFFER_SIZE),

        // These are the options set in a single buffer.
        // To save space in every buffer, these fit in
        // a 16-bit flag word.
        BUFFER_ALLOCATED           = 0x0001,
        MEM_FREE_BUFFER_CACHED     = 0x0002,
        BUFFER_HAS_FOOTER          = 0x0004,
        BUFFER_HAS_FREE_PATTERN    = 0x0008,
        BUFFER_PRINTED             = 0x0010,

        // 16-bit buffer constants.
        HEADER_MAGIC_WORD           = 0xDFD3,
        FOOTER_MAGIC_WORD           = 0xFEDC,

        MEM_POOL_GROW_SIZE          = (1 << 16),

        // The free buffer cache.
        LOG2_NUM_CACHED_SIZES       = 10,
        NUM_CACHED_SIZES            = (1 << LOG2_NUM_CACHED_SIZES),
        MAX_CACHEABLE_SIZE          = ((1 << LOG2_NUM_CACHED_SIZES) - 1)
    };


    ////////////////////////////////////////////
    // This is the header of every memory buffer, whether it is allocated
    // or free.
    struct CBufferHeaderType {
        // These have to be unsigned values. The hex constants are
        // treated as unsigned, so if we do a compare, the hex is
        // expanded to a positive 32 bit num, and this must not be
        // sign extended to a signed 32 bit num.
        uint16                      m_MagicNum;
        uint16                      m_Flags;

#if DD_DEBUG
        const char                  *m_pFileName;
        int32                       m_LineNum;
#endif

        int32                       m_NumDataBytes;
        struct CBufferHeaderType    *m_pNext;
        struct CBufferHeaderType    *m_pPrev;
    }; // CBufferHeaderType


    ////////////////////////////////////////////
    // This is every free buffer. Basically, it stores
    // linked list pointers in the body of the buffer.
    struct CFreeBufferType {
        struct CBufferHeaderType    m_Header;

        struct CFreeBufferType      *m_pNextFree;
        struct CFreeBufferType      *m_pPrevFree;
    }; // CFreeBufferType


    ////////////////////////////////////////////
    // These are added to the end of memory buffers
    // when an option is set. This helps us detect
    // if the client is overwriting the buffer. We
    // cannot optionally add such a buffer to the head,
    // since the head must always be a fixed size
    // from the base of the user data. That lets us
    // translate a pointer to the user data to a
    // pointer to the header on calls to free.
    //
    // WARNING. This may not be even-byte aligned,
    // depending on whether the user allocates an
    // even number of bytes.
    struct CBufferFooterType {
        // These have to be unsigned values. The hex constants are
        // treated as unsigned, so if we do a compare, the hex is
        // expanded to a positive 32 bit num, and this must not be
        // sign extended to a signed 32 bit num.
        uint16                      m_MagicNum;
    }; // CBufferFooterType


    ////////////////////////////////////////////
    // This is what a new chunk of memory looks like
    // when it is allocated. This chunk will then
    // be broken up into buffers and allocated to
    // the client.
    struct CBaseBlockHeaderType {
        int32                       m_NumBytesAfterHeader;

        struct CBaseBlockHeaderType *m_pNext;
    }; // CBaseBlockHeaderType


    ////////////////////////////////////////////
    // This is a list of free buffers for a particular size.
    class CCachedFreeBufferList {
    public:
        int32               m_FreeBufferSize;

        int32               m_NumFreeBuffers;
        int32               m_MaxNumFreeBuffers;
        int32               m_NumAllocsWithEmptyCache;
        int32               m_NumCacheMissesBeforeIncreaseCacheSize;
        int32               m_NumCacheSizeIncreases;

        CFreeBufferType     *m_pFreeBuffers;
    };


    // These are utility methods that are only used privately.
    virtual ErrVal AddStorageImpl(char * basePtr, int32 bufferSize);

    CBaseBlockHeaderType * GetBaseBlock(const void *ptr);

    void AddToFreeList(CFreeBufferType *freeBuf);
    void RemoveFromFreeList(CFreeBufferType *header, int32 sizeClass);

    ErrVal AllocateVirtualMemoryRegion(int32 bufferSize);
    void ReleaseVirtualMemoryRegion(CBaseBlockHeaderType * basePtr);

    void *GrowBuffer(
                CBufferHeaderType *header,
                const void *buffer,
                int32 newSize,
                int32 allocSize,
                int32 newAllocSize);
    void ShrinkBuffer(
                CBufferHeaderType *header,
                const void *buffer,
                int32 newSize,
                int32 newAllocSize);


    OSIndependantLock       m_Lock;

    uint32                  m_Flags;
    bool                    m_fInitialized;

    int32                   m_PageSize;
    uint32                  m_PageAddressMask;
    uint32                  m_PageOffsetMask;

    int32                   m_TotalMem;
    int32                   m_TotalAllocBytes;
    int32                   m_TotalAllocBuffers;

    // This is the free block cache. It saves freed blocks
    // that have not been coalesced yet.
    CCachedFreeBufferList   m_FreeBufferCache[NUM_CACHED_SIZES];
    int32                   m_NumSizeIncreasesBeforeRaiseSensitivity;

    // freeList[i] is the pointer to the next free block of size
    // 2^(i+kMinLogSize). The smallest allocatable block is
    // 2**kMinLogSize bytes. The overhead information precedes
    // the data area returned to the user.
    CFreeBufferType *       m_FreeReassembledBufferList[LOG_MAX_BUFFER_SIZE + 1];

    // This is the list of memory regions we can allocate
    // buffers from.
    CBaseBlockHeaderType    *m_BaseBlocks;
}; // CMemAlloc



extern CMemAlloc g_MainMem;

// These are simple extern C prototypes so they can be accessed by a top-level
// UI without including this header file.
extern void DontCountAllCurrentAllocations();
extern void CheckForMemoryLeaks(
                char *pStartBuffer, 
                int32 maxSize, 
                int32 *pResultSize);


/////////////////////////////////////////////////////////////////////////////
//
//                             NEW AND DELETE
//
// To allocate random memory from my memory pool, call memAlloc and memFree.
// These work just like the normal libc alloc and free. That's fine.
// However, to allocate a class object from my memory pool, you must do 2 things:
//
// 1. Call newex, instead of new. This passes in the fileName and lineNum to
// new. That lets us track memory leaks and is incredibly useful.
//
// 2. Add the line "NEWEX_IMPL()" to the class definition.
// I don't like this, but I don't see any alternative. I used to overload the
// global free-store new and delete functions. That creates all kind of problems
// for any application that is linked into a framework, like ATL, MFC, or something
// else. The problem is ther code calls my delete, and passes in pointers they
// allocated, so I cannot delete them. Basically, globally replacing the entire
// free-store is dangerous, especially since my own free store must be initialized
// and cleaned up when the program starts and stops. So, I use per-class new/delete
// methods, which requires the NEWEX_IMPL() macro.
/////////////////////////////////////////////////////////////////////////////

#define NEWEX_IMPL() void operator delete (void *p) { g_MainMem.Free(p); } \
            void operator delete(void *p, const char *fName, int32 lineNum) { fName = fName; lineNum = lineNum; g_MainMem.Free(p); } \
            void* operator new(size_t sz, const char *fName, int32 lineNum) { fName = fName; lineNum = lineNum; return(g_MainMem.Alloc((int32) sz, fName, lineNum)); } \
            void* operator new [](size_t sz, const char *fName, int32 lineNum) { fName = fName; lineNum = lineNum; return(g_MainMem.Alloc((int32) sz, fName, lineNum)); }


// Here are the stubs to insert debug information into the memory allocation calls.
#if DD_DEBUG
#define newex new(__FILE__, __LINE__)
#define memAlloc(size) g_MainMem.Alloc(size, __FILE__, __LINE__)
#define memAllocPages(numPages) g_MainMem.AllocPages(numPages, __FILE__, __LINE__)
#define memCalloc(size) g_MainMem.Calloc(size, NULL, 0)
#define memFree(p) { if (NULL != (p)) { g_MainMem.Free(p); (p) = NULL; } }
#else
#define newex new(NULL, 0)
#define memAlloc(size) g_MainMem.Alloc(size, NULL, 0)
#define memAllocPages(numPages) g_MainMem.AllocPages(numPages, NULL, 0)
#define memCalloc(size) g_MainMem.Calloc(size, NULL, 0)
#define memFree(p) { if (NULL != (p)) { g_MainMem.Free(p); (p) = NULL; } }
#endif



/////////////////////////////////////////////////////////////////////////////
//
// String Allocators
//
/////////////////////////////////////////////////////////////////////////////

char *strdupexImpl(
                const char *pVoidPtr,
                int32 strLength,
                const char *pFileName,
                int32 lineNum);

#define strdupex(str) strdupexImpl(str, -1, __FILE__, __LINE__)
#define strndupex(str, len) strdupexImpl(str, len, __FILE__, __LINE__)



char *strCatExImpl(
            const char *pVoidPtr,
            const char *pSuffixVoidPtr,
            const char *pFileName,
            int32 lineNum);

#define strCatEx(str, pSuffix) strCatExImpl(str, pSuffix, __FILE__, __LINE__)



#endif // _MEM_ALLOC_H_

