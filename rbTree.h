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

#ifndef _RED_BLACK_TREE_H_
#define _RED_BLACK_TREE_H_


/////////////////////////////////////////////////////////////////////////////
class CRBTree : public CDebugObject {
public:
    enum TreeInternalConstants {
        // Node Flags
        NODE_ALLOCATED_BY_TREE      = 0x01,
        NODE_IN_TREE                = 0x04,
        RED_NODE                    = 0x08, // If this isn't set, then it's black
    }; // TreeCellChildrenConstants


    ///////////////////////////////////////////
    // This is the data for a tree or hash table element.
    class CNode {
    public:
        CNode           *m_pLeftChild;
        CNode           *m_pRightChild;
        CNode           *m_pParent;

        // The short key NON-uniquely identifies the cell. It
        // is used for fast comparisons and hashing cells. The
        // long name uniquely identifies the cell.
        int32           m_KeyHash;
        void            *m_pKey;
        int16           m_KeyLength;
        int8            m_TreeNodeFlags;

        const void      *m_pData;

        CNode();
        NEWEX_IMPL()

        int32 BlackHeight();
    }; // CNode


    CRBTree();
    virtual ~CRBTree();
    NEWEX_IMPL()

    void Initialize(int32 initialOptions);

    const void *GetValue(
                int32 keyHash,
                const void *pKey,
                int32 keyLength);

    ErrVal SetValue(
                int32 keyHash,
                const void *pKey,
                int32 keyLength,
                const void *userData);
    ErrVal SetValueEx(
                int32 keyHash,
                const void *pKey,
                int32 keyLength,
                const void *userData,
                CRBTree::CNode *pNewTreeNode,
                int32 extraNodeFlags);

    void RemoveAllValues();
    bool RemoveValue(
                int32 keyHash,
                const void *pKey,
                int32 keyLength);

    CNode *GetNextNode(CNode *pNode);
    CNode *GetPrevNode(CNode *pNode);

    // CDebugObject
    virtual ErrVal CheckState();

#if INCLUDE_REGRESSION_TESTS
    static void TestTree();
#endif

protected:
    friend class CTable;
    friend class CNameTable;

    ErrVal CheckNode(CNode *ptr);

    // These are used by tree and table.
    void DeleteNode(CNode *target);

    int32 CompareKeyWithNodeKey(
                int32 keyHash,
                const void *pKey,
                int32 keyLength,
                CNode *pNode);

    CNode *GetNode(
                int32 keyHash,
                const void *pKey,
                int32 keyLength,
                CNode **ppResultNode,
                int32 *pChildBranch);

    void LeftRotate(CNode *pOldRoot);
    void RightRotate(CNode *pOldRoot);

    void FixupAfterDelete(CNode *pXNode, CNode *pXNodeParent);

    uint32          m_TreeFlags;
    bool            m_fCheckingState;

    CNode           *m_pRoot;
    int32           m_NumItemsInTree;
}; // CRBTree.




#endif // _RED_BLACK_TREE_H_





