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

#ifndef _BUILDING_BLOCKS_H_
#define _BUILDING_BLOCKS_H_

#include "osIndependantLayer.h"
#include "stringLib.h"
#include "config.h"
#include "log.h"
#include "debugging.h"
#include "memAlloc.h"
#include "refCount.h"
#include "threads.h"
#include "fileUtils.h"
#include "queue.h"
#include "jobQueue.h"
#include "stringParse.h"
#include "rbTree.h"
#include "nameTable.h"
#include "url.h"
#include "blockIO.h"
#include "asyncIOStream.h"
#include "polyHTTPStream.h"
#include "polyXMLDoc.h"
#include "serializedObject.h"



/////////////////////////////////////////////////////////////////////////////
// Building Blocks Config Options
/////////////////////////////////////////////////////////////////////////////

// Proxy Settings
//static char HttpProxyConfigName[]               = "HTTP Proxy";
//static char HttpProxyPortConfigName[]           = "HTTP Proxy Port";
//static char SocksProxyConfigName[]              = "SOCKS Proxy";
//static char SocksProxyPortConfigName[]          = "SOCKS Proxy Port";

// Job Queue
//static char MaxWorkerThreadsValueName[]         = "Max Worker Threads";

extern CConfigSection *g_pBuildingBlocksConfig;



/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
class CBuildingBlocks
{
public:
    //CBuildingBlocks::MINIMAL_LOCAL_ONLY - No networking. Useful for things like cgi-bin
    //CBuildingBlocks::FULL_FUNCTIONALITY - Include networking
    enum {
        MINIMAL_LOCAL_ONLY  = 0,
        FULL_FUNCTIONALITY  = 1,
    };

   static ErrVal Initialize(int32 featureLevel, CProductInfo *pProductInfo);
   static void Shutdown();
   static void TestBuildingBlocks();
}; // CBuildingBlocks

#endif // _BUILDING_BLOCKS_H_
