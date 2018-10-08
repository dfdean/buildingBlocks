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
// Name Table Library
//
// The module implements name binding.
// A table hashes keys to a bucket, and each bucket stores
// a tree of entries. The array of buckets can be large to make
// a better hash function, but at a potential cost of unused slots.
//
// The name table uses trees from rbTree.cpp, so all consistency
// checks and logging are performed in the tree modules.
//
// This is NOT thread-safe. Tables are typically private data
// structures, so for efficiency they are not protected by a
// lock. If a table is shared by threads, then the threads must
// use their own lock to arbitrate access to the table.
//
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
#include "rbTree.h"
#include "nameTable.h"

FILE_DEBUGGING_GLOBALS(LOG_LEVEL_DEFAULT, 0);


// This is a single name in a dictionary.
class CDictEntryImpl {
public:
    CRBTree::CNode      m_TreeNode;
    CDictionaryEntry    m_DictionaryEntry;
    char                m_Key[2];
}; // CDictEntryImpl





/////////////////////////////////////////////////////////////////////////////
//
// [CNameTable]
//
/////////////////////////////////////////////////////////////////////////////
CNameTable::CNameTable() {
    m_pHashBucketList = NULL;
    m_NameTableFlags = 0;
    m_NumHashBuckets = 0;
    m_HashBucketMask = 0;

    m_pParentDictionary = NULL;
} // CNameTable



/////////////////////////////////////////////////////////////////////////////
//
// [~CNameTable]
//
// Do not do anything to deallocate a block. We create temporary
// blocks when we serialize, so they should be deallocated without
// effecting the original. We rely on dispose to do all allocation
// explicitly.
/////////////////////////////////////////////////////////////////////////////
CNameTable::~CNameTable() {
    int index;

    if (NULL != m_pHashBucketList) {
        for (index = 0; index < m_NumHashBuckets; index++) {
            if (NULL != m_pHashBucketList[index]) {
                (m_pHashBucketList[index])->RemoveAllValues();
                delete m_pHashBucketList[index];
                m_pHashBucketList[index] = NULL;
            }
        }

        memFree(m_pHashBucketList);
    } // if (NULL != m_pHashBucketList)

    // Do NOT free m_pParentDictionary.
    // That is a single global shared string list that is used but
    // not deallocated by many string lists.
} // ~CNameTable.





/////////////////////////////////////////////////////////////////////////////
//
// [Initialize]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNameTable::Initialize(int32 initialOptions, int32 log2NumBucketsArg) {
    ErrVal err = ENoErr;
    int32 index;

    if (log2NumBucketsArg < 0) {
        gotoErr(EFail);
    }

    m_NameTableFlags = initialOptions;
    m_NumHashBuckets = 1 << log2NumBucketsArg;
    m_HashBucketMask = m_NumHashBuckets - 1;

    m_pHashBucketList = (CRBTree **) memAlloc(sizeof(CRBTree *) * m_NumHashBuckets);
    if (NULL == m_pHashBucketList) {
        gotoErr(EFail);
    }    
    g_MainMem.DontCountMemoryAsLeaked((char *) m_pHashBucketList);
    for (index = 0; index < m_NumHashBuckets; index++) {
        (m_pHashBucketList[index]) = NULL;
    }

    // In case we are re-initializing a table, mark every hash entry
    // as empty. If there was a previous table, then these values need
    // to be re-initialized.
    for (index = 0; index < m_NumHashBuckets; index++) {
        (m_pHashBucketList[index]) = newex CRBTree;
        if (NULL == m_pHashBucketList[index]) {
            gotoErr(EFail);
        }        
        g_MainMem.DontCountMemoryAsLeaked((char *) m_pHashBucketList[index]);

        (m_pHashBucketList[index])->Initialize(initialOptions);
    }

abort:
    returnErr(err);
} // Initialize.




/////////////////////////////////////////////////////////////////////////////
//
// [InitializeWithParentNameTable]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNameTable::InitializeWithParentNameTable(CNameTable *pParentValue) {
    ErrVal err = ENoErr;

    if (NULL == pParentValue) {
        gotoErr(EFail);
    }

    err = Initialize(pParentValue->m_NameTableFlags | CStringLib::IGNORE_CASE, 4);
    if (err) {
        gotoErr(err);
    }

    m_pParentDictionary = pParentValue;

abort:
    returnErr(err);
} // InitializeWithParentNameTable





/////////////////////////////////////////////////////////////////////////////
//
// [RemoveAllValues]
//
/////////////////////////////////////////////////////////////////////////////
void
CNameTable::RemoveAllValues() {
    int index;

    // Run any standard debugger checks.
    RunChecksOnce();

    DEBUG_LOG("CNameTable::RemoveAllValues.");

    // Do not dispose of the hash table, since it will be resued
    // if we add new entries after this call.
    if (m_pHashBucketList) {
        for (index = 0; index < m_NumHashBuckets; index++) {
            if (m_pHashBucketList[index]) {
                (m_pHashBucketList[index])->RemoveAllValues();
            }
        }
    }
} // RemoveAllValues





/////////////////////////////////////////////////////////////////////////////
//
// [GetValue]
//
/////////////////////////////////////////////////////////////////////////////
const void *
CNameTable::GetValue(const char *pKey, int32 keyLength) {
    const void *ptr = NULL;
    int32 keyHash;
    int32 tableIndex;

    RunChecks();

    keyHash = ComputeKeyHash(pKey, keyLength);
    tableIndex = keyHash & m_HashBucketMask;

    if ((NULL == m_pHashBucketList) || (NULL == m_pHashBucketList[tableIndex])) {
        DEBUG_LOG("CNameTable::GetValue is uninitialized.");
        return(NULL);
    }

    ptr = (m_pHashBucketList[tableIndex])->GetValue(keyHash, pKey, keyLength);

    return(ptr);
} // GetValue.






/////////////////////////////////////////////////////////////////////////////
//
// [SetValue]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNameTable::SetValue(
            const char *pKey,
            int32 keyLength,
            const void *pUserData) {
    ErrVal err = ENoErr;
    int32 keyHash;
    int32 tableIndex;

    // Run any standard debugger checks.
    RunChecks();

    keyHash = ComputeKeyHash(pKey, keyLength);
    tableIndex = keyHash & m_HashBucketMask;

    if ((NULL == m_pHashBucketList) || (NULL == m_pHashBucketList[tableIndex])) {
        returnErr(EFail);
    }

    err = (m_pHashBucketList[tableIndex])->SetValue(
                                                keyHash,
                                                pKey,
                                                keyLength,
                                                pUserData);

    returnErr(err);
} // SetValue.






/////////////////////////////////////////////////////////////////////////////
//
// [SetValueEx]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNameTable::SetValueEx(
            const char *pKey,
            int32 keyLength,
            const void *pUserData,
            CRBTree::CNode *pNewTreeEntry) {
    ErrVal err = ENoErr;
    int32 keyHash;
    int32 tableIndex;

    RunChecks();

    keyHash = ComputeKeyHash(pKey, keyLength);
    tableIndex = keyHash & m_HashBucketMask;

    if ((NULL == m_pHashBucketList) || (NULL == m_pHashBucketList[tableIndex])) {
        gotoErr(EFail);
    }

    err = (m_pHashBucketList[tableIndex])->SetValueEx(
                                                   keyHash,
                                                   pKey,
                                                   keyLength,
                                                   pUserData,
                                                   pNewTreeEntry,
                                                   0);

abort:
    returnErr(err);
} // SetValueEx.







/////////////////////////////////////////////////////////////////////////////
//
// [RemoveValue]
//
/////////////////////////////////////////////////////////////////////////////
bool
CNameTable::RemoveValue(const char *pKey, int32 keyLength) {
    ErrVal err = ENoErr;
    int32 keyHash;
    int32 tableIndex;
    bool fRemovedItem = false;

    RunChecks();

    keyHash = ComputeKeyHash(pKey, keyLength);
    tableIndex = keyHash & m_HashBucketMask;

    if ((NULL == m_pHashBucketList) || (NULL == m_pHashBucketList[tableIndex])) {
        gotoErr(EFail);
    }

    fRemovedItem = (m_pHashBucketList[tableIndex])->RemoveValue(
                                                    keyHash,
                                                    pKey,
                                                    keyLength);

abort:
    return(fRemovedItem);
} // RemoveValue.





/////////////////////////////////////////////////////////////////////////////
//
// [CheckState]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CNameTable::CheckState() {
    ErrVal err = ENoErr;
    int32 treeNum;

    if (!m_pHashBucketList) {
        gotoErr(EFail);
    }

    for (treeNum = 0; treeNum < m_NumHashBuckets; treeNum++) {
        if (NULL == m_pHashBucketList[treeNum]) {
            gotoErr(EFail);
        }

        err = (m_pHashBucketList[treeNum])->CheckState();
        if (err) {
            gotoErr(err);
        }
    }

abort:
    returnErr(ENoErr);
} // CheckState





/////////////////////////////////////////////////////////////////////////////
//
// [ComputeKeyHash]
//
/////////////////////////////////////////////////////////////////////////////
int32
CNameTable::ComputeKeyHash(const char *pKey, int32 keyLength) {
    ErrVal err = ENoErr;
    int32 hash = 0;
    char *pDestPtr;
    char *pEndDestPtr;
    const char *pEndKey;
    char upperCaseKey[512];

    if ((NULL == pKey) || (keyLength < 0)) {
       goto abort;
    }

    if (m_NameTableFlags & CStringLib::IGNORE_CASE) {
        // Don't panic if the key is too big for our buffer.
        // The hash is only meant to be a best guess.
        if (keyLength > (int32) sizeof(upperCaseKey)) {
            keyLength = sizeof(upperCaseKey);
        }
        err = CStringLib::ConvertToUpperCase(
                                        pKey,
                                        keyLength,
                                        upperCaseKey,
                                        sizeof(upperCaseKey),
                                        &keyLength);
        if (!err) {
            pKey = upperCaseKey;
        }
    } // if (m_NameTableFlags & CStringLib::IGNORE_CASE)

    pEndKey = pKey + keyLength;
    pDestPtr = (char *) &hash;
    pEndDestPtr = pDestPtr + sizeof(int32);
    while (pKey < pEndKey) {
        *pDestPtr = *pDestPtr ^ *pKey;
        pKey++;
        pDestPtr++;
        if (pDestPtr >= pEndDestPtr) {
            pDestPtr = (char *) &hash;
        }
    } // while (pKey < pEndKey)

abort:
    return(hash);
} // ComputeKeyHash.







/////////////////////////////////////////////////////////////////////////////
//
// [LookupDictionaryEntry]
//
/////////////////////////////////////////////////////////////////////////////
CDictionaryEntry *
CNameTable::LookupDictionaryEntry(const char *pNameStr, int32 nameLength) {
    const void *ptr = NULL;
    int32 keyHash;
    int32 tableIndex;
    RunChecks();

    if (NULL == pNameStr) {
        return(NULL);
    }
    if (nameLength < 0) {
        nameLength = strlen(pNameStr);
    }

    // Look if this is in the global shared pre-built string list.
    if (m_pParentDictionary) {
        CDictionaryEntry *pNameEntry;

        pNameEntry = m_pParentDictionary->LookupDictionaryEntry(
                                                      pNameStr, 
                                                      nameLength);
        if (pNameEntry) {
            return(pNameEntry);
        }
    } // if (m_pParentDictionary)

    keyHash = ComputeKeyHash(pNameStr, nameLength);
    tableIndex = keyHash & m_HashBucketMask;

    if ((NULL == m_pHashBucketList) 
            || (NULL == m_pHashBucketList[tableIndex])) {
        DEBUG_LOG("CNameTable::GetValue is uninitialized.");
        return(NULL);
    }

    ptr = (m_pHashBucketList[tableIndex])->GetValue(keyHash, pNameStr, nameLength);

    return((CDictionaryEntry *) ptr);
} // LookupDictionaryEntry





/////////////////////////////////////////////////////////////////////////////
//
// [AddDictionaryEntry]
//
/////////////////////////////////////////////////////////////////////////////
CDictionaryEntry *
CNameTable::AddDictionaryEntry(const char *pNameStr, int32 nameLength) {
    ErrVal err = ENoErr;
    int32 keyHash;
    int32 tableIndex;
    CDictionaryEntry *pUserData;
    CDictEntryImpl *pCompleteEntry = NULL;
    int32 totalEntryLength;
    CDictionaryEntry *pEntry = NULL;
    CRBTree::CNode *pTreeNode = NULL;
    char *pStoredNamePtr;

    // Run any standard debugger checks.
    RunChecks();

    if (NULL == pNameStr) {
        gotoErr(EFail);
    }
    if (nameLength < 0) {
        nameLength = strlen(pNameStr);
    }

    // Look to see if the name has already been defined.
    // If it has, then we are done.
    pEntry = LookupDictionaryEntry(pNameStr, nameLength);
    if (pEntry) {
        return(pEntry);
    }

    // Otherwise, we need to add it.
    keyHash = ComputeKeyHash((const char *) pNameStr, nameLength);
    tableIndex = keyHash & m_HashBucketMask;

    if ((NULL == m_pHashBucketList) || (NULL == m_pHashBucketList[tableIndex])) {
        gotoErr(EFail);
    }

    // Allocate an entry that will hold both the data and the key.
    // I do this so 1 allocation will make room for everythng, and I don't have
    // to separately allocate 3 different things.
    totalEntryLength = sizeof(CDictEntryImpl) + nameLength + 2;
    pCompleteEntry = (CDictEntryImpl *) memAlloc(totalEntryLength);
    if (NULL == pCompleteEntry) {
        gotoErr(EFail);
    }
    g_MainMem.DontCountMemoryAsLeaked((char *) pCompleteEntry);

    pStoredNamePtr = &(pCompleteEntry->m_Key[0]);
    pUserData = &(pCompleteEntry->m_DictionaryEntry);
    pTreeNode = &(pCompleteEntry->m_TreeNode);

    // Initialize the entry.
    memcpy(pStoredNamePtr, pNameStr, nameLength);
    pStoredNamePtr[nameLength] = 0;
    pUserData->m_NameLength = nameLength;
    pUserData->m_pName = pStoredNamePtr;
    pUserData->m_pSpecialCloseName = NULL;

    // Add it to the tree.
    err = (m_pHashBucketList[tableIndex])->SetValueEx(
                                                keyHash,
                                                pStoredNamePtr,
                                                nameLength,
                                                pUserData,
                                                pTreeNode,
                                                CRBTree::NODE_ALLOCATED_BY_TREE);
    if (err) {
        gotoErr(err);
    }

    return(pUserData);

abort:
    return(NULL);
} // AddDictionaryEntry





/////////////////////////////////////////////////////////////////////////////
//
// [AddDictionaryEntryList]
//
/////////////////////////////////////////////////////////////////////////////
void
CNameTable::AddDictionaryEntryList(const char **ppNameList) {
    const char *pName;
    int32 nameLength;
    RunChecks();

    while (NULL != *ppNameList) {
        pName = *ppNameList;
        nameLength = strlen(pName);
        if (nameLength <= 0) {
            break;
        }

        (void) AddDictionaryEntry(pName, nameLength);

        ppNameList++;
    } // while (NULL != pName)
} // AddDictionaryEntryList




/////////////////////////////////////////////////////////////////////////////
//
//                       TESTING PROCEDURES
//
/////////////////////////////////////////////////////////////////////////////

#if INCLUDE_REGRESSION_TESTS

#define NUM_TEST_ENTRIES   1000

class CTestTreeItem
{
public:
    NEWEX_IMPL()

    int32 m_Key;
    int32 m_NumVisits;
};

static CTestTreeItem g_TestValues[NUM_TEST_ENTRIES];



/////////////////////////////////////////////////////////////////////////////
//
// [TestNameTable]
//
/////////////////////////////////////////////////////////////////////////////
void
CNameTable::TestNameTable() {
    ErrVal err = ENoErr;
    int32 count;
    CNameTable *pTable = NULL;
    const char *ptr;
    bool fRemovedItem;

    g_DebugManager.StartModuleTest("NameTables");
    g_DebugManager.SetProgressIncrement(80);

    g_MainMem.SetDebugFlags(CDebugObject::CHECK_STATE_ON_EVERY_OP);



    // Make the tests repeatable.
    OSIndependantLayer::SetRandSeed(512);


    pTable = newex CNameTable;
    if (NULL == pTable) {
        gotoErr(EFail);
    }
    err = pTable->Initialize(0, 5);
    if (err) {
        gotoErr(err);
    }
    pTable->SetDebugFlags(CDebugObject::CHECK_STATE_ON_EVERY_OP);


    err = pTable->CheckState();
    if (err) {
        gotoErr(EFail);
    }


    // Create some entries.
    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        (g_TestValues[count]).m_Key = OSIndependantLayer::GetRandomNum();
    }



    g_DebugManager.StartTest("Add Entries");

    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        g_DebugManager.ShowProgress();

        err = pTable->SetValue(
                (char *) &(g_TestValues[count].m_Key),
                sizeof(int32),
                (char *) &(g_TestValues[count]));
        if (err) {
            DEBUG_WARNING("Error from pTable->SetValue");
            gotoErr(err);
        }
    }


    err = pTable->CheckState();
    if (err) {
        DEBUG_WARNING("Error from checkstate");
        gotoErr(err);
    }


    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        g_DebugManager.ShowProgress();

        ptr = (char *) pTable->GetValue(
                            (const char *) &(g_TestValues[count].m_Key),
                            sizeof(int32));

        if (ptr != ((char *) &(g_TestValues[count]))) {
            DEBUG_WARNING("Tree read returns wrong value for a entry.");
            gotoErr(EFail);
        }
    }


    err = pTable->CheckState();
    if (err) {
        DEBUG_WARNING("Error from checkstate");
        gotoErr(EFail);
    }


    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        g_DebugManager.ShowProgress();

        ptr = (char *) pTable->GetValue(
                            (const char *) &(g_TestValues[count].m_Key),
                            sizeof(int32));

        if (ptr != ((char *) &(g_TestValues[count]))) {
            DEBUG_WARNING("Tree read returns wrong value for a entry.");
            gotoErr(EFail);
        }
    }




    g_DebugManager.StartTest("Remove Entries");

    // Remove half of the entries.
    for (count = 1; count < NUM_TEST_ENTRIES; count += 2) {
        g_DebugManager.ShowProgress();

        fRemovedItem = pTable->RemoveValue(
                                (char *) &(g_TestValues[count].m_Key),
                                sizeof(int32));
        if (!fRemovedItem) {
            DEBUG_WARNING("RemoveValue returned false for a real entry.");
            gotoErr(EFail);
        }
    }


    err = pTable->CheckState();
    if (err) {
        DEBUG_WARNING("Error from pTable checkstate");
        gotoErr(err);
    }


    // Check the remaining entries are still there.
    for (count = 0; count < NUM_TEST_ENTRIES; count += 2) {
        g_DebugManager.ShowProgress();

        ptr = (char *) pTable->GetValue(
                            (const char *) &(g_TestValues[count].m_Key),
                            sizeof(int32));

        if (ptr != ((char *) &(g_TestValues[count]))) {
            DEBUG_WARNING("Tree read returns wrong value for a entry.");
            gotoErr(EFail);
        }
    }



    // Check that the removed entries are really gone.
    for (count = 1; count < NUM_TEST_ENTRIES; count += 2) {
        g_DebugManager.ShowProgress();

        ptr = (char *) pTable->GetValue(
                            (const char *) &(g_TestValues[count].m_Key),
                            sizeof(int32));

        if (ptr) {
            DEBUG_WARNING("Incorrectly returned an entry that was removed.");
            gotoErr(EFail);
        }
    }


    // Re-add the removed entries.
    for (count = 1; count < NUM_TEST_ENTRIES; count += 2) {
        g_DebugManager.ShowProgress();

        err = pTable->SetValue(
                (char *) &(g_TestValues[count].m_Key),
                sizeof(int32),
                (char *) &(g_TestValues[count]));
        if (err) {
            DEBUG_WARNING("Error from pTable->SetValue");
            gotoErr(err);
        }
    }


    err = pTable->CheckState();
    if (err) {
        DEBUG_WARNING("Error from pTable checkstate");
        gotoErr(EFail);
    }


    // Check that all entries are in the pTable.
    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        g_DebugManager.ShowProgress();

        ptr = (char *) pTable->GetValue(
                            (const char *) &(g_TestValues[count].m_Key),
                            sizeof(int32));

        if (ptr != ((char *) &(g_TestValues[count]))) {
            DEBUG_WARNING("Tree read returns wrong value for a entry.");
            gotoErr(EFail);
        }
    }




    g_DebugManager.StartTest("Add Entries With Similar Short Keys");

    pTable->RemoveAllValues();


    // Change some entries.
    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        (g_TestValues[count]).m_Key = OSIndependantLayer::GetRandomNum();
    }


    for (count = 0; count < NUM_TEST_ENTRIES; count += 1) {
        g_DebugManager.ShowProgress();

        err = pTable->SetValue(
                (char *) &(g_TestValues[count].m_Key),
                sizeof(int32),
                (char *) &(g_TestValues[count]));
        if (err) {
            DEBUG_WARNING("Error from pTable->SetValue");
            gotoErr(EFail);
        }
    }


    err = pTable->CheckState();
    if (err) {
        DEBUG_WARNING("Error from pTable checkstate");
        gotoErr(EFail);
    }

    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        g_DebugManager.ShowProgress();

        ptr = (char *) pTable->GetValue(
                            (const char *) &(g_TestValues[count].m_Key),
                            sizeof(int32));

        if (ptr != ((char *) &(g_TestValues[count]))) {
            DEBUG_WARNING("Tree read returns wrong value for a entry.");
            gotoErr(EFail);
        }
    }


abort:
    if (pTable) {
        delete pTable;
    }
} // TestNameTable.



#endif // INCLUDE_REGRESSION_TESTS


