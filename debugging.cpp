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
// Debugging Module
//
// This implements some general debugging utilities, including:
//
// 1. DEBUG_LOG, a general procedure line printf except it print
//        formatted messages to a debug log file
//
// 2. DEBUG_WARNING(), the standard procedure for reporting an error. It
//        logs, prints to the console, updates counters, and optionally
//        breaks to the debugger.
//
// 3. ASSERT macros
//
// 4. Code to check the state of an object whenever we enter and leave a
//    a method on the object.
//
// 5. Code to record events in an audit trail stored in any debug object.
//
// This module also defines a base class, CDebugObject, that provides these
// debugging functions to any inherited classes. Most classes in the building
// blocks will inherit from CDebugObject.
//
/////////////////////////////////////////////////////////////////////////////

#include "osIndependantLayer.h"
#include "log.h"
#include "config.h"
#include "debugging.h"

FILE_DEBUGGING_GLOBALS(LOG_LEVEL_DEFAULT, 0);

#if LINUX
#include <signal.h>
#include <sys/signal.h>
static void ProcessLinuxSignal(int signalId);
#endif // LINUX



/////////////////////////////////////////////////////////////////////////////
//
//                            GLOBAL DATA
//
/////////////////////////////////////////////////////////////////////////////

// This is the default profiling state for the entire program.
CEventLog *g_pDebugLog = NULL;

extern bool g_ShutdownBuildingBlocks;
extern CConfigSection *g_pBuildingBlocksConfig;

// The names of config Values that control debugging.
static const char *LogDirRegistryValue = "Log Directory";
static char DebugLogLevelValueName[] = "Log Level";
static const char *MaxLogSizeRegistryValue = "Max Log Size";
static char AllowBreakToDebuggerValueName[] = "Debug Warnings";
static char PrintWarningsToConsoleValueName[] = "Print Warnings To Console";
static char BreakToDebuggerOnUntestedCodeValueName[] = "Debug On Untested Code";

CDebugManager g_DebugManager;
int32 CDebugManager::g_NumTestWarnings = 0;
ErrVal CDebugManager::g_AdditionalPossibleBugError = ENoErr;
bool CDebugManager::g_AlwaysDoConsistencyChecks = false;
bool CDebugManager::g_TestingErrorCases = false;
bool CDebugManager::g_BreakToDebuggerOnWarnings = true;
bool CDebugManager::g_PrintWarningsToConsole = true;
bool CDebugManager::g_BreakToDebuggerOnUntestedCode = true;

int32 CDebugManager::g_DebugLogOptions =  
                                CDebugManager::ADD_TIMESTAMP_TO_EACH_LINE
                                   | CDebugManager::ADD_FILENAME_TO_EACH_LINE
                                   | CDebugManager::ADD_FUNCTION_TO_EACH_LINE;
int32 CDebugManager::g_MaxDebugLogLevel = 0;

static OSIndependantLock   g_DebugLogLock;

static char g_DefaultLogDirectory[2048];

#define DEFAULT_DEBUG_LOG_BUFFER_SIZE (64 * 1024)

// Config Values
// These are not defined in buildingBlocks.h, because they
// are used by files that cannot include buildingBlocks.h since they
// do not use the memory allocator.
static char TestDataDirRegistryValue[] = "Test Data";
static char TestResultDirRegistryValue[] = "Test Results";

static char g_LogDirectoryName[] = "logs";

#pragma GCC diagnostic ignored "-Wformat-truncation"

// Human readable strings for all error messages.
// For internationalization, this should be read from a text file.
class CErrorString {
public:
    ErrVal          m_Err;
    const char      *m_pString;
};
static CErrorString g_ErrorStrings[] = {
   { ENoErr, "OK - No Err" },
   { EFail, "Unknown Error" },
   { EOutOfMemory, "Out Of Memory" },
   { ENotImpl, "Not Implemented" },
   { EInvalidArg, "Invalid Arg" },
   { EEOF, "End Of File" },
   { ENoResponse, "No Response" },
   { EHTTPSRequired, "HTTPS Required" },
   { ENoHostAddress, "Server Not Found" },
   { EValueIsNotNumber, "Not A Number" },
   { EFileNotFound, "File Not Found" },
   { ERequiredFileNotFound, "System File Not Found" },
   { ENoDiskSpace, "Disk Full" },
   { EPeerDisconnected, "Peer Reset" },
   { ETooManySockets, "Too Many Sockets" },
   { EInvalidUrl, "Url Is Too Long" },
   { EInvalidHttpHeader, "Invalid Http Header" },
   { EHTTPDocTooLarge, "HTTP Doc Too Large" },
   { EXMLParseError, "XML Parse Error" },
   { ESyntheticError, "Artificial Test Error" },
   { EDataFileSyntaxError, "DataFile Syntax Error" },
   { EDataFileItemNotFound, "Item Not Found in DataFile" },
   { EInvalidRequest, "Invalid Request" },   
   { ENoErr, NULL }
}; // g_ErrorStrings



// This is a list of URLs that are handy for various tests.
// This used to be stored as a separate data file, but that requires
// some installation before any code can be tested. These are all pretty
// stable URLs, so they shouldn'e become invalid any time soon.
const char *g_TestURLList[] = {
   "http://www.amazon.com/",
   "http://www.microsoft.com/",
   "http://www.microsoft.com/ms.htm",
   "http://www.research.microsoft.com/research/",
   "http://www.wired.com/",
   "http://www.altavista.com/",
   "http://www.aol.com/",
   "http://www.slate.com/",
   "http://www.wired.com/news/",
   "http://www.unitedmedia.com/comics/dilbert/",
   "http://www.cs.princeton.edu/",
   "http://www.inc.com/",
   "http://www.javasoft.com/",
   "http://www.cnn.com/",
   "http://cnnfn.com/",
   "http://www.nytimes.com/",
   "http://www.herring.com",
   "http://home.cnet.com/",
   "http://www.mercurycenter.com",
   "http://www.businessweek.com/",
   "http://www.unitedmedia.com/comics/dilbert/",
   "http://www.reuters.com",
   "http://www.wsj.com/",
   "http://www.thenation.com/",
   "http://www.hollywoodreporter.com/",
   "http://www.lycos.com/",
   "http://www.yahoo.com/",
   "http://www.adaptec.com/",
   "http://www.seagate.com/",
   "http://www.quantum.com/",
   "http://www.avid.com/",
   "http://www.idsoftware.com/",
   "http://www.nyse.com/",
   "http://www.schwab.com/",
   "http://www.citibank.com/",
   "http://www.bofa.com/",
   "http://www.bell-labs.com/",
   "http://www.economist.com/",
   "http://www.aol.com",
   "http://www.news.com/",
   "http://wellsfargo.com/home/",
   "http://www.nasdaq.com/",
   "http://www.shareware.com/",
   "http://www.winfiles.com/",
   "http://www.hp.com/",
   "http://www.sgi.com/",
   "http://www.cs.purdue.edu/coast/archive/index.html",
   "http://www.anonymizer.com/",
   "http://www.apache.org/",
   "http://electron.rutgers.edu/",
   "http://www.ddj.com/",
   "http://gatekeeper.dec.com/",
   "http://www.w3.org/",
   "http://www.ncsa.uiuc.edu/SDG/Software/Mosaic/NCSAMosaicHome.html",
   "http://www.castlerock.com/",
   "http://www.cis.ohio-state.edu/",
   "http://www.dataviz.com/",
   "http://www.tripod.com/",
   "http://www.javasoft.com/",
   "http://java.sun.com/",
   "http://java.sun.com/docs/index.html",
   "http://www.osf.org/",
   "http://www.w3.org/Protocols/Specs.html",
   "http://www.isi.edu/",
   "http://samba.anu.edu.au/samba/",
   "http://java.sun.com/applets/index.html",
   "http://ftp.digital.com/",
   "http://www.mindspring.com/",
   "http://www.thetech.org/",
   "http://sandbox.parc.xerox.com/",
   "http://www.engr.csulb.edu/",
   "http://www.crc.org/",
   "http://www.parc.xerox.com/ops/projects/forum/forum-schedule-external.html",
   "http://www.roughtrade.com/",
   "http://submerge.com/",
   "http://www.rsrecords.com/",
   "http://www.subpop.com/",
   "http://rhino.com/",
   "http://www.maths.monash.edu.au/",
   "http://www.ubl.com/",
   "http://www.musicsearch.com/",
   "http://www.hyperreal.org/raves/sf/",
   "http://www.sfraves.org/",
   "http://www.cygnus.com/misc/",
   "http://www.nme.com/",
   "http://www.sfai.edu/",
   "http://www.christusrex.org/www1/sistine/0-Tour.html",
   "http://www.55broadst.com/",
   "http://www.srl.org/",
   "http://www.robotwars.com/",
   "http://us.imdb.com/",
   "http://www.woz.org/woz/",
   "http://pacer1.usca.sc.edu/",
   "http://www.semenzato.com/",
   "http://edcom.com/",
   "http://www.nutsvolts.com/",
   "http://www.wired.com/wired/index.html",
   "http://www.thenation.com/",
   "http://www.slate.com/",
   "http://cnnfn.com/index.html",
   "http://www.artforum.com/",
   "http://www.rollcall.com/",
   "http://www.businessweek.com/",
   "http://www.reuters.com/",
   "http://www.economist.com/",
   "http://www.yahoo.com/",
   "http://www.usia.gov/usis.html",
   "http://www.uspto.gov/",
   "http://rs.internic.net/",
   "http://www.mapquest.com/",
   "http://www.w3.org/TR/",
   "http://www.financenet.gov/",
   "http://www.thestreet.com/index.html",
   "http://www.realaudio.com/",
   "http://www.lucent.com/",
   "http://www.research.att.com/",
   "http://www.cygnus.com/",
   "http://www.redhat.com/",
   "http://developer.intel.com/",
   NULL
};






/////////////////////////////////////////////////////////////////////////////
//
// [InitializeDebugging]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CDebugManager::InitializeDebugging(CProductInfo *pVersion, int32 options) {
    ErrVal err = ENoErr;
    char logDirPathName[1024];
    char logFilePathName[1024];
    char osVersion[1024];
    char *pDestPtr;
    char *pEndDestPtr;
    bool loggingOn = true;
    int32 logOptions;
    int32 maxLogSize;

    g_DebugLogOptions = options;

    // Remove the control C catcher. This is needed because the
    // software may keep running after the tests stop. For example,
    // the test shell continues to run after the tests end.
#if LINUX
    signal(SIGINT, ProcessLinuxSignal);
    signal(SIGHUP, ProcessLinuxSignal);
    signal(SIGQUIT, ProcessLinuxSignal);
    signal(SIGBUS, ProcessLinuxSignal);
#endif // LINUX

    if (g_pBuildingBlocksConfig) {
        g_BreakToDebuggerOnWarnings = g_pBuildingBlocksConfig->GetBool(
                                            AllowBreakToDebuggerValueName,
                                            g_BreakToDebuggerOnWarnings);
        g_PrintWarningsToConsole = g_pBuildingBlocksConfig->GetBool(
                                            PrintWarningsToConsoleValueName,
                                            g_PrintWarningsToConsole);
        maxLogSize = g_pBuildingBlocksConfig->GetInt(
                                            MaxLogSizeRegistryValue,
                                            CEventLog::UNLIMITED_LOG_MAX_SIZE);
        g_MaxDebugLogLevel = g_pBuildingBlocksConfig->GetInt(
                                            DebugLogLevelValueName,
                                            1);
        g_BreakToDebuggerOnUntestedCode = g_pBuildingBlocksConfig->GetBool(
                                            BreakToDebuggerOnUntestedCodeValueName,
                                            g_BreakToDebuggerOnUntestedCode);
    } // if (g_pBuildingBlocksConfig)

    err = g_DebugLogLock.Initialize();
    if (err) {
        gotoErr(err);
    }

    snprintf(g_DefaultLogDirectory,
             sizeof(g_DefaultLogDirectory),
             "%s%c%s",
             g_SoftwareDirectoryRoot,
             DIRECTORY_SEPARATOR_CHAR,
             g_LogDirectoryName);

    logDirPathName[0] = 0;
    if (g_pBuildingBlocksConfig) {
        g_pBuildingBlocksConfig->GetPathname(
                                    LogDirRegistryValue,
                                    logDirPathName,
                                    sizeof(logDirPathName),
                                    g_DefaultLogDirectory);
    }

    //OSIndependantLayer::PrintToConsole("LogDirRegistryValue: %s", LogDirRegistryValue);
    //OSIndependantLayer::PrintToConsole("logFilePathName: %s", logFilePathName);

    if (0 == logDirPathName[0]) {
        loggingOn = false;
    } else {
        // Make a log file name.
        pDestPtr = logFilePathName;
        pEndDestPtr = pDestPtr + sizeof(logFilePathName);
        err = pVersion->PrintToString(pDestPtr, 
                                      pEndDestPtr - pDestPtr, 
                                      CProductInfo::PRINT_SHORT_SOFTWARE_NAME, 
                                      &pDestPtr);
        pDestPtr += snprintf(pDestPtr, pEndDestPtr - pDestPtr, "Log.txt");
    }


    if (loggingOn) {
        g_pDebugLog = new CEventLog;
        if (NULL == g_pDebugLog) {
            err = EFail;
            goto abort;
        }

        // Initialize the log.
        logOptions = CEventLog::INITIALIZE_LOG
                    | CEventLog::DELETE_OLD_LOGS
                    | CEventLog::ALWAYS_CREATE_NEW_FILE;

#if DD_DEBUG
        // Periodically flush the log.
        //<>logOptions |= CEventLog::ALWAYS_FLUSH;
#endif

        //OSIndependantLayer::PrintToConsole("logFilePathName: %s", 
        // logFilePathName);
        err = g_pDebugLog->Initialize(
                                logDirPathName,
                                logFilePathName,
                                CEventLog::DEFAULT_LOG_BUFFER_SIZE,
                                logOptions,
                                maxLogSize,
                                pVersion,
                                "date time [file thread function] comment");
        if (err) {
            goto abort;
        }

#if WIN32
        LOG_ALWAYS(">Windows build");
#elif LINUX
        LOG_ALWAYS(">Linux build");
#endif

        OSIndependantLayer::GetOSVersionString(osVersion, sizeof(osVersion));
        LOG_ALWAYS(">Host = %s", osVersion);

#if DD_DEBUG
        LOG_ALWAYS(">Debug build");
        LOG_ALWAYS(">Global max DebugLog level = %d", g_MaxDebugLogLevel);
#endif
    } // if (loggingOn)


    // Flush the log, to get all the standard output written to the file.
    err = g_pDebugLog->Flush();
    if (err) {
        goto abort;
    }

abort:
    return(err);
} // InitializeDebugging.






/////////////////////////////////////////////////////////////////////////////
//
// [ShutdownDebugging]
//
/////////////////////////////////////////////////////////////////////////////
void
CDebugManager::ShutdownDebugging() {
    ErrVal err = ENoErr;

    if (NULL != g_pDebugLog) {
        err = g_pDebugLog->Flush();
        if (err) {
            DEBUG_WARNING("Error in main shutdown.");
        }
        delete g_pDebugLog;
    }

    // Leave the debug log buffer open for now.
} // ShutdownDebugging






/////////////////////////////////////////////////////////////////////////////
//
// [CDebugFileInfo]
//
// This is a class so we can execute code as part of initializing a modules
// global variables.
/////////////////////////////////////////////////////////////////////////////
CDebugFileInfo::CDebugFileInfo(const char *szFileNameArg, int32 maxLogLevel, int32 initialFlags) {
    UNUSED_PARAM(szFileNameArg);
    m_MaxLogLevel = maxLogLevel;
    m_FileDebugFlags = initialFlags;
    m_AnyErrorIsABug = false;
} // CDebugFileInfo.





/////////////////////////////////////////////////////////////////////////////
//
// [CDebugObject]
//
// Initialize a debug object. All initialization must be done in the
// constructor, since we cannot rely on any other method being called in
// a buggy program.
/////////////////////////////////////////////////////////////////////////////
CDebugObject::CDebugObject() {
    m_DebugFlags = 0;
    m_pHistory = NULL;
} // CDebugObject.






/////////////////////////////////////////////////////////////////////////////
//
// [~CDebugObject]
//
/////////////////////////////////////////////////////////////////////////////
CDebugObject::~CDebugObject() {
    // Discard any history objects.
    //
    // When the program is exiting, the C-Runtime may no longer
    // be available. This means that when a global variable goes
    // out of scope, it cannot clean up normally. This is ok, because
    // we won't leak since ht eprogram is terminating.
    if ((m_pHistory) && (!g_ShutdownBuildingBlocks)) {
        m_pHistory->m_hHistoryLock.Shutdown();
        delete m_pHistory;
        m_pHistory = NULL;
    }
} // ~CDebugObject.





/////////////////////////////////////////////////////////////////////////////
//
// [CheckState]
//
// This checks the state of the object itself.
// Most classes will overload this method.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CDebugObject::CheckState() {
    return(ENoErr);
} // CheckState.





/////////////////////////////////////////////////////////////////////////////
//
// [SignObjectImpl]
//
/////////////////////////////////////////////////////////////////////////////
void
CDebugObject::SignObjectImpl(
                    CDebugObject::EntryType tType,
                    const char *pMessage,
                    const char *szFileName,
                    int32 lLineNum,
                    int32 intValue) {
    ErrVal err = ENoErr;
    CDebugObject::CHistoryEntry *pEntry;

    // If there is not a history for this object, then allocate one.
    if (NULL == m_pHistory) {
        err = AllocHistory();
        if (err) {
            return;
        }
    }

    m_pHistory->m_hHistoryLock.BasicLock();

    pEntry = &((m_pHistory->m_Entries)[ m_pHistory->m_NextIndex ]);

    pEntry->m_Type = tType;
    pEntry->m_pInfo = pMessage;
    pEntry->m_FileName = szFileName;
    pEntry->m_LineNum = lLineNum;
    pEntry->m_ThreadNum = OSIndependantLayer::GetCurrentThreadId();
    pEntry->m_IntValue = intValue;

    m_pHistory->m_NumItems += 1;
    m_pHistory->m_NextIndex += 1;
    if (m_pHistory->m_NextIndex >= CDebugObject::MAX_HISTORY_ENTRIES) {
        m_pHistory->m_NextIndex = 0;
    }

    m_pHistory->m_hHistoryLock.BasicUnlock();
} // SignObjectImpl.







/////////////////////////////////////////////////////////////////////////////
//
// [AllocHistory]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CDebugObject::AllocHistory() {
    ErrVal err = ENoErr;
    CDebugHistory *pHistory;

    if (NULL == m_pHistory) {
        pHistory = new CDebugHistory;
        if (NULL == pHistory) {
            DEBUG_WARNING("Cannot allocate a history object");
            err = EFail;
            goto abort;
        }

        pHistory->m_NumItems = 0;
        pHistory->m_NextIndex = 0;

        err = pHistory->m_hHistoryLock.Initialize();
        if (err) {
            delete pHistory;
            DEBUG_WARNING("Cannot initialize a history object");
            goto abort;
        }

        m_pHistory = pHistory;
    }

abort:
    return(err);
} // AllocHistory.






/////////////////////////////////////////////////////////////////////////////
//
// [DebugLogImpl]
//
/////////////////////////////////////////////////////////////////////////////
void
CDebugLogObject::DebugLogImpl(const char *format, ...) {
    va_list argPtr;
    CDateTime now;
    const char *srcPtr;
    char *destPtr;
    char *pEndDestPtr;
    char logMessageBuffer[2048];
    bool fHoldingLock = false;
    bool fAddDebugInfo = true;

    if ((NULL == format) || (g_ShutdownBuildingBlocks)) {
        return;
    }

    fAddDebugInfo = false;
    if ((LOG_TYPE_DEBUG == m_LogCallType)
        || (LOG_TYPE_WARNING == m_LogCallType)
        || (LOG_TYPE_ASSERT == m_LogCallType)) {
        fAddDebugInfo = true;
    }

    // Ignore log calls above a specified level of detail.
    if (LOG_TYPE_DEBUG == m_LogCallType) {
        bool fTooMuchInformation = true;

        if (m_LogCallLevel < g_DebugManager.g_MaxDebugLogLevel) {
            fTooMuchInformation = false;
        } else if ((m_pFileInfo) 
                   && (m_LogCallLevel < m_pFileInfo->m_MaxLogLevel)) {
            fTooMuchInformation = false;
        }
        if (fTooMuchInformation) {
            return;
        }
    } // if (LOG_TYPE_DEBUG == m_LogCallType)

    if (NULL == m_pFileName) {
        m_pFileName = "";
    }
    if (NULL == m_pFunctionName) {
        m_pFunctionName = "";
    }

    // Strip off the module name from the function. This loses some information
    // but it is rarely needed and it clutters the log.
    srcPtr = m_pFunctionName;
    while ((*srcPtr) && (':' != *srcPtr)) {
        srcPtr++;
    }
    if (':' == *srcPtr) {
        while (':' == *srcPtr) {
            srcPtr++;
        }
        m_pFunctionName = srcPtr;
    }

    // If we are going to use the file name, then clean it up.
    // Only do this if we will actually use the file name.
    // Strip off the directory path before the file name. This just makes the
    // log harder to read.
    if ((LOG_TYPE_ASSERT == m_LogCallType) 
        || ((CDebugManager::g_DebugLogOptions & CDebugManager::ADD_FILENAME_TO_EACH_LINE))) {
        const char *pLastSeparator;
        pLastSeparator = m_pFileName + strlen(m_pFileName) - 1;
        while (pLastSeparator > m_pFileName) {
            if (IS_DIRECTORY_SEPARATOR(*pLastSeparator)) {
                pLastSeparator += 1;
                break;
            }
            pLastSeparator--;
        }
        if (pLastSeparator > m_pFileName) {
            m_pFileName = pLastSeparator;
        }
    } // Clean up the file name.

    va_start(argPtr, format);


    // Grab the lock. This does 2 things. It ensures that 2 threads will always 
    // have ordered timestamps on their log lines. It also arbitrates contention 
    // on the log file if 2 threads both try to flush the log at the same time.
    g_DebugLogLock.BasicLock();
    fHoldingLock = true;


    // Format the log line.
    destPtr = logMessageBuffer;
    pEndDestPtr = logMessageBuffer + sizeof(logMessageBuffer) - 1;

    // Print the date and time.
    if (CDebugManager::g_DebugLogOptions & CDebugManager::ADD_TIMESTAMP_TO_EACH_LINE) {
        now.GetLocalDateAndTime();
        now.PrintToString(
                destPtr,
                pEndDestPtr - destPtr,
                CDateTime::W3C_LOG_FORMAT,
                &destPtr);
        if ((destPtr + 1) < pEndDestPtr) {
           *(destPtr++) = ' ';
        }
    }

    // Print the file and line.
    if ((CDebugManager::g_DebugLogOptions & CDebugManager::ADD_FILENAME_TO_EACH_LINE)
        && (fAddDebugInfo)) {
       destPtr += snprintf(
                    destPtr,
                    pEndDestPtr - destPtr,
                    "%s:%d ",
                    m_pFileName,
                    m_LineNum);
    }

    if ((CDebugManager::g_DebugLogOptions & CDebugManager::ADD_THREADID_TO_EACH_LINE)
        && (fAddDebugInfo)) {
        destPtr += snprintf(
                        destPtr,
                        pEndDestPtr - destPtr,
                        "(%d) ",
                        OSIndependantLayer::GetCurrentThreadId());
    }

    if ((CDebugManager::g_DebugLogOptions & CDebugManager::ADD_FUNCTION_TO_EACH_LINE)
        && (m_pFunctionName)
        && (*m_pFunctionName)
        && (fAddDebugInfo)) {
        destPtr += snprintf(
                        destPtr,
                        pEndDestPtr - destPtr,
                        "%s() ",
                        m_pFunctionName);
    }


    // Print the log message.
    if ((LOG_TYPE_WARNING == m_LogCallType) 
            && !(CDebugManager::g_TestingErrorCases)) {
        destPtr += snprintf(
                    destPtr,
                    pEndDestPtr - destPtr,
                    "[WARNING] ");
    } else if (LOG_TYPE_ASSERT == m_LogCallType) {
        destPtr += snprintf(
                    destPtr,
                    pEndDestPtr - destPtr,
                    "[ASSERT] ");
    }
    destPtr += vsnprintf(destPtr, pEndDestPtr - destPtr, format, argPtr);


    if (NULL != g_pDebugLog) {
        g_pDebugLog->LogMessage(logMessageBuffer);
    }

    if (fHoldingLock) {
        g_DebugLogLock.BasicUnlock();
    }


    // Outside the lock we may also fail assertions or handle warnings.
    if (LOG_TYPE_WARNING == m_LogCallType) {
        HandleWarning(logMessageBuffer);
    } else if (LOG_TYPE_ASSERT == m_LogCallType) {
        FailAssertion(m_pFileName, m_LineNum, logMessageBuffer);
    }

    va_end(argPtr);
} // DebugLogImpl





/////////////////////////////////////////////////////////////////////////////
//
// [HandleWarning]
//
// This is called when we log a warning.
/////////////////////////////////////////////////////////////////////////////
void
CDebugLogObject::HandleWarning(const char *message) {
    // Don't panic if we are testing error cases.
    if (CDebugManager::g_TestingErrorCases) {
       return;
    }

    // If things are about to crash, then save the log to disk.
    if (NULL != g_pDebugLog) {
        (void) g_pDebugLog->Flush();
    }

    // Do this first. On Linux, BreakToDebugger will do an exit(-1);
    if (CDebugManager::g_PrintWarningsToConsole) {
        OSIndependantLayer::PrintToConsole(message);
    }

#if DD_DEBUG
    if (CDebugManager::g_BreakToDebuggerOnWarnings) {
        OSIndependantLayer::BreakToDebugger();
    }
#endif

    CDebugManager::g_NumTestWarnings += 1;
} // HandleWarning.





/////////////////////////////////////////////////////////////////////////////
//
// [FailAssertion]
//
// This is called when an assert fails.
/////////////////////////////////////////////////////////////////////////////
void
CDebugLogObject::FailAssertion(
                const char *fileName,
                int32 lineNum,
                char *message) {
    // If things are about to crash, then save the log.
    if (NULL != g_pDebugLog) {
            (void) g_pDebugLog->Flush();
    }

#if DD_DEBUG
    if (CDebugManager::g_BreakToDebuggerOnWarnings) {
        OSIndependantLayer::BreakToDebugger();
    }
#endif

    OSIndependantLayer::ReportError(fileName, lineNum, message);
} // FailAssertion






/////////////////////////////////////////////////////////////////////////////
//
// [GetErrorDescriptionString]
//
/////////////////////////////////////////////////////////////////////////////
const char *
CDebugManager::GetErrorDescriptionString(ErrVal err) {
    CErrorString *pErrorInfo;

    pErrorInfo = g_ErrorStrings;
    while (NULL != pErrorInfo->m_pString) {
        if (err ==  pErrorInfo->m_Err) {
            return(pErrorInfo->m_pString);
        }

        pErrorInfo += 1;
    }

    return("Unknown Error");
} // GetErrorDescriptionString.





#if LINUX
/////////////////////////////////////////////////////////////////////////////
//
// [ProcessLinuxSignal]
//
/////////////////////////////////////////////////////////////////////////////
void
ProcessLinuxSignal(int signalId) {
   DEBUG_WARNING("\nCaught signal. signalId=%d", signalId);
} // ProcessLinuxSignal
#endif // LINUX






/////////////////////////////////////////////////////////////////////////////
//
// [RunningUntestedCode]
//
/////////////////////////////////////////////////////////////////////////////
void
CDebugManager::RunningUntestedCode(const char *pFunctionName, const char *pFileName, int32 lineNum) {
    CDebugLogObject(pFunctionName, pFileName, lineNum, &g_DebugFileInfo, LOG_LEVEL_IMPORTANT, LOG_TYPE_DEBUG).DebugLogImpl("Untested Code");

#if DD_DEBUG
    if (g_BreakToDebuggerOnUntestedCode) {
        OSIndependantLayer::BreakToDebugger();
    }
#endif
} // RunningUntestedCode.







/////////////////////////////////////////////////////////////////////////////
//
// [StartAllTests]
//
/////////////////////////////////////////////////////////////////////////////
void
CDebugManager::StartAllTests() {
    //char testDir[512];
    // Don't empty the test results directory. That contains the main
    // debugging log file.
    //AddTestResultsDirectoryPath("", testDir, sizeof(testDir));
    //CSimpleFile::EmptyDirectory(testDir);

    m_NumTestWarnings = 0;
    m_NumTestAsserts = 0;

    m_NumModulesTested = 0;
    m_NumTestSteps = 0;

    m_SubTestNestingLevel = 0;
    m_NumHeartbeats = 0;
    m_HeartbeatsBeforeProgressIndicator = 10;

    DEBUG_LOG("CDebugManager::StartAllTests. Start all tests");
} // StartAllTests.







/////////////////////////////////////////////////////////////////////////////
//
// [EndAllTests]
//
/////////////////////////////////////////////////////////////////////////////
void
CDebugManager::EndAllTests() {
    DEBUG_LOG("CDebugManager::EndAllTests. End all tests.");
    DEBUG_LOG("CDebugManager::EndAllTests. Number Modules = %d",
        m_NumModulesTested);
    DEBUG_LOG("CDebugManager::EndAllTests. Number Tests = %d",
        m_NumTestSteps);
    DEBUG_LOG("CDebugManager::EndAllTests. Total Assertions = %d",
        m_NumTestAsserts);
    DEBUG_LOG("CDebugManager::EndAllTests. Total Warnings = %d",
        m_NumTestWarnings);

    OSIndependantLayer::PrintToConsole("  ");
    OSIndependantLayer::PrintToConsole("  ");
    OSIndependantLayer::PrintToConsole("======================");
    OSIndependantLayer::PrintToConsole("Number Modules: %d", m_NumModulesTested);
    OSIndependantLayer::PrintToConsole("Number Steps: %d", m_NumTestSteps);
    OSIndependantLayer::PrintToConsole("Total Assertions: %d", m_NumTestAsserts);
    OSIndependantLayer::PrintToConsole("Total Warnings: %d", m_NumTestWarnings);
    OSIndependantLayer::PrintToConsole("======================");
    OSIndependantLayer::PrintToConsole("  ");

    if (NULL != g_pDebugLog) {
        ErrVal err = ENoErr;
        err = g_pDebugLog->Flush();
        if (err) {
            DEBUG_WARNING("Error in FlushLog.");
        }
    }

    // For now I don't empty the results directory since that contains logs.
    //AddTestResultsDirectoryPath("", testDir, sizeof(testDir));
    //CSimpleFile::EmptyDirectory(testDir);
} // EndAllTests.







/////////////////////////////////////////////////////////////////////////////
//
// [StartModuleTest]
//
/////////////////////////////////////////////////////////////////////////////
void
CDebugManager::StartModuleTest(const char *name) {
    SetProgressIncrement(10);

    DEBUG_LOG("CDebugManager::StartModuleTest. Start Module Test: %s", name);

    m_SubTestNestingLevel = 0;
    m_NumModulesTested += 1;

    // Print the message.
    OSIndependantLayer::PrintToConsole(" ");
    OSIndependantLayer::PrintToConsole(" ");
    OSIndependantLayer::PrintToConsole("===================================================");
    OSIndependantLayer::PrintToConsole("Module: %s", name);
    OSIndependantLayer::PrintToConsole("===================================================");
} // StartModuleTest.





/////////////////////////////////////////////////////////////////////////////
//
// [StartTest]
//
/////////////////////////////////////////////////////////////////////////////
void
CDebugManager::StartTest(const char *name) {
    char *nextChar;
    char *pLastChar;
    int32 indent;
    char LogMessageBuffer[256];

    DEBUG_LOG("CDebugManager::StartTest. Start Test: %s", name);

    // Print the message.
    nextChar = LogMessageBuffer;
    pLastChar = LogMessageBuffer + sizeof(LogMessageBuffer);
    for (indent = 0; indent < m_SubTestNestingLevel; indent++) {
        nextChar += snprintf(nextChar, pLastChar - nextChar, "   ");
    }
    snprintf(nextChar, pLastChar - nextChar, "==> Testing: %s", name);
    OSIndependantLayer::PrintToConsole(LogMessageBuffer);

    m_NumHeartbeats = 0;
} // StartTest.






/////////////////////////////////////////////////////////////////////////////
//
// [StartSubTest]
//
/////////////////////////////////////////////////////////////////////////////
void
CDebugManager::StartSubTest(const char *name) {
    char *nextChar;
    char *pLastChar;
    int32 indent;
    char LogMessageBuffer[2048];

    DEBUG_LOG("CDebugManager::StartSubTest. Start SubTest: %s", name);

    // Print the message.
    nextChar = LogMessageBuffer;
    pLastChar = LogMessageBuffer + sizeof(LogMessageBuffer);
    for (indent = 0; indent < m_SubTestNestingLevel; indent++) {
        nextChar += snprintf(nextChar, pLastChar - nextChar, "   ");
    }

    snprintf(
        nextChar,
        pLastChar - nextChar,
        "==> Testing: %s",
        name);
    OSIndependantLayer::PrintToConsole(LogMessageBuffer);

    m_NumHeartbeats = 0;
    m_SubTestNestingLevel += 1;
} // StartSubTest.






/////////////////////////////////////////////////////////////////////////////
//
// [EndSubTest]
//
/////////////////////////////////////////////////////////////////////////////
void
CDebugManager::EndSubTest() {
    DEBUG_LOG("CDebugManager::EndSubTest. End SubTest");

    m_NumHeartbeats = 0;

    m_SubTestNestingLevel = m_SubTestNestingLevel - 1;
    if (m_SubTestNestingLevel < 0) {
        m_SubTestNestingLevel = 0;
    }
} // EndSubTest.







/////////////////////////////////////////////////////////////////////////////
//
// [ShowProgress]
//
/////////////////////////////////////////////////////////////////////////////
void
CDebugManager::ShowProgress() {
    m_NumTestSteps += 1;

    m_NumHeartbeats++;
    if (m_NumHeartbeats >= m_HeartbeatsBeforeProgressIndicator) {
        OSIndependantLayer::ShowProgress();
        m_NumHeartbeats = 0;
    }
} // ShowProgress.






/////////////////////////////////////////////////////////////////////////////
//
// [SetProgressIncrement]
//
/////////////////////////////////////////////////////////////////////////////
void
CDebugManager::SetProgressIncrement(int32 val) {
    m_HeartbeatsBeforeProgressIndicator = val;
} // SetProgressIncrement.








/////////////////////////////////////////////////////////////////////////////
//
// [AddTestDataDirectoryPath]
//
/////////////////////////////////////////////////////////////////////////////
void
CDebugManager::AddTestDataDirectoryPath(
                    const char *fileName, char *path, int32 maxPath) {
    char *pSuffix;
    char *pEndPath;
    char defaultPath[256];

    if ((NULL == path) || (maxPath <= 0)) {
        DEBUG_WARNING("Bad arguments");
        return;
    }

    snprintf(
       defaultPath,
       sizeof(defaultPath),
       "%%InstallDir%%%c",
       DIRECTORY_SEPARATOR_CHAR);

    *path = 0;
    if (g_pBuildingBlocksConfig) {
        g_pBuildingBlocksConfig->GetPathname(
                                     TestDataDirRegistryValue,
                                     path,
                                     maxPath,
                                     defaultPath);
    }
    if (0 == *path) {
        return;
    }
    DEBUG_LOG("AddTestDataDirectoryPath: path=%s\n", path);

    pSuffix = path + strlen(path);
    pEndPath = path + maxPath;

    if ((pSuffix < pEndPath)
        && !(IS_DIRECTORY_SEPARATOR(*(pSuffix - 1)))) {
        *(pSuffix++) = DIRECTORY_SEPARATOR_CHAR;
    }

    strncpy(pSuffix, fileName, pEndPath - pSuffix);
} // AddTestDataDirectoryPath.






/////////////////////////////////////////////////////////////////////////////
//
// [AddTestResultsDirectoryPath]
//
/////////////////////////////////////////////////////////////////////////////
void
CDebugManager::AddTestResultsDirectoryPath(
                        const char *fileName,
                        char *path,
                        int32 maxPath) {
    ErrVal err = ENoErr;
    char *pSuffix;
    char *pEndPath;
    char defaultPath[256];


    if ((NULL == path) || (maxPath <= 0)) {
        DEBUG_WARNING("Bad arguments");
        return;
    }

    snprintf(
       defaultPath,
       sizeof(defaultPath),
       "%%InstallDir%%%c",
       DIRECTORY_SEPARATOR_CHAR);

    *path = 0;
    if (g_pBuildingBlocksConfig) {
        g_pBuildingBlocksConfig->GetPathname(
                                 TestResultDirRegistryValue,
                                 path,
                                 maxPath,
                                 defaultPath);
    }
    if (0 == *path) {
        return;
    }

    if (!(CSimpleFile::FileOrDirectoryExists(path))) {
        err = CSimpleFile::CreateDirectory(path);
        if (err) {
            DEBUG_WARNING("Bad arguments");
        }
    }

    pSuffix = path + strlen(path);
    pEndPath = path + maxPath;

    if ((pSuffix < pEndPath)
        && !(IS_DIRECTORY_SEPARATOR(*(pSuffix - 1)))) {
        *(pSuffix++) = DIRECTORY_SEPARATOR_CHAR;
    }

    strncpy(pSuffix, fileName, pEndPath - pSuffix);
} // AddTestResultsDirectoryPath.





/////////////////////////////////////////////////////////////////////////////
//
// [LowLevelReportTestModule]
//
// This is a wrapper function so it can be called by a low level module
// that does not include the test interface. Testing is buiilt on top of
// logging and other modules that may not be included in the lowest level
// modules.
/////////////////////////////////////////////////////////////////////////////
void
LowLevelReportTestModule(const char *pTestName) {
    g_DebugManager.StartModuleTest(pTestName);
} // LowLevelReportTestModule





/////////////////////////////////////////////////////////////////////////////
//
// [LowLevelReportStartTest]
//
// This is a wrapper function so it can be called by a low level module
// that does not include the test interface. Testing is buiilt on top of
// logging and other modules that may not be included in the lowest level
// modules.
/////////////////////////////////////////////////////////////////////////////
void
LowLevelReportStartTest(const char *pTestName) {
    g_DebugManager.StartTest(pTestName);
} // LowLevelReportStartTest



/////////////////////////////////////////////////////////////////////////////
//
// [SetDebugOptions]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CDebugManager::SetDebugOptions(int32 opCode, int32 options, char *pStr) {
    ErrVal err = ENoErr;
    UNUSED_PARAM(opCode);
    UNUSED_PARAM(options);
    UNUSED_PARAM(pStr);
    returnErr(err);
} // SetDebugOptions







