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
// Operating System Independant Layer
//
// This module abstracts some of the simple and common OS-specific
// features. A lot of os-dependent functions, like threading or network
// IO, are abstracted by higher level modules in the building blocks. All
// OS-dependencies are concealed by the overall Building Blocks, but this
// particular module wraps some of the simpler functions that are also used
// by the rest of the building blocks.
//
// This is the lowest level module in the building blocks, so it cannot
// have any dependencies on other modules.
//
// Windows
// Currently, this is designed around basic WinNT functionality (NT4 or later).
// I haven't found much that requires Win9x, except I do use an ioCompletion
// port for the async file IO. My main development system runs Windows XP Pro,
// so it gets the most stress there.
//
// Linux
// This has been tested on Linux (Red Hat Fedora Release 18)
/////////////////////////////////////////////////////////////////////////////

#include <time.h>

#if LINUX
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <stdarg.h>
#include <signal.h>
#include <math.h>
#endif // if LINUX

#include "osIndependantLayer.h"

// Here is the one circular dependency in the system.
//
// * OSIndependentLayer uses StringLib, for things like converting file names
// into UTF-8.
//
// * StringLib uses OSIndependentLayer.h for things like the basic data 
// types (int32) and testing utilities.
//
// There are several ways to resolve this. Combine the files, or split 
// osIndependent layer into one file that doesn't depend on stringLib and
// one that does. However, this interdependency doesn't really matter so
// I am leaving it.
#if WIN32
#include "stringLib.h"
#endif


#if WIN32
#include <excpt.h>
#define NULL_FILE_HANDLE     INVALID_HANDLE_VALUE
#elif LINUX
#define NULL_FILE_HANDLE     -1
#endif

bool g_ShutdownBuildingBlocks = false;
bool g_AllowBreakToDebugger = true;
CProductInfo g_SoftwareVersion;

#define MAX_CONSOLES  10
static CConsoleUserInterface *g_pConsoleList[MAX_CONSOLES];

// Months are 1-based.
static const char *g_MonthNames[] = {"", "Jan", "Feb", "Mar",
		        "Apr", "May", "Jun",
		        "Jul", "Aug", "Sep",
		        "Oct", "Nov", "Dec"};


void CopyUTF8String(char *pDestPtr, const char *pSrcPtr, int32 maxLength);


#if WIN32
typedef void (WINAPI *GetNativeSystemInfoProcType)(LPSYSTEM_INFO);
typedef BOOL (WINAPI *GetProductInfoProcType)(DWORD, DWORD, DWORD, DWORD, PDWORD);
void GetOSVersionString(char *pVersionString, int maxStrSize);

typedef struct CVersionName {
   int   intVal;
   char  *pName;
} CVersionName;

#define PRODUCT_BUSINESS 0x00000006 // Business Edition
#define PRODUCT_BUSINESS_N 0x00000010 // Business Edition
#define PRODUCT_CLUSTER_SERVER 0x00000012 // Cluster Server Edition
#define PRODUCT_DATACENTER_SERVER 0x00000008 // Server Datacenter Edition (full installation)
#define PRODUCT_DATACENTER_SERVER_CORE 0x0000000C // Server Datacenter Edition (core installation)
#define PRODUCT_DATACENTER_SERVER_CORE_V 0x00000027 // Server Datacenter Edition without Hyper-V (core installation)
#define PRODUCT_DATACENTER_SERVER_V 0x00000025 // Server Datacenter Edition without Hyper-V (full installation)
#define PRODUCT_ENTERPRISE 0x00000004 // Enterprise Edition
#define PRODUCT_ENTERPRISE_N 0x0000001B // Enterprise Edition
#define PRODUCT_ENTERPRISE_SERVER 0x0000000A // Server Enterprise Edition (full installation)
#define PRODUCT_ENTERPRISE_SERVER_CORE 0x0000000E // Server Enterprise Edition (core installation)
#define PRODUCT_ENTERPRISE_SERVER_CORE_V 0x00000029 // Server Enterprise Edition without Hyper-V (core installation)
#define PRODUCT_ENTERPRISE_SERVER_IA64 0x0000000F // Server Enterprise Edition for Itanium-based Systems
#define PRODUCT_ENTERPRISE_SERVER_V 0x00000026 // Server Enterprise Edition without Hyper-V (full installation)
#define PRODUCT_HOME_BASIC 0x00000002 // Home Basic Edition
#define PRODUCT_HOME_BASIC_N 0x00000005 // Home Basic Edition
#define PRODUCT_HOME_PREMIUM 0x00000003 // Home Premium Edition
#define PRODUCT_HOME_PREMIUM_N 0x0000001A // Home Premium Edition
#define PRODUCT_HOME_SERVER 0x00000013 // Home Server Edition
#define PRODUCT_MEDIUMBUSINESS_SERVER_MANAGEMENT 0x0000001E // Windows Essential Business Server Management Server
#define PRODUCT_MEDIUMBUSINESS_SERVER_MESSAGING 0x00000020 // Windows Essential Business Server Messaging Server
#define PRODUCT_MEDIUMBUSINESS_SERVER_SECURITY 0x0000001F // Windows Essential Business Server Security Server
#define PRODUCT_SERVER_FOR_SMALLBUSINESS 0x00000018 // Server for Small Business Edition
#define PRODUCT_SMALLBUSINESS_SERVER 0x00000009 // Small Business Server
#define PRODUCT_SMALLBUSINESS_SERVER_PREMIUM 0x00000019 // Small Business Server Premium Edition
#define PRODUCT_STANDARD_SERVER 0x00000007 // Server Standard Edition (full installation)
#define PRODUCT_STANDARD_SERVER_CORE 0x0000000D // Server Standard Edition (core installation)
#define PRODUCT_STANDARD_SERVER_CORE_V 0x00000028 // Server Standard Edition without Hyper-V (core installation)
#define PRODUCT_STANDARD_SERVER_V 0x00000024 // Server Standard Edition wo Hyper-V (full installation)
#define PRODUCT_STARTER 0x0000000B // Starter Edition
#define PRODUCT_STORAGE_ENTERPRISE_SERVER 0x00000017 // Storage Server Enterprise
#define PRODUCT_STORAGE_EXPRESS_SERVER 0x00000014 // Storage Server Express
#define PRODUCT_STORAGE_STANDARD_SERVER 0x00000015 // Storage Server Standard
#define PRODUCT_STORAGE_WORKGROUP_SERVER 0x00000016 // Storage Server Workgroup
#define PRODUCT_UNDEFINED 0x00000000 // An unknown product
#define PRODUCT_ULTIMATE 0x00000001 // Ultimate Edition
#define PRODUCT_ULTIMATE_N 0x0000001C // Ultimate Edition
#define PRODUCT_WEB_SERVER 0x00000011 // Web Server Edition (full installation)
#define PRODUCT_WEB_SERVER_CORE 0x0000001D

static CVersionName g_NTVersions[] = {
{PRODUCT_ULTIMATE, "Ultimate Edition"},
{PRODUCT_HOME_PREMIUM, "Home Premium Edition"},
{PRODUCT_HOME_BASIC, "Home Basic Edition"},
{PRODUCT_ENTERPRISE, "Enterprise Edition"},
{PRODUCT_BUSINESS, "Business Edition"},
{PRODUCT_STARTER, "Starter Edition"},
{PRODUCT_CLUSTER_SERVER, "Cluster Server Edition"},
{PRODUCT_DATACENTER_SERVER, "Datacenter Edition"},
{PRODUCT_DATACENTER_SERVER_CORE, "Datacenter Edition (core installation)"},
{PRODUCT_ENTERPRISE_SERVER, "Enterprise Edition"},
{PRODUCT_ENTERPRISE_SERVER_CORE, "Enterprise Edition (core installation)"},
{PRODUCT_ENTERPRISE_SERVER_IA64, "Enterprise Edition for Itanium-based Systems"},
{PRODUCT_SMALLBUSINESS_SERVER, "Small Business Server"},
{PRODUCT_SMALLBUSINESS_SERVER_PREMIUM, "Small Business Server Premium Edition"},
{PRODUCT_STANDARD_SERVER, "Standard Edition"},
{PRODUCT_STANDARD_SERVER_CORE, "Standard Edition (core installation)"},
{PRODUCT_WEB_SERVER, "Web Server Edition"},
{0, NULL} };
static char *GetStringForValue(int val, CVersionName *pList);


#ifndef SM_SERVERR2
#define SM_SERVERR2 89
#endif

#ifndef SM_STARTER
#define SM_STARTER 88
#endif

#ifndef VER_SUITE_COMPUTE_SERVER
#define VER_SUITE_COMPUTE_SERVER 0x00004000
#endif

#ifndef VER_SUITE_DATACENTER
#define VER_SUITE_DATACENTER               128
#endif

#endif // WIN32


// This may be dynamically changes by a command line or 
// from an environment variable.
char g_SoftwareDirectoryRoot[2048];




/////////////////////////////////////////////////////////////////////////////
//
// [InitializeOSIndependantLayer]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
OSIndependantLayer::InitializeOSIndependantLayer() {
    ErrVal err = ENoErr;

    // This may be dynamically changes by a command line or 
    // from an environment variable.
    if (0 == g_SoftwareDirectoryRoot[0]) {
#if WIN32
        snprintf(g_SoftwareDirectoryRoot, sizeof(g_SoftwareDirectoryRoot),
                 "C:\\");
#elif LINUX
        snprintf(g_SoftwareDirectoryRoot, sizeof(g_SoftwareDirectoryRoot),
                 "/home/ddean/");
#endif
    }

#if WIN32
    WSADATA wsaData;
    WORD wsaVersionRequested;
    int32 result;

    wsaVersionRequested = MAKEWORD(2, 0);
    result = WSAStartup(wsaVersionRequested, &wsaData);
    if (0 != result) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }
#endif

    return(err);
} // InitializeOSIndependantLayer




/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
void
OSIndependantLayer::ShutdownOSIndependantLayer() {
#if WIN32
   // This affects the entire process. So, don't do this if you run as a
   // DLL in a parent application.
   //WSACleanup();
#endif
} // ShutdownOSIndependantLayer





#if WIN32
/////////////////////////////////////////////////////////////////////////////
//
// [TranslateWin32ErrorIntoErrVal]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
TranslateWin32ErrorIntoErrVal(DWORD dwErr, bool alwaysReturnErr) {
    switch (dwErr) {
    case ERROR_SUCCESS:
        if (alwaysReturnErr) {
            return(EFail);
        }
        else {
            return(ENoErr);
        }
        break;

    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return(EFileNotFound);
        break;

    case ERROR_TOO_MANY_OPEN_FILES:
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return(EOutOfMemory);
        break;

    case ERROR_ACCESS_DENIED:
        return(EAccessDenied);
        break;

    case ERROR_WRITE_PROTECT:
        return(EReadOnly);
        break;

    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return(EFileIsBusy);
        break;

    case ERROR_HANDLE_EOF:
        return(EEOF);
        break;

    case ERROR_HANDLE_DISK_FULL:
        return(ENoDiskSpace);
        break;

    default:
        return(EFail);
        break;
    }
} // TranslateWin32ErrorIntoErrVal
#endif




/////////////////////////////////////////////////////////////////////////////
//
// [OSIndependantLock]
//
/////////////////////////////////////////////////////////////////////////////
OSIndependantLock::OSIndependantLock() {
    m_fInitialized = false;
} // OSIndependantLock




/////////////////////////////////////////////////////////////////////////////
//
// [~OSIndependantLock]
//
/////////////////////////////////////////////////////////////////////////////
OSIndependantLock::~OSIndependantLock() {
    Shutdown();
} // ~OSIndependantLock




/////////////////////////////////////////////////////////////////////////////
//
// [Initialize]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
OSIndependantLock::Initialize() {
    bool fSuccess = true;

#if WIN32
     __try {
        // #if (_WIN32_WINNT >= 0x0500)
        //fSuccess = InitializeCriticalSectionAndSpinCount(pLock, 5);
        InitializeCriticalSection(&m_Lock);
     } __except (EXCEPTION_EXECUTE_HANDLER) {
        REPORT_LOW_LEVEL_BUG();
        fSuccess = false;
     }
#elif LINUX
     pthread_mutexattr_t mutexAttr;
     int result;

     pthread_mutexattr_init(&mutexAttr);

     result = pthread_mutexattr_settype(&mutexAttr, PTHREAD_MUTEX_RECURSIVE_NP);
     if (0 != result) {
         fSuccess = false;
         goto abort;
      }

    result = pthread_mutex_init(&m_Lock, &mutexAttr);
    if (0 != result) {
        fSuccess = false;
        goto abort;
    }
abort:
      pthread_mutexattr_destroy(&mutexAttr);
#endif

     if (fSuccess) {
         m_fInitialized = true;
         return(ENoErr);
     } else {
         return(EFail);
     }
} // Initialize





/////////////////////////////////////////////////////////////////////////////
//
// [Shutdown]
//
/////////////////////////////////////////////////////////////////////////////
void
OSIndependantLock::Shutdown() {
    if (m_fInitialized) {
#if WIN32
        __try {
            DeleteCriticalSection(&m_Lock);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            m_fInitialized = m_fInitialized;
        }
#elif LINUX
        pthread_mutex_destroy(&m_Lock);
#endif
    }

    m_fInitialized = false;
} // Shutdown





/////////////////////////////////////////////////////////////////////////////
//
// [BasicLock]
//
/////////////////////////////////////////////////////////////////////////////
void
OSIndependantLock::BasicLock() {
#if WIN32
    __try {
        EnterCriticalSection(&m_Lock);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        m_Lock = m_Lock;
    }
#elif LINUX
    pthread_mutex_lock(&m_Lock);
#endif
} // BasicLock






/////////////////////////////////////////////////////////////////////////////
//
// [BasicUnlock]
//
/////////////////////////////////////////////////////////////////////////////
void
OSIndependantLock::BasicUnlock() {
#if WIN32
    __try {
        LeaveCriticalSection(&m_Lock);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        m_Lock = m_Lock;
    }
#elif LINUX
    pthread_mutex_unlock(&m_Lock);
#endif
} // BasicUnlock




/////////////////////////////////////////////////////////////////////////////
//
// [PrintTimeInMsToString]
//
/////////////////////////////////////////////////////////////////////////////
void
PrintTimeInMsToString(
                uint64 timeInMs,
                char *pDestPtr,
                int32 maxBufferSize) {
    char *pEndDestPtr;
    uint64 milliSeconds = 0;
    uint64 seconds = 0;
    uint64 minutes = 0;
    uint64 hours = 0;
    uint64 days = 0;
    bool fStartedString = false;


    if ((NULL == pDestPtr)
        || (maxBufferSize <= 1)) {
        return;
    }
    *pDestPtr = 0;
    pEndDestPtr = pDestPtr + maxBufferSize;


    // Handle a few special cases.
    if (0 == timeInMs) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        "0 seconds");
        return;
    } else if (timeInMs < 1000) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        INT64FMT " milliseconds",
                        timeInMs);
        return;
    }

    milliSeconds = timeInMs % 1000;

    seconds = timeInMs / 1000;

    // Seconds is anything that didn't round up into minutes.
    minutes = seconds / 60;
    seconds = seconds - (minutes * 60);

    // Minutes is anything that didn't round up into hours.
    hours = minutes / 60;
    minutes = minutes - (hours * 60);

    // Hours is anything that didn't round up into days.
    days = hours / 24;
    hours = hours - (days * 24);



    /////////////////////////////////
    if (1 == days) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        "%d day, ",
                        (int32) days);
        fStartedString = true;
    } else if (days > 0) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        "%d days, ",
                        (int32) days);
        fStartedString = true;
    }


    /////////////////////////////////
    if (1 == hours) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        "%d hour, ",
                        (int32) hours);
        fStartedString = true;
    } else if ((hours > 0) || (fStartedString)) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        "%d hours, ",
                        (int32) hours);
        fStartedString = true;
    }



    /////////////////////////////////
    if (1 == minutes) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        "%d minute, ",
                        (int32) minutes);
        fStartedString = true;
    } else if ((minutes > 1) || (fStartedString)) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        "%d minutes, ",
                        (int32) minutes);
        fStartedString = true;
    }


    /////////////////////////////////
    if (1 == seconds) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        "%d second, ",
                        (int32) seconds);
        fStartedString = true;
    } else if ((seconds > 0) || (fStartedString)) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        "%d seconds, ",
                        (int32) seconds);
        fStartedString = true;
    }


    /////////////////////////////////
    if (1 == milliSeconds) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        "%d milliSecond",
                        (int32) milliSeconds);
        fStartedString = true;
    } else if ((milliSeconds > 0) || (fStartedString)) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        "%d milliSeconds",
                        (int32) milliSeconds);
        fStartedString = true;
    }
} // PrintTimeInMsToString.






/////////////////////////////////////////////////////////////////////////////
//
// [CProductInfo]
//
/////////////////////////////////////////////////////////////////////////////
CProductInfo::CProductInfo() {
    m_CompanyName = NULL;
    m_ProductName = NULL;
    m_Description = NULL;

    m_MajorVersion = 0;
    m_MinorVersion = 0;
    m_MinorMinorVersion = 0;
    m_Milestone = 0;

    m_BuildNumber = 0;
    m_pBuildDate = NULL;

    m_ReleaseType = Development;
    m_ReleaseNumber = 1;

    m_pConfigFile = NULL;
    m_pHomeDirectory = NULL;
} // CProductInfo




/////////////////////////////////////////////////////////////////////////////
//
// [PrintToString]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CProductInfo::PrintToString(
                        char *pDestPtr,
                        int32 maxBufferLength,
                        int32 options,
                        char **destResult) {
    char *pStartBuffer;
    char *pEndDestPtr;

    // destResult is optional, so it may be NULL.
    if ((NULL == pDestPtr)
        || (maxBufferLength <= 0)) {
        return(EFail);
    }
    pStartBuffer = pDestPtr;
    pEndDestPtr = pDestPtr + maxBufferLength;
    *pDestPtr = 0;

    if (options & PRINT_BUILD_NUMBER_ONLY) {
        goto printBuildNumberOnly;
    } // PRINT_BUILD_NUMBER_ONLY


    ///////////////////////////////////////
    if (((options & PRINT_SOFTWARE_NAME)
            || (options & PRINT_SHORT_SOFTWARE_NAME))
         && (NULL != m_ProductName)) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        "%s",
                        m_ProductName);
    }
    if (options & PRINT_SHORT_SOFTWARE_NAME) {
        goto abort;
    }
    if ((pStartBuffer != pDestPtr) && (pDestPtr < pEndDestPtr)) {
       *(pDestPtr++) = ' ';
       *pDestPtr = 0;
    }

    ///////////////////////////////////////
    pDestPtr += snprintf(
                    pDestPtr,
                    pEndDestPtr - pDestPtr,
                    "%d.%d",
                    m_MajorVersion,
                    m_MinorVersion);
    if (m_MinorMinorVersion > 0) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        ".%d",
                        m_MinorMinorVersion);
    }

    if (m_Milestone > 0) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        " Milestone %d",
                        m_Milestone);
    }

    ///////////////////////////////////////
    if (options & PRINT_RELEASE_TYPE) {
        if (CProductInfo::Development == m_ReleaseType) {
            pDestPtr += snprintf(
                            pDestPtr,
                            pEndDestPtr - pDestPtr,
                            " (Pre-Release - Milestone %d)",
                            m_ReleaseNumber);
        }
        else if ((CProductInfo::Alpha == m_ReleaseType)
                    && (m_ReleaseNumber > 0)) {
            pDestPtr += snprintf(
                            pDestPtr,
                            pEndDestPtr - pDestPtr,
                            " (Alpha %d)",
                            m_ReleaseNumber);
        }
        else if ((CProductInfo::Beta == m_ReleaseType)
                    && (m_ReleaseNumber > 0)) {
            pDestPtr += snprintf(
                            pDestPtr,
                            pEndDestPtr - pDestPtr,
                            " (Beta %d)",
                            m_ReleaseNumber);
        }
    }

    ///////////////////////////////////////
printBuildNumberOnly:
    if ((m_BuildNumber > 0)
        && ((options & PRINT_BUILD_NUMBER)
            || (options & PRINT_BUILD_NUMBER_ONLY))) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        " Build %d",
                        m_BuildNumber);
        if ((m_pBuildDate) && (*m_pBuildDate)) {
            pDestPtr += snprintf(
                            pDestPtr,
                            pEndDestPtr - pDestPtr,
                            " (%s)",
                            m_pBuildDate);
        }
    } // PRINT_BUILD_NUMBER

    ///////////////////////////////////////
    if ((options & PRINT_SOFTWARE_NAME) && (NULL != m_Description)) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        " %s",
                        m_Description);
    }

abort:
    if (NULL != destResult) {
        *destResult = pDestPtr;
    }

    return(ENoErr);
} // PrintToString.






/////////////////////////////////////////////////////////////////////////////
//
// [GetLocalDateAndTime]
//
/////////////////////////////////////////////////////////////////////////////
void
CDateTime::GetLocalDateAndTime() {
#if WIN32
   SYSTEMTIME localSystemTime;
   FILETIME localFileTime;

   GetLocalTime(&localSystemTime);
   SystemTimeToFileTime(&localSystemTime, &localFileTime);

   m_TimeVal = localFileTime.dwHighDateTime;
   m_TimeVal = m_TimeVal << 32;
   m_TimeVal |= localFileTime.dwLowDateTime;
#elif LINUX
    struct timeval timeInSecs;
    //struct timezone localTimeZone;

    gettimeofday(&timeInSecs, NULL);
    m_TimeVal = timeInSecs.tv_sec;
#endif  // LINUX
} // GetLocalDateAndTime.






/////////////////////////////////////////////////////////////////////////////
//
// [IsLessThanEqual]
//
/////////////////////////////////////////////////////////////////////////////
bool
CDateTime::IsLessThanEqual(CDateTime *pOther) {
    if (NULL == pOther) {
        return(false);
    }
    return(m_TimeVal < pOther->m_TimeVal);
} // IsLessThanEqual.






/////////////////////////////////////////////////////////////////////////////
//
// [GetValue]
//
//    year is the year as a 4-digit number.
//    month - January = 1, February = 2, and so on.
//    dayOfWeek - Sunday = 0, Monday = 1, and so on.
//    date - the current day of the month.
//    hour - the current hour.
//    minute - the current minute.
//    second - the current second.
/////////////////////////////////////////////////////////////////////////////
void
CDateTime::GetValue(
               int32 *dayOfWeek,
               int32 *month,
               int32 *hour,
               int32 *minutes,
               int32 *seconds,
               int32 *date,
               int32 *year,
               const char **tzNameStr) {
#if WIN32
   SYSTEMTIME localSystemTime;
   FILETIME localFileTime;

   // Convert the integer value to a struct.
   localFileTime.dwHighDateTime = (DWORD) (m_TimeVal >> 32);
   localFileTime.dwLowDateTime = (DWORD) (m_TimeVal & 0xFFFFFFFF);
   FileTimeToSystemTime(&localFileTime, &localSystemTime);

   if (NULL != dayOfWeek) {
      *dayOfWeek = localSystemTime.wDayOfWeek;
   }
   if (NULL != month) {
      *month = localSystemTime.wMonth;
   }
   if (NULL != date) {
      *date = localSystemTime.wDay;
   }
   if (NULL != year) {
      *year = localSystemTime.wYear;
   }
   if (NULL != hour) {
      *hour = localSystemTime.wHour;
   }
   if (NULL != minutes) {
      *minutes = localSystemTime.wMinute;
   }
   if (NULL != seconds) {
      *seconds = localSystemTime.wSecond;
   }
   if (NULL != tzNameStr) {
      *tzNameStr = _tzname[1];
   }
#elif LINUX
   struct tm *expandedTime;
   time_t now = m_TimeVal;

   expandedTime = localtime(&now);
   if (NULL == expandedTime) {
      return;
   }

   if (NULL != dayOfWeek) {
      *dayOfWeek = expandedTime->tm_wday;
   }
   if (NULL != month) {
      *month = expandedTime->tm_mon + 1;
   }
   if (NULL != date) {
      *date = expandedTime->tm_mday;
   }
   if (NULL != year) {
      *year = 1900 + expandedTime->tm_year;
   }
   if (NULL != hour) {
      *hour = expandedTime->tm_hour;
   }
   if (NULL != minutes) {
      *minutes = expandedTime->tm_min;
   }
   if (NULL != seconds) {
      *seconds = expandedTime->tm_sec;
   }
   if (NULL != tzNameStr) {
      *tzNameStr = "";
   }
#endif
} // GetValue





/////////////////////////////////////////////////////////////////////////////
//
// [SetValue]
//
//    year is the year as a 4-digit number.
//    month - January = 1, February = 2, and so on.
//    dayOfWeek - Sunday = 0, Monday = 1, and so on.
//    date - the current day of the month.
//    hour - the current hour.
//    minute - the current minute.
//    second - the current second.
/////////////////////////////////////////////////////////////////////////////
void
CDateTime::SetValue(
               int32 month,
               int32 hour,
               int32 minutes,
               int32 seconds,
               int32 date,
               int32 year,
               char *tzNameStr) {
   // Unused.
   tzNameStr = tzNameStr;

   // Sanity check the date.
   if ((month < 1)
         || (month > 12)
         || (hour < 0)
         || (hour > 24)
         || (minutes < 0)
         || (seconds < 0)
         || (date < 1)
         || (date > 31)
         || (year < 1900)
         || (year > 3000)) {
      REPORT_LOW_LEVEL_BUG();
   }

#if WIN32
   SYSTEMTIME localSystemTime;
   FILETIME localFileTime;

   localSystemTime.wYear = year;
   localSystemTime.wMonth = month;
   // The wDayOfWeek member of the SYSTEMTIME structure is ignored
   localSystemTime.wDayOfWeek = 0;
   localSystemTime.wDay = date;
   localSystemTime.wHour = hour;
   localSystemTime.wMinute = minutes;
   localSystemTime.wSecond = seconds;
   localSystemTime.wMilliseconds = 0;

   // First, convert the struct to an integer.
   SystemTimeToFileTime(&localSystemTime, &localFileTime);

   // Now, save this as an int64.
   m_TimeVal = localFileTime.dwHighDateTime;
   m_TimeVal = m_TimeVal << 32;
   m_TimeVal |= localFileTime.dwLowDateTime;
#elif LINUX
   struct tm expandedTime;

   expandedTime.tm_year = year - 1900;
   expandedTime.tm_mon = month - 1;
   expandedTime.tm_wday = 0;
   expandedTime.tm_mday = date;
   expandedTime.tm_hour = hour;
   expandedTime.tm_min = minutes;
   expandedTime.tm_sec = seconds;

   m_TimeVal = mktime(&expandedTime);
#endif
} // SetValue





/////////////////////////////////////////////////////////////////////////////
//
// [PrintToString]
//
// This prints the string in different formats used by some network
// standards. The format is usually defined by a protocol, not local
// conventions, so this does not need to be globalized/localized as
// much as you may think.
/////////////////////////////////////////////////////////////////////////////
void
CDateTime::PrintToString(
                char *pDestPtr,
                int32 maxBufferSize,
                int32 options,
                char **ppResultPtr) {
    char *pEndDestPtr;
    int32 dayOfWeek;
    int32 month;
    int32 hour;
    int32 minutes;
    int32 seconds;
    int32 date;
    int32 year;
    const char *tzNameStr;

    if (NULL != ppResultPtr) {
       *ppResultPtr = pDestPtr;
    }


    if ((NULL == pDestPtr)
        || (maxBufferSize <= 0)) {
        goto abort;
    }
    pEndDestPtr = pDestPtr + maxBufferSize - 1;

    GetValue(&dayOfWeek, &month, &hour, &minutes, &seconds, &date, &year, &tzNameStr);

    if (options & W3C_LOG_FORMAT) {
        PrintToStringInW3CFormat(
                  month,
                  hour,
                  minutes,
                  seconds,
                  date,
                  year,
                  pDestPtr,
                  pEndDestPtr,
                  &pDestPtr);
    } else if (options & FILE_NAME_FORMAT) {
        PrintToStringInFileNameFormat(
                  month,
                  date,
                  year,
                  pDestPtr,
                  pEndDestPtr,
                  &pDestPtr);
    } else {
        PrintToStringInStandardFormat(
                  month,
                  hour,
                  minutes,
                  seconds,
                  date,
                  year,
                  pDestPtr,
                  pEndDestPtr,
                  options,
                  &pDestPtr);
    }

    *pDestPtr = 0;
    if (NULL != ppResultPtr) {
        *ppResultPtr = pDestPtr;
    }

abort:
    return;
} // PrintToString.





/////////////////////////////////////////////////////////////////////////////
//
// [PrintToStringInStandardFormat]
//
/////////////////////////////////////////////////////////////////////////////
void
CDateTime::PrintToStringInStandardFormat(
                int32 month,
                int32 hour,
                int32 minutes,
                int32 seconds,
                int32 date,
                int32 year,
                char *pDestPtr,
                char *pEndDestPtr,
                int32 options,
                char **ppResultPtr) {
    pDestPtr += snprintf(
                pDestPtr,
                pEndDestPtr - pDestPtr,
                "%s %d, %d",
                g_MonthNames[month],
                date,
                year);

    if (options & CDateTime::DATE_ONLY) {
        goto abort;
    }
    pDestPtr += snprintf(
                pDestPtr,
                pEndDestPtr - pDestPtr,
                ", ");

    if (!(options & PRINT_24_HOUR_TIME) && (hour > 12)) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        "%d",
                        hour - 12);
    } else if (!(options & PRINT_24_HOUR_TIME) && (0 == hour)) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        "12");
    } else {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        "%d",
                        hour);
    }

    if (minutes < 10) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        ":0%d",
                        minutes);
    } else {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        ":%d",
                        minutes);
    }

    if (options & PRINT_SECONDS) {
        if (seconds < 10) {
            pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        ":0%d",
                        seconds);
        } else {
            pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        ":%d",
                        seconds);
        }
    }

    if (!(options & PRINT_24_HOUR_TIME)) {
        if (hour > 12) {
            if ((pDestPtr + 3) < pEndDestPtr) {
               *(pDestPtr++) = ' ';
               *(pDestPtr++) = 'P';
               *(pDestPtr++) = 'M';
            }
        } else {
            if ((pDestPtr + 3) < pEndDestPtr) {
               *(pDestPtr++) = ' ';
               *(pDestPtr++) = 'A';
               *(pDestPtr++) = 'M';
            }
        }
    }

abort:
    *ppResultPtr = pDestPtr;
} // PrintToStringInStandardFormat.




/////////////////////////////////////////////////////////////////////////////
//
// [PrintToStringInW3CFormat]
//
// Print the date and time.
// From the W3C log file format doc:
// (currently at http://www.w3.org/TR/WD-DEBUG_LOGile)
//
//   "Dates are recorded in the format YYYY-MM-DD where YYYY,
//    MM and DD stand for the numeric year, month and day respectively.
//    All dates are specified in GMT. This format is chosen to assist
//    collation using sort."
//
// and
//
//   "Times are recorded in the form HH:MM, HH:MM:SS or HH:MM:SS.S
//    where HH is the hour in 24 hour format, MM is minutes and SS
//    is seconds. All times are specified in GMT."
/////////////////////////////////////////////////////////////////////////////
void
CDateTime::PrintToStringInW3CFormat(
                int32 month,
                int32 hour,
                int32 minutes,
                int32 seconds,
                int32 date,
                int32 year,
                char *pDestPtr,
                char *pEndDestPtr,
                char **ppResultPtr) {
    pDestPtr += snprintf(
                pDestPtr,
                pEndDestPtr - pDestPtr,
                "%d",
                year);

    /////////////////////////////////////
    // Be careful; month ranges between 0 and 11.
    if ((month) < 10) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        ":0%d",
                        month);
    } else {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        ":%d",
                        month);
    }

    /////////////////////////////////////
    if (date < 10) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        ":0%d",
                        date);
    } else {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        ":%d",
                        date);
    }


    /////////////////////////////////////
    if (hour < 10) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        " 0%d",
                        hour);
    } else {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        " %d",
                        hour);
    }

    /////////////////////////////////////
    if (minutes < 10) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        ":0%d",
                        minutes);
    } else {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        ":%d",
                        minutes);
    }


    /////////////////////////////////////
    if (seconds < 10) {
        pDestPtr += snprintf(
                    pDestPtr,
                    pEndDestPtr - pDestPtr,
                    ":0%d",
                    seconds);
    } else {
        pDestPtr += snprintf(
                    pDestPtr,
                    pEndDestPtr - pDestPtr,
                    ":%d",
                    seconds);
    }

    *ppResultPtr = pDestPtr;
} // PrintToStringInW3CFormat.






/////////////////////////////////////////////////////////////////////////////
//
// [PrintToStringInFileNameFormat]
//
/////////////////////////////////////////////////////////////////////////////
void
CDateTime::PrintToStringInFileNameFormat(
                int32 month,
                int32 date,
                int32 year,
                char *pDestPtr,
                char *pEndDestPtr,
                char **ppResultPtr) {
    /////////////////////////////////////
    // Be careful; month ranges between 0 and 11.
    if (month < 10) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        "_0%d",
                        month);
    } else {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        "_%d",
                        month);
    }

    /////////////////////////////////////
    if (date < 10) {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        "_0%d",
                        date);
    } else {
        pDestPtr += snprintf(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        "_%d",
                        date);
    }

    pDestPtr += snprintf(
                pDestPtr,
                pEndDestPtr - pDestPtr,
                "_%d",
                year);

    *ppResultPtr = pDestPtr;
} // PrintToStringInFileNameFormat.






/////////////////////////////////////////////////////////////////////////////
//
// [GetTimeSinceBootInMs]
//
/////////////////////////////////////////////////////////////////////////////
uint64
GetTimeSinceBootInMs() {
#if LINUX
    long msecs;
    time_t secs;
    struct timespec spec;
    uint64 currentTimeInMs;

    clock_gettime(CLOCK_REALTIME, &spec);

    secs  = spec.tv_sec;
    // Divide the nanoseconds by 10^6 to get millisecs.
    msecs = round(spec.tv_nsec / 1.0e6);
    if (msecs > 999) {
        secs += 1;
        msecs = 0;
    }

    currentTimeInMs = (secs * 1000) + msecs;
    return(currentTimeInMs);
#elif WIN32
    return(GetTickCount());
#else
    return(0);
#endif
} // GetTimeSinceBootInMs






/////////////////////////////////////////////////////////////////////////////
//
// [CSimpleFile]
//
/////////////////////////////////////////////////////////////////////////////
CSimpleFile::CSimpleFile() {
    m_FileHandle = NULL_FILE_HANDLE;
}




/////////////////////////////////////////////////////////////////////////////
//
// [~CSimpleFile]
//
/////////////////////////////////////////////////////////////////////////////
CSimpleFile::~CSimpleFile() {
    Close();
}




/////////////////////////////////////////////////////////////////////////////
//
// [OpenExistingFile]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleFile::OpenExistingFile(
                    const char *pFileName,
                    int32 flags) {
    ErrVal err = ENoErr;

    if (NULL == pFileName) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }
    Close();

#if WIN32
    DWORD dwCreationDisposition;
    int32 dwOpenOptions = 0;
    int32 accessRights = 0;
    int32 shareOptions = 0;
    CTempUTF16String unicodeStr;

    dwCreationDisposition = OPEN_EXISTING;

    accessRights |= GENERIC_READ;
    if (!(flags & READ_ONLY)) {
        accessRights |= GENERIC_WRITE;
    }

    shareOptions = FILE_SHARE_READ | FILE_SHARE_DELETE;
    if (flags & SHARE_WRITE) {
        shareOptions |= FILE_SHARE_WRITE;
    }
    if (flags & EXCLUSIVE_ACCESS) {
        shareOptions = 0;
    }
    dwOpenOptions = 0; // FILE_FLAG_RANDOM_ACCESS;

    // Convert from UTF-8 to UTF16 here.
    err = unicodeStr.ConvertUTF8String(pFileName, -1);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }

    m_FileHandle = CreateFileW(
                    unicodeStr.m_pWideStr,
                    accessRights, // access (read-write) mode
                    shareOptions, // share mode
                    NULL, // pointer to security attributes
                    dwCreationDisposition,  // how to create
                    dwOpenOptions,  // file attributes
                    NULL);  // handle to file with attributes to copy

    if (NULL_FILE_HANDLE == m_FileHandle) {
        int reason = GetLastError();
        reason = reason;

        // This happens when we try to open a test data file that was
        // accidentally checked in.
        if (ERROR_ACCESS_DENIED == reason) {
            err = EAccessDenied;
        }
        else if (ERROR_SHARING_VIOLATION == reason) {
            err = EFileIsBusy;
        }
        else if (flags & EXPECT_TO_FIND_FILE) {
            REPORT_LOW_LEVEL_BUG();
            err = ERequiredFileNotFound;
        }
        else {
            err = EFileNotFound;
        }
    }

#elif LINUX
    int openFlags = O_SYNC; // | O_BINARY;
    if (flags & READ_ONLY) {
        openFlags |= O_RDONLY;
    } else {
        openFlags |= O_RDWR;
    } 

    m_FileHandle = open(pFileName, openFlags, 00700);
    if (m_FileHandle < 0) {
        if (flags & EXPECT_TO_FIND_FILE) {
            REPORT_LOW_LEVEL_BUG();
            err = ERequiredFileNotFound;
        }
        else {
            err = EFileNotFound;
        }
    }

#endif

    return(err);
} // OpenExistingFile





/////////////////////////////////////////////////////////////////////////////
//
// [OpenOrCreateFile]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleFile::OpenOrCreateFile(const char *pFileName, int32 flags) {
    ErrVal err = ENoErr;

    if (NULL == pFileName) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }
    Close();

#if WIN32
    DWORD dwCreationDisposition;
    int32 dwOpenOptions = 0;
    int32 accessRights = 0;
    int32 shareOptions = 0;
    CTempUTF16String unicodeStr;

    dwCreationDisposition = OPEN_ALWAYS;
    accessRights |= GENERIC_READ;
    if (!(flags & READ_ONLY)) {
        accessRights |= GENERIC_WRITE;
    }

    shareOptions = FILE_SHARE_READ | FILE_SHARE_DELETE;
    if (flags & SHARE_WRITE) {
        shareOptions |= FILE_SHARE_WRITE;
    }
    if (flags & EXCLUSIVE_ACCESS) {
        shareOptions = 0;
    }
    dwOpenOptions = 0;

    // Convert from UTF-8 to UTF16 here.
    err = unicodeStr.ConvertUTF8String(pFileName, -1);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }

    m_FileHandle = CreateFileW(
                        unicodeStr.m_pWideStr,
                        accessRights, // access (read-write) mode
                        shareOptions, // share mode
                        NULL, // pointer to security attributes
                        dwCreationDisposition,  // how to create
                        dwOpenOptions,  // file attributes
                        NULL);  // handle to file with attributes to copy

    if (NULL_FILE_HANDLE == m_FileHandle) {
        int32 reason = GetLastError();

        // This happens when we try to open a test data file that was
        // accidentally checked in.
        if (ERROR_ACCESS_DENIED == reason) {
            REPORT_LOW_LEVEL_BUG();
        }
        else if (ERROR_SHARING_VIOLATION == reason) {
            err = EFileIsBusy;
        }
        else if (flags & EXPECT_TO_FIND_FILE) {
            REPORT_LOW_LEVEL_BUG();
            err = ERequiredFileNotFound;
        }
        else {
            err = EFileNotFound;
        }
    }
#elif LINUX
    int openFlags = O_SYNC | O_CREAT; // | O_BINARY;
    if (flags & READ_ONLY) {
        openFlags |= O_RDONLY;
    } else {
        openFlags |= O_RDWR;
    } 

    m_FileHandle = open(pFileName, openFlags, 00700);
    if (m_FileHandle < 0) {
        //OSIndependantLayer::PrintToConsole("OpenOrCreateFile. err=%d, file=%s",
        //errno, pFileName);

        if (flags & EXPECT_TO_FIND_FILE) {
            REPORT_LOW_LEVEL_BUG();
            err = ERequiredFileNotFound;
        }
        else {
            err = EFileNotFound;
        }
    }
#endif

    return(err);
} // OpenOrCreateFile






/////////////////////////////////////////////////////////////////////////////
//
// [OpenOrCreateEmptyFile]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleFile::OpenOrCreateEmptyFile(const char *pFileName, int32 flags) {
    ErrVal err = ENoErr;

    err = OpenOrCreateFile(pFileName, flags);
    if (err) {
        return(err);
    }

    err = SetFileLength(0);
    return(err);
} // OpenOrCreateEmptyFile





/////////////////////////////////////////////////////////////////////////////
//
// [Close]
//
/////////////////////////////////////////////////////////////////////////////
void
CSimpleFile::Close() {
    if (NULL_FILE_HANDLE != m_FileHandle) {
#if WIN32
        BOOL fSuccess = CloseHandle(m_FileHandle);
        if (!fSuccess) {
            int32 reason = GetLastError();
            // Force clever compilers to keep the variable arouns so we can
            // inspect it in a debugger.
            fSuccess = fSuccess;
            REPORT_LOW_LEVEL_BUG();
        }
#elif LINUX
        close(m_FileHandle);
#endif
        m_FileHandle = NULL_FILE_HANDLE;
    } // if (NULL_FILE_HANDLE != m_FileHandle)
} // Close





/////////////////////////////////////////////////////////////////////////////
//
// [IsOpen]
//
/////////////////////////////////////////////////////////////////////////////
bool
CSimpleFile::IsOpen() {
    return(NULL_FILE_HANDLE != m_FileHandle);
} // IsOpen






/////////////////////////////////////////////////////////////////////////////
//
// [Read]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleFile::Read(
               void *pBuffer,
               int32 numBytesToRead,
               int32 *pNumBytesRead) {
    ErrVal err = ENoErr;

    if ((NULL == pBuffer)
        || (NULL == pNumBytesRead)) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }

    if (NULL_FILE_HANDLE == m_FileHandle) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }

#if WIN32
    BOOL fSuccess = ReadFile(
                        m_FileHandle,
                        pBuffer,
                        numBytesToRead,
                        (unsigned long *) pNumBytesRead,
                        NULL);
    if (!fSuccess) {
        if (ERROR_HANDLE_EOF == GetLastError()) {
            err = EEOF;
        }
        else {
            REPORT_LOW_LEVEL_BUG();
            err = EFail;
        }
    }

    if ((numBytesToRead > 0) && (0 == *pNumBytesRead)) {
        DWORD dwErr = GetLastError();
        err = EEOF;
        dwErr = dwErr;
    }
#elif LINUX
    int bytesRead = read(m_FileHandle, pBuffer, numBytesToRead);
    if (bytesRead < 0) {
        *pNumBytesRead = 0;
        err = EFail;
    } else
    {
        *pNumBytesRead = bytesRead;
    }
#endif

    return(err);
} // Read







/////////////////////////////////////////////////////////////////////////////
//
// [Write]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleFile::Write(const void *pBuffer, int32 numBytesToWrite) {
    ErrVal err = ENoErr;

    if ((NULL == pBuffer)
            || (numBytesToWrite < 0)) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }

    if (NULL_FILE_HANDLE == m_FileHandle) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }

#if WIN32
    BOOL fSuccess;
    DWORD dwNumBytesWritten;

    fSuccess = WriteFile(
                    m_FileHandle,
                    pBuffer,
                    numBytesToWrite,
                    &dwNumBytesWritten,
                    NULL);
    if ((!fSuccess)
         || (dwNumBytesWritten != (DWORD) numBytesToWrite)) {
        DWORD dwErr = GetLastError();
        if (ERROR_DISK_FULL == dwErr) {
            err = ENoDiskSpace;
        }
        else {
            REPORT_LOW_LEVEL_BUG();
            err = EFail;
        }
    }
#elif LINUX
    int bytesWritten = write(m_FileHandle, pBuffer, numBytesToWrite);
    if (bytesWritten < 0) {
        err = EFail;
    }
#endif

    return(err);
} // Write






/////////////////////////////////////////////////////////////////////////////
//
// [Seek]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleFile::Seek(uint64 offset) {
    return(Seek(offset, SEEK_START));
} // Seek





/////////////////////////////////////////////////////////////////////////////
//
// [Seek]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleFile::Seek(uint64 offset, int32 whence) {
    ErrVal err = ENoErr;

    if (NULL_FILE_HANDLE == m_FileHandle) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }

#if WIN32
    DWORD dwResult;
    uint64 shiftedOffset;
    DWORD lowerOffset;
    LONG upperOffset;

    lowerOffset = (DWORD) (offset & 0xFFFFFFFF);
    shiftedOffset = offset >> 32;
    upperOffset = (LONG) (shiftedOffset & 0xFFFFFFFF);

    dwResult = SetFilePointer(
                    m_FileHandle,
                    lowerOffset,
                    &upperOffset,
                    whence);
    if (0xFFFFFFFF == dwResult) {
        DWORD dwErr = GetLastError();
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
    }
#elif LINUX
    off_t seekResult;

    seekResult = lseek(m_FileHandle, offset, whence);
    if (seekResult < 0) {
        err = EFail;
    }
#endif

    return(err);
} // Seek





/////////////////////////////////////////////////////////////////////////////
//
// [GetFilePosition]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleFile::GetFilePosition(uint64 *resultPos) {
    ErrVal err = ENoErr;

    if (NULL == resultPos) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }

    if (NULL_FILE_HANDLE == m_FileHandle) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }

#if WIN32
    DWORD lowerOffset;
    DWORD upperOffset = 0;

    lowerOffset = SetFilePointer(
                    m_FileHandle, // must have GENERIC_READ and/or GENERIC_WRITE
                    0,     // do not move pointer
                    (LONG *) &upperOffset,  // hFile is not large enough to need this pointer
                    FILE_CURRENT);  // provides offset from current position

    *resultPos = upperOffset;
    *resultPos = *resultPos << 32;
    *resultPos |= lowerOffset;
#elif LINUX
    off_t seekResult;

    seekResult = lseek(m_FileHandle, 0, SEEK_CUR);
    if (seekResult < 0) {
        return(EFail);
    }

    *resultPos = seekResult;
#endif

    return(err);
} // GetFilePosition








/////////////////////////////////////////////////////////////////////////////
//
// [GetFileLength]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleFile::GetFileLength(uint64 *pdwLength) {
    ErrVal err = ENoErr;

    if (NULL_FILE_HANDLE == m_FileHandle) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }

    if (NULL == pdwLength) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }

#if WIN32
    DWORD upperLength = 0;
    DWORD lowerLength;

    lowerLength = GetFileSize(m_FileHandle, &upperLength);
    if (0xFFFFFFFF == lowerLength) {
        DWORD dwErr = GetLastError();
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
    }

    *pdwLength = upperLength;
    *pdwLength = *pdwLength << 32;
    *pdwLength |= lowerLength;
#elif LINUX
   struct stat statInfo;
   int statResult;

    statResult = fstat(m_FileHandle, &statInfo);
    if (statResult < 0) {
        return(EFail);
    }

    *pdwLength = statInfo.st_size;
#endif

    return(err);
} // GetFileLength







/////////////////////////////////////////////////////////////////////////////
//
// [SetFileLength]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleFile::SetFileLength(uint64 newLength) {
    ErrVal err = ENoErr;

    if (NULL_FILE_HANDLE == m_FileHandle) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }

#if WIN32
    BOOL fSuccess;
    DWORD dwResult;
    uint64 shiftedLength;
    DWORD lowerLength;
    DWORD upperLength;

    lowerLength = (DWORD) (newLength & 0xFFFFFFFF);
    shiftedLength = newLength >> 32;
    upperLength = (DWORD) (shiftedLength & 0xFFFFFFFF);

    dwResult = SetFilePointer(
                    m_FileHandle,
                    lowerLength,
                    (LONG *) &upperLength,
                    FILE_BEGIN);
    if (0xFFFFFFFF == dwResult) {
        DWORD dwErr = GetLastError();
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
        goto abort;
    }

    fSuccess = SetEndOfFile(m_FileHandle);
    if (!fSuccess) {
        DWORD dwErr = GetLastError();
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
        goto abort;
    }

#elif LINUX
    // Use truncate if you want to pass in a pathname instead.
    int dwResult = ftruncate(m_FileHandle, newLength);
    if (dwResult < 0) {
        err = EFail;
        goto abort;
    }
#endif

abort:
    return(err);
} // SetFileLength






/////////////////////////////////////////////////////////////////////////////
//
// [Flush]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleFile::Flush() {
    ErrVal err = ENoErr;

    if (NULL_FILE_HANDLE == m_FileHandle) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }

#if WIN32
    BOOL fSuccess = FlushFileBuffers(m_FileHandle);
    if (!fSuccess) {
        DWORD dwErr = GetLastError();
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
    }
#elif LINUX
    int result = fsync(m_FileHandle);
    if (result < 0) {
        err = EFail;
    }
#endif

    return(err);
} // Flush





/////////////////////////////////////////////////////////////////////////////
//
// [DeleteFile]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleFile::DeleteFile(const char *pFileName) {
    ErrVal err = ENoErr;

    if (NULL == pFileName) {
       goto abort;
    }

#if WIN32
    {
        BOOL fSuccess;
        CTempUTF16String unicodeStr;

        // Convert from UTF-8 to UTF16 here.
        err = unicodeStr.ConvertUTF8String(pFileName, -1);
        if (err) {
            REPORT_LOW_LEVEL_BUG();
            err = EFail;
        }

        fSuccess = ::DeleteFileW(unicodeStr.m_pWideStr);
        if (!fSuccess) {
            DWORD dwErr = GetLastError();
            if (ERROR_FILE_NOT_FOUND != dwErr) {
                REPORT_LOW_LEVEL_BUG();
                err = EFail;
            }
        }
    }
#elif LINUX
    unlink(pFileName);
#endif

abort:
    return(err);
} // DeleteFile






/////////////////////////////////////////////////////////////////////////////
//
// [FileExists]
//
/////////////////////////////////////////////////////////////////////////////
bool
CSimpleFile::FileExists(const char *pFileName) {
#if WIN32
    ErrVal err = ENoErr;
    BOOL fSuccess = FALSE;
    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    CTempUTF16String unicodeStr;

    // Convert from UTF-8 to UTF16 here.
    err = unicodeStr.ConvertUTF8String(pFileName, -1);
    if (err) {
        return(false);
    }

    fSuccess = GetFileAttributesExW(
                    unicodeStr.m_pWideStr,
                    GetFileExInfoStandard,
                    &fileInfo);
   if ((fSuccess)
       && !(fileInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
       return(true);
   }
#elif LINUX
   int result;

   result = access(pFileName, F_OK);
   // access returns success if a file or directory exists.
   if (0 == result) {
      struct stat statInfo;

      result = stat(pFileName, &statInfo);
      if (result < 0) {
         return(false);
      }
      if (S_IFDIR == (statInfo.st_mode & S_IFMT)) {
         return(false);
      }

      return(true);
   } // if (0 == result)
#endif

   return(false);
} // FileExists.






/////////////////////////////////////////////////////////////////////////////
//
// [FileOrDirectoryExists]
//
/////////////////////////////////////////////////////////////////////////////
bool
CSimpleFile::FileOrDirectoryExists(const char *pFileName) {
#if WIN32
    ErrVal err = ENoErr;
    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    BOOL fSuccess = FALSE;
    CTempUTF16String unicodeStr;

    // Convert from UTF-8 to UTF16 here.
    err = unicodeStr.ConvertUTF8String(pFileName, -1);
    if (err) {
        return(false);
    }

    fSuccess = GetFileAttributesExW(
                        unicodeStr.m_pWideStr,
                        GetFileExInfoStandard,
                        &fileInfo);
    if (fSuccess) {
        return(true);
    }
#elif LINUX
   int result;
   struct stat statInfo;

   result = stat(pFileName, &statInfo);
   if (result < 0) {
       return(false);
   } else {
       return(true);
   }
#endif

    return(false);
} // FileOrDirectoryExists.







/////////////////////////////////////////////////////////////////////////////
//
// [DirectoryExists]
//
/////////////////////////////////////////////////////////////////////////////
bool
CSimpleFile::DirectoryExists(const char *pDirName) {
    if (NULL == pDirName) {
        REPORT_LOW_LEVEL_BUG();
        return(false);
    }

#if WIN32
    ErrVal err = ENoErr;
    BOOL fSuccess = FALSE;
    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    CTempUTF16String unicodeStr;

    // Convert from UTF-8 to UTF16 here.
    err = unicodeStr.ConvertUTF8String(pDirName, -1);
    if (err) {
        return(false);
    }

   fSuccess = GetFileAttributesExW(
                    unicodeStr.m_pWideStr,
                    GetFileExInfoStandard,
                    &fileInfo);
   if ((fSuccess)
       && (fileInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
       return(true);
   }
#elif LINUX
   int result;
   struct stat statInfo;

   result = stat(pDirName, &statInfo);
   if (result < 0) {
       return(false);
   }
   if (S_IFDIR == (statInfo.st_mode & S_IFMT)) {
       return(true);
   }
#endif

   return(false);
} // DirectoryExists.





/////////////////////////////////////////////////////////////////////////////
//
// [IsDirectory]
//
/////////////////////////////////////////////////////////////////////////////
bool
CSimpleFile::IsDirectory(const char *pDirName) {
    if (NULL == pDirName) {
        REPORT_LOW_LEVEL_BUG();
        return(false);
    }

#if WIN32
    ErrVal err = ENoErr;
    BOOL fSuccess = FALSE;
    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    CTempUTF16String unicodeStr;

    // Convert from UTF-8 to UTF16 here.
    err = unicodeStr.ConvertUTF8String(pDirName, -1);
    if (err) {
        return(false);
    }

   fSuccess = GetFileAttributesExW(
                    unicodeStr.m_pWideStr,
                    GetFileExInfoStandard,
                    &fileInfo);
   if ((fSuccess)
       && (fileInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
       return(true);
   }
#elif LINUX
   int result;
   struct stat statInfo;

   result = stat(pDirName, &statInfo);
   if (result < 0) {
       return(false);
   }
   if (S_IFDIR == (statInfo.st_mode & S_IFMT)) {
       return(true);
   }
#endif

   return(false);
} // IsDirectory






/////////////////////////////////////////////////////////////////////////////
//
// [CreateDirectory]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleFile::CreateDirectory(const char *pDirName) {
    ErrVal err = ENoErr;

    if (NULL == pDirName) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }

#if WIN32
    BOOL fSuccess;
    CTempUTF16String unicodeStr;

    // Convert from UTF-8 to UTF16 here.
    err = unicodeStr.ConvertUTF8String(pDirName, -1);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
    }

    fSuccess = ::CreateDirectoryW(unicodeStr.m_pWideStr, NULL);
    if (!fSuccess) {
        DWORD dwReason = GetLastError();
        if (ERROR_ALREADY_EXISTS != dwReason) {
            REPORT_LOW_LEVEL_BUG();
            err = EFail;
        }
    }
#elif LINUX
   int result;

   result = mkdir(
               pDirName,
               S_IRWXU | S_IRWXG | S_IRWXO);
   //<>printf("CreateDirectory. pDirName=%s, Resuly=%d, errno=%d\n", pDirName, result, errno);

   if (result < 0) {
       // If the directory already exists, then this is not an error.
       if (EEXIST == errno) {
           err = ENoErr;
       } else if (EACCES == errno) {
           err = EAccessDenied;
       } else {
           REPORT_LOW_LEVEL_BUG();
           err = EFail;
       }
   }
#endif

    return(err);
} // // CreateDirectory.






/////////////////////////////////////////////////////////////////////////////
//
// [DeleteDirectory]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleFile::DeleteDirectory(const char *pDirName) {
    ErrVal err = ENoErr;

    if (NULL == pDirName) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }

    err = CSimpleFile::EmptyDirectory(pDirName);
    if (err) {
        goto abort;
    }

#if WIN32
    {
        BOOL fSuccess;
        CTempUTF16String unicodeStr;

        // Convert from UTF-8 to UTF16 here.
        err = unicodeStr.ConvertUTF8String(pDirName, -1);
        if (err) {
            REPORT_LOW_LEVEL_BUG();
            err = EFail;
        }

        fSuccess = ::RemoveDirectoryW(unicodeStr.m_pWideStr);
        if (!fSuccess) {
            DWORD dwReason = GetLastError();
            REPORT_LOW_LEVEL_BUG();
            err = EFail;
        }
    }
#elif LINUX
   int result;

   result = rmdir(pDirName);
   if (result < 0) {
       REPORT_LOW_LEVEL_BUG();
       err = EFail;
   }
#endif

abort:
    return(err);
} // DeleteDirectory.





/////////////////////////////////////////////////////////////////////////////
//
// [EmptyDirectory]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleFile::EmptyDirectory(const char *pDirName) {
    ErrVal err = ENoErr;
    CDirFileList fileIter;
    char fileName[512];

    if (NULL == pDirName) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }

    err = fileIter.Open(pDirName);
    if (err) {
        goto abort;
    }

    while (fileIter.GetNextFile(fileName, sizeof(fileName))) {
        if (IsDirectory(fileName)) {
            err = CSimpleFile::EmptyDirectory(fileName);
            if (err) {
                goto abort;
            }
            err = CSimpleFile::DeleteDirectory(fileName);
            if (err) {
                goto abort;
            }
        }
        else {
            err = CSimpleFile::DeleteFile(fileName);
            if (err) {
                goto abort;
            }
        }
    }

abort:
    return(err);
} // EmptyDirectory.






/////////////////////////////////////////////////////////////////////////////
//
// [MoveFile]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CSimpleFile::MoveFile(const char *pSrcPathName, const char *pDestPathName) {
    ErrVal err = ENoErr;

    if ((NULL == pSrcPathName)
        || (NULL == pDestPathName)) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }

#if WIN32
    BOOL fSuccess;
    CTempUTF16String srcUnicodeStr;
    CTempUTF16String destUnicodeStr;

    // Convert from UTF-8 to UTF16 here.
    err = srcUnicodeStr.ConvertUTF8String(pSrcPathName, -1);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
    }
    err = destUnicodeStr.ConvertUTF8String(pDestPathName, -1);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
    }

    fSuccess = ::MoveFileW(
                        srcUnicodeStr.m_pWideStr,
                        destUnicodeStr.m_pWideStr);
    if (!fSuccess) {
        DWORD dwReason = GetLastError();
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
    }
#elif LINUX
    int result;

    // Create a new hard link.
    result = rename(pSrcPathName, pDestPathName);
    if (result < 0) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
    }
#endif

    return(err);
} // MoveFile.





/////////////////////////////////////////////////////////////////////////////
//
// [GetFileNameFromPath]
//
/////////////////////////////////////////////////////////////////////////////
char *
CSimpleFile::GetFileNameFromPath(const char *pPathName) {
    const char *pSuffix;

    if (NULL == pPathName) {
        return(NULL);
    }
    if (!(*pPathName)) {
        return((char *) pPathName);
    }

    pSuffix = pPathName + strlen(pPathName) - 1;
    while ((pSuffix > pPathName)
        && !(IS_DIRECTORY_SEPARATOR(*pSuffix))) {
        pSuffix--;
    }

    if (IS_DIRECTORY_SEPARATOR(*pSuffix)) {
        return((char *) (pSuffix + 1));
    }

    return((char *) pPathName);
} // GetFileNameFromPath







/////////////////////////////////////////////////////////////////////////////
//
// [CDirFileList]
//
/////////////////////////////////////////////////////////////////////////////
CDirFileList::CDirFileList() {
    m_FileIndex = 0;

#if WIN32
    m_SearchHandle = NULL_FILE_HANDLE;

    memset(&m_fileInfo, 0, sizeof(m_fileInfo));

    m_CurrentPathLen = 0;
    m_CurrentPath[0] = 0;
#endif

#if LINUX
    m_pDir = NULL;
#endif
} // CDirFileList






/////////////////////////////////////////////////////////////////////////////
//
// [~CDirFileList]
//
/////////////////////////////////////////////////////////////////////////////
CDirFileList::~CDirFileList() {
    Close();
} // ~CDirFileList






/////////////////////////////////////////////////////////////////////////////
//
// [Open]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CDirFileList::Open(const char *pPath) {
    ErrVal err = ENoErr;

    if (NULL == pPath) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }
    m_FileIndex = 0;

    m_CurrentPathLen = (int32) strlen(pPath);
    if ((m_CurrentPathLen + 2) >= MAX_DIR_PATH) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }
    CopyUTF8String(m_CurrentPath, pPath, MAX_DIR_PATH - 1);

    if (!(IS_DIRECTORY_SEPARATOR(m_CurrentPath[m_CurrentPathLen - 1]))) {
        m_CurrentPath[m_CurrentPathLen] = DIRECTORY_SEPARATOR_CHAR;
        m_CurrentPath[m_CurrentPathLen + 1] = 0;
        m_CurrentPathLen += 1;
    }

#if WIN32
    CTempUTF16String unicodeStr;

    // FindFirstFile wants the pathname to end with "\*"
    m_CurrentPath[m_CurrentPathLen] = '*';
    m_CurrentPath[m_CurrentPathLen + 1] = 0;
    m_CurrentPathLen += 1;

    // Convert from UTF-8 to UTF16 here.
    err = unicodeStr.ConvertUTF8String(m_CurrentPath, -1);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
    }

    // Find the first file in the directory.
    m_SearchHandle = FindFirstFileW(unicodeStr.m_pWideStr, &m_fileInfo);
#elif LINUX
    m_pDir = opendir(pPath);
    if (NULL == m_pDir) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }
#endif

    return(err);
} // Open





/////////////////////////////////////////////////////////////////////////////
//
// [Close]
//
/////////////////////////////////////////////////////////////////////////////
void
CDirFileList::Close() {
#if WIN32
    if (NULL_FILE_HANDLE != m_SearchHandle) {
        (void) FindClose(m_SearchHandle);
        m_SearchHandle = NULL_FILE_HANDLE;
    }
#elif LINUX
    if (m_pDir) {
       closedir(m_pDir);
       m_pDir = NULL;
    }
#endif
} // Close





/////////////////////////////////////////////////////////////////////////////
//
// [GetNextFile]
//
/////////////////////////////////////////////////////////////////////////////
bool
CDirFileList::GetNextFile(char *pFileName, int32 maxPath) {
    char *pEndDestPtr;

#if WIN32
    {
    ErrVal err = ENoErr;
    BOOL foundNext = 0;
    CTempUTF8String utf8Str;

    // Keep looping until we find a file we want.
    while (1) {
        if (0 == m_FileIndex) {
            foundNext = (NULL_FILE_HANDLE != m_SearchHandle);
        }
        else {
            foundNext = FindNextFileW(m_SearchHandle, &m_fileInfo);
        }

        if (!foundNext) {
            break;
        }
        m_FileIndex += 1;


        // Skip the . and .. entries.
        if ((m_fileInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
           && (L'.' == m_fileInfo.cFileName[0])) {
            if ((0 == m_fileInfo.cFileName[1])
                  || ((L'.' == m_fileInfo.cFileName[1])
                     && (0 == m_fileInfo.cFileName[2]))) {
                continue;
            }
        }

        err = utf8Str.ConvertUTF16String(m_fileInfo.cFileName, -1);
        if (err) {
            REPORT_LOW_LEVEL_BUG();
            return(false);
        }

        pEndDestPtr = pFileName + maxPath;
        if (maxPath <= (int32) (m_CurrentPathLen + (int32) strlen(utf8Str.m_pStr) + 5)) {
            break;
        }

        CopyUTF8String(pFileName, m_CurrentPath, (int32) (pEndDestPtr - pFileName));
        pFileName += (m_CurrentPathLen - 1); // Back up over the '*', but leave the \.
        CopyUTF8String(pFileName, utf8Str.m_pStr, (int32) (pEndDestPtr - pFileName));

        return(true);
    } // while (1)
    } // WIN32
#endif // WIN32

#if LINUX
    while (1) {
        struct dirent *pDirEntry;

        pDirEntry = readdir(m_pDir);
        if (NULL == pDirEntry) {
            break;
        }
        m_FileIndex += 1;

        // Skip the . and .. entries.
        if ('.' == pDirEntry->d_name[0]) {
            if ((0 == pDirEntry->d_name[1])
                  || (('.' == pDirEntry->d_name[1])
                     && (0 == pDirEntry->d_name[2]))) {
                continue;
            }
        }

        pEndDestPtr = pFileName + maxPath;
        if (maxPath <= (int32) (m_CurrentPathLen + (int32) strlen(pDirEntry->d_name) + 5)) {
            break;
        }

        CopyUTF8String(pFileName, m_CurrentPath, (int32) (pEndDestPtr - pFileName));
        pFileName += (m_CurrentPathLen);
        CopyUTF8String(pFileName, pDirEntry->d_name, (int32) (pEndDestPtr - pFileName));

        return(true);
   } // while (1)
#endif // LINUX

    return(false);
} // GetNextFile.






/////////////////////////////////////////////////////////////////////////////
//
// [GetCurrentThreadId]
//
/////////////////////////////////////////////////////////////////////////////
int32
OSIndependantLayer::GetCurrentThreadId() {
#if WIN32
    return(::GetCurrentThreadId());
#elif LINUX
    //pthread_t self;
    //self = pthread_self();
    return(getpid());
#endif
} // GetCurrentThreadId






/////////////////////////////////////////////////////////////////////////////
//
// [BreakToDebugger]
//
/////////////////////////////////////////////////////////////////////////////
void
OSIndependantLayer::BreakToDebugger() {
#if DD_DEBUG
    if (!g_AllowBreakToDebugger) {
        return;
    }

#if WIN32
    DebugBreak();
#elif LINUX
    raise(SIGINT);
    printf("\nBreakToDebugger causes program exit.\n");
    exit(-1);
#endif

#endif // DD_DEBUG
} // BreakToDebugger





/////////////////////////////////////////////////////////////////////////////
//
// [SetRandSeed]
//
/////////////////////////////////////////////////////////////////////////////
void
OSIndependantLayer::SetRandSeed(int32 seedVal) {
    srand(seedVal);
} // SetRandSeed.





/////////////////////////////////////////////////////////////////////////////
//
// [GetRandomNum]
//
/////////////////////////////////////////////////////////////////////////////
int32
OSIndependantLayer::GetRandomNum() {
    int32 result;
    int32 temp;

    result = rand();
    if (result < 0) {
        result = -result;
    }
    result = result << 14;

    temp = rand();
    if (temp < 0) {
        temp = -temp;
    }

    result = result + temp;
    if (result < 0) {
        result = -result;
    }

    return(result);
} // GetRandomNum.





/////////////////////////////////////////////////////////////////////////////
//
// [SleepForMilliSecs]
//
/////////////////////////////////////////////////////////////////////////////
void
OSIndependantLayer::SleepForMilliSecs(int32 numMilliSecs) {
#if WIN32
   Sleep(numMilliSecs);
#elif LINUX
   // usleep takes microseconds.
   usleep(numMilliSecs * 1000);
#endif
} // SleepForMilliSecs







#if WIN32
////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
void
OSIndependantLayer::GetOSVersionString(char *pVersionString, int maxStrSize) {
   OSVERSIONINFOEXA osVersionInfo;
   SYSTEM_INFO systemInfo;
   GetNativeSystemInfoProcType pGetNativeSystemInfoProc;
   GetProductInfoProcType pGetProductInfoProc;
   BOOL success;
   DWORD dwType = 0;
   char *pProductName = NULL;
   char *pEditionName = "";
   char *pServicePack = "";
   char *pArchitectureType = "";


   ZeroMemory(&systemInfo, sizeof(systemInfo));
   ZeroMemory(&osVersionInfo, sizeof(osVersionInfo));
   osVersionInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);

   if (NULL == pVersionString) {
      return;
   }
   *pVersionString = 0;


   success = GetVersionEx((OSVERSIONINFO *) &osVersionInfo);
   if (!success) {
      return;
   }

   // Call GetNativeSystemInfo if it is available.
   pGetNativeSystemInfoProc = (GetNativeSystemInfoProcType)
            GetProcAddress(GetModuleHandle(TEXT("kernel32.dll")), "GetNativeSystemInfo");
   if (NULL != pGetNativeSystemInfoProc) {
      pGetNativeSystemInfoProc(&systemInfo);
   } else {
      GetSystemInfo(&systemInfo);
   }

    if ((VER_PLATFORM_WIN32_NT == osVersionInfo.dwPlatformId)
        && (osVersionInfo.dwMajorVersion > 4)) {
        ///////////////////////////////////////////////////////////////////
        // Windows 10 and Windows Server 2016
        if ((6 == osVersionInfo.dwMajorVersion) && (3 == osVersionInfo.dwMinorVersion)) {
            // Product
            if (VER_NT_WORKSTATION == osVersionInfo.wProductType)  {
                pProductName = "Windows 10";
            } else {
                pProductName = "Windows Server 2016";
             }
            // Architecture
            if (PROCESSOR_ARCHITECTURE_AMD64 == systemInfo.wProcessorArchitecture) {
                pArchitectureType = ", 64-bit";
            } else if (PROCESSOR_ARCHITECTURE_INTEL == systemInfo.wProcessorArchitecture) {
                pArchitectureType = ", 32-bit";
            }
        } // if ((6 == osVersionInfo.dwMajorVersion)

        ///////////////////////////////////////////////////////////////////
        // Windows 8.1 and Windows Server 2012 R2
        if ((6 == osVersionInfo.dwMajorVersion) && (3 == osVersionInfo.dwMinorVersion)) {
            // Product
            if (VER_NT_WORKSTATION == osVersionInfo.wProductType)  {
                pProductName = "Windows 8.1";
            } else {
                pProductName = "Windows Server 2012 R2";
             }
            // Architecture
            if (PROCESSOR_ARCHITECTURE_AMD64 == systemInfo.wProcessorArchitecture) {
                pArchitectureType = ", 64-bit";
            } else if (PROCESSOR_ARCHITECTURE_INTEL == systemInfo.wProcessorArchitecture) {
                pArchitectureType = ", 32-bit";
            }
        } // if ((6 == osVersionInfo.dwMajorVersion)

        ///////////////////////////////////////////////////////////////////
        // Windows 8 and Windows Server 2012
        if ((6 == osVersionInfo.dwMajorVersion) && (2 == osVersionInfo.dwMinorVersion)) {
            // Product
            if (VER_NT_WORKSTATION == osVersionInfo.wProductType)  {
                pProductName = "Windows 8";
            } else {
                pProductName = "Windows Server 2012";
             }
            // Architecture
            if (PROCESSOR_ARCHITECTURE_AMD64 == systemInfo.wProcessorArchitecture) {
                pArchitectureType = ", 64-bit";
            } else if (PROCESSOR_ARCHITECTURE_INTEL == systemInfo.wProcessorArchitecture) {
                pArchitectureType = ", 32-bit";
            }
        } // if ((6 == osVersionInfo.dwMajorVersion)
      
        ///////////////////////////////////////////////////////////////////
        // Windows 7 and Windows Server 2008
        if ((6 == osVersionInfo.dwMajorVersion) && (1 == osVersionInfo.dwMinorVersion)) {
            // Product
            if (VER_NT_WORKSTATION == osVersionInfo.wProductType)  {
                pProductName = "Windows 7";
            } else {
                pProductName = "Windows Server 2008 R2";
             }
            // Architecture
            if (PROCESSOR_ARCHITECTURE_AMD64 == systemInfo.wProcessorArchitecture) {
                pArchitectureType = ", 64-bit";
            } else if (PROCESSOR_ARCHITECTURE_INTEL == systemInfo.wProcessorArchitecture) {
                pArchitectureType = ", 32-bit";
            }
        } // if ((6 == osVersionInfo.dwMajorVersion)
      
       ///////////////////////////////////////////////////////////////////
       // Windows Vista and Windows Server 2008
       if ((6 == osVersionInfo.dwMajorVersion)
            && (0 == osVersionInfo.dwMinorVersion)) {
         // Product
         if (VER_NT_WORKSTATION == osVersionInfo.wProductType)  {
             pProductName = "Windows Vista";
         } else {
            pProductName = "Windows Server 2008";
         }

         // Edition
         pGetProductInfoProc
            = (GetProductInfoProcType) GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                      "GetProductInfo");
         if (NULL != pGetProductInfoProc) {
            pGetProductInfoProc(6, 0, 0, 0, &dwType);
            pEditionName = GetStringForValue(dwType, g_NTVersions);
         }

         // Architecture
         if (PROCESSOR_ARCHITECTURE_AMD64 == systemInfo.wProcessorArchitecture) {
            pArchitectureType = ", 64-bit";
         } else if (PROCESSOR_ARCHITECTURE_INTEL == systemInfo.wProcessorArchitecture) {
            pArchitectureType = ", 32-bit";
         }
      } // if ((6 == osVersionInfo.dwMajorVersion)

      ///////////////////////////////////////////////////////////////////
      // Windows Server 2003 and Windows XP
      if ((5 == osVersionInfo.dwMajorVersion)
            && (2 == osVersionInfo.dwMinorVersion)) {
         // Product
         if (GetSystemMetrics(SM_SERVERR2)) {
            pProductName = "Windows Server 2003 R2";
         } else if (GetSystemMetrics(SM_MEDIACENTER)) {
            pProductName = "Windows XP Media Center Edition";
         } else if (GetSystemMetrics(SM_TABLETPC)) {
            pProductName = "Windows XP Tablet PC Edition";
         } else if (GetSystemMetrics(SM_STARTER)) {
            pProductName = "Windows XP Starter Edition";
         }
         else if ((VER_NT_WORKSTATION == osVersionInfo.wProductType)
                  && (PROCESSOR_ARCHITECTURE_AMD64 == systemInfo.wProcessorArchitecture)) {
            pProductName = "Windows XP Professional x64 Edition";
         } else {
            pProductName = "Windows Server 2003";
         }

         // Edition
         if (osVersionInfo.wProductType != VER_NT_WORKSTATION) {
            if (PROCESSOR_ARCHITECTURE_IA64 == systemInfo.wProcessorArchitecture) {
                if (osVersionInfo.wSuiteMask & VER_SUITE_DATACENTER) {
                   pEditionName = "Datacenter Edition for Itanium-based Systems";
                }
                else if (osVersionInfo.wSuiteMask & VER_SUITE_ENTERPRISE) {
                   pEditionName = "Enterprise Edition for Itanium-based Systems";
                }
            } else if (PROCESSOR_ARCHITECTURE_AMD64 == systemInfo.wProcessorArchitecture) {
                if (osVersionInfo.wSuiteMask & VER_SUITE_DATACENTER) {
                   pEditionName = "Datacenter x64 Edition";
                }
                else if (osVersionInfo.wSuiteMask & VER_SUITE_ENTERPRISE) {
                   pEditionName = "Enterprise x64 Edition";
                }
                else {
                   pEditionName = "Standard x64 Edition";
                }
            } else {
                if (osVersionInfo.wSuiteMask & VER_SUITE_COMPUTE_SERVER) {
                   pEditionName = "Compute Cluster Edition";
                }
                else if (osVersionInfo.wSuiteMask & VER_SUITE_DATACENTER) {
                   pEditionName = "Datacenter Edition";
                }
                else if (osVersionInfo.wSuiteMask & VER_SUITE_ENTERPRISE) {
                   pEditionName = "Enterprise Edition";
                }
                else if (osVersionInfo.wSuiteMask & VER_SUITE_BLADE) {
                   pEditionName = "Web Edition";
                }
                else {
                   pEditionName = "Standard Edition";
                }
            }
         }
      } // Windows Server 2003 and Windows XP
      
      ///////////////////////////////////////////////////////////////////
      // Windows XP
      if ((5 == osVersionInfo.dwMajorVersion)
            && (1 == osVersionInfo.dwMinorVersion)) {
         // Product
         pProductName = "Windows XP";
         if (GetSystemMetrics(SM_MEDIACENTER)) {
            pProductName = "Windows XP Media Center Edition";
         }
         else if (GetSystemMetrics(SM_TABLETPC)) {
            pProductName = "Windows XP Tablet PC Edition";
         } else if (GetSystemMetrics(SM_STARTER)) {
            pProductName = "Windows XP Starter Edition";
         }

         // Edition
         if (osVersionInfo.wSuiteMask & VER_SUITE_PERSONAL) {
            pEditionName = "Home Edition";
         } else {
            pEditionName = "Professional";
         }
      } // Windows XP

      ///////////////////////////////////////////////////////////////////
      // Windows 2000
      if ((5 == osVersionInfo.dwMajorVersion)
            && (0 == osVersionInfo.dwMinorVersion)) {
         // Product
         pProductName = "Windows 2000";

         // Edition
         if (VER_NT_WORKSTATION == osVersionInfo.wProductType) {
            pEditionName = "Professional";
         } else {
            if (osVersionInfo.wSuiteMask & VER_SUITE_DATACENTER) {
               pEditionName = "Datacenter Server";
            } else if (osVersionInfo.wSuiteMask & VER_SUITE_ENTERPRISE) {
               pEditionName = "Advanced Server";
            } else {
               pEditionName = "Server";
            }
         }
      } // Windows 2000

      pServicePack = osVersionInfo.szCSDVersion;
   } // if ((VER_PLATFORM_WIN32_NT == osVersionInfo.dwPlatformId)

    // Now, assemble the pieces.
    if (NULL != pProductName) {
        if ((pEditionName) && (*pEditionName)) {
            snprintf(pVersionString, maxStrSize, "%s %s%s %s (build %d)",
                        pProductName,
                        pEditionName,
                        pArchitectureType,
                        pServicePack,
                        osVersionInfo.dwBuildNumber);
        } else {
            snprintf(pVersionString, maxStrSize, "%s%s %s (build %d)",
                        pProductName,
                        pArchitectureType,
                        pServicePack,
                        osVersionInfo.dwBuildNumber);
        }
    } else {
        snprintf(pVersionString, maxStrSize, "Unrecognized Windows. Platform %d Version %d.%d (build %d)",
                        osVersionInfo.dwPlatformId,
                        osVersionInfo.dwMajorVersion,
                        osVersionInfo.dwMinorVersion,
                        osVersionInfo.dwBuildNumber);
    }
} // GetOSVersionString




////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
char *
GetStringForValue(int val, CVersionName *pList) {
   if (NULL == pList) {
      return("");
   }

   while (NULL != pList->pName) {
      if (val == pList->intVal) {
         return(pList->pName);
      }

      pList += 1;
   } // while (NULL != pList->pName)

   return("");
} // GetStringForValue
#endif // WIN32


#if LINUX
////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
void
OSIndependantLayer::GetOSVersionString(char *pVersionString, int maxStrSize) {
   struct utsname nameInfo;
   int result;

   if (NULL == pVersionString) {
      return;
   }
   *pVersionString = 0;


   result = uname(&nameInfo);
   if (0 == result) {
      snprintf(
            pVersionString,
            maxStrSize,
            "%s, Release %s, (Version %s), %s",
            nameInfo.sysname,
            nameInfo.release,
            nameInfo.version,
            nameInfo.machine);
   }
} // GetOSVersionString

#endif // LINUX






/////////////////////////////////////////////////////////////////////////////
//
// [CopyUTF8String]
//
// This avoids several problems of the POSIX strncpy:
//
// 1. It raises a warning if the string is truncated.
//
// 2. In the official strncpy, a null character
// is not appended automatically to the copied string. This always
// NULL-terminates a string.
//
// 3. In the official strncpy, if count is greater than
// the length of strSource, the destination string is padded with
// null characters up to length count.
//
// 4. This is aware of UTF-8, and will always truncate at a character
// boundary. If the last chararacter is a multi-byte character, then
// this will truncate at the start of the last character.
//
/////////////////////////////////////////////////////////////////////////////
void
CopyUTF8String(char *pDestPtr, const char *pSrcPtr, int32 maxLength) {
    const char *pEndDestPtr;

    if ((NULL == pDestPtr)
        || (NULL == pSrcPtr)
        || (maxLength < 0)) {
        REPORT_LOW_LEVEL_BUG();
        return;
    }

    pEndDestPtr = pDestPtr + maxLength;

    // Leave space for the NULL-terminating char.
    // We always NULL-terminate.
    //
    // This is tricky. Sometimes, maxLength is the size
    // of the string, and we assume that the buffer is at least 1
    // character longer.
    //
    // We define the length to be the length of the source string.
    // This allows us to copy a substring from a larger string.

    // Stop at the length or the null-terminator, whichever comes first.
    while ((*pSrcPtr) && (pDestPtr < pEndDestPtr)) {
        *(pDestPtr++) = *(pSrcPtr++);
    }

    // If we stopped before the end of the string, then
    // we truncated the string. This is not a real bug;
    // we often copy a substring from a larger string.

    // We left space for the NULL-terminating char, so
    // we don't have to test whether pDestPtr >= pEndDestPtr;
    *pDestPtr = 0;
} // CopyUTF8String





/////////////////////////////////////////////////////////////////////////////
//
// [ResetConsoles]
//
/////////////////////////////////////////////////////////////////////////////
void
OSIndependantLayer::ResetConsoles() {
    int32 index;
    for (index = 0; index < MAX_CONSOLES; index++) {
        g_pConsoleList[index] = NULL;
    }
} // ResetConsoles




/////////////////////////////////////////////////////////////////////////////
//
// [SetConsole]
//
/////////////////////////////////////////////////////////////////////////////
void
OSIndependantLayer::SetConsole(CConsoleUserInterface *pConsole) {
    int32 index;

    for (index = 0; index < MAX_CONSOLES; index++) {
        if (NULL == g_pConsoleList[index]) {
	        g_pConsoleList[index] = pConsole;
	        break;
	    }
    }
} // SetConsole




/////////////////////////////////////////////////////////////////////////////
//
// [RemoveConsole]
//
/////////////////////////////////////////////////////////////////////////////
void
OSIndependantLayer::RemoveConsole(CConsoleUserInterface *pConsole) {
    int32 index;

    for (index = 0; index < MAX_CONSOLES; index++) {
        if (pConsole == g_pConsoleList[index]) {
	        g_pConsoleList[index] = NULL;
	        break;
	    }
    }
} // RemoveConsole






/////////////////////////////////////////////////////////////////////////////
//
// [PrintToConsole]
//
/////////////////////////////////////////////////////////////////////////////
void
OSIndependantLayer::PrintToConsole(const char *pFormat, ...) {
    va_list argList;
    char tempBuffer[1024];
    CConsoleUserInterface *pConsole;
    int32 index;

    va_start(argList, pFormat);
    vsnprintf(tempBuffer, sizeof(tempBuffer), pFormat, argList);
    va_end(argList);

    for (index = 0; index < MAX_CONSOLES; index++) {
        pConsole = g_pConsoleList[index];
        if (pConsole) {
            pConsole->PrintToConsole(tempBuffer);
        }
    }
} // PrintToConsole





/////////////////////////////////////////////////////////////////////////////
//
// [ShowProgress]
//
/////////////////////////////////////////////////////////////////////////////
void
OSIndependantLayer::ShowProgress() {
    CConsoleUserInterface *pConsole;
    int32 index;

    for (index = 0; index < MAX_CONSOLES; index++) {
        pConsole = g_pConsoleList[index];
	if (pConsole) {
	  pConsole->ShowProgress();
	}
    }
} // ShowProgress.





/////////////////////////////////////////////////////////////////////////////
//
// [ReportError]
//
/////////////////////////////////////////////////////////////////////////////
void
OSIndependantLayer::ReportError(
                       const char *pFileName, 
                       int lineNum, 
                       const char *pMsg) {
    char tempBuffer[1024];
    CConsoleUserInterface *pConsole;
    int32 index;

    if (NULL == pFileName){
        pFileName = "";
    }

    snprintf(
	 tempBuffer,
	 sizeof(tempBuffer),
	 "\nError (%s, %d) %s",
	 pFileName,
	 lineNum,
	 pMsg);

    for (index = 0; index < MAX_CONSOLES; index++) {
        pConsole = g_pConsoleList[index];
	if (pConsole) {
	    pConsole->PrintToConsole(tempBuffer);
	}
    }

#if DD_DEBUG
    BreakToDebugger();
#endif // DD_DEBUG
} // ReportError






/////////////////////////////////////////////////////////////////////////////
//
//                       TESTING PROCEDURES
//
// This testing is a little special because this module is so low level that
// it cannot use the standard test package. It simply prints an error code
// indicating whether it passed its tests or not.
/////////////////////////////////////////////////////////////////////////////
#if INCLUDE_REGRESSION_TESTS

static void TestRandomNumbers();

static const char *g_TestFileName = "text.txt";
static const char *g_TestFileContents = "This is the contents of the test file.\n";
static int32 g_TestFileContentsLength;

#pragma GCC diagnostic ignored "-Wformat-truncation"
#define MAX_TEST_PATHNAME_LENGTH     2048





/////////////////////////////////////////////////////////////////////////////
//
// [TestOSIndependantLayer]
//
/////////////////////////////////////////////////////////////////////////////
void
OSIndependantLayer::TestOSIndependantLayer() {
    ErrVal err = ENoErr;
    int measured;
    char testDirPath[MAX_TEST_PATHNAME_LENGTH];
    char testFilePath[MAX_TEST_PATHNAME_LENGTH];
    char testAltFilePath[MAX_TEST_PATHNAME_LENGTH + 16];
    CSimpleFile file;
    CSimpleFile file2;
    uint64 resultLength;
    char resultBuffer[1024];
    int32 resultRead;


    OSIndependantLayer::PrintToConsole("Test Module: OS Interface");


    ///////////////////////////////////////////////////////////////////////
    OSIndependantLayer::PrintToConsole("  Test: Basic data type sizes.");
    measured = sizeof(uint8);
    if (measured != 1) {
        OSIndependantLayer::PrintToConsole("Error. Wrong size for uint8");
        REPORT_LOW_LEVEL_BUG();
    }

    measured = sizeof(int8);
    if (measured != 1) {
        OSIndependantLayer::PrintToConsole("Error. Wrong size for int8");
        REPORT_LOW_LEVEL_BUG();
    }

    measured = sizeof(uint16);
    if (measured != 2) {
        OSIndependantLayer::PrintToConsole("Error. Wrong size for uint16");
        REPORT_LOW_LEVEL_BUG();
    }

    measured = sizeof(int16);
    if (measured != 2) {
        OSIndependantLayer::PrintToConsole("Error. Wrong size for int16");
        REPORT_LOW_LEVEL_BUG();
    }

    measured = sizeof(uint32);
    if (measured != 4) {
        OSIndependantLayer::PrintToConsole("Error. Wrong size for uint32");
        REPORT_LOW_LEVEL_BUG();
    }

    measured = sizeof(int32);
    if (measured != 4) {
        OSIndependantLayer::PrintToConsole("Error. Wrong size for int32");
        REPORT_LOW_LEVEL_BUG();
    }

    measured = sizeof(uchar);
    if (measured != 1) {
        OSIndependantLayer::PrintToConsole("Error. Wrong size for uchar");
        REPORT_LOW_LEVEL_BUG();
    }


    ///////////////////////////////////////////////////////////////////////
    OSIndependantLayer::PrintToConsole("  Test: Random Number Generator");
    TestRandomNumbers();


    ///////////////////////////////////////////////////////////////////////
    OSIndependantLayer::PrintToConsole("  Test: Directory Operations.");

    // Create the directory and make sure it exists.
    snprintf(testDirPath, sizeof(testDirPath), 
             "%s%c%s", 
             g_SoftwareDirectoryRoot,
             DIRECTORY_SEPARATOR_CHAR,
             "testData");
    //printf("\n==> testDirPath = %s\n", testDirPath);

    err = CSimpleFile::CreateDirectory(testDirPath);
    if (err) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    if (!(CSimpleFile::DirectoryExists(testDirPath))) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    if (!(CSimpleFile::FileOrDirectoryExists(testDirPath))) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    if (!(CSimpleFile::IsDirectory(testDirPath))) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    if (CSimpleFile::FileExists(testDirPath)) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }

    // Delete it and make sure it is gone.
    err = CSimpleFile::DeleteDirectory(testDirPath);
    if (err) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    if (CSimpleFile::DirectoryExists(testDirPath)) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    if (CSimpleFile::FileOrDirectoryExists(testDirPath)) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    if (CSimpleFile::IsDirectory(testDirPath)) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    if (CSimpleFile::FileExists(testDirPath)) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }


    // Recreate it.
    err = CSimpleFile::CreateDirectory(testDirPath);
    if (err) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    // Try to create it when it already exists.
    err = CSimpleFile::CreateDirectory(testDirPath);
    if (err) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    if (!(CSimpleFile::DirectoryExists(testDirPath))) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    if (!(CSimpleFile::FileOrDirectoryExists(testDirPath))) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    if (!(CSimpleFile::IsDirectory(testDirPath))) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    if (CSimpleFile::FileExists(testDirPath)) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }



    ///////////////////////////////////////////////////////////////////////
    OSIndependantLayer::PrintToConsole("  Test: File Operations");
    snprintf(
       testFilePath,
       sizeof(testFilePath),
       "%s%c%s",
       testDirPath,
       DIRECTORY_SEPARATOR_CHAR,
       g_TestFileName);
    snprintf(
       testAltFilePath,
       sizeof(testAltFilePath),
       "%s.alt",
       testFilePath);
    g_TestFileContentsLength = strlen(g_TestFileContents);

    err = CSimpleFile::DeleteFile(testFilePath);
    if (err) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }

    err = file.OpenOrCreateEmptyFile(testFilePath, 0);
    if (err) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    err = file.Write(g_TestFileContents, g_TestFileContentsLength);
    if (err) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    file.Close();
    err = file2.OpenExistingFile(testFilePath, CSimpleFile::READ_ONLY);
    if (err) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    err = file2.GetFileLength(&resultLength);
    if ((err) || (resultLength != (uint64) g_TestFileContentsLength)) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    err = file2.Read(
            resultBuffer,
            g_TestFileContentsLength,
            &resultRead);
    if (err) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    if (resultRead != g_TestFileContentsLength) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    if (0 != strncmp(resultBuffer, g_TestFileContents, resultRead)) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    file2.Close();
    file.Close();

    if (!(CSimpleFile::FileExists(testFilePath))) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    err = CSimpleFile::MoveFile(testFilePath, testAltFilePath);
    if (err) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    if (CSimpleFile::FileExists(testFilePath)) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }




    ///////////////////////////////////////////////////////////////////////
    OSIndependantLayer::PrintToConsole("  Test: File Directory Iteration");
    CDirFileList dirList;

    err = dirList.Open(testDirPath);
    if (err) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }

    while (1) {
       char filePath[1024];
       bool gotFile;

       gotFile = dirList.GetNextFile(filePath, sizeof(filePath));
       if (!gotFile) {
          break;
       }
       //<>OSIndependantLayer::PrintToConsole("\nFound file: %s", filePath);
    }


    // Now, just clean up.
    err = CSimpleFile::DeleteFile(testAltFilePath);
    if (err) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }
    if (CSimpleFile::FileExists(testFilePath)) {
        OSIndependantLayer::PrintToConsole("Error, test failed");
        REPORT_LOW_LEVEL_BUG();
    }


    ///////////////////////////////////////////////////////////////////////
    OSIndependantLayer::PrintToConsole("  Test: Date/Time");
    CDateTime date;
    char dateStr[256];

    date.SetValue(
            1, // month,
            12, // hour,
            30, // minutes,
            0, // seconds,
            31, // date,
            1963, // year,
            NULL); // *tzNameStr

    date.PrintToString(dateStr, sizeof(dateStr), CDateTime::W3C_LOG_FORMAT, NULL);
    OSIndependantLayer::PrintToConsole("DateStr: %s", dateStr);

    PrintTimeInMsToString((uint64) 2820721 * 1000, dateStr, sizeof(dateStr));
    OSIndependantLayer::PrintToConsole("Time: %s", dateStr);

    OSIndependantLayer::PrintToConsole("\n");
} // TestOSIndependantLayer





/////////////////////////////////////////////////////////////////////////////
//
// [TestRandomNumbers]
//
/////////////////////////////////////////////////////////////////////////////
#define NUM_TEST_RANDOM_NUMBERS      200
void
TestRandomNumbers() {
    int32 valueNum;
    int32 compareNum;
    int32 values[NUM_TEST_RANDOM_NUMBERS];


    // This test was described in the paper we used for the
    // random number generator algorithm.
    OSIndependantLayer::SetRandSeed(1);
    for (valueNum = 0; valueNum < NUM_TEST_RANDOM_NUMBERS; valueNum++) {
        int32 tempRand = OSIndependantLayer::GetRandomNum();
        values[valueNum] = tempRand;
    }

    for (valueNum = 0; valueNum < NUM_TEST_RANDOM_NUMBERS; valueNum++) {
        for (compareNum = 0; compareNum < valueNum; compareNum++) {
            if (values[valueNum] == values[compareNum]) {
                OSIndependantLayer::PrintToConsole("Error, Two different calls generated the same number");
                REPORT_LOW_LEVEL_BUG();
            }
        }
    }
} // TestRandomNumbers



#endif // INCLUDE_REGRESSION_TESTS

