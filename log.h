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

#ifndef _EVENT_LOG_H_
#define _EVENT_LOG_H_



/////////////////////////////////////////////////////////////////////////////
// This is the entire runtime state for log recording.
class CEventLog
{
public:
    // These are options for the runtime recording state.
    enum LogConstants
    {
        ALWAYS_FLUSH                    = 0x01,
        INITIALIZE_LOG                  = 0x02,
        DELETE_OLD_LOGS                 = 0x04,
        APPEND_DATE_TO_FILE_NAME        = 0x08,
        ALWAYS_CREATE_NEW_FILE          = 0x10,

        // These are the sizes of the buffers for the log state.
        DEFAULT_LOG_BUFFER_SIZE         = 4096,
        UNLIMITED_LOG_MAX_SIZE          = -1,
        MIN_BUFFER_LENGTH               = 256,
        MAX_CHARACTER_SIZE              = 4,
    };

#if INCLUDE_REGRESSION_TESTS
    static void TestLog();
#endif

    CEventLog();
    virtual ~CEventLog();

    ErrVal Initialize(
                char *plogDirName,
                char *logFileName,
                int32 bufferSize,
                int32 initialOptions,
                int32 maxLogSize,
                CProductInfo *pVersion,
                const char *szEntryFormatDescription);

    ErrVal LogMessage(char *valBuffer);
    ErrVal GetRecenLogEntries(
                   char *pDestBuffer, 
                   int32 maxSize, 
                   int32 *pActualSize);
    ErrVal ResetLog();

    ErrVal Flush();

private:
    ErrVal OpenExistingLogFile();
    ErrVal InitLogFile();

    ErrVal DeleteOldLogs();
    ErrVal CreateLogFileName(CDateTime *pNow);

    // Runtime state of the log module.
    int32               m_Options;
    bool                m_BufferDirty;
    bool                m_fBusy;
    OSIndependantLock   m_LogLock;

    // These point into the current buffer of data.
    char                *m_StartBuffer;
    char                *m_EndBuffer;
    char                *m_NextByteInBuffer;

    // The files holding previously saved log data.
    char                *m_pDirectoryPathName;
    char                *m_pFileNamePrefix;
    char                *m_pCurrentFilePathName;
    uint64              m_BufferPosInMedia;

    // These are standard strings we use when creating log entries.
    char                m_cEndOfLineChar1;
    char                m_cEndOfLineChar2;
    char                *m_pHeaderFormatLine;
    char                *m_pSoftwareVersionString;

    // Configuration values controlling this log.
    int32               m_MaxLogSize;
}; // CEventLog


#endif // _EVENT_LOG_H_





