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

#ifndef _NAME_TABLE_H_
#define _NAME_TABLE_H_



/////////////////////////////////////////////////////////////////////////////
// This is a single name in a dictionary.
class CDictionaryEntry {
public:
    int16           m_NameLength;
    const char      *m_pName;
    const char      *m_pSpecialCloseName;
}; // CDictionaryEntry



/////////////////////////////////////////////////////////////////////////////
class CNameTable : public CDebugObject {
public:
    CNameTable();
    virtual ~CNameTable();
    NEWEX_IMPL()

    ErrVal Initialize(int32 initalOptions, int32 log2NumBucketsArg);

    const void *GetValue(const char *pKey, int32 keyLength);

    ErrVal SetValue(
                const char *pKey,
                int32 keyLength,
                const void *pUserData);

    ErrVal SetValueEx(
                const char *pKey,
                int32 keyLength,
                const void *pUserData,
                CRBTree::CNode *pNewTreeEntry);

    bool RemoveValue(const char *pKey, int32 keyLength);
    void RemoveAllValues();

    // CDebugObject
    virtual ErrVal CheckState();

    // The NameTable can also store/retrieve special DictionaryEntry objects.
    // This is just a particular value that is stored by SetValue() and SetValueEx(),
    // so these functions are just wrappers for SetValue() and SetValueEx(), although
    // they add a few extra features like looking in a parent dictionary.
    // Originally, this was a whole separate module, but it is so trivial it is
    // being merged into nameTable.
    CDictionaryEntry *LookupDictionaryEntry(const char *pNameStr, int32 nameLength);
    CDictionaryEntry *AddDictionaryEntry(const char *pNameStr, int32 nameLength);
    void AddDictionaryEntryList(const char **ppNameList);
    ErrVal InitializeWithParentNameTable(CNameTable *pStringList);


#if INCLUDE_REGRESSION_TESTS
    static void TestNameTable();
#endif

private:
    int32 ComputeKeyHash(const char *pKey, int32 keyLength);

    int32           m_NameTableFlags;
    int32           m_NumHashBuckets;
    int32           m_HashBucketMask;
    CRBTree         **m_pHashBucketList;

    // Dictionary
    CNameTable      *m_pParentDictionary;
}; // CNameTable.




#endif // _NAME_TABLE_H_


