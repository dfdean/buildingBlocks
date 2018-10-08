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
// BuildingBlocks
//
// The buildingBlocks initializes a hierarchy of modules. Really, this hierarchy
// is only significant when you have to port the buildingBlocks to another 
// platform. All software aboive the buildingBlocks treats it as a single 
// large statically linked library, like the C runtime.
//
//
//   buildingBlocksManager.cpp
//
//------------------------------------------------------------
// Above this level, all modules can use serialized objects
//------------------------------------------------------------
//
//   serializedObject.cpp
//
//------------------------------------------------------------
// Above this level, all modules can use HTTP and XML
//------------------------------------------------------------
//
//   polyXMLDocText.cpp
//   polyXMLDoc.cpp
//
//   polyHTTPStreamBasic.cpp
//   polyHTTPStream.cpp
//
// *** LOGGING IS USED BELOW HERE
//
//------------------------------------------------------------
// Above this level, all modules can use stream IO
//------------------------------------------------------------
//
//   asyncIOStream.cpp
//
//   netBlockIO.cpp
//   fileBlockIO.cpp
//   memoryBlockIO.cpp
//   blockIO.cpp
//
//------------------------------------------------------------
// Above this level, all modules can use URLs
//------------------------------------------------------------
//
//   url.cpp
//
//   nameTable.cpp
//   rbTree.cpp
//
//   stringParse.cpp
//
//   jobQueue.cpp
//
//   queue.cpp
//
//   fileUtils.cpp
//
//------------------------------------------------------------
// Above this level, all modules are thread safe.
//------------------------------------------------------------
//
//   threads.cpp
//
//------------------------------------------------------------
// Above this level, all modules can use refcounted objects.
//------------------------------------------------------------
//
//   refCount.cpp
//
//------------------------------------------------------------
// Above this level, all modules can use the debug memory heap
//------------------------------------------------------------
//
//   memAlloc.cpp
//
//------------------------------------------------------------
// Above this level, all modules can use debugging utilities.
//------------------------------------------------------------
//
//   debugging.cpp
//
//   log.cpp
//
//   config.cpp
//
//   stringLib.cpp
//
//   OSIndependantLayer.cpp
//
/////////////////////////////////////////////////////////////////////////////

#include "buildNumber.h"

#include "buildingBlocks.h"

FILE_DEBUGGING_GLOBALS(LOG_LEVEL_DEFAULT, 0);

extern bool g_ShutdownBuildingBlocks;
extern CDebugManager g_DebugManager;
CConfigSection *g_pBuildingBlocksConfig = NULL;

static char BuildingBlocksConfigKeyName[] = "BuildingBlocks";
static int32 g_FeatureLevel = 0;


/////////////////////////////////////////////////////////////////////////////
//
// [Initialize]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CBuildingBlocks::Initialize(int32 featureLevel, CProductInfo *pProductInfo) {
    ErrVal err = ENoErr;

    g_FeatureLevel = featureLevel;

    // If some product does not provide a valid version, then
    // make a generic description.
    if (NULL == pProductInfo) {
        static CProductInfo g_DefaultProductInfo;
        pProductInfo = &g_DefaultProductInfo;

        pProductInfo->m_CompanyName = "DawsonDean";
        pProductInfo->m_ProductName = "TestProduct";
        pProductInfo->m_MajorVersion = 1;
        pProductInfo->m_MinorVersion = 0;
        pProductInfo->m_MinorMinorVersion = 0;
        pProductInfo->m_Milestone = 0;
        pProductInfo->m_ReleaseType = CProductInfo::Release;
        pProductInfo->m_ReleaseNumber = 1;
    }

    // Fill in the standard part of the product version. These are
    // not specific to one particular product type. They instead
    // programmatically generated in the generic buildVersion header file.
    pProductInfo->m_BuildNumber = BUILD_NUMBER;
    pProductInfo->m_pBuildDate = BUILD_DATE;

    // This may be dynamically changes by a command line or 
    // from an environment variable.
    if (pProductInfo->m_pHomeDirectory) {
        snprintf(g_SoftwareDirectoryRoot, 
                 sizeof(g_SoftwareDirectoryRoot),
                 pProductInfo->m_pHomeDirectory);
    }
    

    err = OSIndependantLayer::InitializeOSIndependantLayer();
    if (err) {
        gotoErr(err);
    }

    // Initialize this before we turn on logging. This tells
    // us where to put the log file.
    g_Config = new CConfigFile;
    if (NULL == g_Config) {
        gotoErr(EFail);
    }
    g_Config->ReadProductConfig(pProductInfo);

    g_pBuildingBlocksConfig 
            = g_Config->FindOrCreateSection(BuildingBlocksConfigKeyName);
    if (NULL == g_pBuildingBlocksConfig) {
        gotoErr(EFail);
    }

    err = CDebugManager::InitializeDebugging(
                             pProductInfo,
                             CDebugManager::ADD_TIMESTAMP_TO_EACH_LINE
                                 | CDebugManager::ADD_FILENAME_TO_EACH_LINE
                                 // | CDebugManager::ADD_THREADID_TO_EACH_LINE
                                 | CDebugManager::ADD_FUNCTION_TO_EACH_LINE);
    if (err) {
        gotoErr(err);
    }

    err = g_MainMem.Initialize(0);
    if (err) {
        gotoErr(err);
    }

    err = CRefCountImpl::InitializeGlobalState();
    if (err) {
        gotoErr(err);
    }

    err = CSimpleThread::InitializeModule();
    if (err) {
        gotoErr(err);
    }

    err = CJobQueue::InitializeGlobalJobQueues();
    if (err) {
        gotoErr(err);
    }

    err = InitializeMemoryBlockIO();
    if (err) {
        gotoErr(err);
    }

    err = InitializeFileBlockIO();
    if (err) {
        gotoErr(err);
    }

    if (FULL_FUNCTIONALITY == g_FeatureLevel) {
        err = InitializeNetBlockIO();
        if (err) {
            gotoErr(err);
        }

        err = InitializeBasicHTTPStreamGlobalState(pProductInfo);
        if (err) {
            gotoErr(err);
        }
    } // if (xxxx == g_FeatureLevel)

abort:
    returnErr(err);
} // Initialize.





/////////////////////////////////////////////////////////////////////////////
//
// [Shutdown]
//
/////////////////////////////////////////////////////////////////////////////
void
CBuildingBlocks::Shutdown() {
    if (FULL_FUNCTIONALITY == g_FeatureLevel) {
        if (g_pNetIOSystem) {
            (void) g_pMemoryIOSystem->Shutdown();
        }
    }

    if (g_pMemoryIOSystem) {
        (void) g_pMemoryIOSystem->Shutdown();
    }
    
    if (g_pFileIOSystem) {
        (void) g_pFileIOSystem->Shutdown();
    }

    CJobQueue::ShutdownGlobalJobQueues();
    CDebugManager::ShutdownDebugging();
    OSIndependantLayer::ShutdownOSIndependantLayer();

    // All future deallocators will not work, since we are about to
    // shut down the memory system.
    g_ShutdownBuildingBlocks = true;
} // Shutdown





/////////////////////////////////////////////////////////////////////////////
//
// [PrepareEngineToRecordAllocations]
//
/////////////////////////////////////////////////////////////////////////////
void
PrepareEngineToRecordAllocations(bool fInIOCallback) {
    UNUSED_PARAM(fInIOCallback);

///////////////////////
#if 0
    int32 numAllowedActiveCallbacks = 0;

    if (fInIOCallback) {
        numAllowedActiveCallbacks = 1;
    }

    // Do this first, because some buffers (IOEvents) will
    // be released as idle buffers.
    while (CIOSystem::GetTotalActiveIOJobs() > numAllowedActiveCallbacks) {
        Sleep(10);
    }
    g_NetIOSystem.WaitForAllBlockIOsToClose();
#endif

    CRefCountImpl::EmptyPendingDeleteList();
} // PrepareEngineToRecordAllocations





/////////////////////////////////////////////////////////////////////////////
//
// [TestBuildingBlocks]
//
/////////////////////////////////////////////////////////////////////////////
void
CBuildingBlocks::TestBuildingBlocks() {
#if INCLUDE_REGRESSION_TESTS
    // Initialize the world for a testing client.
    CBuildingBlocks::Initialize(CBuildingBlocks::FULL_FUNCTIONALITY, NULL);
    g_DebugManager.StartAllTests();

    //OSIndependantLayer::TestOSIndependantLayer();
    //CStringLib::TestStringLib();
    //CConfigFile::TestConfig();
    //CEventLog::TestLog();
    //CMemAlloc::TestAlloc();
    //CRefCountImpl::TestRefCounting();
    //CSimpleThread::TestThreads();
    //TestQueue();
    //CJobQueue::TestJobQueue();
    //CRBTree::TestTree();
    //CNameTable::TestNameTable();
    //CParsedUrl::TestURL();
    //CIOSystem::TestBlockIO();
    //CAsyncIOStream::TestAsyncIOStream();
    //CPolyHttpStream::TestHTTPStream();
    CPolyXMLDoc::TestXMLModule();

    g_DebugManager.EndAllTests();
    CBuildingBlocks::Shutdown();
    //OSIndependantLayer::PrintToConsole("\nDone\n");
#endif // INCLUDE_REGRESSION_TESTS
} // TestBuildingBlocks




/////////////////////////////////////////////////////////////////////////////
//
// [TestBuildingBlocks]
//
/////////////////////////////////////////////////////////////////////////////
void
TestBuildingBlocks() {
    CBuildingBlocks::TestBuildingBlocks();
} // TestBuildingBlocks




