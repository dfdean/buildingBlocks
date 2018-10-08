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
// Serialized Object Base Class
//
//
// This is a base class. Objects that inherit from this class can implement
// a single method (sort of like a RPC serializer function) that will serialize
// or deserialize all the fields of the object. This overloaded method
// simply contains macros for each member variable of the object. Clients can
// then call methods on the base class and serialize or deserialize an
// object to differetn backings, like XML or property lists.
// This class is used for things like reading and writing the contents of
// SOAP function calls.
//
// This is a schema that is used for both serializing and deserializing.
// A typical method body might look like this:
//
// SERIALIZE_INTEGER(&m_foo, "Foo");
// SERIALIZE_STRING(&m_pBar, "Bar");
// SERIALIZE_BOOL(&m_pBop, "Bop");
// SERIALIZE_FLAG(&m_pBop, SAMPLE_FLAG_DEFINE, "Bop");
//
// You can also have sections that are only used for some schema.
//
// START_NAMED_SCHEMA("xml");
//    SERIALIZE_INTEGER(&m_foo, "Foo");
//    SERIALIZE_STRING(&m_pBar, "Bar");
//    SERIALIZE_BOOL(&m_pBop, "Bop");
//    SERIALIZE_FLAG(&m_pBop, SAMPLE_FLAG_DEFINE, "Bop");
// STOP_NAMED_SCHEMA();
//
// To Do:
// Serializing a list can build up strings rather than append new items.
//   Add a new TRANSFER_OBJECT_MERGE which just does a transfer object and sets a flag.
//   TransferString appends strings if they already exist.
//   Transfer bool and int do nothing.
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
#include "polyXMLDoc.h"
#include "serializedObject.h"

FILE_DEBUGGING_GLOBALS(LOG_LEVEL_DEFAULT, 0);





////////////////////////////////////////////////////////////////////////////////
//
// [CSerializedObject]
//
////////////////////////////////////////////////////////////////////////////////
CSerializedObject::CSerializedObject() {
    m_SerializeDirection = SERIALIZE_OBJECT_TO_BACKING;
    m_SerializeBackingType = TEXT_BUFFER_BACKING;
    m_SerializedIndentLevel = 0;

    m_pSerializeStartBuffer = NULL;
    m_pSerializeStopBuffer = NULL;
    m_pSerializeBuffer = NULL;
    m_ppSerializeNextPointer = NULL;

    m_pSerializeXMLNode = NULL;
} // CSerializedObject






////////////////////////////////////////////////////////////////////////////////
//
// [~CSerializedObject]
//
////////////////////////////////////////////////////////////////////////////////
CSerializedObject::~CSerializedObject() {
    memFree(m_pSerializeStartBuffer);
}





////////////////////////////////////////////////////////////////////////////////
//
// [WriteToXMLFile]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CSerializedObject::WriteToXMLFile(
                           const char *pPathName,
                           const char *pTopLevelElement) {
    ErrVal err = ENoErr;
    CSimpleFile fileHandle;
    const char *pFixedString;
    int32 bufferLength;

    if ((NULL == pPathName) || (NULL == pTopLevelElement)) {
        gotoErr(EFail);
    }

    m_SerializeDirection = SERIALIZE_OBJECT_TO_BACKING;
    m_SerializeBackingType = TEXT_BUFFER_BACKING;
    m_SerializedIndentLevel = 0;
    m_pSerializeStartBuffer = NULL;
    m_pSerializeStopBuffer = NULL;
    m_pSerializeBuffer = NULL;
    m_ppSerializeNextPointer = NULL;

    err = Serialize();
    if (err) {
        gotoErr(err);
    }

    // Create the file and open it.
    err = fileHandle.OpenOrCreateEmptyFile(pPathName, 0);
    if (err) {
        gotoErr(err);
    }

    err = fileHandle.Seek(0, CSimpleFile::SEEK_START);
    if (err) {
        gotoErr(err);
    }

    pFixedString = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<";
    bufferLength = (int32) strlen(pFixedString);
    err = fileHandle.Write(pFixedString, bufferLength);
    if (err) {
        gotoErr(err);
    }

    bufferLength = (int32) strlen(pTopLevelElement);
    err = fileHandle.Write(pTopLevelElement, bufferLength);
    if (err) {
        gotoErr(err);
    }

    pFixedString = ">\n";
    bufferLength = (int32) strlen(pFixedString);
    err = fileHandle.Write(pFixedString, bufferLength);
    if (err) {
        gotoErr(err);
    }

    bufferLength = (int32) (m_pSerializeBuffer - m_pSerializeStartBuffer);
    err = fileHandle.Write(m_pSerializeStartBuffer, bufferLength);
    if (err) {
        gotoErr(err);
    }

    pFixedString = "\n</";
    bufferLength = (int32) strlen(pFixedString);
    err = fileHandle.Write(pFixedString, bufferLength);
    if (err) {
        gotoErr(err);
    }

    bufferLength = (int32) strlen(pTopLevelElement);
    err = fileHandle.Write(pTopLevelElement, bufferLength);
    if (err) {
        gotoErr(err);
    }

    pFixedString = ">\n\n\n";
    bufferLength = (int32) strlen(pFixedString);
    err = fileHandle.Write(pFixedString, bufferLength);
    if (err) {
        gotoErr(err);
    }

abort:
    fileHandle.Close();

    return(err);
} // WriteToXMLFile







////////////////////////////////////////////////////////////////////////////////
//
// [ReadFromXMLFile]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CSerializedObject::ReadFromXMLFile(
                            const char *pPathName,
                            const char *pTopLevelElement) {
    ErrVal err = ENoErr;
    CPolyXMLDoc *pXMLDoc = NULL;
    CPolyXMLNode *pRoot = NULL;
    CPolyXMLNode *pXMLNode = NULL;


    if ((NULL == pPathName) || (NULL == pTopLevelElement)) {
        gotoErr(EFail);
    }

    err = OpenSimpleXMLFile(pPathName, &pXMLDoc);
    if (err) {
        gotoErr(err);
    }

    pRoot = pXMLDoc->GetRoot();
    if (NULL == pRoot) {
        gotoErr(EFail);
    }

    pXMLNode = pRoot->GetNamedChild(pTopLevelElement);
    if (NULL == pXMLNode) {
        gotoErr(EFail);
    }

    err = ReadFromXMLNode(pXMLNode);

abort:
    RELEASE_OBJECT(pXMLDoc);

    return(err);
} // ReadFromXMLFile





////////////////////////////////////////////////////////////////////////////////
//
// [ReadFromXMLBuffer]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CSerializedObject::ReadFromXMLBuffer(
                            const char *pDataBuffer,
                            const char *pTopLevelElement) {
    ErrVal err = ENoErr;
    CPolyXMLDoc *pXMLDoc = NULL;
    CPolyXMLNode *pRoot = NULL;
    CPolyXMLNode *pXMLNode = NULL;
    int32 bufferLength;

    if ((NULL == pDataBuffer) || (NULL == pTopLevelElement)) {
        gotoErr(EFail);
    }

    bufferLength = strlen(pDataBuffer);
    err = ParseSimpleXMLBuffer((char *) pDataBuffer, bufferLength, &pXMLDoc);
    if (err) {
        gotoErr(err);
    }

    pRoot = pXMLDoc->GetRoot();
    if (NULL == pRoot) {
        gotoErr(EFail);
    }

    pXMLNode = pRoot->GetNamedChild(pTopLevelElement);
    if (NULL == pXMLNode) {
        gotoErr(EFail);
    }

    err = ReadFromXMLNode(pXMLNode);

abort:
    RELEASE_OBJECT(pXMLDoc);
    return(err);
} // ReadFromXMLBuffer





////////////////////////////////////////////////////////////////////////////////
//
// [WriteToXMLNode]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CSerializedObject::WriteToXMLNode(CPolyXMLNode *pXMLNode) {
    ErrVal err = ENoErr;

    m_SerializeDirection = SERIALIZE_OBJECT_TO_BACKING;
    m_SerializeBackingType = XML_BACKING;
    m_pSerializeXMLNode = pXMLNode;
    m_ppSerializeNextPointer = NULL;

    err = Serialize();
    return(err);
} // WriteToXMLNode




////////////////////////////////////////////////////////////////////////////////
//
// [ReadFromXMLNode]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CSerializedObject::ReadFromXMLNode(CPolyXMLNode *pXMLNode) {
    ErrVal err = ENoErr;

    m_SerializeDirection = DESERIALIZE_OBJECT_FROM_BACKING;
    m_SerializeBackingType = XML_BACKING;
    m_pSerializeXMLNode = pXMLNode;
    m_ppSerializeNextPointer = NULL;

    err = Serialize();
    return(err);
} // ReadFromXMLNode





////////////////////////////////////////////////////////////////////////////////
//
// [TransferString]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CSerializedObject::TransferString(
                        const char *pDataName,
                        char **pLocalData,
                        char *pDefaultValue) {
    ErrVal err = ENoErr;

    /////////////////////////////////////////////
    if (SERIALIZE_OBJECT_TO_BACKING == m_SerializeDirection) {
        // This may not be an error; it may just be a missing optional value.
        if (NULL == *pLocalData) {
            gotoErr(ENoErr);
        }

        switch (m_SerializeBackingType) {
        /////////////////////////
        case XML_BACKING:
            err = m_pSerializeXMLNode->SetChildElementValue(pDataName, *pLocalData, -1);
            break;

        /////////////////////////
        case TEXT_BUFFER_BACKING:
            err = WriteXMLToBuffer(pDataName, *pLocalData);
            break;

        /////////////////////////
        default:
            err = EFail;
            break;
        } // switch (m_SerializeBackingType)
    } // if (SERIALIZE_OBJECT_TO_BACKING == m_SerializeDirection)
    /////////////////////////////////////////////
    else { // if (DESERIALIZE_OBJECT_FROM_BACKING == m_SerializeDirection) {
        memFree(*pLocalData);
        *pLocalData = NULL;

        switch (m_SerializeBackingType) {
        /////////////////////////
        case XML_BACKING:
            // GetChildStringValue makes a copy of the string, so we don't have to do that here.
            *pLocalData = m_pSerializeXMLNode->GetChildStringValue(pDataName, pDefaultValue);

            // This may not be an error. It usually just means that a
            // param was missing. In this case, use the default.
            if ((NULL == *pLocalData) && (NULL != pDefaultValue)) {
                *pLocalData = strdupex(pDefaultValue);
                if (NULL == *pLocalData) {
                    err = EFail;
                }
            }
            break;

        /////////////////////////
        default:
            err = EFail;
            break;
        } // switch (m_SerializeBackingType)
    } // if (DESERIALIZE_OBJECT_FROM_BACKING == m_SerializeDirection)

abort:
    return(err);
} // TransferString






////////////////////////////////////////////////////////////////////////////////
//
// [TransferInteger]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CSerializedObject::TransferInteger(
                        const char *pDataName,
                        int32 *pLocalData,
                        int32 defaultValue) {
    ErrVal err = ENoErr;
    char numberBuffer[32];
    int32 strLength;

    /////////////////////////////////////////////
    if (SERIALIZE_OBJECT_TO_BACKING == m_SerializeDirection) {
        snprintf(numberBuffer, sizeof(numberBuffer), "%d", *pLocalData);
        strLength = strlen(numberBuffer);

        switch (m_SerializeBackingType) {
        /////////////////////////
        case XML_BACKING:
            err = m_pSerializeXMLNode->SetChildElementValue(pDataName, numberBuffer, strLength);
            break;

        /////////////////////////
        case TEXT_BUFFER_BACKING:
            err = WriteXMLToBuffer(pDataName, numberBuffer);
            break;

        /////////////////////////
        default:
            err = EFail;
            break;
        } // switch (m_SerializeBackingType)
    } // if (SERIALIZE_OBJECT_TO_BACKING == m_SerializeDirection)
    /////////////////////////////////////////////
    else { // if (DESERIALIZE_OBJECT_FROM_BACKING == m_SerializeDirection) {
        switch (m_SerializeBackingType) {
        /////////////////////////
        case XML_BACKING:
            *pLocalData = m_pSerializeXMLNode->GetChildIntegerValue(pDataName, defaultValue);
            break;

        /////////////////////////
        default:
            err = EFail;
            break;
        } // switch (m_SerializeBackingType)
    } // if (DESERIALIZE_OBJECT_FROM_BACKING == m_SerializeDirection)

    return(err);
} // TransferInteger





////////////////////////////////////////////////////////////////////////////////
//
// [TransferUnsignedInteger]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CSerializedObject::TransferUnsignedInteger(
                        const char *pDataName,
                        uint32 *pLocalData,
                        uint32 defaultValue) {
    ErrVal err = ENoErr;
    char numberBuffer[32];
    int32 strLength;

    /////////////////////////////////////////////
    if (SERIALIZE_OBJECT_TO_BACKING == m_SerializeDirection) {
        snprintf(numberBuffer, sizeof(numberBuffer), "%d", *pLocalData);
        strLength = strlen(numberBuffer);

        switch (m_SerializeBackingType) {
        /////////////////////////
        case XML_BACKING:
            err = m_pSerializeXMLNode->SetChildElementValue(pDataName, numberBuffer, strLength);
            break;

        /////////////////////////
        case TEXT_BUFFER_BACKING:
            err = WriteXMLToBuffer(pDataName, numberBuffer);
            break;

        /////////////////////////
        default:
            err = EFail;
            break;
        } // switch (m_SerializeBackingType)
    } // if (SERIALIZE_OBJECT_TO_BACKING == m_SerializeDirection)
    /////////////////////////////////////////////
    else { // if (DESERIALIZE_OBJECT_FROM_BACKING == m_SerializeDirection) {
        switch (m_SerializeBackingType) {
        /////////////////////////
        case XML_BACKING:
            *pLocalData = (uint32) (m_pSerializeXMLNode->GetChildIntegerValue(pDataName, defaultValue));
            break;

        /////////////////////////
        default:
            err = EFail;
            break;
        } // switch (m_SerializeBackingType)
    } // if (DESERIALIZE_OBJECT_FROM_BACKING == m_SerializeDirection)

    return(err);
} // TransferUnsignedInteger








////////////////////////////////////////////////////////////////////////////////
//
// [TransferInteger64]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CSerializedObject::TransferInteger64(
                        const char *pDataName,
                        int64 *pLocalData,
                        int64 defaultValue) {
    ErrVal err = ENoErr;
    char numberBuffer[32];
    int32 strLength;
    int32 int32Value;


    /////////////////////////////////////////////
    if (SERIALIZE_OBJECT_TO_BACKING == m_SerializeDirection) {
        int32Value = (int32) (*pLocalData);
        snprintf(numberBuffer, sizeof(numberBuffer), "%d", int32Value);
        strLength = strlen(numberBuffer);

        switch (m_SerializeBackingType) {
        /////////////////////////
        case XML_BACKING:
            err = m_pSerializeXMLNode->SetChildElementValue(pDataName, numberBuffer, strLength);
            break;

        /////////////////////////
        case TEXT_BUFFER_BACKING:
            err = WriteXMLToBuffer(pDataName, numberBuffer);
            break;

        /////////////////////////
        default:
            err = EFail;
            break;
        } // switch (m_SerializeBackingType)
    } // if (SERIALIZE_OBJECT_TO_BACKING == m_SerializeDirection)
    /////////////////////////////////////////////
    else { // if (DESERIALIZE_OBJECT_FROM_BACKING == m_SerializeDirection) {
        switch (m_SerializeBackingType) {
        /////////////////////////
        case XML_BACKING:
            *pLocalData = m_pSerializeXMLNode->GetChildIntegerValue(pDataName, (int32) defaultValue);
            break;

        /////////////////////////
        default:
            err = EFail;
            break;
        } // switch (m_SerializeBackingType)
    } // if (DESERIALIZE_OBJECT_FROM_BACKING == m_SerializeDirection)

    return(err);
} // TransferInteger64






////////////////////////////////////////////////////////////////////////////////
//
// [TransferFloat]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CSerializedObject::TransferFloat(
                        const char *pDataName,
                        float *pLocalData,
                        float defaultValue) {
    ErrVal err = ENoErr;
    char numberBuffer[32];
    char *pStr;
    int32 strLength;


    /////////////////////////////////////////////
    if (SERIALIZE_OBJECT_TO_BACKING == m_SerializeDirection) {
        snprintf(numberBuffer, sizeof(numberBuffer), "%f", *pLocalData);
        strLength = strlen(numberBuffer);

        switch (m_SerializeBackingType) {
        /////////////////////////
        case XML_BACKING:
            err = m_pSerializeXMLNode->SetChildElementValue(pDataName, numberBuffer, strLength);
            break;

        /////////////////////////
        case TEXT_BUFFER_BACKING:
            err = WriteXMLToBuffer(pDataName, numberBuffer);
            break;

        /////////////////////////
        default:
            err = EFail;
            break;
        } // switch (m_SerializeBackingType)
    } // if (SERIALIZE_OBJECT_TO_BACKING == m_SerializeDirection)
    /////////////////////////////////////////////
    else { // if (DESERIALIZE_OBJECT_FROM_BACKING == m_SerializeDirection) {
        switch (m_SerializeBackingType) {
        /////////////////////////
        case XML_BACKING:
            pStr = m_pSerializeXMLNode->GetChildStringValue(pDataName, "");
            if ((NULL == pStr) || (0 == *pStr)) {
                *pLocalData = defaultValue;
            } else {
                float tempFloat;
                sscanf(pStr, "%f", &tempFloat);
                *pLocalData = tempFloat;
            }
            break;

        /////////////////////////
        default:
            err = EFail;
            break;
        } // switch (m_SerializeBackingType)
    } // if (DESERIALIZE_OBJECT_FROM_BACKING == m_SerializeDirection)

    return(err);
} // TransferFloat






////////////////////////////////////////////////////////////////////////////////
//
// [TransferBool]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CSerializedObject::TransferBool(
                        const char *pDataName,
                        bool *pLocalData,
                        bool defaultValue) {
    ErrVal err = ENoErr;
    char *pValue;
    char *pCopiedValue = NULL;

    *pLocalData = defaultValue;

    /////////////////////////////////////////////
    if (SERIALIZE_OBJECT_TO_BACKING == m_SerializeDirection) {
        if (*pLocalData) {
            pValue = (char *) "1";
        } else {
            pValue = (char *) "0";
        }

        switch (m_SerializeBackingType) {
        /////////////////////////
        case XML_BACKING:
            err = m_pSerializeXMLNode->SetChildElementValue(pDataName, pValue, 1);
            break;

        /////////////////////////
        case TEXT_BUFFER_BACKING:
            err = WriteXMLToBuffer(pDataName, pValue);
            break;

        /////////////////////////
        default:
            err = EFail;
            break;
        } // switch (m_SerializeBackingType)
    } // if (SERIALIZE_OBJECT_TO_BACKING == m_SerializeDirection)
    /////////////////////////////////////////////
    else { // if (DESERIALIZE_OBJECT_FROM_BACKING == m_SerializeDirection) {
        switch (m_SerializeBackingType) {
        /////////////////////////
        case XML_BACKING:
            pCopiedValue = m_pSerializeXMLNode->GetChildStringValue(pDataName, NULL);
            pValue = pCopiedValue;
            break;

        /////////////////////////
        default:
            err = EFail;
            goto abort;
            break;
        } // switch (m_SerializeBackingType)

        if (NULL == pValue) {
            // The value is missing, so just leave it as the default.
            goto abort;
        }
        if ((0 == strcasecmpex(pValue, "1"))
            || (0 == strcasecmpex(pValue, "true"))
            || (0 == strcasecmpex(pValue, "yes"))
            || (0 == strcasecmpex(pValue, "on"))) {
            *pLocalData = true;
        } else {
            *pLocalData = false;
        }
    } // if (DESERIALIZE_OBJECT_FROM_BACKING == m_SerializeDirection)

abort:
    memFree(pCopiedValue);
    return(err);
} // TransferBool






////////////////////////////////////////////////////////////////////////////////
//
// [GetNextSourceNode]
//
////////////////////////////////////////////////////////////////////////////////
void *
CSerializedObject::GetNextSourceNode(
                            const char *pListName,
                            const char *pName,
                            void *pPrevNode) {
    void *pResult = NULL;
    CPolyXMLNode *pParentNode;


    /////////////////////////////////////////////
    if (SERIALIZE_OBJECT_TO_BACKING == m_SerializeDirection) {
        ASSERT(0);
        goto abort;
    } // if (SERIALIZE_OBJECT_TO_BACKING == m_SerializeDirection)


    switch (m_SerializeBackingType) {
    /////////////////////////
    case XML_BACKING:
        pParentNode = m_pSerializeXMLNode;
        if ((pListName) && (NULL != m_pSerializeXMLNode)) {
            pParentNode = m_pSerializeXMLNode->GetNamedChild(pListName);
        }

        // There is no parent node. This just means the whole
        // list is missing, so we are done.
        if (NULL == pParentNode) {
            goto abort;
        }

        if (NULL == pPrevNode) {
            pResult = pParentNode->GetNamedChild(pName);
        } else {
            pResult = ((CPolyXMLNode *) pPrevNode)->GetNamedSibling(pName);
        }
        break;

    /////////////////////////
    default:
        pResult = NULL;
        break;
    } // switch (m_SerializeBackingType)

abort:
    return(pResult);
} // GetNextSourceNode







////////////////////////////////////////////////////////////////////////////////
//
// [GetNextDestNode]
//
////////////////////////////////////////////////////////////////////////////////
void *
CSerializedObject::GetNextDestNode(
                        const char *pListName,
                        const char *pName,
                        void *pPrevNode) {
    void *pResult = NULL;
    CPolyXMLNode *pParentNode;
    UNUSED_PARAM(pPrevNode);

    switch (m_SerializeBackingType) {
    /////////////////////////
    case XML_BACKING:
        pParentNode = m_pSerializeXMLNode;
        if ((pListName) && (NULL != m_pSerializeXMLNode)) {
            pParentNode = m_pSerializeXMLNode->GetNamedChild(pListName);
            if (NULL == pParentNode) {
                pParentNode = m_pSerializeXMLNode->AddNamedChild(pListName);
            }
        }

        if (NULL == pParentNode) {
            goto abort;
        }
        pResult = pParentNode->AddNamedChild(pName);
        break;

    /////////////////////////
    case TEXT_BUFFER_BACKING:
        pResult = NULL;
        break;

    /////////////////////////
    default:
        ASSERT(0);
        break;
    } // switch (m_SerializeBackingType)

abort:
    return(pResult);
} // GetNextDestNode






////////////////////////////////////////////////////////////////////////////////
//
// [StartObjectList]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CSerializedObject::StartObjectList(const char *pListName) {
    ErrVal err = ENoErr;

    if (NULL == pListName) {
        gotoErr(EFail);
    }

    /////////////////////////////////////////////
    if (SERIALIZE_OBJECT_TO_BACKING == m_SerializeDirection) {
        switch (m_SerializeBackingType) {
        /////////////////////////
        case XML_BACKING:
            break;

        /////////////////////////
        case TEXT_BUFFER_BACKING:
            err = WriteXMLElementToBuffer(pListName, 2, 0, false);
            if (err) {
                gotoErr(err);
            }
            m_SerializedIndentLevel++;
            break;

        /////////////////////////
        default:
            ASSERT(0);
            err = EFail;
            break;
        } // switch (m_SerializeBackingType)
    } // if (SERIALIZE_OBJECT_TO_BACKING == m_SerializeDirection)
    /////////////////////////////////////////////
    else { // if (DESERIALIZE_OBJECT_FROM_BACKING == m_SerializeDirection) {
    } // if (DESERIALIZE_OBJECT_FROM_BACKING == m_SerializeDirection)

abort:
    return(err);
} // StartObjectList






////////////////////////////////////////////////////////////////////////////////
//
// [StopObjectList]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CSerializedObject::StopObjectList(const char *pListName) {
    ErrVal err = ENoErr;

    if (NULL == pListName) {
        gotoErr(EFail);
    }

    /////////////////////////////////////////////
    if (SERIALIZE_OBJECT_TO_BACKING == m_SerializeDirection) {
        switch (m_SerializeBackingType) {
        /////////////////////////
        case XML_BACKING:
            break;

        /////////////////////////
        case TEXT_BUFFER_BACKING:
            m_SerializedIndentLevel--;
            err = WriteXMLElementToBuffer(pListName, 1, 1, true);
            if (err) {
                gotoErr(err);
            }
            break;

        /////////////////////////
        default:
            ASSERT(0);
            err = EFail;
            break;
        } // switch (m_SerializeBackingType)
    } // if (SERIALIZE_OBJECT_TO_BACKING == m_SerializeDirection)
    /////////////////////////////////////////////
    else { // if (DESERIALIZE_OBJECT_FROM_BACKING == m_SerializeDirection) {
    } // if (DESERIALIZE_OBJECT_FROM_BACKING == m_SerializeDirection)

abort:
    return(err);
} // StopObjectList






////////////////////////////////////////////////////////////////////////////////
//
// [TransferObject]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CSerializedObject::TransferObject(
                            const char *pItemName,
                            CSerializedObject *pObject,
                            void *pBackingNode) {
    ErrVal err = ENoErr;
    int32 childBufferSize;

    pObject->m_SerializedIndentLevel = m_SerializedIndentLevel + 1;

    /////////////////////////////////////////////
    if (SERIALIZE_OBJECT_TO_BACKING == m_SerializeDirection) {
        switch (m_SerializeBackingType) {
        /////////////////////////
        case XML_BACKING:
            err = pObject->WriteToXMLNode((CPolyXMLNode *) pBackingNode);
            break;

        /////////////////////////
        case TEXT_BUFFER_BACKING:
            pObject->m_pSerializeStartBuffer = NULL;
            pObject->m_pSerializeStopBuffer = NULL;
            pObject->m_pSerializeBuffer = NULL;
            pObject->m_SerializeDirection = SERIALIZE_OBJECT_TO_BACKING;
            pObject->m_SerializeBackingType = TEXT_BUFFER_BACKING;

            pObject->m_ppSerializeNextPointer = NULL;

            // Add some whitespace and the child element names
            // around the nested objects.
            err = WriteXMLElementToBuffer(pItemName, 1, 0, false);
            if (err) {
                gotoErr(err);
            }

            // Serialize the child object into a subbuffer.
            err = pObject->Serialize();
            if (err) {
                gotoErr(err);
            }

            // Copy the subbuffer into our serialized buffer.
            childBufferSize = pObject->m_pSerializeBuffer - pObject->m_pSerializeStartBuffer;
            err = GrowBuffer(childBufferSize);
            if (err) {
                gotoErr(err);
            }

            memcpy(m_pSerializeBuffer, pObject->m_pSerializeStartBuffer, childBufferSize);
            m_pSerializeBuffer += childBufferSize;

            // Add some whitespace around the nested objects.
            err = WriteXMLElementToBuffer(pItemName, 1, 0, true);
            if (err) {
                gotoErr(err);
            }
            break;

        /////////////////////////
        default:
            ASSERT(0);
            err = EFail;
            break;
        } // switch (m_SerializeBackingType)
    } // if (SERIALIZE_OBJECT_TO_BACKING == m_SerializeDirection)
    /////////////////////////////////////////////
    else { // if (DESERIALIZE_OBJECT_FROM_BACKING == m_SerializeDirection) {
        if (NULL == pObject) {
            ASSERT(0);
            goto abort;
        }

        switch (m_SerializeBackingType) {
        /////////////////////////
        case XML_BACKING:
            err = pObject->ReadFromXMLNode((CPolyXMLNode *) pBackingNode);
            break;

        /////////////////////////
        default:
            break;
        } // switch (m_SerializeBackingType)
    } // if (DESERIALIZE_OBJECT_FROM_BACKING == m_SerializeDirection)

abort:
    return(err);
} // TransferObject






////////////////////////////////////////////////////////////////////////////////
//
// [GrowBuffer]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CSerializedObject::GrowBuffer(int32 pBufferSize) {
    ErrVal err = ENoErr;
    int32 newBufferLength;
    int32 offset;

    if ((NULL == m_pSerializeStartBuffer)
        || ((m_pSerializeBuffer + pBufferSize) >= m_pSerializeStopBuffer)) {
        offset = (m_pSerializeBuffer - m_pSerializeStartBuffer);

        newBufferLength = (m_pSerializeStopBuffer - m_pSerializeStartBuffer)
                            + pBufferSize + 1024;

        m_pSerializeStartBuffer = (char *) g_MainMem.Realloc(m_pSerializeStartBuffer, newBufferLength);
        if (NULL == m_pSerializeStartBuffer) {
            gotoErr(EFail);
        }

        // Leave space for the NULL terminator.
        m_pSerializeStopBuffer = m_pSerializeStartBuffer + newBufferLength - 1;

        m_pSerializeBuffer = m_pSerializeStartBuffer + offset;
    }

abort:
    returnErr(err);
} // GrowBuffer






////////////////////////////////////////////////////////////////////////////////
//
// [WriteXMLToBuffer]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CSerializedObject::WriteXMLToBuffer(const char *pDataName, const char *pText) {
    ErrVal err = ENoErr;
    int32 textLength;

    err = WriteXMLElementToBuffer(pDataName, 1, 0, false);
    if (err) {
        gotoErr(err);
    }

    textLength = strlen(pText);
    err = GrowBuffer(textLength);
    if (err) {
        gotoErr(err);
    }
    memcpy(m_pSerializeBuffer, pText, textLength);
    m_pSerializeBuffer += textLength;

    err = WriteXMLElementToBuffer(pDataName, 0, 0, true);
    if (err) {
        gotoErr(err);
    }

abort:
    returnErr(err);
} // WriteXMLToBuffer





////////////////////////////////////////////////////////////////////////////////
//
// [WriteXMLElementToBuffer]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CSerializedObject::WriteXMLElementToBuffer(const char *pDataName,
                                           int32 numNewlinesBeforeElement,
                                           int32 numNewlinesAfterElement,
                                           bool fClose) {
    ErrVal err = ENoErr;
    int32 newLength;
    int32 nameLength;
    int32 indentSize;
    int32 index;

    indentSize = m_SerializedIndentLevel * 3;
    nameLength = strlen(pDataName);

    newLength = 2 + indentSize + nameLength + 2
            + numNewlinesBeforeElement
            + numNewlinesAfterElement;
    err = GrowBuffer(newLength);
    if (err) {
        gotoErr(err);
    }

    for (index = 0; index < numNewlinesBeforeElement; index++) {
        *(m_pSerializeBuffer++) = '\n';
        //*(m_pSerializeBuffer++) = '\r';
    }
    if (numNewlinesBeforeElement > 0) {
        for (index = 0; index < indentSize; index++) {
            *(m_pSerializeBuffer++) = ' ';
        }
    }

    *(m_pSerializeBuffer++) = '<';
    if (fClose) {
        *(m_pSerializeBuffer++) = '/';
    }
    memcpy(m_pSerializeBuffer, pDataName, nameLength);
    m_pSerializeBuffer += nameLength;
    *(m_pSerializeBuffer++) = '>';

    for (index = 0; index < numNewlinesAfterElement; index++) {
        *(m_pSerializeBuffer++) = '\n';
        //*(m_pSerializeBuffer++) = '\r';
    }

abort:
    returnErr(err);
} // WriteXMLElementToBuffer




////////////////////////////////////////////////////////////////////////////////
//
// [AssignNextSerializedPointer]
//
////////////////////////////////////////////////////////////////////////////////
void
CSerializedObject::AssignNextSerializedPointer(CSerializedObject *pValue) {
    UNUSED_PARAM(pValue);
} // AssignNextSerializedPointer


////////////////////////////////////////////////////////////////////////////////
//
// [GetNextSerializeObject]
//
////////////////////////////////////////////////////////////////////////////////
void *
CSerializedObject::GetNextSerializeObject() {
    return(NULL);
} // GetNextSerializeObject


