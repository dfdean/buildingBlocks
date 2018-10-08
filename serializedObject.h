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
/////////////////////////////////////////////////////////////////////////////

#ifndef _SERIALIZED_OBJECT_H_
#define _SERIALIZED_OBJECT_H_

class CSerializedObject;

typedef void (*SerializerClassFactoryProc)(void **ppBaseClass, CSerializedObject **ppSerializedObject);


// These are Macros called in the Serialize() method of each serialized class.
#define SERIALIZED_INTEGER(pValueName, pData, defaultValue) { err = TransferInteger(pValueName, pData, defaultValue); if (err) goto abort; }
#define SERIALIZED_UNSIGNED_INTEGER(pValueName, pData, defaultValue) { err = TransferUnsignedInteger(pValueName, pData, defaultValue); if (err) goto abort; }
#define SERIALIZED_INTEGER64(pValueName, pData, defaultValue) { err = TransferInteger64(pValueName, pData, defaultValue); if (err) goto abort; }
#define SERIALIZED_STRING(pValueName, pData, defaultValue) { err = TransferString(pValueName, pData, (char *) defaultValue); if (err) goto abort; }
#define SERIALIZED_BOOL(pValueName, pData, defaultValue) { err = TransferBool(pValueName, pData, defaultValue); if (err) goto abort; }
#define SERIALIZED_NEXT_POINTER(memberField) { m_ppSerializeNextPointer = (void **) memberField; }
#define SERIALIZED_FLOAT(pValueName, pData, defaultValue) { err = TransferFloat(pValueName, pData, defaultValue); if (err) goto abort; }

// This macro is called in a Serialize() method when a class member has a pointer to a single serialized object.
#define SERIALIZE_ONE_CONCRETE_OBJECT(pValueName, classType, factoryMethod, memberField) { \
    CSerializedObject *pSerializedObject = NULL; \
    if ((IsSerializing()) && (*(memberField))) { \
        void *pBackingNode = GetNextDestNode(NULL, pValueName, NULL); \
        err = TransferObject(pValueName, *(memberField), pBackingNode);  if (err) { goto abort; } \
    } else if (IsDeserializing()) { \
        void *pBackingNode = GetNextSourceNode(NULL, pValueName, NULL); \
        if (pBackingNode) { \
            if (factoryMethod) { \
                (*((SerializerClassFactoryProc) factoryMethod))((void **) (memberField), &pSerializedObject); \
            } else { \
                *(memberField) = newex classType; \
                pSerializedObject = *(memberField); \
            } \
            if (NULL == pSerializedObject) {err = EFail; goto abort;} \
            err = TransferObject(pValueName, pSerializedObject, pBackingNode); if (err) { goto abort; } \
        } \
    } \
} // SERIALIZE_ONE_CONCRETE_OBJECT


// This macro is called in a Serialize() method when a class member has a pointer to a linked list of serialized objects.
#define SERIALIZE_CONCRETE_OBJECT_LIST(pListName, pValueName, classType, memberField) { \
    classType *pCurrentDestObjectBaseClass = NULL; \
    classType **ppPrevObjectNextPtr = NULL; \
    CSerializedObject *pPrevDestObject = NULL; \
    CSerializedObject *pSerializedObject = NULL; \
    void *pBackingNode = NULL; \
    if ((IsSerializing()) && (memberField)) { pSerializedObject = (CSerializedObject *) *(memberField); } \
    if ((IsDeserializing()) && (memberField)) { *memberField = NULL; } \
    err = StartObjectList(pListName); if (err) {goto abort;} \
    while (1) { \
        if ((IsSerializing()) && (pSerializedObject)) { \
            pBackingNode = GetNextDestNode(pListName, pValueName, pBackingNode); \
            err = TransferObject(pValueName, pSerializedObject, pBackingNode); if (err) { goto abort; } \
            pSerializedObject = *((CSerializedObject **) (pSerializedObject->m_ppSerializeNextPointer)); \
        } else if (IsDeserializing()) { \
            pBackingNode = GetNextSourceNode(pListName, pValueName, pBackingNode); \
            if (NULL == pBackingNode) { break; } \
            pCurrentDestObjectBaseClass = newex classType; \
            pSerializedObject = (CSerializedObject *) (pCurrentDestObjectBaseClass); \
            if (NULL == pSerializedObject) { err = EFail; goto abort; } \
            pSerializedObject->m_ppSerializeNextPointer = NULL; \
            err = TransferObject(pValueName, pSerializedObject, pBackingNode); if (err) {goto abort;} \
            if (pPrevDestObject) { \
                pPrevDestObject->AssignNextSerializedPointer(pSerializedObject); \
                if (ppPrevObjectNextPtr) { *ppPrevObjectNextPtr = pCurrentDestObjectBaseClass; } \
            } else {*memberField = pCurrentDestObjectBaseClass; } \
            ppPrevObjectNextPtr = (classType **) (pSerializedObject->m_ppSerializeNextPointer); \
            pPrevDestObject = pSerializedObject; pSerializedObject = NULL; pCurrentDestObjectBaseClass = NULL; \
        } \
        else { break; } \
    } \
    err = StopObjectList(pListName); if (err) {goto abort;} \
} // SERIALIZE_CONCRETE_OBJECT_LIST



// This macro is called in a Serialize() method when a class member has a pointer to a linked list of serialized objects.
// This is different than SERIALIZE_OBJECT_LIST because here we assume the base type is a pure virtual class and
// is not related to a concrete class or to the serialized object class.
#define SERIALIZE_VIRTUAL_OBJECT_LIST(pListName, pValueName, classType, factoryMethod, memberField) { \
    classType *pCurrentDestObjectBaseClass = NULL; \
    classType **ppPrevObjectNextPtr = NULL; \
    CSerializedObject *pPrevDestObject = NULL; \
    CSerializedObject *pSerializedObject = NULL; \
    void *pBackingNode = NULL; \
    if ((IsSerializing()) && (memberField)) { pSerializedObject = (CSerializedObject *) *(memberField); } \
    if ((IsDeserializing()) && (memberField)) { *memberField = NULL; } \
    err = StartObjectList(pListName); if (err) {goto abort;} \
    while (1) { \
        if ((IsSerializing()) && (pSerializedObject)) { \
            pBackingNode = GetNextDestNode(pListName, pValueName, pBackingNode); \
            err = TransferObject(pValueName, pSerializedObject, pBackingNode); if (err) { goto abort; } \
            pSerializedObject = *((CSerializedObject **) (pSerializedObject->m_ppSerializeNextPointer)); \
        } else if (IsDeserializing()) { \
            pBackingNode = GetNextSourceNode(pListName, pValueName, pBackingNode); \
            if (NULL == pBackingNode) { break; } \
            if (factoryMethod) { \
                (*((SerializerClassFactoryProc) factoryMethod))((void **) &pCurrentDestObjectBaseClass, &pSerializedObject); \
            } else { pSerializedObject = NULL; } \
            if (NULL == pSerializedObject) { err = EFail; goto abort; } \
            pSerializedObject->m_ppSerializeNextPointer = NULL; \
            err = TransferObject(pValueName, pSerializedObject, pBackingNode); if (err) {goto abort;} \
            if (pPrevDestObject) { \
                pPrevDestObject->AssignNextSerializedPointer(pSerializedObject); \
                if (ppPrevObjectNextPtr) { *ppPrevObjectNextPtr = pCurrentDestObjectBaseClass; } \
            } else {*memberField = pCurrentDestObjectBaseClass; } \
            ppPrevObjectNextPtr = (classType **) (pSerializedObject->m_ppSerializeNextPointer); \
            pPrevDestObject = pSerializedObject; pSerializedObject = NULL; pCurrentDestObjectBaseClass = NULL; \
        } \
        else { break; } \
    } \
    err = StopObjectList(pListName); if (err) {goto abort;} \
} // SERIALIZE_VIRTUAL_OBJECT_LIST







////////////////////////////////////////////////////////////////////////////////
//
// This is the base class of any object that is serialized to/from a backing store.
// All functions are defined in the base class EXCEPT the Serialize() method,
// which must be defined by each serialized subclass.
////////////////////////////////////////////////////////////////////////////////
class CSerializedObject {
public:
    CSerializedObject();
    virtual ~CSerializedObject();
    
    //////////////////////////////////////////////////////////
    // These are the public functions that higher level client code will call *ON* a
    // serialized object. These eventually will call the object's Serialize() method.
    ErrVal WriteToXMLFile(const char *pPathName, const char *pTopLevelElement);
    ErrVal ReadFromXMLFile(const char *pPathName, const char *pTopLevelElement);
    
    ErrVal ReadFromXMLBuffer(const char *pDataBuffer, const char *pTopLevelElement);

    ErrVal WriteToXMLNode(CPolyXMLNode *pNode);
    ErrVal ReadFromXMLNode(CPolyXMLNode *pNode);
    
    //////////////////////////////////////////////////////////
    // This is overloaded by each serialized object subclasses.
    // This will call the macros on all the appropriate member variables.
    virtual ErrVal Serialize() = 0;

    //////////////////////////////////////////////////////////
    // These are called by the macros in the serialize method. They
    // are only inplemented in the base class, and they serialize/deserialize
    // individual fields with primitive types (int, str,...) in an object.
    ErrVal TransferString(
                    const char *pDataName,
                    char **pLocalData,
                    char *pDefaultValue);
    ErrVal TransferInteger(
                    const char *pDataName,
                    int32 *pLocalData,
                    int32 defaultValue);
    ErrVal TransferUnsignedInteger(
                    const char *pDataName,
                    uint32 *pLocalData,
                    uint32 defaultValue);
    ErrVal TransferInteger64(
                    const char *pDataName,
                    int64 *pLocalData,
                    int64 defaultValue);
    ErrVal TransferBool(
                    const char *pDataName,
                    bool *pLocalData,
                    bool defaultValue);
    ErrVal TransferFloat(
                    const char *pDataName,
                    float *pLocalData,
                    float defaultValue);
    void *GetNextSourceNode(
                    const char *pListName,
                    const char *pName,
                    void *pPrevNode);
    void *GetNextDestNode(
                    const char *pListName,
                    const char *pItemName,
                    void *pPrevNode);
    ErrVal TransferObject(
                    const char *pItemName,
                    CSerializedObject *pObject,
                    void *pBackingNode);

    ErrVal StartObjectList(const char *pListName);
    ErrVal StopObjectList(const char *pListName);

    // These are called inside the serialize method to switch on the operation.
    bool IsSerializing() { return(SERIALIZE_OBJECT_TO_BACKING == m_SerializeDirection); }
    bool IsDeserializing() { return(DESERIALIZE_OBJECT_FROM_BACKING == m_SerializeDirection); }
    
    //////////////////////////////////////////////////////////
    // This may *optionally* be overloaded in a serialized object subclasses. It sets
    // the next pointer in a subtype-specific way if the subtype is private and so not accessible
    // to the serializing baseclass.
    virtual void AssignNextSerializedPointer(CSerializedObject *pValue);
    virtual void *GetNextSerializeObject(void);
    void **m_ppSerializeNextPointer;

protected:
    // This can be used by fancy subclasses with customized Serialize() methods.
    CPolyXMLNode *GetSerializedXMLNode() { return(m_pSerializeXMLNode); }

private:
    ErrVal WriteXMLToBuffer(const char *pDataName, const char *pText);
    ErrVal WriteXMLElementToBuffer(
                        const char *pDataName,
                        int32 numNewlinesBeforeElement,
                        int32 numNewlinesAfterElement,
                        bool fClose);

    ErrVal GrowBuffer(int32 pBufferSize);

    enum CInternalConstants {
      // The Data types.
      INTEGER                           = 1,
      STRING                            = 2,
      BOOL                              = 3,
      INT64                             = 4,

      // Directions of data transfer
      SERIALIZE_OBJECT_TO_BACKING       = 1,
      DESERIALIZE_OBJECT_FROM_BACKING   = 2,

      // The types of backing we serialize to and deserialize from.
      XML_BACKING                       = 0,
      TEXT_BUFFER_BACKING               = 1,
    };

    int8                m_SerializeDirection;
    int8                m_SerializeBackingType;

    // These are the different backings we write to.
    CPolyXMLNode        *m_pSerializeXMLNode;

    char                *m_pSerializeStartBuffer;
    char                *m_pSerializeStopBuffer;
    char                *m_pSerializeBuffer;

    int32               m_SerializedIndentLevel;
}; // CSerializedObject



#endif // _SERIALIZED_OBJECT_H_


