/////////////////////////////////////////////////////////////////////////////
/*
Copyright (c) 2005-2018 Dawson Dean

Permission is hereby granted, free of charge, to any person obtaining a 
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation 
the rights to use, copy, modify, merge, publish, distribute, sublicense, 
and/or sell copies of the Software, and to permit persons to whom the 
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included 
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY 
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, 
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/
/////////////////////////////////////////////////////////////////////////////
// See the corresponding .cpp file for a description of this module.
/////////////////////////////////////////////////////////////////////////////

#ifndef _OS_INDEPENDANT_LAYER_H_
#define _OS_INDEPENDANT_LAYER_H_


/////////////////////////////////////////////////
#if WIN32
// This turns on strict type-checking with MS VisualC
#ifndef STRICT
#define STRICT
#endif

#ifdef FD_SETSIZE
#undef FD_SETSIZE
#endif
#define FD_SETSIZE 150

#include <windows.h>
#include <winbase.h>
#include <process.h>

#include <winreg.h>
#include <winsock.h>
#endif // WIN32


/////////////////////////////////////////////////
#if LINUX
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/unistd.h>
#define _REENTRANT 1
#include <pthread.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h> // For atoi
#endif

#include <stdio.h>


/////////////////////////////////////////////////
#if WIN32
// Disable warnings for _snprintf.
// <> To do: Investigate _snprintf_s
#define _CRT_SECURE_NO_DEPRECATE 1
#pragma warning( disable: 4996 )
#define snprintf _snprintf
#define unlink _unlink
#endif



/////////////////////////////////////////////////////////////////////////////
//
//                              COMPILER TRICKS
//
/////////////////////////////////////////////////////////////////////////////
#define UNUSED_PARAM(x) (void)(x)

// WebAssembly may have to declare that some procedures should not be removed by the
// optimizer. In a WebAssembly build, this is defined to be some constant specific to
// the Web Assembly compiler (emcc). For normal C/C++ code, however, this is a no-op.
#define WEBASSEMBLY_FUNCTION 



/////////////////////////////////////////////////////////////////////////////
//
//                       BASIC DATA TYPE DEFINITIONS
//
// By convention: Buffers have type uchar *, and strings have type
// char *. There is, of course, no special meaning of a negative 
// printing character, but this is more consistent with K&R.
/////////////////////////////////////////////////////////////////////////////

// These may also be defined in some public headers that do not include 
// the entire buildingBlocks library.
#ifndef _INTEGER_DATA_TYPES_
#define _INTEGER_DATA_TYPES_

#if WIN32
#define POINTER_SIZE_IN_BYTES       4
#define INT16_SIZE_IN_STRUCT_IN_BYTES 2
#elif LINUX
#define POINTER_SIZE_IN_BYTES       8
#define INT16_SIZE_IN_STRUCT_IN_BYTES 4
#elif WASM
#define POINTER_SIZE_IN_BYTES       8
#define INT16_SIZE_IN_STRUCT_IN_BYTES 4
#else
#error - No definition for POINTER_SIZE_IN_BYTES
#endif


#if WIN32
typedef unsigned __int64    uint64;
typedef __int64             int64;
#elif LINUX
typedef unsigned long long uint64;
typedef long long int64;
#elif WASM
typedef unsigned long long uint64;
typedef long long int64;
#else
#error - No definition for int64 and uint64
#endif

typedef signed int          int32;
typedef unsigned int        uint32;

typedef signed short        int16;
typedef unsigned short      uint16;

// Be careful, int8 is commonly defined, but other libraries may
// define it as something slightly different, like "signed char". 
#ifndef INT8_DEFINED
typedef char                int8;
#endif // INT8_DEFINED

typedef unsigned char       uint8;
typedef unsigned char       uchar;
#endif // _INTEGER_DATA_TYPES_

//typedef int bool;
#define true 1
#define false 0

#if LINUX
typedef unsigned short      WCHAR;
#endif // LINUX


#ifndef NULL
#define NULL    ((void *) 0)
#endif

#if WIN32
#define INT64FMT    "%I64d"
#define ERRFMT      "%d"
#elif LINUX
#define INT64FMT    "%lld"
#define ERRFMT      "%d"
#elif WASM
#define INT64FMT    "%lld"
#define ERRFMT      "%d"
#endif


// These have several properties which should cause
// code to immediately break if it tries to use these.
// That's good, since it caluses errors to happen closer
// to the source.
//
// 1. They are large unsigned or negative signed numbers.
//    This makes them invalid as pointers or handles.
//
// 2. They are odd numbers. This makes them invalid as pointers.
//
static const unsigned char invalidInt8     = 0xCD;
static const unsigned char unallocatedInt8 = 0xCF;



/////////////////////////////////////////////////////////////////////////////
//
//                           ERROR CODES
//
/////////////////////////////////////////////////////////////////////////////

#define ERROR_IS_UNEXPECTED 0x80000000
 
typedef uint32 ErrVal;
enum  {
    ENoErr                     = 0,

    //////////////////////////////////////////////////////
    EFail                      = 1 | ERROR_IS_UNEXPECTED,
    EOutOfMemory               = 2 | ERROR_IS_UNEXPECTED,
    ENotImpl                   = 5 | ERROR_IS_UNEXPECTED,
    EInvalidArg                = 7 | ERROR_IS_UNEXPECTED,
    EValueIsNotNumber          = 8,
    ESyntheticError            = 10,

    //////////////////////////////////////////////////////
    // File Errors
    EEOF                       = 100,
    EFileNotFound              = 101,
    ERequiredFileNotFound      = 102 | ERROR_IS_UNEXPECTED,
    EAccessDenied              = 103,
    ENoDiskSpace               = 104,
    EFileIsBusy                = 105,
    EReadOnly                  = 106,

    //////////////////////////////////////////////////////
    // Network errors
    ENoResponse                = 200,
    ENoHostAddress             = 201,
    EPeerDisconnected          = 202,
    ETooManySockets            = 203,
    EHTTPSRequired             = 204,

    //////////////////////////////////////////////////////
    // Property List errors
    EPropertyNotFound          = 400,
    EWrongPropertyType         = 401 | ERROR_IS_UNEXPECTED,

    //////////////////////////////////////////////////////
    // User option errors
    EMissingConfigFile         = 500, // This is NOT a bug.
    EInvalidConfigFile         = 501 | ERROR_IS_UNEXPECTED,

    //////////////////////////////////////////////////////
    // URL errors.
    EInvalidUrl                = 600,

    //////////////////////////////////////////////////////
    // HTTP errors
    EInvalidHttpHeader         = 700,
    EHTTPDocTooLarge           = 701,

    //////////////////////////////////////////////////////
    // XML Parsing errors
    EXMLParseError             = 800,

    //////////////////////////////////////////////////////
    // General User Data File Errors
    EDataFileSyntaxError        = 1000,
    EDataFileItemNotFound       = 1001,

    //////////////////////////////////////////////////////
    // Record file errors
    ERecordNotFound            = 1100,
}; // ErrVal


#if LINUX
#define GET_LAST_ERROR() errno
#endif

#if WIN32
#define GET_LAST_ERROR() GetLastError()
#endif

#define ERROR_CODE(err) ((err & ~ERROR_IS_UNEXPECTED))



/////////////////////////////////////////////////////////////////////////////
//
//                      PRODUCT INFORMATION
//
// This records the product name and version. It is used both to display
// information to the user (like in an About box) and to generate product-specific
// information (like where to look for its installation information).
/////////////////////////////////////////////////////////////////////////////

class CProductInfo {
public:
    enum VersionConstants {
        // These are the values for m_ReleaseType.
        Development                 = 0,
        Alpha                       = 1,
        Beta                        = 2,
        Release                     = 3,

        // Printing options
        PRINT_SOFTWARE_NAME         = 0x01,
        PRINT_RELEASE_TYPE          = 0x02,
        PRINT_BUILD_NUMBER          = 0x04,
        PRINT_SHORT_SOFTWARE_NAME   = 0x08,
        PRINT_BUILD_NUMBER_ONLY     = 0x10,
    };

    const char  *m_CompanyName;
    const char  *m_ProductName;
    const char  *m_Description;

    uint32      m_MajorVersion;
    uint32      m_MinorVersion;
    uint32      m_MinorMinorVersion;

    uint32      m_Milestone;

    uint32      m_BuildNumber;
    const char  *m_pBuildDate;

    uint8       m_ReleaseType;
    uint32      m_ReleaseNumber;

    // This allows a version of a product to specify where its config 
    // file is located, and the home directory for the software.
    // By default, these are both NULL and so the system generates the file
    // name and location from things like the product name, and 
    // system state like the Registry on windows or environment variables.
    const char  *m_pConfigFile;
    const char  *m_pHomeDirectory;
}; // CProductInfo.


// This is implemented in the osIndependantLayer.cpp
extern CProductInfo g_SoftwareVersion;





/////////////////////////////////////////////////////////////////////////////
//
//                          ERROR HANDLING
//
// We may pass an expression into gotoErr or returnErr.
// Do not evaluate the expression twice; that may have illegal side 
// effects and/or be expensive. Instead, evaluate the expression
// once by assigning it to a temporary variable, and then work
// with that temporary expression.
/////////////////////////////////////////////////////////////////////////////

void BreakToDebugger();

#define gotoErr(_errParam) do { err = _errParam; if (ENoErr != err) { BreakToDebugger(); } goto abort; } while (0)
#define returnErr(_errParam) do { if (ENoErr != _errParam) { BreakToDebugger(); } return(_errParam); }  while (0)



/////////////////////////////////////////////////////////////////////////////
//
//                              CONSOLES
//
/////////////////////////////////////////////////////////////////////////////

// These are implemented differently for every user interface.
class CConsoleUserInterface {
public:
    virtual void PrintToConsole(const char *pStr) = 0;
    virtual void ShowProgress() = 0;
    virtual void ReportError(const char *pStr) = 0;
}; // CConsoleUserInterface.



/////////////////////////////////////////////////////////////////////////////
//
//                          GLOBAL PROCEDURES
//
// Some global functions for the module.
/////////////////////////////////////////////////////////////////////////////

class OSIndependantLayer {
public:
    static void BreakToDebugger() { }

    // Console Output
    static void ResetConsoles() { }
    static void SetConsole(CConsoleUserInterface *pConsole) { pConsole = pConsole; }
}; // OSIndependantLayer

extern bool g_AllowBreakToDebugger;



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

   static ErrVal Initialize(int32 featureLevel, CProductInfo *pProductInfo) {
        featureLevel = featureLevel;
        pProductInfo = pProductInfo;
        return(ENoErr);
    }
   static void Shutdown() { }
}; // CBuildingBlocks





/////////////////////////////////////////////////////////////////////////////
//
//                              DEBUGGING
//
/////////////////////////////////////////////////////////////////////////////

#define LOG_LEVEL_DEFAULT   1
#define FILE_DEBUGGING_GLOBALS(maxLogLevel, flags) 




/////////////////////////////////////////////////////////////////////////////
//
//                             ASSERTIONS
//
/////////////////////////////////////////////////////////////////////////////

#undef ASSERT

#if DD_DEBUG
// #cond converts the expression into a string. This produces
// a string of the actual source code for the expression.
#define ASSERT(cond) { if (!(cond)) BreakToDebugger(); }
#define ASSERT_UNTESTED() { BreakToDebugger(); }

#if WIN32
#define ASSERT_WIN32(cond) { if (!(cond)) BreakToDebugger(); }
#define ASSERT_LINUX(cond) 
#elif LINUX
#define ASSERT_WIN32(cond) 
#define ASSERT_LINUX(cond) { if (!(cond)) BreakToDebugger(); }
#endif
#else // DD_DEBUG
#define ASSERT(cond) 
#define ASSERT_WIN32(cond) 
#define ASSERT_LINUX(cond) 
#define ASSERT_UNTESTED() 
#endif // DD_DEBUG





/////////////////////////////////////////////////////////////////////////////
//
//                             Memory Allocation
//
/////////////////////////////////////////////////////////////////////////////

#define memAlloc(size) new char[size]
extern char *memCalloc(int32 size);
#define memFree(ptr) do { if (ptr) { delete (char *) ptr; } } while (0)
// free(ptr);

#define NEWEX_IMPL() 
#define newex new



/////////////////////////////////////////////////////////////////////////////
//
//                              String functions
//
/////////////////////////////////////////////////////////////////////////////

#define strncpyex(pDestPtr, pSrcPtr, maxLength) CopyUTF8String(pDestPtr, pSrcPtr, maxLength)

#if WIN32
#define strcasecmpex(pStr1, pStr2) stricmp(pStr1, pStr2)
#define strncasecmpex(pStr1, pStr2, length) _strnicmp(pStr1, pStr2, length)
#else
#define strcasecmpex(pStr1, pStr2) strcasecmp(pStr1, pStr2)
#define strncasecmpex(pStr1, pStr2, length) strncasecmp(pStr1, pStr2, length)
#endif

char *strCatEx(const char *pVoidPtr, const char *pSuffixVoidPtr);
char *strdupex(const char *pVoidPtr);
void CopyUTF8String(char *pDestPtr, const char *pSrcPtr, int32 maxLength);

void WASMmemcpy(void *pDestVoidPtr, const void *pSrcVoidPtr, int32 maxLength);
#if WASM
int32 strlen(const char *pPtr);
#endif



/////////////////////////////////////////////////////////////////////////////
//
//                              Date and Time
//
/////////////////////////////////////////////////////////////////////////////

class CDateTime {
public:
    int64    m_TimeVal;

    // Print options
    enum PrintOptions {
        PRINT_24_HOUR_TIME  = 0x01,
        PRINT_SECONDS       = 0x02,
        DATE_ONLY           = 0x40,
    }; // PrintOptions

    void GetLocalDateAndTime();
    bool IsLessThanEqual(CDateTime *pOther);

    void GetValue(
            int32 *dayOfWeek,
            int32 *month,
            int32 *hour,
            int32 *minutes,
            int32 *seconds,
            int32 *date,
            int32 *year,
            const char **tzNameStr);
    void SetValue(
            int32 month,
            int32 hour,
            int32 minutes,
            int32 seconds,
            int32 date,
            int32 year,
            char *tzNameStr);

    void PrintToString(
            char *destPtr,
            int32 maxBufferSize,
            int32 options,
            char **ppResultPtr);

private:
    void PrintToStringInStandardFormat(
                int32 month,
                int32 hour,
                int32 minutes,
                int32 seconds,
                int32 date,
                int32 year,
                char *destPtr,
                char *endDest,
                int32 options,
                char **ppResultPtr);
}; // CDateTime


extern void PrintTimeInMsToString(uint64 timeInMs, char *pDestPtr, int32 maxBufferSize);
extern uint64 GetTimeSinceBootInMs();




/////////////////////////////////////////////////////////////////////////////
//
//                              FILES
//
/////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////
class CSimpleFile {
public:
    enum {
        // OpenOptions
        READ_ONLY           = 0x0001,
        SHARE_WRITE         = 0x0002,
        EXCLUSIVE_ACCESS    = 0x0004,
        EXPECT_TO_FIND_FILE = 0x0008,

        // Common Options
        RECURSIVE           = 0x0100,

        // Seek
#if WIN32
        SEEK_START          = FILE_BEGIN,
        SEEK_FILE_END       = FILE_END
#elif LINUX
        SEEK_START          = SEEK_SET,
        SEEK_FILE_END       = SEEK_END
#elif WASM
        SEEK_START          = 0,
        SEEK_FILE_END       = 1
#endif
    };


    CSimpleFile();
    ~CSimpleFile();

    ErrVal OpenExistingFile(const char *pFileName, int32 flags);
    ErrVal OpenOrCreateFile(const char *pFileName, int32 flags);
    ErrVal OpenOrCreateEmptyFile(const char *pFileName, int32 flags);

    void Close();
    bool IsOpen();

    ErrVal Read(
            void *pBuffer,
            int32 numBytesToRead,
            int32 *pNumBytesRead);
    ErrVal Write(
            const void *pBuffer,
            int32 numBytesToWrite);
    ErrVal Flush();

    ErrVal Seek(uint64 offsetFromStart);
    ErrVal Seek(uint64 offset, int32 whence);
    ErrVal GetFilePosition(uint64 *pos);

    ErrVal GetFileLength(uint64 *pdwLength);
    ErrVal SetFileLength(uint64 newLength);

#if LINUX
    int GetFD() { return(m_FileHandle); }
    void ForgetFD() { m_FileHandle = -1; }
#elif WIN32
    HANDLE GetFD() { return(m_FileHandle); }
    void ForgetFD() { m_FileHandle = INVALID_HANDLE_VALUE; }
#endif

    static ErrVal DeleteFile(const char *pFileName);
    static bool IsDirectory(const char *pDirName);

private:
#if WIN32
    HANDLE      m_FileHandle;
#elif LINUX
    int         m_FileHandle;
#endif
}; // CSimpleFile



#if WIN32
#define DIRECTORY_SEPARATOR_CHAR '\\'
#define IS_DIRECTORY_SEPARATOR(c) (('/' == (c)) || ('\\' == (c)))
#elif LINUX
#define DIRECTORY_SEPARATOR_CHAR '/'
#define IS_DIRECTORY_SEPARATOR(c) ('/' == (c))
#endif





/////////////////////////////////////////////////////////////////////////////
class CDirFileList {
public:
    CDirFileList();
    ~CDirFileList();
    
    ErrVal Open(const char *pDirPath);
    void Close();
    
    bool GetNextFile(char *pFileName, int32 maxPath);

private:
    enum CDirFileListConstants {
        MAX_DIR_PATH   = 2048
    };

    int32               m_CurrentPathLen;
    char                m_CurrentPath[MAX_DIR_PATH];

    int                 m_FileIndex;

#if WIN32
    HANDLE              m_SearchHandle;
    WIN32_FIND_DATAA    m_fileInfo;
#endif

#if LINUX
   DIR                  *m_pDir;
#endif
}; // CDirFileList




#endif // _OS_INDEPENDANT_LAYER_H_


