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
// CPolyXMLDoc Base Class Module
//
/////////////////////////////////////////////////////////////////////////////

#include "osIndependantLayer.h"
#include "config.h"
#include "log.h"
#include "debugging.h"
#include "memAlloc.h"
#include "refCount.h"
#include "fileUtils.h"
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
#include "polyXMLDoc.h"

FILE_DEBUGGING_GLOBALS(LOG_LEVEL_DEFAULT, 0);


// This is the state of pretty-printing a document.
class CPrintFormat {
public:
    CAsyncIOStream      *m_pOutStream;
    CAsyncIOStream      *m_pSourceStream;

    int32               m_IndentLevel;

    int32               m_Options;
    bool                m_fIgnoreNextWhitespace;

    bool                m_SuspendIndentation;
}; // CPrintFormat

static ErrVal WriteSubTree(CPolyXMLNode *pNode, CPrintFormat *pFormat);
static ErrVal WriteTextNode(CPolyXMLNode *pNode, CPrintFormat *pFormat);
static ErrVal WriteElement(CPolyXMLNode *pNode, CPrintFormat *pFormat);
static ErrVal PrintValueToAsyncIOStream(
                            CPropertyValue *pValue,
                            CAsyncIOStream *pOutStream,
                            CAsyncIOStream *pSrcStream);

#if DEBUG_XML
CPolyXMLNode *GetNodeWrittenAtPosition(CPolyXMLNode *pNode, int64 position);
int32 g_NextDebugNodeId = 0;
int32 g_BuggyNode = 1889;
#endif // DEBUG_XML




/////////////////////////////////////////////////////////////////////////////
//
// [CPolyXMLDoc]
//
/////////////////////////////////////////////////////////////////////////////
CPolyXMLDoc::CPolyXMLDoc() {
    m_pUrl = NULL;
} // CPolyXMLDoc




/////////////////////////////////////////////////////////////////////////////
//
// [~CPolyXMLDoc]
//
/////////////////////////////////////////////////////////////////////////////
CPolyXMLDoc::~CPolyXMLDoc() {
   RELEASE_OBJECT(m_pUrl);
} // ~CPolyXMLDoc




/////////////////////////////////////////////////////////////////////////////
//
// [WriteDocToStream]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyXMLDoc::WriteDocToStream(CAsyncIOStream *pOutStream, int32 writeOptions) {
    ErrVal err = ENoErr;
    CPolyXMLNode *pRoot = NULL;
    CPrintFormat format;

    format.m_IndentLevel = 0;
    format.m_Options = writeOptions;
    format.m_fIgnoreNextWhitespace = false;
    format.m_SuspendIndentation = false;
    format.m_pOutStream = pOutStream;
    err = GetIOStream(
                &(format.m_pSourceStream),
                NULL, // *pStartPosition,
                NULL); // *pLength
    if (err) {
        gotoErr(err);
    }

    pRoot = GetRoot();
    if (NULL != pRoot) {
        err = WriteSubTree(pRoot, &format);
    }

abort:
    returnErr(err);
} // WriteParsedPage.






/////////////////////////////////////////////////////////////////////////////
//
// [WriteSubTree]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
WriteSubTree(CPolyXMLNode *pNode, CPrintFormat *pFormat) {
    ErrVal err = ENoErr;
    CPolyXMLNode *pChild;
    const char *pCloseName;
    int32 closeNameLength;
    int32 nodeFlags = 0;
    CPolyXMLNode::XMLNodeType nodeType;
    bool fRecurseOnChildren = false;
    int32 deltaIndent = 0;
    bool fSuspendingIndentation = false;


    if ((NULL == pNode)
         || (NULL == pFormat)) {
       gotoErr(EFail);
    }


#if DEBUG_XML
    if ((pNode->m_StartWritePos > 0) || (pNode->m_StopWritePos > 0)) {
        pNode = pNode;
    }

    if ((g_BuggyNode > 0) && (g_BuggyNode == pNode->m_DebugNodeId)) {
        g_BuggyNode = g_BuggyNode;
    }
#endif // DEBUG_XML

    nodeType = pNode->GetNodeType();
    switch (nodeType) {
    /////////////////////////////////////////////
    case CPolyXMLNode::XML_NODE_ELEMENT:
#if DEBUG_XML
        if (pFormat->m_pOutStream) {
            pNode->m_StartWritePos = pFormat->m_pOutStream->GetPosition();
        }
#endif // DEBUG_XML

        nodeFlags = pNode->GetNodeFlags();
        if (nodeFlags & CPolyXMLNode::LAZY_ATTRIBUTES_PARSED) {
            err = WriteElement(pNode, pFormat);
            if (err) {
                gotoErr(err);
            }
        } else {
            err = WriteTextNode(pNode, pFormat);
            if (err) {
                gotoErr(err);
            }
        }

        fRecurseOnChildren = true;
        deltaIndent = 1;

#if DEBUG_XML
        if (pFormat->m_pOutStream) {
            pNode->m_StopWritePos = pFormat->m_pOutStream->GetPosition();
        }
#endif // DEBUG_XML
        break;

    /////////////////////////////////////////////
    case CPolyXMLNode::XML_NODE_TEXT:
    case CPolyXMLNode::XML_NODE_COMMENT:
    case CPolyXMLNode::XML_NODE_PROCESSING_INSTRUCTION:
    case CPolyXMLNode::XML_NODE_CDATA:
#if DEBUG_XML
        if (pFormat->m_pOutStream) {
            pNode->m_StartWritePos = pFormat->m_pOutStream->GetPosition();
        }
#endif // DEBUG_XML

        err = WriteTextNode(pNode, pFormat);
        if (err) {
            gotoErr(err);
        }

#if DEBUG_XML
        if (pFormat->m_pOutStream) {
            pNode->m_StopWritePos = pFormat->m_pOutStream->GetPosition();
        }
#endif // DEBUG_XML
        break;

    /////////////////////////////////////////////
    case CPolyXMLNode::XML_NODE_DOCUMENT:
        fRecurseOnChildren = true;
        deltaIndent = 0;
        break;

    /////////////////////////////////////////////
    case CPolyXMLNode::XML_NODE_ATTRIBUTE:
    default:
       gotoErr(EFail);
       break;
    } // switch (nodeType)


    if (fRecurseOnChildren) {
        pChild = pNode->GetFirstChild();
        while (NULL != pChild) {
            pFormat->m_IndentLevel += deltaIndent;
            err = WriteSubTree(pChild, pFormat);
            pFormat->m_IndentLevel -= deltaIndent;

            if (err) {
                gotoErr(err);
            }

            pChild = pChild->GetNextSibling();
        } // writing every tag.
    } // if (fRecurseOnChildren)


    // Write the closing element.
    if ((fRecurseOnChildren)
        && (CPolyXMLNode::XML_NODE_ELEMENT == nodeType)
        && !(nodeFlags & CPolyXMLNode::ELEMENT_HAS_NO_CLOSE)
        && !(nodeFlags & CPolyXMLNode::SELF_CLOSING_PROCESSING_INSTRUCTION)
        && !(nodeFlags & CPolyXMLNode::SELF_CLOSING_ELEMENT)) {
        if (nodeFlags & CPolyXMLNode::ELEMENT_HAS_SPECIAL_CLOSE) {
            pCloseName = pNode->GetSpecialCloseName();
            if (pFormat->m_pOutStream) {
                err = pFormat->m_pOutStream->printf(pCloseName);
                if (err) {
                    gotoErr(err);
                }
            }
        } else {
            pNode->GetName(&pCloseName, &closeNameLength);
            if (pFormat->m_pOutStream) {
                err = pFormat->m_pOutStream->printf("</");
                if (err) {
                    gotoErr(err);
                }
                err = pFormat->m_pOutStream->Write(pCloseName, closeNameLength);
                if (err) {
                    gotoErr(err);
                }
                err = pFormat->m_pOutStream->printf(">");
                if (err) {
                    gotoErr(err);
                }
            }
        }
    }

    if (fSuspendingIndentation) {
        pFormat->m_SuspendIndentation = false;
    }


abort:
    returnErr(err);
} // WriteSubTree.






/////////////////////////////////////////////////////////////////////////////
//
// [WriteTextNode]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
WriteTextNode(CPolyXMLNode *pNode, CPrintFormat *pFormat) {
    ErrVal err = ENoErr;
    int32 offsetIntoNode;
    char *pContentPtr;
    int32 actualLength;
    bool fEndOfContent;

    if ((NULL == pNode)
         || (NULL == pFormat)) {
       gotoErr(EFail);
    }

    fEndOfContent = false;
    offsetIntoNode = 0;
    while (!fEndOfContent) {
        err = pNode->GetContentPtr(
                            offsetIntoNode,
                            &pContentPtr,
                            &actualLength,
                            &fEndOfContent);
        if (err) {
            gotoErr(err);
        }

        if (pFormat->m_pOutStream) {
            err = pFormat->m_pOutStream->Write(pContentPtr, actualLength);
            if (err) {
                gotoErr(err);
            }
        }

        offsetIntoNode += actualLength;
    } // while (1)

abort:
    returnErr(err);
} // WriteTextNode.






/////////////////////////////////////////////////////////////////////////////
//
// [WriteElement]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
WriteElement(CPolyXMLNode *pNode, CPrintFormat *pFormat) {
    ErrVal err = ENoErr;
    const char *pNamespace;
    int32 namespaceLength;
    const char *pName;
    int32 nameLength;
    int32 nodeFlags = 0;
    CPolyXMLDocAttribute *pAttribute;
    CPropertyValue *pValue;


    if ((NULL == pNode)
         || (NULL == pFormat)) {
       gotoErr(EFail);
    }

    nodeFlags = pNode->GetNodeFlags();

    pNode->GetNamespace(&pNamespace, &namespaceLength);
    pNode->GetName(&pName, &nameLength);

    if (pFormat->m_pOutStream) {
        err = pFormat->m_pOutStream->PutByte('<');
        if (err) {
            gotoErr(err);
        }
    }
    if (pNode->GetNodeFlags() & CPolyXMLNode::UNBALANCED_CLOSE_ELEMENT) {
        if (pFormat->m_pOutStream) {
            err = pFormat->m_pOutStream->PutByte('/');
            if (err) {
                gotoErr(err);
            }
        }
    }
    if ((NULL != pNamespace) && (namespaceLength > 0)) {
        if (pFormat->m_pOutStream) {
            err = pFormat->m_pOutStream->Write(pNamespace, namespaceLength);
            if (err) {
                gotoErr(err);
            }
            err = pFormat->m_pOutStream->PutByte(':');
            if (err) {
                gotoErr(err);
            }
        }
    }

    if (pFormat->m_pOutStream) {
        err = pFormat->m_pOutStream->Write(pName, nameLength);
        if (err) {
            gotoErr(err);
        }
    }

    // Now, write the attributes.
    pAttribute = pNode->GetFirstAttribute();
    while (NULL != pAttribute) {
        pAttribute->GetNamespace(&pNamespace, &namespaceLength);
        pAttribute->GetName(&pName, &nameLength);
        if (NULL != pName) {
            if (pFormat->m_pOutStream) {
                err = pFormat->m_pOutStream->PutByte(' ');
                if (err) {
                    gotoErr(err);
                }
            }

            if ((NULL != pNamespace) && (namespaceLength > 0)) {
                if (pFormat->m_pOutStream) {
                    err = pFormat->m_pOutStream->Write(pNamespace, namespaceLength);
                    if (err)
                    {
                        gotoErr(err);
                    }
                    err = pFormat->m_pOutStream->PutByte(':');
                    if (err)
                    {
                        gotoErr(err);
                    }
                }
            }

            if (pFormat->m_pOutStream) {
                err = pFormat->m_pOutStream->Write(pName, nameLength);
                if (err) {
                    gotoErr(err);
                }
            }
        } // if (NULL != pName)

        pValue = pAttribute->GetValue();
        if ((NULL != pValue) && (PROPERTY_UNKNOWN != pValue->m_ValueType)) {
            if (pFormat->m_pOutStream) {
                err = pFormat->m_pOutStream->PutByte('=');
                if (err) {
                    gotoErr(err);
                }
            }

            if (pFormat->m_pOutStream) {
                err = PrintValueToAsyncIOStream(
                                    pValue,
                                    pFormat->m_pOutStream,
                                    pFormat->m_pSourceStream);
                if (err) {
                    gotoErr(err);
                }
            }
        }

        pAttribute = pAttribute->GetNextAttribute();
    } // while (NULL != pAttribute)

    // This is the case for shorthand tags like <b<foo>
    if (!(nodeFlags & CPolyXMLNode::ELEMENT_HAS_NO_ENDING_CHAR)) {
        const char *pCloseStr = ">";

        if (nodeFlags & CPolyXMLNode::SELF_CLOSING_ELEMENT) {
            pCloseStr = " />";
        } else if (nodeFlags & CPolyXMLNode::SELF_CLOSING_PROCESSING_INSTRUCTION) {
            pCloseStr = " ?>";
        }

        if (pFormat->m_pOutStream) {
            err = pFormat->m_pOutStream->printf(pCloseStr);
            if (err) {
                gotoErr(err);
            }
        }
    }

abort:
    returnErr(err);
} // WriteElement.







#if DEBUG_XML
/////////////////////////////////////////////////////////////////////////////
//
// [GetNodeWrittenAtPosition]
//
/////////////////////////////////////////////////////////////////////////////
CPolyXMLNode *
GetNodeWrittenAtPosition(CPolyXMLNode *pNode, int64 position) {
    CPolyXMLNode *pChild;
    CPolyXMLNode *pMatchingNode;

    if (NULL == pNode) {
       return(NULL);
    }

    if ((position >= pNode->m_StartWritePos)
        && (position < pNode->m_StopWritePos)) {
        return(pNode);
    }

    pChild = pNode->GetFirstChild();
    while (NULL != pChild) {
        pMatchingNode = GetNodeWrittenAtPosition(pChild, position);
        if (pMatchingNode) {
            return(pMatchingNode);
        }

        pChild = pChild->GetNextSibling();
    } // while (NULL != pChild)

    return(NULL);
} // GetNodeWrittenAtPosition.
#endif // DEBUG_XML







/////////////////////////////////////////////////////////////////////////////
//
// [GetChildStringValue]
//
/////////////////////////////////////////////////////////////////////////////
char *
CPolyXMLNode::GetChildStringValue(const char *pName, const char *pDefaultVal) {
    char *pResult = NULL;
    char buffer[1024];
    int bufferLength;

    bufferLength = GetChildElementValue(pName, buffer, sizeof(buffer));
    if (bufferLength > 0) {
        pResult = strdupex(buffer);
    } else {
        if (NULL != pDefaultVal) {
            pResult = strdupex(pDefaultVal);
        }
    }

    return(pResult);
} // GetChildStringValue






/////////////////////////////////////////////////////////////////////////////
//
// [GetChildIntegerValue]
//
/////////////////////////////////////////////////////////////////////////////
int32
CPolyXMLNode::GetChildIntegerValue(const char *pName, int32 defaultVal) {
    ErrVal err = ENoErr;
    int32 result = defaultVal;
    char buffer[1024];
    int bufferLength;

    bufferLength = GetChildElementValue(pName, buffer, sizeof(buffer));
    if (bufferLength > 0) {
        err = CStringLib::StringToNumber(buffer, bufferLength, &result);
        if (err) {
           result = defaultVal;
        }
    }

    return(result);
} // GetChildIntegerValue







/////////////////////////////////////////////////////////////////////////////
//
// [PrintValueToAsyncIOStream]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
PrintValueToAsyncIOStream(
                CPropertyValue *pValue,
                CAsyncIOStream *pOutStream,
                CAsyncIOStream *pSrcStream) {
    ErrVal err = ENoErr;
    char *pPtr;
    int32 length;
    int64 streamOffset;
    char TempBuffer[32];


    if ((NULL == pValue) || (NULL == pOutStream)) {
        gotoErr(EFail);
    }


    switch (pValue->m_ValueType) {
    ///////////////////////////////////
    case PROPERTY_INTEGER:
        err = pOutStream->printf("%d", pValue->m_Value.m_IntValue);
        if (err) {
            gotoErr(err);
        }
        break;

    ///////////////////////////////////
    case PROPERTY_BOOL:
        if (pValue->m_Value.m_BoolValue) {
            err = pOutStream->printf("TRUE");
        } else {
            err = pOutStream->printf("FALSE");
        }
        if (err) {
            gotoErr(err);
        }
        break;

    ///////////////////////////////////
    case PROPERTY_FLOAT:
        ::snprintf(TempBuffer, sizeof(TempBuffer), "%f", pValue->m_Value.m_FloatValue);
        err = pOutStream->printf("%s", TempBuffer);
        if (err) {
            gotoErr(err);
        }
        break;

    ///////////////////////////////////
    case PROPERTY_STRING:
        err = pValue->GetStringValue(&pPtr, &length);
        if (err) {
            gotoErr(err);
        }

        err = pOutStream->Write(pPtr, length);
        if (err) {
            gotoErr(err);
        }
        break;

    ///////////////////////////////////
    case PROPERTY_INT64:
        ::snprintf(TempBuffer, sizeof(TempBuffer), INT64FMT, pValue->m_Value.m_Int64Value);
        err = pOutStream->printf("%s", TempBuffer);
        if (err) {
            gotoErr(err);
        }
        break;

    ///////////////////////////////////
    case PROPERTY_RANGE:
        if (NULL == pSrcStream) {
            gotoErr(EFail);
        }

        err = pValue->GetRangeValue(&streamOffset, &length);
        if (err) {
            gotoErr(err);
        }

        err = pSrcStream->SetPosition(streamOffset);
        if (err) {
            gotoErr(err);
        }

        err = pSrcStream->CopyStream(pOutStream, length, false);
        if (err) {
            gotoErr(err);
        }
        break;

    ///////////////////////////////////
    case PROPERTY_BUFFER: // TBD
    case PROPERTY_UNKNOWN:
    default:
        gotoErr(EFail);
    } // switch (pValue->m_ValueType)

abort:
    returnErr(err);
} // PrintValueToAsyncIOStream







/////////////////////////////////////////////////////////////////////////////
//
//                      TESTING PROCEDURES
//
/////////////////////////////////////////////////////////////////////////////
#if INCLUDE_REGRESSION_TESTS


static CSynchPolyHttpCallback *g_TestHTTPCallback;
static CAsyncIOEventHandlerSynch *g_TestFileCallback;

static char g_DumpFileName[1024];
static char g_OrigFileName[1024];
static char g_DataFileName[1024];

void TestOneHTTPURL(const char *urlStr);
void TestOneFileURL(char *pFilePath);

ErrVal CheckRestoredXMLDocument(
                CPolyXMLDoc *pDoc,
                CAsyncIOStream *pRegeneratedStream);

ErrVal SaveOriginalDocument(
                CPolyXMLDoc *pDoc,
                char *pFileName);

void ReportDocMismatch(
                CPolyXMLDoc *pDoc,
                CAsyncIOStream *pOriginalStream,
                CAsyncIOStream *pRegeneratedStream);




/////////////////////////////////////////////////////////////////////////////
//
// [TestXMLModule]
//
/////////////////////////////////////////////////////////////////////////////
void
CPolyXMLDoc::TestXMLModule() {
    ErrVal err = ENoErr;

    g_DebugManager.StartModuleTest("XML Document Module");


    g_TestHTTPCallback = newex CSynchPolyHttpCallback;
    if (NULL == g_TestHTTPCallback) {
        return;
    }
    err = g_TestHTTPCallback->Initialize();
    if (err) {
        return;
    }

    g_TestFileCallback = newex CAsyncIOEventHandlerSynch;
    if (NULL == g_TestFileCallback) {
        return;
    }
    err = g_TestFileCallback->Initialize();
    if (err) {
        return;
    }


    g_DebugManager.AddTestResultsDirectoryPath("xmlRecreated.txt", g_DumpFileName, 1024);
    g_DebugManager.AddTestResultsDirectoryPath("xmlOriginal.txt", g_OrigFileName, 1024);

    DontCountAllCurrentAllocations();

    // EXMLParseError is a bad error when we test this module.
    // Normally, it isn't treated like a bug because we assume
    // some server gave us a malformed page. Inthis test, however,
    // is may mean there isa bug in the parser.
    CDebugManager::g_AdditionalPossibleBugError = EXMLParseError;

    // Test a page that has everything in it.
    g_DebugManager.AddTestDataDirectoryPath("xmlBenchmarkPage.html", g_DataFileName, 1024);
    g_DebugManager.StartTest(g_DataFileName);
    TestOneFileURL(g_DataFileName);

#if 0
    // Now, test all live online pages.
    for (int32 index = 0; ; index++) {
        const char *pTestUrl;
        pTestUrl = g_TestURLList[index];
        if (NULL == pTestUrl) {
            break;
        }

        g_DebugManager.StartTest(pTestUrl);
        TestOneHTTPURL(pTestUrl);
    } // running each test.
#endif

    CDebugManager::g_AdditionalPossibleBugError = ENoErr;
} // TestXMLModule.





/////////////////////////////////////////////////////////////////////////////
//
// [TestOneHTTPURL]
//
/////////////////////////////////////////////////////////////////////////////
void
TestOneHTTPURL(const char *urlStr) {
    ErrVal err = ENoErr;
    CPolyXMLDoc *pXMLDoc = NULL;
    CAsyncIOStream *pRegeneratedStream = NULL;
    CParsedUrl *pUrl = NULL;
    CPolyHttpStream *pHTTPStream = NULL;

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

    // Load the document.
    pHTTPStream->ReadHTTPDocument(pUrl, g_TestHTTPCallback, NULL);
    err = g_TestHTTPCallback->Wait();
    if (err) {
        if ((ENoResponse == err)
            || (EEOF == err)
            || (ENoHostAddress == err)
            || (EHTTPSRequired == err)) {
            OSIndependantLayer::PrintToConsole(CDebugManager::GetErrorDescriptionString(err));
        } else {
            DEBUG_WARNING("Error from ReadHTTPDocument.");
        }
        gotoErr(err);
    }


    // Parse the html data.
    err = OpenSimpleXMLFromHTTP(pHTTPStream, &pXMLDoc);
    if (err) {
        gotoErr(err);
    }

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
                                g_TestFileCallback,
                                NULL);
    if (err) {
        gotoErr(err);
    }
    err = g_TestFileCallback->Wait();
    if (err) {
        gotoErr(err);
    }
    // Write the document. This regenerates the page.
    pRegeneratedStream = g_TestFileCallback->m_pAsyncIOStream;
    g_TestFileCallback->m_pAsyncIOStream = NULL;

    err = pXMLDoc->WriteDocToStream(pRegeneratedStream, 0);
    if (err) {
        gotoErr(err);
    }

    pRegeneratedStream->Flush();
    err = g_TestFileCallback->Wait();
    if (err) {
        gotoErr(err);
    }

    err = CheckRestoredXMLDocument(pXMLDoc, pRegeneratedStream);
    if (err) {
        gotoErr(err);
    }

abort:
    if (pRegeneratedStream) {
        pRegeneratedStream->Close();
    }
    RELEASE_OBJECT(pRegeneratedStream);
    RELEASE_OBJECT(pUrl);
    RELEASE_OBJECT(pXMLDoc);
} // TestOneHTTPURL.








/////////////////////////////////////////////////////////////////////////////
//
// [TestOneFileURL]
//
/////////////////////////////////////////////////////////////////////////////
void
TestOneFileURL(char *pFilePath) {
    ErrVal err = ENoErr;
    CPolyXMLDoc *pXMLDoc = NULL;
    CAsyncIOStream *pRegeneratedStream = NULL;
    CParsedUrl *pUrl = NULL;

    err = OpenSimpleXMLFile(pFilePath, &pXMLDoc);
    if (err) {
        gotoErr(err);
    }

    pUrl = CParsedUrl::AllocateFileUrl(g_DumpFileName);
    if (NULL == pUrl) {
        gotoErr(EFail);
    }

    err = CAsyncIOStream::OpenAsyncIOStream(
                                pUrl,
                                CAsyncBlockIO::CREATE_NEW_STORE
                                    | CAsyncBlockIO::READ_ACCESS
                                    | CAsyncBlockIO::WRITE_ACCESS,
                                g_TestFileCallback,
                                NULL);
    if (err) {
        gotoErr(err);
    }
    err = g_TestFileCallback->Wait();
    if (err) {
        gotoErr(err);
    }
    // Write the document. This regenerates the page.
    pRegeneratedStream = g_TestFileCallback->m_pAsyncIOStream;
    g_TestFileCallback->m_pAsyncIOStream = NULL;

    err = pXMLDoc->WriteDocToStream(pRegeneratedStream, 0);
    if (err) {
        gotoErr(err);
    }
    pRegeneratedStream->Flush();
    err = g_TestFileCallback->Wait();
    if (err) {
        gotoErr(err);
    }

    err = CheckRestoredXMLDocument(pXMLDoc, pRegeneratedStream);
    if (err) {
        gotoErr(err);
    }
    pRegeneratedStream->Close();

abort:
    RELEASE_OBJECT(pXMLDoc);
    RELEASE_OBJECT(pRegeneratedStream);
    RELEASE_OBJECT(pUrl);
} // TestOneFileURL.







/////////////////////////////////////////////////////////////////////////////
//
// [CheckRestoredXMLDocument]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CheckRestoredXMLDocument(
                    CPolyXMLDoc *pDoc,
                    CAsyncIOStream *pRegeneratedStream) {
    ErrVal err = ENoErr;
    char c1;
    char c2;
    CAsyncIOStream *pOriginalStream = NULL;
    int64 startPosition;
    int32 contentLength;


    SaveOriginalDocument(pDoc, g_OrigFileName);

    err = pDoc->GetIOStream(
                        &pOriginalStream,
                        &startPosition,
                        &contentLength);
    if (err) {
        gotoErr(err);
    }
    if (NULL == pOriginalStream) {
        gotoErr(EFail);
    }

    err = pRegeneratedStream->SetPosition(0);
    if (err) {
        gotoErr(err);
    }


    // Compare characters. Check for whitespace.
    while (1) {
        err = pOriginalStream->GetByte(&c1);
        if (err) {
            if (EEOF == err) {
                err = ENoErr;
            } else {
                DEBUG_WARNING("Error while reading body.");
            }

            break;
        }

        while (CStringLib::IsByte(c1, CStringLib::WHITE_SPACE_CHAR)) {
            err = pOriginalStream->GetByte(&c1);
            if (err) {
                if (EEOF == err) {
                    err = ENoErr;
                    goto EndOfRegeneratedFile;
                } else {
                    DEBUG_WARNING("Error while reading body.");
                }
                break;
            }
        }


        err = pRegeneratedStream->GetByte(&c2);
        if (err) {
            DEBUG_WARNING("Error while reading body.");
            break;
        }

        while (CStringLib::IsByte(c2, CStringLib::WHITE_SPACE_CHAR)) {
            err = pRegeneratedStream->GetByte(&c2);
            if (err) {
                DEBUG_WARNING("Error while reading body.");
                break;
            }
        }

        // Ignore case for the built-in element names. These are all ASCII.
        if ((c1 >= 'a') && (c1 <= 'z')) {
            c1 = c1 - ('a' - 'A');
        }
        if ((c2 >= 'a') && (c2 <= 'z')) {
            c2 = c2 - ('a' - 'A');
        }
        if (c1 != c2) {
            ReportDocMismatch(pDoc, pOriginalStream, pRegeneratedStream);
        }
    }

EndOfRegeneratedFile:
    // Make sure the writestream is not a superset of the original stream.
    err = pRegeneratedStream->SkipWhileCharType(CStringLib::WHITE_SPACE_CHAR);
    if (EEOF == err) {
        err = ENoErr;
    }
    if (err) {
        DEBUG_WARNING("Error while reading body.");
        gotoErr(err);
    }

    while (1) {
        err = pRegeneratedStream->GetByte(&c2);
        if (err) {
            if (EEOF == err) {
                break;
            } else {
                DEBUG_WARNING("Printed stream is shorter than the original stream.");
            }

        } else if (('>' != c2)
                && !(CStringLib::IsByte(c2, CStringLib::WHITE_SPACE_CHAR))) {
            ReportDocMismatch(pDoc, pOriginalStream, pRegeneratedStream);
            DEBUG_WARNING("The pRegeneratedStream is not a superset of the original stream.");
        }
    }

abort:
    RELEASE_OBJECT(pOriginalStream);

    if (EEOF == err) {
        err = ENoErr;
    }

    return(err);
} // CheckRestoredXMLDocument.





/////////////////////////////////////////////////////////////////////////////
//
// [SaveOriginalDocument]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
SaveOriginalDocument(CPolyXMLDoc *pDoc, char *pFileName) {
    ErrVal err = ENoErr;
    CAsyncIOStream *pSourceStream = NULL;
    CAsyncIOStream *pWriteStream = NULL;
    CParsedUrl *pUrl = NULL;
    int64 startPosition;
    int32 contentLength;

    err = pDoc->GetIOStream(
                        &pSourceStream,
                        &startPosition,
                        &contentLength);
    if (err) {
        gotoErr(err);
    }
    if (NULL == pSourceStream) {
        gotoErr(EFail);
    }

    pUrl = CParsedUrl::AllocateFileUrl(pFileName);
    if (NULL == pUrl) {
        gotoErr(EFail);
    }

    err = CAsyncIOStream::OpenAsyncIOStream(
                                pUrl,
                                CAsyncBlockIO::CREATE_NEW_STORE
                                    | CAsyncBlockIO::READ_ACCESS
                                    | CAsyncBlockIO::WRITE_ACCESS,
                                g_TestFileCallback,
                                NULL);
    if (err) {
        gotoErr(err);
    }

    err = g_TestFileCallback->Wait();
    if (err) {
        gotoErr(err);
    }
    pWriteStream = g_TestFileCallback->m_pAsyncIOStream;
    g_TestFileCallback->m_pAsyncIOStream = NULL;


    err = pSourceStream->CopyStream(pWriteStream, CAsyncIOStream::COPY_TO_EOF, false);
    if (err) {
        gotoErr(err);
    }

    pWriteStream->Flush();
    err = g_TestFileCallback->Wait();
    if (err) {
        gotoErr(err);
    }

    pWriteStream->Close();

abort:
    RELEASE_OBJECT(pSourceStream);
    RELEASE_OBJECT(pWriteStream);
    RELEASE_OBJECT(pUrl);

    returnErr(err);
} // SaveOriginalDocument








/////////////////////////////////////////////////////////////////////////////
//
// [ReportDocMismatch]
//
/////////////////////////////////////////////////////////////////////////////
void
ReportDocMismatch(
            CPolyXMLDoc *pDoc,
            CAsyncIOStream *pOriginalStream,
            CAsyncIOStream *pRegeneratedStream) {
    char *pOriginalStr = NULL;
    char *pRecreatedStr = NULL;
    int64 readCharNum = 0;
    int64 writeCharNum = 0;
    char tempBuffer[16];
    UNUSED_PARAM(pDoc);

    readCharNum = pOriginalStream->GetPosition();
    writeCharNum = pRegeneratedStream->GetPosition();

    //pOriginalTag = pDoc->GetTagReadAtPosition(pOriginalStream->GetPosition() - 1);
    //writeTag = pDoc->GetTagWrittenAtPosition(pRegeneratedStream->GetPosition() - 1);

    (void) pOriginalStream->GetPtr(readCharNum, 1, tempBuffer, 2, &pOriginalStr);
    (void) pRegeneratedStream->GetPtr(writeCharNum, 1, tempBuffer, 2, &pRecreatedStr);
    pOriginalStr--;
    pRecreatedStr--;

#if DEBUG_XML
    CPolyXMLNode *writeTag = NULL;
    writeTag = GetNodeWrittenAtPosition(pDoc->GetRoot(), writeCharNum);
    writeTag = writeTag; // Only used inside a debugger.
#endif // DEBUG_XML
    
    DEBUG_WARNING("Mismatched body.");
} // ReportDocMismatch



#endif // INCLUDE_REGRESSION_TESTS





