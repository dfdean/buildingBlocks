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
// Log Module
//
// This module implements an append-only log file. There may be several
// log files, including a debug log, web server logs, and more. This
// module can be used for any log file, not a single global log. The debugging
// module uses this module to implement a global debug log.
//
// This module is designed to do a few things:
//
// 1. It rotates log files and optionally enforces a max log file size.
// 2. In the future, this is a convenient place to control whether events
//    are logged locally or sent to a remote machine.
//
// Logs are UTF-8 text.
/////////////////////////////////////////////////////////////////////////////

#include "osIndependantLayer.h"
#include "stringLib.h"
#include "log.h"

static char g_TruncatedLogLine[] = "\n\r\n\rTRUNCATED\n\r\n\r";
static int32 g_TruncatedLogLineLength = 0;

static char g_FileNameSuffix[] = ".txt";
static int32 g_FileNameSuffixLength = 0;

static bool g_InitializedGlobalState = false;




/////////////////////////////////////////////////////////////////////////////
//
// [CEventLog]
//
/////////////////////////////////////////////////////////////////////////////
CEventLog::CEventLog() {
    m_Options = 0;
    m_BufferDirty = false;
    m_fBusy = false;

    m_StartBuffer = NULL;
    m_EndBuffer = NULL;
    m_NextByteInBuffer = NULL;

    m_pCurrentFilePathName = NULL;
    m_pDirectoryPathName = NULL;
    m_pFileNamePrefix = NULL;

    m_pHeaderFormatLine = NULL;
    m_pSoftwareVersionString = NULL;

    m_BufferPosInMedia = 0;

    m_cEndOfLineChar1 = '\r';
    m_cEndOfLineChar2 = '\n';

    m_MaxLogSize = UNLIMITED_LOG_MAX_SIZE;
} // CEventLog







/////////////////////////////////////////////////////////////////////////////
//
// [~CEventLog]
//
/////////////////////////////////////////////////////////////////////////////
CEventLog::~CEventLog() {
    if (NULL != m_StartBuffer) {
        delete m_StartBuffer;
        m_StartBuffer = NULL;
    }
    if (NULL != m_pCurrentFilePathName) {
        delete m_pCurrentFilePathName;
        m_pCurrentFilePathName = NULL;
    }
    if (NULL != m_pDirectoryPathName) {
        delete m_pDirectoryPathName;
        m_pDirectoryPathName = NULL;
    }
    if (NULL != m_pFileNamePrefix) {
        delete m_pFileNamePrefix;
        m_pFileNamePrefix = NULL;
    }
    if (m_pHeaderFormatLine) {
        delete m_pHeaderFormatLine;
        m_pHeaderFormatLine = NULL;
    }
    if (m_pSoftwareVersionString) {
        delete m_pSoftwareVersionString;
        m_pSoftwareVersionString = NULL;
    }
} // ~CEventLog






/////////////////////////////////////////////////////////////////////////////
//
// [Initialize]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CEventLog::Initialize(
                char *pBasicPathName,
                char *pLogFilePathName,
                int32 bufferSize,
                int32 initialOptions,
                int32 maxLogSize,
                CProductInfo *pVersion,
                const char *pEntryFormatDescription) {
    ErrVal err = ENoErr;
    int32 length;
    char *pFileName;
    char tempBuffer[2048];
    
    if ((NULL == pBasicPathName) || (NULL == pLogFilePathName) || (NULL == pVersion)) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
        goto abort;
    }

    if (bufferSize <= 0) {
        bufferSize = DEFAULT_LOG_BUFFER_SIZE;
    }

    // Initialize some global state the first time we run.
    if (!g_InitializedGlobalState) {
        g_TruncatedLogLineLength = strlen(g_TruncatedLogLine);
        g_FileNameSuffixLength = strlen(g_FileNameSuffix);

        g_InitializedGlobalState = true;
    }

    // Make sure we can at least write a header line.
    if (bufferSize < g_TruncatedLogLineLength) {
        bufferSize = g_TruncatedLogLineLength;
    }
    if (bufferSize < MIN_BUFFER_LENGTH) {
        bufferSize = MIN_BUFFER_LENGTH;
    }


    // Initialize the runtime state.
    m_Options = initialOptions;
    m_MaxLogSize = maxLogSize;

    m_cEndOfLineChar1 = '\r';
    m_cEndOfLineChar2 = '\n';

    // Allocate a new buffer.
    if (NULL != m_StartBuffer) {
        delete m_StartBuffer;
        m_StartBuffer = NULL;
    }
    // Add a character size to the buffer. This lets us write
    // buffers of bufferSize before detecting that it is time to write.
    // The bufferSize is often something efficient, like 4096,
    // which is a multiple of page and sector size. Writing aligned
    // buffers to disk is more efficient, so we want to
    // write when we reach bufferSize, not something just under
    // bufferSize, like 4093.
    m_StartBuffer = new char[bufferSize + MAX_CHARACTER_SIZE];
    if (NULL == m_StartBuffer) {
        err = EFail;
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }
    m_EndBuffer = m_StartBuffer + bufferSize;

    // Initially, there are no bytes in the buffer.
    m_NextByteInBuffer = m_StartBuffer;
    m_BufferDirty = false;
    m_fBusy = false;


    // Make a copy of the directory path.
    length = strlen(pBasicPathName);
    // This is + 2 so we have space to add a separator char if we need to.
    m_pDirectoryPathName = new char[length + 2];
    if (NULL == m_pDirectoryPathName) {
        err = EFail;
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }
    strncpyex(m_pDirectoryPathName, pBasicPathName, length);
    // Make sure the path ends with a directory separator char.
    // We added padding space to add a char if we need to.
    if (!(IS_DIRECTORY_SEPARATOR(m_pDirectoryPathName[length - 1]))) {
        m_pDirectoryPathName[length] = DIRECTORY_SEPARATOR_CHAR;
        m_pDirectoryPathName[length + 1] = 0;
    }

    // Make sure the log directory exists.
    if (!(CSimpleFile::DirectoryExists(m_pDirectoryPathName))) {
        err = CSimpleFile::CreateDirectory(m_pDirectoryPathName);
        if (err) {
            REPORT_LOW_LEVEL_BUG();
            goto abort;
        }
    }


    // Strip off the file name from the directory path.
    pFileName = CSimpleFile::GetFileNameFromPath(pLogFilePathName);

    // Make a copy of just the file name, without the directory.
    length = strlen(pFileName);
    m_pFileNamePrefix = new char[length + 1];
    if (NULL == m_pFileNamePrefix) {
        err = EFail;
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }
    strncpyex(m_pFileNamePrefix, pFileName, length);

    // Strip the file suffix from the file name. We will add
    // our own file suffix when we create file names. This
    // gives us a place to insert additional text into the file name,
    // like the current date.
    pFileName = m_pFileNamePrefix;
    while (*pFileName) {
        if ('.' == *pFileName) {
            *pFileName = 0;
            break;
        }
        pFileName++;
    }

    // Initialize the log lock.
    err = m_LogLock.Initialize();
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }


    // Make a copy of the format line and software version.
    // We need these each time we rotate a log file.
    if (NULL != pEntryFormatDescription) {
        length = strlen(pEntryFormatDescription);
        m_pHeaderFormatLine = new char[length + 1];
        if (NULL == m_pHeaderFormatLine) {
            err = EFail;
            REPORT_LOW_LEVEL_BUG();
            goto abort;
        }
        strncpyex(m_pHeaderFormatLine, pEntryFormatDescription, length);
    }

    err = pVersion->PrintToString(
                        tempBuffer,
                        sizeof(tempBuffer),
                        CProductInfo::PRINT_SOFTWARE_NAME
                            | CProductInfo::PRINT_RELEASE_TYPE
                            | CProductInfo::PRINT_BUILD_NUMBER,
                        NULL);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    length = strlen(tempBuffer);
    m_pSoftwareVersionString = new char[length + 1];
    if (NULL == m_pSoftwareVersionString) {
        err = EFail;
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }
    strncpyex(m_pSoftwareVersionString, tempBuffer, length);


    // Discard old logs.
    if (m_Options & DELETE_OLD_LOGS) {
        DeleteOldLogs();
    }

    // Check if we are initializing a new log file or opening an
    // existing file.
    if (m_Options & INITIALIZE_LOG) {
        // This initializes the log file itself.
        err = InitLogFile();
        if (err) {
            REPORT_LOW_LEVEL_BUG();
            goto abort;
        }
    } // initializing the file.
    // Otherwise, the log describes an existing file.
    else {
        // This will advance m_BufferPosInMedia.
        err = OpenExistingLogFile();
        if (err) {
            REPORT_LOW_LEVEL_BUG();
            goto abort;
        }
    } // opening an existing log file.

    //printf("\nLog File: %s\n", m_pCurrentFilePathName);

abort:
    return(err);
} // Initialize.






/////////////////////////////////////////////////////////////////////////////
//
// [DeleteOldLogs]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CEventLog::DeleteOldLogs() {
    ErrVal err = ENoErr;
    CDirFileList fileIter;
    char oldDEBUG_LOGileName[256];
    char *pFileName;
    int32 prefixLength;

    prefixLength = strlen(m_pFileNamePrefix);

    err = fileIter.Open(m_pDirectoryPathName);
    if (err) {
        OSIndependantLayer::PrintToConsole(
                               "CEventLog::DeleteOldLogs. Error for dir: %s", 
                               m_pDirectoryPathName);
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    while (fileIter.GetNextFile(oldDEBUG_LOGileName, sizeof(oldDEBUG_LOGileName))) {
        pFileName = CSimpleFile::GetFileNameFromPath(oldDEBUG_LOGileName);
        if (0 == strncasecmpex(pFileName, m_pFileNamePrefix, prefixLength)) {
            CSimpleFile::DeleteFile(oldDEBUG_LOGileName);
        }
    }

abort:
    return(err);
} // DeleteOldLogs.





/////////////////////////////////////////////////////////////////////////////
//
// [CreateLogFileName]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CEventLog::CreateLogFileName(CDateTime *pNow) {
    ErrVal err = ENoErr;
    int32 length;
    char *pDestPtr;
    char *pEndDestPtr;
    char *pStartFileSuffix;
    int32 uniqueId = 0;


    // Discard any previous file name.
    if (NULL != m_pCurrentFilePathName) {
        delete m_pCurrentFilePathName;
        m_pCurrentFilePathName = NULL;
    }

    // Allocate a buffer to print the new name into.
    length = strlen(m_pDirectoryPathName)
                + strlen(m_pFileNamePrefix)
                + g_FileNameSuffixLength
                + 60;

    m_pCurrentFilePathName = new char[length + 1];
    if (NULL == m_pCurrentFilePathName) {
        err = EFail;
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    // Start with the directory path and the file name prefix.
    pDestPtr = m_pCurrentFilePathName;
    pEndDestPtr = m_pCurrentFilePathName + length;
    pDestPtr += snprintf(
                    pDestPtr,
                    pEndDestPtr - pDestPtr,
                    "%s%s",
                    m_pDirectoryPathName,
                    m_pFileNamePrefix);

    if ((m_Options & APPEND_DATE_TO_FILE_NAME)
        && (NULL != pNow)) {
        pNow->PrintToString(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        CDateTime::FILE_NAME_FORMAT,
                        &pDestPtr);
    }

    // Do NOT advance pDestPtr. We want the pointer to remain at
    // the same place in the fileName in case we have to add a unique
    // suffix.
    pStartFileSuffix = pDestPtr;
    (void) snprintf(
                pStartFileSuffix,
                pEndDestPtr - pStartFileSuffix,
                "%s",
                g_FileNameSuffix);

    // If we are creating a new log, then make sure the
    // filename is unique.
    if (m_Options & ALWAYS_CREATE_NEW_FILE) {
        while (true) {
            if (!(CSimpleFile::FileExists(m_pCurrentFilePathName))) {
                break;
            }

            uniqueId += 1;
            snprintf(
                pStartFileSuffix,
                pEndDestPtr - pStartFileSuffix,
                "_%d%s",
                uniqueId,
                g_FileNameSuffix);
        } // while (true)
    } // if (m_Options & ALWAYS_CREATE_NEW_FILE)

abort:
    return(err);
} // CreateLogFileName.






/////////////////////////////////////////////////////////////////////////////
//
// [InitLogFile]
//
// Write an Extended File Format header.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CEventLog::InitLogFile() {
    ErrVal err = ENoErr;
    char tempBuf[1024];
    char *pDestPtr;
    char *pEndDestPtr;
    CDateTime now;
    CSimpleFile file;

    now.GetLocalDateAndTime();

    err = CreateLogFileName(&now);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    ///////////////////////
    // Create an empty file.
    err = file.OpenOrCreateEmptyFile(m_pCurrentFilePathName, 0);
    if (err) {
        OSIndependantLayer::PrintToConsole(
                               "CEventLog::InitLogFile. Error for file: %s", 
                               m_pCurrentFilePathName);
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }
    file.Close();

    m_BufferPosInMedia = 0;


    ///////////////////////
    // Write the log header.

    // This is the name of the program, including the program version.
    snprintf(tempBuf, sizeof(tempBuf), "#Software: %s", m_pSoftwareVersionString);
    err = LogMessage(tempBuf);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

#if 0
    // This is the version of the W3C extended log file format.
    // The current draft defines version 1.0.
    snprintf(tempBuf, sizeof(tempBuf), "#Version: 1.0");
    err = LogMessage(tempBuf);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }
#endif

    // Print the date and time.
    // Even if its in the log name, it should be in the log itself.
    // Ideally, each log line won't include the whole date.
    pDestPtr = tempBuf;
    pEndDestPtr = tempBuf + sizeof(tempBuf);
    pDestPtr += snprintf(pDestPtr, pEndDestPtr - pDestPtr, "#Date: ");
    now.PrintToString(
                    pDestPtr,
                    pEndDestPtr - pDestPtr,
                    CDateTime::W3C_LOG_FORMAT,
                    &pDestPtr);
    err = LogMessage(tempBuf);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    // Print the date with seconds (in a more readable way).
    pDestPtr = tempBuf;
    pEndDestPtr = tempBuf + sizeof(tempBuf);
    pDestPtr += snprintf(pDestPtr, pEndDestPtr - pDestPtr, "#XDate: ");
    now.PrintToString(
                    pDestPtr,
                    pEndDestPtr - pDestPtr,
                    0,
                    &pDestPtr);
    err = LogMessage(tempBuf);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    if (NULL != m_pHeaderFormatLine) {
        snprintf(tempBuf, sizeof(tempBuf), "#Fields: %s", m_pHeaderFormatLine);
        err = LogMessage(tempBuf);
        if (err) {
            REPORT_LOW_LEVEL_BUG();
            goto abort;
        }
    }

    err = Flush();
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

abort:
    return(err);
} // InitLogFile






/////////////////////////////////////////////////////////////////////////////
//
// [OpenExistingLogFile]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CEventLog::OpenExistingLogFile() {
    ErrVal err = ENoErr;
    CSimpleFile file;

    if ((NULL == m_StartBuffer) || (NULL == m_EndBuffer)) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }

    err = CreateLogFileName(NULL);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    err = file.OpenExistingFile(m_pCurrentFilePathName, 0);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    // Find where we start writing in the log.
    err = file.GetFileLength(&m_BufferPosInMedia);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

abort:
    file.Close();

    return(err);
} // OpenExistingLogFile.






/////////////////////////////////////////////////////////////////////////////
//
// [Flush]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CEventLog::Flush() {
    ErrVal err = ENoErr;
    CSimpleFile file;
    int32 numBytesToWrite;
    char *ptr;
    int32 numAttempts;
    int32 maxAttempts = 10;

    m_LogLock.BasicLock();

    if ((NULL == m_NextByteInBuffer)
        || (NULL == m_StartBuffer)) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
        goto abort;
    }

    // If there is no un-saved data to flush, then there is nothing to
    // flush so we are done.
    // This is NOT an error.
    if (!m_BufferDirty) {
        err = ENoErr;
        goto abort;
    }

    // Write the whole buffer at once. We keep the buffer in memory
    // in the same format as it is stored in the file.
    numBytesToWrite = m_NextByteInBuffer - m_StartBuffer;

    // If the file is too big, then truncate it. Truncating at
    // m_BufferPosInMedia will discard the old end-of-log marker and
    // any old log entries after it.
    if ((m_MaxLogSize > 0)
        && (m_BufferPosInMedia > 0)
        && ((m_BufferPosInMedia + numBytesToWrite) > (uint64) m_MaxLogSize)) {
        numBytesToWrite = (int32) (m_MaxLogSize - m_BufferPosInMedia);
        if (numBytesToWrite < 0) {
            numBytesToWrite = 0;
        }

        if (numBytesToWrite >= g_TruncatedLogLineLength) {
            ptr = m_StartBuffer + numBytesToWrite - g_TruncatedLogLineLength;
            strncpyex(ptr, g_TruncatedLogLine, g_TruncatedLogLineLength);
        } else {
            ptr = m_StartBuffer;
            strncpyex(ptr, g_TruncatedLogLine, g_TruncatedLogLineLength);

            numBytesToWrite = g_TruncatedLogLineLength;
            m_BufferPosInMedia = m_MaxLogSize - numBytesToWrite;
        }
    }

    // We may be sharing the log file with several threads.
    // Don't freak out too much if we cannot open right away.
    numAttempts = 0;
    while (1) {
        err = file.OpenOrCreateFile(m_pCurrentFilePathName, 0);
        if (!err) {
            break;
        }
        if (numAttempts > maxAttempts) {
            goto abort;
        }
        numAttempts += 1;
        OSIndependantLayer::SleepForMilliSecs(100);
    }

    err = file.Seek(m_BufferPosInMedia, CSimpleFile::SEEK_START);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    err = file.Write(m_StartBuffer, numBytesToWrite);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    // Don't call file.Flush().
    // This seems to confuse synchronous writes on Win32.
    // file.Flush();

    m_BufferPosInMedia += numBytesToWrite;

abort:
    m_NextByteInBuffer = m_StartBuffer;
    m_BufferDirty = false;

    m_LogLock.BasicUnlock();

    // Keep the file closed when we are not writing to it. This allows
    // monitor applications to share and read the log file while another
    // program runs and writes to the log.
    file.Close();

    return(err);
} // Flush.







/////////////////////////////////////////////////////////////////////////////
//
// [LogMessage]
//
// This is the main procedure for all value recording.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CEventLog::LogMessage(char *valBuffer) {
    ErrVal err = ENoErr;

    m_LogLock.BasicLock();

    if ((NULL == valBuffer)
        || (NULL == m_NextByteInBuffer)
        || (NULL == m_EndBuffer)) {
        err = EFail;
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    // If we have a problem flushing, then it could easily try to
    // log an error message. Re-entrant error messages will cause errors
    // so avoid them.
    if (m_fBusy) {
        err = ENoErr;
        goto abort;
    }
    m_fBusy = true;

    // Now that we know this is the buffer we will be writing to,
    // record that we have written to it.
    m_BufferDirty = true;

    // Now, at this point we know there is enough room in the buffer
    // for the value. Copy the value to the buffer. We save it in
    // the buffer in the same format as it will appear in the file.
    while (*valBuffer) {
        // Now, check if the buffer has enough space to hold the new value.
        if (m_NextByteInBuffer >= m_EndBuffer) {
            err = Flush();
            if (err) {
                REPORT_LOW_LEVEL_BUG();
                goto abort;
            }
        } // flushing.

        // Expand special characters.
        if ('\n' == *valBuffer) {
            *(m_NextByteInBuffer++) = '\\';
            *(m_NextByteInBuffer++) = 'n';
        } else if ('\r' == *valBuffer) {
            *(m_NextByteInBuffer++) = '\\';
            *(m_NextByteInBuffer++) = 'r';
        } else if ('\t' == *valBuffer) {
            *(m_NextByteInBuffer++) = '\\';
            *(m_NextByteInBuffer++) = 't';
        } else {
            *(m_NextByteInBuffer++) = *valBuffer;
        }

        valBuffer++;
    } // Print the log message.

    // Now, check if the buffer has enough space to hold the end-of-line.
    // Add an extra space so we can make this a C-string and print it if needed.
    if ((m_NextByteInBuffer + 3) >= m_EndBuffer) {
        err = Flush();
        if (err) {
            goto abort;
        }
    } // flushing.

    *(m_NextByteInBuffer++) = m_cEndOfLineChar1;
    *(m_NextByteInBuffer++) = m_cEndOfLineChar2;

    if (m_Options & ALWAYS_FLUSH) {
        err = Flush();
        if (err) {
            REPORT_LOW_LEVEL_BUG();
            goto abort;
        }
    }

abort:
    m_fBusy = false;
    m_LogLock.BasicUnlock();

    return(ENoErr);
} // LogMessage.






/////////////////////////////////////////////////////////////////////////////
//
// [GetRecenLogEntries]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CEventLog::GetRecenLogEntries(
                   char *pDestBuffer, 
                   int32 maxSize, 
                   int32 *pActualSize) {
    ErrVal err = ENoErr;
    int32 numBytesInBuffer;
    int32 numBytesToCopy;
    int32 totalBytesCopied;
    int32 numAttempts;
    int32 maxAttempts = 10;
    char *pDestPtr;
    char *pSrcPtr;
    int32 numBytesToCopyFromBuffer;
    int32 numBytesToCopyFromFile;
    CSimpleFile file;


    m_LogLock.BasicLock();

    if ((NULL == pDestBuffer)
        || (maxSize < 0)
        || (NULL == pActualSize)
        || (NULL == m_NextByteInBuffer)
        || (NULL == m_EndBuffer)) {
        err = EFail;
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }
    //char pEndDestBuffer = pDestBuffer + maxSize;
    *pActualSize = 0;
    totalBytesCopied = 0;

    if (m_fBusy) {
        err = ENoErr;
        goto abort;
    }
    m_fBusy = true;

 
    // Save the entire buffer, or as much of it as we can.
    numBytesInBuffer = (int32) (m_NextByteInBuffer - m_StartBuffer);
    numBytesToCopy = (int32) (m_BufferPosInMedia + numBytesInBuffer);
    if (numBytesToCopy > maxSize) {
        numBytesToCopy = maxSize;
    }
    numBytesToCopyFromBuffer = numBytesInBuffer;
    if (numBytesToCopyFromBuffer >= numBytesToCopy) {
        numBytesToCopyFromBuffer = numBytesToCopy;
    }
    numBytesToCopyFromFile = numBytesToCopy - numBytesToCopyFromBuffer;

    // If all the data will fit into the buffer, then start at the beginning of the buffer.
    // Otherwise, write at the end of the buffer and we will fill the start 
    // of the buffer from a file.
    pDestPtr = pDestBuffer + numBytesToCopyFromFile;

    pSrcPtr = m_NextByteInBuffer - numBytesToCopyFromBuffer;
    memcpy(pDestPtr, pSrcPtr, numBytesToCopyFromBuffer);
    totalBytesCopied = numBytesToCopyFromBuffer;



    // If we didn't fill the buffer, then read more from the last file.
    if ((totalBytesCopied < numBytesToCopy) && (m_BufferPosInMedia > 0)) {
        numBytesToCopy = (int32) m_BufferPosInMedia;
        if ((totalBytesCopied + numBytesToCopy) > maxSize) {
            numBytesToCopy = maxSize - totalBytesCopied;
        }
        //printf("\n** GetRecenLogEntries. File Copy. numBytesToCopy=%d\n", numBytesToCopy);

        // We may be sharing the log file with several threads.
        // Don't freak out too much if we cannot open right away.
        numAttempts = 0;
        while (1) {
            err = file.OpenExistingFile(m_pCurrentFilePathName, 0);
            if (!err) {
                break;
            }
            if (numAttempts > maxAttempts) {
                goto abort;
            }
            numAttempts += 1;
            OSIndependantLayer::SleepForMilliSecs(100);
        }

        err = file.Seek((m_BufferPosInMedia - numBytesToCopy), CSimpleFile::SEEK_START);
        if (err) {
            REPORT_LOW_LEVEL_BUG();
            goto abort;
        }

        pDestPtr = pDestBuffer;
        err = file.Read(pDestPtr, numBytesToCopy, &numBytesToCopy);
        if (err) {
            REPORT_LOW_LEVEL_BUG();
            goto abort;
        }

        totalBytesCopied += numBytesToCopy;
    } // if (totalBytesCopied < maxSize)


    if (pActualSize) {
        *pActualSize = totalBytesCopied;
    }

abort:
    file.Close();
    m_fBusy = false;
    m_LogLock.BasicUnlock();

    return(ENoErr);
} // GetRecenLogEntries





/////////////////////////////////////////////////////////////////////////////
//
// [ResetLog]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CEventLog::ResetLog() {
    ErrVal err = ENoErr;

    m_LogLock.BasicLock();

    DeleteOldLogs();

    // Initially, there are no bytes in the buffer.
    m_NextByteInBuffer = m_StartBuffer;
    m_BufferDirty = false;
    m_fBusy = false;

    // This initializes the log file itself.
    err = InitLogFile();
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

abort:
    m_fBusy = false;
    m_LogLock.BasicUnlock();

    return(ENoErr);
} // ResetLog





/////////////////////////////////////////////////////////////////////////////
//
//                       TESTING PROCEDURES
//
// Log testing is a little special. Every other module uses the log
// to test. The log, however, does not use the standard test package.
// It simply returns an error code indicating whether it passed its
// tests or not.
/////////////////////////////////////////////////////////////////////////////
#if INCLUDE_REGRESSION_TESTS

#define NUM_TEST_TUPLES 400
#define NUM_TEST_VALUES 5
#define MAX_LOG_NAME_SIZE 28

static char valNames[NUM_TEST_VALUES][MAX_LOG_NAME_SIZE];
static int32 valueBase = 0;
static CEventLog g_TestLog;

static char testLogDirName[2048];
static char testLogFileName[2048];
static ErrVal TestWriteLog(int8 ignoreErrors);

#pragma GCC diagnostic ignored "-Wformat-truncation"



/////////////////////////////////////////////////////////////////////////////
//
// [TestLog]
//
/////////////////////////////////////////////////////////////////////////////
void
CEventLog::TestLog() {
    ErrVal err = ENoErr;
    int32 valueNum;

    snprintf(testLogDirName, 
             sizeof(testLogDirName), 
             "%s%c%s%c",
             g_SoftwareDirectoryRoot,
             DIRECTORY_SEPARATOR_CHAR,
             "testData",
             DIRECTORY_SEPARATOR_CHAR);

    snprintf(testLogFileName, 
             sizeof(testLogFileName), 
             "logValueTestLog.txt");

    OSIndependantLayer::PrintToConsole("Test Module: Logging");

    for (valueNum = 0; valueNum < NUM_TEST_VALUES; valueNum++) {
        snprintf(
            &(valNames[valueNum][0]),
            MAX_LOG_NAME_SIZE,
            "valueName%d",
            valueNum);
    }


    OSIndependantLayer::PrintToConsole("  Test: Basic logging.");

    err = g_TestLog.Initialize(
                  testLogDirName,
                  testLogFileName,
                  CEventLog::DEFAULT_LOG_BUFFER_SIZE,
                  CEventLog::INITIALIZE_LOG,
                  CEventLog::UNLIMITED_LOG_MAX_SIZE,
                  &g_SoftwareVersion,
                  "   ");
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        OSIndependantLayer::PrintToConsole("Error from Initialize");
        return;
    }

    valueBase = 48;
    err = TestWriteLog(false);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        OSIndependantLayer::PrintToConsole("Error from TestWriteLog");
        return;
    }


    OSIndependantLayer::PrintToConsole("  Test: Small log data buffer.");

    err = g_TestLog.Initialize(
                  testLogDirName,
                  testLogFileName,
                  500,
                  CEventLog::INITIALIZE_LOG,
                  CEventLog::UNLIMITED_LOG_MAX_SIZE,
                   &g_SoftwareVersion,
                  "   ");
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        OSIndependantLayer::PrintToConsole("Error from Initialize");
        return;
    }

    valueBase = 96;
    err = TestWriteLog(false);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        OSIndependantLayer::PrintToConsole("Error from TestWriteLog");
        return;
    }



    OSIndependantLayer::PrintToConsole("  Test: Small log max size.");

    err = g_TestLog.Initialize(
                  testLogDirName,
                  testLogFileName,
                  400,
                  CEventLog::INITIALIZE_LOG,
                  2000,
                  &g_SoftwareVersion,
                  "   ");
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        OSIndependantLayer::PrintToConsole("Error Initialize");
        return;
    }

    valueBase = 98;
    err = TestWriteLog(false);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        OSIndependantLayer::PrintToConsole("Error TestWriteLog");
        return;
    }
} // TestLog





/////////////////////////////////////////////////////////////////////////////
//
// [TestWriteLog]
//
/////////////////////////////////////////////////////////////////////////////
static ErrVal
TestWriteLog(int8 ignoreErrors) {
    ErrVal err = ENoErr;
    int32 tupleNum;
    int32 valueNum;
    char valBuffer[1024];
    char *pDestPtr;
    char *pEndBuffer;


    pEndBuffer = valBuffer + sizeof(valBuffer);
    for (tupleNum = 0; tupleNum < NUM_TEST_TUPLES; tupleNum++) {
        pDestPtr = valBuffer;
        for (valueNum = 0; valueNum < NUM_TEST_VALUES; valueNum++) {
            pDestPtr += snprintf(
                            pDestPtr,
                            pEndBuffer - pDestPtr,
                            "%s = %d ",
                            &(valNames[valueNum][0]),
                            valueNum + valueBase);
        }

        err = g_TestLog.LogMessage(valBuffer);
        if ((err) && (!ignoreErrors)) {
            REPORT_LOW_LEVEL_BUG();
            return(err);
        }

        valueBase += 1;
    }

    err = g_TestLog.Flush();
    if ((err) && (!ignoreErrors)) {
        REPORT_LOW_LEVEL_BUG();
        return(err);
    }

    return(err);
} // TestWriteLog.

#endif // INCLUDE_REGRESSION_TESTS

