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
// Queue Utilities Module
//
// This implements a queue class and an object that is on a queue.
// Objects do not typically subclass CQueueHook because a single object
// may be on several different queues at the same time. Instead, objects
// contains instance variables for the "coat hanger" for each different
// type of queue. I could instead implement a queue as a list of pointers,
// like STL vectors, but then that requires allocating memory blocks for the
// pointer lists, which I try to avoid.
//
// There are no locks around queues, since that is wasteful when it
// is not needed. If a queue must be lock-protected, then the higher
// level module must also include a lock.
//
// Do NOT remove an item from a queue in the item's destructor.
// The queue may or may no tbe protected by a lock. We don't want
// to intruduce an extra lock just for the queue to be safe, since
// that would be expensive.
//
/////////////////////////////////////////////////////////////////////////////

#include "osIndependantLayer.h"
#include "log.h"
#include "config.h"
#include "debugging.h"
#include "refCount.h"
#include "memAlloc.h"
#include "queue.h"

FILE_DEBUGGING_GLOBALS(LOG_LEVEL_DEFAULT, 0);



/////////////////////////////////////////////////////////////////////////////
//
//                       TESTING PROCEDURES
//
/////////////////////////////////////////////////////////////////////////////
#if INCLUDE_REGRESSION_TESTS

#define NUM_TEST_ITEMS   32



class CTestType
{
public:
    CTestType();

    int                     value;
    CQueueHook<CTestType>   qHook;
};


//////////////////////////////////////////////////////
CTestType::CTestType() : qHook(this) {
    value = 0;
}


CTestType qItems[NUM_TEST_ITEMS];



/////////////////////////////////////////////////////////////////////////////
//
// [TestQueue]
//
/////////////////////////////////////////////////////////////////////////////
void
TestQueue() {
    CQueueList<CTestType> qHead;
    int32 itemNum;

    g_DebugManager.StartModuleTest("Queue");

    g_DebugManager.StartTest("Insert items into a queue");

    for (itemNum = 0; itemNum < NUM_TEST_ITEMS; itemNum++) {
        qHead.InsertHead(&(qItems[itemNum].qHook));
        (void) qHead.CheckState();

        if (qHead.GetLength() != (itemNum + 1)) {
            DEBUG_WARNING("Wrong length");
        }
    }

    if (qHead.IsEmpty()) {
        DEBUG_WARNING("IsEmpty failed");
    }




    g_DebugManager.StartTest("Remove items from a queue");

    for (itemNum = 0; itemNum < NUM_TEST_ITEMS; itemNum++) {
        (qItems[itemNum]).qHook.RemoveFromQueue();
        (void) qHead.CheckState();
    }


    if (!(qHead.IsEmpty())) {
        DEBUG_WARNING("IsEmpty failed");
    }

    if (qHead.GetLength() != 0) {
        DEBUG_WARNING("Wrong length");
    }
} // TestQueue.



#endif // INCLUDE_REGRESSION_TESTS

