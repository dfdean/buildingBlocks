/////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2005-2018 Dawson Dean
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

#ifndef _OS_INDEPENDANT_LAYER_H_
#define _OS_INDEPENDANT_LAYER_H_

#define DD_DEBUG 1

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
enum {
    ENoErr                     = 0,

    //////////////////////////////////////////////////////
    // Common Errors
    EFail                       = 1 | ERROR_IS_UNEXPECTED,
    EOutOfMemory                = 2 | ERROR_IS_UNEXPECTED,
    ENotImpl                    = 5 | ERROR_IS_UNEXPECTED,
    EInvalidArg                 = 7 | ERROR_IS_UNEXPECTED,
    EValueIsNotNumber           = 8,
    ESyntheticError             = 9,

    //////////////////////////////////////////////////////
    // File Errors
    EEOF                        = 100,
    EFileNotFound               = 101 | ERROR_IS_UNEXPECTED,
    ERequiredFileNotFound       = 102 | ERROR_IS_UNEXPECTED,
    EAccessDenied               = 103,
    ENoDiskSpace                = 104,
    EFileIsBusy                 = 105,
    EReadOnly                   = 106,

    //////////////////////////////////////////////////////
    // Network errors
    ENoResponse                 = 200,
    ENoHostAddress              = 201,
    EPeerDisconnected           = 202,
    ETooManySockets             = 203,
    EHTTPSRequired              = 204,

    //////////////////////////////////////////////////////
    // User option errors
    EMissingConfigFile          = 300, // This is NOT a bug.
    EInvalidConfigFile          = 301 | ERROR_IS_UNEXPECTED,

    //////////////////////////////////////////////////////
    // HTTP errors
    EInvalidUrl                 = 400,
    EInvalidHttpHeader          = 401,
    EHTTPDocTooLarge            = 402,

    //////////////////////////////////////////////////////
    // XML Parsing errors
    EXMLParseError              = 500,
    EWrongAttributeType         = 501 | ERROR_IS_UNEXPECTED,

    //////////////////////////////////////////////////////
    // General User Data File Errors
    EDataFileSyntaxError        = 600,
    EDataFileItemNotFound       = 601,
    EInvalidRequest             = 602,
}; // ErrVal


#if LINUX
#define GET_LAST_ERROR() errno
#elif WIN32
#define GET_LAST_ERROR() GetLastError()
#endif

#define ERROR_CODE(err) ((err & ~ERROR_IS_UNEXPECTED))

#if WIN32
ErrVal TranslateWin32ErrorIntoErrVal(DWORD dwErr, bool alwaysReturnErr);
#endif



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

    CProductInfo();
    ErrVal PrintToString(
                char *destPtr,
                int32 maxBufferSize,
                int32 options,
                char **destResult);
}; // CProductInfo.


// This is implemented in the osIndependantLayer.cpp
extern CProductInfo g_SoftwareVersion;





/////////////////////////////////////////////////////////////////////////////
//
//                              TIME
//
// Time is always milliseconds since the computer started. If you
// want the time of the day, then use CDateTime.
/////////////////////////////////////////////////////////////////////////////

uint64 GetTimeSinceBootInMs();
void PrintTimeInMsToString(uint64 timeInMs, char *destPtr, int32 maxBufferSize);



/////////////////////////////////////////////////////////////////////////////
//
//                              DATE
//
/////////////////////////////////////////////////////////////////////////////

class CDateTime {
public:
    int64    m_TimeVal;

    // Print options
    enum PrintOptions {
        PRINT_24_HOUR_TIME  = 0x01,
        PRINT_SECONDS       = 0x02,
        W3C_LOG_FORMAT      = 0x04,
        FILE_NAME_FORMAT    = 0x08,
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

    void PrintToStringInW3CFormat(
                int32 month,
                int32 hour,
                int32 minutes,
                int32 seconds,
                int32 date,
                int32 year,
                char *destPtr,
                char *endDest,
                char **ppResultPtr);

    void PrintToStringInFileNameFormat(
                int32 month,
                int32 date,
                int32 year,
                char *destPtr,
                char *endDest,
                char **ppResultPtr);
}; // CDateTime




/////////////////////////////////////////////////////////////////////////////
//
//                              LOCKING
//
/////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////
class OSIndependantLock {
public:
    OSIndependantLock();
    ~OSIndependantLock();

    ErrVal Initialize();
    void Shutdown();

    void BasicLock();
    void BasicUnlock();

private:
    bool                m_fInitialized;

#if WIN32
    CRITICAL_SECTION    m_Lock;
#elif LINUX
    pthread_mutex_t     m_Lock;
#endif
}; // OSIndependantLock






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
    static ErrVal MoveFile(const char *pSrcPath, const char *pDestPath);

    static bool FileExists(const char *pFileName);
    static bool DirectoryExists(const char *pDirName);
    static bool FileOrDirectoryExists(const char *pFileName);
    static bool IsDirectory(const char *pDirName);

    static ErrVal CreateDirectory(const char *pDirName);
    static ErrVal DeleteDirectory(const char *pDirName);
    static ErrVal EmptyDirectory(const char *pDirName);

    static char *GetFileNameFromPath(const char *pPathName);

private:
#if WIN32
    HANDLE      m_FileHandle;
#elif LINUX
    int         m_FileHandle;
#endif
}; // CSimpleFile





#if WIN32
#define DIRECTORY_SEPARATOR_CHAR '\\'
#define DIRECTORY_SEPARATOR_CHAR_STRING "\\"
#define IS_DIRECTORY_SEPARATOR(c) (('/' == (c)) || ('\\' == (c)))
#elif LINUX
#define DIRECTORY_SEPARATOR_CHAR '/'
#define DIRECTORY_SEPARATOR_CHAR_STRING "/"
#define IS_DIRECTORY_SEPARATOR(c) ('/' == (c))
#endif

extern char g_SoftwareDirectoryRoot[2048];



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
    WIN32_FIND_DATAW    m_fileInfo;
#elif LINUX
   DIR                  *m_pDir;
#endif
}; // CDirFileList




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
    static void BreakToDebugger();
    static int32 GetCurrentThreadId();

    static ErrVal InitializeOSIndependantLayer();
    static void ShutdownOSIndependantLayer();

#if INCLUDE_REGRESSION_TESTS
    static void TestOSIndependantLayer();
#endif

    static void GetOSVersionString(char *pVersionString, int maxStrSize);

    // Console Output
    static void ResetConsoles();
    static void SetConsole(CConsoleUserInterface *pConsole);
    static void RemoveConsole(CConsoleUserInterface *pConsole);
    static void PrintToConsole(const char *pFormat, ...);
    static void ShowProgress();
    static void ReportError(const char *pFileName, int lineNum, const char *pMsg);

    // Sleeping
    static void SleepForMilliSecs(int32 numMilliSecs);

    // Random Number Generator
    static void SetRandSeed(int32 seedVal);
    static int32 GetRandomNum();
}; // OSIndependantLayer

extern bool g_AllowBreakToDebugger;

// This is used to report a problem in low level code that is below
// the debugging layer in the module hierarchy.
#define REPORT_LOW_LEVEL_BUG()   OSIndependantLayer::ReportError(__FILE__, __LINE__, "Low level bug")

#endif // _OS_INDEPENDANT_LAYER_H_



