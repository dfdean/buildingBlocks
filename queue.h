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

#ifndef _QUEUE_H_
#define _QUEUE_H_

template <class ItemType> class CQueueList;

// Disable the warning:
//      warning C4355: 'this' : used in base member initializer list
//
// This is safe so long as the constructor does not use the value of
// "this" for anything. We just stash it off to a member variable for
// later retrieval.
#if WIN32
#pragma warning(disable:4355)
#endif //WIN32


/////////////////////////////////////////////////////////////////////////////
// A single item in the queue.
template <class ItemType>
class CQueueHook {
public:
    CQueueHook(ItemType *pObject);
    NEWEX_IMPL()

    void ResetQueue();

    ItemType *GetPreviousInQueue();
    ItemType *GetNextInQueue();

    bool OnAnyQueue() { return(NULL != m_pOwnerQueue); }

    void RemoveFromQueue();

private:
    friend class CQueueList<ItemType>;

    ItemType                *m_pObject;

    CQueueHook<ItemType>    *m_pNext;
    CQueueHook<ItemType>    *m_pPrev;

    // Do NOT remove an item from a queue in the item's destructor.
    // The queue may or may not be protected by a lock. We don't want
    // to intruduce an extra lock just for the queue to be safe, since
    // that would be expensive.
    CQueueList<ItemType>    *m_pOwnerQueue;
}; // CQueueHook





/////////////////////////////////////////////////////////////////////////////
// A complete queue.
template <class ItemType>
class CQueueList {
public:
    CQueueList();
    virtual ~CQueueList();
    NEWEX_IMPL()

    void ResetQueue();

    ItemType *GetHead();
    ItemType *GetTail();

    void InsertHead(CQueueHook<ItemType> *qItem);
    void InsertTail(CQueueHook<ItemType> *qItem);

    void RemoveFromQueue(CQueueHook<ItemType> *qItem);
    ItemType *RemoveHead();

    bool IsEmpty() { return (NULL == m_pFirst); }
    int32 GetLength() { return(m_NumItemsInQueue); }

    ErrVal CheckState();

private:
    friend class CQueueHook<ItemType>;

    static void ReportQError(const char *msg);

    int32                   m_NumItemsInQueue;

    CQueueHook<ItemType>    *m_pFirst;
    CQueueHook<ItemType>    *m_pLast;
}; // CQueueList






/////////////////////////////////////////////////////////////////////////////
//
// [CQueueList]
//
/////////////////////////////////////////////////////////////////////////////
template <class ItemType>
CQueueList<ItemType>::CQueueList() {
    ResetQueue();
} // CQueueList.




/////////////////////////////////////////////////////////////////////////////
//
// [~CQueueList]
//
/////////////////////////////////////////////////////////////////////////////
template <class ItemType>
CQueueList<ItemType>::~CQueueList() {
} // ~CQueueList.




/////////////////////////////////////////////////////////////////////////////
//
// [CheckState]
//
/////////////////////////////////////////////////////////////////////////////
template <class ItemType>
ErrVal
CQueueList<ItemType>::CheckState() {
    CQueueHook<ItemType> *prevItem;
    CQueueHook<ItemType> *item;
    int32 numItems;

    prevItem = NULL;
    item = m_pFirst;
    numItems = 0;
    while (NULL != item) {
        if (NULL == item->m_pObject) {
            return(EFail);
        }

        if (NULL != prevItem) {
            if (prevItem->m_pNext != item) {
                return(EFail);
            }
            if (prevItem != item->m_pPrev) {
                return(EFail);
            }
        } else { // if (NULL == prevItem)
            if (item->m_pPrev) {
                return(EFail);
            }
            if (m_pFirst != item) {
                return(EFail);
            }
        }

        if (this != item->m_pOwnerQueue) {
            return(EFail);
        }

        prevItem = item;
        item = item->m_pNext;
        numItems++;
    }

    if (numItems != m_NumItemsInQueue) {
        return(EFail);
    }

    return(ENoErr);
} // CheckState.




/////////////////////////////////////////////////////////////////////////////
//
// [ReportQError]
//
/////////////////////////////////////////////////////////////////////////////
template <class ItemType>
void
CQueueList<ItemType>::ReportQError(const char *msg) {
    OSIndependantLayer::ReportError(__FILE__, __LINE__, msg);
} // ReportQError.



/////////////////////////////////////////////////////////////////////////////
//
// [ResetQueue]
//
/////////////////////////////////////////////////////////////////////////////
template <class ItemType>
void
CQueueList<ItemType>::ResetQueue() {
    m_pFirst = NULL;
    m_pLast = NULL;
    m_NumItemsInQueue = 0;
} // ResetQueue




/////////////////////////////////////////////////////////////////////////////
//
// [InsertHead]
//
/////////////////////////////////////////////////////////////////////////////
template <class ItemType>
void
CQueueList<ItemType>::InsertHead(CQueueHook<ItemType> *qItem) {
    // If the item is already in a queue, then this is an error.
    if ((NULL == qItem)
        || (NULL != qItem->m_pOwnerQueue)
        || (NULL == qItem->m_pObject)) {
        CQueueList::ReportQError("Item already in another queue. ");
        return;
    }

    qItem->m_pOwnerQueue = this;
    qItem->m_pPrev = NULL;
    qItem->m_pNext = m_pFirst;

    if (NULL != m_pFirst) {
        m_pFirst->m_pPrev = qItem;
    }

    m_pFirst = qItem;
    if (!m_pLast) {
        m_pLast = qItem;
    }

    m_NumItemsInQueue += 1;
} // InsertHead.




/////////////////////////////////////////////////////////////////////////////
//
// [InsertTail]
//
/////////////////////////////////////////////////////////////////////////////
template <class ItemType>
void
CQueueList<ItemType>::InsertTail(CQueueHook<ItemType> *qItem) {
    // If the item is already in a queue, then this is an error.
    if ((NULL == qItem)
        || (NULL != qItem->m_pOwnerQueue)
        || (NULL == qItem->m_pObject)) {
        CQueueList::ReportQError("Item already in another queue. ");
        return;
    }

    qItem->m_pOwnerQueue = this;
    qItem->m_pPrev = m_pLast;
    qItem->m_pNext = NULL;

    if (NULL != m_pLast) {
        m_pLast->m_pNext = qItem;
    }

    m_pLast = qItem;
    if (!m_pFirst) {
        m_pFirst = qItem;
    }

    m_NumItemsInQueue += 1;
} // InsertTail.






/////////////////////////////////////////////////////////////////////////////
//
// [RemoveFromQueue]
//
/////////////////////////////////////////////////////////////////////////////
template <class ItemType>
void
CQueueList<ItemType>::RemoveFromQueue(CQueueHook<ItemType> *qItem) {
    if (NULL == qItem) {
        CQueueList::ReportQError("NULL item. ");
        goto abort;
    }

    if (this != qItem->m_pOwnerQueue) {
        CQueueList::ReportQError("Item already in another queue. ");
        goto abort;
    }

    if (qItem->m_pNext) {
        (qItem->m_pNext)->m_pPrev = qItem->m_pPrev;
    }

    if (qItem->m_pPrev) {
        (qItem->m_pPrev)->m_pNext = qItem->m_pNext;
    }

    if (qItem == m_pFirst) {
        m_pFirst = qItem->m_pNext;
    }

    if (qItem == m_pLast) {
        m_pLast = qItem->m_pPrev;
    }

    qItem->m_pPrev = NULL;
    qItem->m_pNext = NULL;
    qItem->m_pOwnerQueue = NULL;

    m_NumItemsInQueue = m_NumItemsInQueue - 1;

abort:
    return;
} // RemoveFromQueue





/////////////////////////////////////////////////////////////////////////////
//
// [GetHead]
//
/////////////////////////////////////////////////////////////////////////////
template <class ItemType>
ItemType *
CQueueList<ItemType>::GetHead() {
    ItemType *pResult = NULL;

    if (NULL != m_pFirst) {
        pResult = m_pFirst->m_pObject;
    }

    return(pResult);
} // GetHead




/////////////////////////////////////////////////////////////////////////////
//
// [GetTail]
//
/////////////////////////////////////////////////////////////////////////////
template <class ItemType>
ItemType *
CQueueList<ItemType>::GetTail() {
    ItemType *pResult = NULL;

    if (NULL != m_pLast) {
        pResult = m_pLast->m_pObject;
    }

    return(pResult);
} // GetTail




/////////////////////////////////////////////////////////////////////////////
//
// [RemoveHead]
//
/////////////////////////////////////////////////////////////////////////////
template <class ItemType>
ItemType *
CQueueList<ItemType>::RemoveHead() {
    ItemType *pObject = NULL;

    if (NULL != m_pFirst) {
        pObject = m_pFirst->m_pObject;
        RemoveFromQueue(m_pFirst);
    }

    return(pObject);
} // RemoveHead.






/////////////////////////////////////////////////////////////////////////////
//
// [CQueueHook]
//
/////////////////////////////////////////////////////////////////////////////
template <class ItemType>
CQueueHook<ItemType>::CQueueHook(ItemType *pObject) {
    m_pNext = NULL;
    m_pPrev = NULL;
    m_pOwnerQueue = NULL;

    m_pObject = pObject;
} // CQueueHook






/////////////////////////////////////////////////////////////////////////////
//
// [Reset]
//
/////////////////////////////////////////////////////////////////////////////
template <class ItemType>
void
CQueueHook<ItemType>::ResetQueue() {
    m_pNext = NULL;
    m_pPrev = NULL;
    m_pOwnerQueue = NULL;
} // Reset.




/////////////////////////////////////////////////////////////////////////////
//
// [GetNextInQueue]
//
/////////////////////////////////////////////////////////////////////////////
template <class ItemType>
ItemType *
CQueueHook<ItemType>::GetNextInQueue() {
    ItemType *pObject = NULL;

    if (NULL != m_pNext) {
        pObject = m_pNext->m_pObject;
    }

    return(pObject);
} // GetNextInQueue




/////////////////////////////////////////////////////////////////////////////
//
// [Prev]
//
/////////////////////////////////////////////////////////////////////////////
template <class ItemType>
ItemType *
CQueueHook<ItemType>::GetPreviousInQueue() {
    ItemType *pObject = NULL;

    if (NULL != m_pPrev) {
        pObject = m_pPrev->m_pObject;
    }

    return(pObject);
} // Prev.



/////////////////////////////////////////////////////////////////////////////
//
// [RemoveFromQueue]
//
/////////////////////////////////////////////////////////////////////////////
template <class ItemType>
void
CQueueHook<ItemType>::RemoveFromQueue() {
    if (NULL != m_pOwnerQueue) {
        m_pOwnerQueue->RemoveFromQueue(this);
    }
} // RemoveFromQueue.



void TestQueue();


#endif // _QUEUE_H_



