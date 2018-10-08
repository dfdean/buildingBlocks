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
// PolyXMLDoc Text Module
//
// This reads an XML document from some text, either from a file, or a string,
// or read through an HTTP request.
/////////////////////////////////////////////////////////////////////////////

#include "buildingBlocks.h"

FILE_DEBUGGING_GLOBALS(LOG_LEVEL_DEFAULT, 0);

class CSimpleXMLDoc;
class CSimpleXMLNode;


// These are the built-in names.
// We pre-define names for 2 reasons:
//
// 1. Some elements are found in almost any document, like <XML> in an XML document
//   or <HTML>, <HEAD>, <BODY> in an html document. It is just silly to define them
//   new for each document.
//
// 2. Some elements have special properties, so we set those in this table.
//
static CNameTable *m_pGlobalNameList = NULL;
static const char *g_CommonBuiltInNames[] = {
    "#document",
    "#text",
    "#cdata-section",
    "#comment",
    "#document-fragment",
    "xml",
    "html",
    "head",
    "title",
    "body",
    "a",
    "img",
    "meta",
    "font",
    "table",
    NULL
};

enum {
   DONT_EXPAND_REF_CHARS   = 0,
};


class CEntityValue {
public:
   const char *pName;
   char value;
}; // CEntityValue

/////////////////////////////////////////////////////////////////////////////
// This should be filled in from http://www.w3.org/TR/REC-html32
// See the section "Character Entities for ISO Latin-1"
CEntityValue g_EntityNames[] = {
    { "amp", '&' },     // ampersand
    { "copy", '\xA9' }, // copyright symbol
    { "gt", '>' },      // greater than
    { "lt", '<' },      // less than
    { "nbsp", '\xA0' }, // non-breaking space
    { "quot", '\"' },   // double quote
    { "reg", '\xAE' },  // registered trademark
    { "shy", '\xAD' },  // soft hyphen
    { "apos", '\'' },   // apostrophe
    { NULL, '\0' }
}; // g_EntityNames



/////////////////////////////////////////////////////////////////////////////
// This describes one XML document.
/////////////////////////////////////////////////////////////////////////////
class CParseState {
public:
    enum {
        MAX_ELEMENT_STACK_DEPTH = 64 * 1024,
    };

    // This is parsing state. It is only allocated while we parse a doc.
    CSimpleXMLNode      *m_pStack[MAX_ELEMENT_STACK_DEPTH];
    int32               m_StackDepth;

    CDictionaryEntry    *m_CDataNodeName; // "#cdata-section";
    CDictionaryEntry    *m_CommentNodeName; // "#comment";
}; // CParseState





/////////////////////////////////////////////////////////////////////////////
class CXMLNamespace {
public:
    CXMLNamespace();
    ~CXMLNamespace();
    NEWEX_IMPL()

    CDictionaryEntry    *m_pName;

    char                *m_pUrl;

    CSimpleXMLNode      *m_pDeclarationElement;

    CXMLNamespace       *m_pNextNamespace;
}; // CXMLNamespace





/////////////////////////////////////////////////////////////////////////////
// This describes one XML attribute.
// It implements the abstract CPolyXMLDocAttribute class with a simple
// property value.
/////////////////////////////////////////////////////////////////////////////
class CSimpleXMLAttribute : public CPolyXMLDocAttribute,
                            public CRefCountImpl {
public:
    CSimpleXMLAttribute();
    virtual ~CSimpleXMLAttribute() { }
    NEWEX_IMPL()

    // CRefCountInterface
    PASS_REFCOUNT_TO_REFCOUNTIMPL()

    // CPolyXMLDocAttribute
    virtual CPropertyValue *GetValue() { return(&m_Value); }

    // Tree Navigation
    virtual CPolyXMLNode *GetFirstChild() { ASSERT(0); return(NULL); }
    virtual CPolyXMLNode *GetNextSibling() { ASSERT(0); return(NULL); }
    virtual CPolyXMLNode *GetParent() { ASSERT(0); return(NULL); }
    virtual CPolyXMLNode *GetNamedChild(const char *pName) { UNUSED_PARAM(pName); ASSERT(0); return(NULL); }
    virtual CPolyXMLNode *GetNamedSibling(const char *pName) { UNUSED_PARAM(pName); ASSERT(0); return(NULL); }
    virtual CPolyXMLNode *GetChildByType(int32 childType) { childType = childType; ASSERT(0); return(NULL); }

    // Editing
    virtual CPolyXMLNode *AddNamedChild(const char *pName) { UNUSED_PARAM(pName); ASSERT(0); return(NULL); }
    virtual int32 GetChildElementValue(const char *pName, char *pBuffer, int32 maxBufferLength) { 
        UNUSED_PARAM(pName); UNUSED_PARAM(pBuffer); UNUSED_PARAM(maxBufferLength); ASSERT(0); return(-1); }
    virtual ErrVal SetChildElementValue(const char *pName, char *pBuffer, int32 bufferLength) { 
        UNUSED_PARAM(pName); UNUSED_PARAM(pBuffer); UNUSED_PARAM(bufferLength); ASSERT(0); return(EFail); }

    // Properties of this node.
    virtual CPolyXMLNode::XMLNodeType GetNodeType() { return(XML_NODE_ATTRIBUTE); }
    virtual int32 GetNodeFlags() { return(m_AttrFlags); }
    virtual void GetNamespace(const char **ppName, int32 *pNameLength) {
            if ((NULL == m_pNamespace) || (NULL == m_pNamespace->m_pName)) {
                if (NULL != ppName) { *ppName = NULL; }
                if (NULL != pNameLength) { *pNameLength = 0; }
                return;
            }
            if (NULL != ppName) { *ppName = m_pNamespace->m_pName->m_pName; }
            if (NULL != pNameLength) { *pNameLength = m_pNamespace->m_pName->m_NameLength; }
        }
    virtual void GetName(const char **ppName, int32 *pNameLength) {
            if (NULL == m_pName) {
                if (NULL != ppName) { *ppName = NULL; }
                if (NULL != pNameLength) { *pNameLength = 0; }
                return;
            }
            if (NULL != ppName) { *ppName = m_pName->m_pName; }
            if (NULL != pNameLength) { *pNameLength = m_pName->m_NameLength; }
        }
    virtual const char *GetSpecialCloseName() { return(NULL); }
    virtual CPolyXMLDocAttribute *GetFirstAttribute() { ASSERT(0); return(NULL); }
    virtual ErrVal GetContentPtr(
                        int32 offsetIntoNode,
                        char **ppContentPtr,
                        int32 *pActualLength,
                        bool *pEndOfContent) { 
        UNUSED_PARAM(offsetIntoNode); UNUSED_PARAM(ppContentPtr); UNUSED_PARAM(pActualLength);
        UNUSED_PARAM(pEndOfContent); return(EFail); }

    virtual CPolyXMLDocAttribute *GetNextAttribute() { return(m_pNextAttribute); }

    int32               m_AttrFlags;

    CDictionaryEntry         *m_pName;
    CXMLNamespace       *m_pNamespace;

    int64               m_ValuePosition;
    int32               m_ValueSize;

    CPropertyValue      m_Value;

    CSimpleXMLAttribute *m_pNextAttribute;
}; // CSimpleXMLAttribute








/////////////////////////////////////////////////////////////////////////////
// This describes one XML node.
// It implements the abstract CPolyXMLNode class.
/////////////////////////////////////////////////////////////////////////////
class CSimpleXMLNode : public CPolyXMLNode,
                        public CRefCountImpl {
public:
    CSimpleXMLNode();
    ~CSimpleXMLNode();
    NEWEX_IMPL()

    // CRefCountInterface
    PASS_REFCOUNT_TO_REFCOUNTIMPL()

    // Tree Navigation
    virtual CPolyXMLNode *GetFirstChild() { return(m_pFirstChild); }
    virtual CPolyXMLNode *GetNextSibling() { return(m_pNextSibling); }
    virtual CPolyXMLNode *GetParent() { return(m_pParentNode); }
    virtual CPolyXMLNode *GetNamedChild(const char *pName);
    virtual CPolyXMLNode *GetNamedSibling(const char *pName);
    virtual CPolyXMLNode *GetChildByType(int32 childType);

    // Editing
    virtual CPolyXMLNode *AddNamedChild(const char *pName);
    virtual int32 GetChildElementValue(const char *pName, char *pBuffer, int32 maxBufferLength);
    virtual ErrVal SetChildElementValue(const char *pName, char *pBuffer, int32 bufferLength);

    // Properties of this node.
    virtual CPolyXMLNode::XMLNodeType GetNodeType() { return(m_NodeType); }
    virtual int32 GetNodeFlags() { return(m_NodeFlags); }
    virtual void GetNamespace(const char **ppName, int32 *pNameLength) {
                    if ((NULL == m_pNamespace) || (NULL == m_pNamespace->m_pName))
                    {
                        if (NULL != ppName) { *ppName = NULL; }
                        if (NULL != pNameLength) { *pNameLength = 0; }
                        return;
                    }
                    if (NULL != ppName) { *ppName = m_pNamespace->m_pName->m_pName; }
                    if (NULL != pNameLength) { *pNameLength = m_pNamespace->m_pName->m_NameLength; }
                }
    virtual void GetName(const char **ppName, int32 *pNameLength) {
                    if (NULL == m_pName)
                    {
                        if (NULL != ppName) { *ppName = NULL; }
                        if (NULL != pNameLength) { *pNameLength = 0; }
                        return;
                    }
                    if (NULL != ppName) { *ppName = m_pName->m_pName; }
                    if (NULL != pNameLength) { *pNameLength = m_pName->m_NameLength; }
                    }
    virtual const char *GetSpecialCloseName() {
        if (NULL == m_pName) { return(NULL); }
        return(m_pName->m_pSpecialCloseName); }
    virtual CPolyXMLDocAttribute *GetFirstAttribute() { return(m_NodeBody.Element.m_pAttributeList); }
    virtual ErrVal GetContentPtr(
                        int32 offsetIntoNode,
                        char **ppContentPtr,
                        int32 *pActualLength,
                        bool *pEndOfContent);

#if DEBUG_XML
    virtual void SetDebugMode(int opCode) { UNUSED_PARAM(opCode); ParseAttributes(); }
#endif // DEBUG_XML

    // CSimpleXMLNode
    void InsertLastChild(CSimpleXMLNode *pChildNode);


    // These are the state transitions when we read the attributes
    // of an element.
    enum {
        READING_WHITESPACE,
        READING_NAME,
        READING_VALUE,
        READING_QUOTED_VALUE,
    };

    // Private flags for m_NodeFlags.
    enum {
      CAN_DELETE_TAG_BODY    = 0x8000,
    };


    XMLNodeType         m_NodeType;
    int16               m_NodeFlags;

    int64               m_StartPosition;
    int32               m_NodeSize;

    CSimpleXMLNode      *m_pFirstChild;
    CSimpleXMLNode      *m_pNextSibling;
    CSimpleXMLNode      *m_pParentNode;

    CSimpleXMLDoc       *m_pParentDocument;

    CDictionaryEntry    *m_pName;
    CXMLNamespace       *m_pNamespace;

    /////////////////////////////////
    union {
        // Used by XML_NODE_TEXT, XML_NODE_COMMENT, XML_NODE_CDATA
        struct CXMLTextBody {
            char                 *m_pText;
        } Text;

        // Used by XML_NODE_ELEMENT, XML_NODE_ATTRIBUTE
        struct CXMLElementBody {
            int32                m_OffsetToName;
            CSimpleXMLAttribute  *m_pAttributeList;
        } Element;
    } m_NodeBody;

    ErrVal ParseAttributes();
    ErrVal AddAttribute(
                char *pName,
                int32 nameLength,
                CSimpleXMLAttribute **ppResultAttribute,
                CSimpleXMLAttribute **ppLastAttribute);
}; // CSimpleXMLNode





/////////////////////////////////////////////////////////////////////////////
// This describes one XML document.
// It implements the abstract CPolyXMLNode class.
/////////////////////////////////////////////////////////////////////////////
class CSimpleXMLDoc : public CPolyXMLDoc,
                        public CRefCountImpl {
public:
    CSimpleXMLDoc();
    virtual ~CSimpleXMLDoc();
    NEWEX_IMPL()

    // CRefCountInterface
    PASS_REFCOUNT_TO_REFCOUNTIMPL()

    // CPolyXMLDoc
    virtual ErrVal GetIOStream(
                        CAsyncIOStream **ppSrcStream,
                        int64 *pStartPosition,
                        int32 *pLength);
    virtual CPolyXMLNode *GetRoot() { return(m_pRootNode); }

    ErrVal InternalInitialize();
    ErrVal ReadXMLBuffer(
                        char *pTextBuffer,
                        int64 startOffset,
                        int32 contentLength);
    ErrVal ReadStreamImpl(
                        CAsyncIOStream *pAsyncIOStream,
                        int64 startPos,
                        int32 length);

private:
    friend class CSimpleXMLNode;

    // CSimpleXMLDoc
    ErrVal InitializeParseState();

    virtual ErrVal CheckState();

    ErrVal FirstLexicalScan();
    ErrVal FirstLexicalScanOneElement(int32 openCharLength);
    ErrVal SkipUnformattedText(CSimpleXMLNode *pParentNode);
    ErrVal SkipCDATA(CSimpleXMLNode *pNode);
    ErrVal ReadCommentNode(CSimpleXMLNode *pNode);

    ErrVal FindNamespaceDeclarations(CSimpleXMLNode *pNode);
    ErrVal DeclareNamespaces(CSimpleXMLNode *pNode);
    CXMLNamespace *GetNamespace(char *pName, int32 nameLength);

    ErrVal InsertNewNodeIntoTree(CSimpleXMLNode *pNode);
    ErrVal CloseNode(CDictionaryEntry *pName, int64 startPosition);

    ErrVal GetXMLChar(
               int referenceMode,
               char *pBuffer,
               int32 maxCharLen,
               int *pCharLen);

    void FlattenNodesWithNoChildren(int32 topIndex);

    ErrVal AddUnbalancedCloseNode(CDictionaryEntry *pName, int64 startPosition);

    CSimpleXMLNode          *m_pRootNode;

    CNameTable              *m_pNameList;
    CDictionaryEntry        *m_TextNodeName; // "#text";
    CDictionaryEntry        *m_DocumentNodeName; // "#document";
    CDictionaryEntry        *m_NamespaceDeclarationName; // "xmlns";

    bool                    m_fEnforceStrictXML;

    // This is used when we read from a stream, like an HTTP stream.
    CAsyncIOStream          *m_pAsyncIOStream;
    int64                   m_StartDocPosition;
    int32                   m_ContentLength;

    // All namespaces defined for this document.
    CXMLNamespace           *m_pNamespaceList;
    CXMLNamespace           *m_pNamespaceNamespace;

    // This is parsing state. It is only allocated while we parse a doc.
    CParseState             *m_pParseState;
}; // CSimpleXMLDoc





/////////////////////////////////////////////////////////////////////////////
enum PrivateFlags {
    // Attributes
    CAN_DELETE_ATTR_VALUE           = 0x1000,
}; // PrivateFlags


static const char *g_CloseCommentStr = "-->";
static const int32 g_CloseCommentStrLength = 3;
static const char *g_CloseElementStr = ">";
static const int32 g_CloseElementStrLength = 1;

static const char *g_XMLNamespaceDeclarationPrefix = "xmlns";
static const int32 g_XMLNamespaceDeclarationPrefixLength = 5;

#if DEBUG_XML
extern int32 g_NextDebugNodeId;
extern int32 g_BuggyNode;
#endif // DEBUG_XML






/////////////////////////////////////////////////////////////////////////////
//
// [OpenSimpleXMLFile]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
OpenSimpleXMLFile(const char *pFileName, CPolyXMLDoc **ppResult) {
    ErrVal err = ENoErr;
    CSimpleXMLDoc *pDoc = NULL;
    char *pTextBuffer = NULL;
    int32 contentLength;

    if (NULL == ppResult) {
        gotoErr(EFail);
    }
    *ppResult = NULL;
    if (NULL == pFileName) {
        gotoErr(EFail);
    }

    pDoc = newex CSimpleXMLDoc;
    if (NULL == pDoc) {
        gotoErr(EFail);
    }

    err = pDoc->InternalInitialize();
    if (err) {
        gotoErr(err);
    }


    err = CFileUtils::ReadFileToBuffer(pFileName, &pTextBuffer, &contentLength);
    if (err) {
        gotoErr(err);
    }

    err = pDoc->ReadXMLBuffer(pTextBuffer, 0, contentLength);
    if (err) {
        gotoErr(err);
    }

    *ppResult = pDoc;
    pDoc = NULL;
    pTextBuffer = NULL;

abort:
    RELEASE_OBJECT(pDoc);
    memFree(pTextBuffer);

    returnErr(err);
} // OpenSimpleXMLFile




/////////////////////////////////////////////////////////////////////////////
//
// [ParseSimpleXMLBuffer]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
ParseSimpleXMLBuffer(char *pTextBuffer, int32 bufferLength, CPolyXMLDoc **ppResult) {
    ErrVal err = ENoErr;
    CSimpleXMLDoc *pDoc = NULL;

    if ((NULL == pTextBuffer) 
            || (NULL == ppResult)) {
        gotoErr(EFail);
    }
    *ppResult = NULL;

    pDoc = newex CSimpleXMLDoc;
    if (NULL == pDoc) {
        gotoErr(EFail);
    }
    err = pDoc->InternalInitialize();
    if (err) {
        gotoErr(err);
    }

    if (bufferLength < 0) {
        bufferLength = strlen(pTextBuffer);
    }
    err = pDoc->ReadXMLBuffer(pTextBuffer, 0, bufferLength);
    if (err) {
        gotoErr(err);
    }

    *ppResult = pDoc;
    pDoc = NULL;

abort:
    RELEASE_OBJECT(pDoc);

    returnErr(err);
} // ParseSimpleXMLBuffer









/////////////////////////////////////////////////////////////////////////////
//
// [OpenSimpleXMLFromHTTP]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
OpenSimpleXMLFromHTTP(CPolyHttpStream *pHTTPStream, CPolyXMLDoc **ppResult) {
    ErrVal err = ENoErr;
    CSimpleXMLDoc *pDoc = NULL;
    CAsyncIOStream *pAsyncIOStream = NULL;
    int64 startPosition;
    int32 contentLength;


    if (NULL == ppResult) {
        err = EFail;
        goto abort;
    }
    *ppResult = NULL;


    if (NULL == pHTTPStream) {
        err = EFail;
        goto abort;
    }

    pDoc = newex CSimpleXMLDoc;
    if (NULL == pDoc) {
        err = EFail;
        goto abort;
    }
    err = pDoc->InternalInitialize();
    if (err) {
        gotoErr(err);
    }



    pDoc->m_pUrl = pHTTPStream->GetUrl();

    // Parse the html data.
    err = pHTTPStream->GetIOStream(
                            &pAsyncIOStream,
                            &startPosition,
                            &contentLength);
    if (err) {
        gotoErr(err);
    }

    err = pDoc->ReadStreamImpl(pAsyncIOStream, startPosition, contentLength);
    if (err) {
        gotoErr(err);
    }

    *ppResult = pDoc;
    pDoc = NULL;

abort:
    RELEASE_OBJECT(pDoc);

    returnErr(err);
} // OpenSimpleXMLFromHTTP








/////////////////////////////////////////////////////////////////////////////
//
// [CSimpleXMLDoc]
//
/////////////////////////////////////////////////////////////////////////////
CSimpleXMLDoc::CSimpleXMLDoc() {
    m_fEnforceStrictXML = false;

    m_pAsyncIOStream = NULL;

    m_pRootNode = NULL;

    m_pNameList = NULL;
    m_TextNodeName = NULL;
    m_DocumentNodeName = NULL;
    m_NamespaceDeclarationName = NULL;

    m_ContentLength = 0;
    m_StartDocPosition = 0;

    m_pNamespaceList = NULL;

    m_pParseState = NULL;
} // CSimpleXMLDoc







/////////////////////////////////////////////////////////////////////////////
//
// [~CSimpleXMLDoc]
//
/////////////////////////////////////////////////////////////////////////////
CSimpleXMLDoc::~CSimpleXMLDoc() {
    CSimpleXMLNode *pNode;
    CSimpleXMLNode *pTargetNode = NULL;
    CXMLNamespace *pNextNamespace;


    // Delete all names we allocated for this document.
    if (NULL != m_pNameList) {
        delete m_pNameList;
    }

    RELEASE_OBJECT(m_pAsyncIOStream);

    // Free all the nodes. This does a simple
    // DFS walk of the tree, deleting every node.
    pNode = m_pRootNode;
    while (NULL != pNode) {
        if (pNode->m_pFirstChild) {
            pNode = pNode->m_pFirstChild;
        } else {
            pTargetNode = pNode;
            if (pNode->m_pParentNode) {
                pNode->m_pParentNode->m_pFirstChild = pNode->m_pNextSibling;
            }

            if (pNode->m_pNextSibling) {
                pNode = pNode->m_pNextSibling;
            }
            else {
                pNode = pNode->m_pParentNode;
            }
            RELEASE_OBJECT(pTargetNode);
        }
    }

    // Discard all namespaces.
    while (NULL != m_pNamespaceList) {
        pNextNamespace = m_pNamespaceList->m_pNextNamespace;
        delete m_pNamespaceList;
        m_pNamespaceList = pNextNamespace;
    } // while (NULL != m_pNamespaceList)
} // ~CSimpleXMLDoc







/////////////////////////////////////////////////////////////////////////////
//
// [InternalInitialize]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleXMLDoc::InternalInitialize() {
    ErrVal err = ENoErr;

    m_pNameList = newex CNameTable;
    if (NULL == m_pNameList) {
        gotoErr(EFail);
    }
    err = m_pNameList->Initialize(CStringLib::IGNORE_CASE, 8);
    if (err) {
        gotoErr(err);
    }

    m_TextNodeName = m_pNameList->AddDictionaryEntry("#text", -1);
    m_DocumentNodeName = m_pNameList->AddDictionaryEntry("#document", -1);
    m_NamespaceDeclarationName = m_pNameList->AddDictionaryEntry("xmlns", -1);
    if ((NULL == m_TextNodeName)
         || (NULL == m_DocumentNodeName)
         || (NULL == m_NamespaceDeclarationName)) {
        gotoErr(EFail);
    }

    m_pNamespaceNamespace = newex CXMLNamespace;
    if (NULL == m_pNamespaceNamespace) {
        gotoErr(EFail);
    }
    m_pNamespaceNamespace->m_pName = m_NamespaceDeclarationName;
    m_pNamespaceNamespace->m_pUrl = strdupex("http://www.w3.org/2000/xmlns/");
    m_pNamespaceNamespace->m_pDeclarationElement = NULL;
    m_pNamespaceNamespace->m_pNextNamespace = NULL;
    m_pNamespaceList = m_pNamespaceNamespace;


abort:
    returnErr(err);
} // InternalInitialize










/////////////////////////////////////////////////////////////////////////////
//
// [ReadXMLBuffer]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleXMLDoc::ReadXMLBuffer(
                     char *pTextBuffer,
                     int64 startOffset,
                     int32 contentLength) {
    ErrVal err = ENoErr;
    CAsyncIOEventHandlerSynch *pSyncAsyncIOStreamCallback = NULL;
    CParsedUrl *pUrl = NULL;
    CAsyncIOStream *pAsyncIOStream = NULL;
    RunChecks();

    m_ContentLength = contentLength;

    pSyncAsyncIOStreamCallback = newex CAsyncIOEventHandlerSynch;
    if (NULL == pSyncAsyncIOStreamCallback) {
        gotoErr(EFail);
    }

    err = pSyncAsyncIOStreamCallback->Initialize();
    if (err) {
        gotoErr(err);
    }

    pUrl = CParsedUrl::AllocateMemoryUrl(
                                 pTextBuffer,
                                 m_ContentLength,
                                 m_ContentLength);
    if (NULL == pUrl) {
        err = EFail;
        goto abort;
    }

    err = CAsyncIOStream::OpenAsyncIOStream(
                              pUrl,
                              0, // openOptions,
                              pSyncAsyncIOStreamCallback,
                              NULL); // pCallbackContext
    if (err) {
        gotoErr(err);
    }
    err = pSyncAsyncIOStreamCallback->Wait();
    if (err) {
        gotoErr(err);
    }
    pAsyncIOStream = pSyncAsyncIOStreamCallback->m_pAsyncIOStream;
    if (NULL == pAsyncIOStream) {
        gotoErr(EFail);
    }

    err = ReadStreamImpl(pAsyncIOStream, startOffset, contentLength);
    if (err) {
        gotoErr(err);
    }

abort:
    RELEASE_OBJECT(pSyncAsyncIOStreamCallback);
    RELEASE_OBJECT(pUrl);

    returnErr(err);
} // ReadXMLBuffer





/////////////////////////////////////////////////////////////////////////////
//
// [ReadStreamImpl]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleXMLDoc::ReadStreamImpl(
                    CAsyncIOStream *pAsyncIOStream,
                    int64 startPos,
                    int32 length) {
    ErrVal err = ENoErr;

    if (NULL == pAsyncIOStream) {
        gotoErr(EFail);
    }

    RELEASE_OBJECT(m_pAsyncIOStream);
    m_pAsyncIOStream = pAsyncIOStream;
    ADDREF_OBJECT(pAsyncIOStream);

    m_StartDocPosition = startPos;
    m_ContentLength = length;

    err = FirstLexicalScan();
    if (err) {
      gotoErr(err);
    }

abort:
    returnErr(err);
} // ReadStreamImpl






/////////////////////////////////////////////////////////////////////////////
//
// [FirstLexicalScan]
//
// This does the first quick scan of the document. It classifies all nodes
// as either:
//
//    1. Text
//    2. Comment
//    3. Processing Instruction
//    4. Element
//    5. CData
//
// It does not parse the contents of elements, so this does not extract
// attributes. It does find the name of elements. That lets us pass over
// special elements like <script> which may contain characters like < and >.
// It also lets us build a parse tree. It also lets us identify Processing
// Instructions and use them for further parsing.
//
// The goal is to do the bare minimum parsing needed, and then lazily parse
// the rest if and when we need it. For example, it usually isn't necessary
// to parse and allocate data structures for every attribute of every element
// in the document.
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleXMLDoc::FirstLexicalScan() {
    ErrVal err = ENoErr;
    char streamCharBuffer[16];
    int32 currentStreamCharSize;
    int64 stopPosition;
    int64 nodeSize;
    bool fInsideTextNode = false;
    CSimpleXMLNode *pNode = NULL;
    CParseState tempParseState;
    RunChecks();


    // <> Later we should select the alphabet somehow.
    // That can come from the HTTP header, or the first 2 bytes of the file.
    m_pParseState = &tempParseState;
    err = InitializeParseState();
    if (err) {
        gotoErr(err);
    }


    err = m_pAsyncIOStream->SetPosition(m_StartDocPosition);
    if (err) {
        gotoErr(err);
    }

    // Create the root node.
    m_pRootNode = newex CSimpleXMLNode;
    if (NULL == m_pRootNode) {
       gotoErr(EFail);
    }
    m_pRootNode->m_pParentDocument = this;
    m_pRootNode->m_NodeType = CPolyXMLNode::XML_NODE_DOCUMENT;
    m_pRootNode->m_StartPosition = 0;
    m_pRootNode->m_NodeSize = (int32) (m_pAsyncIOStream->GetDataLength());
    m_pRootNode->m_pName = m_DocumentNodeName;
    err = InsertNewNodeIntoTree(m_pRootNode);
    if (err) {
        gotoErr(err);
    }


    // Do a fast scan to find all the element boundaries.
    while (1) {
        err = GetXMLChar(
                  DONT_EXPAND_REF_CHARS,
                  streamCharBuffer,
                  sizeof(streamCharBuffer),
                  &currentStreamCharSize);
        if (err) {
            gotoErr(err);
        } // if (err)

        /////////////////////////////////////////////
        // Open an element.
        if (*streamCharBuffer == '<') {
            if (fInsideTextNode) {
                stopPosition = m_pAsyncIOStream->GetPosition() - currentStreamCharSize;
                nodeSize = stopPosition - pNode->m_StartPosition;
                pNode->m_NodeSize = (int32) nodeSize;
                fInsideTextNode = false;
            }

            err = FirstLexicalScanOneElement(currentStreamCharSize);
            if (err) {
               gotoErr(err);
            }
        } // Open an element.
        /////////////////////////////////////////////
        // Read some text.
        else {
            if (!fInsideTextNode) {
                fInsideTextNode = true;
                pNode = newex CSimpleXMLNode;
                if (NULL == pNode) {
                    gotoErr(EFail);
                }
                pNode->m_pParentDocument = this;
                pNode->m_NodeType = CPolyXMLNode::XML_NODE_TEXT;
                pNode->m_pName = m_TextNodeName;
                pNode->m_StartPosition = m_pAsyncIOStream->GetPosition() - currentStreamCharSize;

                err = InsertNewNodeIntoTree(pNode);
                if (err) {
                    gotoErr(err);
                }
            }
        } // Read a text character
    } // while (1)

abort:
    if (EEOF == err) {
       err = ENoErr;
    }

    if ((!err) && (fInsideTextNode)) {
        stopPosition = m_pAsyncIOStream->GetPosition();
        nodeSize = stopPosition - pNode->m_StartPosition;
        pNode->m_NodeSize = (int32) nodeSize;
        fInsideTextNode = false;
    }

    if (!err) {
        int32 index;
        CSimpleXMLNode *pNode;

        for (index = 0; index < m_pParseState->m_StackDepth; index++) {
            pNode = m_pParseState->m_pStack[index];
            if (CPolyXMLNode::XML_NODE_ELEMENT == pNode->m_NodeType) {
                break;
            }
        }
        if (index > 0) {
            index--;
        }
        FlattenNodesWithNoChildren(index);
        m_pParseState->m_StackDepth = index;
    }

    m_pParseState = NULL;

    returnErr(err);
} // FirstLexicalScan





/////////////////////////////////////////////////////////////////////////////
//
// [FirstLexicalScanOneElement]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleXMLDoc::FirstLexicalScanOneElement(int32 openCharLength) {
    ErrVal err = ENoErr;
    char streamCharBuffer[16];
    char elementName[1024];
    int32 currentStreamCharSize;
    CSimpleXMLNode *pNode = NULL;
    bool fReadElementName = false;
    char *pDestPtr;
    char *pEndDestPtr;
    char *pSrcPtr;
    char *pEndSrcPtr;
    bool seenElementEnd = false;
    bool fClosingElement = false;
    bool prevCharWasSlash = false;
    bool cdataElement = false;
    int64 startNamePosition;
    int64 nodeSize;
    int32 nameLength;
    int64 nodeStartPosition;
    int64 nodeCharPosition;
    CPolyXMLNode::XMLNodeType correctNodeType = CPolyXMLNode::XML_NODE_ELEMENT;
    CDictionaryEntry *pCorrectNodeName = NULL;
    CDictionaryEntry *pName = NULL;
    char *pStartName;
    char *pEndName;
    char *pEndNamespace;
    bool fPossibleNamespaceDeclaration = false;
    const char *pCurrentNamespacePrefixMatch;
    RunChecks();

    nodeStartPosition = m_pAsyncIOStream->GetPosition() - openCharLength;


    // Get the first character of the element. This will tell us a lot,
    // like whether it is a close, processing-instruction/comment, or
    // normal element.
    err = GetXMLChar(
               DONT_EXPAND_REF_CHARS,
               streamCharBuffer,
               sizeof(streamCharBuffer),
               &currentStreamCharSize);
    if (err) {
        gotoErr(err);
    } // if (err)


    if ('/' == *streamCharBuffer) {
        fClosingElement = true;
        currentStreamCharSize = 0;
        fReadElementName = true;
    } else if ('!' == *streamCharBuffer) {
        // We need to tell if this is a comment or CDATA.
        // Some tag names like !ELEMENT and !DOCTYPE start with !, but
        // comments and ![CDATA also start with it.
        err = GetXMLChar(
                  DONT_EXPAND_REF_CHARS,
                  streamCharBuffer,
                  sizeof(streamCharBuffer),
                  &currentStreamCharSize);
        if (err) {
            gotoErr(err);
        } // if (err)

        if (*streamCharBuffer == '-') {
            correctNodeType = CPolyXMLNode::XML_NODE_COMMENT;
            pCorrectNodeName = m_pParseState->m_CommentNodeName;
        } else if (*streamCharBuffer == '[') {
            // I used to require the CDATA, but then I found strange
            // nodes that looked like invlalid HTML that used <![ ]>
            // for CDATA. Browsers seem to accept this, so I will.
            //err = m_pAsyncIOStream->scanf("CDATA");
            cdataElement = true;
            correctNodeType = CPolyXMLNode::XML_NODE_CDATA;
            pCorrectNodeName = m_pParseState->m_CDataNodeName;
        } else {
            correctNodeType = CPolyXMLNode::XML_NODE_PROCESSING_INSTRUCTION;
            fReadElementName = true;
        }
    } // End of reading <!
    else {
        // Otherwise, this is just a normal element name.
        fReadElementName = true;
    }


    // We do not allocate explicit nodes for closing elements.
    if (!fClosingElement) {
        pNode = newex CSimpleXMLNode;
        if (NULL == pNode) {
            gotoErr(EFail);
        }
        pNode->m_pParentDocument = this;
        pNode->m_NodeType = correctNodeType;
        pNode->m_pName = pCorrectNodeName;
        pNode->m_StartPosition = nodeStartPosition;
    } // if (!fClosingElement)


    // Read the name of the element.
    if (fReadElementName) {
        pDestPtr = elementName;
        pEndDestPtr = elementName + sizeof(elementName);
        pSrcPtr = streamCharBuffer;
        pEndSrcPtr = streamCharBuffer + currentStreamCharSize;

        // Copy the first character of the name which we read above.
        while ((pSrcPtr < pEndSrcPtr) && (pDestPtr < pEndDestPtr)) {
            *(pDestPtr++) = *(pSrcPtr++);
        }

        startNamePosition = m_pAsyncIOStream->GetPosition() - currentStreamCharSize;
        if (pNode) {
            pNode->m_NodeBody.Element.m_OffsetToName
                = (int32) (startNamePosition - pNode->m_StartPosition);
        }

        // Read every character in the name.
        while (1) {
            err = GetXMLChar(
                        DONT_EXPAND_REF_CHARS,
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        &currentStreamCharSize);
            if (err) {
                gotoErr(err);
            } // if (err)


            if (*pDestPtr == '>') {
                seenElementEnd = true;
                if ((prevCharWasSlash) && (pNode)) {
                    pNode->m_NodeFlags |= CPolyXMLNode::SELF_CLOSING_ELEMENT;
                }
                break;
            }
            // Check for the special <a<b> sequence.
            if (*pDestPtr == '<') {
                nodeCharPosition = m_pAsyncIOStream->GetPosition() - currentStreamCharSize;
                err = m_pAsyncIOStream->SetPosition(nodeCharPosition);
                if (err) {
                    gotoErr(err);
                }

                if (pNode) {
                    pNode->m_NodeFlags |= CPolyXMLNode::ELEMENT_HAS_NO_ENDING_CHAR;
                }

                seenElementEnd = true;
                if ((prevCharWasSlash) && (pNode)) {
                    pNode->m_NodeFlags |= CPolyXMLNode::SELF_CLOSING_ELEMENT;
                }
                break;
            }
            if (CStringLib::WHITE_SPACE_CHAR & CStringLib::GetCharProperties(pDestPtr, 1)) {
                break;
            }

            if ('/' == *streamCharBuffer) {
                prevCharWasSlash = true;
            }
            else {
                prevCharWasSlash = false;
            }

            pDestPtr += currentStreamCharSize;
        } // while (1)

        nameLength = pDestPtr - elementName;
        pStartName = elementName;

        // Check if this starts with a namespace
        pEndName = pStartName + nameLength;
        pEndNamespace = pStartName;
        while ((pEndNamespace < pEndName) && (':' != *pEndNamespace)) {
            pEndNamespace++;
        }
        if ((pEndNamespace < pEndName) && (':' == *pEndNamespace)) {
            if (pNode) {
                pNode->m_pNamespace = GetNamespace(
                                            pStartName,
                                            pEndNamespace - pStartName);
                // If the namespace is not declared, then ignore it.
                // Otherwise, ignore the namespace and treat the whole name as a
                // single name.
                //
                // A lot of web pages have a common html error, where they declare
                // a namespace with xml:lang, rather than xmlns:lang. This does not
                // match the built-in "xmlns" namespace, so we also cannot declare
                // the new "lang" namespace. So, we treat the whole name "xml:lang"
                // and names like "lang:foo" as a single name without namespaces.
                if (NULL != pNode->m_pNamespace) {
                    pStartName = pEndNamespace + 1;
                    nameLength = pEndName - pStartName;
                }
            } // if (pNode)
        }

        pName = m_pNameList->AddDictionaryEntry(pStartName, nameLength);
        if (NULL == pName) {
            gotoErr(EFail);
        }
        if (pNode) {
            pNode->m_pName = pName;
        }
    } // if (fReadElementName)


    // Comments are special.
    // Many web pages will use a comment to wrap nested elements.
    // So, the comment ends with a "-->" sequence, not the first ">".
    if (CPolyXMLNode::XML_NODE_COMMENT == correctNodeType) {
        err = ReadCommentNode(pNode);
        if (err) {
            gotoErr(err);
        }
        seenElementEnd = true;
    }


    // Now, read to the end of this element.
    if (!seenElementEnd) {
        // Look for the special tags. We need to detect these here in the
        // fast loop because they can contain things like < and > inside
        // their body, which will mess up the rest of the parsing.
        if (cdataElement) {
            err = SkipCDATA(pNode);
            if (err) {
                gotoErr(err);
            }
        } else {
            // While we search for the end of the element, we also check whether there
            // is a namespace declared in this element. If there isn't one, then don't
            // do the more expensive search below.
            fPossibleNamespaceDeclaration = false;
            pCurrentNamespacePrefixMatch = g_XMLNamespaceDeclarationPrefix;
            while (1) {
                err = GetXMLChar(
                        DONT_EXPAND_REF_CHARS,
                        streamCharBuffer,
                        sizeof(streamCharBuffer),
                        &currentStreamCharSize);
                if (err) {
                    gotoErr(err);
                } // if (err)

                /////////////////////////////////////
                if (*streamCharBuffer == '>') {
                    seenElementEnd = true;
                    if ((prevCharWasSlash) && (pNode)) {
                        pNode->m_NodeFlags |= CPolyXMLNode::SELF_CLOSING_ELEMENT;
                    }
                    break;
                }
                /////////////////////////////////////
                // Check for the special <a<b> sequence.
                else if (*pDestPtr == '<') {
                    nodeCharPosition = m_pAsyncIOStream->GetPosition() - currentStreamCharSize;
                    err = m_pAsyncIOStream->SetPosition(nodeCharPosition);
                    if (err) {
                        gotoErr(err);
                    }

                    if (pNode) {
                        pNode->m_NodeFlags |= CPolyXMLNode::ELEMENT_HAS_NO_ENDING_CHAR;
                    }

                    seenElementEnd = true;
                    if ((prevCharWasSlash) && (pNode)) {
                        pNode->m_NodeFlags |= CPolyXMLNode::SELF_CLOSING_ELEMENT;
                    }
                    break;
                }

                // Look for namespaces.
                if (*streamCharBuffer == *pCurrentNamespacePrefixMatch) {
                    pCurrentNamespacePrefixMatch += 1;
                    if (0 == *pCurrentNamespacePrefixMatch) {
                        fPossibleNamespaceDeclaration = true;
                        pCurrentNamespacePrefixMatch = g_XMLNamespaceDeclarationPrefix;
                    }
                } else {
                    pCurrentNamespacePrefixMatch = g_XMLNamespaceDeclarationPrefix;
                }


                if (*streamCharBuffer == '/') {
                    prevCharWasSlash = true;
                }
                else {
                    prevCharWasSlash = false;
                }
            } // while (1)
        }
    } // if (!seenElementEnd)



    if (pNode) {
       nodeSize = m_pAsyncIOStream->GetPosition() - pNode->m_StartPosition;
       pNode->m_NodeSize = (int32) nodeSize;
    }


    // If this is a special element,
    if ((pNode)
            && (!cdataElement)
            && (NULL != pNode->m_pName)
            && (NULL != pNode->m_pName->m_pSpecialCloseName)) {
        err = SkipUnformattedText(pNode);
        if (err) {
            gotoErr(err);
        }
    } // if (NULL != pNode->m_pName->m_pSpecialCloseName)


    if (fClosingElement) {
        err = CloseNode(pName, nodeStartPosition);
        if (err) {
           gotoErr(err);
        }
    } else {
        err = InsertNewNodeIntoTree(pNode);
        if (err) {
            gotoErr(err);
        }
    }

    if ((pNode) 
            && (CPolyXMLNode::XML_NODE_ELEMENT == pNode->m_NodeType)
            && (fPossibleNamespaceDeclaration)) {
        err = FindNamespaceDeclarations(pNode);
        if (err) {
            gotoErr(err);
        }
    }


abort:
    if (EEOF == err) {
        err = ENoErr;
    }

    returnErr(err);
} // FirstLexicalScanOneElement







/////////////////////////////////////////////////////////////////////////////
//
// [FindNamespaceDeclarations]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleXMLDoc::FindNamespaceDeclarations(CSimpleXMLNode *pNode) {
    ErrVal err = ENoErr;
    int64 startAttrPosition;
    int64 stopPosition;
    int64 savePosition = -1;
    RunChecks();

    // Only elements can have namespace delcarations. This immediately
    // eliminates all text nodes, which is the majority of a typical html page.
    if ((NULL == pNode)
        || (CPolyXMLNode::XML_NODE_ELEMENT != pNode->m_NodeType)) {
        gotoErr(ENoErr);
    }

    // If the node is already parsed, then check the attributes.
    if (pNode->m_NodeFlags & CPolyXMLNode::LAZY_ATTRIBUTES_PARSED) {
        err = DeclareNamespaces(pNode);
        gotoErr(err);
    }

    if (NULL == m_pAsyncIOStream) {
        gotoErr(ENoErr);
    }
    savePosition = m_pAsyncIOStream->GetPosition();

    startAttrPosition = pNode->m_StartPosition
                            + pNode->m_NodeBody.Element.m_OffsetToName;
    if (pNode->m_pName) {
        startAttrPosition += pNode->m_pName->m_NameLength;
    }
    stopPosition = pNode->m_StartPosition + pNode->m_NodeSize;
    err = m_pAsyncIOStream->SetPosition(startAttrPosition);
    if (err) {
        gotoErr(err);
    }

    stopPosition = m_pAsyncIOStream->FindString(
                                          g_XMLNamespaceDeclarationPrefix,
                                          g_XMLNamespaceDeclarationPrefixLength,
                                          CStringLib::IGNORE_CASE,
                                          stopPosition);
    if (stopPosition > 0) {
        err = pNode->ParseAttributes();
        if (err) {
            gotoErr(err);
        }
        err = DeclareNamespaces(pNode);
        if (err) {
            gotoErr(err);
        }
    }

abort:
    if (savePosition >= 0) {
        m_pAsyncIOStream->SetPosition(savePosition);
    }

    returnErr(err);
} // FindNamespaceDeclarations








/////////////////////////////////////////////////////////////////////////////
//
// [DeclareNamespaces]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleXMLDoc::DeclareNamespaces(CSimpleXMLNode *pNode) {
    ErrVal err = ENoErr;
    CSimpleXMLAttribute *pAttribute;
    CXMLNamespace *pNamespace = NULL;
    int64 startPosInStream;
    int32 length;
    char *pUrl;
    char *pEndUrl;
    char tempUrlBuffer[1024];
    RunChecks();


    // Look for attributes that declare a new namespace.
    // These will have the "xmlns:" namespace.
    pAttribute = pNode->m_NodeBody.Element.m_pAttributeList;
    while (NULL != pAttribute) {
        if (m_pNamespaceNamespace == pAttribute->m_pNamespace) {
            if (PROPERTY_STRING == pAttribute->m_Value.m_ValueType) {
                err = pAttribute->m_Value.GetStringValue(&pUrl, &length);
                if (err) {
                    gotoErr(err);
                }
            }
            else if ((PROPERTY_RANGE == pAttribute->m_Value.m_ValueType)
                    && (NULL != m_pAsyncIOStream)) {
                err = pAttribute->m_Value.GetRangeValue(&startPosInStream, &length);
                if (err) {
                    gotoErr(err);
                }
                err = m_pAsyncIOStream->GetPtr(
                                            startPosInStream,
                                            length,
                                            tempUrlBuffer,
                                            sizeof(tempUrlBuffer),
                                            &pUrl);
                if (err) {
                    gotoErr(err);
                }
            }
            // This may NOT be an error. Sometimes, there are
            // namespace declarations like "<html xmlns:ie>".
            // Treat these as an empty namespace.
            else {
                pUrl = NULL;
                length = 0;
            }

            pEndUrl = pUrl + length;
            while ((pUrl < pEndUrl) && ('\"' == *pUrl)) {
                pUrl++;
            }
            while ((pUrl < pEndUrl) && ('\"' == *(pEndUrl - 1))) {
                pEndUrl--;
            }

            pNamespace = newex CXMLNamespace;
            if (NULL == pNamespace) {
                gotoErr(EFail);
            }

            if (NULL != pUrl) {
                pNamespace->m_pUrl = strndupex(pUrl, pEndUrl - pUrl);
                if (NULL == pNamespace->m_pUrl) {
                    gotoErr(EFail);
                }
            }

            pNamespace->m_pDeclarationElement = pNode;
            pNamespace->m_pName = pAttribute->m_pName;
            pNamespace->m_pNextNamespace = m_pNamespaceList;
            m_pNamespaceList = pNamespace;
            pNamespace = NULL;
        } // if (m_NamespaceDeclarationName == pAttribute->m_pName)

        pAttribute = pAttribute->m_pNextAttribute;
    } // while (NULL != pAttribute)

abort:
    if (pNamespace) {
        delete pNamespace;
    }

    returnErr(err);
} // DeclareNamespaces







/////////////////////////////////////////////////////////////////////////////
//
// [GetNamespace]
//
/////////////////////////////////////////////////////////////////////////////
CXMLNamespace *
CSimpleXMLDoc::GetNamespace(char *pName, int32 nameLength) {
    CXMLNamespace *pNamespace = NULL;
    RunChecks();

    pNamespace = m_pNamespaceList;
    while (NULL != pNamespace) {
        if ((NULL != pNamespace->m_pName)
            && (NULL != pNamespace->m_pName->m_pName)
            && (nameLength == pNamespace->m_pName->m_NameLength)
            && (0 == strncasecmpex(pNamespace->m_pName->m_pName, pName, nameLength))) {
            break;
        }
        pNamespace = pNamespace->m_pNextNamespace;
    }

    return(pNamespace);
} // GetNamespace







/////////////////////////////////////////////////////////////////////////////
//
// [SkipCDATA]
//
// This handles nodes CDTATA nodes, which are less structured
// than <listing>, <script> and <code>.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleXMLDoc::SkipCDATA(CSimpleXMLNode *pNode) {
    ErrVal err = ENoErr;
    int64 matchPosition = -1;
    int64 stopPosition;
    int32 patternSize;
    RunChecks();

    // Look for the special tags. We need to detect these here in the
    // fast loop because they can contain things like < and > inside
    // their body, which will mess up the rest of the parsing.
    if ((NULL != pNode->m_pName)
       && (NULL != pNode->m_pName->m_pSpecialCloseName)) {
        patternSize = strlen(pNode->m_pName->m_pSpecialCloseName);
        matchPosition = m_pAsyncIOStream->FindString(
                                            pNode->m_pName->m_pSpecialCloseName,
                                            patternSize,
                                            CStringLib::IGNORE_CASE,
                                            -1);
        if (matchPosition < 0) {
            // This is a hack. But, if we cannot find the CDATA close or whatever
            // close we want, then consider the next > to be the close.
            // Some documents use ]> instead of ]]> and similar mistakes.
            patternSize = g_CloseElementStrLength;
            matchPosition = m_pAsyncIOStream->FindString(
                                                g_CloseElementStr,
                                                g_CloseElementStrLength,
                                                CStringLib::IGNORE_CASE,
                                                -1);
        }
        if (matchPosition < 0) {
            gotoErr(EEOF);
        }
    } // if (NULL != pTextNode->m_pName->m_pSpecialCloseName)


    stopPosition = m_pAsyncIOStream->GetPosition();
    stopPosition += patternSize;

    err = m_pAsyncIOStream->SetPosition(stopPosition);
    if (err) {
        gotoErr(err);
    }

abort:
    returnErr(err);
} // SkipCDATA







/////////////////////////////////////////////////////////////////////////////
//
// [SkipUnformattedText]
//
// This handles nodes like <listing>, <script> and <code>.
// The text is within well defined elements.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleXMLDoc::SkipUnformattedText(CSimpleXMLNode *pParentNode) {
    ErrVal err = ENoErr;
    CSimpleXMLNode *pTextNode = NULL;
    int64 matchPosition = -1;
    int64 stopPosition;
    int64 nodeSize;
    int32 patternSize;
    RunChecks();

    pParentNode->m_NodeFlags |= CPolyXMLNode::ELEMENT_HAS_SPECIAL_CLOSE;

    pTextNode = newex CSimpleXMLNode;
    if (NULL == pTextNode) {
       gotoErr(EFail);
    }
    pTextNode->m_pParentDocument = this;
    pTextNode->m_NodeType = CPolyXMLNode::XML_NODE_CDATA;
    pTextNode->m_pName = m_pParseState->m_CDataNodeName;
    pTextNode->m_StartPosition = m_pAsyncIOStream->GetPosition();

    // Look for the special tags. We need to detect these here in the
    // fast loop because they can contain things like < and > inside
    // their body, which will mess up the rest of the parsing.
    if ((NULL != pParentNode->m_pName)
       && (NULL != pParentNode->m_pName->m_pSpecialCloseName)) {
        patternSize = strlen(pParentNode->m_pName->m_pSpecialCloseName);
        matchPosition = m_pAsyncIOStream->FindString(
                                            pParentNode->m_pName->m_pSpecialCloseName,
                                            patternSize,
                                            CStringLib::IGNORE_CASE,
                                            -1);
        if (matchPosition < 0) {
            // There is no close to the <script> element.
            // OK, there are 2 ways we can go here. Either
            // treat the rest of the document as a script,
            // or assume it's an empty script.
            // The one page I actually saw this on had an empty
            // script, but it is safer to treat the rest of
            // the doc as a script. Script text makes parsing weird.
            matchPosition = (m_pAsyncIOStream->GetDataLength() - patternSize) + 1;
            err = m_pAsyncIOStream->SetPosition(matchPosition);
            if (err) {
                gotoErr(err);
            }
            pParentNode->m_NodeFlags |= CPolyXMLNode::ELEMENT_HAS_NO_CLOSE;
        }
    } // if (NULL != pTextNode->m_pName->m_pSpecialCloseName)

    // We stopped at the beginning of the close element.
    stopPosition = m_pAsyncIOStream->GetPosition();
    err = m_pAsyncIOStream->SetPosition(stopPosition + patternSize);
    if (EEOF == err) {
        err = ENoErr;
    }
    if (err) {
        gotoErr(err);
    }

    // Set the node size of the child text node.
    nodeSize = stopPosition - pTextNode->m_StartPosition;
    pTextNode->m_NodeSize = (int32) (nodeSize);

    pParentNode->InsertLastChild(pTextNode);

abort:
    returnErr(err);
} // SkipUnformattedText









/////////////////////////////////////////////////////////////////////////////
//
// [ReadCommentNode]
//
// Technically, a single comment may have several comment bodies, like:
// <!-- xxxxx -- -- xxxxx --!>
//
// In practice, people use comments to block out chunks of HTML, so
// the body may contain any sequence of --, >, or < characters. I
// don't try to parse these. Instead, just find the first occurrence
// of -->
//
// We do NOT close the tag with an isolated > character.
// Comments often separate out things like JavaScript, which
// will include text like "if (a > 10)" and we don't want to
// interpret that > as closing the comment.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleXMLDoc::ReadCommentNode(CSimpleXMLNode *pNode) {
    ErrVal err = ENoErr;
    int64 stopPosition;
    UNUSED_PARAM(pNode);

    stopPosition = m_pAsyncIOStream->FindString(
                                          g_CloseCommentStr,
                                          g_CloseCommentStrLength,
                                          CStringLib::IGNORE_CASE,
                                          -1);
    if (stopPosition < 0) {
        gotoErr(EEOF);
    }

    // Now, go to the end of the comment.
    stopPosition += g_CloseCommentStrLength;
    err = m_pAsyncIOStream->SetPosition(stopPosition);
    if (err) {
        gotoErr(err);
    }

abort:
    returnErr(err);
} // ReadCommentNode





/////////////////////////////////////////////////////////////////////////////
//
// [InsertNewNodeIntoTree]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleXMLDoc::InsertNewNodeIntoTree(CSimpleXMLNode *pNode) {
    ErrVal err = ENoErr;
    CSimpleXMLNode *pParent;
    RunChecks();

    // Make this tag a child of the current parent.
    if (m_pParseState->m_StackDepth >= 1) {
        pParent = m_pParseState->m_pStack[m_pParseState->m_StackDepth - 1];
        pParent->InsertLastChild(pNode);
    }

    // Special elements like CDATA do not go onto the stack.
    // They only contain 1 child, the CDATA text, and we handle
    // that when we parse the node.
    if (pNode->m_NodeFlags & CPolyXMLNode::ELEMENT_HAS_SPECIAL_CLOSE) {
        gotoErr(ENoErr);
    }

    // Put this element on the stack.
    if ((CPolyXMLNode::XML_NODE_DOCUMENT == pNode->m_NodeType)
            || ((CPolyXMLNode::XML_NODE_ELEMENT == pNode->m_NodeType)
                && !(pNode->m_NodeFlags & CPolyXMLNode::SELF_CLOSING_PROCESSING_INSTRUCTION)
                && !(pNode->m_NodeFlags & CPolyXMLNode::SELF_CLOSING_ELEMENT)
                && (NULL != pNode->m_pName))) {
        if (m_pParseState->m_StackDepth >= CParseState::MAX_ELEMENT_STACK_DEPTH) {
            gotoErr(EXMLParseError);
        } // processing an overlapping tag.

        m_pParseState->m_pStack[m_pParseState->m_StackDepth] = pNode;
        m_pParseState->m_StackDepth += 1;
    } // Push an element onto the stack.

abort:
    returnErr(err);
} // InsertNewNodeIntoTree





/////////////////////////////////////////////////////////////////////////////
//
// [CloseNode]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleXMLDoc::CloseNode(CDictionaryEntry *pName, int64 startPosition) {
    ErrVal err = ENoErr;
    int stackIndex;
    CSimpleXMLNode *pParent;
    RunChecks();

    // Find the matching opening tag.
    for (stackIndex = m_pParseState->m_StackDepth - 1; stackIndex >= 0; stackIndex--) {
       pParent = m_pParseState->m_pStack[stackIndex];
       if (pName == pParent->m_pName) {
           break;
       }
    } // Find the matching opening tag.

    // If there is no parent, then either this is a close tag
    // with no corresponding open (an unlikely but possible case caused
    // by an html syntax error), or the open tag that corresponded to this
    // tag was removed from the stack when we previously found a
    // non-overlapping tag.
    if (stackIndex < 0) {
        if (m_fEnforceStrictXML) {
            gotoErr(EXMLParseError);
        } else {
            err = AddUnbalancedCloseNode(pName, startPosition);
            gotoErr(err);
        }
    } // (stackIndex <= 0)
    // Otherwise, we found the open tag that matches this close.
    else
    {
        // Any nodes that were not closed are assumed to have no
        // children. This may introduce special overlapping elements
        // if the close shows up later.
        FlattenNodesWithNoChildren(stackIndex);

        // Closing an element pops that element and any of its children
        // from the stack.
        m_pParseState->m_StackDepth = stackIndex;
     }

abort:
    returnErr(err);
} // CloseNode






/////////////////////////////////////////////////////////////////////////////
//
// [FlattenNodesWithNoChildren]
//
// Any nodes that were not closed are assumed to have no
// children. This will get really confused with overlapping
// tags, but those are not allowed in XML and they are confusing
// and bad style in HTML.
//
// <a>
//   <br>
//     <br>
//       <br>
// </a>
//
// Becomes this:
//
// <a>
//   <br> <br> <br>
// </a>
//
// This preserves the DFS order of the nodes.
// This happens in several steps. <* means it is on the stack.
// We start with this:
//
// <a>
//   <*br>
//     <*br>
//       <br>
// </a>
//
// After the first step:
//
// <a>
//   <*br>
//     <*br>
//       <br>
// </a>
//
// After the second step:
//
// <a>
//   <*br>
//     <*br> <br>
// </a>
//
//
/////////////////////////////////////////////////////////////////////////////
void
CSimpleXMLDoc::FlattenNodesWithNoChildren(int32 stopIndex) {
    int32 index;
    CSimpleXMLNode *pNode;
    CSimpleXMLNode *pPrevDFSNode;
    CSimpleXMLNode *pNextChildNode;
    CSimpleXMLNode *pChildNode;
    RunChecks();

    if ((stopIndex < 0)
        || (stopIndex >= m_pParseState->m_StackDepth)) {
        return;
    }

    // Mark every child that is on the stack.
    // This node has no close, so it cannot have any children.
    // But, its children may or may not have legal close elements,
    // so those grandchildren may not necessarily need fixing. They will
    // if and only if their parent (this nodes child) is also on the stack.
    for (index = m_pParseState->m_StackDepth - 1; index > stopIndex; index--) {
        pNode = m_pParseState->m_pStack[index];
        pNode->m_NodeFlags |= CPolyXMLNode::ELEMENT_HAS_NO_CLOSE;

#if DEBUG_XML
        if ((g_BuggyNode > 0) && (g_BuggyNode == pNode->m_DebugNodeId)) {
            g_BuggyNode = g_BuggyNode;
        }
#endif

        pPrevDFSNode = pNode;
        pChildNode = pNode->m_pFirstChild;
        while (NULL != pChildNode) {
            pNextChildNode = pChildNode->m_pNextSibling;

            pChildNode->m_pNextSibling = pPrevDFSNode->m_pNextSibling;
            pPrevDFSNode->m_pNextSibling = pChildNode;
            pChildNode->m_pParentNode = pPrevDFSNode->m_pParentNode;

            pPrevDFSNode = pChildNode;
            pChildNode = pNextChildNode;
        } // while (NULL != pChildNode)

        pNode->m_pFirstChild = NULL;
    } // for (index = m_pParseState->m_StackDepth - 1; index > stopIndex; index--)
} // FlattenNodesWithNoChildren





/////////////////////////////////////////////////////////////////////////////
//
// [AddUnbalancedCloseNode]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleXMLDoc::AddUnbalancedCloseNode(CDictionaryEntry *pName, int64 startPosition) {
    ErrVal err = ENoErr;
    CSimpleXMLNode *pParent = NULL;
    CSimpleXMLNode *pNode = NULL;
    RunChecks();

    if (m_pParseState->m_StackDepth <= 0) {
        gotoErr(EXMLParseError);
    }

    pParent = m_pParseState->m_pStack[m_pParseState->m_StackDepth - 1];

    pNode = newex CSimpleXMLNode;
    if (NULL == pNode) {
        gotoErr(EFail);
    }

    pNode->m_pParentDocument = this;
    pNode->m_NodeType = CPolyXMLNode::XML_NODE_ELEMENT;
    pNode->m_pName = pName;
    pNode->m_StartPosition = startPosition;
    pNode->m_NodeFlags |= CPolyXMLNode::UNBALANCED_CLOSE_ELEMENT;
    pNode->m_NodeFlags |= CPolyXMLNode::ELEMENT_HAS_NO_CLOSE;

    // Assume the node is just </name>
    pNode->m_NodeSize = pName->m_NameLength + 3;
    pNode->m_NodeBody.Element.m_OffsetToName = 2;
    pNode->m_NodeBody.Element.m_pAttributeList = NULL;

    pParent->InsertLastChild(pNode);

abort:
    returnErr(err);
} // AddUnbalancedCloseNode






/////////////////////////////////////////////////////////////////////////////
//
// [InitializeParseState]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleXMLDoc::InitializeParseState() {
    ErrVal err = ENoErr;
    CDictionaryEntry *pName = NULL;
    RunChecks();

    // Lazily allocate and initialize the global name list.
    if (NULL == m_pGlobalNameList) {
        m_pGlobalNameList = newex CNameTable;
        if (NULL == m_pGlobalNameList) {
            gotoErr(EFail);
        }
        err = m_pGlobalNameList->Initialize(CStringLib::IGNORE_CASE, 8);
        if (err) {
            gotoErr(err);
        }

        // Add the built-in names.
        m_pGlobalNameList->AddDictionaryEntryList(g_CommonBuiltInNames);

        // Add the special names.
        pName = m_pGlobalNameList->AddDictionaryEntry("script", -1);
        if (NULL != pName) {
            pName->m_pSpecialCloseName = "</script>";
        }
        pName = m_pGlobalNameList->AddDictionaryEntry("plaintext", -1);
        if (NULL != pName) {
            pName->m_pSpecialCloseName = "</plaintext>";
        }
        pName = m_pGlobalNameList->AddDictionaryEntry("listing", -1);
        if (NULL != pName) {
            pName->m_pSpecialCloseName = "</listing>";
        }
        pName = m_pGlobalNameList->AddDictionaryEntry("cdata", -1);
        if (NULL != pName) {
            pName->m_pSpecialCloseName = "]]>";
        }
        pName = m_pGlobalNameList->AddDictionaryEntry("xmp", -1);
        if (NULL != pName) {
            pName->m_pSpecialCloseName = "</xmp>";
        }
    } // if (NULL == m_pGlobalNameList)

    // Allocate and initialize the new empty name list
    // for this document.
    err = m_pNameList->InitializeWithParentNameTable(m_pGlobalNameList);
    if (err) {
        gotoErr(err);
    }

    m_pParseState->m_CDataNodeName = m_pNameList->AddDictionaryEntry("#cdata-section", -1);
    m_pParseState->m_CommentNodeName = m_pNameList->AddDictionaryEntry("#comment", -1);
    if ((NULL == m_pParseState->m_CDataNodeName)
        || (NULL == m_pParseState->m_CommentNodeName)) {
        gotoErr(EFail);
    }
    m_pParseState->m_CDataNodeName->m_pSpecialCloseName = "]]>";

    m_pParseState->m_StackDepth = 0;

abort:
    returnErr(err);
} // InitializeParseState






/////////////////////////////////////////////////////////////////////////////
//
// [GetIOStream]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleXMLDoc::GetIOStream(
                        CAsyncIOStream **ppSrcStream,
                        int64 *pStartPosition,
                        int32 *pLength) {
    ErrVal err = ENoErr;

    if (NULL == m_pAsyncIOStream) {
        if (NULL != ppSrcStream) {
           *ppSrcStream = NULL;
        }
        if (NULL != pStartPosition) {
           *pStartPosition = 0;
        }
        if (NULL != pLength) {
           *pLength = 0;
        }
        gotoErr(ENoErr);
    }

    if (NULL != ppSrcStream) {
        *ppSrcStream = m_pAsyncIOStream;
        ADDREF_OBJECT(m_pAsyncIOStream);

        err = m_pAsyncIOStream->SetPosition(m_StartDocPosition);
        if (err) {
            gotoErr(err);
        }
    }
    if (NULL != pStartPosition) {
        *pStartPosition = m_StartDocPosition;
    }
    if (NULL != pLength) {
        *pLength = m_ContentLength;
    }

abort:
    returnErr(err);
} // GetIOStream.








//////////////////////////////////////////////////////////////////////////////
//
// [GetXMLChar]
//
// Get a single character for the XML document.
//
// This translates character references.
// It also works with different character sizes and alphabets.
//////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleXMLDoc::GetXMLChar(
                  int referenceMode,
                  char *pBuffer,
                  int32 maxCharLen,
                  int *pCharLen) {
    ErrVal err = ENoErr;
    char escapeSequenceBuffer[13];
    char *pDestPtr;
    char *pEndDestPtr;
    char currentCharBuffer[16];
    int32 currentCharLen;
    int64 startPosition;
    bool fExpandingEntity = false;
    CEntityValue *pEntity;
    char currentChar;
    char *pStartSuffix;
    char *pNumChar;
    int32 intValue;


    if ((NULL == pBuffer) || (NULL == pCharLen)) {
        gotoErr(EFail);
    }

    // Read the character.
    err = m_pAsyncIOStream->GetChar(pBuffer, maxCharLen, pCharLen);
    if (err) {
        gotoErr(err);
    }

    // If this is not an escape sequence, then we are done.
    if (*pBuffer != '&') {
        gotoErr(ENoErr);
    }
    // We also may choose to not treat & as an entity.
    if (DONT_EXPAND_REF_CHARS == referenceMode) {
        gotoErr(ENoErr);
    }

    // Save the position.
    // If we cannot translate the reference, then we just return it.
    fExpandingEntity = true;
    startPosition = m_pAsyncIOStream->GetPosition();


    // Read the entire escape sequence into a buffer.
    // Ignore the '&', we don't need it.
    //
    // Look for a terminating ';' character. This might just be an innocent
    // '&' char in the middle of some text and not a reference.
    pDestPtr = escapeSequenceBuffer;
    pEndDestPtr = escapeSequenceBuffer + sizeof(escapeSequenceBuffer);
    // Leave one for the NULL terminator.
    pEndDestPtr = pEndDestPtr - 1;
    while (pDestPtr < pEndDestPtr) {
        err = m_pAsyncIOStream->GetChar(
                                 currentCharBuffer,
                                 sizeof(currentCharBuffer),
                                 &currentCharLen);
        if (err) {
            gotoErr(err);
        }
        currentChar = *currentCharBuffer;

        // If the first character after the '&' is a ' ' or some text,
        // then this is not an entity.
        if (pDestPtr == escapeSequenceBuffer) {
           int32 charProps = CStringLib::GetCharProperties(&currentChar, 1);
           if (CStringLib::WHITE_SPACE_CHAR & charProps)
           {
               goto abort;
           }
        }

        // A ';' ends the sequence.
        if (';' == currentChar) {
            *pDestPtr = 0;
            break;
        }

        *(pDestPtr++) = currentChar;
    } // while (1)


    // If we read the characters and did not find the entity, then
    // don't panic. This might just be an innocent
    // '&' char in the middle of some text and not a reference.
    if ((pDestPtr >= pEndDestPtr) || (pDestPtr == escapeSequenceBuffer)) {
       gotoErr(ENoErr);
    }

    // Split off any "(...)" suffix.
    pStartSuffix = escapeSequenceBuffer;
    while (pStartSuffix < pDestPtr) {
       if ('(' == *pStartSuffix) {
          *pStartSuffix = 0;
          break;
       }
       pStartSuffix++;
    }

    // The first character tells us what kind of character reference this is.
    // Handle the case of an entity like #123. This is a character code.
    if ('#' == escapeSequenceBuffer[0]) {
        pNumChar = &(escapeSequenceBuffer[1]);
        err = CStringLib::StringToNumber(
                                    pNumChar,
                                    pStartSuffix - pNumChar,
                                    &intValue);
        if (err) {
           gotoErr (err);
        }

        *pBuffer = (char) intValue;
        *pCharLen = 1;
        fExpandingEntity = false;
        goto abort;
    } // if ('#' == escapeSequenceBuffer[0])


    // Look if this is one of our recognized characters.
    pEntity = g_EntityNames;
    while (NULL != pEntity->pName) {
       if (0 == strcasecmpex(escapeSequenceBuffer, pEntity->pName)) {
          break;
       }
       pEntity++;
    } // while (NULL != pEntity->pName)

    // If we got a match, then we are done.
    if (NULL != pEntity->pName) {
       *pBuffer = pEntity->value;
       *pCharLen = 1;
       fExpandingEntity = false;
       goto abort;
    }

abort:
    // If we couldn't translate the reference, then just return the '&'
    // character.
    if (fExpandingEntity) {
       err = m_pAsyncIOStream->SetPosition(startPosition);
    }

    returnErr(err);
} // GetXMLChar







/////////////////////////////////////////////////////////////////////////////
//
// [CSimpleXMLNode]
//
/////////////////////////////////////////////////////////////////////////////
CSimpleXMLNode::CSimpleXMLNode() {
    m_NodeType = XML_NODE_ELEMENT;
    m_NodeFlags = 0;
    m_NodeSize = 0;
    m_StartPosition = -1;

    m_pName = NULL;
    m_pNamespace = NULL;

    m_pParentDocument = NULL;

    // Make sure to initialize enough of the union so all fields are initialized.
    // If one union selector is shorter than another, then it will only initialize
    // part of the data.
    m_NodeBody.Element.m_OffsetToName = 0;
    m_NodeBody.Element.m_pAttributeList = NULL;
    m_NodeBody.Text.m_pText = NULL;

    m_pFirstChild = NULL;
    m_pNextSibling = NULL;
    m_pParentNode = NULL;

#if DEBUG_XML
    m_StartWritePos = -1;
    m_StopWritePos = -1;

    if ((g_BuggyNode > 0) && (g_BuggyNode == g_NextDebugNodeId)) {
        g_BuggyNode = g_BuggyNode;
    }
    m_DebugNodeId = g_NextDebugNodeId;
    g_NextDebugNodeId++;
#endif // DEBUG_XML
} // CSimpleXMLNode





/////////////////////////////////////////////////////////////////////////////
// [~CSimpleXMLNode]
/////////////////////////////////////////////////////////////////////////////
CSimpleXMLNode::~CSimpleXMLNode() {
    CSimpleXMLAttribute *pAttribute;

    if ((XML_NODE_ELEMENT == m_NodeType)
        || (XML_NODE_PROCESSING_INSTRUCTION == m_NodeType)
        || (XML_NODE_ATTRIBUTE == m_NodeType)) {
        while (NULL != m_NodeBody.Element.m_pAttributeList) {
            pAttribute = m_NodeBody.Element.m_pAttributeList;
            m_NodeBody.Element.m_pAttributeList = pAttribute->m_pNextAttribute;
            RELEASE_OBJECT(pAttribute);
        }
    }

    if ((XML_NODE_TEXT == m_NodeType)
        || (XML_NODE_COMMENT == m_NodeType)
        || (XML_NODE_CDATA == m_NodeType)) {
        if ((m_NodeFlags & CAN_DELETE_TAG_BODY)
            && (m_NodeBody.Text.m_pText)) {
            memFree(m_NodeBody.Text.m_pText);
        }
    }
} // ~CSimpleXMLNode






/////////////////////////////////////////////////////////////////////////////
//
// [InsertLastChild]
//
/////////////////////////////////////////////////////////////////////////////
void
CSimpleXMLNode::InsertLastChild(CSimpleXMLNode *pChildNode) {
    CSimpleXMLNode *pLastChild;

    pLastChild = m_pFirstChild;
    while ((NULL != pLastChild) && (NULL != pLastChild->m_pNextSibling)) {
        pLastChild = pLastChild->m_pNextSibling;
    }

    if (NULL == pLastChild) {
        m_pFirstChild = pChildNode;
    } else {
        pLastChild->m_pNextSibling = pChildNode;
    }
    pChildNode->m_pNextSibling = NULL;
    pChildNode->m_pParentNode = this;
} // InsertLastChild







/////////////////////////////////////////////////////////////////////////////
//
// [ParseAttributes]
//
// This is a key feature of this DOM.
// We lazily parse the attributes of elements, so we only parse the attributes
// of an element iff we actually look at those attributes.
// The parsing time may not be huge, but the data structures for every atteibute
// can be expensive.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleXMLNode::ParseAttributes() {
    ErrVal err = ENoErr;
    char nameBuffer[1024];
    char *pDestPtr;
    char *pEndDestPtr;
    int32 charSize;
    int32 state;
    int64 startValuePosition;
    int64 startAttrPosition;
    CSimpleXMLAttribute *pCurrentAttribute = NULL;
    CSimpleXMLAttribute *pLastAttribute = NULL;
    CAsyncIOStream *pAsyncIOStream;
    bool fExpectingValue;
    int32 valueLength;
    char openQuoteChar;
    RunChecks();

    if ((NULL == m_pParentDocument)
        || (NULL == m_pParentDocument->m_pAsyncIOStream)) {
        gotoErr(EFail);
    }
    pAsyncIOStream = m_pParentDocument->m_pAsyncIOStream;

#if DEBUG_XML
    if ((g_BuggyNode > 0) && (g_BuggyNode == m_DebugNodeId)) {
        g_BuggyNode = g_BuggyNode;
    }
#endif

    startAttrPosition = m_StartPosition + m_NodeBody.Element.m_OffsetToName;
    if (m_pName) {
        startAttrPosition += m_pName->m_NameLength;
    }
    err = pAsyncIOStream->SetPosition(startAttrPosition);
    if (err) {
        gotoErr(err);
    }

    // Now, read the attributes of this element.
    pDestPtr = nameBuffer;
    pEndDestPtr = nameBuffer + sizeof(nameBuffer);
    fExpectingValue = false;
    state = READING_WHITESPACE;
    while (1) {
        err = m_pParentDocument->GetXMLChar(
                                    DONT_EXPAND_REF_CHARS,
                                    pDestPtr,
                                    pEndDestPtr - pDestPtr,
                                    &charSize);
        if (err) {
            gotoErr(err);
        } // if (err)


        /////////////////////////////////////////////////////
        // A '>' will close the element.
        if (*pDestPtr == '>') {
            // Finish a current name of value
            // Be careful, a ?> is not the end of a name.
            if (READING_NAME == state) {
                if ((1 == (pDestPtr - nameBuffer)) && (*nameBuffer == '?')) {
                    m_NodeFlags |= SELF_CLOSING_PROCESSING_INSTRUCTION;
                }
                else if ((1 == (pDestPtr - nameBuffer)) && (*nameBuffer == '/')) {
                    m_NodeFlags |= SELF_CLOSING_ELEMENT;
                }
                else {
                    err = AddAttribute(
                                nameBuffer,
                                pDestPtr - nameBuffer,
                                &pCurrentAttribute,
                                &pLastAttribute);
                    if (err)
                    {
                        gotoErr(err);
                    }
                }
            }
            else if ((READING_VALUE == state)
                    || (READING_QUOTED_VALUE == state)) {
                // Finish the value.
                // Sometimes quoted values don't have a close quote, and end
                // with the >, such as <a href="a.b.c>
                // It is described as an obsolete feature in the html spec.
                //
                // Do not allow an < to close a string by opening a shorthand tag.
                valueLength = (int32) ((pAsyncIOStream->GetPosition() - charSize)
                                        - startValuePosition);
                if (valueLength < 0) {
                    valueLength = valueLength;
                }
                pCurrentAttribute->m_Value.SetRangeValue(startValuePosition, valueLength);
                pCurrentAttribute = NULL;
            }

            break;
        }

        /////////////////////////////////////////////////////
        // This is a whitespace char.
        else if (CStringLib::WHITE_SPACE_CHAR & CStringLib::GetCharProperties(pDestPtr, 1)) //<> IsByte
        {
            switch (state) {
            case READING_WHITESPACE:
                // It's just more whitespace.
                break;

            case READING_NAME:
                // Add a new attribute.
                err = AddAttribute(
                            nameBuffer,
                            pDestPtr - nameBuffer,
                            &pCurrentAttribute,
                            &pLastAttribute);
                if (err) {
                    gotoErr(err);
                }

                // Skip all remaining whitespace.
                state = READING_WHITESPACE;
                pDestPtr = nameBuffer;
                fExpectingValue = false;
                break;

            case READING_VALUE:
                // Finish the value.
                valueLength = (int32) (pAsyncIOStream->GetPosition() - startValuePosition);
                if (valueLength < 0) {
                    valueLength = valueLength;
                }
                pCurrentAttribute->m_Value.SetRangeValue(startValuePosition, valueLength);
                pCurrentAttribute = NULL;
                fExpectingValue = false;

                // Skip any additional whitespace.
                state = READING_WHITESPACE;
                pDestPtr = nameBuffer;
                break;

            case READING_QUOTED_VALUE:
                // This is just part of the quoted value.
                pDestPtr += charSize;
                break;

            default:
                break;
            }; // switch (state)
        } // Whitespace

        /////////////////////////////////////////////////////
        // This is an equal char.
        else if (*pDestPtr == '=') {
            switch (state) {
            case READING_WHITESPACE:
                fExpectingValue = true;
                break;

            case READING_NAME:
                // Add a new attribute.
                err = AddAttribute(
                            nameBuffer,
                            pDestPtr - nameBuffer,
                            &pCurrentAttribute,
                            &pLastAttribute);
                if (err) {
                    gotoErr(err);
                }
                fExpectingValue = true;

                // Skip all remaining whitespace.
                state = READING_WHITESPACE;
                pDestPtr = nameBuffer;
                break;

            case READING_QUOTED_VALUE:
                // This is just part of the quoted value.
                pDestPtr += charSize;
                break;

            case READING_VALUE:
                // This is really a syntax error, but just treat it as part
                // of the value. It may be an unquoted URL value that includes
                // fragment or query suffixes.
                pDestPtr += charSize;
                break;

            default:
                break;
            }; // switch (state)
        }
        /////////////////////////////////////////////////////
        // This is a quote char.
        //
        // It seems that browsers treat single and double quotes as
        // functionally the same, although only a single can close a
        // single, and only a double can close a double.
        else if ((*pDestPtr == '\"') || (*pDestPtr == '\'')) {
            // If this starts a quote, then remember how we started.
            // Quotes can nest, and only a single can close a
            // single, and only a double can close a double.
            if (READING_QUOTED_VALUE != state) {
                openQuoteChar = *pDestPtr;
            }

            switch (state) {
            case READING_WHITESPACE:
                if (fExpectingValue) {
                    state = READING_QUOTED_VALUE;
                    startValuePosition = pAsyncIOStream->GetPosition() - charSize;
                    pDestPtr += charSize;
                }
                else {
                    err = AddAttribute(
                                nameBuffer,
                                pDestPtr - nameBuffer,
                                &pCurrentAttribute,
                                &pLastAttribute);
                    if (err)
                    {
                        gotoErr(err);
                    }
                    state = READING_NAME;
                    pDestPtr = nameBuffer;
                }
                break;

            case READING_NAME:
                err = AddAttribute(
                            nameBuffer,
                            pDestPtr - nameBuffer,
                            &pCurrentAttribute,
                            &pLastAttribute);
                if (err) {
                    gotoErr(err);
                }
                state = READING_NAME;
                pDestPtr = nameBuffer;
                break;

            case READING_QUOTED_VALUE:
                if (*pDestPtr == openQuoteChar) {
                    // Finish the value.
                    valueLength = (int32) (pAsyncIOStream->GetPosition() - startValuePosition);
                    if (valueLength < 0)
                    {
                        valueLength = valueLength;
                    }
                    pCurrentAttribute->m_Value.SetRangeValue(startValuePosition, valueLength);

                    // Get ready for the next attribute.
                    state = READING_WHITESPACE;
                    pDestPtr = nameBuffer;
                    fExpectingValue = false;
                }
                break;

            case READING_VALUE:
                // This is probably a syntax error, but we treat it as part
                // of the value. Switch state so we go to the end of the quote
                // and don't stop at the next space char.
                pDestPtr += charSize;
                state = READING_QUOTED_VALUE;
                break;

            default:
                break;
            }; // switch (state)
        }

        /////////////////////////////////////////////////////
        // This is a normal text char.
        else {
            switch (state) {
            case READING_WHITESPACE:
                if (fExpectingValue) {
                    state = READING_VALUE;
                    startValuePosition = pAsyncIOStream->GetPosition() - charSize;
                    pDestPtr = nameBuffer + charSize;
                }
                else {
                    state = READING_NAME;
                    pDestPtr += charSize;
                }
                break;

            case READING_NAME:
            case READING_QUOTED_VALUE:
            case READING_VALUE:
                pDestPtr += charSize;
                break;

            default:
                break;
            }; // switch (state)
        }
    } // while (1)

    m_NodeFlags |= CPolyXMLNode::LAZY_ATTRIBUTES_PARSED;

abort:
    returnErr(err);
} // ParseAttributes








/////////////////////////////////////////////////////////////////////////////
//
// [AddAttribute]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleXMLNode::AddAttribute(
                char *pName,
                int32 nameLength,
                CSimpleXMLAttribute **ppResultAttribute,
                CSimpleXMLAttribute **ppLastAttribute) {
    ErrVal err = ENoErr;
    CSimpleXMLAttribute *pAttribute = NULL;
    char *pEndName;
    char *pEndNamespace;
    RunChecks();

    *ppResultAttribute = NULL;

    pAttribute = newex CSimpleXMLAttribute;
    if (NULL == pAttribute) {
        gotoErr(EFail);
    }


    // Check if this starts with a namespace
    pEndName = pName + nameLength;
    pEndNamespace = pName;
    while ((pEndNamespace < pEndName) && (':' != *pEndNamespace)) {
        pEndNamespace++;
    }
    if ((pEndNamespace < pEndName) && (':' == *pEndNamespace)) {
        pAttribute->m_pNamespace = m_pParentDocument->GetNamespace(pName, pEndNamespace - pName);

        // If the namespace is not declared, then ignore it.
        // Otherwise, ignore the namespace and treat the whole name as a
        // single name.
        //
        // A lot of web pages have a common html error, where they declare
        // a namespace with xml:lang, rather than xmlns:lang. This does not
        // match the built-in "xmlns" namespace, so we also cannot declare
        // the new "lang" namespace. So, we treat the whole name "xml:lang"
        // and names like "lang:foo" as a single name without namespaces.
        if (NULL != pAttribute->m_pNamespace) {
            pName = pEndNamespace + 1;
            nameLength = pEndName - pName;
        }
    }

    // Record the name.
    pAttribute->m_pName = m_pParentDocument->m_pNameList->AddDictionaryEntry(pName, nameLength);
    if (NULL == pAttribute->m_pName) {
        delete pAttribute;
        gotoErr(EFail);
    }

    *ppResultAttribute = pAttribute;
    if (NULL != *ppLastAttribute) {
        (*ppLastAttribute)->m_pNextAttribute = pAttribute;
    } else
    {
        m_NodeBody.Element.m_pAttributeList = pAttribute;
    }
    *ppLastAttribute = pAttribute;


abort:
    returnErr(err);
} // AddAttribute






/////////////////////////////////////////////////////////////////////////////
//
// [GetContentPtr]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleXMLNode::GetContentPtr(
                        int32 offsetIntoNode,
                        char **ppContentPtr,
                        int32 *pActualLength,
                        bool *pEndOfContent) {
    ErrVal err = ENoErr;
    int64 startPosInStream = 0;
    int64 stopPosInStream = 0;
    int32 requestedLength = 0;
    int32 actualLength = 0;
    char *pStr;

    if ((offsetIntoNode < 0) || (NULL == ppContentPtr)) {
       gotoErr(EFail);
    }

    ////////////////////////////////////////
    if ((XML_NODE_TEXT == m_NodeType)
            && (m_NodeBody.Text.m_pText)) {
        pStr = m_NodeBody.Text.m_pText;
        if (offsetIntoNode > 0) {
            pStr = pStr + offsetIntoNode;
        }

        *ppContentPtr = pStr;
        actualLength = m_NodeSize;
    }
    ////////////////////////////////////////
    else if ((m_pParentDocument)
               && (m_pParentDocument->m_pAsyncIOStream)) {
        startPosInStream = m_StartPosition + offsetIntoNode;
        stopPosInStream = m_StartPosition + m_NodeSize;
        requestedLength = (int32) (stopPosInStream - startPosInStream);

        err = m_pParentDocument->m_pAsyncIOStream->GetPtrRef(
                                                         startPosInStream,
                                                         requestedLength,
                                                         ppContentPtr,
                                                         &actualLength);
        if (err) {
            gotoErr(err);
        }

        if ((offsetIntoNode + actualLength) > m_NodeSize) {
            actualLength = m_NodeSize - offsetIntoNode;
        }
    }
    ////////////////////////////////////////
    else
    {
       gotoErr(EFail);
    }

    if (pEndOfContent) {
        *pEndOfContent = ((actualLength >= requestedLength) || (actualLength <= 0));
    }
    if (pActualLength) {
        *pActualLength = actualLength;
    }

abort:
    returnErr(err);
} // GetContentPtr







/////////////////////////////////////////////////////////////////////////////
//
// [GetNamedChild]
//
/////////////////////////////////////////////////////////////////////////////
CPolyXMLNode *
CSimpleXMLNode::GetNamedChild(const char *pNamePath) {
    CSimpleXMLNode *pCurrentNode;
    const char *pStartName;
    const char *pEndName;
    int32 nameLength;


    if (NULL == pNamePath) {
       return(NULL);
    }

    // Each iteration of this loop finds the next child.
    // The name may be a path, like "aaa/bbb/ccc/ddd", so
    // each iteration of this outer loop finds the child
    // corresponding to one element name in the path.
    pStartName = pNamePath;
    pCurrentNode = this;
    while (*pStartName) {
       pEndName = pStartName;
       while ((*pEndName) && ('/' != *pEndName) && ('\\' != *pEndName)) {
          pEndName++;
       }
       nameLength = pEndName - pStartName;

       // Find the child with the current element name.
       // Look through all children of the current node.
       pCurrentNode = pCurrentNode->m_pFirstChild;
       while (NULL != pCurrentNode) {
           if ((XML_NODE_ELEMENT == pCurrentNode->m_NodeType)
                  && (NULL != pCurrentNode->m_pName)
                  && (nameLength == pCurrentNode->m_pName->m_NameLength)
                  && (0 == strncasecmpex(pCurrentNode->m_pName->m_pName, pStartName, nameLength))) {
               break;
           }
           pCurrentNode = pCurrentNode->m_pNextSibling;
       } // while (NULL != pCurrentNode)

       // Quit the first time we cannot find a child.
       if (NULL == pCurrentNode) {
          return(NULL);
       }

       pStartName = pEndName;
       while ((*pStartName)
               && (('/' == *pStartName) || ('\\' == *pStartName))) {
          pStartName++;
       }
    } // while (pStartName)


    // If we didn't quit after not finding a child, then we stopped
    // the loop when we hit the end of the path. So, return what
    // we finally found.
    return(pCurrentNode);
} // GetNamedChild







/////////////////////////////////////////////////////////////////////////////
//
// [GetNamedSibling]
//
/////////////////////////////////////////////////////////////////////////////
CPolyXMLNode *
CSimpleXMLNode::GetNamedSibling(const char *pName) {
    CSimpleXMLNode *pCurrentNode;
    int32 nameLength;

    if (NULL == pName) {
       return(NULL);
    }

    pCurrentNode = m_pNextSibling;
    nameLength = strlen(pName);
    while (NULL != pCurrentNode) {
        if ((XML_NODE_ELEMENT == pCurrentNode->m_NodeType)
            && (NULL != pCurrentNode->m_pName)
            && (nameLength == pCurrentNode->m_pName->m_NameLength)
            && (0 == strncasecmpex(pCurrentNode->m_pName->m_pName, pName, nameLength))) {
            return(pCurrentNode);
        }
        pCurrentNode = pCurrentNode->m_pNextSibling;
    } // while (NULL != pCurrentNode)

    return(NULL);
} // GetNamedSibling







/////////////////////////////////////////////////////////////////////////////
//
// [GetChildByType]
//
/////////////////////////////////////////////////////////////////////////////
CPolyXMLNode *
CSimpleXMLNode::GetChildByType(int32 childType) {
    CSimpleXMLNode *pCurrentNode;

    pCurrentNode = m_pFirstChild;
    while (NULL != pCurrentNode) {
        if (childType == pCurrentNode->m_NodeType) {
            return(pCurrentNode);
        }
        pCurrentNode = pCurrentNode->m_pNextSibling;
    } // while (NULL != pCurrentNode)

    return(NULL);
} // GetChildByType







/////////////////////////////////////////////////////////////////////////////
//
// [GetChildElementValue]
//
/////////////////////////////////////////////////////////////////////////////
int32
CSimpleXMLNode::GetChildElementValue(const char *pName, char *pBuffer, int32 maxBufferLength) {
    ErrVal err = ENoErr;
    CSimpleXMLNode *pCurrentNode;
    CSimpleXMLNode *pValueNode;
    int32 offsetIntoNode = 0;
    char *pDestPtr;
    char *pEndDestPtr;
    char *pSrcPtr;
    int32 length;
    bool fEndOfContent;


    if ((NULL == pBuffer) || (maxBufferLength <= 0)) {
        return(-1);
    }

    // Leave room for a null terminator.
    maxBufferLength = maxBufferLength - 1;


    pValueNode = (CSimpleXMLNode *) GetNamedChild(pName);
    if (NULL == pValueNode) {
       return(-1);
    }

    // Look through all children of the current node.
    pCurrentNode = pValueNode->m_pFirstChild;
    while (NULL != pCurrentNode) {
        if (XML_NODE_TEXT == pCurrentNode->m_NodeType) {
            pDestPtr = pBuffer;
            pEndDestPtr = pBuffer + maxBufferLength;
            offsetIntoNode = 0;
            fEndOfContent = false;
            while (true) {
                err = pCurrentNode->GetContentPtr(
                                          offsetIntoNode,
                                          &pSrcPtr,
                                          &length,
                                          &fEndOfContent);
                if (err) {
                   gotoErr(err);
                }

                if ((pDestPtr + length) >= pEndDestPtr) {
                   goto abort;
                }

                memcpy(pDestPtr, pSrcPtr, length);
                pDestPtr += length;

                if (fEndOfContent) {
                   *pDestPtr = 0;
                   return(pDestPtr - pBuffer);
                }
            } // while (1)
        } // if (XML_NODE_TEXT == pCurrentNode->m_NodeType)

        pCurrentNode = pCurrentNode->m_pNextSibling;
    } // while (NULL != pCurrentNode)

abort:
    return(-1);
} // GetChildElementValue







/////////////////////////////////////////////////////////////////////////////
//
// [AddNamedChild]
//
/////////////////////////////////////////////////////////////////////////////
CPolyXMLNode *
CSimpleXMLNode::AddNamedChild(const char *pNameStr) {
    ErrVal err = ENoErr;
    CDictionaryEntry *pName = NULL;
    CSimpleXMLNode *pNode = NULL;
    int32 nameLength;
    RunChecks();

    if ((NULL == pNameStr)
       || (NULL == m_pParentDocument)
       || (NULL == m_pParentDocument->m_pNameList)) {
        gotoErr(EFail);
    }


    nameLength = strlen(pNameStr);
    pName = m_pParentDocument->m_pNameList->AddDictionaryEntry(pNameStr, nameLength);
    if (NULL == pName) {
        gotoErr(EFail);
    }

    pNode = newex CSimpleXMLNode;
    if (NULL == pNode) {
        gotoErr(EFail);
    }

    pNode->m_pParentDocument = m_pParentDocument;
    pNode->m_NodeType = CPolyXMLNode::XML_NODE_ELEMENT;
    pNode->m_pName = pName;
    pNode->m_StartPosition = -1;
    pNode->m_NodeFlags |= CPolyXMLNode::LAZY_ATTRIBUTES_PARSED;

    pNode->m_NodeSize = 0;
    pNode->m_NodeBody.Element.m_OffsetToName = 0;
    pNode->m_NodeBody.Element.m_pAttributeList = NULL;

    InsertLastChild(pNode);

abort:
    return(pNode);
} // AddNamedChild







/////////////////////////////////////////////////////////////////////////////
//
// [SetChildElementValue]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleXMLNode::SetChildElementValue(const char *pNameStr, char *pBuffer, int32 bufferLength) {
    ErrVal err = ENoErr;
    CSimpleXMLNode *pElementNode = NULL;
    CSimpleXMLNode *pValueNode = NULL;
    RunChecks();

    if ((NULL == pNameStr)
       || (NULL == m_pParentDocument)
       || (NULL == m_pParentDocument->m_pNameList)
       || (NULL == pBuffer)) {
        gotoErr(EFail);
    }

    if (bufferLength < 0) {
        bufferLength = strlen(pBuffer);
    }

    pElementNode = (CSimpleXMLNode *) AddNamedChild(pNameStr);
    if (NULL == pElementNode) {
        gotoErr(EFail);
    }

    pValueNode = newex CSimpleXMLNode;
    if (NULL == pValueNode) {
        gotoErr(EFail);
    }

    pValueNode->m_pParentDocument = m_pParentDocument;
    pValueNode->m_NodeType = CPolyXMLNode::XML_NODE_TEXT;
    pValueNode->m_pName = m_pParentDocument->m_TextNodeName;
    pValueNode->m_NodeFlags |= CAN_DELETE_TAG_BODY;
    pValueNode->m_StartPosition = -1;
    pValueNode->m_NodeSize = bufferLength;
    pValueNode->m_NodeBody.Text.m_pText = (char *) memAlloc(bufferLength + 1);
    if (NULL != pValueNode->m_NodeBody.Text.m_pText) {
       memcpy(pValueNode->m_NodeBody.Text.m_pText, pBuffer, bufferLength);
       pValueNode->m_NodeBody.Text.m_pText[bufferLength] = 0;
    } else
    {
       pValueNode->m_NodeSize = 0;
    }

    pElementNode->InsertLastChild(pValueNode);

abort:
    returnErr(err);
} // SetChildElementValue









/////////////////////////////////////////////////////////////////////////////
// [CSimpleXMLAttribute]
/////////////////////////////////////////////////////////////////////////////
CSimpleXMLAttribute::CSimpleXMLAttribute() {
    m_pName = NULL;
    m_pNamespace = NULL;

    m_AttrFlags = 0;

    m_ValuePosition = -1;
    m_ValueSize = 0;

    m_pNextAttribute = NULL;
} // CSimpleXMLAttribute






/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
CXMLNamespace::CXMLNamespace() {
    m_pName = NULL;
    m_pUrl = NULL;
    m_pDeclarationElement = NULL;
    m_pNextNamespace = NULL;
} // CXMLNamespace




/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
CXMLNamespace::~CXMLNamespace() {
    memFree(m_pUrl);
} // ~CXMLNamespace





/////////////////////////////////////////////////////////////////////////////
//
// [CPropertyValue]
//
/////////////////////////////////////////////////////////////////////////////
CPropertyValue::CPropertyValue() {
    m_ValueType = PROPERTY_UNKNOWN;
    m_ValueFlags = 0;

    // Make sure to initialize enough of the union so all fields are initialized.
    // If one union selector is shorter than another, then it will only initialize
    // part of the data.
    m_Value.m_Int64Value = 0;
    m_Value.m_StringValue.m_pString = NULL;
    m_Value.m_StringValue.m_Length = 0;
    m_Value.m_RangeValue.m_Offset = 0;
    m_Value.m_RangeValue.m_Length = 0;

    m_pNextValue = NULL;
} // CPropertyValue




/////////////////////////////////////////////////////////////////////////////
//
// [CPropertyValue]
//
/////////////////////////////////////////////////////////////////////////////
CPropertyValue::~CPropertyValue() {
    DiscardValue();
} // ~CPropertyValue





/////////////////////////////////////////////////////////////////////////////
//
// [DiscardValue]
//
/////////////////////////////////////////////////////////////////////////////
void
CPropertyValue::DiscardValue() {
    m_ValueType = PROPERTY_UNKNOWN;
    m_ValueFlags = 0;
} // DiscardValue










/////////////////////////////////////////////////////////////////////////////
//
// [SetRangeValue]
//
/////////////////////////////////////////////////////////////////////////////
void
CPropertyValue::SetRangeValue(int64 offset, int32 length) {
    // Discard the old value if it needs it.
    DiscardValue();

    m_ValueType = PROPERTY_RANGE;
    m_Value.m_RangeValue.m_Offset = offset;
    m_Value.m_RangeValue.m_Length = length;
} // SetRangeValue








/////////////////////////////////////////////////////////////////////////////
//
// [GetStringValue]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPropertyValue::GetStringValue(char **pResultValue, int32 *pResultLength) {
   ErrVal err = ENoErr;

   if (PROPERTY_STRING != m_ValueType) {
      gotoErr(EWrongAttributeType);
   }

   if (NULL != pResultValue) {
      *pResultValue = m_Value.m_StringValue.m_pString;
   }
   if (NULL != pResultLength) {
      *pResultLength = m_Value.m_StringValue.m_Length;
   }

abort:
   returnErr(err);
} // GetStringValue







/////////////////////////////////////////////////////////////////////////////
//
// [GetRangeValue]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPropertyValue::GetRangeValue(int64 *pOffset, int32 *pLength) {
    ErrVal err = ENoErr;

    if ((NULL == pOffset) || (NULL == pLength)) {
        gotoErr(EInvalidArg);
    }

    if (PROPERTY_RANGE != m_ValueType) {
        gotoErr(EWrongAttributeType);
    }

    *pOffset = m_Value.m_RangeValue.m_Offset;
    *pLength = m_Value.m_RangeValue.m_Length;

abort:
    returnErr(err);
} // GetRangeValue





/////////////////////////////////////////////////////////////////////////////
//
// [CheckState]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleXMLDoc::CheckState() {
    return(ENoErr);
} // CheckState.

