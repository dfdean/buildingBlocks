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
// Tree Library
//
// The module implements a binary tree of named entries.
// It is a balanced red-black tree, and the basic algorighm
// is taken from "Introduction to Algorithms", by Cormen,
// Leiserson, and Rivest.
//
// Each node is identified by BOTH a variable length key and a
// hash of that key. Keys may be text strings or binary blobs,
// and the hash is always a 32-bit number. Keys are unique
// within the tree, but hashes are not.
//
// A tree only keeps pointers to the user data and the long
// key, it does NOT allocate these data structures. A tree is
// a name-binding mechanism, not a storage allocation mechanism.
// Trees will allocate tree entries that reference user data.
// For performance, this module also allows a client to pass in
// a combined user data object that also includes a tree node so
// the tree module does not have to do the extra allocation.
//
// This is NOT thread-safe. Trees are typically private data
// structures, so for efficiency they are not protected by a
// lock. If a tree is shared by threads, then the threads must
// use their own lock to arbitrate access to the tree.
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

FILE_DEBUGGING_GLOBALS(LOG_LEVEL_DEFAULT, 0);


#define NODE_IS_RED(n) ((n)->m_TreeNodeFlags & CRBTree::RED_NODE)
#define NODE_IS_BLACK(n) (! ((n)->m_TreeNodeFlags & CRBTree::RED_NODE))

#define MARK_NODE_RED(n) (n)->m_TreeNodeFlags |= CRBTree::RED_NODE
#define MARK_NODE_BLACK(n) (n)->m_TreeNodeFlags &= ~CRBTree::RED_NODE

#define GET_NODE_COLOR(n) ((n)->m_TreeNodeFlags & CRBTree::RED_NODE)
#define SET_NODE_COLOR(n, color) { (n)->m_TreeNodeFlags &= ~CRBTree::RED_NODE; (n)->m_TreeNodeFlags |= color; }

#define LEFT_CHILD(n) (n)->m_pLeftChild
#define RIGHT_CHILD(n) (n)->m_pRightChild
#define PARENT(n) (n)->m_pParent



/////////////////////////////////////////////////////////////////////////////
//
// [CRBTree]
//
/////////////////////////////////////////////////////////////////////////////
CRBTree::CNode::CNode() {
    m_KeyHash = 0;
    m_pKey = NULL;
    m_KeyLength = 0;
    m_pData = NULL;

    m_pLeftChild = NULL;
    m_pRightChild = NULL;
    m_pParent = NULL;

    m_TreeNodeFlags = 0;
} // CNode




/////////////////////////////////////////////////////////////////////////////
//
// [BlackHeight]
//
/////////////////////////////////////////////////////////////////////////////
int32
CRBTree::CNode::BlackHeight() {
    int32 height = 0;

    if (NODE_IS_BLACK(this)) {
        height += 1;
    }

    if (NULL != LEFT_CHILD(this)) {
        height += LEFT_CHILD(this)->BlackHeight();
    } else if (NULL != RIGHT_CHILD(this)) {
        height += RIGHT_CHILD(this)->BlackHeight();
    } else
    {
        height += 1;
    }

    return(height);
} // BlackHeight






/////////////////////////////////////////////////////////////////////////////
//
// [CRBTree]
//
/////////////////////////////////////////////////////////////////////////////
CRBTree::CRBTree() {
    Initialize(0);
} // CRBTree




/////////////////////////////////////////////////////////////////////////////
//
// [~CRBTree]
//
/////////////////////////////////////////////////////////////////////////////
CRBTree::~CRBTree() {
    RemoveAllValues();
} // ~CRBTree




/////////////////////////////////////////////////////////////////////////////
//
// [Initialize]
//
/////////////////////////////////////////////////////////////////////////////
void
CRBTree::Initialize(int32 initialOptions) {
    m_TreeFlags = initialOptions;
    m_fCheckingState = false;
    m_pRoot = NULL;
    m_NumItemsInTree = 0;
} // Initialize.





/////////////////////////////////////////////////////////////////////////////
//
// [RemoveAllValues]
//
/////////////////////////////////////////////////////////////////////////////
void
CRBTree::RemoveAllValues() {
    CNode *pNode;
    CNode *pParentNode;
    RunChecks();

    //DEBUG_LOG("CRBTree::RemoveAllValues.");

    // Because this frees nodes, we do all descendents before freeing
    // each node, so this is a post-order tree walk.
    pNode = m_pRoot;
    while (pNode) {
        if (LEFT_CHILD(pNode)) {
            pNode = LEFT_CHILD(pNode);
        } else if (RIGHT_CHILD(pNode)) {
            pNode = RIGHT_CHILD(pNode);
        } else {
            pParentNode = pNode->m_pParent;
            if (pParentNode) {
                if (LEFT_CHILD(pParentNode) == pNode) {
                    LEFT_CHILD(pParentNode) = NULL;
                }
                else if (RIGHT_CHILD(pParentNode) == pNode) {
                    RIGHT_CHILD(pParentNode) = NULL;
                }
            }

            DeleteNode(pNode);

            // Process the parent on the next iteration.
            pNode = pParentNode;
        } // deleting a leaf node.
    } // while (pNode)

    m_pRoot = NULL;
    m_NumItemsInTree = 0;
} // RemoveAllValues.




/////////////////////////////////////////////////////////////////////////////
//
// [DeleteNode]
//
/////////////////////////////////////////////////////////////////////////////
void
CRBTree::DeleteNode(CRBTree::CNode *pNode) {
    if (NULL != pNode) {
       pNode->m_pKey = NULL;

       // Delete the leaf node only if we are allocating the
       // entries ourselves.
       if (pNode->m_TreeNodeFlags & NODE_ALLOCATED_BY_TREE) {
          delete pNode;
       }
    }
} // DeleteNode.




/////////////////////////////////////////////////////////////////////////////
//
// [CheckState]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CRBTree::CheckState() {
    ErrVal err = ENoErr;
    CNode *pNode;
    int32 numItemsFound = 0;

    if (NULL == m_pRoot) {
        returnErr(ENoErr);
    }

    // Get the left-most element in the tree. This is the first item
    // in the iteration.
    pNode = m_pRoot;
    while (LEFT_CHILD(pNode)) {
        pNode = LEFT_CHILD(pNode);
    }

    // Examine every node in order.
    while (pNode) {
        err = CheckNode(pNode);
        if (err) {
            returnErr(err);
        }

        numItemsFound++;
        pNode = GetNextNode(pNode);
    } // Check every node.


    if (m_NumItemsInTree != numItemsFound) {
        gotoErr(EFail);
    }


abort:
    returnErr(err);
} // CheckState.






/////////////////////////////////////////////////////////////////////////////
//
// [CheckNode]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CRBTree::CheckNode(CNode *pNode) {
    ErrVal err = ENoErr;
    CNode *pParentNode;
    CNode *pCurrentNode;
    int32 compareResult;
    int32 leftHeight;
    int32 rightHeight;
    bool fInTree;


    if ((m_fCheckingState) || (NULL == pNode)) {
        returnErr(ENoErr);
    }
    m_fCheckingState = true;


    if (!(pNode->m_TreeNodeFlags & NODE_IN_TREE)
        || (pNode->m_KeyLength < 0)) {
        gotoErr(EFail);
    }

    if (pNode->m_TreeNodeFlags & NODE_ALLOCATED_BY_TREE) {
        err = g_MainMem.CheckPtr((char *) pNode);
        if (err) {
            gotoErr(err);
        }
    }

    // Verify that this node is in this tree.
    pCurrentNode = pNode;
    fInTree = false;
    while (pCurrentNode) {
        if (m_pRoot == pCurrentNode) {
            fInTree = true;
            break;
        }

        pParentNode = pCurrentNode->m_pParent;
        if ((pCurrentNode != LEFT_CHILD(pParentNode))
            && (pCurrentNode != RIGHT_CHILD(pParentNode))) {
            gotoErr(EFail);
        }

        pCurrentNode = pParentNode;
    }
    if (!fInTree) {
        gotoErr(EFail);
    }


    // Verify that this node has a parent or it is the root.
    pParentNode = pNode->m_pParent;
    if (NULL == pParentNode) {
        if (m_pRoot != pNode) {
            gotoErr(EFail);
        }
    } else
    {
        // If this node has a parent, then verify that it is a child
        // of its parent.
        if ((pNode != LEFT_CHILD(pParentNode))
            && (pNode != RIGHT_CHILD(pParentNode))) {
            gotoErr(EFail);
        }
    }


    // Verify that this node is the parent of its children.
    if (LEFT_CHILD(pNode)) {
        if (pNode != LEFT_CHILD(pNode)->m_pParent) {
            gotoErr(EFail);
        }

        compareResult = CompareKeyWithNodeKey(
                           pNode->m_KeyHash,
                           pNode->m_pKey,
                           pNode->m_KeyLength,
                           LEFT_CHILD(pNode));
        if (compareResult <= 0) {
            gotoErr(EFail);
        }

        leftHeight = LEFT_CHILD(pNode)->BlackHeight();
    } else
    {
        leftHeight = 1;
    }



    if (RIGHT_CHILD(pNode)) {
        if (pNode != RIGHT_CHILD(pNode)->m_pParent) {
            gotoErr(EFail);
        }

        compareResult = CompareKeyWithNodeKey(
                           pNode->m_KeyHash,
                           pNode->m_pKey,
                           pNode->m_KeyLength,
                           RIGHT_CHILD(pNode));
        if (compareResult >= 0) {
            gotoErr(EFail);
        }

        rightHeight = RIGHT_CHILD(pNode)->BlackHeight();
    } else
    {
        rightHeight = 1;
    }

    if (leftHeight != rightHeight) {
        DEBUG_WARNING("Bad Height");
    }


abort:
    m_fCheckingState = false;

    returnErr(err);
} // CheckNode.





/////////////////////////////////////////////////////////////////////////////
//
// [SetValue]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CRBTree::SetValue(
            int32 keyHash,
            const void *pKey,
            int32 keyLength,
            const void *userData) {
    ErrVal err = SetValueEx(keyHash, pKey, keyLength, userData, NULL, 0);
    returnErr(err);
} // SetValue.




/////////////////////////////////////////////////////////////////////////////
//
// [SetValueEx]
//
// This is a simple performance optimization of SetValue. If the client
// can allocate a tree node (usually it is part of the user data),
// then we can avoid a separate allocation for the tree node.
//
// The node will have the same lifetime as the user data. If the
// user data is later replaced in the tree, then its tree node will
// be stitched out at that time.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CRBTree::SetValueEx(
            int32 keyHash,
            const void *pKey,
            int32 keyLength,
            const void *userData,
            CNode *pNewNode,
            int32 extraNodeFlags) {
    ErrVal err = ENoErr;
    CNode *pMatchingNode;
    CNode *pParentNode = NULL;
    int32 compareResult;
    bool fAllocatedNewNode = false;
    CNode *pX = NULL;
    CNode *pY = NULL;
    RunChecks();

    // Look for this node already in the tree.
    // This is an important call because even if a node is not in the
    // tree this will find out where it belongs when we add it.
    pMatchingNode = GetNode(
                        keyHash,
                        pKey,
                        keyLength,
                        &pParentNode,
                        &compareResult);

    // If the node isn't in the tree, and we were not provided a pre-allocated
    // node, then allocate one now.
    if ((NULL == pMatchingNode) && (NULL == pNewNode)) {
        pNewNode = newex CNode;
        if (NULL == pNewNode) {
            gotoErr(EFail);
        }
        fAllocatedNewNode = true;
    }


    // If there is a node we will insert into the tree, then initialize it now.
    if (NULL != pNewNode) {
        pNewNode->m_KeyHash = keyHash;
        pNewNode->m_KeyLength = (int16) keyLength;
        pNewNode->m_TreeNodeFlags = 0;
        pNewNode->m_pData = userData;
        pNewNode->m_pKey = (void *) pKey;
        if (fAllocatedNewNode) {
            pNewNode->m_TreeNodeFlags |= NODE_ALLOCATED_BY_TREE;
        }
    } // if (NULL != pNewNode)

    if (extraNodeFlags) {
        pNewNode->m_TreeNodeFlags |= extraNodeFlags;
    }


    // If the node already exists, then overwrite it. We allocate
    // the entries, so we can control the lifetime of each node.
    // The existing node will now have the lifetime of the new data.
    if (NULL != pMatchingNode) {
        // If we want the new node inserted, then replace pMatchingNode with
        // this new node.
        if (NULL != pNewNode) {
           LEFT_CHILD(pNewNode) = LEFT_CHILD(pMatchingNode);
           RIGHT_CHILD(pNewNode) = RIGHT_CHILD(pMatchingNode);
           PARENT(pNewNode) = PARENT(pMatchingNode);
           if (LEFT_CHILD(pNewNode))
           {
              LEFT_CHILD(pNewNode)->m_pParent = pNewNode;
           }
           if (RIGHT_CHILD(pNewNode))
           {
              RIGHT_CHILD(pNewNode)->m_pParent = pNewNode;
           }
           if (PARENT(pNewNode))
           {
              if (PARENT(pNewNode)->m_pLeftChild == pMatchingNode)
              {
                 PARENT(pNewNode)->m_pLeftChild = pNewNode;
              }
              else
              {
                  PARENT(pNewNode)->m_pRightChild = pNewNode;
              }
          }
          else
          {
              m_pRoot = pNewNode;
          }

          SET_NODE_COLOR(pNewNode, GET_NODE_COLOR(pMatchingNode));
          pNewNode->m_TreeNodeFlags |= NODE_IN_TREE;
          pMatchingNode->m_TreeNodeFlags &= ~NODE_IN_TREE;

          // If we allocate the entries, then discard the old node.
          DeleteNode(pMatchingNode);
        } // if (NULL != pNewNode)
        else {
           pMatchingNode->m_pData = userData;
        }

        gotoErr(ENoErr);
    } // if (pMatchingNode)


    // If we could not find the node, but there is a parent
    // node, then create a new node and place it under its parent.
    if (pParentNode) {
        if (compareResult < 0) {
           LEFT_CHILD(pParentNode) = pNewNode;
        } else {
           RIGHT_CHILD(pParentNode) = pNewNode;
        }
    } else
    {
        if (m_pRoot) {
            gotoErr(EFail);
        }

        m_pRoot = pNewNode;
    }


    pNewNode->m_pLeftChild = NULL;
    pNewNode->m_pRightChild = NULL;
    pNewNode->m_pParent = pParentNode;

    m_NumItemsInTree += 1;
    pNewNode->m_TreeNodeFlags |= NODE_IN_TREE;


    // Now, re-balance the tree. This implements the RB-INSERT procedure
    // on page 268, section 14.3, of "Introduction to Algorithms"
    // by Cormen, Leiserson, Rivest.
    pX = pNewNode;
    pNewNode = NULL;
    fAllocatedNewNode = false;

    MARK_NODE_RED(pX);
    while ((pX != m_pRoot) && (NODE_IS_RED(PARENT(pX)))) {
        // pX is in the left subtree of its grandparent.
        if (PARENT(pX) == LEFT_CHILD(PARENT(PARENT(pX)))) {
            // pY is the uncle of pX.
            pY = RIGHT_CHILD(PARENT(PARENT(pX)));

            if ((NULL != pY) && (NODE_IS_RED(pY))) {
                MARK_NODE_BLACK(PARENT(pX));
                MARK_NODE_BLACK(pY);
                MARK_NODE_RED(PARENT(PARENT(pX)));
                pX = PARENT(PARENT(pX));
            }
            else {
                if (pX == RIGHT_CHILD(PARENT(pX))) {
                    pX = PARENT(pX);
                    LeftRotate(pX);
                }

                MARK_NODE_BLACK(PARENT(pX));
                MARK_NODE_RED(PARENT(PARENT(pX)));
                RightRotate(PARENT(PARENT(pX)));
            }
        }
        // Otherwise, pX is in the right subtree of its grandparent.
        else {
            // pY is the uncle of the current node.
            pY = LEFT_CHILD(PARENT(PARENT(pX)));

            if ((NULL != pY) && (NODE_IS_RED(pY))) {
                MARK_NODE_BLACK(PARENT(pX));
                MARK_NODE_BLACK(pY);
                MARK_NODE_RED(PARENT(PARENT(pX)));
                pX = PARENT(PARENT(pX));
            }
            else {
                if (pX == LEFT_CHILD(PARENT(pX))) {
                    pX = PARENT(pX);
                    RightRotate(pX);
                }

                MARK_NODE_BLACK(PARENT(pX));
                MARK_NODE_RED(PARENT(PARENT(pX)));
                LeftRotate(PARENT(PARENT(pX)));
            }
        }
    }

    MARK_NODE_BLACK(m_pRoot);

abort:
    if (fAllocatedNewNode) {
        DeleteNode(pNewNode);
        pNewNode = NULL;
    }

    returnErr(err);
} // SetValueEx.





/////////////////////////////////////////////////////////////////////////////
//
// [GetValue]
//
/////////////////////////////////////////////////////////////////////////////
const void *
CRBTree::GetValue(int32 keyHash, const void *pKey, int32 keyLength) {
    CNode *pNode;

    pNode = GetNode(keyHash, pKey, keyLength, NULL, NULL);
    if (pNode) {
        return(pNode->m_pData);
    }

    //DEBUG_LOG("CRBTree::GetValue. Cannot find a value in the tree.");
    return(NULL);
} // GetValue.







/////////////////////////////////////////////////////////////////////////////
//
// [GetNode]
//
// Find a node in the tree. This is based on the ITERATIVE-TREE-SEARCH
// function on page 248, section 14.2, of "Introduction to Algorithms"
// by Cormen, Leiserson, Rivest.
/////////////////////////////////////////////////////////////////////////////
CRBTree::CNode *
CRBTree::GetNode(
            int32 keyHash,
            const void *pKey,
            int32 keyLength,
            CNode **ppParentNodeResult,
            int32 *pLastCompareResult) {
    CNode *pX = NULL;
    CNode *pParentNode = NULL;
    int32 compareResult = -1;

    if (NULL == pKey) {
        DEBUG_LOG("CRBTree::GetNode. NULL == pKey.");
        goto abort;
    }

    pParentNode = NULL;
    pX = m_pRoot;
    while (NULL != pX) {
        compareResult = CompareKeyWithNodeKey(keyHash, pKey, keyLength, pX);
        if (0 == compareResult) {
            goto abort;
        } // matching an node.

        pParentNode = pX;
        if (compareResult < 0) {
            pX = LEFT_CHILD(pX);
        } else {
            pX = RIGHT_CHILD(pX);
        }
    }

abort:
    // Record where we left off. This is either the match,
    // or else the parent of where the node should be inserted.
    if (ppParentNodeResult) {
        *ppParentNodeResult = pParentNode;
    }
    if (pLastCompareResult) {
        *pLastCompareResult = compareResult;
    }

    if (0 == compareResult) {
      return(pX);
    } else
    {
       return(NULL);
    }
} // GetNode.








/////////////////////////////////////////////////////////////////////////////
//
// [GetPrevNode]
//
/////////////////////////////////////////////////////////////////////////////
CRBTree::CNode *
CRBTree::GetPrevNode(CNode *pNode) {
    CNode *pResult = NULL;
    RunChecks();

    if (NULL == pNode) {
        if (NULL == m_pRoot) {
            pResult = NULL;
        } else {
           pResult = m_pRoot;
           while (RIGHT_CHILD(pResult))
           {
               pResult = RIGHT_CHILD(pResult);
           }
        }
    } else if (LEFT_CHILD(pNode)) {
        pResult = LEFT_CHILD(pNode);
        while ((NULL != pResult) && (RIGHT_CHILD(pResult))) {
            pResult = RIGHT_CHILD(pResult);
        }
    } else
    {
        pResult = PARENT(pNode);
        while ((NULL != pResult) && (pNode == LEFT_CHILD(pResult))) {
            pNode = pResult;
            pResult = PARENT(pResult);
        }
    }

    return(pResult);
} // GetPrevNode.





/////////////////////////////////////////////////////////////////////////////
//
// [GetNextNode]
//
// Find the successor node in the tree. This is based on the Tree-Minimum function
// on page 248, and the Tree-Successor function on page 249, of "Introduction to
// Algorithms" by Cormen, Leiserson, Rivest.
/////////////////////////////////////////////////////////////////////////////
CRBTree::CNode *
CRBTree::GetNextNode(CNode *pPrevNode) {
    CNode *pResult = NULL;

    if (NULL == pPrevNode) {
        if (!m_pRoot) {
           return(NULL);
        }
        pResult = m_pRoot;
        while (LEFT_CHILD(pResult)) {
            pResult = LEFT_CHILD(pResult);
        }
    } else if (NULL != RIGHT_CHILD(pPrevNode)) {
        // This is the Tree-Minimum function.
        pResult = RIGHT_CHILD(pPrevNode);
        while ((NULL != pResult) && (NULL != LEFT_CHILD(pResult))) {
            pResult = LEFT_CHILD(pResult);
        }
    } else
    {
        // This is the rest of the Tree-Successor function.
        pResult = PARENT(pPrevNode);
        while ((NULL != pResult) && (pPrevNode == RIGHT_CHILD(pResult))) {
            pPrevNode = pResult;
            pResult = PARENT(pResult);
        }
    }

    return(pResult);
} // GetNextNode






/////////////////////////////////////////////////////////////////////////////
//
// [RemoveValue]
//
// Remove a value from the tree. This is based on the Tree-Delete function
// on page 253 and the RB-DELETE function on page 273 of
// "Introduction to Algorithms" by Cormen, Leiserson, Rivest.
//
// I apologize for the bogus variable names (like Z and Y), but these are
// the names used in the text and I wanted to keep the code close to that.
/////////////////////////////////////////////////////////////////////////////
bool
CRBTree::RemoveValue(int32 keyHash, const void *pKey, int32 keyLength) {
    CNode *pZ = NULL; // The node we want to remove.
    CNode *pY = NULL; // The node we actually can remove; either pZ or pZ's successor.
    CNode *pX = NULL; // The node we replace pY with
    CNode *pParentOfReplacementNode = NULL;
    int16 colorOfReplacementNode = 0;


    pZ = GetNode(keyHash, pKey, keyLength, NULL, NULL);
    if (NULL == pZ) {
        DEBUG_WARNING("CRBTree::RemoveValue. Cannot find a node in the tree.");
        return(false);
    }

    // Frst, find a node to remove from the tree. If the target has no
    // children or only one child, then it can be removed without disrupting
    // the tree. Otherwise, we need to leave that node where it is, and we remove
    // the value by replacing the value in the target node with the next higher
    // value. We can remove the successor value's node from the tree, because it
    // is guaranteed to have at most one child. This is because it is the smallest
    // node in the target node's right subtree.
    //
    // pY is the node we will remove from the tree.
    if ((NULL == LEFT_CHILD(pZ)) || (NULL == RIGHT_CHILD(pZ))) {
        pY = pZ;
    } else
    {
        pY = GetNextNode(pZ);
    }


    // pX is the node we will use to replace pY. Note that pX may be NULL
    // if pY had no children.
    if (NULL != LEFT_CHILD(pY)) {
        pX = LEFT_CHILD(pY);
    } else
    {
        pX = RIGHT_CHILD(pY);
    }


    // Replace the target node with its successor in the tree.
    if (NULL != pX) {
        PARENT(pX) = PARENT(pY);
    }
    if (NULL == PARENT(pY)) {
        m_pRoot = pX;
    } else
    {
        if (pY == LEFT_CHILD(PARENT(pY))) {
            LEFT_CHILD(PARENT(pY)) = pX;
        } else {
            RIGHT_CHILD(PARENT(pY)) = pX;
        }
    }


    // pZ is the node we wanted to remove, and pY is the node we actually
    // removed. If these are different, then we replaced the target node
    // with its successor. In that case, we have to copy all values from
    // the successor to the new node.
    //
    // Now, this is a bit tricky. We *could* just copy the key and value
    // fields, but sometimes a client will provide the node as well and that
    // should follow the value around in the tree. So, we just swap the tree
    // nodes.
    //
    // So, unlike RB-DELETE in "Introduction to Algorithms", we *actually*
    // do remove pZ from the tree. This is fine, but it means that the tree
    // fixup is slightly different.
    //
    colorOfReplacementNode = GET_NODE_COLOR(pY);
    pParentOfReplacementNode = PARENT(pY);
    if (pY != pZ) {
        // Replace pZ in the tree with pY.
        LEFT_CHILD(pY) = LEFT_CHILD(pZ);
        RIGHT_CHILD(pY) = RIGHT_CHILD(pZ);
        PARENT(pY) = PARENT(pZ);
        if (LEFT_CHILD(pZ)) {
            PARENT(LEFT_CHILD(pZ)) = pY;
        }
        if (RIGHT_CHILD(pZ)) {
            PARENT(RIGHT_CHILD(pZ)) = pY;
        }
        if (PARENT(pZ)) {
            if (LEFT_CHILD(PARENT(pZ)) == pZ) {
                LEFT_CHILD(PARENT(pZ)) = pY;
            }
            else {
                RIGHT_CHILD(PARENT(pZ)) = pY;
            }
        } else {
            m_pRoot = pY;
        }

        SET_NODE_COLOR(pY, GET_NODE_COLOR(pZ));

        // If we replaced pZ with one of its children, then the replacement
        // node's parent just changed in the preceeding lines that swapped
        // out pZ.
        if (pParentOfReplacementNode == pZ) {
            pParentOfReplacementNode = pY;
        }
    } // (pY != pZ)

    // If we moved a black node, then we may have imbalanced the tree. In that
    // case, rebalance it.
    if (!(colorOfReplacementNode & CRBTree::RED_NODE)) {
        FixupAfterDelete(pX, pParentOfReplacementNode);
    }

    // Now, we can actually delete pZ.
    pZ->m_TreeNodeFlags &= ~NODE_IN_TREE;
    m_NumItemsInTree = m_NumItemsInTree - 1;
    DeleteNode(pZ);

    return(true);
} // RemoveValue.







/////////////////////////////////////////////////////////////////////////////
//
// [FixupAfterDelete]
//
// Remove a value from the tree. This is based on the RB-DELETE-FIXUP function
// on page 274 of "Introduction to Algorithms" by Cormen, Leiserson, Rivest.
//
// I apologize for the bogus variable names (like Z and Y), but these are
// the names used in the text and I wanted to keep the code close to that.
/////////////////////////////////////////////////////////////////////////////
void
CRBTree::FixupAfterDelete(CNode *pX, CNode *pXParent) {
    CNode *pW;

    while ((NULL == pX) || (NODE_IS_BLACK(pX))) {
        if ((m_pRoot == pX)
            || (NULL == pXParent)) {
            break;
        }

        if (pX == LEFT_CHILD(pXParent)) {
            pW = RIGHT_CHILD(pXParent);
            if (NULL == pW) {
                break;
            }

            if (NODE_IS_RED(pW)) {
                MARK_NODE_BLACK(pW);
                MARK_NODE_RED(pXParent);

                LeftRotate(pXParent);
                pW = RIGHT_CHILD(pXParent);

                ASSERT(NULL != pW);
            }

            if (((NULL == LEFT_CHILD(pW))
                    || (NODE_IS_BLACK(LEFT_CHILD(pW))))
                && ((NULL == RIGHT_CHILD(pW))
                    || (NODE_IS_BLACK(RIGHT_CHILD(pW))))) {
                MARK_NODE_RED(pW);

                pX = pXParent;
                pXParent = pX->m_pParent;
            }
            else {
                if ((NULL == RIGHT_CHILD(pW))
                    || (NODE_IS_BLACK(RIGHT_CHILD(pW)))) {
                    if (NULL != LEFT_CHILD(pW))
                    {
                        MARK_NODE_BLACK(LEFT_CHILD(pW));
                        MARK_NODE_RED(pW);
                        RightRotate(pW);
                        pW = RIGHT_CHILD(pXParent);
                    }
                }

                SET_NODE_COLOR(pW, GET_NODE_COLOR(pXParent));
                MARK_NODE_BLACK(pXParent);
                if (NULL != RIGHT_CHILD(pW)) {
                    MARK_NODE_BLACK(RIGHT_CHILD(pW));
                }
                LeftRotate(pXParent);

                // This will cause the loop to stop.
                pX = m_pRoot;
                pXParent = NULL;
            }
        } else { // if (pX == RIGHT_CHILD(pXParent))
            pW = LEFT_CHILD(pXParent);

            if (NULL == pW) {
                break;
            }

            if (NODE_IS_RED(pW)) {
                MARK_NODE_BLACK(pW);
                MARK_NODE_RED(pXParent);
                RightRotate(pXParent);
                pW = LEFT_CHILD(pXParent);

                ASSERT(NULL != pW);
            }

            if (((NULL == LEFT_CHILD(pW))
                    || (NODE_IS_BLACK(LEFT_CHILD(pW))))
                && ((NULL == RIGHT_CHILD(pW))
                    || (NODE_IS_BLACK(RIGHT_CHILD(pW))))) {
                MARK_NODE_RED(pW);

                pX = pXParent;
                pXParent = pX->m_pParent;
            }
            else {
                if ((NULL == LEFT_CHILD(pW))
                    || (NODE_IS_BLACK(LEFT_CHILD(pW)))) {
                    if (NULL != RIGHT_CHILD(pW))
                    {
                        MARK_NODE_BLACK(RIGHT_CHILD(pW));
                        MARK_NODE_RED(pW);
                        LeftRotate(pW);
                        pW = LEFT_CHILD(pXParent);
                    }
                }

                SET_NODE_COLOR(pW, GET_NODE_COLOR(pXParent));
                MARK_NODE_BLACK(pXParent);
                if (NULL != LEFT_CHILD(pW)) {
                    MARK_NODE_BLACK(LEFT_CHILD(pW));
                }

                RightRotate(pXParent);

                pX = m_pRoot;
                pXParent = NULL;
            }
        }
    }

    if (NULL != pX) {
        MARK_NODE_BLACK(pX);
    }
} // FixupAfterDelete




/////////////////////////////////////////////////////////////////////////////
//
// [LeftRotate]
//
// This is the LEFT-ROTATE procedure described on page 266, section 14.2,
// of "Introduction to Algorithms" by Cormen, Leiserson, Rivest.
//
// I apologize for the single character variable names, I tried to keep them
// close to those in the book. I also use macros to more closely resemble
// the notation of the book.
//
// There is nothing too mysterious about this procedure, it rebalances the
// tree by one step. Imagine that you are holding a rope in your hand, and both
// ends of the rope hang down. Now, suppose that one end is longer than the
// other, because you are not holding the rope in the middle. You can move your
// hand 1 step along the rope closer to the middle. This will shift everything and
// make the rope closer to balanced. That's basically what this procedure does
// to a tree. It makes one child of the root the new root, and adjusts the child
// and parent pointers accordingly. Effectively, this just switches a root of a
// subtree with one of its children.
//
// This can be applied to the root of any subtree in the tree.
//
// pX is the old root, and pY is the new root.
/////////////////////////////////////////////////////////////////////////////
void
CRBTree::LeftRotate(CNode *pX) {
    CNode *pY = NULL;

    ASSERT(NULL != pX);
    if (NULL == pX) {
        return;
    }

    pY = RIGHT_CHILD(pX);
    if (NULL == pY) {
        return;
    }

    RIGHT_CHILD(pX) = LEFT_CHILD(pY);
    if (NULL != LEFT_CHILD(pY)) {
        LEFT_CHILD(pY)->m_pParent = pX;
    }

    PARENT(pY) = PARENT(pX);
    if (NULL == PARENT(pX)) {
        m_pRoot = pY;
    } else
    {
       if (pX == PARENT(pX)->m_pLeftChild) {
           PARENT(pX)->m_pLeftChild = pY;
       } else {
           PARENT(pX)->m_pRightChild = pY;
       }
    }

    LEFT_CHILD(pY) = pX;
    PARENT(pX) = pY;
} // LeftRotate





/////////////////////////////////////////////////////////////////////////////
//
// [RightRotate]
//
// See the comments under LeftRotate. This is the same function, but it
// rebalances the tree in the other direction.
/////////////////////////////////////////////////////////////////////////////
void
CRBTree::RightRotate(CNode *pX) {
    CNode *pY = NULL;

    ASSERT(NULL != pX);
    if (NULL == pX) {
        return;
    }

    pY = LEFT_CHILD(pX);
    if (NULL == pY) {
        return;
    }

    LEFT_CHILD(pX) = RIGHT_CHILD(pY);
    if (NULL != RIGHT_CHILD(pY)) {
        RIGHT_CHILD(pY)->m_pParent = pX;
    }

    PARENT(pY) = PARENT(pX);
    if (NULL == PARENT(pX)) {
        m_pRoot = pY;
    } else if (pX == PARENT(pX)->m_pLeftChild) {
        PARENT(pX)->m_pLeftChild = pY;
    } else
    {
        PARENT(pX)->m_pRightChild = pY;
    }

    RIGHT_CHILD(pY) = pX;
    PARENT(pX) = pY;
} // RightRotate







/////////////////////////////////////////////////////////////////////////////
//
// [CompareKeyWithNodeKey]
//
// NOTE: The hash may compare differently than the full key.
// Specifically, you could have hash(key1) < hash(key2),
// even if key1 > key2. I don't think this matters so long
// as it is consistent for any 2 keys. Ideally, we never compare
// full keys, and only compare hashes, since that is faster.
// We only compare full keys when the hashes are tied. So,
// the keys may not appear in strict alphabetical order, but
// they are kept in a predictable and deterministic order and
// we can always find them and the tree is always balanced.
//
// The important thing is that this function deterministically can
// compare 2 keys. How it does this comparison does not affect the
// rest of the algorighms.
/////////////////////////////////////////////////////////////////////////////
int32
CRBTree::CompareKeyWithNodeKey(
                int32 keyHash,
                const void *pKey,
                int32 keyLength,
                CNode *pNode) {
    int32 result = 0;
    int32 compareLength;

    // First, see if the hash will give us a fast comparison.
    if (keyHash > pNode->m_KeyHash) {
        result = 1;
    } else if (keyHash < pNode->m_KeyHash) {
        result = -1;
    } else { // (keyHash == pNode->m_KeyHash)
        // If the hash values are equal, compare the actual keys.
        compareLength = keyLength;
        if (compareLength > pNode->m_KeyLength) {
           compareLength = pNode->m_KeyLength;
        }

        if (m_TreeFlags & CStringLib::IGNORE_CASE) {
            result = strncasecmpex(
                        (char *) pKey,
                        (char *) pNode->m_pKey,
                        compareLength);
        } else {
            result = memcmp(
                        (char *) pKey,
                        (char *) pNode->m_pKey,
                        compareLength);
        }

        // Check for the case that one key is a prefix match of the
        // other key, but the suffix is different. For example, the
        // keys "abc" and "abcXYZ" are only prefix matches.
        if ((0 == result) && (keyLength != pNode->m_KeyLength)) {
            if (keyLength < pNode->m_KeyLength) {
                result = -1;
            }
            else {
                result = 1;
            }
        }
    } // (keyHash == pNode->m_KeyHash)

    return(result);
} // CompareKeyWithNodeKey.






/////////////////////////////////////////////////////////////////////////////
//
//                  TESTING PROCEDURES
//
/////////////////////////////////////////////////////////////////////////////
#if INCLUDE_REGRESSION_TESTS

#define NUM_TEST_ENTRIES   2000

class CTestTreeItem
{
public:
    NEWEX_IMPL()

    int32 m_KeyHash;
    int32 m_Key;
    int32 m_NumVisits;
};

static CTestTreeItem g_TestValues[NUM_TEST_ENTRIES];
static CTestTreeItem g_NewTestValues[NUM_TEST_ENTRIES];

static int32 GetUniqueTestKey(int32 numValudEntries);





/////////////////////////////////////////////////////////////////////////////
//
// [TestTree]
//
/////////////////////////////////////////////////////////////////////////////
void
CRBTree::TestTree() {
    ErrVal err = ENoErr;
    int32 count;
    CRBTree *pTree = NULL;
    char *ptr;
    CRBTree::CNode *pPrevNode;
    CTestTreeItem *pNodeUserData;
    bool fSawPrevNode;
    int32 prevNodeKeyHash;
    int32 NumItemsVisited;
    bool fRemovedItem;


    g_DebugManager.StartModuleTest("Trees");
    g_DebugManager.SetProgressIncrement(50);

    // Make the tests repeatable.
    OSIndependantLayer::SetRandSeed(256);



    pTree = newex CRBTree;
    if (!pTree) {
        DEBUG_WARNING("Cannot allocate a test tree.");
        gotoErr(EFail);
    }

    pTree->Initialize(0);
    pTree->SetDebugFlags(CDebugObject::CHECK_STATE_ON_EVERY_OP);

    // Create some entries.
    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        (g_TestValues[count]).m_KeyHash = OSIndependantLayer::GetRandomNum();
        (g_TestValues[count]).m_Key = GetUniqueTestKey(count);
    }



    g_DebugManager.StartTest("Tree Add and Get");

    // Add some entries. This creates a very unbalanced tree.
    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        g_DebugManager.ShowProgress();

        err = pTree->SetValue(
                        (g_TestValues[count]).m_KeyHash,
                        (char *) &(g_TestValues[count].m_Key),
                        sizeof(int32),
                        (char *) &(g_TestValues[count]));
        if (err) {
            DEBUG_WARNING("Error from pTree->SetValue");
        }
    }


    // Make sure everything is in the tree.
    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        g_DebugManager.ShowProgress();

        ptr = (char *) pTree->GetValue(
                            (g_TestValues[count]).m_KeyHash,
                            (const char *) &(g_TestValues[count].m_Key),
                            sizeof(int32));

        if (ptr != ((char *) &(g_TestValues[count]))) {
            DEBUG_WARNING("Tree read returns wrong value for a node.");
        }
    }




    ///////////////////////////////////////////////
    g_DebugManager.StartTest("Forward Iteration");

    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        (g_TestValues[count]).m_NumVisits = 0;
    }
    fSawPrevNode = false;
    prevNodeKeyHash = 0;
    NumItemsVisited = 0;



    pPrevNode = NULL;
    while (1) {
        pPrevNode = pTree->GetNextNode(pPrevNode);
        if (NULL == pPrevNode) {
            break;
        }
        if (pPrevNode->m_KeyLength != sizeof(int32)) {
            DEBUG_WARNING(" NULL userData");
            break;
        }

        pNodeUserData = (CTestTreeItem *) pPrevNode->m_pData;
        if (!pNodeUserData) {
            DEBUG_WARNING("GetNextNode returns NULL user data.");
            break;
        }
        pNodeUserData->m_NumVisits += 1;


        // Make sure short keys do not decrease. We should also test
        // long keys.
        if ((fSawPrevNode)
            && (prevNodeKeyHash > pNodeUserData->m_KeyHash)) {
            DEBUG_WARNING("Short keys are out of order.");
            break;
        }
        fSawPrevNode = true;
        prevNodeKeyHash = pNodeUserData->m_KeyHash;

        NumItemsVisited += 1;
    }



    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        if ((g_TestValues[count]).m_NumVisits != 1) {
            DEBUG_WARNING("Not all elements were visited by GetNextNode.");
            break;
        }
    }

    if (NUM_TEST_ENTRIES != NumItemsVisited) {
        DEBUG_WARNING("Iterator did not visit the expected number of items.");
    }



    ///////////////////////////////////////////////
    g_DebugManager.StartTest("Reverse Iteration");

    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        (g_TestValues[count]).m_NumVisits = 0;
    }
    fSawPrevNode = false;
    prevNodeKeyHash = 0;
    NumItemsVisited = 0;


    pPrevNode = NULL;
    while (1) {
        pPrevNode = pTree->GetPrevNode(pPrevNode);
        if (!pPrevNode) {
            break;
        }
        if (pPrevNode->m_KeyLength != sizeof(int32)) {
            DEBUG_WARNING(" NULL userData");
            break;
        }


        pNodeUserData = (CTestTreeItem *) pPrevNode->m_pData;
        if (!pNodeUserData) {
            DEBUG_WARNING("GetNextNode returns NULL user data.");
            break;
        }
        pNodeUserData->m_NumVisits += 1;


        // Make sure short keys do not increase. We should also test
        // long keys.
        if ((fSawPrevNode)
            && (prevNodeKeyHash < pNodeUserData->m_KeyHash)) {
            DEBUG_WARNING("Short keys are out of order.");
            break;
        }
        fSawPrevNode = true;
        prevNodeKeyHash = pNodeUserData->m_KeyHash;

        NumItemsVisited += 1;
    }


    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        if ((g_TestValues[count]).m_NumVisits != 1) {
            DEBUG_WARNING("Not all elements were visited by GetPrevNode.");
            break;
        }
    }

    if (NUM_TEST_ENTRIES != NumItemsVisited) {
        DEBUG_WARNING("Iterator did not visit the expected number of items.");
    }



    ///////////////////////////////////////////////
    g_DebugManager.StartTest("Tree Remove");

    // Remove half of the entries.
    for (count = 0; count < NUM_TEST_ENTRIES; count += 2) {
        g_DebugManager.ShowProgress();

        fRemovedItem = pTree->RemoveValue(
                                    (g_TestValues[count]).m_KeyHash,
                                    (char *) &(g_TestValues[count].m_Key),
                                    sizeof(int32));
        if (!fRemovedItem) {
            DEBUG_WARNING("RemoveValue returned false for a real node.");
        }
    }


    // Make sure the entries were really removed.
    for (count = 0; count < NUM_TEST_ENTRIES; count += 2) {
        g_DebugManager.ShowProgress();

        ptr = (char *) pTree->GetValue(
                        (g_TestValues[count]).m_KeyHash,
                        (char *) &(g_TestValues[count].m_Key),
                        sizeof(int32));
        if (ptr) {
            DEBUG_WARNING("Tree RemoveValue not delete a node.");
        }
    }


    // Make sure that the remaining entries are still in the table.
    for (count = 1; count < NUM_TEST_ENTRIES; count += 2) {
        g_DebugManager.ShowProgress();

        ptr = (char *) pTree->GetValue(
                        (g_TestValues[count]).m_KeyHash,
                        (char *) &(g_TestValues[count].m_Key),
                        sizeof(int32));

        if (ptr != ((char *) &(g_TestValues[count]))) {
            DEBUG_WARNING("Tree read returns wrong value for a node.");
        }
    }





    // Re add the original entries.
    for (count = 0; count < NUM_TEST_ENTRIES; count += 2) {
        g_DebugManager.ShowProgress();

        err = pTree->SetValue(
                        (g_TestValues[count]).m_KeyHash,
                        (char *) &(g_TestValues[count].m_Key),
                        sizeof(int32),
                        (char *) &(g_TestValues[count]));
        if (err) {
            DEBUG_WARNING("Error from pTree->SetValue");
        }
    }


    // Make sure that all entries are in the table.
    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        g_DebugManager.ShowProgress();

        ptr = (char *) pTree->GetValue(
                                (g_TestValues[count]).m_KeyHash,
                                (char *) &(g_TestValues[count].m_Key),
                                sizeof(int32));

        if (ptr != ((char *) &(g_TestValues[count]))) {
            DEBUG_WARNING("Tree read returns wrong value for a node.");
        }
    }



    ///////////////////////////////////////////////
    g_DebugManager.StartTest("Tree Add that overwrites.");

    // Create some new entries.
    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        g_DebugManager.ShowProgress();

        (g_NewTestValues[count]).m_KeyHash = (g_TestValues[count]).m_KeyHash;
        (g_NewTestValues[count]).m_Key = (g_TestValues[count]).m_Key;
    }

    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        g_DebugManager.ShowProgress();

        err = pTree->SetValue(
                (g_NewTestValues[count]).m_KeyHash,
                (char *) &(g_NewTestValues[count].m_Key),
                sizeof(int32),
                (char *) &(g_NewTestValues[count]));
        if (err) {
            DEBUG_WARNING("Error from pTree->SetValue");
        }
    }



    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        g_DebugManager.ShowProgress();

        ptr = (char *) pTree->GetValue(
                        (g_NewTestValues[count]).m_KeyHash,
                        (char *) &(g_NewTestValues[count].m_Key),
                        sizeof(int32));

        if (ptr != ((char *) &(g_NewTestValues[count]))) {
            DEBUG_WARNING("Tree read returns wrong value for a node.");
        }
    }




    ///////////////////////////////////////////////
    g_DebugManager.StartTest("Tree Add and Get with duplicate short keys");

    pTree->RemoveAllValues();


    // Add a bunch of entries with different long keys but the same
    // short key.
    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        g_DebugManager.ShowProgress();

        (g_TestValues[count]).m_KeyHash = 35;
        (g_TestValues[count]).m_Key = GetUniqueTestKey(count);

        err = pTree->SetValue(
                (g_TestValues[count]).m_KeyHash,
                (char *) &(g_TestValues[count].m_Key),
                sizeof(int32),
                (char *) &(g_TestValues[count]));
        if (err) {
            DEBUG_WARNING("Error from pTree->SetValue");
        }
    }

    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        g_DebugManager.ShowProgress();

        ptr = (char *) pTree->GetValue(
                        (g_TestValues[count]).m_KeyHash,
                        (char *) &(g_TestValues[count].m_Key),
                        sizeof(int32));

        if (ptr != ((char *) &(g_TestValues[count]))) {
            DEBUG_WARNING("Tree read returns wrong value for a node.");
        }
    }


    /////////////////////////////////////////////
    g_DebugManager.StartTest("Tree Remove with duplicate short keys");
    g_DebugManager.SetProgressIncrement(100);


    // Remove half of the entries.
    for (count = 0; count < NUM_TEST_ENTRIES; count += 2) {
        g_DebugManager.ShowProgress();

        fRemovedItem = pTree->RemoveValue(
                            (g_TestValues[count]).m_KeyHash,
                            (char *) &(g_TestValues[count].m_Key),
                            sizeof(int32));
    }


    // Make sure the entries were really removed.
    for (count = 0; count < NUM_TEST_ENTRIES; count += 2) {
        g_DebugManager.ShowProgress();

        ptr = (char *) pTree->GetValue(
                        (g_TestValues[count]).m_KeyHash,
                        (char *) &(g_TestValues[count].m_Key),
                        sizeof(int32));
        if (ptr) {
            DEBUG_WARNING("Tree RemoveValue not delete a node.");
        }
    }


    // Make sure that the remaining entries are still in the table.
    for (count = 1; count < NUM_TEST_ENTRIES; count += 2) {
        g_DebugManager.ShowProgress();

        ptr = (char *) pTree->GetValue(
                            (g_TestValues[count]).m_KeyHash,
                            (char *) &(g_TestValues[count].m_Key),
                            sizeof(int32));

        if (ptr != ((char *) &(g_TestValues[count]))) {
            DEBUG_WARNING("Tree read returns wrong value for a node.");
        }
    }


    // Re-add the original entries.
    for (count = 0; count < NUM_TEST_ENTRIES; count += 2) {
        g_DebugManager.ShowProgress();

        err = pTree->SetValue(
                        (g_TestValues[count]).m_KeyHash,
                        (char *) &(g_TestValues[count].m_Key),
                        sizeof(int32),
                        (char *) &(g_TestValues[count]));
        if (err) {
            DEBUG_WARNING("Error from pTree->SetValue");
        }
    }


    // Make sure that all entries are in the table.
    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        g_DebugManager.ShowProgress();

        ptr = (char *) pTree->GetValue(
                        (g_TestValues[count]).m_KeyHash,
                        (char *) &(g_TestValues[count].m_Key),
                        sizeof(int32));
        if (ptr != ((char *) &(g_TestValues[count]))) {
            DEBUG_WARNING("Tree read returns wrong value for a node.");
        }
    }


    /////////////////////////////////////////////////////
    g_DebugManager.StartTest("Balanced Tree Add and Get");

    pTree->RemoveAllValues();


    // Add some entries. This creates a very unbalanced tree.
    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        g_DebugManager.ShowProgress();

        (g_TestValues[count]).m_KeyHash = OSIndependantLayer::GetRandomNum();
    }


    // Add some entries. This creates a very unbalanced tree.
    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        g_DebugManager.ShowProgress();

        err = pTree->SetValue(
                        (g_TestValues[count]).m_KeyHash,
                        (char *) &(g_TestValues[count].m_Key),
                        sizeof(int32),
                        (char *) &(g_TestValues[count]));
        if (err) {
            DEBUG_WARNING("Error from pTree->SetValue");
        }
    }



    // Make sure everything is in the tree.
    for (count = 0; count < NUM_TEST_ENTRIES; count++) {
        g_DebugManager.ShowProgress();

        ptr = (char *) pTree->GetValue(
                        (g_TestValues[count]).m_KeyHash,
                        (char *) &(g_TestValues[count].m_Key),
                        sizeof(int32));
        if (ptr != ((char *) &(g_TestValues[count]))) {
            DEBUG_WARNING("Tree read returns wrong value for a node.");
        }
    }


abort:
    if (pTree) {
        delete pTree;
    }
} // TestTree





/////////////////////////////////////////////////////////////////////////////
//
// [GetUniqueTestKey]
//
/////////////////////////////////////////////////////////////////////////////
int32
GetUniqueTestKey(int32 numValudEntries) {
    int32 count;
    int32 value;

    while (1) {
        value = OSIndependantLayer::GetRandomNum();
        for (count = 0; count < numValudEntries; count++) {
            if ((g_TestValues[count]).m_Key == value) {
                break;
            }
        }

        if (count >= numValudEntries) {
            return(value);
        }
    }
} // GetUniqueTestKey


#endif // INCLUDE_REGRESSION_TESTS




