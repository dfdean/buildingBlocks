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
// HTTP Stream Module
//
// This manages a complete request-response cycle.
/////////////////////////////////////////////////////////////////////////////

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
#include "nameTable.h"
#include "url.h"
#include "blockIO.h"
#include "asyncIOStream.h"
#include "polyHTTPStream.h"

FILE_DEBUGGING_GLOBALS(LOG_LEVEL_DEFAULT, 0);



/////////////////////////////////////////////////////////////////////////////
//
// [CSynchPolyHttpCallback]
//
/////////////////////////////////////////////////////////////////////////////
CSynchPolyHttpCallback::CSynchPolyHttpCallback() {
    m_pSemaphore = NULL;
    m_Err = ENoErr;
} // CSynchPolyHttpCallback



/////////////////////////////////////////////////////////////////////////////
//
// [~CSynchPolyHttpCallback]
//
/////////////////////////////////////////////////////////////////////////////
CSynchPolyHttpCallback::~CSynchPolyHttpCallback() {
    RELEASE_OBJECT(m_pSemaphore);
} // ~CSynchPolyHttpCallback




/////////////////////////////////////////////////////////////////////////////
//
// [Initialize]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSynchPolyHttpCallback::Initialize() {
    ErrVal err = ENoErr;

    m_pSemaphore = newex CRefEvent;
    if (NULL == m_pSemaphore) {
        gotoErr(EFail);
    }

    err = m_pSemaphore->Initialize();
    if (err) {
        gotoErr(err);
    }

abort:
    returnErr(err);
} // Initialize.





/////////////////////////////////////////////////////////////////////////////
//
// [Wait]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSynchPolyHttpCallback::Wait() {
    if (m_pSemaphore) {
        m_pSemaphore->Wait();
    }
    return(m_Err);
} // Wait.





/////////////////////////////////////////////////////////////////////////////
//
// [OnReadHTTPDocument]
//
/////////////////////////////////////////////////////////////////////////////
void
CSynchPolyHttpCallback::OnReadHTTPDocument(
                            ErrVal err,
                            CPolyHttpStream *pStream,
                            void *pCallbackContext) {
    // Unused
    pStream = pStream;
    pCallbackContext = pCallbackContext;

    m_Err = err;
    if (m_pSemaphore) {
       m_pSemaphore->Signal();
    }
} // OnReadHTTPDocument.







/////////////////////////////////////////////////////////////////////////////
//
// [OnWriteHTTPDocument]
//
/////////////////////////////////////////////////////////////////////////////
void
CSynchPolyHttpCallback::OnWriteHTTPDocument(
                            ErrVal err,
                            CPolyHttpStream *pStream,
                            void *pCallbackContext) {
    // Unused
    pStream = pStream;
    pCallbackContext = pCallbackContext;

    m_Err = err;
    if (m_pSemaphore) {
       m_pSemaphore->Signal();
    }
} // OnWriteHTTPDocument.





/////////////////////////////////////////////////////////////////////////////
//
//                     TESTING PROCEDURES
//
/////////////////////////////////////////////////////////////////////////////
#if INCLUDE_REGRESSION_TESTS

static char g_DumpFileName[1024];
static CAsyncIOEventHandlerSynch *g_pTestCallback = NULL;

static void TestOneURL(const char *urlStr, int32 *pResultSize);


/////////////////////////////////////////////////////////////////////////////
//
// [TestHTTPStream]
//
/////////////////////////////////////////////////////////////////////////////
void
CPolyHttpStream::TestHTTPStream() {
    ErrVal err = ENoErr;
    const char *test;
    int32 size1;
    int32 index;

    g_DebugManager.StartModuleTest("HTTP Stream");

    g_DebugManager.AddTestResultsDirectoryPath("LexerDump.txt", g_DumpFileName, sizeof(g_DumpFileName));

    g_pTestCallback = newex CAsyncIOEventHandlerSynch;
    if (NULL == g_pTestCallback) {
        DEBUG_WARNING("Cannot parse the url.");
        return;
    }
    err = g_pTestCallback->Initialize();
    if (err) {
        DEBUG_WARNING("Cannot parse the url.");
        return;
    }

    g_pNetIOSystem->SetDebugFlags(CDebugObject::CHECK_STATE_ON_EVERY_OP);
    DontCountAllCurrentAllocations();

    ////////////////////////////////////////
    for (index = 0; ; index++) {
        test = g_TestURLList[index];
        if (NULL == test) {
            break;
        }

        g_DebugManager.StartTest(test);
        TestOneURL(test, &size1);
    } // running each test.
} // TestHTTPStream.





/////////////////////////////////////////////////////////////////////////////
//
// [TestOneURL]
//
/////////////////////////////////////////////////////////////////////////////
void
TestOneURL(const char *urlStr, int32 *pResultSize) {
    ErrVal err = ENoErr;
    CAsyncIOStream *pBodyStream = NULL;
    CAsyncIOStream *pWriteStream = NULL;
    CParsedUrl *pUrl = NULL;
    char bodyStr[256];
    CPolyHttpStream *pHTTPStream = NULL;
    CSynchPolyHttpCallback *pCallback = NULL;
    int64 startBodyPosition;
    int16 type;
    int16 subType;
    int32 statusCode;

    *pResultSize = 0;

    // Make a URL
    pUrl = CParsedUrl::AllocateUrl(urlStr);
    if (NULL == pUrl) {
        gotoErr(EFail);
    }

    // Make a stream
    pHTTPStream = CPolyHttpStream::AllocateSimpleStream();
    if (NULL == pHTTPStream) {
        gotoErr(EFail);
    }

    // Make a callback.
    pCallback = newex CSynchPolyHttpCallback;
    if (NULL == pCallback) {
        gotoErr(EFail);
    }
    err = pCallback->Initialize();
    if (err) {
        gotoErr(err);
    }

    // Load the document.
    pHTTPStream->ReadHTTPDocument(pUrl, pCallback, NULL);
    err = pCallback->Wait();
    if (err) {
        //if ((ENoResponse == err) || (ENoHostAddress == err) || (EEOF == err) || (EHTTPSRequired == err))
        OSIndependantLayer::PrintToConsole(CDebugManager::GetErrorDescriptionString(err));
        gotoErr(err);
    }

    statusCode = pHTTPStream->GetStatusCode();
    err = pHTTPStream->GetIOStream(
                        &pBodyStream,
                        &startBodyPosition,
                        NULL); // *pLength
    if (NULL == pBodyStream) {
        gotoErr(EFail);
    }


    // Many servers return no body when they return an error
    // like a redirect message.
    if (200 != statusCode) {
        OSIndependantLayer::PrintToConsole("Error status code: %d.", statusCode);
    }


    // Copy the page to a local file.
    RELEASE_OBJECT(pUrl);
    pUrl = CParsedUrl::AllocateFileUrl(g_DumpFileName);
    if (NULL == pUrl) {
        gotoErr(EFail);
    }

    err = CAsyncIOStream::OpenAsyncIOStream(
                                pUrl,
                                CAsyncBlockIO::CREATE_NEW_STORE
                                    | CAsyncBlockIO::READ_ACCESS
                                    | CAsyncBlockIO::WRITE_ACCESS,
                                g_pTestCallback,
                                NULL);
    if (err) {
        gotoErr(err);
    }

    err = g_pTestCallback->Wait();
    if (err) {
        gotoErr(err);
    }

    pWriteStream = g_pTestCallback->m_pAsyncIOStream;
    g_pTestCallback->m_pAsyncIOStream = NULL;


    // I want the http header as well as the body.
    err = pBodyStream->SetPosition(0);
    if (err) {
        if (EEOF != err)
            DEBUG_WARNING("Cannot open the http stream.");
        gotoErr(err);
    }

    err = pBodyStream->CopyStream(pWriteStream, CAsyncIOStream::COPY_TO_EOF, false);
    if (err) {
        gotoErr(err);
    }

    pWriteStream->Flush();
    err = g_pTestCallback->Wait();
    if (err) {
        gotoErr(err);
    }


    err = pBodyStream->SetPosition(startBodyPosition);
    if (err) {
        if (EEOF != err) {
            DEBUG_WARNING("Cannot open the http stream.");
        }
        gotoErr(err);
    }

    err = pBodyStream->Read(bodyStr, 100);
    bodyStr[100] = 0;
    if ((err) && (EEOF != err)) {
        DEBUG_WARNING("Cannot read 100 bytes from the body.");
        gotoErr(err);
    }

    *pResultSize = (int32) (pBodyStream->GetDataLength());
    err = pHTTPStream->GetContentType(&type, &subType);
    if (err) {
        gotoErr(err);
    }

abort:
    RELEASE_OBJECT(pUrl);
    if (pHTTPStream) {
        pHTTPStream->CloseStreamToURL();
    }
    if (pWriteStream) {
        pWriteStream->Close();
    }
    RELEASE_OBJECT(pWriteStream);
    RELEASE_OBJECT(pHTTPStream);
    RELEASE_OBJECT(pBodyStream);
    RELEASE_OBJECT(pCallback);
} // TestOneURL.


#endif // INCLUDE_REGRESSION_TESTS

