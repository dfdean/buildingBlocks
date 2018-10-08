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
// File Utilities
//
// A few simple utilities like reading the contents of an entire file to a
// C-style string. These are generally more complex than the low level
// OS-independent file class, but not much.
/////////////////////////////////////////////////////////////////////////////

#include "osIndependantLayer.h"
#include "stringLib.h"
#include "log.h"
#include "config.h"
#include "debugging.h"
#include "memAlloc.h"
#include "fileUtils.h"

FILE_DEBUGGING_GLOBALS(LOG_LEVEL_DEFAULT, 0);




////////////////////////////////////////////////////////////////////////////////
//
// [ReadFileToBuffer]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CFileUtils::ReadFileToBuffer(
                  const char *pFileName,
                  char **ppResultBuffer,
                  int32 *pBufferLength) {
    ErrVal err = ENoErr;
    CSimpleFile fileHandle;
    uint32 fileLength;
    uint64 fileLength64;
    uint32 bytesRead;
    char *pFileBuffer = NULL;


    if ((NULL == pFileName) || (NULL == ppResultBuffer)) {
        gotoErr(EFail);
    }
    *ppResultBuffer = NULL;
    DEBUG_LOG("CStringLib::ReadFileToBuffer. pFileName = %s", pFileName);

    // Create the file and open it.
    err = fileHandle.OpenExistingFile(pFileName, CSimpleFile::READ_ONLY);
    if (err) {
        printf("\n\n>ACK! %s\n", pFileName);
        gotoErr(err);
    }

    err = fileHandle.GetFileLength(&fileLength64);
    if (err) {
        gotoErr(err);
    }
    fileLength = (int32) fileLength64;

    // Seek the beginning of the file
    err = fileHandle.Seek(0L, CSimpleFile::SEEK_START);
    if (err) {
        gotoErr(err);
    }

    // Read the entire file into a buffer.
    pFileBuffer = (char *) memAlloc(fileLength + 1);
    if (NULL == pFileBuffer) {
        gotoErr(EFail);
    }

    err = fileHandle.Read(pFileBuffer, fileLength, (int32 *) &bytesRead);
    if (err) {
        gotoErr(err);
    }
    if (bytesRead != fileLength) {
        gotoErr(EFail);
    }

    // Make this a C-string for easier parsing.
    pFileBuffer[fileLength] = 0;
    *ppResultBuffer = pFileBuffer;
    pFileBuffer = NULL;
    if (NULL != pBufferLength) {
       *pBufferLength = fileLength;
    }

abort:
    fileHandle.Close();
    if (pFileBuffer) {
        delete [] pFileBuffer;
    }

    returnErr(err);
} // ReadFileToBuffer







////////////////////////////////////////////////////////////////////////////////
//
// [WriteBufferToFile]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CFileUtils::WriteBufferToFile(
                  const char *pFileName,
                  const char *pBuffer,
                  int32 bufferLength) {
    ErrVal err = ENoErr;
    CSimpleFile fileHandle;

    if ((NULL == pFileName) || (NULL == pBuffer)) {
        gotoErr(EFail);
    }
    if (bufferLength < 0) {
        bufferLength = strlen(pBuffer);
    }
    DEBUG_LOG("CStringLib::WriteBufferToFile. pFileName = %s, bufferLength = %d\n",
                pFileName, bufferLength);

    // Create the file and open it.
    err = fileHandle.OpenOrCreateEmptyFile(pFileName, 0);
    if (err) {
        gotoErr(err);
    }

    err = fileHandle.Seek(0, CSimpleFile::SEEK_START);
    if (err) {
        gotoErr(err);
    }

    err = fileHandle.Write(pBuffer, bufferLength);
    if (err) {
        gotoErr(err);
    }

abort:
    fileHandle.Close();

    returnErr(err);
} // WriteBufferToFile










