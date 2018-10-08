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
// Config Library
//
// This stores user configuration values, so it provides a simple database
// of strings, integers and booleans that are used to control various features
// throughout the product.
//
// This stores the data in a simple text file whose syntax resembles XML.
// Note, however, this is not complete XML, it's just designed to provide the
// minimal file syntax for a config file. It is case insensitive, and doesn't
// parse more complicated features of XML. There is a more complete XML engine
// higher up in the code that is built on top of the building blocks. Second,
// this module only supports UTF8/ASCII files, not UTF16 or other alphabets.
//
/////////////////////////////////////////////////////////////////////////////

#include "osIndependantLayer.h"
#include "stringLib.h"
#include "config.h"

#if LINUX
#include <unistd.h>
#endif

CConfigFile *g_Config = NULL;

static char g_FinishedFileSuffix[] = ".done";
static char g_ProgressFileSuffix[] = ".tmp";

static char g_StartOpenConfigElement[] = "<config";
static int32 g_StartOpenConfigElementLength = -1;

static char g_OpenConfigElement[] = "<config version=\"1\">";

static char g_CloseConfigElement[] = "</config>";
static int32 g_CloseConfigElementLength = -1;

static char g_CloseGroupElement[] = "</group>";
static int32 g_CloseGroupElementLength = -1;

static char g_CloseValueElement[] = "</value>";
static int32 g_CloseValueElementLength = -1;

static char g_GroupElementName[] = "group";
static int32 g_GroupElementNameLength = -1;

static char g_ValueElementName[] = "value";
static int32 g_ValueElementNameLength = -1;

static char g_NameAttribute[] = "name";
static int32 g_NameAttributeLength = -1;

static char g_IndentStr[] = "   ";
static int32 g_IndentStrLength = 3;

char g_InstallationDirVariableName[] = "%InstallDir%";
int32 g_InstallationDirVariableNameLength = 12;

static char g_OpenElementChar = '<';
static char g_CloseElementChar = '>';


#if WIN32
static HKEY g_InstallRegistrySection = HKEY_LOCAL_MACHINE;
static char *g_DefaultInstallRegistryKeyName = "SOFTWARE";
#endif

#if LINUX
#define DEFAULT_CONFIG_PATH_PREFIX    "/home/ddean"
#define CONFIG_FILE_NAME              "config.txt"
#endif

#define WRITE_BUFFER_SIZE        8192

#define WHITE_SPACE(p) ((' ' == (p)) || ('\t' == (p)) || ('\r' == (p)) || ('\n' == (p)))
#define QUOTE_CHAR(p) (('\"' == (p)) || ('\'' == (p)))




/////////////////////////////////////////////////////////////////////////////
//
// [CConfigFile]
//
/////////////////////////////////////////////////////////////////////////////
CConfigFile::CConfigFile() {
    ErrVal err = ENoErr;

    g_StartOpenConfigElementLength = strlen(g_StartOpenConfigElement);
    g_CloseConfigElementLength = strlen(g_CloseConfigElement);
    g_CloseGroupElementLength = strlen(g_CloseGroupElement);
    g_CloseValueElementLength = strlen(g_CloseValueElement);
    g_GroupElementNameLength = strlen(g_GroupElementName);
    g_ValueElementNameLength = strlen(g_ValueElementName);
    g_NameAttributeLength = strlen(g_NameAttribute);
    g_IndentStrLength = strlen(g_IndentStr);
    g_InstallationDirVariableNameLength = strlen(g_InstallationDirVariableName);

    m_ConfigFlags = 0;

    m_pInstallDirName = NULL;
    m_pFileName = NULL;
    m_pFinishedFileName = NULL;
    m_pInProgressFileName = NULL;

    m_pFirstSection = NULL;
    m_pLastSection = NULL;

    err = m_OSLock.Initialize();
    if (err) {
        REPORT_LOW_LEVEL_BUG();
    }
} // CConfigFile.







/////////////////////////////////////////////////////////////////////////////
//
// [~CConfigFile]
//
/////////////////////////////////////////////////////////////////////////////
CConfigFile::~CConfigFile() {
    CConfigSection *pNextKey;

    m_OSLock.BasicLock();

    while (m_pFirstSection) {
        pNextKey = m_pFirstSection->m_pNextSection;
        delete m_pFirstSection;
        m_pFirstSection = pNextKey;
    }
    m_pFirstSection = NULL;
    m_pLastSection = NULL;

    m_OSLock.BasicUnlock();


    if (NULL != m_pFileName) {
        delete m_pFileName;
        m_pFileName = NULL;
    }

    if (NULL != m_pFinishedFileName) {
        delete m_pFinishedFileName;
        m_pFinishedFileName = NULL;
    }

    if (NULL != m_pInstallDirName) {
        delete m_pInstallDirName;
        m_pInstallDirName = NULL;
    }

    if (NULL != m_pInProgressFileName) {
       delete m_pInProgressFileName;
       m_pInProgressFileName = NULL;
    }


    m_OSLock.Shutdown();
} // ~CConfigFile.







/////////////////////////////////////////////////////////////////////////////
//
// [ReadProductConfig]
//
/////////////////////////////////////////////////////////////////////////////
void
CConfigFile::ReadProductConfig(CProductInfo *pProductInfo) {
#if WIN32
    ErrVal err = ENoErr;
    int32 length;
    LONG lResult;
    HKEY hAllProductsKey = NULL;
    HKEY hCompanyKey = NULL;
    DWORD valueType;
    DWORD dwDataLength;
    DWORD dwErr = 0;
#endif
    char pathName[2000];

    if (NULL == pProductInfo) {
       goto abort;
    }
    if (pProductInfo->m_pConfigFile) {
        strncpyex(pathName, pProductInfo->m_pConfigFile, sizeof(pathName));
        goto foundFile;
    }

#if WIN32
    /////////////////////////////////////////////////////////////////////////
    // WINDOWS
    lResult = RegOpenKeyExA(
                  g_InstallRegistrySection,
                  g_DefaultInstallRegistryKeyName,
                  0,
                  KEY_READ | KEY_QUERY_VALUE,
                  &hAllProductsKey);
    if (ERROR_SUCCESS == lResult) {
        lResult = RegOpenKeyExA(
                      hAllProductsKey,
                      pProductInfo->m_CompanyName,
                      0,
                      KEY_READ | KEY_QUERY_VALUE,
                      &hCompanyKey);
    }

    if (ERROR_SUCCESS != lResult) {
        // The registry has separate stores for 32 and 64 bit applications.
        // We default to the area of the registry that matches the binary 
        // of this application, so depending on whether this is a 32 or 64 bit 
        // build, it will default to different parts of the registry. 
        // However, the installer may put the registry information in a 
        // different part of the registry. For example, a 64 bit installer
        // will register the keys in the 64 bit registry, but a 32-bit build
        // of this program will default to the 32-bit section. We really don't 
        // care. The registry is just used to/ give us the pathname of the
        // real config file, which we define and doesn't make a distinction 
        // for 32/64 bit programs. So, if we cannot find the registry key we want,
        // then explicitly look in the different subsections.
        // First, try the 64-bit registry.
        lResult = RegOpenKeyExA(
                      g_InstallRegistrySection,
                      g_DefaultInstallRegistryKeyName,
                      0,
                      KEY_READ | KEY_QUERY_VALUE | KEY_WOW64_64KEY,
                      &hAllProductsKey);
        if (ERROR_SUCCESS == lResult) {
            lResult = RegOpenKeyExA(
                          hAllProductsKey,
                          pProductInfo->m_CompanyName,
                          0,
                          KEY_READ | KEY_QUERY_VALUE | KEY_WOW64_64KEY,
                          &hCompanyKey);
        }

        // If that didn't work, then try the 32-bit registry.
        if (ERROR_SUCCESS != lResult) {
            lResult = RegOpenKeyExA(
                          g_InstallRegistrySection,
                          g_DefaultInstallRegistryKeyName,
                          0,
                          KEY_READ | KEY_QUERY_VALUE | KEY_WOW64_32KEY,
                          &hAllProductsKey);
            if (ERROR_SUCCESS == lResult) {
                lResult = RegOpenKeyExA(
                              hAllProductsKey,
                              pProductInfo->m_CompanyName,
                              0,
                              KEY_READ | KEY_QUERY_VALUE | KEY_WOW64_32KEY,
                              &hCompanyKey);
            }

            // If nothing worked, then give up.
            if (ERROR_SUCCESS != lResult) {
                goto abort;
            }
        } // Try the 32-bit registry.
    } // Try the 64-bit registry.


    // This is the one thing we use the registry for. Read the pathname to the actual
    // config file.
    dwDataLength = sizeof(pathName);
    lResult = RegQueryValueExA(
                  hCompanyKey,
                  pProductInfo->m_ProductName,
                  NULL,
                  &valueType,
                  (uchar *) pathName,
                  &dwDataLength);
    if ((ERROR_SUCCESS != lResult)
        || (REG_SZ != valueType)) {
        goto abort;
    }

    pathName[dwDataLength] = 0;
    length = dwDataLength;

#elif LINUX
    ///////////////////////////////////////////////////////////////////////
    // LINUX
    if (pProductInfo->m_pConfigFile) {
        strncpyex(pathName, pProductInfo->m_pConfigFile, sizeof(pathName));
    } else if (pProductInfo->m_ProductName) {
        snprintf(
             pathName,
             sizeof(pathName),
             "%s%s",
             pProductInfo->m_ProductName, 
             CONFIG_FILE_NAME);
    } else {
        snprintf(
             pathName,
             sizeof(pathName),
             "%s%c%s",
             DEFAULT_CONFIG_PATH_PREFIX,
             DIRECTORY_SEPARATOR_CHAR,
             CONFIG_FILE_NAME);
    }

#endif

foundFile:
    //printf("\n\nCONFIG: Read path %s\n", pathName);
    ReadConfigFile(pathName);

abort:
    // Do this whether we can find a config file or not. It is used
    // to create variables that expand config values.
    RecordPathVariableValues();

#if WIN32
    if (NULL != hAllProductsKey) {
        RegCloseKey(hAllProductsKey);
        hAllProductsKey = NULL;
    }
    if (NULL != hCompanyKey) {
        RegCloseKey(hCompanyKey);
        hCompanyKey = NULL;
    }
#endif
} // ReadProductConfig






/////////////////////////////////////////////////////////////////////////////
//
// [ReadConfigFile]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CConfigFile::ReadConfigFile(const char *pathName) {
    ErrVal err = ENoErr;
    int32 length;
    char *pEndPath;

    if (NULL == pathName) {
       err = EFail;
       goto abort;
    }
    length = strlen(pathName);


    // Save a copy of the config file name.
    m_pFileName = new char[length + 1];
    if (NULL == m_pFileName) {
       REPORT_LOW_LEVEL_BUG();
       goto abort;
    }
    strncpyex(m_pFileName, pathName, length);


    // Derive a name for the temporary file. This must
    // be a well-known name (not a temp name derived by the OS),
    // so we can find it on restart.
    m_pFinishedFileName = new char[length + 16];
    if (NULL == m_pFinishedFileName) {
       REPORT_LOW_LEVEL_BUG();
       goto abort;
    }
    strncpyex(m_pFinishedFileName, pathName, length);
    pEndPath = m_pFinishedFileName + length;
    strncpyex(pEndPath, g_FinishedFileSuffix, 16);


    m_pInProgressFileName = new char[length + 16];
    if (NULL == m_pInProgressFileName) {
       REPORT_LOW_LEVEL_BUG();
       goto abort;
    }
    strncpyex(m_pInProgressFileName, pathName, length);
    pEndPath = m_pInProgressFileName + length;
    strncpyex(pEndPath, g_ProgressFileSuffix, 16);



    CSimpleFile::DeleteFile(m_pInProgressFileName);

    // Check if there was a crash after committing previous changes.
    if (CSimpleFile::FileExists(m_pFinishedFileName)) {
        // Discard the old master copy.
        CSimpleFile::DeleteFile(m_pFileName);

        // Make the committed version the new master copy.
        err = CSimpleFile::MoveFile(
                                m_pFinishedFileName, // file name
                                m_pFileName); // new file name
        if (err) {
            goto abort;
        }
    }

    // Now, read the file.
    ReadAndParseFile();
    m_ConfigFlags &= ~CHANGES_NEED_SAVE;


abort:
    // Do this whether we can find a config file or not. It is used
    // to create variables that expand config values.
    RecordPathVariableValues();

    return(err);
} // ReadConfigFile






/////////////////////////////////////////////////////////////////////////////
//
// [RecordPathVariableValues]
//
/////////////////////////////////////////////////////////////////////////////
void
CConfigFile::RecordPathVariableValues() {
    int32 length;
    char tempBuffer[2000];
    char *pSrcPtr;
    char *pEndPath;


    if (NULL != m_pInstallDirName) {
       goto abort;
    }

    if (NULL != m_pFileName) {
       pSrcPtr = m_pFileName;
    } else {
#if WIN32
       GetCurrentDirectoryA(sizeof(tempBuffer), tempBuffer);
#elif LINUX
       pSrcPtr = getcwd(tempBuffer, sizeof(tempBuffer));
       if (NULL == pSrcPtr) {
	 tempBuffer[0] = '.';
	 tempBuffer[1] = '/';
	 tempBuffer[2] = 0;
       }
#endif
       pSrcPtr = tempBuffer;
    }


    length = strlen(pSrcPtr);
    m_pInstallDirName = new char[length + 1];
    if (NULL == m_pInstallDirName) {
       REPORT_LOW_LEVEL_BUG();
       goto abort;
    }
    strncpyex(m_pInstallDirName, pSrcPtr, length);


    pEndPath = m_pInstallDirName + length - 1;
    while ((pEndPath > m_pInstallDirName)
            && (!(IS_DIRECTORY_SEPARATOR(*pEndPath)))) {
        pEndPath--;
    }
    *pEndPath = 0;

#if WIN32
    if ((length > 3)
       && (m_pInstallDirName[0])
       && (':' == m_pInstallDirName[1])
       && (0 == m_pInstallDirName[2])) {
       m_pInstallDirName[2] = '\\';
       m_pInstallDirName[3] = 0;
    }
#endif

abort:
    return;
} // RecordPathVariableValues






/////////////////////////////////////////////////////////////////////////////
//
// [Save]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CConfigFile::Save() {
    ErrVal err = ENoErr;
    char *pFileBuffer = NULL;
    char *pDestPtr;
    char *pEndBuffer;
    int32 bufferLength;
    CConfigSection *pGroup;


    m_OSLock.BasicLock();

    if (!(m_ConfigFlags & CHANGES_NEED_SAVE)) {
        goto abort;
    }
    // If this is not a persistent config, then do nothing.
    if (NULL == m_pFileName) {
       goto abort;
    }

    // Create a new temporary file.
    CSimpleFile::DeleteFile(m_pInProgressFileName);
    err = m_FileHandle.OpenOrCreateEmptyFile(m_pInProgressFileName, 0);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    // Write the entire file into a buffer.
    pFileBuffer = new char[WRITE_BUFFER_SIZE];
    if (NULL == pFileBuffer) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
        goto abort;
    }
    pEndBuffer = pFileBuffer + WRITE_BUFFER_SIZE;
    pDestPtr = pFileBuffer;


    pDestPtr += snprintf(
                    pDestPtr,
                    pEndBuffer - pDestPtr,
                    "%s\r\n",
                    g_OpenConfigElement);


    // Each iteration writes one group.
    pGroup = m_pFirstSection;
    while (pGroup) {
        err = WriteOneSection(
                pGroup,
                1,
                pFileBuffer,
                pEndBuffer,
                pDestPtr,
                &pDestPtr);
        if (err) {
            REPORT_LOW_LEVEL_BUG();
            goto abort;
        }

        pGroup = pGroup->m_pNextSection;
    } // writing every option.


    // If there is not enough space at the end of the buffer for the
    // new string, then flush the buffer.
    err = CheckSpaceInWriteBuffer(pFileBuffer, &pDestPtr, pEndBuffer, g_CloseConfigElementLength + 20);
    if (err) {
       REPORT_LOW_LEVEL_BUG();
       goto abort;
    }

    pDestPtr += snprintf(
                    pDestPtr,
                    pEndBuffer - pDestPtr,
                    "\r\n%s\r\n",
                    g_CloseConfigElement);


    // Write any data at the end of the buffer.
    bufferLength = pDestPtr - pFileBuffer;
    err = m_FileHandle.Write(pFileBuffer, bufferLength);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }


abort:
    m_FileHandle.Close();

    // Make the in-progress copy the finished copy. This commits the transaction.
    if (!err) {
        CSimpleFile::DeleteFile(m_pFinishedFileName);
        if ((m_pInProgressFileName) && (m_pFinishedFileName)) {
            err = CSimpleFile::MoveFile(
                                m_pInProgressFileName, // file name
                                m_pFinishedFileName); // new file name
        }
    }

    // At this point, we are committed. If we crash, then the
    // next step will be re-done when we start up next.
    //
    // Make the finished copy the new master copy.
    if (!err) {
        CSimpleFile::DeleteFile(m_pFileName);
        if ((m_pInProgressFileName) && (m_pFinishedFileName)) {
            err = CSimpleFile::MoveFile(
                                m_pFinishedFileName, // file name
                                m_pFileName); // new file name
        }
    }

    if (err) {
        CSimpleFile::DeleteFile(m_pInProgressFileName);
    }
    m_ConfigFlags &= ~CHANGES_NEED_SAVE;

    m_OSLock.BasicUnlock();

    if (pFileBuffer) {
        delete [] pFileBuffer;
    }

    return(err);
} // Save







/////////////////////////////////////////////////////////////////////////////
//
// [CreateSection]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CConfigFile::CreateSection(
                        CConfigSection *pParentSection,
                        const char *pName,
                        CConfigSection **ppResult) {
    ErrVal err = ENoErr;
    CConfigSection *pGroup = NULL;
    CConfigSection::CValue *pValue = NULL;

    m_OSLock.BasicLock();

    // pParentSection may be NULL.
    if ((NULL == pName) || (NULL == ppResult)) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
        goto abort;
    }
    *ppResult = NULL;

    pGroup = new CConfigSection;
    if (NULL == pGroup) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
        goto abort;
    }

    pGroup->m_NameLen = (int32) strlen(pName);
    pGroup->m_pName = new char[pGroup->m_NameLen + 1];
    if (NULL == pGroup->m_pName) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
        goto abort;
    }
    strncpyex(pGroup->m_pName, pName, pGroup->m_NameLen);

    pGroup->m_pOwnerDatabase = this;
    err = pGroup->m_OSLock.Initialize();
    if (err) {
        goto abort;
    }

    if (pParentSection) {
        err = pParentSection->AddValue(pGroup->m_pName, NULL, &pValue);
        if (err) {
            REPORT_LOW_LEVEL_BUG();
            goto abort;
        }
        if (NULL == pValue) {
            REPORT_LOW_LEVEL_BUG();
            err = EFail;
            goto abort;
        }

        pValue->m_pSubsection = pGroup;
    } else
    {
        // Put this at the end of the list. This preserves the order
        // of the original file.
        if (m_pLastSection) {
            m_pLastSection->m_pNextSection = pGroup;
        }

        pGroup->m_pNextSection = NULL;

        m_pLastSection = pGroup;
        if (NULL == m_pFirstSection) {
            m_pFirstSection = pGroup;
        }
    }

    *ppResult = pGroup;
    pGroup = NULL;

abort:
    m_OSLock.BasicUnlock();

    if (pGroup) {
        delete pGroup;
    }

    return(err);
} // CreateSection.





/////////////////////////////////////////////////////////////////////////////
//
// [FindSection]
//
/////////////////////////////////////////////////////////////////////////////
CConfigSection *
CConfigFile::FindSection(const char *pName, bool fCreateIfMissing) {
    ErrVal err = ENoErr;
    CConfigSection *pGroup = NULL;

    if (NULL == pName) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
        goto done;
    }

    m_OSLock.BasicLock();

    pGroup = m_pFirstSection;
    while (pGroup) {
        if (0 == strcasecmpex(pGroup->m_pName, pName)) {
            break;
        }

        pGroup = pGroup->m_pNextSection;
    }
    m_OSLock.BasicUnlock();


    if ((NULL == pGroup) && (fCreateIfMissing)) {
        err = CreateSection(NULL, pName, &pGroup);
        if (err) {
            REPORT_LOW_LEVEL_BUG();
            err = ENoErr;
            pGroup = NULL;
        }
    }

done:
    return(pGroup);
} // FindSection








/////////////////////////////////////////////////////////////////////////////
//
// [RemoveSection]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CConfigFile::RemoveSection(CConfigSection *pTargetGroup) {
    ErrVal err = ENoErr;
    CConfigSection *pGroup = NULL;
    CConfigSection *pPrevTargetGroup = NULL;

    if (NULL == pTargetGroup) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
        goto done;
    }

    m_OSLock.BasicLock();

    pPrevTargetGroup = NULL;
    pGroup = m_pFirstSection;
    while (pGroup) {
        if (pGroup == pTargetGroup) {
            break;
        }
        pPrevTargetGroup = pGroup;
        pGroup = pGroup->m_pNextSection;
    }
    m_OSLock.BasicUnlock();

    if (NULL == pGroup) {
        REPORT_LOW_LEVEL_BUG();
        err = ENoErr;
    } else if (NULL == pPrevTargetGroup) {
        m_pFirstSection = pGroup->m_pNextSection;
    } else {
        pPrevTargetGroup->m_pNextSection = pGroup->m_pNextSection;
    }
    m_ConfigFlags |= CConfigFile::CHANGES_NEED_SAVE;
    
done:
    return(err);
} // RemoveSection







/////////////////////////////////////////////////////////////////////////////
//
// [ReadAndParseFile]
//
/////////////////////////////////////////////////////////////////////////////
void
CConfigFile::ReadAndParseFile() {
    ErrVal err = ENoErr;
    uint32 fileLength;
    uint64 fileLength64;
    uint32 bytesRead;
    char *pFileBuffer = NULL;
    char *pSrcPtr;

    err = m_FileHandle.OpenOrCreateFile(m_pFileName, 0); // CSimpleFile::READ_ONLY);
    if (err) {
        err = EMissingConfigFile;
        goto abort;
    }

    err = m_FileHandle.GetFileLength(&fileLength64);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }
    fileLength = (int32) fileLength64;

    // Seek the beginning of the file
    err = m_FileHandle.Seek(0L, CSimpleFile::SEEK_START);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    // Read the entire file into a buffer.
    pFileBuffer = new char[fileLength + 1];
    if (NULL == pFileBuffer) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
        goto abort;
    }

    err = m_FileHandle.Read(pFileBuffer, fileLength, (int32 *) &bytesRead);
    if ((err) || (bytesRead != fileLength)) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    // Make this a C-string for easier parsing.
    pFileBuffer[fileLength] = 0;


    // Go to the start of the next name/value pair.
    pSrcPtr = pFileBuffer;
    while (*pSrcPtr) {
        // Skip to the next element.
        while ((*pSrcPtr) && (g_OpenElementChar != *pSrcPtr)) {
            pSrcPtr++;
        }

        // If we hit the end of the file, then we are done.
        if (!(*pSrcPtr)) {
            goto abort;
        }

        // If this element marks a tree of name/value pairs, then stop
        // looking. The next step will be to parse this tree.
        if (0 == strncasecmpex(
                    pSrcPtr,
                    g_StartOpenConfigElement,
                    g_StartOpenConfigElementLength)) {
           pSrcPtr += g_StartOpenConfigElementLength;
           break;
        }

        pSrcPtr++;
    } // Find the next name/value pair.


    // We stop in the middle of the first <config...> element. This is
    // OK; we will advance to the start of the next element, which
    // means we will first leave the end of this config.


    // Each iteration reads the element for 1 name/value pair.
    // This may be a parent value which contains a subtree of simpler values.
    // Stop when we hit the end of the whole file.
    while (*pSrcPtr) {
        // Skip to the start of the next element.
        while ((*pSrcPtr) && (g_OpenElementChar != *pSrcPtr)) {
            pSrcPtr++;
        }

        // If we hit the end of the file, then we are done.
        if (!(*pSrcPtr)) {
            break;
        }

        // If this element marks the end of a tree of name/value pairs, then stop.
        if (0 == strncasecmpex(
                        pSrcPtr,
                        g_CloseConfigElement,
                        g_CloseConfigElementLength)) {
           break;
        }

        err = ReadOneElement(NULL, pSrcPtr, &pSrcPtr);
        if (err) {
            goto abort;
        }
    } // while (*pSrcPtr)

abort:
    m_FileHandle.Close();
    if (pFileBuffer) {
        delete [] pFileBuffer;
    }
} // ReadAndParseFile.








/////////////////////////////////////////////////////////////////////////////
//
// [ReadOneElement]
//
// Read and parse one element in the file. The element may be a simple
// value or a subtree of values.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CConfigFile::ReadOneElement(
                  CConfigSection *pParentSection,
                  char *pSrcPtr,
                  char **ppResumePtr) {
    ErrVal err = ENoErr;
    char *pStartElementName = NULL;
    bool fIsCloseElement = false;
    char *pStartNameAttribute = NULL;
    char *pStopNameAttribute = NULL;
    char *pStartElementBody = NULL;
    char *pStopElement = NULL;
    int32 nameLength;

    *ppResumePtr = NULL;


    // Get the end of the element.
    pStopElement = pSrcPtr;
    while ((*pStopElement) && (g_CloseElementChar != *pStopElement)) {
        pStopElement++;
    }
    if (g_CloseElementChar != *pStopElement) {
        REPORT_LOW_LEVEL_BUG();
        err = EInvalidConfigFile;
        goto abort;
    }


    // Get the element name.
    while (g_OpenElementChar == *pSrcPtr) {
        pSrcPtr++;
    }
    if ('/' == *pSrcPtr) {
        fIsCloseElement = true;
        while ('/' == *pSrcPtr) {
            pSrcPtr++;
        }
    }
    pStartElementName = pSrcPtr;
    while ((pSrcPtr < pStopElement)
        && (!(WHITE_SPACE(*pSrcPtr)))
        && (g_CloseElementChar != *pSrcPtr)) {
        pSrcPtr++;
    }
    pStartElementBody = pSrcPtr;


    // Look for the name attribute. This is the name of the value described
    // by this element.
    while (pSrcPtr < pStopElement) {
        if (0 == strncasecmpex(pSrcPtr, g_NameAttribute, g_NameAttributeLength)) {
            pSrcPtr += g_NameAttributeLength;
            if ((WHITE_SPACE(*pSrcPtr)) || ('=' == *pSrcPtr)) {
                while (WHITE_SPACE(*pSrcPtr)) {
                    pSrcPtr++;
                }
                if ('=' == *pSrcPtr) {
                    pSrcPtr++;
                    while (WHITE_SPACE(*pSrcPtr)) {
                        pSrcPtr++;
                    }
                    if (QUOTE_CHAR(*pSrcPtr)) {
                        pSrcPtr++;
                    }
                    pStartNameAttribute = pSrcPtr;
                    break;
                }
            }
         }

         pSrcPtr++;
    } // while (pSrcPtr < pStopElement)

    if (pSrcPtr >= pStopElement) {
        pSrcPtr = pStopElement;
        goto abort;
    }

    pStopNameAttribute = pStartNameAttribute;
    while ((pStopNameAttribute < pStopElement) && !(QUOTE_CHAR(*pStopNameAttribute))) {
        pStopNameAttribute++;
    }
    if (!(QUOTE_CHAR(*pStopNameAttribute))) {
        REPORT_LOW_LEVEL_BUG();
        err = EInvalidConfigFile;
        goto abort;
    }

    // Now, switch on the element type. This is different than the name attribute.
    nameLength = pStartElementBody - pStartElementName;
    if ((nameLength == g_GroupElementNameLength)
         && (0 == strncasecmpex(pStartElementName, g_GroupElementName, nameLength))) {
        err = ReadSubSection(
                  pParentSection,
                  pStopElement,
                  fIsCloseElement,
                  pStartNameAttribute,
                  pStopNameAttribute,
                  &pSrcPtr);
    } else if ((nameLength == g_ValueElementNameLength)
         && (0 == strncasecmpex(pStartElementName, g_ValueElementName, nameLength))) {
        err = ReadSimpleValue(
                  pParentSection,
                  pStartElementBody,
                  pStopElement,
                  pStartNameAttribute,
                  pStopNameAttribute,
                  &pSrcPtr);
    } else
    {
        while ((*pSrcPtr) && (g_CloseElementChar != *pSrcPtr)) {
            pSrcPtr++;
        }
    }

abort:
    *ppResumePtr = pSrcPtr;

    return(err);
} // ReadOneElement.





/////////////////////////////////////////////////////////////////////////////
//
// [ReadSubSection]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CConfigFile::ReadSubSection(
                        CConfigSection *pParentSection,
                        char *pStopElement,
                        bool fIsCloseElement,
                        char *pStartNameAttribute,
                        char *pStopNameAttribute,
                        char **ppResumePtr) {
    ErrVal err = ENoErr;
    char saveChar1;
    CConfigSection *pGroup = NULL;
    char *pSrcPtr = NULL;


    *ppResumePtr = NULL;

    // We cannot start with a close element.
    if (fIsCloseElement) {
        REPORT_LOW_LEVEL_BUG();
        err = EInvalidConfigFile;
        goto abort;
    }

    // Create a data structure to store the subtree
    saveChar1 = *pStopNameAttribute;
    *pStopNameAttribute = 0;
    err = CreateSection(pParentSection, pStartNameAttribute, &pGroup);
    *pStopNameAttribute = saveChar1;
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    // Go to the end of this element.
    pSrcPtr = pStopElement;
    while (g_CloseElementChar == *pSrcPtr) {
        pSrcPtr++;
    }


    // Each iteration reads one sub-element in the current parent element.
    // Stop when we hit the close element.
    while (*pSrcPtr) {
        // Skip to the next element.
        while ((*pSrcPtr) && (g_OpenElementChar != *pSrcPtr)) {
            pSrcPtr++;
        }

        // If we hit the end of the file, then we are done.
        if (!(*pSrcPtr)) {
            break;
        }

        // If we hit the close element, then we are done.
        if (0 == strncasecmpex(pSrcPtr, g_CloseGroupElement, g_CloseGroupElementLength)) {
           pSrcPtr += g_CloseGroupElementLength;
           break;
        }

        err = ReadOneElement(pGroup, pSrcPtr, &pSrcPtr);
        if (err) {
            REPORT_LOW_LEVEL_BUG();
            goto abort;
        }
    } // Read every nested child element


abort:
    if (NULL != ppResumePtr) {
        *ppResumePtr = pSrcPtr;
    }

    // Do not delete the group, even if we have an error, since
    // the key may be in the linked list already. This may cause a memory
    // leak if we haven't already inserted the group in the list, but
    // that is not significant.

    return(err);
} // ReadSubSection.






/////////////////////////////////////////////////////////////////////////////
//
// [ReadSimpleValue]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CConfigFile::ReadSimpleValue(
                        CConfigSection *pParentSection,
                        char *pStartElementBody,
                        char *pStopElement,
                        char *pStartNameAttribute,
                        char *pStopNameAttribute,
                        char **ppResumePtr) {
    ErrVal err = ENoErr;
    char *pSrcPtr;
    char *pStartData = NULL;
    char *pStopData = NULL;
    char saveChar1;
    char saveChar2;


    if ((NULL == pParentSection)
        || (NULL == pStartElementBody)
        || (NULL == pStopElement)
        || (NULL == pStartNameAttribute)
        || (NULL == pStopNameAttribute)
        || (NULL == ppResumePtr)) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }
    *ppResumePtr = pStopElement;


    // The data immediately follows the element. The format is:
    //       <element>xxxxxxxDataxxxxxxx</element>
    // Find the start and end of the data.
    pSrcPtr = pStopElement + 1;
    pStartData = pStopElement + 1;
    while (*pSrcPtr) {
        if ((g_OpenElementChar == *pSrcPtr)
            && (0 == strncasecmpex(pSrcPtr, g_CloseValueElement, g_CloseValueElementLength))) {
            pStopData = pSrcPtr;
            *ppResumePtr = pSrcPtr + g_CloseValueElementLength;
            break;
        }

        pSrcPtr++;
    } // while (*pSrcPtr)

    if (0 == *pSrcPtr) {
        REPORT_LOW_LEVEL_BUG();
        err = EInvalidConfigFile;
        goto abort;
    }

    // Strip off whitespace around the data.
    while ((pStartData < pStopData) && (WHITE_SPACE(*pStartData))) {
        pStartData += 1;
    }
    while ((pStartData < pStopData) && (WHITE_SPACE(*(pStopData - 1)))) {
        pStopData -= 1;
    }

    // Make the name and value into C strings.
    saveChar1 = *pStopNameAttribute;
    *pStopNameAttribute = 0;
    saveChar2 = *pStopData;
    *pStopData = 0;

    // Now, record the value.
    err = pParentSection->AddValue(pStartNameAttribute, pStartData, NULL);

    // Restore the strings.
    *pStopNameAttribute = saveChar1;
    *pStopData = saveChar2;

    // If we could not add the value, then abort.
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

abort:
    return(err);
} // ReadSimpleValue.









/////////////////////////////////////////////////////////////////////////////
//
// [CheckSpaceInWriteBuffer]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CConfigFile::CheckSpaceInWriteBuffer(
                                 char *pFileBuffer,
                                 char **ppDestPtr,
                                 char *pEndBuffer,
                                 int32 neededSpace) {
    ErrVal err = ENoErr;
    char *pDestPtr;
    int32 bufferLength;

    pDestPtr = *ppDestPtr;

    // If there is not enough space at the end of the buffer for the
    // new string, then flush the buffer.
    if ((pDestPtr + neededSpace) >= pEndBuffer) {
        bufferLength = pDestPtr - pFileBuffer;
        err = m_FileHandle.Write(pFileBuffer, bufferLength);
        if (err) {
            REPORT_LOW_LEVEL_BUG();
            goto abort;
        }
        pDestPtr = pFileBuffer;

        // If there is still not enough space, then the buffer
        // is corrupted.
        if ((pDestPtr + neededSpace) >= pEndBuffer) {
            REPORT_LOW_LEVEL_BUG();
            err = EInvalidConfigFile;
            goto abort;
        }
    }

abort:
    *ppDestPtr = pDestPtr;
    return(err);
} // CheckSpaceInWriteBuffer.






/////////////////////////////////////////////////////////////////////////////
//
// [WriteOneSection]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CConfigFile::WriteOneSection(
                    CConfigSection *pGroup,
                    int32 indentLevel,
                    char *pFileBuffer,
                    char *pEndBuffer,
                    char *pDestPtr,
                    char **ppResumePtr) {
    ErrVal err = ENoErr;
    CConfigSection::CValue *pValue;
    int newLineLength;
    int32 index;
    int32 expandedLength;


    // If there is not enough space at the end of the buffer for the
    // new string, then flush the buffer.
    newLineLength = g_GroupElementNameLength
                        + pGroup->m_NameLen
                        + 28
                        + (2 * indentLevel * g_IndentStrLength);
    err = CheckSpaceInWriteBuffer(pFileBuffer, &pDestPtr, pEndBuffer, newLineLength);
    if (err) {
       REPORT_LOW_LEVEL_BUG();
       goto abort;
    }

    // Write the open group element.
    pDestPtr += snprintf(
                    pDestPtr,
                    pEndBuffer - pDestPtr,
                    "\r\n");
    for (index = 0; index < indentLevel; index++) {
        pDestPtr += snprintf(
                         pDestPtr,
                         pEndBuffer - pDestPtr,
                         g_IndentStr);
    }
    pDestPtr += snprintf(
                    pDestPtr,
                    pEndBuffer - pDestPtr,
                    "<%s %s=\"%s\">\r\n",
                    g_GroupElementName,
                    g_NameAttribute,
                    pGroup->m_pName);


    // Each iteration writes one value.
    pValue = pGroup->m_pFirstValue;
    while (pValue) {
        if (pValue->m_pSubsection) {
            err = WriteOneSection(
                    pValue->m_pSubsection,
                    indentLevel + 1,
                    pFileBuffer,
                    pEndBuffer,
                    pDestPtr,
                    &pDestPtr);
            if (err) {
                REPORT_LOW_LEVEL_BUG();
                goto abort;
            }
        } // a nested group.
        // A simple name-value pair.
        else if ((pValue->m_pName)
            && (pValue->m_pStringValue)
            && (pValue->m_StringValueLength > 0)) {
            newLineLength = pValue->m_NameLen
                                + g_ValueElementNameLength
                                + g_NameAttributeLength
                                + g_CloseValueElementLength
                                + (pValue->m_StringValueLength)
                                + (2 * g_IndentStrLength * (indentLevel + 1))
                                + 28;
            err = CheckSpaceInWriteBuffer(pFileBuffer, &pDestPtr, pEndBuffer, newLineLength);
            if (err) {
               REPORT_LOW_LEVEL_BUG();
               goto abort;
            }

            // Write the open element.
            for (index = 0; index < (indentLevel + 1); index++) {
                pDestPtr += snprintf(
                              pDestPtr,
                              pEndBuffer - pDestPtr,
                              g_IndentStr);
            }

            pDestPtr += snprintf(
                           pDestPtr,
                           pEndBuffer - pDestPtr,
                           "<%s %s=\"%s\">",
                           g_ValueElementName,
                           g_NameAttribute,
                           pValue->m_pName);

            // Do NOT do an snprintf.
            // I do not want % or other escape characters to be expanded
            // by snprintf.
            expandedLength = WriteStringWithEscapes(
                                        pDestPtr,
                                        pValue->m_pStringValue,
                                        pEndBuffer - pDestPtr);
            pDestPtr += expandedLength;

            pDestPtr += snprintf(
                           pDestPtr,
                           pEndBuffer - pDestPtr,
                           "%s\r\n",
                           g_CloseValueElement);
        } // A simple name-value pair.

        pValue = pValue->m_pNextValue;
    } // writing every option.


    // If there is not enough space at the end of the buffer for the
    // new string, then flush the buffer.
    newLineLength = g_CloseGroupElementLength + 30;
    err = CheckSpaceInWriteBuffer(pFileBuffer, &pDestPtr, pEndBuffer, newLineLength);
    if (err) {
       REPORT_LOW_LEVEL_BUG();
       goto abort;
    }

    // Write the close group element.
    // Add some whitespace between keys or else this gets
    // hard for to read.
    for (index = 0; index < indentLevel; index++) {
        pDestPtr += snprintf(
                         pDestPtr,
                         pEndBuffer - pDestPtr,
                         g_IndentStr);
    }
    pDestPtr += snprintf(
                    pDestPtr,
                    pEndBuffer - pDestPtr,
                    "%s\r\n",
                    g_CloseGroupElement);

abort:
    *ppResumePtr = pDestPtr;

    return(err);
} // WriteOneSection







/////////////////////////////////////////////////////////////////////////////
//
// [WriteStringWithEscapes]
//
/////////////////////////////////////////////////////////////////////////////
int32
CConfigFile::WriteStringWithEscapes(
                    char *pDestPtr,
                    char *pSrcPtr,
                    int32 maxDestLength) {
    char *pBeginDestPtr;
    char *pEndDestPtr;

    pBeginDestPtr = pDestPtr;
    pEndDestPtr = pDestPtr + maxDestLength - 1;
    while ((*pSrcPtr) && (pDestPtr < pEndDestPtr)) {
        if ('\n' == *pSrcPtr) {
            if ((pDestPtr + 1) < pEndDestPtr) {
                *(pDestPtr++) = '\\';
                *(pDestPtr++) = 'n';
            }
            pSrcPtr += 1;
        } else if ('\r' == *pSrcPtr) {
            if ((pDestPtr + 1) < pEndDestPtr) {
                *(pDestPtr++) = '\\';
                *(pDestPtr++) = 'r';
            }
            pSrcPtr += 1;
        } else if ('\t' == *pSrcPtr) {
            if ((pDestPtr + 1) < pEndDestPtr) {
                *(pDestPtr++) = '\\';
                *(pDestPtr++) = 't';
            }
            pSrcPtr += 1;
        } else if ('\\' == *pSrcPtr) {
            if ((pDestPtr + 1) < pEndDestPtr) {
                *(pDestPtr++) = '\\';
                *(pDestPtr++) = '\\';
            }
            pSrcPtr += 1;
        } else if ('\'' == *pSrcPtr) {
            if ((pDestPtr + 1) < pEndDestPtr) {
                *(pDestPtr++) = '\\';
                *(pDestPtr++) = '\'';
            }
            pSrcPtr += 1;
        } else if ('\"' == *pSrcPtr) {
            if ((pDestPtr + 1) < pEndDestPtr) {
                *(pDestPtr++) = '\\';
                *(pDestPtr++) = '\"';
            }
            pSrcPtr += 1;
        } else {
            *(pDestPtr++) = *(pSrcPtr++);
        }
    }

    // Do not advance the pointer, because the terminating character
    // is not part of the string length.
    if (pDestPtr < pEndDestPtr) {
        *pDestPtr = 0;
    }

    return(pDestPtr - pBeginDestPtr);
} // WriteStringWithEscapes.







/////////////////////////////////////////////////////////////////////////////
//
// [WriteStringAndRemoveEscapes]
//
/////////////////////////////////////////////////////////////////////////////
int32
CConfigFile::WriteStringAndRemoveEscapes(
                    char *pDestPtr,
                    const char *pSrcPtr,
                    int32 maxDestLength,
                    bool fRemoveEscapeChars) {
    char *pBeginDestPtr;
    char *pEndDestPtr;

    pBeginDestPtr = pDestPtr;
    pEndDestPtr = pDestPtr + maxDestLength;
    while ((*pSrcPtr) && (pDestPtr < pEndDestPtr)) {
        if (('\\' == *pSrcPtr)
            && (fRemoveEscapeChars)
            && (pSrcPtr[1])) {
            // Skip the escaping \ character.
            pSrcPtr++;

            // Find out what was escaped.
            if ('n' == *pSrcPtr) {
                *(pDestPtr++) = '\n';
                pSrcPtr += 1;
            }
            else if ('r' == *pSrcPtr) {
                *(pDestPtr++) = '\r';
                pSrcPtr += 1;
            }
            else if ('t' == *pSrcPtr) {
                *(pDestPtr++) = '\t';
                pSrcPtr += 1;
            }
            else {
                *(pDestPtr++) = *(pSrcPtr++);
            }
        } else {
            *(pDestPtr++) = *(pSrcPtr++);
        }
    }

    // Do not advance the pointer, because the terminating character
    // is not part of the string length.
    if (pDestPtr < pEndDestPtr) {
        *pDestPtr = 0;
    }

    return(pDestPtr - pBeginDestPtr);
} // WriteStringAndRemoveEscapes.






/////////////////////////////////////////////////////////////////////////////
//
// [CConfigSection]
//
/////////////////////////////////////////////////////////////////////////////
CConfigSection::CConfigSection() {
    m_pName = NULL;
    m_NameLen = 0;

    m_pFirstValue = NULL;
    m_pLastValue = NULL;

    m_pOwnerDatabase = NULL;

    m_pNextSection = NULL;
} // CConfigSection.






/////////////////////////////////////////////////////////////////////////////
//
// [~CConfigSection]
//
/////////////////////////////////////////////////////////////////////////////
CConfigSection::~CConfigSection() {
    CValue *pNextValue;

    if (m_pName) {
        delete m_pName;
    }

    while (m_pFirstValue) {
        pNextValue = m_pFirstValue->m_pNextValue;
        delete m_pFirstValue;
        m_pFirstValue = pNextValue;
    }

    m_OSLock.Shutdown();
} // ~CConfigSection.






/////////////////////////////////////////////////////////////////////////////
//
// [FindValue]
//
/////////////////////////////////////////////////////////////////////////////
CConfigSection::CValue *
CConfigSection::FindValue(CValue *pPrevValue, const char *pValueName) {
    CValue *pValue = NULL;

    m_OSLock.BasicLock();

    if (NULL == pValueName) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    if (NULL != pPrevValue) {
        pValue = pPrevValue->m_pNextValue;
    } else
    {
        pValue = m_pFirstValue;
    }

    while (pValue) {
        if (0 == strcasecmpex(pValue->m_pName, pValueName)) {
            break;
        }

        pValue = pValue->m_pNextValue;
    }

abort:
    m_OSLock.BasicUnlock();
    return(pValue);
} // FindValue








/////////////////////////////////////////////////////////////////////////////
//
// [GetString]
//
/////////////////////////////////////////////////////////////////////////////
char *
CConfigSection::GetString(const char *pValueName, char *defaultValue) {
    CValue *pValue;
    char *pResult = defaultValue;

    m_OSLock.BasicLock();

    if (NULL == pValueName) {
        goto abort;
    }

    pValue = FindValue(NULL, pValueName);
    if ((NULL != pValue) && (NULL != pValue->m_pStringValue)) {
        pResult = pValue->m_pStringValue;
    }

abort:
    m_OSLock.BasicUnlock();
    return(pResult);
} // GetString








/////////////////////////////////////////////////////////////////////////////
//
// [GetBool]
//
/////////////////////////////////////////////////////////////////////////////
bool
CConfigSection::GetBool(const char *pValueName, bool fDefault) {
    CValue *pValue;
    bool fResult = fDefault;

    m_OSLock.BasicLock();

    if (NULL == pValueName) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    pValue = FindValue(NULL, pValueName);
    if (NULL == pValue) {
        goto abort;
    }

    if (NULL == pValue->m_pStringValue) {
       REPORT_LOW_LEVEL_BUG();
       goto abort;
    }

    if ((strcasecmpex(pValue->m_pStringValue, "true") == 0)
       || (strcasecmpex(pValue->m_pStringValue, "yes") == 0)
       || (strcasecmpex(pValue->m_pStringValue, "on") == 0)
       || (strcasecmpex(pValue->m_pStringValue, "1") == 0)) {
       fResult = true;
    } else
    {
       fResult = false;
    }

abort:
    m_OSLock.BasicUnlock();
    return(fResult);
} // GetBool





/////////////////////////////////////////////////////////////////////////////
//
// [GetInt]
//
/////////////////////////////////////////////////////////////////////////////
int32
CConfigSection::GetInt(const char *pValueName, int32 defaultVal) {
    CValue *pValue;
    int32 result = defaultVal;

    m_OSLock.BasicLock();

    if (NULL == pValueName) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    pValue = FindValue(NULL, pValueName);
    if (NULL == pValue) {
        goto abort;
    }

    if (NULL != pValue->m_pStringValue) {
        result = atoi(pValue->m_pStringValue);
    }

abort:
    m_OSLock.BasicUnlock();
    return(result);
} // GetInt







/////////////////////////////////////////////////////////////////////////////
//
// [GetPathname]
//
// This is like GetString except that it does variable expansion.
/////////////////////////////////////////////////////////////////////////////
void
CConfigSection::GetPathname(
                    const char *pValueName,
                    char *result,
                    int maxResultLen,
                    char *defaultValue) {
    char *pSrcPtr;
    char *pDestPtr;
    char *pEndDestPtr;
    int32 length;
    CValue *pValue;

    m_OSLock.BasicLock();

    if ((NULL == pValueName)
        || (NULL == result)
        || (maxResultLen < 1)) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }
    *result = 0;


    pValue = FindValue(NULL, pValueName);
    if ((NULL != pValue)
          && (NULL != pValue->m_pStringValue)
          && (pValue->m_StringValueLength < maxResultLen)) {
        pSrcPtr = pValue->m_pStringValue;
    } else {
        pSrcPtr = defaultValue;
    }

    // Copy the result and expand the variables.
    pDestPtr = result;
    pEndDestPtr = result + maxResultLen - 1;
    while ((*pSrcPtr) && (pDestPtr < pEndDestPtr)) {
       if ((*pSrcPtr == g_InstallationDirVariableName[0])
            && (0 == strncasecmpex(
                           pSrcPtr,
                           g_InstallationDirVariableName,
                           g_InstallationDirVariableNameLength))) {

            if (NULL == m_pOwnerDatabase->m_pInstallDirName) {
                REPORT_LOW_LEVEL_BUG();
                goto abort;
            }

            length = (int32) strlen(m_pOwnerDatabase->m_pInstallDirName);
            if ((pDestPtr + length) >= pEndDestPtr) {
                REPORT_LOW_LEVEL_BUG();
                goto abort;
            }

            strncpyex(pDestPtr, m_pOwnerDatabase->m_pInstallDirName, length);

            pSrcPtr += g_InstallationDirVariableNameLength;
            pDestPtr += length;
        } else {
            *(pDestPtr++) = *(pSrcPtr++);
        }
    }
    *pDestPtr = 0;


abort:
    m_OSLock.BasicUnlock();
} // GetPathname






/////////////////////////////////////////////////////////////////////////////
//
// [GetSection]
//
/////////////////////////////////////////////////////////////////////////////
CConfigSection *
CConfigSection::GetSection(const char *pValueName) {
    CValue *pValue = NULL;
    CConfigSection *pResult = NULL;

    m_OSLock.BasicLock();

    if (NULL == pValueName) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    pValue = FindValue(NULL, pValueName);
    if (NULL != pValue) {
        pResult = pValue->m_pSubsection;
    }

abort:
    m_OSLock.BasicUnlock();

    return(pResult);
} // GetSection.








/////////////////////////////////////////////////////////////////////////////
//
// [SetValue]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CConfigSection::SetValue(const char *pValueName, const char *pValueData) {
    ErrVal err = ENoErr;
    CValue *pValue;

    m_OSLock.BasicLock();

    if ((NULL == pValueName)
        || (NULL == pValueData)) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
        goto abort;
    }

    pValue = FindValue(NULL, pValueName);
    if (NULL != pValue) {
        if (pValue->m_pStringValue) {
            delete pValue->m_pStringValue;
        }

        pValue->m_StringValueLength = (int32) strlen(pValueData);
        pValue->m_pStringValue = new char[pValue->m_StringValueLength + 1];
        if (NULL == pValue->m_pStringValue) {
            REPORT_LOW_LEVEL_BUG();
            err = EFail;
            goto abort;
        }
        strncpyex(
            pValue->m_pStringValue,
            pValueData,
            pValue->m_StringValueLength);
    } else
    {
        err = AddValue(pValueName, pValueData, NULL);
        if (err) {
            REPORT_LOW_LEVEL_BUG();
            goto abort;
        }
    }

    if (m_pOwnerDatabase) {
        m_pOwnerDatabase->m_ConfigFlags |= CConfigFile::CHANGES_NEED_SAVE;
    }

abort:
    m_OSLock.BasicUnlock();
    return(err);
} // SetValue






/////////////////////////////////////////////////////////////////////////////
//
// [AddValue]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CConfigSection::AddValue(
                    const char *pValueName,
                    const char *pValueData,
                    CValue **ppResultValue) {
    ErrVal err = ENoErr;
    CValue *pValue = NULL;

    m_OSLock.BasicLock();

    if (ppResultValue) {
        *ppResultValue = NULL;
    }

    if (NULL == pValueName) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
        goto abort;
    }


    pValue = new CValue;
    if (NULL == pValue) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
        goto abort;
    }

    pValue->m_NameLen = (int32) strlen(pValueName);
    pValue->m_pName = new char[pValue->m_NameLen + 1];
    if (NULL == pValue->m_pName) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
        goto abort;
    }

    //strncpyex(pValue->m_pName, pValueName, pValue->m_NameLen);
    pValue->m_NameLen = CConfigFile::WriteStringAndRemoveEscapes(
                                                pValue->m_pName,
                                                (char *) pValueName,
                                                pValue->m_NameLen + 1,
                                                true); // fRemoveEscapeChars

    if (pValueData) {
        pValue->m_StringValueLength = (int32) strlen(pValueData);
        pValue->m_pStringValue = new char[pValue->m_StringValueLength + 1];
        if (NULL == pValue->m_pStringValue) {
            REPORT_LOW_LEVEL_BUG();
            err = EFail;
            goto abort;
        }

        // strncpyex(pValue->m_pStringValue, pValueData, pValue->m_StringValueLength);
        pValue->m_StringValueLength = CConfigFile::WriteStringAndRemoveEscapes(
                                            pValue->m_pStringValue,
                                            pValueData,
                                            pValue->m_StringValueLength + 1,
                                            true); // fRemoveEscapeChars
    }

    // Put this at the end of the list. This preserves the order
    // of the original file.
    if (m_pLastValue) {
        m_pLastValue->m_pNextValue = pValue;
    }

    pValue->m_pPrevValue = m_pLastValue;
    pValue->m_pNextValue = NULL;

    m_pLastValue = pValue;
    if (NULL == m_pFirstValue) {
        m_pFirstValue = pValue;
    }

    if (ppResultValue) {
        *ppResultValue = pValue;
    }

    pValue = NULL;

abort:
    if (pValue) {
        delete pValue;
    }

    m_OSLock.BasicUnlock();
    return(err);
} // AddValue






/////////////////////////////////////////////////////////////////////////////
//
// [SetString]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CConfigSection::SetString(const char *pValueName, const char *pValueData) {
    return(SetValue(pValueName, pValueData));
} // SetString





/////////////////////////////////////////////////////////////////////////////
//
// [SetPathNameString]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CConfigSection::SetPathNameString(const char *pValueName, char *pValueData) {
    char *pStr = pValueData;
    if (pStr) {
        while (*pStr) {
            if ('\\' == *pStr) {
                *pStr = '/';
            }
            pStr++;
        } // while (*pStr)
    } // if (pStr)

    return(SetValue(pValueName, pValueData));
} // SetPathNameString






/////////////////////////////////////////////////////////////////////////////
//
// [SetBool]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CConfigSection::SetBool(const char *pValueName, bool value) {
    ErrVal err = ENoErr;
    char tempStr[32];

    if (value) {
        strncpyex(tempStr, "True", sizeof(tempStr) - 1);
    } else
    {
        strncpyex(tempStr, "False", sizeof(tempStr) - 1);
    }

    err = SetValue(pValueName, tempStr);
    return(err);
} // SetBool.



/////////////////////////////////////////////////////////////////////////////
//
// [SetInt]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CConfigSection::SetInt(const char *pValueName, int32 value) {
    ErrVal err = ENoErr;
    char tempStr[32];

    snprintf(tempStr, sizeof(tempStr) - 1, "%d", value);
    err = SetValue(pValueName, tempStr);

    return(err);
} // SetInt.







/////////////////////////////////////////////////////////////////////////////
//
// [CValue]
//
/////////////////////////////////////////////////////////////////////////////
CConfigSection::CValue::CValue() {
    m_pName = NULL;
    m_NameLen = 0;

    m_pStringValue = NULL;
    m_StringValueLength = 0;
    m_pSubsection = NULL;

    m_pPrevValue = NULL;
    m_pNextValue = NULL;
} // CValue.




/////////////////////////////////////////////////////////////////////////////
//
// [~CValue]
//
/////////////////////////////////////////////////////////////////////////////
CConfigSection::CValue::~CValue() {
    if (m_pName) {
        delete m_pName;
    }

    if (m_pStringValue) {
        delete m_pStringValue;
    }

    if (m_pSubsection) {
        delete m_pSubsection;
    }
} // ~CValue.






/////////////////////////////////////////////////////////////////////////////
//
//                       TESTING PROCEDURES
//
// Config testing is a little special. Test modules are built on top
// of config, so config does not use the standard test package.
// It simply returns an error code indicating whether it passed its
// tests or not.
/////////////////////////////////////////////////////////////////////////////
#if INCLUDE_REGRESSION_TESTS

static CConfigFile g_TestConfig;

static char TEST_FILE_PATH[1024];
#pragma GCC diagnostic ignored "-Wformat-truncation"



/////////////////////////////////////////////////////////////////////////////
//
// [TestConfig]
//
/////////////////////////////////////////////////////////////////////////////
void
CConfigFile::TestConfig() {
    ErrVal err = ENoErr;
    CConfigSection *testGroup = NULL;
    CConfigSection *nestedGroup1 = NULL;
    CConfigSection *nestedGroup2 = NULL;

    OSIndependantLayer::PrintToConsole("Test Module: Config");
    snprintf(TEST_FILE_PATH, 
             sizeof(TEST_FILE_PATH), 
             "%s%c%s%ctempData.cpp",
             g_SoftwareDirectoryRoot,
             DIRECTORY_SEPARATOR_CHAR,
             "testData",
             DIRECTORY_SEPARATOR_CHAR);


    g_TestConfig.ReadConfigFile(TEST_FILE_PATH);


    /////////////////////////////////////////////////
    OSIndependantLayer::PrintToConsole("  Test: Adding config values.");
    err = g_TestConfig.CreateSection(
                            NULL,
                            "TestGroup",
                            &testGroup);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    err = testGroup->SetString("TestGroup", "blah");
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }
    err = testGroup->SetString("TestGroup", "This is a test string.");
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }
    err = testGroup->SetInt("TestInt", 52);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }
    err = testGroup->SetBool("TestBool", true);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }


    /////////////////////////////////////////////////
    OSIndependantLayer::PrintToConsole("  Test: Adding nested groups.");
    err = g_TestConfig.CreateSection(
                            testGroup,
                            "Nested Test Group",
                            &nestedGroup1);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    err = nestedGroup1->SetString("TestGroup", "This is a test string.");
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }
    err = nestedGroup1->SetInt("TestInt", 52);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }
    err = nestedGroup1->SetBool("TestBool", true);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }


    err = g_TestConfig.CreateSection(
                            testGroup,
                            "Nested Test Group 2",
                            &nestedGroup2);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    err = nestedGroup2->SetString("TestGroup", "This is a test string.");
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }
    err = nestedGroup2->SetInt("TestInt", 52);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }
    err = nestedGroup2->SetBool("TestBool", true);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    /////////////////////////////////////////////////
    OSIndependantLayer::PrintToConsole("  Test: Writing a config file.");
    err = g_TestConfig.Save();
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        goto abort;
    }

    OSIndependantLayer::PrintToConsole("\n");
    return;
    
abort:
    OSIndependantLayer::PrintToConsole("\nError!!!\n\n");
    return;
} // TestConfig.


#endif // INCLUDE_REGRESSION_TESTS


