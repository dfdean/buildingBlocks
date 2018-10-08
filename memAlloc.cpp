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
// Memory Allocator
//
// This implements the main memory allocator. It provided the basic
// functionality of malloc, plus leak detection, validity checks, and
// lots of debugging support. It also provides fast allocation by lazily
// recombining free blocks. This means the memory pool tunes itself to act
// as a free list for the commonly allocated buffer sizes. As a result, you
// can use a single global memory pool instead of many different cell pools
// for fast allocation. This reduces the memory footprint, which offsets the
// cost of contention on a single pool.
//
/////////////////////////////////////////////////////////////////////////////

#include "osIndependantLayer.h"
#include "stringLib.h"
#include "log.h"
#include "config.h"
#include "debugging.h"
#include "memAlloc.h"

#include "buildingBlocks.h"

FILE_DEBUGGING_GLOBALS(LOG_LEVEL_DEFAULT, 0);


#if LINUX
#include <unistd.h>  //extern "C" void *sbrk(intptr_t incr);
extern "C" int getpagesize();
#endif // if LINUX

CMemAlloc g_MainMem;
extern bool g_ShutdownBuildingBlocks;
extern CConfigSection *g_pBuildingBlocksConfig;

// This MUST be a multiple of 4. The body starts at the end of this,
// and the body must start on a 4-byte boundary.
#if DD_DEBUG
static const int32 g_AlignedSpaceForHeader 
        = 8 + (2 * INT16_SIZE_IN_STRUCT_IN_BYTES) + (3 * POINTER_SIZE_IN_BYTES);
#else
static const int32 g_AlignedSpaceForHeader
        = 4 + (2 * INT16_SIZE_IN_STRUCT_IN_BYTES) + (2 * POINTER_SIZE_IN_BYTES);

#endif

static bool IsMemSet(const void *pVoidBuffer, int32 size, unsigned char byteVal);



/////////////////////////////////////////////////////////////////////////////
//
//                     MEMORY ALLOCATION STATISTICS
//
// This is used to dump information about the current state of memory.
/////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////
class CMemoryAllocationClient {
public:
    const char  *pFileName;
    int         lineNum;

    int32       sizeOfEachItem;

    int32       numItemsInUse;
    int32       totalSizeInUse;
}; // CMemoryAllocationClient

#define MAX_ALLOCATION_REPORTS      400
static CMemoryAllocationClient g_AllocationDumpList[MAX_ALLOCATION_REPORTS];
static int32 g_NumAllocationInfoItems = 0;
static int32 g_NumAllocationOverflowInfoItems = 0;

static void QSortMemUsageArray(
                    int32 leftIndex,
                    int32 rightIndex);
static void ReportOneInstanceOfMemoryUse(
                    const char *pFileName,
                    int32 lineNum,
                    int32 allocationSize);

extern void PrepareEngineToRecordAllocations(bool fInIOCallback);




/////////////////////////////////////////////////////////////////////////////
//
// [CMemAlloc]
//
/////////////////////////////////////////////////////////////////////////////
CMemAlloc::CMemAlloc() {
    m_Flags = 0;
    m_fInitialized = false;

    m_PageSize = 0;
    m_PageAddressMask = 0;
    m_PageOffsetMask = 0;

    m_TotalMem = 0;
    m_TotalAllocBytes = 0;
    m_TotalAllocBuffers = 0;

    m_BaseBlocks = NULL;
}; // CMemAlloc.




/////////////////////////////////////////////////////////////////////////////
//
// [CMemAlloc]
//
/////////////////////////////////////////////////////////////////////////////
CMemAlloc::~CMemAlloc() {
    m_BaseBlocks = NULL;
};




/////////////////////////////////////////////////////////////////////////////
//
// [Initialize]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CMemAlloc::Initialize(int32 initialFlags) {
    ErrVal err = ENoErr;
    int32 sizeClass;
    int32 index;
    CCachedFreeBufferList *pCachedBufferList;

    err = m_Lock.Initialize();
    if (err) {
        gotoErr(err);
    }

    // Record machine-specific information.
#if LINUX
    m_PageSize = getpagesize();
#elif WIN32
    SYSTEM_INFO  systemInfo;
    GetSystemInfo(&systemInfo);
    m_PageSize = systemInfo.dwPageSize;
#endif // WIN32

    m_PageAddressMask = m_PageSize - 1;
    m_PageAddressMask = ~m_PageAddressMask;
    m_PageOffsetMask = m_PageSize - 1;

    m_Flags = initialFlags;

    // Make all pools of free buffers empty. We only start to
    // allocate buffers when the user asks for one and we
    // have none.
    for (sizeClass = 0; sizeClass <= LOG_MAX_BUFFER_SIZE; sizeClass++) {
        m_FreeReassembledBufferList[sizeClass] = NULL;
    }

    m_TotalMem = 0;
    m_TotalAllocBytes = 0;
    m_TotalAllocBuffers = 0;

    m_BaseBlocks = NULL;

    // Initialize the free block cache.
    for (index = 0; index < NUM_CACHED_SIZES; index++) {
        pCachedBufferList = &(m_FreeBufferCache[index]);

        pCachedBufferList->m_FreeBufferSize = index;
        pCachedBufferList->m_NumFreeBuffers = 0;
        pCachedBufferList->m_MaxNumFreeBuffers = 1;
        pCachedBufferList->m_NumAllocsWithEmptyCache = 0;
        pCachedBufferList->m_NumCacheMissesBeforeIncreaseCacheSize = 4;
        pCachedBufferList->m_NumCacheSizeIncreases = 0;
        pCachedBufferList->m_pFreeBuffers = NULL;
    }
    // Larger buffers need to be used more before we start
    // to cache them. They are more expensive to cache.
    for (index = 0; index < 6; index++) {
        pCachedBufferList = &(m_FreeBufferCache[index]);
        pCachedBufferList->m_NumCacheMissesBeforeIncreaseCacheSize = 3;
    }
    m_NumSizeIncreasesBeforeRaiseSensitivity = 3;

    m_fInitialized = true;

abort:
    returnErr(err);
} // Initialize.








/////////////////////////////////////////////////////////////////////////////
//
// [Alloc]
//
// This is the main allocator. new and memAlloc call this.
//
// If a buffer is allocated or cached, then m_NumDataBytes does NOT
// include the footer. If a buffer is free, then there is no footer,
// so m_NumDataBytes is the total size (including what was once the footer).
// For example, a buffer's m_NumDataBytes may increase when it is freed
// because m_NumDataBytes grows to take into account all of the extra memory.
/////////////////////////////////////////////////////////////////////////////
void *
CMemAlloc::Alloc(int32 numDataBytes, const char *pFileName, int lineNum) {
    ErrVal err = ENoErr;
    CFreeBufferType *pFreeBuffer = NULL;
    CBufferHeaderType *pHeader;
    CBufferHeaderType *pUpperHeader;
    char *pBuffer = NULL;
    char *pUpperBuffer;
    int32 allocSize;
    int32 classSize;
    int32 extraSpace;
    int32 growSize;
    int32 sizeClass;
    int32 upperBufferSize;
    bool expandedPool;
    int32 origSizeClass;
#if DD_DEBUG
    CBufferFooterType trailer;
    char *pSrcPtr;
    char *pEndSrcPtr;
    char *pDestPtr;
#endif
    int32 freeBufSizeClass;
    CCachedFreeBufferList *pCachedBufferList;


    // Run any standard debugger checks.
    RunChecks();

    m_Lock.BasicLock();

    // Make sure that the arguments are legal.
    if (numDataBytes < 0) {
        gotoErr(EFail);
    }

    // Sometimes, when we run in the MFC or ATL framework, the surrounding
    // code will call our new or delete operators. This is in spite of the fact
    // that they are declared inline and only defined in our header files. This
    // means ATL code is calling a new and delete function that is not defined
    // in any header it includes. In this case,
    if (!m_fInitialized) {
#if WIN32
        pBuffer = (char *) HeapAlloc(GetProcessHeap(), 0, numDataBytes);
        goto allocatedMemoryFromOS;
#else
        DEBUG_WARNING("Uninitialized Heap");
#endif
    }

    //////////////////////////////////////
    // FAST PATH
    //
    // Check if there is a cached free entry in the buffer.
    //
    // We use the pHeader->m_NumDataBytes as the cache size. The actual
    // size may be larger, to include space for the footer, rounded up size,
    // unused space between buffers and more. But, we can be sure that
    // a buffer that can serve N pHeader->m_NumDataBytes once can be
    // recycled again for the same size.
    if (numDataBytes <= MAX_CACHEABLE_SIZE) {
        pCachedBufferList = &(m_FreeBufferCache[numDataBytes]);
        if (NULL != pCachedBufferList->m_pFreeBuffers) {
            ASSERT(numDataBytes == pCachedBufferList->m_FreeBufferSize);

            pFreeBuffer = pCachedBufferList->m_pFreeBuffers;
            pCachedBufferList->m_pFreeBuffers = pFreeBuffer->m_pNextFree;
            pCachedBufferList->m_NumFreeBuffers = pCachedBufferList->m_NumFreeBuffers - 1;

            // Get a pointer to the part of the buffer that holds user data.
            pHeader = (CBufferHeaderType *) pFreeBuffer;
            pBuffer = (char *) pFreeBuffer;
            pBuffer += g_AlignedSpaceForHeader;

            pHeader->m_Flags &= ~MEM_FREE_BUFFER_CACHED;

            goto foundCachedFreeBuffer;
        } else {
            pCachedBufferList->m_NumAllocsWithEmptyCache += 1;
            // If we have found an empty cache several times, then increase the
            // cache size.
            if (pCachedBufferList->m_NumAllocsWithEmptyCache >= pCachedBufferList->m_NumCacheMissesBeforeIncreaseCacheSize) {
                // Increase the cache size.
                pCachedBufferList->m_MaxNumFreeBuffers += 1;
                pCachedBufferList->m_NumAllocsWithEmptyCache = 0;
                pCachedBufferList->m_NumCacheSizeIncreases += 1;

                // If we are continually underestimating the right size, then get more
                // sensitive to increasing the cache size.
                if ((pCachedBufferList->m_NumCacheSizeIncreases > m_NumSizeIncreasesBeforeRaiseSensitivity)
                    && (pCachedBufferList->m_NumCacheMissesBeforeIncreaseCacheSize > 1)) {
                    pCachedBufferList->m_NumCacheMissesBeforeIncreaseCacheSize
                        = pCachedBufferList->m_NumCacheMissesBeforeIncreaseCacheSize - 1;
                    pCachedBufferList->m_NumCacheSizeIncreases = 0;
                }
            }
        }
    } // FAST PATH



    // First get the real size of the memory request. This is the
    // size of the user data plus the size of the overhead.
    allocSize = numDataBytes;
#if DD_DEBUG
    allocSize += sizeof(CBufferFooterType);
#endif

    // To save overhead, we don't deal with anything smaller than a
    // certain size.
    if (allocSize < MIN_BUFFER_SIZE) {
        allocSize = MIN_BUFFER_SIZE;
    }

    // The data is also allocated in multiples of 4 bytes. This
    // insures that allocated buffers are always 4-byte aligned,
    // so the header at the front of the buffer can be accessed
    // as a normal C struct. This also ensures that the trailer will
    // be on a 4 byte alignment.
    while (allocSize & ILLEGAL_ADDR_MASK) {
        allocSize += 1;
    }


    // After all rounding up, make sure the size if ok.
    if (allocSize > MAX_BUFFER_SIZE) {
        gotoErr(EFail);
    }


    // Now, find the size class of the block we want to allocate.
    // This will be the smallest N such that 2**N is larger than
    // the size of buffer we want to allocate.
    classSize = MIN_BUFFER_SIZE;
    sizeClass = LOG_MIN_BUFFER_SIZE;
    while (classSize < allocSize) {
        classSize <<= 1;
        sizeClass += 1;
    }
    origSizeClass = sizeClass;

    // The class size should be <= MAX_BUFFER_SIZE
    // because allocSize < MAX_BUFFER_SIZE.
    ASSERT(sizeClass <= LOG_MAX_BUFFER_SIZE);

    // This loop iterates until we find a block or give up.
    freeBufSizeClass = -1;
    expandedPool = false;
    while (1) {
        // Look at every size class that is big enough to hold
        // this block.
        while (sizeClass <= LOG_MAX_BUFFER_SIZE) {
            // Look at every free buffer in this size class for either
            // the first or best fit.
            pFreeBuffer = m_FreeReassembledBufferList[sizeClass];
            while (pFreeBuffer) {
                extraSpace = (pFreeBuffer->m_Header).m_NumDataBytes - allocSize;

                // We are using first-fit, so stop with the first
                // buffer that is big enough.
                if (extraSpace >= 0) {
                    freeBufSizeClass = sizeClass;
                    goto foundReassembledBuffer;
                } // looking at one free buffer.

                pFreeBuffer = pFreeBuffer->m_pNextFree;
            } // looking at all free blocks in this storage class.

            // Otherwise, we cannot find any free block in this storage
            // class. In this case, look in the next larger class. We
            // will have to find some larger block and split it.
            sizeClass++;
        } // looking through all free lists whose blocks are big enough.

        // If we looked at every storage class, and we already allocated
        // more core and we still cannot find a block, then either we cannot
        // allocate more, or else the system is out of VM, or the process
        // has expanded its address space as much as it can, or the client
        // is probably trying to allocate something too huge. In any case,
        // we fail to allocate.
        if (expandedPool) {
            gotoErr(EFail);
        }

        // Otherwise, try to allocate more memory. This may fail if we
        // are allocating out of a non-resizeable buffer, or any of the
        // reasons listed above.
        growSize = MEM_POOL_GROW_SIZE;

        while (growSize
                <= ((int32) (allocSize + sizeof(CBaseBlockHeaderType) + g_AlignedSpaceForHeader))) {
            growSize = growSize << 1;
        }
        if (growSize > MAX_BUFFER_SIZE) {
            growSize = MAX_BUFFER_SIZE;
        }

        err = AllocateVirtualMemoryRegion(growSize);
        if (err) {
            gotoErr(err);
        }

        // Go back and search all possible size classes again.
        sizeClass = origSizeClass;
        expandedPool = true;
    } // trying to allocate a buffer until heap cant expand any more.

    // If we jumped to here, then we found a buffer that is big enough
    // for the request.
foundReassembledBuffer:

    // Get a pointer to the part of the buffer that holds user data.
    pHeader = (CBufferHeaderType *) pFreeBuffer;
    pBuffer = (char *) pFreeBuffer;
    pBuffer += g_AlignedSpaceForHeader;

    // Remove the buffer from the free list.
    RemoveFromFreeList(pFreeBuffer, freeBufSizeClass);

    // At this point, we are committed to allocating the buffer.

    // Find out how much extra space is in the buffer. If there
    // is enough to hold a second buffer, then split this buffer.
    upperBufferSize = pHeader->m_NumDataBytes - allocSize;
    upperBufferSize = upperBufferSize - g_AlignedSpaceForHeader;

    // If there is enough space, then break it off and make it
    // into its own buffer.
    if (upperBufferSize >= MIN_BUFFER_SIZE) {
        // allocSize is an aligned offset, so pUpperHeader and
        // pUpperBuffer are both aligned.
        pUpperBuffer = ((char *) pBuffer) + allocSize;
        pUpperHeader = (CBufferHeaderType *) pUpperBuffer;
        pUpperBuffer += g_AlignedSpaceForHeader;

        // Initialize the upper header.
        pUpperHeader->m_Flags = 0;
        pUpperHeader->m_NumDataBytes = upperBufferSize;
        pUpperHeader->m_MagicNum = HEADER_MAGIC_WORD;

#if DD_DEBUG
        pUpperHeader->m_pFileName = NULL;
        pUpperHeader->m_LineNum = 0;
#endif

        // Insert the upper header into the chain of all memory.
        pUpperHeader->m_pPrev = pHeader;
        pUpperHeader->m_pNext = pHeader->m_pNext;
        if (pHeader->m_pNext) {
            pHeader->m_pNext->m_pPrev = pUpperHeader;
        }
        pHeader->m_pNext = pUpperHeader;

        // The upper buffer was taken from the free list, so it
        // is already patterned. This is true ONLY because we have
        // not *yet* initialized the buffer with invalidInt8.

        AddToFreeList((CFreeBufferType *) pUpperHeader);
    } // breaking off the upper buffer.

foundCachedFreeBuffer:
    // Fill in the header.
    pHeader->m_Flags = BUFFER_ALLOCATED;
    pHeader->m_MagicNum = HEADER_MAGIC_WORD;
#if DD_DEBUG
    pHeader->m_Flags |= BUFFER_HAS_FOOTER;
    pHeader->m_pFileName = pFileName;
    pHeader->m_LineNum = lineNum;
#endif

    // Whether or not we split the buffer, the data size may
    // be smaller than what we allocated. Adjust the size of the
    // allocated buffer to reflect the data size. The nextPtr will
    // point to the next buffer in the heap, so that can be used
    // to figure the allocated size.
    pHeader->m_NumDataBytes = numDataBytes;

    // If the user wants us to, initialize the trailer. The trailer
    // always appears at the end of the allocated data, but at the end
    // of the valid user data. We only record the allocated size in each
    // buffer, so that is the only thing we have to find the trailer.
#if DD_DEBUG
    trailer.m_MagicNum = FOOTER_MAGIC_WORD;
    // Use the header numDataBytes. The buffer may be larger
    // than we requested, so make sure we always put the trailer
    // at the real end.
    pDestPtr = ((char *) pBuffer) + pHeader->m_NumDataBytes;
    pSrcPtr = (char *) &trailer;
    pEndSrcPtr = pSrcPtr + sizeof(CBufferFooterType);
    while (pSrcPtr < pEndSrcPtr) {
        *(pDestPtr++) = *(pSrcPtr++);
    }
#endif

    // Add to the statistics.
    m_TotalAllocBytes += numDataBytes;
    m_TotalAllocBuffers += 1;

#if DD_DEBUG
    memset(pBuffer, invalidInt8, numDataBytes);
#endif

#if WIN32
allocatedMemoryFromOS:
#endif
    m_Lock.BasicUnlock();
    return((void *) pBuffer);

abort:
    DEBUG_WARNING("CMemAlloc::Free fails.");

    m_Lock.BasicUnlock();
    return(NULL);
} // Alloc.






/////////////////////////////////////////////////////////////////////////////
//
// [Free]
//
// NOTE: This routine takes a reference to the pointer, so
// it can be safely NULL'ed when the buffer is deallocated.
/////////////////////////////////////////////////////////////////////////////
void
CMemAlloc::Free(const void *pBuffer) {
    ErrVal err = ENoErr;
    char *pRawHeaderPtr;
    CFreeBufferType *pFreeBuffer;
    CBufferHeaderType *pHeader;
    CBufferHeaderType *pNextHeader;
    CBufferHeaderType *pPrevHeader;
    int32 allocSize;
    int32 oldAllocSize;
    CBaseBlockHeaderType *pBasePtr;
    RunChecks();

    // delete NULL is a legal no-op.
    if (NULL == pBuffer) {
        return;
    }

    if ((g_ShutdownBuildingBlocks) || (!m_fInitialized)) {
        DEBUG_LOG("CMemAlloc::Free. Cannot free a buffer");
        return;
    }

#if DD_DEBUG
    CheckPtr(pBuffer);
#endif

    m_Lock.BasicLock();

    // Make sure the buffer is legal, and get a pointer to its overhead.
    if (((int64) pBuffer) & ILLEGAL_ADDR_MASK) {
        DEBUG_WARNING("CMemAlloc::Free Invalid pointer param.");
        gotoErr(EFail);
    }

    // Get the header for this buffer.
    pRawHeaderPtr = ((char *) pBuffer) - g_AlignedSpaceForHeader;
    pFreeBuffer = (CFreeBufferType *) pRawHeaderPtr;
    pHeader = (CBufferHeaderType *) pRawHeaderPtr;
    oldAllocSize = pHeader->m_NumDataBytes;


    // Check that the buffer is legal. The real check here is
    // to make sure that the user did not hand us a random pointer, but
    // instead passed the pointer to a real buffer.
    if (pHeader->m_MagicNum != HEADER_MAGIC_WORD) {
        if (g_ShutdownBuildingBlocks) {
            gotoErr(ENoErr);
        }

        DEBUG_WARNING("CMemAlloc::Free Invalid pointer param.");
        gotoErr(EFail);
    }

    if (!(pHeader->m_Flags & BUFFER_ALLOCATED)) {
        DEBUG_WARNING("CMemAlloc::Free Invalid pointer param.");
        gotoErr(EFail);
    }


    ////////////////////////////////////////////////////
    // FAST PATH
    //
    // Check if we want to cache this free buffer.
    // We use the pHeader->m_NumDataBytes as the cache size. The actual
    // size may be larger, to include space for the footer, rounded up size,
    // unused space between buffers and more. But, we can be sure that
    // a buffer that can serve N pHeader->m_NumDataBytes once can be
    // recycled again for the same size.
    if (pHeader->m_NumDataBytes <= MAX_CACHEABLE_SIZE) {
        int32 index;
        CCachedFreeBufferList *pCachedBufferList;

        index = pHeader->m_NumDataBytes;
        pCachedBufferList = &(m_FreeBufferCache[index]);
        ASSERT(pHeader->m_NumDataBytes == pCachedBufferList->m_FreeBufferSize);
        if (pCachedBufferList->m_NumFreeBuffers < pCachedBufferList->m_MaxNumFreeBuffers) {
            pFreeBuffer = (CFreeBufferType *) pHeader;

#if DD_DEBUG
            // This will pattern the buffer to the very end, INCLUDING any bytes that
            // were not included in the numDataBytes. This is important when we merge
            // this buffer with another patterned free buffer.
            //
            // Do this BEFORE we initialize the free buffer. To ensure
            // we cover the entire buffer, we write over the free buffer
            // overlap with the data payload. That's ok because then we later
            // initialize the free buffer header to be valid.
            memset(
                (void *) pBuffer,
                unallocatedInt8,
                pHeader->m_NumDataBytes);
            pHeader->m_Flags |= BUFFER_HAS_FREE_PATTERN;
#endif

            pFreeBuffer->m_pNextFree = pCachedBufferList->m_pFreeBuffers;
            pCachedBufferList->m_pFreeBuffers = pFreeBuffer;
            pCachedBufferList->m_NumFreeBuffers += 1;

            pHeader->m_Flags |= MEM_FREE_BUFFER_CACHED;
            pHeader->m_Flags &= ~BUFFER_ALLOCATED;

            goto cachedFreeBuffer;
        }
    } // FAST PATH


    pNextHeader = pHeader->m_pNext;
    pPrevHeader = pHeader->m_pPrev;

    // When a buffer is free, all of its contents are data bytes.
    // An allocated buffer might only use some of its space for data
    // if it included extra slop on the end.
    if ((pFreeBuffer->m_Header).m_pNext) {
        allocSize = ((char *) (pFreeBuffer->m_Header).m_pNext) - ((char *) pBuffer);
    } else
    {
        pBasePtr = GetBaseBlock(pBuffer);
        if (pBasePtr)
            allocSize = ((char *) (((char *) pBasePtr)
                                + sizeof(CBaseBlockHeaderType)
                                + pBasePtr->m_NumBytesAfterHeader))
                    - ((char *) pBuffer);
        else {
            allocSize = (pFreeBuffer->m_Header).m_NumDataBytes;
        }
    }
    (pFreeBuffer->m_Header).m_NumDataBytes = allocSize;

#if DD_DEBUG
    // This will pattern the buffer to the very end, INCLUDING any bytes that
    // were not included in the numDataBytes. This is important when we merge
    // this buffer with another patterned free buffer.
    memset((char *) pBuffer, unallocatedInt8, allocSize);
    pHeader->m_Flags |= BUFFER_HAS_FREE_PATTERN;
#endif

    // Try to combine the current buffer with the previous buffer.
    // BE CAREFUL: Cached free blocks are not allocated, but they
    // should not be recombined. The whole point of the free cache
    // is to save the overhead of reassembling/disassembling the buffers.
    if (pPrevHeader) {
        if (!(pPrevHeader->m_Flags & BUFFER_ALLOCATED)
            && !(pPrevHeader->m_Flags & MEM_FREE_BUFFER_CACHED)) {
            // The previous buffer is now part of the current buffer, so
            // remove the previous buffer from the free list. IMPORTANT.
            // Remove the entry before we change its size so we can find
            // it in the free list.
            RemoveFromFreeList((CFreeBufferType *) pPrevHeader, -1);

            // Both the header and data parts of the current
            // buffer are added to the data of the previous buffer.
            pPrevHeader->m_NumDataBytes = pPrevHeader->m_NumDataBytes
                       + g_AlignedSpaceForHeader + pHeader->m_NumDataBytes;

            // Clear the header flags so we don't think the header
            // is patterned. Even if both buffers we are combining
            // were patterned, there is a mismatch where they join.
            pPrevHeader->m_Flags = 0;

            // The current buffer is now part of the previous buffer, so
            // remove the current buffer from the list of memory segments.
            pPrevHeader->m_pNext = pHeader->m_pNext;
            if (pHeader->m_pNext) {
                (pHeader->m_pNext)->m_pPrev = pPrevHeader;
            }

#if DD_DEBUG
            // The header now is part of the body of a larger freed block.
            // The other free buffer may not have been patterned when it
            // was freed. If we are patterning, then make sure it is patterned.
            if (!(pPrevHeader->m_Flags & BUFFER_HAS_FREE_PATTERN)) {
                char *tpSrcPtr = (char *) pPrevHeader;
                tpSrcPtr += sizeof(CFreeBufferType);
                char *tpEndSrcPtr = (char *) pPrevHeader;
                tpEndSrcPtr += g_AlignedSpaceForHeader;
                tpEndSrcPtr += pPrevHeader->m_NumDataBytes;

                memset(tpSrcPtr, unallocatedInt8, tpEndSrcPtr - tpSrcPtr);
            }

            memset(((char *) pHeader), unallocatedInt8, sizeof(CFreeBufferType));
            pPrevHeader->m_Flags |= BUFFER_HAS_FREE_PATTERN;
#endif

            pHeader = pPrevHeader;
        }
    } // combining the current buffer with the previous buffer.

    // Try to combine the current buffer with the next buffer.
    // BE CAREFUL: Cached free blocks are not allocated, but they
    // should not be recombined. The whole point of the free cache
    // is to save the overhead of reassembling/disassembling the buffers.
    if (pNextHeader) {
        if (!(pNextHeader->m_Flags & BUFFER_ALLOCATED)
            && !(pNextHeader->m_Flags & MEM_FREE_BUFFER_CACHED)) {
            // The next buffer is now part of the current buffer, so remove
            // the next buffer from the free list. IMPORTANT. Remove it from
            // the free list before we change its size, so we can find it in
            // the free list.
            RemoveFromFreeList((CFreeBufferType *) pNextHeader, -1);

            // Both the header and data parts of the next
            //  buffer are added to the data of the current buffer.
            pHeader->m_NumDataBytes = pHeader->m_NumDataBytes
                + pNextHeader->m_NumDataBytes + g_AlignedSpaceForHeader;

            // The next buffer is now part of the current buffer, so
            // remove the next buffer from the list of memory segments.
            pHeader->m_pNext = pNextHeader->m_pNext;
            if (pNextHeader->m_pNext) {
                (pNextHeader->m_pNext)->m_pPrev = pHeader;
            }

#if DD_DEBUG
            // The header now is part of the body of a larger freed block.
            // The other free buffer may not have been patterned when it
            // was freed. If we are patterning, then make sure it is patterned.
            if (!(pNextHeader->m_Flags & BUFFER_HAS_FREE_PATTERN)) {
                char *tpSrcPtr = (char *) pNextHeader;
                tpSrcPtr += sizeof(CFreeBufferType);
                char *tpEndSrcPtr = (char *) pNextHeader;
                tpEndSrcPtr += g_AlignedSpaceForHeader;
                tpEndSrcPtr += pNextHeader->m_NumDataBytes;

                memset(
                    tpSrcPtr,
                    unallocatedInt8,
                    tpEndSrcPtr - tpSrcPtr);
            }

            memset(
                (char *) pNextHeader,
                unallocatedInt8,
                sizeof(CFreeBufferType));
#endif
        }
    }

    // Add the new header to the free list. This may be the result
    // of combining adjacent buffers, which is why we removed them
    // from the free list (so they don't appear twice).
    AddToFreeList((CFreeBufferType *) pHeader);

cachedFreeBuffer:
    // Update the statistics before we combine this buffer
    // with adjacent buffers that were previously freed.
    m_TotalAllocBytes = m_TotalAllocBytes - oldAllocSize;
    m_TotalAllocBuffers = m_TotalAllocBuffers - 1;

abort:
    // Null out the caller's pointer.
    pBuffer = NULL;
    m_Lock.BasicUnlock();
} // Free.





/////////////////////////////////////////////////////////////////////////////
//
// [Calloc]
//
/////////////////////////////////////////////////////////////////////////////
void *
CMemAlloc::Calloc(int32 numDataBytes, const char *pFileName, int lineNum) {
    ErrVal err = ENoErr;
    void *pBuffer = NULL;

    pBuffer = Alloc(numDataBytes, pFileName, lineNum);
    if (NULL == pBuffer) {
        gotoErr(EFail);
    }

    memset((char *) pBuffer, 0, numDataBytes);

abort:
    return(pBuffer);
} // Calloc.





/////////////////////////////////////////////////////////////////////////////
//
// [Realloc]
//
/////////////////////////////////////////////////////////////////////////////
void *
CMemAlloc::Realloc(void *pBuffer, int32 newSize) {
    ErrVal err = ENoErr;
    char *pRawHeaderPtr;
    CBufferHeaderType *pHeader;
    int32 CurrentBufferSize;
    int32 newAllocSize;
    CBaseBlockHeaderType *pBasePtr;
    char *endPtr;
    RunChecks();


    // Realloc(NULL is the same as malloc()
    if (NULL == pBuffer) {
        return(Alloc(newSize, NULL, 0));
    }

    // Grab the lock. Any cleanup code assumes that the lock is held.
    m_Lock.BasicLock();

#if DD_DEBUG
    CheckPtr(pBuffer);
#endif

    // Make sure the buffer is legal, and get a pointer to its overhead.
    if (((int64) pBuffer) & ILLEGAL_ADDR_MASK) {
        DEBUG_WARNING("CMemAlloc::Realloc Invalid pointer param.");
        gotoErr(EFail);
    }

    pRawHeaderPtr = ((char *) pBuffer) - g_AlignedSpaceForHeader;
    pHeader = (CBufferHeaderType *) pRawHeaderPtr;

    // Again, check that the buffer is legal. The real check here is
    // to make sure that the user did not hand us a random pointer, but
    // instead passed the pointer to a real buffer.
    if ((pHeader->m_MagicNum != HEADER_MAGIC_WORD)
          || (pHeader->m_NumDataBytes <= 0)
          || (!(pHeader->m_Flags & BUFFER_ALLOCATED))) {
        DEBUG_WARNING("CMemAlloc::Realloc Invalid pointer param.");
        gotoErr(EFail);
    }

    if (newSize <= 0) {
        DEBUG_WARNING("CMemAlloc::Realloc Invalid newSize param.");
        gotoErr(EFail);
    }

    // First get the real size of the memory request. This is the
    // size of the user data plus the size of the overhead.
    newAllocSize = newSize;
#if DD_DEBUG
    newAllocSize += sizeof(CBufferFooterType);
#endif

    // To save overhead, we don't deal with anything smaller than a
    // certain size.
    if (newAllocSize < MIN_BUFFER_SIZE) {
        newAllocSize = MIN_BUFFER_SIZE;
    }

    // The data is also allocated in multiples of 4 bytes. This
    // insures that allocated buffers are always 4-byte aligned,
    // so the header at the front of the buffer can be accessed
    // as a normal C struct. This also ensures that the trailer will
    // be on a 4 byte alignment.
    while (newAllocSize & ILLEGAL_ADDR_MASK) {
        newAllocSize += 1;
    }

    if (newAllocSize >= MAX_BUFFER_SIZE) {
        DEBUG_WARNING("CMemAlloc::Realloc Invalid newSize param.");
        gotoErr(EFail);
    }

    // Get the current size of the buffer. An allocated buffer
    // might only use some of its space for data if it included
    // extra slop on the end.
    if (pHeader->m_pNext) {
        CurrentBufferSize = ((char *) pHeader->m_pNext) - ((char *) pBuffer);
    } else
    {
        pBasePtr = GetBaseBlock(pBuffer);
        if (pBasePtr) {
            endPtr = ((char *) pBasePtr);
            endPtr += sizeof(CBaseBlockHeaderType);
            endPtr += pBasePtr->m_NumBytesAfterHeader;
            CurrentBufferSize = endPtr - ((char *) pBuffer);
        } else {
            CurrentBufferSize = pHeader->m_NumDataBytes;
        }
    }

    if (newAllocSize < CurrentBufferSize) {
        ShrinkBuffer(pHeader, pBuffer, newSize, newAllocSize);
    } else
    {
        pBuffer = GrowBuffer(pHeader, pBuffer, newSize, CurrentBufferSize, newAllocSize);
    }

    m_Lock.BasicUnlock();
    return(pBuffer);

abort:
    m_Lock.BasicUnlock();
    return(NULL);
} // Realloc.







/////////////////////////////////////////////////////////////////////////////
//
// [GetPtrSize]
//
// We need a wrapper for this method because CheckState and CheckPtr
// will use the global state so they need a consistent view.
/////////////////////////////////////////////////////////////////////////////
int32
CMemAlloc::GetPtrSize(const void *pBuffer) {
    ErrVal err = ENoErr;
    char *pRawHeaderPtr;
    CBufferHeaderType *pHeader;
    RunChecks();

    // Make sure the buffer is legal, and get a pointer to its overhead.
    if ((NULL == pBuffer)
            || (((int64) pBuffer) & ILLEGAL_ADDR_MASK)) {
        gotoErr(EFail);
    }

#if DD_DEBUG
    CheckPtr(pBuffer);
#endif

    pRawHeaderPtr = ((char *) pBuffer)  - g_AlignedSpaceForHeader;
    pHeader = (CBufferHeaderType *) pRawHeaderPtr;

    // Again, check that the buffer is legal. The real check here is
    // to make sure that the user did not hand us a random pointer, but
    // instead passed the pointer to a real buffer.
    if ((pHeader->m_MagicNum != HEADER_MAGIC_WORD)
          || (pHeader->m_NumDataBytes <= 0)
          || (!(pHeader->m_Flags & BUFFER_ALLOCATED))) {
        gotoErr(EFail);
    }

    return(pHeader->m_NumDataBytes);

abort:
    return(0);
} // GetPtrSize.





/////////////////////////////////////////////////////////////////////////////
//
// [DontCountMemoryAsLeaked]
//
/////////////////////////////////////////////////////////////////////////////
void
CMemAlloc::DontCountMemoryAsLeaked(const void *pBuffer) {
    ErrVal err = ENoErr;
    char *pRawHeaderPtr;
    CBufferHeaderType *pHeader;

    // Make sure the buffer is legal, and get a pointer to its overhead.
    if ((NULL == pBuffer)
        || (((int64) pBuffer) & ILLEGAL_ADDR_MASK)) {
        gotoErr(EFail);
    }

    // Run any standard debugger checks.
    // We only check the pointer because we do not have a global lock.
#if DD_DEBUG
    CheckPtr(pBuffer);
#endif

    pRawHeaderPtr = ((char *) pBuffer) - g_AlignedSpaceForHeader;
    pHeader = (CBufferHeaderType *) pRawHeaderPtr;

    // Again, check that the buffer is legal. The real check here is
    // to make sure that the user did not hand us a random pointer, but
    // instead passed the pointer to a real buffer.
    if ((pHeader->m_MagicNum != HEADER_MAGIC_WORD)
          || (pHeader->m_NumDataBytes <= 0)
          || (!(pHeader->m_Flags & BUFFER_ALLOCATED))) {
        gotoErr(EFail);
    }

    pHeader->m_Flags |= BUFFER_PRINTED;

abort:
    return;
} // DontCountMemoryAsLeaked.







/////////////////////////////////////////////////////////////////////////////
//
// [CheckPtr]
//
// This does not check checksums, because we do not know
// if the client is currently modifying a buffer (and hence
// the checksum would be invalid). Checksums must be explicitly
// called by the client.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CMemAlloc::CheckPtr(const void *pBuffer) {
    ErrVal err = ENoErr;
    char *pRawHeaderPtr;
    CBufferHeaderType *pHeader;
    CBaseBlockHeaderType *pBasePtr;
#if DD_DEBUG
    CBufferFooterType trailer;
    char *pSrcPtr;
    char *pEndSrcPtr;
    char *pDestPtr;
#endif
    char *startValidRegion;
    char *stopValidRegion;


    // Grab the lock. Any cleanup code assumes that the lock is held.
    m_Lock.BasicLock();


    // Make sure the ptr is legal, and get a pointer to its overhead.
    if ((NULL == pBuffer)
        || (((int64) pBuffer) & ILLEGAL_ADDR_MASK)) {
        gotoErr(EInvalidArg);
    }

    // Check if a pointer is in one of our allocation regions.
    // Do this BEFORE we try to read the head. This lets us know
    // that the pointer is not totally invalid.
    pBasePtr = GetBaseBlock(pBuffer);

    // If the pointer is not in a region, then it is illegal.
    if (NULL == pBasePtr) {
        gotoErr(EInvalidArg);
    }

    pRawHeaderPtr = ((char *) pBuffer) - g_AlignedSpaceForHeader;
    pHeader = (CBufferHeaderType *) pRawHeaderPtr;

    // Again, check that the buffer is legal. The real check here is
    // to make sure that the user did not hand us a random pointer, but
    // instead passed the pointer to a real buffer.
    if ((pHeader->m_MagicNum != HEADER_MAGIC_WORD)
          || (!(pHeader->m_Flags & BUFFER_ALLOCATED))) {
        gotoErr(EFail);
    }

    if (pHeader->m_NumDataBytes < 0) {
        gotoErr(EFail);
    }

    // Make sure the buffer fits entirely in this block. Even if
    // 2 allocation blocks are adjacent, buffers from the 2 never
    // recombine, so a buffer can never straddle them.
    startValidRegion = (char *) pBasePtr;
    startValidRegion += sizeof(CBaseBlockHeaderType);
    stopValidRegion = startValidRegion + pBasePtr->m_NumBytesAfterHeader;

    if ((((char *) pBuffer) + pHeader->m_NumDataBytes) > stopValidRegion) {
        gotoErr(EFail);
    }

    // Check the pointer chains.
    if (pHeader->m_pNext) {
        if (((int64) (pHeader->m_pNext)) & ILLEGAL_ADDR_MASK) {
            gotoErr(EFail);
        }
        if ((pHeader->m_pNext)->m_pPrev != pHeader) {
            gotoErr(EFail);
        }
    }

    if (pHeader->m_pPrev) {
        if (((int64) (pHeader->m_pPrev)) & ILLEGAL_ADDR_MASK) {
            gotoErr(EFail);
        }
        if ((pHeader->m_pPrev)->m_pNext != pHeader) {
            gotoErr(EFail);
        }
    }

    // Check the trailer.
#if DD_DEBUG
    if (pHeader->m_Flags & BUFFER_HAS_FOOTER) {
        pSrcPtr = ((char *) pBuffer) + pHeader->m_NumDataBytes;
        pEndSrcPtr = pSrcPtr + sizeof(CBufferFooterType);
        pDestPtr = (char *) &trailer;

        while (pSrcPtr < pEndSrcPtr) {
            *(pDestPtr++) = *(pSrcPtr++);
        }

        if (trailer.m_MagicNum != FOOTER_MAGIC_WORD) {
            gotoErr(EFail);
        }
    }
#endif

abort:
    m_Lock.BasicUnlock();

    returnErr(err);
} // CheckPtr.






/////////////////////////////////////////////////////////////////////////////
//
// [CheckState]
//
// This does not check checksums, because we do not know
// if the client is currently modifying a buffer (and hence
// the checksum would be invalid). Checksums must be explicitly
// called by the client.
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CMemAlloc::CheckState() {
    ErrVal err = ENoErr;
    CBaseBlockHeaderType *pBasePtr;
    char *startValidRegion;
    char *stopValidRegion;
    char *pRawHeaderPtr;
    CBufferHeaderType *pHeader;
#if DD_DEBUG
    CBufferFooterType trailer;
    char *pSrcPtr;
    char *pEndSrcPtr;
    char *pDestPtr;
#endif
    char *pBuffer;
    CFreeBufferType *pFreeBuffer;
    int32 index;
    CCachedFreeBufferList *pCachedBufferList;
    int32 sizeClass;
    char *pEndBuffer;


    m_Lock.BasicLock();


    pBasePtr = m_BaseBlocks;
    while (pBasePtr) {
        startValidRegion = (char *) pBasePtr;
        startValidRegion += sizeof(CBaseBlockHeaderType);
        stopValidRegion = startValidRegion + pBasePtr->m_NumBytesAfterHeader;

        // This inner loop looks through every buffer in the current
        // base block.
        pRawHeaderPtr = (char *) pBasePtr;
        pRawHeaderPtr += sizeof(CBaseBlockHeaderType);
        pHeader = (CBufferHeaderType *) pRawHeaderPtr;
        while (pHeader) {
            pBuffer = (char *) pHeader + g_AlignedSpaceForHeader;
            if (((int64) pBuffer) & ILLEGAL_ADDR_MASK) {
                gotoErr(EFail);
            }

            // Again, check that the buffer is legal. The real check here is
            // to make sure that the user did not hand us a random pointer, but
            // instead passed the pointer to a real buffer.
            if (pHeader->m_MagicNum != HEADER_MAGIC_WORD) {
                gotoErr(EFail);
            }
            if (pHeader->m_NumDataBytes < 0) {
                gotoErr(EFail);
            }


            pEndBuffer = pBuffer + pHeader->m_NumDataBytes;
#if DD_DEBUG
            if (pHeader->m_Flags & BUFFER_HAS_FOOTER) {
                if (!(pHeader->m_Flags & BUFFER_ALLOCATED)
                    && !(pHeader->m_Flags & MEM_FREE_BUFFER_CACHED)) {
                    gotoErr(EFail);
                }

                pEndBuffer += sizeof(CBufferFooterType);
            }
#endif

            // Make sure the buffer fits entirely in this block. Even if
            // 2 allocation blocks are adjacent, buffers from the 2 never
            // recombine, so a buffer can never straddle them. NOTE: This
            // must be >, not >=, because the last buffer ptr goes to the
            // the block.
            if (pEndBuffer > stopValidRegion) {
                gotoErr(EFail);
            }
            if ((pHeader->m_pNext)
                && (pEndBuffer > ((char *) pHeader->m_pNext))) {
                gotoErr(EFail);
            }

            // Check if a pointer is in one of our allocation regions.
            // Do this BEFORE we try to read the head. This lets us know
            // that the pointer is not totally invalid.
            if (pHeader->m_pNext) {
                if (((int64) (pHeader->m_pNext)) & ILLEGAL_ADDR_MASK) {
                    gotoErr(EFail);
                }
                if ((pHeader->m_pNext)->m_pPrev != pHeader) {
                    gotoErr(EFail);
                }
            }

            if (pHeader->m_pPrev) {
                if (((int64) (pHeader->m_pPrev)) & ILLEGAL_ADDR_MASK) {
                    gotoErr(EFail);
                }
                if ((pHeader->m_pPrev)->m_pNext != pHeader) {
                    gotoErr(EFail);
                }
            }

            // We do these additional checks for allocated buffers.
            if (pHeader->m_Flags & BUFFER_ALLOCATED) {
#if DD_DEBUG
                // Check the trailer.
                if (pHeader->m_Flags & BUFFER_HAS_FOOTER) {
                    pSrcPtr = pBuffer + pHeader->m_NumDataBytes;
                    pEndSrcPtr = pSrcPtr + sizeof(CBufferFooterType);
                    pDestPtr = (char *) &trailer;

                    while (pSrcPtr < pEndSrcPtr)
                    {
                        *(pDestPtr++) = *(pSrcPtr++);
                    }

                    if (trailer.m_MagicNum != FOOTER_MAGIC_WORD)
                    {
                        gotoErr(EFail);
                    }
                }
#endif
                // We do NOT check the checksums. Clients may not tell
                // us when a checksum becomes invalid.
            } // checking an allocated buffer.

            // We do these additional checks for freed buffers.
            // If they are on the free buffer cache, then they will
            // also have the BUFFER_ALLOCATED flag set, since they
            // are not reassembled.
#if DD_DEBUG
            if (pHeader->m_Flags & BUFFER_HAS_FREE_PATTERN) {
                pSrcPtr = (char *) pHeader;
                pSrcPtr += sizeof(CFreeBufferType);
                pEndSrcPtr = (char *) pHeader;
                pEndSrcPtr += g_AlignedSpaceForHeader;
                pEndSrcPtr += pHeader->m_NumDataBytes;

                // Do this check. Small buffers may be entirely overwritten
                // with the free header. In fact, the free header may extend into
                // extra overhead we added to the allocSize, but is not counted
                // in pHeader->m_NumDataBytes.
                if (pEndSrcPtr >= pSrcPtr) {
                    if (!(IsMemSet(pSrcPtr, pEndSrcPtr - pSrcPtr, unallocatedInt8))) {
                        gotoErr(EFail);
                    }
                }
            } // checking a patterned free buffer.
#endif // DD_DEBUG

            pHeader = pHeader->m_pNext;
        } // checking every buffer in the current allocation block.

        pBasePtr = pBasePtr->m_pNext;
    } // checking every basic block.


    // Check every buffer in the free list. This is a very
    // simple check. In particular, we do NOT check that
    // each free buffer corresponds to a header we found
    // in the previous checks.
    for (sizeClass = LOG_MIN_BUFFER_SIZE;
           sizeClass <= LOG_MAX_BUFFER_SIZE;
           sizeClass++) {
        // Find the base block that contains this free pointer.
        pFreeBuffer = m_FreeReassembledBufferList[sizeClass];
        while (pFreeBuffer) {
            // Find the base block that contains this free pointer.
            pBasePtr = GetBaseBlock((char *) pFreeBuffer);
            if (NULL == pBasePtr) {
                gotoErr(EFail);
            }

            if (pFreeBuffer->m_pNextFree) {
                if (GetBaseBlock((char *) (pFreeBuffer->m_pNextFree)) == NULL) {
                    gotoErr(EFail);
                }

                if ((pFreeBuffer->m_pNextFree)->m_pPrevFree != pFreeBuffer) {
                    gotoErr(EFail);
                }
            }

            if (pFreeBuffer->m_pPrevFree) {
                if (GetBaseBlock((char *) (pFreeBuffer->m_pPrevFree)) == NULL) {
                    gotoErr(EFail);
                }

                if ((pFreeBuffer->m_pPrevFree)->m_pNextFree != pFreeBuffer) {
                    gotoErr(EFail);
                }
            }

            pFreeBuffer = pFreeBuffer->m_pNextFree;
        } // checking all free blocks in this storage class.
    } // checking all free blocks.



    // Check the free block cache.
    for (index = 0; index < NUM_CACHED_SIZES; index++) {
        pCachedBufferList = &(m_FreeBufferCache[index]);

        pFreeBuffer = pCachedBufferList->m_pFreeBuffers;
        while (NULL != pFreeBuffer) {
            if (HEADER_MAGIC_WORD != pFreeBuffer->m_Header.m_MagicNum) {
                gotoErr(EFail);
            }

            pFreeBuffer = pFreeBuffer->m_pNextFree;
        }
    }


    m_Lock.BasicUnlock();
    returnErr(ENoErr);

abort:
    m_Lock.BasicUnlock();
    returnErr(EFail);
} // CheckState.





/////////////////////////////////////////////////////////////////////////////
//
// [GetNumAllocations]
//
// This checks the state of the object itself.
/////////////////////////////////////////////////////////////////////////////
int32
CMemAlloc::GetNumAllocations(int32 flags) {
    char *pRawHeaderPtr;
    int32 count = 0;
    CBaseBlockHeaderType *pBasePtr;
    CBufferHeaderType *pHeader;

    // Grab the lock. Any cleanup code assumes that the lock is held.
    m_Lock.BasicLock();

    pBasePtr = m_BaseBlocks;
    while (pBasePtr) {
        // This inner loop looks through every buffer in the current
        // base block.
        pRawHeaderPtr = (char *) pBasePtr;
        pRawHeaderPtr += sizeof(CBaseBlockHeaderType);
        pHeader = (CBufferHeaderType *) pRawHeaderPtr;
        while (pHeader) {
            if ((pHeader->m_Flags & BUFFER_ALLOCATED)
                && !(pHeader->m_Flags & MEM_FREE_BUFFER_CACHED)) {
                if ((flags & PRINT_ONLY_CHANGES)
                        && !(pHeader->m_Flags & BUFFER_PRINTED)) {
                    count += 1;
                }
            }

            pHeader = pHeader->m_pNext;
        } // checking every buffer in the current allocation block.

        pBasePtr = pBasePtr->m_pNext;
    } // checking every basic block.

    m_Lock.BasicUnlock();

    return(count);
} // GetNumAllocations.





/////////////////////////////////////////////////////////////////////////////
//
// [DontCountCurrentAllocations]
//
/////////////////////////////////////////////////////////////////////////////
void
CMemAlloc::DontCountCurrentAllocations() {
    char *pRawHeaderPtr;
    CBaseBlockHeaderType *pBasePtr;
    CBufferHeaderType *pHeader;

    // Grab the lock. Any cleanup code assumes that the lock is held.
    m_Lock.BasicLock();

    pBasePtr = m_BaseBlocks;
    while (pBasePtr) {
        // This inner loop looks through every buffer in the current
        // base block.
        pRawHeaderPtr = (char *) pBasePtr;
        pRawHeaderPtr += sizeof(CBaseBlockHeaderType);
        pHeader = (CBufferHeaderType *) pRawHeaderPtr;
        while (pHeader) {
            if (pHeader->m_Flags & BUFFER_ALLOCATED) {
                pHeader->m_Flags |= BUFFER_PRINTED;
            }

            pHeader = pHeader->m_pNext;
        } // checking every buffer in the current allocation block.

        pBasePtr = pBasePtr->m_pNext;
    } // checking every basic block.

    m_Lock.BasicUnlock();
} // DontCountCurrentAllocations.






/////////////////////////////////////////////////////////////////////////////
//
// [PrintAllocations]
//
/////////////////////////////////////////////////////////////////////////////
void
CMemAlloc::PrintAllocations(int32 flags, char *pStartBuffer, int32 maxSize, int32 *pResultSize) {
    CMemoryAllocationClient *pItem;
    int index;
    char *pDestPtr;
    char *pEndDestPtr;
    char *pLastSafeDestPtr;
    int32 totalMemInUse = 0;
    int32 totalBuffersAllocated = 0;

    if ((NULL == pStartBuffer) || (maxSize < 0)) {
        return;
    }
    pDestPtr = pStartBuffer;
    pEndDestPtr = pStartBuffer + maxSize;
    pLastSafeDestPtr = pStartBuffer + (maxSize - 500);

#if DD_DEBUG
    PrepareEngineToRecordAllocations(false);

    // Get all modules to report their memory usage.
    // This is all memory heaps and all cell pools and all
    // object pools.
    g_NumAllocationInfoItems = 0;
    g_NumAllocationOverflowInfoItems = 0;
    g_MainMem.CollectMemoryStats(flags);

    // Sort the memory statistics.
    QSortMemUsageArray(0, g_NumAllocationInfoItems - 1);

    // Leave off any header so we can import this to Excel.
    pDestPtr += snprintf(pDestPtr, (pEndDestPtr - pDestPtr), "\n");

    // Add up the totals
    totalMemInUse = 0;
    totalBuffersAllocated = 0;
    for (index = 0; index < g_NumAllocationInfoItems; index++) {
        pItem = &(g_AllocationDumpList[index]);

        totalMemInUse += pItem->numItemsInUse;
        totalBuffersAllocated += pItem->totalSizeInUse;
    }

    pDestPtr += snprintf(pDestPtr, (pEndDestPtr - pDestPtr), "==================================================");
    pDestPtr += snprintf(pDestPtr, (pEndDestPtr - pDestPtr), "\nPageSize: %d", m_PageSize);
    pDestPtr += snprintf(pDestPtr, (pEndDestPtr - pDestPtr), "\nTotal Memory Size: %d", m_TotalMem);
    pDestPtr += snprintf(pDestPtr, (pEndDestPtr - pDestPtr), "\nAllocated Bytes: %d", totalMemInUse);
    pDestPtr += snprintf(pDestPtr, (pEndDestPtr - pDestPtr), "\nAllocated Buffers: %d", totalBuffersAllocated);
    pDestPtr += snprintf(pDestPtr, (pEndDestPtr - pDestPtr), "\n");

    // Print each memory client usage in order.
    for (index = 0; index < g_NumAllocationInfoItems; index++) {
        if (pDestPtr < pLastSafeDestPtr) {
            pItem = &(g_AllocationDumpList[index]);
            pDestPtr += snprintf(
                        pDestPtr,
                        (pEndDestPtr - pDestPtr),
                        "%d bytes (%d allocations, %d bytes each)  %s, %d\n",
                        pItem->totalSizeInUse,
                        pItem->numItemsInUse,
                        pItem->sizeOfEachItem,
                        pItem->pFileName,
                        pItem->lineNum);    
        } // if (pDestPtr < pLastSafeDestPtr)
    } // for (index = 0; index < g_NumAllocationInfoItems; index++)
#endif

    if (pResultSize) {
        *pResultSize = (pDestPtr - pStartBuffer);
    }
} // PrintAllocations






/////////////////////////////////////////////////////////////////////////////
//
// [CollectMemoryStats]
//
/////////////////////////////////////////////////////////////////////////////
void
CMemAlloc::CollectMemoryStats(int32 flags) {
    char *pRawHeaderPtr;
    CBaseBlockHeaderType *pBasePtr;
    CBufferHeaderType *pHeader;

    // Grab the lock. Any cleanup code assumes that the lock is held.
    m_Lock.BasicLock();

    pBasePtr = m_BaseBlocks;
    while (pBasePtr) {
        // This inner loop looks through every buffer in the current
        // base block.
        pRawHeaderPtr = (char *) pBasePtr;
        pRawHeaderPtr += sizeof(CBaseBlockHeaderType);
        pHeader = (CBufferHeaderType *) pRawHeaderPtr;
        while (pHeader) {
            // Decide whether we will print this buffer.
            if ((pHeader->m_Flags & BUFFER_ALLOCATED)
                    && !(pHeader->m_Flags & MEM_FREE_BUFFER_CACHED)) {
                if (!(flags & PRINT_ONLY_CHANGES) 
                        || ((flags & PRINT_ONLY_CHANGES) && !(pHeader->m_Flags & BUFFER_PRINTED))) {
                    pHeader->m_Flags |= BUFFER_PRINTED;        
                    ReportOneInstanceOfMemoryUse(
                                pHeader->m_pFileName,
                                pHeader->m_LineNum,
                                pHeader->m_NumDataBytes);
                }
            }

            pHeader = pHeader->m_pNext;
        } // checking every buffer in the current allocation block.

        pBasePtr = pBasePtr->m_pNext;
    } // checking every basic block.

    m_Lock.BasicUnlock();
} // CollectMemoryStats





/////////////////////////////////////////////////////////////////////////////
//
// [ReportOneInstanceOfMemoryUse]
//
/////////////////////////////////////////////////////////////////////////////
void
ReportOneInstanceOfMemoryUse(
                        const char *pFileName,
                        int32 lineNum,
                        int32 allocationSize) {
#if DD_DEBUG
    int32 index = 0;
    CMemoryAllocationClient *pClient;

    if (NULL == pFileName) {
        return;
    }

    // Check if the source already exists.
    for (index = 0; index < g_NumAllocationInfoItems; index++) {
        pClient = &(g_AllocationDumpList[index]);
        if ((lineNum == pClient->lineNum)
            && (0 == strcasecmpex(pFileName, pClient->pFileName))) {
            break;
        }
    }

    if (index >= g_NumAllocationInfoItems) {
        if (g_NumAllocationInfoItems >= MAX_ALLOCATION_REPORTS) {
            g_NumAllocationOverflowInfoItems += 1;
            return;
        }

        pClient = &(g_AllocationDumpList[g_NumAllocationInfoItems]);
        g_NumAllocationInfoItems += 1;

        pClient->pFileName = pFileName;
        pClient->lineNum = lineNum;
        pClient->sizeOfEachItem = allocationSize;
        pClient->numItemsInUse = 1;
        pClient->totalSizeInUse = allocationSize;
    } else {
        pClient = &(g_AllocationDumpList[index]);

        pClient->sizeOfEachItem = allocationSize;
        pClient->numItemsInUse += 1;
        pClient->totalSizeInUse += allocationSize;
    }

#endif
} // ReportOneInstanceOfMemoryUse.






/////////////////////////////////////////////////////////////////////////////
//
// [AddStorageImpl]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CMemAlloc::AddStorageImpl(char *pBuffer, int32 numBytes) {
    ErrVal err = ENoErr;
    CBufferHeaderType *pHeader;
    CBaseBlockHeaderType *pNewBaseBlock;

    //DEBUG_LOG("Grow Memory Pool. buffer %d, size %d", buffer, numBytes);

    // Run any standard debugger checks.
    RunChecks();

    if ((NULL == pBuffer) || (numBytes <= 0)) {
        gotoErr(EFail);
    }

    while (((int64) pBuffer) & ILLEGAL_ADDR_MASK) {
        pBuffer += 1;
        numBytes = numBytes - 1;
    }

    // Add a new baseBlock to the chain. This records all the allocation
    // blocks, so we can find all meory that is part of this pool.
    pNewBaseBlock = (CBaseBlockHeaderType *) pBuffer;
    pNewBaseBlock->m_NumBytesAfterHeader = numBytes - sizeof(CBaseBlockHeaderType);
    pNewBaseBlock->m_pNext = m_BaseBlocks;
    m_BaseBlocks = pNewBaseBlock;

    // We start allocating data right after the base block pHeader.
    pBuffer += sizeof(CBaseBlockHeaderType);
    numBytes = pNewBaseBlock->m_NumBytesAfterHeader;

    // Initially, the entire block is one big free buffer.
    // Initialize the header of the first buffer in this block.
    pHeader = (CBufferHeaderType *) pBuffer;
    pBuffer += g_AlignedSpaceForHeader;
    pHeader->m_MagicNum = HEADER_MAGIC_WORD;
    pHeader->m_NumDataBytes = numBytes - g_AlignedSpaceForHeader;
    pHeader->m_Flags = 0;

#if DD_DEBUG
    pHeader->m_pFileName = NULL;
    pHeader->m_LineNum = 0;
#endif

    // Insert the upper header into the chain of all memory. This new
    // piece of memory may not be contigous with other storage in the pool
    // (several pools may be calling sbrk, so chunks of VM may not be
    // contigous. Of course, separate chunks of ram added at different times
    // may also not be contigous. As a result, the pool is made up of islands
    // of memory, and we cannot consolidate data in different islands.
    pHeader->m_pPrev = NULL;
    pHeader->m_pNext = NULL;

#if DD_DEBUG
    // The header now is part of the body of a larger freed block.
    memset(pBuffer, unallocatedInt8, pHeader->m_NumDataBytes);
    pHeader->m_Flags |= BUFFER_HAS_FREE_PATTERN;
#endif

    // Put the buffer onto the free list.
    AddToFreeList((CFreeBufferType *) pHeader);

    m_TotalMem += numBytes;

abort:
    returnErr(err);
} // AddStorageImpl.







/////////////////////////////////////////////////////////////////////////////
//
// [AllocPages]
//
/////////////////////////////////////////////////////////////////////////////
void *
CMemAlloc::AllocPages(int32 numPages, const char *pFileName, int lineNum) {
    ErrVal err = ENoErr;
    int32 numDataBytes;
    CBufferHeaderType *pHeader;
    int32 upperBufferSize;
    int32 lowerBufferSize;
    CBufferHeaderType *pUpperHeader;
    CBufferHeaderType saveHeader;
    char *pUpperBuffer;
    void *pBuffer;
    char *pRawHeaderPtr;
    char *endBufPtr;
    CBaseBlockHeaderType * pBasePtr;
    char *pageBasePtr;
#if DD_DEBUG
    CBufferFooterType trailer;
    char *pSrcPtr;
    char *pEndSrcPtr;
    char *pDestPtr;
#endif
    int32 allocSize;
    char *endAllocDataBytes;

    // Run any standard debugger checks.
    RunChecks();

    m_Lock.BasicLock();

    if (numPages <= 0) {
        gotoErr(EFail);
    }

    // Add some buffering. This means no matter how the
    // allocated buffer is aligned, we should be able to trim
    // it to a page aligned buffer of the right size.
    numDataBytes = m_PageSize + (numPages * m_PageSize) + m_PageSize;

    pBuffer = Alloc(numDataBytes, pFileName, lineNum);
    if (NULL == pBuffer) {
        gotoErr(EFail);
    }

    endAllocDataBytes = ((char *) pBuffer) + numDataBytes;
    pRawHeaderPtr = ((char *) pBuffer) - g_AlignedSpaceForHeader;
    pHeader = (CBufferHeaderType *) pRawHeaderPtr;

    // Find out how big the buffer really is.
    if (pHeader->m_pNext) {
        allocSize = ((char *) (pHeader->m_pNext)) - ((char *) pBuffer);
    } else
    {
        pBasePtr = GetBaseBlock(pBuffer);
        if (pBasePtr) {
            allocSize = ((char *) (((char *) pBasePtr)
                                 + sizeof(CBaseBlockHeaderType)
                                 + pBasePtr->m_NumBytesAfterHeader))
                        - ((char *) pBuffer);
        } else {
            allocSize = pHeader->m_NumDataBytes;
        }
    }
    endBufPtr = ((char *) pBuffer) + allocSize;

    // pageBasePtr is >= buffer, so there is guaranteed to
    // be enough room for the header in the new page aligned
    // buffer. At worst, it uses some or all of the space used
    // for the original buffer header. There may not, however,
    // be room for a second buffer to be made from the part
    // we chop off of the beginning.
    pageBasePtr = (char *) (((int64) pBuffer) & m_PageAddressMask);
    if (NULL == pageBasePtr) {
        gotoErr(EFail);
    }
    pageBasePtr = pageBasePtr + m_PageSize;


    // Subtracting an aligned size from a page boundary yields an
    // aligned address.
    pRawHeaderPtr = pageBasePtr - g_AlignedSpaceForHeader;
    pUpperHeader = (CBufferHeaderType *) pRawHeaderPtr;

    // Make sure that the lower buffer is big enough that we
    // can trim it off and make a new buffer.
    lowerBufferSize = pRawHeaderPtr - ((char *) pBuffer);
    if (lowerBufferSize < MIN_BUFFER_SIZE) {
        pageBasePtr = pageBasePtr + m_PageSize;

        pRawHeaderPtr = pageBasePtr - g_AlignedSpaceForHeader;
        pUpperHeader = (CBufferHeaderType *) pRawHeaderPtr;

        lowerBufferSize = pRawHeaderPtr - ((char *) pBuffer);
    }

    // Replace the orgininal header with the new header in the list
    // of buffers. Do this with an intermediate variable since the
    // two regions may overlap.
    saveHeader = *pHeader;
    *pUpperHeader = saveHeader;
    if (pUpperHeader->m_pNext) {
        (pUpperHeader->m_pNext)->m_pPrev = pUpperHeader;
    }
    if (pUpperHeader->m_pPrev) {
        (pUpperHeader->m_pPrev)->m_pNext = pUpperHeader;
    }

    // Adjust the size of the upper header
    pUpperHeader->m_NumDataBytes = numPages * m_PageSize;

#if DD_DEBUG
    // Add a footer to the upper header.
    trailer.m_MagicNum = FOOTER_MAGIC_WORD;
    // Use the header numDataBytes. The buffer may be larger
    // than we requested, so make sure we always put the trailer
    // at the real end.
    pDestPtr = pageBasePtr + pUpperHeader->m_NumDataBytes;
    pSrcPtr = (char *) &trailer;
    pEndSrcPtr = pSrcPtr + sizeof(CBufferFooterType);
    while (pSrcPtr < pEndSrcPtr) {
        *(pDestPtr++) = *(pSrcPtr++);
    }
#endif

    // Now, try to free the old buffer back to the list of free
    // buffers, if possible. Otherwise, we drop it and this is a space leak.
    lowerBufferSize = pRawHeaderPtr - ((char *) pBuffer);
    if (lowerBufferSize >= MIN_BUFFER_SIZE) {
        pHeader->m_NumDataBytes = lowerBufferSize;
        pHeader->m_Flags = 0;

#if DD_DEBUG
        pHeader->m_pFileName = NULL;
        pHeader->m_LineNum = 0;

        memset(
            (((char *) pHeader) + g_AlignedSpaceForHeader),
            unallocatedInt8,
            pHeader->m_NumDataBytes);
        pHeader->m_Flags |= BUFFER_HAS_FREE_PATTERN;
#endif

        // Insert the lower header into the chain of all memory.
        // It keeps its prev pointer, so just change the next pointer.
        if (pUpperHeader->m_pPrev) {
            (pUpperHeader->m_pPrev)->m_pNext = pHeader;
        }
        pUpperHeader->m_pPrev = pHeader;
        pHeader->m_pNext = pUpperHeader;

        // Also do any striping of free buffers here.
        AddToFreeList((CFreeBufferType *) pHeader);
    } // breaking off the upper buffer.
    else
    {
        DEBUG_WARNING("SPACE LEAK from allocPages");
        // SPACE LEAK. We are throwing away the lower fragment.
        // This is really bad if the lower fragment was the start of
        // an allocation buffer because now CheckState cannot walk
        // through all buffers in a baseBlock.
    }
    m_TotalAllocBytes = m_TotalAllocBytes - lowerBufferSize;

    // We have freed the original base header, so ignore it.
    // All calculations are now relative to the page aligned
    // header.
    pHeader = pUpperHeader;

    // Find the buffer that starts after the page region.
    pUpperBuffer = pageBasePtr + (numPages * m_PageSize);
#if DD_DEBUG
    pUpperBuffer += sizeof(CBufferFooterType);
#endif
    // Make sure the upper buffer is aligned. The trailer is not
    // 4 bytes, so it is 2 bytes off the end of the page, which
    // is mis-aligned.
    while (((int64) pUpperBuffer) & ILLEGAL_ADDR_MASK) {
        pUpperBuffer += 1;
    }
    pUpperHeader = (CBufferHeaderType *) pUpperBuffer;
    pUpperBuffer += g_AlignedSpaceForHeader;


    // Find out how much extra space is in the buffer. If there
    // is enough to hold a second buffer, then split this buffer.
    upperBufferSize = endBufPtr - pUpperBuffer;
    if (upperBufferSize >= MIN_BUFFER_SIZE) {
        // Initialize the upper header.
        pUpperHeader->m_Flags = 0;
        pUpperHeader->m_NumDataBytes = upperBufferSize;
        pUpperHeader->m_MagicNum = HEADER_MAGIC_WORD;

#if DD_DEBUG
        pUpperHeader->m_pFileName = NULL;
        pUpperHeader->m_LineNum = 0;

        memset(
            (((char *) pUpperHeader) + g_AlignedSpaceForHeader),
            unallocatedInt8,
            pUpperHeader->m_NumDataBytes);
        pUpperHeader->m_Flags |= BUFFER_HAS_FREE_PATTERN;
#endif

        // Insert the upper header into the chain of all memory.
        pUpperHeader->m_pPrev = pHeader;
        pUpperHeader->m_pNext = pHeader->m_pNext;
        if (pHeader->m_pNext) {
            (pHeader->m_pNext)->m_pPrev = pUpperHeader;
        }
        pHeader->m_pNext = pUpperHeader;

        // Also do any striping of free buffers here.
        AddToFreeList((CFreeBufferType *) pUpperHeader);
    } // breaking off the upper buffer.
    m_TotalAllocBytes = m_TotalAllocBytes - (endAllocDataBytes - pageBasePtr);

    m_Lock.BasicUnlock();
    return(pageBasePtr);

abort:
    m_Lock.BasicUnlock();
    return(NULL);
} // AllocPages.






/////////////////////////////////////////////////////////////////////////////
//
// [GetBaseBlock]
//
// Find the base block that contains this free pointer.
/////////////////////////////////////////////////////////////////////////////
CMemAlloc::CBaseBlockHeaderType *
CMemAlloc::GetBaseBlock(const void *ptr) {
    CBaseBlockHeaderType * pBasePtr;
    char *startValidRegion;
    char *stopValidRegion;

    pBasePtr = m_BaseBlocks;
    while (pBasePtr) {
        startValidRegion = (char *) pBasePtr;
        startValidRegion += sizeof(CBaseBlockHeaderType);
        stopValidRegion = startValidRegion + pBasePtr->m_NumBytesAfterHeader;

        if ((((char *) ptr) >= startValidRegion)
                    && (((char *) ptr) <= stopValidRegion)) {
            break;
        }

        pBasePtr = pBasePtr->m_pNext;
    }

    return(pBasePtr);
} // GetBaseBlock.






/////////////////////////////////////////////////////////////////////////////
//
// [AddToFreeList]
//
/////////////////////////////////////////////////////////////////////////////
void
CMemAlloc::AddToFreeList(CFreeBufferType *pFreeBuffer) {
    int32 allocSize;
    int32 sizeClass;
    CFreeBufferType * nextFree;
    uint16 mask;

    // Hopefully, the compiler will do constant propagation
    // and make this a simple assignment.
    mask = BUFFER_ALLOCATED | BUFFER_HAS_FOOTER;
    (pFreeBuffer->m_Header).m_Flags &= ~mask;

    // Now, find the size class of the buffer. This will be
    // the smallest N such that 2**Nis larger than the size
    // of buffer we want to allocate.
    allocSize = MIN_BUFFER_SIZE;
    sizeClass = LOG_MIN_BUFFER_SIZE;
    while (allocSize < (pFreeBuffer->m_Header).m_NumDataBytes) {
        allocSize <<= 1;
        sizeClass += 1;
    }

    nextFree = m_FreeReassembledBufferList[sizeClass];
    pFreeBuffer->m_pNextFree = nextFree;
    pFreeBuffer->m_pPrevFree = NULL;
    if (nextFree) {
        nextFree->m_pPrevFree = pFreeBuffer;
    }
    m_FreeReassembledBufferList[sizeClass] = pFreeBuffer;
} // AddToFreeList.






/////////////////////////////////////////////////////////////////////////////
//
// [RemoveFromFreeList]
//
/////////////////////////////////////////////////////////////////////////////
void
CMemAlloc::RemoveFromFreeList(CFreeBufferType *pFreeBuffer, int32 sizeClass) {
    int32 allocSize;
    CFreeBufferType * nextFree;
    CFreeBufferType * prevFree;

    // If we don't know the size class of the buffer, then we will
    // have to calculate it. This will be  // the smallest N such that 2**Nis larger than the size
    // of buffer we want to allocate.
    if (sizeClass < 0) {
        allocSize = MIN_BUFFER_SIZE;
        sizeClass = LOG_MIN_BUFFER_SIZE;
        while (allocSize < (pFreeBuffer->m_Header).m_NumDataBytes) {
            allocSize <<= 1;
            sizeClass += 1;
        }
    }

    nextFree = pFreeBuffer->m_pNextFree;
    prevFree = pFreeBuffer->m_pPrevFree;
    if (prevFree) {
        prevFree->m_pNextFree = nextFree;
    } else
    {
        m_FreeReassembledBufferList[sizeClass] = nextFree;
    }

    if (nextFree) {
        nextFree->m_pPrevFree = prevFree;
    }
} // RemoveFromFreeList.







/////////////////////////////////////////////////////////////////////////////
//
// [AllocateVirtualMemoryRegion]
//
// This allocates another buffer of maximal size.
//
// The new buffer will be allocated on a buffer boundary; this
// is important because that means we only have to look at the bits
// below the bit that is log2 of the buffer size to find out where
// a partial block fits into a block. This is essential if we are to
// recombine buddy blocks.
//
// The bufferSize argument must be a power of 2.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CMemAlloc::AllocateVirtualMemoryRegion(int32 bufferSize) {
    ErrVal err = ENoErr;
    char *pBlock;

#if WIN32
    // At the bottom most level, all user-mode memory for a process is
    // allocated using NTAllocateVirtualMemory, including heap allocations.
    // Next up is VirtualAlloc and VirtualAllocEx. This bypasses the
    // run-time pool allocator.
    pBlock = (char *) VirtualAlloc(
                            NULL, // address of region to allocate. NULL lets the system decide
                            bufferSize, // size of region
                            MEM_COMMIT, // type of allocation
                            PAGE_READWRITE); // type of access protection
    if (NULL == pBlock) {
        DWORD dwErr = GetLastError();
        DEBUG_LOG("CMemAlloc::AllocateVirtualMemoryRegion. VirtualAlloc failed. GetLastError = %d", dwErr);
        gotoErr(TranslateWin32ErrorIntoErrVal(dwErr, true));
    }
#endif // WIN32
#if LINUX
    int32 extraSpace;
    caddr_t result;
    int32 bitsMask;
    int32 sBrkOffset;
    char *topOfRam;

    // Advance the sbrk so the buffer is allocated on a page boundary.
    // That ensures correct alignment, and also allows us to do page operations
    // like locking or unlocking pages.

    // Get the current break. This is the highest location defined
    // by the program and data areas. This is effectively the ceiling
    // of the useable part of our address space.
    while (1) {
        topOfRam = (char *) sbrk(0);
        if (topOfRam != (char *) -1) {
            break;
        }

        // Like all system calls, this one may be interrupted. We ignore
        // any errors and retry if the systm call was interrupted.
        if (errno != EINTR) {
            returnErr(EFail);
        }
    }

    // This is the amount of bytes between the break and the next
    // buffer boundary. We mask off the bits below the max buffer bit,
    // which shows the position of the sbrk relative to the previous
    // maxBuffer boundary.
    bitsMask = m_PageSize;
    bitsMask = bitsMask - 1;
    sBrkOffset = ((int64) topOfRam) & bitsMask;

    // This is the number of bytes until the next maxBuffer boundary.
    extraSpace = m_PageSize - sBrkOffset;

    // Advance the break so it falls on a maxBuffer boundary. Calls to
    // alloc may be interspersed with calls to other allocators so the sbrk
    // may not be where we left it last time.
    if (extraSpace) {
        while (1) {
            result = (char *) sbrk((int) extraSpace);
            if (result != (caddr_t)-1) {
                break;
            }

            // Like all systrem calls, this one may be interrupted. We ignore
            // any errors and retry if the systm call was interrupted.
            if (errno != EINTR) {
                gotoErr(EFail);
            }
        }
    }

    // Advance the break to reserve more memory for this address space.
    // The break is the highest location defined by the program and data
    // areas. This is effectively the ceiling of the useable part of our
    // address space. It is on a page boundary, and we only allocate data
    // on page trailers, so we will always get newly created page aligned
    // memory.
    while (1) {
        pBlock = (char *) sbrk((int) bufferSize);
        if (pBlock != (char *) -1) {
            break;
        }

        // Like all system calls, this one may be interrupted. We ignore
        // any errors and retry if the systm call was interrupted.
        if (errno != EINTR) {
            gotoErr(EFail);
        }
    }
#endif // LINUX


    err = AddStorageImpl(pBlock, bufferSize);
    if (err) {
        // This is a leak. We really should clean up the block.
        gotoErr(EFail);
    }

abort:
    returnErr(err);
} // AllocateVirtualMemoryRegion.







/////////////////////////////////////////////////////////////////////////////
//
// [ReleaseVirtualMemoryRegion]
//
/////////////////////////////////////////////////////////////////////////////
void
CMemAlloc::ReleaseVirtualMemoryRegion(CBaseBlockHeaderType * pBasePtr) {
    if (NULL == pBasePtr) {
        return;
    }

    m_TotalMem = m_TotalMem - pBasePtr->m_NumBytesAfterHeader;

#if WIN32
    BOOL fSuccess;

    fSuccess = VirtualFree(
                    (LPVOID) pBasePtr, // address of region of committed pages
                    0, // size of region. Must be 0 when we use MEM_RELEASE.
                    MEM_RELEASE); // type of free operation
    if (!fSuccess) {
        DWORD dwErr = GetLastError();
        DEBUG_LOG("CMemAlloc::ReleaseVirtualMemoryRegion. VirtualFree failed. GetLastError = %d", dwErr);
    }
#elif LINUX
#endif
} // ReleaseVirtualMemoryRegion






/////////////////////////////////////////////////////////////////////////////
//
// [GrowBuffer]
//
/////////////////////////////////////////////////////////////////////////////
void *
CMemAlloc::GrowBuffer(
                CBufferHeaderType *header,
                const void *buffer,
                int32 newSize,
                int32 allocSize,
                int32 newAllocSize) {
    ErrVal err = ENoErr;
    CBufferHeaderType *pNextHeader;
    int32 combinedSize;
    CBufferHeaderType *pUpperHeader;
    char *pUpperBuffer;
    int32 upperBufferSize;
#if DD_DEBUG
    CBufferFooterType trailer;
    char *pSrcPtr;
    char *pEndSrcPtr;
    char *pDestPtr;
#endif
    void *newBuffer;

    // Try to combine the current buffer with the next buffer.
    pNextHeader = header->m_pNext;
    if (pNextHeader) {
        combinedSize = allocSize
                        + g_AlignedSpaceForHeader // the next buffer header
                        + pNextHeader->m_NumDataBytes; // the next buffer

        // BE CAREFUL: Cached free blocks are not allocated, but they
        // should not be recombined. The whole point of the free cache
        // is to save the overhead of reassembling/disassembling the buffers.
        if ((!(pNextHeader->m_Flags & BUFFER_ALLOCATED))
                && !(pNextHeader->m_Flags & MEM_FREE_BUFFER_CACHED)
                && (combinedSize >= newAllocSize)) {
            // The next buffer is now part of the current buffer, so remove
            // the next buffer from the free list. IMPORTANT. Remove it from
            // the free list before we change its size, so we can find it in
            // the free list.
            RemoveFromFreeList((CFreeBufferType *) pNextHeader, -1);

            // Both the header and data parts of the next
            // buffer are added to the data of the current buffer.
            header->m_NumDataBytes = newSize;

            // Update the statistics before we combine this buffer
            // with adjacent buffers that were previously freed.
            m_TotalAllocBytes += g_AlignedSpaceForHeader;
            m_TotalAllocBytes += pNextHeader->m_NumDataBytes;

            // The next buffer is now part of the current buffer, so
            // remove the next buffer from the list of memory segments.
            header->m_pNext = pNextHeader->m_pNext;
            if (pNextHeader->m_pNext) {
                (pNextHeader->m_pNext)->m_pPrev = header;
            }

#if DD_DEBUG
            // If the old buffer had a trailer, then write a trailer at
            // the new end of user data.
            if (header->m_Flags & BUFFER_HAS_FOOTER) {
                trailer.m_MagicNum = FOOTER_MAGIC_WORD;
                pDestPtr = ((char *) buffer) + newSize;
                pSrcPtr = (char *) &trailer;
                pEndSrcPtr = pSrcPtr + sizeof(CBufferFooterType);

                while (pSrcPtr < pEndSrcPtr) {
                    *(pDestPtr++) = *(pSrcPtr++);
                }
            }
#endif

            // Find out how much extra space is in the buffer. If there
            // is enough to hold a second buffer, then split this buffer.
            upperBufferSize = combinedSize - newAllocSize;
            upperBufferSize = upperBufferSize - g_AlignedSpaceForHeader;

            // If there is enough space, then break it off and make it
            // into its own buffer.
            if (upperBufferSize >= MIN_BUFFER_SIZE) {
                // allocSize is an aligned offset, so pUpperHeader and
                // pUpperBuffer are both aligned.
                pUpperBuffer = ((char *) buffer) + newAllocSize;
                pUpperHeader = (CBufferHeaderType *) pUpperBuffer;
                pUpperBuffer += g_AlignedSpaceForHeader;

                // Initialize the upper header.
                pUpperHeader->m_Flags = 0;
                pUpperHeader->m_NumDataBytes = upperBufferSize;
                pUpperHeader->m_MagicNum = HEADER_MAGIC_WORD;

#if DD_DEBUG
                pUpperHeader->m_pFileName = NULL;
                pUpperHeader->m_LineNum = 0;

                memset(
                    (((char *) pUpperHeader) + g_AlignedSpaceForHeader),
                    unallocatedInt8,
                    pUpperHeader->m_NumDataBytes);
                pUpperHeader->m_Flags |= BUFFER_HAS_FREE_PATTERN;
#endif

                // Insert the upper header into the chain of all memory.
                pUpperHeader->m_pPrev = header;
                pUpperHeader->m_pNext = header->m_pNext;
                if (header->m_pNext) {
                    (header->m_pNext)->m_pPrev = pUpperHeader;
                }
                header->m_pNext = pUpperHeader;

                // Update the statistics.
                m_TotalAllocBytes = m_TotalAllocBytes - g_AlignedSpaceForHeader;
                m_TotalAllocBytes = m_TotalAllocBytes - pUpperHeader->m_NumDataBytes;

                // Also do any striping of free buffers here.
                AddToFreeList((CFreeBufferType *) pUpperHeader);
            } // breaking off the upper buffer.

            return((void *) buffer);
        } // combining the previous buffer with the next buffer.
    } // looking at the next buffer.

    newBuffer = Alloc(newSize);
    if (NULL == newBuffer) {
        gotoErr(EFail);
    }

    // Copy the contents from the old to the new buffer.
    memcpy(newBuffer, buffer, header->m_NumDataBytes);
    Free(buffer);

    return(newBuffer);
abort:
    return(NULL);
} // GrowBuffer







/////////////////////////////////////////////////////////////////////////////
//
// [ShrinkBuffer]
//
/////////////////////////////////////////////////////////////////////////////
void
CMemAlloc::ShrinkBuffer(
                CBufferHeaderType *header,
                const void *buffer,
                int32 newSize,
                int32 newAllocSize) {
    CBufferHeaderType *pUpperHeader;
    char *pUpperBuffer;
    int32 upperBufferSize;
#if DD_DEBUG
    CBufferFooterType trailer;
    char *pSrcPtr;
    char *pEndSrcPtr;
    char *pDestPtr;
#endif

    // If the user wants us to, initialize the trailer. The trailer
    // always appears at the end of the valid user data, not at the end
    // of the buffer. This is a better test for heap smashes and also
    // means the trailer is not effected if we decide to split the
    // buffer into two buffers.
#if DD_DEBUG
    if (header->m_Flags & BUFFER_HAS_FOOTER) {
        trailer.m_MagicNum = FOOTER_MAGIC_WORD;

        pDestPtr = ((char *) buffer) + newSize;
        pSrcPtr = (char *) &trailer;
        pEndSrcPtr = pSrcPtr + sizeof(CBufferFooterType);

        while (pSrcPtr < pEndSrcPtr) {
            *(pDestPtr++) = *(pSrcPtr++);
        }
    }
#endif

    // Find out how much extra space is in the buffer. If there
    // is enough to hold a second buffer, then split this buffer.
    // newAllocSize includes space for the footer.
    upperBufferSize = header->m_NumDataBytes - newAllocSize;
    upperBufferSize = upperBufferSize - g_AlignedSpaceForHeader;

    // If there is enough space, then break it off and make it
    // into its own buffer.
    if (upperBufferSize >= MIN_BUFFER_SIZE) {
        // newAllocSize is an aligned offset, so pUpperHeader and
        // pUpperBuffer are both aligned.
        pUpperBuffer = ((char *) buffer) + newAllocSize;
        pUpperHeader = (CBufferHeaderType *) pUpperBuffer;
        pUpperBuffer += g_AlignedSpaceForHeader;

        // Initialize the upper header.
        pUpperHeader->m_Flags = 0;
        pUpperHeader->m_NumDataBytes = upperBufferSize;
        pUpperHeader->m_MagicNum = HEADER_MAGIC_WORD;

#if DD_DEBUG
        pUpperHeader->m_pFileName = NULL;
        pUpperHeader->m_LineNum = 0;
#endif

        // Insert the upper header into the chain of all memory.
        pUpperHeader->m_pPrev = header;
        pUpperHeader->m_pNext = header->m_pNext;
        if (header->m_pNext) {
            (header->m_pNext)->m_pPrev = pUpperHeader;
        }
        header->m_pNext = pUpperHeader;

#if DD_DEBUG
        memset(
            (((char *) pUpperHeader) + g_AlignedSpaceForHeader),
            unallocatedInt8,
            upperBufferSize);
        pUpperHeader->m_Flags |= BUFFER_HAS_FREE_PATTERN;
#endif

        // Also do any striping of free buffers here.
        AddToFreeList((CFreeBufferType *) pUpperHeader);
    } // breaking off the upper buffer.

    m_TotalAllocBytes = m_TotalAllocBytes - header->m_NumDataBytes;
    m_TotalAllocBytes = m_TotalAllocBytes + newSize;

    // This field records the size of the user data, not its
    // allocation size.
    header->m_NumDataBytes = newSize;
} // ShrinkBuffer.







/////////////////////////////////////////////////////////////////////////////
//
// [DontCountAllCurrentAllocations]
//
// This is a global debugging procedure.
/////////////////////////////////////////////////////////////////////////////
void
DontCountAllCurrentAllocations() {
#if DD_DEBUG
    g_MainMem.DontCountCurrentAllocations();
#endif
} // DontCountAllCurrentAllocations






/////////////////////////////////////////////////////////////////////////////
//
// [CheckForMemoryLeaks]
//
// This is a global debugging procedure.
/////////////////////////////////////////////////////////////////////////////
void
CheckForMemoryLeaks(char *pStartBuffer, int32 maxSize, int32 *pResultSize) {
    int32 total = 0;

    // Pass true if this is in the middle of an IO callback.
    PrepareEngineToRecordAllocations(false);

#if DD_DEBUG
    total = g_MainMem.GetNumAllocations(CMemAlloc::PRINT_ONLY_CHANGES);
#endif

    // Check if there were any resource leaks.
    if (total > 0) {
#if DD_DEBUG
        PrepareEngineToRecordAllocations(false);
        g_MainMem.PrintAllocations(CMemAlloc::PRINT_ONLY_CHANGES | CMemAlloc::PRINT_VERBOSE, pStartBuffer, maxSize, pResultSize);
#endif
        DEBUG_WARNING("Leak");
    }
} // CheckForMemoryLeaks






/////////////////////////////////////////////////////////////////////////////
//
// [QSortMemUsageArray]
//
/////////////////////////////////////////////////////////////////////////////
static void
QSortMemUsageArray(int32 leftIndex, int32 rightIndex) {
    CMemoryAllocationClient tempItem;
    CMemoryAllocationClient partitionItem;
    CMemoryAllocationClient *currentItem;
    int32 currentIndex;
    int32 partitionIndex;

    if (leftIndex >= rightIndex) {
        return;
    }

    // Partition the array around the partition item. This moves through
    // the array, moving every item that is less than the partition item
    // to the left of the partition. This moves the partition right one
    // spot so it emcompasses the new item. The partition index ends up
    // at the boundary of items less than the partition and items greater
    // than the partition.
    partitionItem = g_AllocationDumpList[leftIndex];
    partitionIndex = leftIndex;
    for (currentIndex = leftIndex + 1; currentIndex <= rightIndex; currentIndex++) {
        currentItem = &(g_AllocationDumpList[currentIndex]);

        // If this item is < the partition, then move it to the left
        // of the partition index.
        if (currentItem->totalSizeInUse >= partitionItem.totalSizeInUse) {
            // Make space to the left of the partition. This makes the
            // partition include item X. Item X was to the right of the
            // partition, so it was greater than the partition item.
            partitionIndex++;

            // Swap the current item with item X. This moves the
            // small item into the left side of the partition and
            // item X back to the right of the partition.
            tempItem = g_AllocationDumpList[currentIndex];
            g_AllocationDumpList[currentIndex] = g_AllocationDumpList[partitionIndex];
            g_AllocationDumpList[partitionIndex] = tempItem;
        }
    } // partitioning the array.

    // Move the partition item to the partition index. This is the
    // boundary between items less than and items greater than
    // the partition.
    tempItem = g_AllocationDumpList[leftIndex];
    g_AllocationDumpList[leftIndex] = g_AllocationDumpList[partitionIndex];
    g_AllocationDumpList[partitionIndex] = tempItem;

    // Recursively sort both sides of the partition.
    QSortMemUsageArray(leftIndex, partitionIndex - 1);
    QSortMemUsageArray(partitionIndex+1, rightIndex);
} // QSortMemUsageArray.




/////////////////////////////////////////////////////////////////////////////
//
// [IsMemSet]
//
/////////////////////////////////////////////////////////////////////////////
bool
IsMemSet(const void *pVoidBuffer, int32 size, unsigned char byteVal) {
    // This has to be the same signed/unsigned type as the buffer ptr.
    unsigned char *pSrcByte;
    unsigned char *pEndSrcByte;

    if ((NULL == pVoidBuffer) || (size < 0)) {
        DEBUG_WARNING("Invalid params");
        return(false);
    }

    pSrcByte = (unsigned char *) pVoidBuffer;
    pEndSrcByte = pSrcByte + size;
    while (pSrcByte < pEndSrcByte) {
        if (byteVal != *(pSrcByte++)) {
            return(false);
        }
    }

    return(true);
} // IsMemSet





/////////////////////////////////////////////////////////////////////////////
//
// [strdupexImpl]
//
/////////////////////////////////////////////////////////////////////////////
char *
strdupexImpl(
    const char *pVoidPtr,
    int32 length,
    const char *pFileName,
    int32 lineNum) {
    ErrVal err = ENoErr;
    const char *pSrcPtr;
    char *pResultStr = NULL;

    pSrcPtr = (const char *) pVoidPtr;
    if (NULL == pSrcPtr) {
        return(NULL);
    }

    if (length < 0) {
        length = strlen(pSrcPtr);
    }

    pResultStr = (char *) (g_MainMem.Alloc(length + 1, pFileName, lineNum));
    if (NULL == pResultStr) {
        gotoErr(EFail);
    }

    strncpyex(pResultStr, pSrcPtr, length);

abort:
    if ((err) && (NULL != pResultStr)) {
        g_MainMem.Free(pResultStr);
        pResultStr = NULL;
    }

    return(pResultStr);
} // strdupexImpl





/////////////////////////////////////////////////////////////////////////////
//
// [strCatExImpl]
//
/////////////////////////////////////////////////////////////////////////////
char *
strCatExImpl(
        const char *pVoidPtr,
        const char *pSuffixVoidPtr,
        const char *pFileName,
        int32 lineNum) {
    ErrVal err = ENoErr;
    const char *pSrcPtr;
    const char *pSuffixPtr;
    int32 totalLength = 0;
    int32 srcLength = 0;
    int32 suffixLength = 0;
    char *pResultStr = NULL;


    pSrcPtr = (const char *) pVoidPtr;
    pSuffixPtr = (const char *) pSuffixVoidPtr;
    if (NULL == pSrcPtr) {
        return(NULL);
    }

    srcLength = strlen(pSrcPtr);
    if (pSuffixPtr) {
        suffixLength = strlen(pSuffixPtr);
    }
    totalLength = srcLength + suffixLength;


    pResultStr = (char *) (g_MainMem.Alloc(totalLength + 1, pFileName, lineNum));
    if (NULL == pResultStr) {
        gotoErr(EFail);
    }

    strncpyex(pResultStr, pSrcPtr, srcLength);
    strncpyex(pResultStr + srcLength, pSuffixPtr, suffixLength);

abort:
    if ((err) && (NULL != pResultStr)) {
        g_MainMem.Free(pResultStr);
        pResultStr = NULL;
    }

    return(pResultStr);
} // strCatExImpl






/////////////////////////////////////////////////////////////////////////////
//
//                     TESTING PROCEDURES
//
/////////////////////////////////////////////////////////////////////////////
#if INCLUDE_REGRESSION_TESTS

static void TestBufferSize(CMemAlloc *state, int32 size, int32 numPtrs);
static void TestErrors(CMemAlloc *state);
static void TestPageAllocs(
                CMemAlloc *state,
                int32 numPtrs,
                int32 numPages);


static int32 SMALL_BLOCK_SIZE = 40;
static int32 MED_BLOCK_SIZE = 5000;
static int32 LARGE_BLOCK_SIZE = 25000;

static int32 NUM_SMALL_BLOCKS = 600; // 600;
static int32 NUM_MEDIUM_BLOCKS = 100; // 100;
static int32 NUM_LARGE_BLOCKS = 10; // 10;

// This must be <= 1/4 the smallest buffer size.
static int32 SMALL_GROW_INCREMENT = 8;

#define kMaxNumTestPtrs     1500
static void * ptrList[kMaxNumTestPtrs];





/////////////////////////////////////////////////////////////////////////////
//
// [TestAlloc]
//
/////////////////////////////////////////////////////////////////////////////
void
CMemAlloc::TestAlloc() {
    ErrVal err = ENoErr;
    CMemAlloc state;
    int32 g_MinUserPayloadForFreeHeader = 8;

    g_DebugManager.StartModuleTest("memAlloc");


    err = state.Initialize(0);
    if (err) {
        return;
    }

    // Do some sanity checking.
    if (g_AlignedSpaceForHeader != sizeof(CBufferHeaderType)) {
        DEBUG_WARNING("Invalid data sizes");
    }
    g_MinUserPayloadForFreeHeader = sizeof (CFreeBufferType) - g_AlignedSpaceForHeader;
    if (g_MinUserPayloadForFreeHeader > MIN_BUFFER_SIZE) {
        DEBUG_WARNING("Invalid data sizes");
    }

    state.SetDebugFlags(CDebugObject::CHECK_STATE_ON_EVERY_OP);

    TestErrors(&state);

    g_DebugManager.StartSubTest("Allocating blocks with trailers and free-patterns and init patterns");

    g_DebugManager.SetProgressIncrement(100);
    g_DebugManager.StartSubTest("Allocating small blocks");
    TestBufferSize(&state, SMALL_BLOCK_SIZE, NUM_SMALL_BLOCKS);
    g_DebugManager.EndSubTest();

    g_DebugManager.SetProgressIncrement(50);
    g_DebugManager.StartSubTest("Allocating medium blocks");
    TestBufferSize(&state, MED_BLOCK_SIZE, NUM_MEDIUM_BLOCKS);
    g_DebugManager.EndSubTest();

    g_DebugManager.SetProgressIncrement(10);
    g_DebugManager.StartSubTest("Allocating large blocks");
    TestBufferSize(&state, LARGE_BLOCK_SIZE, NUM_LARGE_BLOCKS);
    g_DebugManager.EndSubTest();


    g_DebugManager.EndSubTest();

    TestErrors(&state);

    TestPageAllocs(&state, 5, 3);
} // TestAlloc.









/////////////////////////////////////////////////////////////////////////////
//
// [TestBufferSize]
//
/////////////////////////////////////////////////////////////////////////////
static void
TestBufferSize(CMemAlloc *state, int32 size, int32 numPtrs) {
    ErrVal err = ENoErr;
    int32 ptrNum;
    char *pDestPtr;
    char *endDest;
    int32 currentSize;
    char c;


    g_DebugManager.StartTest("Allocate buffers");

    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        ptrList[ptrNum] = state->Alloc(size);
        if (NULL == ptrList[ptrNum]) {
            DEBUG_WARNING("Alloc returned a NULL pointer");
        }
    }

    // Write some sample data to the blocks.
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        pDestPtr = (char *) ptrList[ptrNum];
        endDest = pDestPtr + size;
        c = (char) (ptrNum + 12);
        while (pDestPtr < endDest) {
            *(pDestPtr++) = c;
            c += 1;
        }
    }

    // Check that the data is still ok.
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        pDestPtr = (char *) ptrList[ptrNum];
        endDest = pDestPtr + size;
        c = (char) (ptrNum + 12);
        while (pDestPtr < endDest) {
            if (*(pDestPtr++) != c) {
                DEBUG_WARNING("Mangled data 1.");
            }
            c += 1;
        }
    }



    g_DebugManager.StartTest("GetPtrSize");

    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        currentSize = state->GetPtrSize(ptrList[ptrNum]);
        if (currentSize != size) {
            DEBUG_WARNING("GetPtrSize returns the wrong value.");
        }
    }




    g_DebugManager.StartTest("CheckPtr");

    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        err = state->CheckPtr(ptrList[ptrNum]);
        if (err) {
            DEBUG_WARNING("CheckPtr returns an error.");
        }
    }


    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();
        state->Free(ptrList[ptrNum]);
    }



    g_DebugManager.StartTest("Re-Allocate and free buffers");

    // Now re-allocate the blocks.
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        ptrList[ptrNum] = state->Alloc(size);
        if (ptrList[ptrNum] == NULL) {
            DEBUG_WARNING("re-alloc returned a NULL pointer");
        }
    }


    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        // Write some sample data to the blocks.
        pDestPtr = (char *) ptrList[ptrNum];
        endDest = pDestPtr + size;
        c = (char) (ptrNum + 21);
        while (pDestPtr < endDest) {
            *(pDestPtr++) = c;
            c += 1;
        }
    }

    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }


    // Check that the data is still ok.
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        pDestPtr = (char *) ptrList[ptrNum];
        endDest = pDestPtr + size;
        c = (char) (ptrNum + 21);
        while (pDestPtr < endDest) {
            if (*(pDestPtr++) != c) {
                DEBUG_WARNING("Mangled data 2. ptr %d, value %d",
                ptrNum, ptrList[ptrNum]);
            }
            c += 1;
        }
    }





    g_DebugManager.StartTest("CheckPtr");

    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        err = state->CheckPtr(ptrList[ptrNum]);
        if (err) {
            DEBUG_WARNING("CheckPtr returns an error.");
        }
    }


    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }


    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();
        state->Free(ptrList[ptrNum]);
    }


    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }



    // Now re-allocate the blocks.
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        ptrList[ptrNum] = state->Alloc(size);
        if (ptrList[ptrNum] == NULL) {
            DEBUG_WARNING("re-alloc returned a NULL pointer");
        }

        // Write some sample data to the blocks.
        pDestPtr = (char *) ptrList[ptrNum];
        endDest = pDestPtr + size;
        c = (char) (ptrNum + 21);
        while (pDestPtr < endDest) {
            *(pDestPtr++) = c;
            c += 1;
        }
    }

    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }



    // Check that the data is ok.
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        pDestPtr = (char *) ptrList[ptrNum];
        endDest = pDestPtr + size;
        c = (char) (ptrNum + 21);
        while (pDestPtr < endDest) {
            if (*(pDestPtr++) != c) {
                DEBUG_WARNING("Mangled data 3.");
            }
            c += 1;
        }
    }


    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }





    g_DebugManager.StartTest("Free half the buffers");

    // Free half the blocks.
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum += 2) {
        g_DebugManager.ShowProgress();
        state->Free(ptrList[ptrNum]);
    }


    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }



    // Check that the remaining blocks are still ok.
    for (ptrNum = 1; ptrNum < numPtrs; ptrNum += 2) {
        g_DebugManager.ShowProgress();

        pDestPtr = (char *) ptrList[ptrNum];
        endDest = pDestPtr + size;
        c = (char) (ptrNum + 21);
        while (pDestPtr < endDest) {
            if (*(pDestPtr++) != c) {
                DEBUG_WARNING("Mangled data 4.");
            }
            c += 1;
        }
    }


    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }


    // Now re-allocate half the blocks.
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum += 2) {
        g_DebugManager.ShowProgress();

        ptrList[ptrNum] = state->Alloc(size);
        if (ptrList[ptrNum] == NULL)
            DEBUG_WARNING("re-Alloc returned a NULL pointer");

        // Write some sample data to the blocks.
        pDestPtr = (char *) ptrList[ptrNum];
        endDest = pDestPtr + size;
        c = (char) (ptrNum + 21);
        while (pDestPtr < endDest) {
            *(pDestPtr++) = c;
            c += 1;
        }
    }


    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }



    // Check that the all blocks are still ok.
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        pDestPtr = (char *) ptrList[ptrNum];
        endDest = pDestPtr + size;
        c = (char) (ptrNum + 21);
        while (pDestPtr < endDest) {
            if (*(pDestPtr++) != c) {
                DEBUG_WARNING("Mangled data 5.");
            }
            c += 1;
        }
    }

    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }



    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();
        state->Free(ptrList[ptrNum]);
    }

    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }






    g_DebugManager.StartTest("Allocate buffers for resize tests");

    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }

    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        ptrList[ptrNum] = state->Alloc(size);
        if (NULL == ptrList[ptrNum]) {
            DEBUG_WARNING("Alloc returned a NULL pointer");
        }
    }

    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }


    // Write some sample data to the blocks.
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        pDestPtr = (char *) ptrList[ptrNum];
        endDest = pDestPtr + size;
        c = (char) (ptrNum + 12);
        while (pDestPtr < endDest) {
            *(pDestPtr++) = c;
            c += 1;
        }
    }

    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }


    // Check that the data is still ok.
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        pDestPtr = (char *) ptrList[ptrNum];
        endDest = pDestPtr + size;
        c = (char) (ptrNum + 12);
        while (pDestPtr < endDest) {
            if (*(pDestPtr++) != c) {
                DEBUG_WARNING("Mangled data 1.");
            }
            c += 1;
        }
    }

    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }


    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        currentSize = state->GetPtrSize(ptrList[ptrNum]);
        if (currentSize != size) {
            DEBUG_WARNING("GetPtrSize returns the wrong value.");
        }
    }


    g_DebugManager.StartTest("Shrink buffers");

    size = size >> 1;
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        ptrList[ptrNum] = state->Realloc(ptrList[ptrNum], size);
        if (NULL == ptrList[ptrNum]) {
            DEBUG_WARNING("Alloc returned a NULL pointer");
        }
    }

    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }


    // Write some sample data to the blocks.
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        pDestPtr = (char *) ptrList[ptrNum];
        endDest = pDestPtr + size;
        c = (char) (ptrNum + 12);
        while (pDestPtr < endDest) {
            *(pDestPtr++) = c;
            c += 1;
        }
    }

    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }


    // Check that the data is still ok.
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        pDestPtr = (char *) ptrList[ptrNum];
        endDest = pDestPtr + size;
        c = (char) (ptrNum + 12);
        while (pDestPtr < endDest) {
            if (*(pDestPtr++) != c) {
                DEBUG_WARNING("Mangled data 1.");
            }
            c += 1;
        }
    }

    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }


    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        currentSize = state->GetPtrSize(ptrList[ptrNum]);
        if (currentSize != size) {
            DEBUG_WARNING("GetPtrSize returns the wrong value.");
        }
    }

    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        err = state->CheckPtr(ptrList[ptrNum]);
        if (err) {
            DEBUG_WARNING("CheckPtr returns an error.");
        }
    }




    g_DebugManager.StartTest("Grow buffers a little");

    // For really small buffers, this will extend an overallocated buffer
    // (because shrinking did not leave enough room for the extra
    // space to become a new buffer). For larger buffers, this will cause
    // a recombination with a new buffer.
    size = size + SMALL_GROW_INCREMENT;
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        ptrList[ptrNum] = state->Realloc(ptrList[ptrNum], size);
        if (NULL == ptrList[ptrNum]) {
            DEBUG_WARNING("Alloc returned a NULL pointer");
        }
    }

    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }


    // Write some sample data to the blocks.
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        pDestPtr = (char *) ptrList[ptrNum];
        endDest = pDestPtr + size;
        c = (char) (ptrNum + 12);
        while (pDestPtr < endDest) {
            *(pDestPtr++) = c;
            c += 1;
        }
    }

    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }


    // Check that the data is still ok.
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        pDestPtr = (char *) ptrList[ptrNum];
        endDest = pDestPtr + size;
        c = (char) (ptrNum + 12);
        while (pDestPtr < endDest) {
            if (*(pDestPtr++) != c) {
                DEBUG_WARNING("Mangled data 1.");
            }
            c += 1;
        }
    }

    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }


    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        currentSize = state->GetPtrSize(ptrList[ptrNum]);
        if (currentSize != size) {
            DEBUG_WARNING("GetPtrSize returns the wrong value.");
        }
    }

    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        err = state->CheckPtr(ptrList[ptrNum]);
        if (err) {
            DEBUG_WARNING("CheckPtr returns an error.");
        }
    }





    g_DebugManager.StartTest("Grow buffers a lot");

    // For really small buffers, this will extend an overallocated buffer
    // (because shrinking did not leave enough room for the extra
    // space to become a new buffer). For larger buffers, this will cause
    // a recombination with a new buffer.

    size = size << 2;
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        ptrList[ptrNum] = state->Realloc(ptrList[ptrNum], size);
        if (NULL == ptrList[ptrNum]) {
            DEBUG_WARNING("Alloc returned a NULL pointer");
        }
    }

    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }


    // Write some sample data to the blocks.
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        pDestPtr = (char *) ptrList[ptrNum];
        endDest = pDestPtr + size;
        c = (char) (ptrNum + 12);
        while (pDestPtr < endDest) {
            *(pDestPtr++) = c;
            c += 1;
        }
    }

    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }


    // Check that the data is still ok.
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        pDestPtr = (char *) ptrList[ptrNum];
        endDest = pDestPtr + size;
        c = (char) (ptrNum + 12);
        while (pDestPtr < endDest) {
            if (*(pDestPtr++) != c) {
                DEBUG_WARNING("Mangled data 1.");
            }
            c += 1;
        }
    }

    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }


    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        currentSize = state->GetPtrSize(ptrList[ptrNum]);
        if (currentSize != size) {
            DEBUG_WARNING("GetPtrSize returns the wrong value.");
        }
    }

    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        err = state->CheckPtr(ptrList[ptrNum]);
        if (err) {
            DEBUG_WARNING("CheckPtr returns an error.");
        }
    }




    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();
        state->Free(ptrList[ptrNum]);
    }


    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }
} // TestBufferSize.







/////////////////////////////////////////////////////////////////////////////
//
// [TestErrors]
//
/////////////////////////////////////////////////////////////////////////////
static void
TestErrors(CMemAlloc *state) {
    ErrVal err = ENoErr;
    int32 currentSize;
    void *ptr;


    g_DebugManager.StartTest("Error Cases");
    CDebugManager::g_TestingErrorCases = true;

    ptr = state->Alloc(-1);
    if (ptr) {
        DEBUG_WARNING("Allocated a negative size.");
    }
    ptr = state->Alloc(0);
    if (!ptr) {
        DEBUG_WARNING("Didn't allocate a zero size buffer.");
    }

    ptr = (char *) 0;
    state->Free(ptr);
    ptr = (char *) 1;
    state->Free(ptr);
    state->Free(ptrList[0]);


    ptr = state->Realloc(NULL, 1);
    if (ptr) {
        DEBUG_WARNING("re-allocated a NULL ptr.");
    }
    ptr = state->Realloc((char *) 1, 1);
    if (ptr) {
        DEBUG_WARNING("re-allocated a bad ptr.");
    }
    ptr = state->Realloc(ptrList[0], 1);
    if (ptr) {
        DEBUG_WARNING("re-allocated a free ptr.");
    }


    currentSize = state->GetPtrSize(NULL);
    if (currentSize > 0) {
        DEBUG_WARNING("GetPtrSize returned a size for a bad pointer.");
    }
    currentSize = state->GetPtrSize((char *) 1);
    if (currentSize > 0) {
        DEBUG_WARNING("GetPtrSize returned a size for a bad pointer.");
    }
    currentSize = state->GetPtrSize(ptrList[0]);
    if (currentSize > 0) {
        DEBUG_WARNING("GetPtrSize returned a size for a bad pointer.");
    }


    err = state->CheckPtr(NULL);
    if (!err) {
        DEBUG_WARNING("CheckPtr returned ENoErr for a bad pointer.");
    }
    err = state->CheckPtr((char *) 1);
    if (!err) {
        DEBUG_WARNING("CheckPtr returned ENoErr for a bad pointer.");
    }
    err = state->CheckPtr(ptrList[0]);
    if (!err) {
        DEBUG_WARNING("CheckPtr returned ENoErr for a bad pointer.");
    }

    CDebugManager::g_TestingErrorCases = false;
} // TestErrors.









/////////////////////////////////////////////////////////////////////////////
//
// [TestPageAllocs]
//
/////////////////////////////////////////////////////////////////////////////
static void
TestPageAllocs(CMemAlloc *state, int32 numPtrs, int32 numPages) {
    ErrVal err = ENoErr;
    int32 ptrNum;
    char *pDestPtr;
    char *endDest;
    int32 currentSize;
    char c;
    int32 size;


    g_DebugManager.StartTest("Page Allocations");
    size = numPages * (state->GetBytesPerPage());


    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }


    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        ptrList[ptrNum] = state->AllocPages(numPages, NULL, 0);
        if (NULL == ptrList[ptrNum]) {
            DEBUG_WARNING("Alloc returned a NULL pointer");
        }
    }


    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }


    // Write some sample data to the blocks.
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        pDestPtr = (char *) ptrList[ptrNum];
        endDest = pDestPtr + size;
        c = (char) (ptrNum + 12);
        while (pDestPtr < endDest) {
            *(pDestPtr++) = c;
            c += 1;
        }
    }

    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }


    // Check that the data is still ok.
    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        pDestPtr = (char *) ptrList[ptrNum];
        endDest = pDestPtr + size;
        c = (char) (ptrNum + 12);
        while (pDestPtr < endDest) {
            if (*(pDestPtr++) != c) {
                DEBUG_WARNING("Mangled data 1.");
            }
            c += 1;
        }
    }


    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }


    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        currentSize = state->GetPtrSize(ptrList[ptrNum]);
        if (currentSize != size) {
            DEBUG_WARNING("GetPtrSize returns the wrong value.");
        }
    }


    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();

        err = state->CheckPtr(ptrList[ptrNum]);
        if (err) {
            DEBUG_WARNING("CheckPtr returns an error.");
        }
    }


    for (ptrNum = 0; ptrNum < numPtrs; ptrNum++) {
        g_DebugManager.ShowProgress();
        state->Free(ptrList[ptrNum]);
    }


    err = state->CheckState();
    if (err) {
        DEBUG_WARNING("Error from CheckState.");
    }
} // TestPageAllocs.

#endif // INCLUDE_REGRESSION_TESTS


