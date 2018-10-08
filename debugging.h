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

#ifndef _DEBUGGING_H_
#define _DEBUGGING_H_

// This is part of ANSI C and so should be on every platform.
#include <stdarg.h>

class CAutoStateChecker;




/////////////////////////////////////////////////////////////////////////////
//
//                        GLOBAL DEBUG MANAGER
//
// This manages global debugging state.
/////////////////////////////////////////////////////////////////////////////

class CDebugManager {
public:
    enum {
        // These are the options for every line to the debug log.
        ADD_FILENAME_TO_EACH_LINE   = 0x0001,
        ADD_THREADID_TO_EACH_LINE   = 0x0002,
        ADD_TIMESTAMP_TO_EACH_LINE  = 0x0004,
        ADD_FUNCTION_TO_EACH_LINE   = 0x0008,
    };

    // This controls RunChecks
    static bool     g_AlwaysDoConsistencyChecks;
    static bool     g_BreakToDebuggerOnUntestedCode;

    // These control gotoErr and returnErr
    static bool     g_TestingErrorCases;
    static bool     g_BreakToDebuggerOnWarnings;
    static ErrVal   g_AdditionalPossibleBugError;
    static int32    g_NumTestWarnings;

    static int32    g_DebugLogOptions;
    static int32    g_MaxDebugLogLevel;

    // These control the UI
    static bool     g_PrintWarningsToConsole;

    // Testing State
    int32           m_NumModulesTested;
    int32           m_NumTestSteps;
    int32           m_HeartbeatsBeforeProgressIndicator;
    int32           m_NumHeartbeats;
    int32           m_SubTestNestingLevel;

    int32           m_NumTestWarnings;
    int32           m_NumTestAsserts;


    static ErrVal InitializeDebugging(CProductInfo *pVersion, int32 options);
    static void ShutdownDebugging();

    static void WriteDebugMessagesToLog();
    static const char *GetErrorDescriptionString(ErrVal err);
    static void RunningUntestedCode(const char *pFunctionName, const char *pFileName, int32 lineNum);

    // Test Status Reporting
    void StartAllTests();
    void EndAllTests();
    void StartModuleTest(const char *name);
    void StartSubTest(const char *name);
    void EndSubTest();
    void StartTest(const char *name);
    void SetProgressIncrement(int32 val);
    void ShowProgress();

    // Test Directory
    void AddTestDataDirectoryPath(const char *fileName, char *path, int32 maxPath);
    void AddTestResultsDirectoryPath(const char *fileName, char *path, int32 maxPath);

    ErrVal SetDebugOptions(int32 opCode, int32 options, char *pStr);
}; // CDebugManager

extern CDebugManager g_DebugManager;

extern const char *g_TestURLList[];
void LowLevelReportTestModule(const char *pTestName);
void LowLevelReportStartTest(const char *pTestName);





/////////////////////////////////////////////////////////////////////////////
//
//                     PER-FILE DEBUGGING GLOBAL VARIABLES
//
// This is used in each file to declare the file name, and other per-file
// debugging settings that are used by various debugging routines.
//
// Define this class so we can run code in its constructor when
// global variables are initialized. This means it is not available
// to other global constructors.
/////////////////////////////////////////////////////////////////////////////

class CDebugFileInfo {
public:
    enum DebuggingFlags {
        SIGN_ALL_OBJECTS        = 0x0001,
        PRINTF_ANY_ERROR        = 0x0002,
    };

    int32   m_FileDebugFlags;
    int32   m_MaxLogLevel;
    bool    m_AnyErrorIsABug;

    CDebugFileInfo(const char *szFileNameArg, int32 maxLogLevel, int32 initialFlags);
}; // CDebugFileInfo

// This must be static so each file can have a different one.
//
// Adding the comment at the end of the line is important, because
// it means that if there is a mistaken ";" or "();" after this
// declaration, then it will be ignored.
#define FILE_DEBUGGING_GLOBALS(maxLogLevel, flags) static CDebugFileInfo g_DebugFileInfo(__FILE__, maxLogLevel, flags); //

enum {
    // These are the flags passed to log calls.
    LOG_TYPE_USER_INFO          = 1,
    LOG_TYPE_DEBUG              = 2,
    LOG_TYPE_WARNING            = 3,
    LOG_TYPE_ASSERT             = 4,

    // Level of importance for the log calls.
    LOG_LEVEL_IMPORTANT         = 0,
    // LOG_LEVEL_DEFAULT < LOG_LEVEL_DEBUG, so by default all debugging doesn't get logged.
    LOG_LEVEL_DEFAULT           = 1,
    LOG_LEVEL_DEBUG             = 2,
    LOG_LEVEL_VERBOSE           = 3,
};





/////////////////////////////////////////////////////////////////////////////
//
//                     DEBUG OBJECT BASE CLASS
//
// This is a base class for lots of other objects in the system.
//
// Histories are optionally attached to any debug object. It is limited-length
// list of audit records that are created at runtime. Anybody may add an audit
// record to a debug object. This is a large data structure, so we only
// allocate it if we are monitoring an object. Most objects of type CDebugObject
// will not allocate one of these.
/////////////////////////////////////////////////////////////////////////////

class CDebugObject {
public:
    enum DebugObjectConstants {
        // Debug Object Flags
        CHECK_STATE_ON_EVERY_OP         = 0x0001,
        SIGN_OBJECT_ON_EVERY_METHOD     = 0x0002,

        MAX_HISTORY_ENTRIES             = 75,
    };

    enum EntryType {
       CONSTRUCTOR         = 2,
       ADDREF_PROCEDURE    = 3,
       RELEASE_PROCEDURE   = 4,
       COMMENT             = 5,
    };


    CDebugObject();
    virtual ~CDebugObject();

    // Subclasses may overload this.
    virtual ErrVal CheckState();

    // Actions other objects can perform on a debug object.
    void SignObjectImpl(
                CDebugObject::EntryType tType,
                const char *pMessage,
                const char *szFileName,
                int32 lLineNum,
                int32 intValue);
    virtual void SetDebugFlags(uint32 newFlags) { m_DebugFlags |= newFlags; }

protected:
    friend class CAutoStateChecker;

    ErrVal AllocHistory();

    ///////////////////////////////////////
    // This is a single event in a history list.
    class CHistoryEntry {
    public:
       // This is what the signature records.
       EntryType       m_Type;
       const char      *m_pInfo;

       // This identifies who created this signature.
       int32           m_ThreadNum;
       const char      *m_FileName;
       int32           m_LineNum;

       // Additional recorded information for debugging. The value
       // depends on the type of signature and who signed it.
       int32           m_IntValue;
    }; // CHistoryEntry


    ///////////////////////////////////////
    class CDebugHistory {
    public:
        OSIndependantLock      m_hHistoryLock;

        int32                       m_NumItems;
        int32                       m_NextIndex;

        CDebugObject::CHistoryEntry m_Entries[MAX_HISTORY_ENTRIES];
    }; // CDebugHistory


    // These flags control debugging behavior.
    uint32              m_DebugFlags;

    // These are the historical traces of values for this object.
    CDebugHistory       *m_pHistory;
}; // CDebugObject


#if DD_DEBUG
#define SignObject(pMessage) SignObjectImpl(CDebugObject::COMMENT, pMessage, __FILE__, __LINE__, 0)
#else
#define SignObject(pMessage)
#endif




/////////////////////////////////////////////////////////////////////////////
//
//                      CONSISTENCY CHECKING
//
// This runs validity checks when we enter and leave a scope. It is
// typically used to check the state when we enter and leave a procedure.
// Really, this is just a mechanism to call the CheckState() method on
// the "this" pointer whenever we enter and leave a scope. CheckState() is
// a virtual function, so each class may override it with something different.
//
// It assumes it is being called from a method of a class that inherits
// from CDebugObject.
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
class CAutoStateChecker {
public:
    CDebugObject    *m_ob;
    CDebugFileInfo  *m_pFileInfo;

    CAutoStateChecker(CDebugObject *ob, CDebugFileInfo *pFileInfo) {
        m_ob = ob;
        m_pFileInfo = pFileInfo;

        if ((m_ob->m_DebugFlags & CDebugObject::CHECK_STATE_ON_EVERY_OP)
                || (CDebugManager::g_AlwaysDoConsistencyChecks)) {
            m_ob->CheckState();
        }
    }
    virtual ~CAutoStateChecker() {
        // Only check the state, never a pointer on exit. The
        // pointer may be deleted inside the procedure.
        if ((m_ob->m_DebugFlags & CDebugObject::CHECK_STATE_ON_EVERY_OP)
                || (CDebugManager::g_AlwaysDoConsistencyChecks)) {
            m_ob->CheckState();
        }
        m_ob = NULL;
    }
}; // CAutoStateChecker




/////////////////////////////////////////////////////////////////////////////
// This is what appears in the methods. This is called
// in a method of the CDebugObject class, so "this" refers
// to the CDebugObject object, not the autoStateChecker
// object.
//
// Consistency checks run within a method, so they assume
// any necessary lock is already acquired. To do this, you must
// get the lock before declaring the object. This works because
// C++ allows objects to be declared anywhere in the code.
//
// Be careful to only run the checks once and save the result in
// a local variables. Checks are expensive, though they shouldn't
// have side effects.
#if CONSISTENCY_CHECKS

#define RunChecks() CAutoStateChecker _checkObject(this, &g_DebugFileInfo)
#define RunChecksOnce() do { \
    if ((m_DebugFlags & CDebugObject::CHECK_STATE_ON_EVERY_OP) \
        || (CDebugManager::g_AlwaysDoConsistencyChecks)) \
    { (void) CheckState(); } \
} while (0)

////////////////////
#else

#define RunChecks()
#define RunChecksOnce()

#endif // !CONSISTENCY_CHECKS






/////////////////////////////////////////////////////////////////////////////
//
//                            LOGGING ROUTINES
//
// The DEBUG_LOG macro expands to something like this:
//    object.method(params 1)(params 2)
//
// This combines 2 sets of parameters:
//
// 1. The macros pass in the file name and line number which are
//    only available through macros, since we want the line number of the
//    log call, not the line number within the implementation of the log
//    call. The values are passed to the constructor of a temporary
//    CDebugLogObject object where they are saved as member variables.
//
// 2. The parameters to the original DEBUG_LOG call. These are passed as
//    direct parameters to the DebugLogImpl on the temporary object.
//
/////////////////////////////////////////////////////////////////////////////

class CDebugLogObject {
public:
    CDebugLogObject(
        const char *pFunctionName,
        const char *pFileName,
        int32 lineNumArg,
        CDebugFileInfo *pFileInfo,
        int32 logLevel,
        int32 logCallType) {
        m_pFunctionName = pFunctionName;
        m_pFileName = pFileName;
        m_LineNum = lineNumArg;
        m_pFileInfo = pFileInfo;
        m_LogCallLevel = logLevel;
        m_LogCallType = logCallType;
    };

    void DebugLogImpl(const char *format, ...);

private:
    void FailAssertion(const char *fileName, int32 lineNum, char *message);
    void HandleWarning(const char *message);

    CDebugFileInfo  *m_pFileInfo;
    const char      *m_pFunctionName;
    const char      *m_pFileName;
    int32           m_LineNum;
    int32           m_LogCallLevel;
    int32           m_LogCallType;
}; // CDebugLogObject


#define LOG_ALWAYS CDebugLogObject(__FUNCTION__, __FILE__, __LINE__, &g_DebugFileInfo, \
                            LOG_LEVEL_IMPORTANT, LOG_TYPE_USER_INFO).DebugLogImpl

#define DEBUG_WARNING CDebugLogObject(__FUNCTION__, __FILE__, __LINE__, &g_DebugFileInfo, \
                            LOG_LEVEL_IMPORTANT, LOG_TYPE_WARNING).DebugLogImpl

#define DEBUG_LOG CDebugLogObject(__FUNCTION__, __FILE__, __LINE__, &g_DebugFileInfo, \
                            LOG_LEVEL_DEBUG, LOG_TYPE_DEBUG).DebugLogImpl

#define DEBUG_LOG_VERBOSE CDebugLogObject(__FUNCTION__, __FILE__, __LINE__, &g_DebugFileInfo, \
                            LOG_LEVEL_VERBOSE, LOG_TYPE_DEBUG).DebugLogImpl




/////////////////////////////////////////////////////////////////////////////
//
//                             ASSERTIONS
//
/////////////////////////////////////////////////////////////////////////////

#undef ASSERT

#if DD_DEBUG
// #cond converts the expression into a string. This produces
// a string of the actual source code for the expression.
#define ASSERT(cond) { if (!(cond)) CDebugLogObject(__FUNCTION__, __FILE__, __LINE__, &g_DebugFileInfo, \
                                        LOG_LEVEL_IMPORTANT, LOG_TYPE_ASSERT).DebugLogImpl(#cond); }
#define ASSERT_UNTESTED() { CDebugManager::RunningUntestedCode(__FUNCTION__, __FILE__, __LINE__); }

#if WIN32
#define ASSERT_WIN32(cond) { if (!(cond)) CDebugLogObject(__FUNCTION__, __FILE__, __LINE__, &g_DebugFileInfo, \
                                                LOG_LEVEL_IMPORTANT, LOG_TYPE_ASSERT).DebugLogImpl(#cond); }
#elif LINUX
#define ASSERT_WIN32(cond)
#endif
#else // DD_DEBUG
#define ASSERT(cond)
#define ASSERT_WIN32(cond)
#define ASSERT_UNTESTED()
#endif // DD_DEBUG





/////////////////////////////////////////////////////////////////////////////
//
//                          ERROR HANDLING
//
// We may pass an expression into gotoErr or returnErr. These macros
// log errors, (so you get complete traces of how an error passes up
// through several layers of calls), and can do things like break into
// a debugger for serious errors.
//
// Do not evaluate the expression twice; that may have illegal side
// effects and/or be expensive. Instead, evaluate the expression
// once by assigning it to a temporary variable, and then work
// with that temporary expression.
/////////////////////////////////////////////////////////////////////////////


#if DD_DEBUG

/////////////////////////////////////////////
#define gotoErr(_errParam) do { \
    err = (_errParam); \
    if (((uint32) err) && (g_DebugFileInfo.m_FileDebugFlags & CDebugFileInfo::PRINTF_ANY_ERROR)) { OSIndependantLayer::PrintToConsole("gotoError (%d) [%s, %d]", ERROR_CODE(err), __FILE__, __LINE__); } \
    if (((uint32) err) & ERROR_IS_UNEXPECTED) { DEBUG_WARNING("Goto Error: " ERRFMT, (err & ~ERROR_IS_UNEXPECTED)); } \
    if ((err) && (g_DebugFileInfo.m_AnyErrorIsABug)) { DEBUG_WARNING("Goto Error: " ERRFMT, err); } \
    if ((ENoErr != CDebugManager::g_AdditionalPossibleBugError) && (err == CDebugManager::g_AdditionalPossibleBugError)) { DEBUG_WARNING("Goto Error: " ERRFMT, err); } \
    if (((((uint32) err) & ERROR_IS_UNEXPECTED) || ((err) && (g_DebugFileInfo.m_AnyErrorIsABug)) || ((err) && (g_DebugFileInfo.m_FileDebugFlags & CDebugFileInfo::PRINTF_ANY_ERROR)))) { DEBUG_LOG("Goto abort with error: err = " ERRFMT " (%s), line = %d", err, CDebugManager::GetErrorDescriptionString(err), __LINE__); } \
    goto abort; \
} while (0)

/////////////////////////////////////////////
#define returnErr(_errParam) do { \
    ErrVal _tempErr = (_errParam); \
    if (((uint32) _tempErr) && (g_DebugFileInfo.m_FileDebugFlags & CDebugFileInfo::PRINTF_ANY_ERROR)) { OSIndependantLayer::PrintToConsole("returnError (%d) [%s, %d]", ERROR_CODE(_tempErr), __FILE__, __LINE__); } \
    if (((uint32) _tempErr) & ERROR_IS_UNEXPECTED) { DEBUG_WARNING("Return Error: " ERRFMT, (_tempErr & ~ERROR_IS_UNEXPECTED)); } \
    if ((_tempErr) && (g_DebugFileInfo.m_AnyErrorIsABug)) { DEBUG_WARNING("Return Error: " ERRFMT, _tempErr); } \
    if ((ENoErr != CDebugManager::g_AdditionalPossibleBugError) && (_tempErr == CDebugManager::g_AdditionalPossibleBugError)) { DEBUG_WARNING("Goto Error: " ERRFMT, _tempErr); } \
    if (((((uint32) _tempErr) & ERROR_IS_UNEXPECTED) || ((_tempErr) && (g_DebugFileInfo.m_AnyErrorIsABug)) || ((_tempErr) && (g_DebugFileInfo.m_FileDebugFlags & CDebugFileInfo::PRINTF_ANY_ERROR)))) { DEBUG_LOG("Returning error: err = " ERRFMT ", (%s), line = %d", _tempErr, CDebugManager::GetErrorDescriptionString(_tempErr), __LINE__); } \
    return(_tempErr); \
} while (0)

#else // if !DD_DEBUG

/////////////////////////////////////////////
#define gotoErr(_errParam) do { err = _errParam; goto abort; } while (0)
#define returnErr(_errParam) return(_errParam)

#endif // !DD_DEBUG



#endif // _DEBUGGING_H_





