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

#ifndef _POLY_XDOC_H_
#define _POLY_XDOC_H_

class CPolyXMLDoc;
class CPolyXMLNode;
class CPolyXMLDocAttribute;
class CPolyHttpStream;
class CPropertyValue;


#define DEBUG_XML 0



/////////////////////////////////////////////////////////////////////////////
// These are the types of each individual name and value.

typedef enum {
   PROPERTY_UNKNOWN,
   PROPERTY_INTEGER,
   PROPERTY_BOOL,
   PROPERTY_FLOAT,
   PROPERTY_STRING,
   PROPERTY_BUFFER,
   PROPERTY_INT64,
   PROPERTY_RANGE,
} PropertyValueType;



/////////////////////////////////////////////////////////////////////////////
// This is a single named and typed value.
class CPropertyValue {
public:
    CPropertyValue();
    virtual ~CPropertyValue();
    NEWEX_IMPL()

    /////////////////////////////////
    // GET Functions
    ErrVal GetStringValue(char **pResultValue, int32 *pResultLength);
    ErrVal GetRangeValue(int64 *pOffset, int32 *pLength);

    /////////////////////////////////
    // SET Functions
    void SetRangeValue(int64 offset, int32 length);

    void DiscardValue();

    PropertyValueType   m_ValueType;
    CPropertyValue      *m_pNextValue;
    char                m_ValueFlags;

    ////////////////////////////////////////
    // The value
    union {
        int32               m_IntValue;
        int64               m_Int64Value;
        float               m_FloatValue;
        bool                m_BoolValue;

        struct StringValue {
            char           *m_pString;
            int32          m_Length;
        } m_StringValue;

        struct RangeValue {
            int64          m_Offset;
            int32          m_Length;
        } m_RangeValue;
    } m_Value;

    // These are flags for string values.
    enum {
        USED            = 0x0002,
        CHECKED         = 0x0004,
    };
}; // CPropertyValue







/////////////////////////////////////////////////////////////////////////////
// This describes one XML document
/////////////////////////////////////////////////////////////////////////////
class CPolyXMLDoc : public CRefCountInterface {
public:
#if INCLUDE_REGRESSION_TESTS
    static void TestXMLModule();
#endif

    CPolyXMLDoc();
    virtual ~CPolyXMLDoc();

    // Writing
    ErrVal WriteDocToStream(CAsyncIOStream *pOutStream, int32 writeOptions);

    // Get the top-level parts of a document.
    virtual CPolyXMLNode *GetRoot() { return(NULL); }
    virtual ErrVal GetIOStream(
                        CAsyncIOStream **ppSrcStream,
                        int64 *pStartPosition,
                        int32 *pLength) = 0;

    CParsedUrl          *m_pUrl;
}; // CPolyXMLDoc

ErrVal OpenSimpleXMLFile(const char *pFileName, CPolyXMLDoc **ppResult);
ErrVal OpenSimpleXMLFromHTTP(CPolyHttpStream *pHTTPStream, CPolyXMLDoc **ppResult);
ErrVal ParseSimpleXMLBuffer(char *pTextBuffer, int32 bufferLength, CPolyXMLDoc **ppResult);




/////////////////////////////////////////////////////////////////////////////
// This describes one node, which can be some text, an element, processing
// instruction, or something else.
/////////////////////////////////////////////////////////////////////////////
class CPolyXMLNode : public CRefCountInterface {
public:
      //////////////////////////////////
      enum XMLNodeType {
         XML_NODE_ELEMENT                = 0,
         XML_NODE_TEXT                   = 1,
         XML_NODE_COMMENT                = 2,
         XML_NODE_PROCESSING_INSTRUCTION = 3,
         XML_NODE_DOCUMENT               = 4,
         XML_NODE_ATTRIBUTE              = 6,
         XML_NODE_CDATA                  = 7,
      }; // XMLNodeType

      //////////////////////////////////
      enum XMLNodeFlags {
          SELF_CLOSING_ELEMENT                  = 0x0001,
          ELEMENT_HAS_SPECIAL_CLOSE             = 0x0004,
          ELEMENT_HAS_NO_CLOSE                  = 0x0008,
          LAZY_ATTRIBUTES_PARSED                = 0x0010, // Internal implementation flag.
          SELF_CLOSING_PROCESSING_INSTRUCTION   = 0x0020,
          ELEMENT_HAS_NO_ENDING_CHAR            = 0X0040,
          UNBALANCED_CLOSE_ELEMENT              = 0X0080,
      }; // XMLNodeFlags

    // Tree Navigation
    virtual CPolyXMLNode *GetFirstChild() = 0;
    virtual CPolyXMLNode *GetNextSibling() = 0;
    virtual CPolyXMLNode *GetParent() = 0;
    virtual CPolyXMLNode *GetNamedChild(const char *pName) = 0;
    virtual CPolyXMLNode *GetNamedSibling(const char *pName) = 0;
    virtual CPolyXMLNode *GetChildByType(int32 childType) = 0;

    // Editing
    virtual CPolyXMLNode *AddNamedChild(const char *pName) = 0;

    // Accessors
    // There are some specialized accessors here, but they are here for SerializedObject.
    virtual int32 GetChildElementValue(const char *pName, char *pBuffer, int32 maxBufferLength) = 0;
    virtual ErrVal SetChildElementValue(const char *pName, char *pBuffer, int32 bufferLength) = 0;
    virtual char *GetChildStringValue(const char *pName, const char *pDefaultVal);
    virtual int32 GetChildIntegerValue(const char *pName, int32 defaultVal);

    // Properties of this node.
    virtual CPolyXMLNode::XMLNodeType GetNodeType() = 0;
    virtual int32 GetNodeFlags() = 0;
    virtual void GetNamespace(const char **ppName, int32 *pNameLength) = 0;
    virtual void GetName(const char **ppName, int32 *pNameLength) = 0;
    virtual const char *GetSpecialCloseName() = 0;
    virtual CPolyXMLDocAttribute *GetFirstAttribute() = 0;

    virtual ErrVal GetContentPtr(
                        int32 offsetIntoNode,
                        char **ppContentPtr,
                        int32 *pActualLength,
                        bool *pEndOfContent) = 0;

#if DEBUG_XML
    virtual void SetDebugMode(int opCode) { opCode = opCode; };
    int32       m_DebugNodeId;
    int64       m_StartWritePos;
    int64       m_StopWritePos;
#endif // DEBUG_XML
}; // CPolyXMLNode



/////////////////////////////////////////////////////////////////////////////
// This describes one attribute of an element.
/////////////////////////////////////////////////////////////////////////////
class CPolyXMLDocAttribute : public CPolyXMLNode {
public:
    virtual CPropertyValue *GetValue() = 0;
    virtual CPolyXMLDocAttribute *GetNextAttribute() = 0;
}; // CPolyXMLDocAttribute


#endif // _POLY_XDOC_H_

