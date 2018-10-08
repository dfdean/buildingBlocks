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
// Parsed URL Module
//
// This object parses and records the fields in a URL.
/////////////////////////////////////////////////////////////////////////////

#if LINUX
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include "osIndependantLayer.h"
#include "config.h"
#include "log.h"
#include "debugging.h"
#include "memAlloc.h"
#include "refCount.h"
#include "threads.h"
#include "stringLib.h"
#include "stringParse.h"
#include "url.h"

FILE_DEBUGGING_GLOBALS(LOG_LEVEL_DEFAULT, 0);


#define DEFAULT_PORT_FTP        25

// It is not uncommon for URL's to have 10 or more query variables.
#define MAX_QUERY_FIELDS        20

#define MAX_SCHEME_NAME_LENGTH  30


enum URL_TOKEN_TYPES {
   SCHEME_TOKEN      = 1,
   HOST_TOKEN        = 2,
   PATH_TOKEN        = 3,
   FRAGMENT_TOKEN    = 4,
   PORT_TOKEN        = 5,
   QUERY_NAME_TOKEN  = 6,
   QUERY_VALUE_TOKEN = 7,
   USER_NAME_TOKEN   = 8,
};

DEFINE_GRAMMAR(g_URLGrammar)
    DEFINE_RULE("", "[<scheme>]<path>[#<fragment>][?<query>]")

    DEFINE_TOKEN("<scheme>", "https://<hostRegion>", SCHEME_TOKEN, CParsedUrl::URL_SCHEME_HTTPS)
    DEFINE_TOKEN("<scheme>", "ip://<hostRegion>", SCHEME_TOKEN, CParsedUrl::URL_SCHEME_IP_ADDRESS)
    DEFINE_TOKEN("<scheme>", "ftp://<hostRegion>", SCHEME_TOKEN, CParsedUrl::URL_SCHEME_FTP)
    DEFINE_TOKEN("<scheme>", "http://<hostRegion>", SCHEME_TOKEN, CParsedUrl::URL_SCHEME_HTTP)
    DEFINE_TOKEN("<scheme>", "memory://", SCHEME_TOKEN, CParsedUrl::URL_SCHEME_MEMORY)
    DEFINE_TOKEN("<scheme>", "empty://", SCHEME_TOKEN, CParsedUrl::URL_SCHEME_EMPTY)
    DEFINE_TOKEN("<scheme>", "file://", SCHEME_TOKEN, CParsedUrl::URL_SCHEME_FILE)
    DEFINE_TOKEN("<scheme>", "urn://<hostRegion>", SCHEME_TOKEN, CParsedUrl::URL_SCHEME_URN)

    DEFINE_RULE("<hostRegion>", "[<userName>@]<host>[:<port>]")
    DEFINE_TOKEN("<host>", "*(ALPHANUM / . / _ / -)", HOST_TOKEN, 0)
    DEFINE_INTEGER_TOKEN("<port>", "*(DIGIT)", PORT_TOKEN)
    DEFINE_TOKEN("<userName>", "*(ALPHANUM / : )", USER_NAME_TOKEN, 0)

    DEFINE_TOKEN("<path>", "*(URL_PATH_CHAR)", PATH_TOKEN, 0)

    DEFINE_TOKEN("<fragment>", "*(URL_FRAGMENT_CHAR)", FRAGMENT_TOKEN, 0)

    DEFINE_RULE("<query>", "<queryEntry>*[&<queryEntry>]")
    DEFINE_RULE("<queryEntry>", "<queryName>[<queryValue>]")
    DEFINE_TOKEN("<queryName>", "*(URL_QUERY_CHAR)", QUERY_NAME_TOKEN, 0)
    DEFINE_RULE("<queryValue>", "=[<queryValueData>]")
    DEFINE_TOKEN("<queryValueData>", "*(URL_QUERY_CHAR)", QUERY_VALUE_TOKEN, 0)
STOP_GRAMMAR(g_URLGrammar);


// File types that we recognize.
struct CFileContentType {
    const char *suffix;
    int suffixLen;
    int16 contentType;
    int16 contentSubType;
};
static CFileContentType g_FileContentTypes[] = {
    { ".gif", 4, CONTENT_TYPE_IMAGE, CONTENT_SUBTYPE_IMAGE_GIF }, // image/gif
    { ".tif", 4, CONTENT_TYPE_IMAGE, CONTENT_SUBTYPE_IMAGE_TIF }, // image/tif
    { ".jpeg", 5, CONTENT_TYPE_IMAGE, CONTENT_SUBTYPE_IMAGE_JPEG }, // image/jpeg
    { ".jpg", 4, CONTENT_TYPE_IMAGE, CONTENT_SUBTYPE_IMAGE_JPEG }, // image/jpeg
    { ".jpe", 4, CONTENT_TYPE_IMAGE, CONTENT_SUBTYPE_IMAGE_JPEG }, // image/jpeg

    { ".htm", 4, CONTENT_TYPE_TEXT, CONTENT_SUBTYPE_TEXT_HTML }, // text/html
    { ".html", 5, CONTENT_TYPE_TEXT, CONTENT_SUBTYPE_TEXT_HTML }, // text/html

    { ".txt", 4, CONTENT_TYPE_TEXT, CONTENT_SUBTYPE_TEXT_PLAIN }, // text/plain
    { ".text", 5, CONTENT_TYPE_TEXT, CONTENT_SUBTYPE_TEXT_PLAIN }, // text/plain
    { ".cgi", 5, CONTENT_TYPE_TEXT, CONTENT_SUBTYPE_TEXT_DYNAMIC }, // text/plain
    { ".asp", 5, CONTENT_TYPE_TEXT, CONTENT_SUBTYPE_TEXT_DYNAMIC }, // text/plain
    { ".dll", 5, CONTENT_TYPE_TEXT, CONTENT_SUBTYPE_TEXT_DYNAMIC }, // text/plain

    { ".asp", 4, CONTENT_TYPE_TEXT, CONTENT_SUBTYPE_TEXT_HTML }, // text/html
    { ".cgi", 4, CONTENT_TYPE_TEXT, CONTENT_SUBTYPE_TEXT_HTML }, // text/html
    { ".aspx", 5, CONTENT_TYPE_TEXT, CONTENT_SUBTYPE_TEXT_HTML }, // text/html

    { ".pdf", 5, CONTENT_TYPE_TEXT, CONTENT_SUBTYPE_TEXT_PDF }, // text/pdf

    { ".wma", 4, CONTENT_TYPE_AUDIO, CONTENT_SUBTYPE_AUDIO_WMA }, // text/plain
    { ".asf", 4, CONTENT_TYPE_AUDIO, CONTENT_SUBTYPE_AUDIO_ASF }, // text/plain
    { ".rma", 4, CONTENT_TYPE_AUDIO, CONTENT_SUBTYPE_AUDIO_RMA }, // text/plain
    { ".mp3", 4, CONTENT_TYPE_AUDIO, CONTENT_SUBTYPE_AUDIO_MP3 }, // text/plain
    { ".wav", 4, CONTENT_TYPE_AUDIO, CONTENT_SUBTYPE_AUDIO_WAV }, // audio/wav

    { ".wmv", 4, CONTENT_TYPE_VIDEO, CONTENT_SUBTYPE_VIDEO_WMV }, // text/plain
    { ".mpg", 4, CONTENT_TYPE_VIDEO, CONTENT_SUBTYPE_VIDEO_MPEG }, // text/plain
    { ".mpeg", 5, CONTENT_TYPE_VIDEO, CONTENT_SUBTYPE_VIDEO_MPEG }, // text/plain

    { ".exe", 4, CONTENT_TYPE_SOFTWARE, CONTENT_SUBTYPE_SOFTWARE_APPLICATION }, // software/plain

    { ".css", 4, CONTENT_TYPE_APPLICATION, CONTENT_SUBTYPE_APPLICATION_CSS },

    { "", 0, 0, 0 }
}; // g_FileContentTypes



// This is the list of URL schemes and some of the protocol properties
// for each scheme.
struct CSchemeFlagsType {
    int16 schemeId;
    int32 flags;
    const char *schemeName;
    int schemeNameLength;
    int defaultPort;
};
static CSchemeFlagsType g_CURLSchemeList[] = {
    { CParsedUrl::URL_SCHEME_UNKNOWN, 0, "", 0, 0 },
    { CParsedUrl::URL_SCHEME_HTTP, CParsedUrl::ABSOLUTE_URL | CParsedUrl::NETWORK_NAME, "http://", 7, CParsedUrl::DEFAULT_PORT_HTTP },
    { CParsedUrl::URL_SCHEME_FTP, CParsedUrl::ABSOLUTE_URL | CParsedUrl::NETWORK_NAME, "ftp://", 6, DEFAULT_PORT_FTP },
    { CParsedUrl::URL_SCHEME_FILE, CParsedUrl::ABSOLUTE_URL, "file://", 7, 0 },
    { CParsedUrl::URL_SCHEME_IP_ADDRESS, CParsedUrl::ABSOLUTE_URL | CParsedUrl::NETWORK_NAME, "ip://", 5, 0 },
    { CParsedUrl::URL_SCHEME_MEMORY, CParsedUrl::ABSOLUTE_URL, "memory://", 9, 0 },
    { CParsedUrl::URL_SCHEME_EMPTY, CParsedUrl::ABSOLUTE_URL, "empty://", 8, 0 },
    { CParsedUrl::URL_SCHEME_HTTPS, CParsedUrl::ABSOLUTE_URL | CParsedUrl::NETWORK_NAME, "https://", 8, CParsedUrl::DEFAULT_PORT_HTTP },
    { CParsedUrl::URL_SCHEME_URN, CParsedUrl::ABSOLUTE_URL | CParsedUrl::NETWORK_NAME, "urn://", 8, CParsedUrl::DEFAULT_PORT_HTTP }
}; // g_CURLSchemeList







/////////////////////////////////////////////////////////////////////////////
//
// [AllocateFileUrl]
//
/////////////////////////////////////////////////////////////////////////////
CParsedUrl *
CParsedUrl::AllocateFileUrl(const char *pPath) {
    ErrVal err = ENoErr;
    CParsedUrl *pUrl = NULL;
    char pathBuffer[MAX_URL_LENGTH];

    if (NULL == pPath) {
        gotoErr(EFail);
    }
    DEBUG_LOG("AllocateFileUrl. url = %s", pPath);

    // Avoid possible attacks or bugs with unreasonably
    // long URL's.
    if ((g_CURLSchemeList[URL_SCHEME_FILE].schemeNameLength + strlen(pPath) + 4)
            >= MAX_URL_LENGTH) {
        gotoErr(EInvalidUrl);
    }

    pUrl = newex CParsedUrl;
    if (NULL == pUrl) {
        gotoErr(EFail);
    }

    snprintf(
        pathBuffer,
        MAX_URL_LENGTH - 1,
        "%s%s",
        g_CURLSchemeList[URL_SCHEME_FILE].schemeName,
        pPath);
    // snprintf does not guarantee that it is null terminated.
    pathBuffer[sizeof(pathBuffer) - 1] = 0;

    err = pUrl->Initialize(pathBuffer, strlen(pathBuffer), NULL);
    if (err) {
        gotoErr(err);
    }

    return(pUrl);

abort:
    RELEASE_OBJECT(pUrl);
    return(NULL);
} // AllocateFileUrl.






/////////////////////////////////////////////////////////////////////////////
//
// [AllocateMemoryUrl]
//
/////////////////////////////////////////////////////////////////////////////
CParsedUrl *
CParsedUrl::AllocateMemoryUrl(const char *pPtr, int32 length, int32 numValidBytes) {
    ErrVal err = ENoErr;
    CParsedUrl *pUrl = NULL;
    char pathBuffer[MAX_URL_LENGTH];

    pUrl = newex CParsedUrl;
    if (NULL == pUrl) {
        gotoErr(EFail);
    }

    DEBUG_LOG("AllocateMemoryUrl. length = %d, numValidBytes = %d", length, numValidBytes);
    snprintf(
        pathBuffer,
        MAX_URL_LENGTH - 1,
        "%s%p/%d/%d",
        g_CURLSchemeList[URL_SCHEME_MEMORY].schemeName,
        pPtr,
        length,
        numValidBytes);

    err = pUrl->Initialize(pathBuffer, strlen(pathBuffer), NULL);
    if (err) {
        gotoErr(err);
    }

    return(pUrl);

abort:
    RELEASE_OBJECT(pUrl);
    return(NULL);
} // AllocateMemoryUrl.





/////////////////////////////////////////////////////////////////////////////
//
// [AllocateUrl]
//
/////////////////////////////////////////////////////////////////////////////
CParsedUrl *
CParsedUrl::AllocateUrl(const char *pPath) {
    ErrVal err = ENoErr;
    CParsedUrl *pUrl = NULL;

    if (NULL == pPath) {
        gotoErr(EFail);
    }
    DEBUG_LOG("AllocateUrl. pPath = %s", pPath);

    pUrl = newex CParsedUrl;
    if (NULL == pUrl) {
        gotoErr(EFail);
    }

    err = pUrl->Initialize(pPath, strlen(pPath), NULL);
    if (err) {
        gotoErr(err);
    }

    return(pUrl);

abort:
    RELEASE_OBJECT(pUrl);
    return(NULL);
} // AllocateUrl.





/////////////////////////////////////////////////////////////////////////////
//
// [AllocateUrl]
//
/////////////////////////////////////////////////////////////////////////////
CParsedUrl *
CParsedUrl::AllocateUrl(const char *pPath, int32 pathLen, const char *pBaseUrlStr) {
    ErrVal err = ENoErr;
    CParsedUrl *pUrl = NULL;

    if ((NULL == pPath) || (pathLen < 0)) {
        gotoErr(EFail);
    }
    DEBUG_LOG("AllocateUrl. pPath = %p, pathLen = %d", pPath, pathLen);

    pUrl = newex CParsedUrl;
    if (NULL == pUrl) {
        gotoErr(EFail);
    }

    err = pUrl->InitializeEx(
                     pPath,
                     pathLen,
                     NULL, // pBaseUrl
                     pBaseUrlStr,
                     false); // fPreEncoded
    if (err) {
        gotoErr(err);
    }

    return(pUrl);

abort:
    RELEASE_OBJECT(pUrl);
    return(NULL);
} // AllocateUrl.





/////////////////////////////////////////////////////////////////////////////
//
// [CParsedUrl]
//
/////////////////////////////////////////////////////////////////////////////
CParsedUrl::CParsedUrl() {
    m_pParsingBuffer = NULL;
    m_pExtraParams = NULL;
    m_pGeneratedAbsoluteURL = NULL;

    // Initialize the parsed URL descriptor. An empty user
    // name is different than no user name, so this is
    // different than length set to 0.
    m_Flags = 0;
    m_Scheme = URL_SCHEME_UNKNOWN;
    m_Port = DEFAULT_PORT_HTTP;

    m_AbsoluteUrlSize = 0;
    m_HostNameSize = 0;
    m_UserNameSize = 0;
    m_PathSize = 0;
    m_FragmentSize = 0;

    m_pHostName = NULL;
    m_pUserName = NULL;
    m_pPath = NULL;
    m_pFragment = NULL;

    m_pSockAddr = NULL;

    m_NumQueryFields = 0;
    m_QueryNameList = NULL;
    m_QueryValueList = NULL;
    m_QueryNameSizeList = NULL;
    m_QueryValueSizeList = NULL;

    m_BuiltInBuffer[0] = 0;
} // CParsedUrl.





/////////////////////////////////////////////////////////////////////////////
//
// [~CParsedUrl]
//
/////////////////////////////////////////////////////////////////////////////
CParsedUrl::~CParsedUrl() {
    if (m_pSockAddr) {
        memFree(m_pSockAddr);
        m_pSockAddr = NULL;
    }

    if ((m_pParsingBuffer) && (m_pParsingBuffer != m_BuiltInBuffer)) {
        memFree(m_pParsingBuffer);
    }

    if (m_pGeneratedAbsoluteURL) {
        memFree(m_pGeneratedAbsoluteURL);
    }

    if (m_pExtraParams) {
        memFree(m_pExtraParams);
    }

    if (m_QueryNameList) {
        memFree(m_QueryNameList);
    }
    if (m_QueryValueList) {
        memFree(m_QueryValueList);
    }
    if (m_QueryNameSizeList) {
        memFree(m_QueryNameSizeList);
    }
    if (m_QueryValueSizeList) {
        memFree(m_QueryValueSizeList);
    }
} // ~CParsedUrl.





/////////////////////////////////////////////////////////////////////////////
//
// [Initialize]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CParsedUrl::Initialize(const char *pUrl, int32 urlLength, CParsedUrl *pBaseUrl) {
    return(InitializeEx(pUrl, urlLength, pBaseUrl, NULL, false));
} // Initialize.







/////////////////////////////////////////////////////////////////////////////
//
// [InitializeEx]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CParsedUrl::InitializeEx(
                  const char *pUrl,
                  int32 urlLength,
                  CParsedUrl *pBaseUrl,
                  const char *pBaseUrlStr,
                  bool fPreEncoded) {
    ErrVal err = ENoErr;
    int32 totalLen;
    int32 absoluteUrlBufferSize;
    char *pDestPtr;
    char *pEndDestPtr;
    const char *pSrcPtr;
    const char *pEndSrcPtr;
    char *pEndPtr;
    const char *pAbsoluteUrlSchemeMarker;
    int i;
    const char *pStartRelativeUrlBody;
    const char *pStopBasePtr;
    int16 numSlashesInBase = 0;
    int16 numSlashesInRelative = 0;
    char tempBaseUrlBuffer[MAX_URL_LENGTH];
    bool fParsedString;


    if ((NULL == pUrl) || (urlLength < 0)) {
        gotoErr(EFail);
    }

    // Discard any previous buffer.
    if ((m_pParsingBuffer) && (m_pParsingBuffer != m_BuiltInBuffer)) {
        memFree(m_pParsingBuffer);
    }
    m_pParsingBuffer = NULL;
    if (m_pGeneratedAbsoluteURL) {
        memFree(m_pGeneratedAbsoluteURL);
        m_pGeneratedAbsoluteURL = NULL;
    }

    // Compute the size of the new url. This will be a combination of
    // the relative and base url. This is slightly larger than needed.
    totalLen = CStringLib::GetMaxEncodedLength(
                                        CStringLib::URL_ENCODING,
                                        pUrl,
                                        urlLength);
    totalLen += 6;
    if (pBaseUrl) {
        totalLen += pBaseUrl->m_AbsoluteUrlSize;
    }
    if (pBaseUrlStr) {
        totalLen += CStringLib::GetMaxEncodedLength(
                                            CStringLib::URL_ENCODING,
                                            pBaseUrlStr,
                                            strlen(pBaseUrlStr));
    }


    // Avoid possible attacks or bugs with unreasonably long URL's.
    if (totalLen >= MAX_URL_LENGTH) {
        gotoErr(EInvalidUrl);
    }

    // Allocate storage for the url.
    totalLen += 2;
    if (totalLen < BUILTIN_BUFFER_SIZE) {
        m_pParsingBuffer = m_BuiltInBuffer;
        absoluteUrlBufferSize = sizeof(m_BuiltInBuffer);
    } else
    {
        DEBUG_LOG("CParsedUrl::InitializeEx. Allocating a large URL. totalLen = %d", totalLen);
        m_pParsingBuffer = (char *) memAlloc(totalLen);
        if (!m_pParsingBuffer) {
            returnErr(EFail);
        }
        absoluteUrlBufferSize = totalLen;
    }

    // Look for the :// pattern to test if this is an absolute URL.
    pAbsoluteUrlSchemeMarker = NULL;
    pSrcPtr = pUrl;
    pEndSrcPtr = pUrl + urlLength;
    if (urlLength > MAX_SCHEME_NAME_LENGTH) {
        pEndSrcPtr = pUrl + MAX_SCHEME_NAME_LENGTH;
    }
    while ((pSrcPtr + 3) < pEndSrcPtr) {
        if ((':' == pSrcPtr[0])
            && ('/' == pSrcPtr[1])
            && ('/' == pSrcPtr[2])) {
            pAbsoluteUrlSchemeMarker = pSrcPtr;
            break;
        }
        pSrcPtr++;
    }

    // Copy the url into the private storage. If there is a base URL,
    // then this requires merging the base and relative url's.
    pDestPtr = m_pParsingBuffer;
    if (((pBaseUrl) || (pBaseUrlStr))
        && (('/' == *pUrl) || (NULL == pAbsoluteUrlSchemeMarker))) {
        DEBUG_LOG("CParsedUrl::InitializeEx. Merge a relative URL");

        pEndSrcPtr = pUrl + urlLength;

        // Find out how many slashes the relative url starts with.
        // This determines where in the base url we will substitute
        // with the relative url.
        pStartRelativeUrlBody = pUrl;
        numSlashesInRelative = 0;
        while (('/' == *pStartRelativeUrlBody)
                && (pStartRelativeUrlBody < pEndSrcPtr)) {
            numSlashesInRelative++;
            pStartRelativeUrlBody++;
        }

        // Print the base url. This re-encodes all decoded characters.
        if (NULL != pBaseUrl) {
            err = pBaseUrl->PrintToString(
                                CParsedUrl::ENTIRE_URL,
                                tempBaseUrlBuffer,
                                sizeof(tempBaseUrlBuffer),
                                &pEndPtr);
            if (err) {
                gotoErr(err);
            }
            pBaseUrlStr = tempBaseUrlBuffer;
        } else if (NULL == pBaseUrlStr) {
            tempBaseUrlBuffer[0] = 0;
            pBaseUrlStr = tempBaseUrlBuffer;
        }

        // Find how much of the base URL we use. If the relative url doesn't
        // have / chars, then this is the last / in the base. Otherwise, this
        // is the last occurrence of where the same number of slashes appear
        // in the base url.
        if (0 == numSlashesInRelative) {
            pStopBasePtr = pBaseUrlStr + strlen(pBaseUrlStr) - 1;
            while (pStopBasePtr > pBaseUrlStr) {
                if (('/' == *pStopBasePtr) || ('\\' == *pStopBasePtr)) {
                    pStopBasePtr++;
                    break;
                }
                pStopBasePtr--;
            }
        } else {
            pStopBasePtr = pBaseUrlStr;
            while (*pStopBasePtr) {
                if (('/' == *pStopBasePtr) || ('\\' == *pStopBasePtr)) {
                    numSlashesInBase++;
                } // processing a / in the base URL.
                else {
                    if (numSlashesInBase == numSlashesInRelative)
                    {
                        break;
                    }
                    numSlashesInBase = 0;
                }
                pStopBasePtr++;
            }
        } // finding how much of the base url to use.

        // Copy from the base URL up to the overlap point.
        pDestPtr = m_pParsingBuffer;
        pEndDestPtr = m_pParsingBuffer + absoluteUrlBufferSize;
        pSrcPtr = pBaseUrlStr;
        while ((pSrcPtr < pStopBasePtr) && (pDestPtr < pEndDestPtr)) {
            *(pDestPtr++) = *(pSrcPtr++);
        }

        // Copy from the rest of the relative pointer.
        pSrcPtr = pStartRelativeUrlBody;
        while ((pSrcPtr < pEndSrcPtr) && (pDestPtr < pEndDestPtr)) {
            *(pDestPtr++) = *(pSrcPtr++);
        }

        if (pDestPtr < pEndDestPtr) {
            *(pDestPtr) = 0;
        }

        m_AbsoluteUrlSize = pDestPtr - m_pParsingBuffer;
    } else
    {
        strncpyex(m_pParsingBuffer, pUrl, urlLength);
        m_AbsoluteUrlSize = (int16) urlLength;
    }


    m_Flags = 0;
    m_Scheme = URL_SCHEME_UNKNOWN;
    m_pHostName = NULL;
    m_HostNameSize = 0;
    m_Port = DEFAULT_PORT_HTTP;
    m_pUserName = NULL;
    m_UserNameSize = 0;
    m_pPath = NULL;
    m_PathSize = 0;
    m_pFragment = NULL;
    m_FragmentSize = 0;
    m_NumQueryFields = 0;
    m_Port = 0;

    fParsedString = g_URLGrammar.ParseString(m_pParsingBuffer, m_AbsoluteUrlSize, this, NULL);
    if (!fParsedString) {
        ASSERT(0);
        gotoErr(err);
    }

    // After we are done parsing, then decode the url. This may replace
    // encoded characters with # or other special characters that would have
    // disrupted parsing.
    if (fPreEncoded) {
        m_Flags |= URL_IS_PREENCODED;
    } else
    {
        bool fShrunkString;

        UrlDecodeString(m_pHostName, &m_HostNameSize, &fShrunkString);
        UrlDecodeString(m_pUserName, &m_UserNameSize, &fShrunkString);
        UrlDecodeString(m_pPath, &m_PathSize, &fShrunkString);
        UrlDecodeString(m_pFragment, &m_FragmentSize, &fShrunkString);
        for (i = 0; i < m_NumQueryFields; i++) {
            UrlDecodeString(m_QueryNameList[i], &(m_QueryNameSizeList[i]), &fShrunkString);
            UrlDecodeString(m_QueryValueList[i], &(m_QueryValueSizeList[i]), &fShrunkString);
        }
    }

abort:
    returnErr(err);
} // InitializeEx.









/////////////////////////////////////////////////////////////////////////////
//
// [Equal]
//
/////////////////////////////////////////////////////////////////////////////
bool
CParsedUrl::Equal(int32 urlSection, CParsedUrl *pUrl) {
    if (NULL == pUrl) {
        return(false);
    }

    /////////////////////////////////////////////////
    if ((ENTIRE_URL == urlSection)
        || (URL_HOST == urlSection)) {
        if ((NULL == m_pHostName)
           || (NULL == pUrl->m_pHostName)) {
            return(false);
        }

        // This is the quickest way to find a difference.
        if ((m_HostNameSize != pUrl->m_HostNameSize)
           || (m_Port != pUrl->m_Port)
           || (0 != strncasecmpex(m_pHostName, pUrl->m_pHostName, m_HostNameSize))) {
            return(false);
        }
    }

    /////////////////////////////////////////////////
    if (URL_PATH == urlSection) {
        if ((NULL == m_pPath)
           || (NULL == pUrl->m_pPath)) {
            return(false);
        }

        // These are the quickest way to find a difference.
        if ((m_PathSize != pUrl->m_PathSize)
           || (0 != strncasecmpex(m_pPath, pUrl->m_pPath, m_PathSize))) {
            return(false);
        }
    }

    /////////////////////////////////////////////////
    if (ENTIRE_URL == urlSection) {
        if (NULL == pUrl->m_pPath) {
            return(false);
        }

        // These are the quickest way to find a difference.
        if ((m_PathSize != pUrl->m_PathSize)
            || (m_HostNameSize != pUrl->m_HostNameSize)
            || (0 != strcasecmpex(m_pPath, pUrl->m_pPath))
            || (0 != strncasecmpex(m_pHostName, pUrl->m_pHostName, m_HostNameSize))) {
            return(false);
        }
    }

    return(true);
} // Equal









/////////////////////////////////////////////////////////////////////////////
//
// [PrintToString]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CParsedUrl::PrintToString(
                    int32 urlSection,
                    char *pDestPtr,
                    int32 bufferLength,
                    char **destResult) {
    ErrVal err = ENoErr;
    char *pEndDestPtr;

    if ((NULL == pDestPtr) || (bufferLength <= 0)) {
        returnErr(EFail);
    }
    pEndDestPtr = pDestPtr + bufferLength;

    // Leave space for the NULL terminator.
    pEndDestPtr = pEndDestPtr - 2;

    ///////////////////////////////////////////////////
    // Scheme.
    if ((CParsedUrl::ENTIRE_URL == urlSection)
        && (URL_SCHEME_UNKNOWN != m_Scheme)) {
        if ((pDestPtr + g_CURLSchemeList[m_Scheme].schemeNameLength) >= pEndDestPtr) {
            gotoErr(EInvalidUrl);
        }
        strncpyex(
            pDestPtr,
            g_CURLSchemeList[m_Scheme].schemeName,
            pEndDestPtr - pDestPtr);
        pDestPtr += g_CURLSchemeList[m_Scheme].schemeNameLength;
    }

    ///////////////////////////////////////////////////
    // HostName
    if (CParsedUrl::ENTIRE_URL == urlSection) {
        // User name
        if (m_pUserName) {
            pDestPtr = WriteString(
                            pDestPtr,
                            pEndDestPtr - pDestPtr,
                            m_pUserName,
                            m_UserNameSize);
            if ((pDestPtr + 1) >= pEndDestPtr) {
                gotoErr(EInvalidUrl);
            }
            *(pDestPtr++) = '@';
        } // printing the user name.

        // Host Name
        if ((m_pHostName) && (m_HostNameSize > 0)) {
            pDestPtr = WriteString(
                            pDestPtr,
                            pEndDestPtr - pDestPtr,
                            m_pHostName,
                            m_HostNameSize);
        }

        // Port
        if (m_Flags & PORT_IS_SPECIFIED) {
            if ((pDestPtr + 1) >= pEndDestPtr) {
                gotoErr(EInvalidUrl);
            }
            *(pDestPtr++) = ':';
            snprintf(pDestPtr, pEndDestPtr - pDestPtr, "%d", (int32) m_Port);
            pDestPtr = pDestPtr + strlen(pDestPtr);
        } // printing the port.
    } // HostName


    ///////////////////////////////////////////////////
    // Path
    if ((NULL != m_pPath)
        && ((CParsedUrl::ENTIRE_URL == urlSection)
            || (CParsedUrl::URL_PATH == urlSection)
            || (CParsedUrl::PATH_AND_SUFFIX == urlSection))) {
        pDestPtr = WriteString(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        m_pPath,
                        m_PathSize);
    } // Path


    ///////////////////////////////////////////////////
    // Fragment
    if ((NULL != m_pFragment)
        && ((CParsedUrl::ENTIRE_URL == urlSection)
            || (CParsedUrl::PATH_AND_SUFFIX == urlSection))) {
        if ((pDestPtr + m_FragmentSize + 1) >= pEndDestPtr) {
            returnErr(EInvalidUrl);
        }

        *(pDestPtr++) = '#';

        pDestPtr = WriteString(
                        pDestPtr,
                        pEndDestPtr - pDestPtr,
                        m_pFragment,
                        m_FragmentSize);
    } // Fragment


    ///////////////////////////////////////////////////
    // Queries
    if ((m_NumQueryFields > 0)
        && ((CParsedUrl::ENTIRE_URL == urlSection)
            || (CParsedUrl::PATH_AND_SUFFIX == urlSection))) {
        int queryNum;

        if ((pDestPtr + 1) >= pEndDestPtr) {
            gotoErr(EInvalidUrl);
        }
        *(pDestPtr++) = '?';


        for (queryNum = 0; queryNum < m_NumQueryFields; queryNum++) {
            pDestPtr = WriteString(
                            pDestPtr,
                            pEndDestPtr - pDestPtr,
                            m_QueryNameList[queryNum],
                            m_QueryNameSizeList[queryNum]);

            // There may not be a value for every name. We can have
            // either name=value&name=value or else name&name=value
            // or some other combination.
            if (m_QueryValueList[queryNum]) {
                if ((pDestPtr + 1) >= pEndDestPtr) {
                    gotoErr(EInvalidUrl);
                }
                *(pDestPtr++) = '=';

                pDestPtr = WriteString(
                                pDestPtr,
                                pEndDestPtr - pDestPtr,
                                m_QueryValueList[queryNum],
                                m_QueryValueSizeList[queryNum]);
            }

            // If there are more queries to come, then separate them.
            if ((queryNum + 1) < m_NumQueryFields) {
                if ((pDestPtr + 1) >= pEndDestPtr) {
                    returnErr(EInvalidUrl);
                }
                *(pDestPtr++) = '&';
            }
        } // for (queryNum = 0; queryNum < m_NumQueryFields; queryNum++)
    } // Queries


    if (pDestPtr < pEndDestPtr) {
        *(pDestPtr++) = 0;
    }

    if (NULL != destResult) {
        *destResult = pDestPtr;
    }

abort:
    returnErr(err);
} // PrintToString.









/////////////////////////////////////////////////////////////////////////////
//
// [GetImpliedContentType]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CParsedUrl::GetImpliedContentType(int16 *pType, int16 *pSubType) {
    ErrVal err = ENoErr;
    CFileContentType *info;
    const char *endOfFileName;
    const char *startSuffix;
    const char *endSuffix;
    int32 maxSuffixLength;

    if ((NULL == pType) || (NULL == pSubType)) {
        gotoErr(EFail);
    }
    // Default to text.
    *pType = CONTENT_TYPE_TEXT;
    *pSubType = CONTENT_SUBTYPE_TEXT_HTML;


    // If there is no path, then this is the default file
    // in the root directory. Typically, this is a text file.
    if ((NULL == m_pPath) || (m_PathSize <= 0)) {
        DEBUG_LOG("CParsedUrl::GetImpliedContentType. Defaulting to text");
        gotoErr(ENoErr);
    }

    endOfFileName = m_pPath + m_PathSize;
    startSuffix = endOfFileName - 1;
    while ((startSuffix > m_pPath) && ('.' != *startSuffix)) {
       startSuffix--;
    }
    maxSuffixLength = endOfFileName - startSuffix;

    // See if this file name matches any known file type.
    info = g_FileContentTypes;
    while ((info->suffix) && ((info->suffix)[0])) {
        if (maxSuffixLength >= info->suffixLen) {
            if (strncasecmpex(startSuffix, info->suffix, info->suffixLen) == 0) {
                endSuffix = startSuffix + info->suffixLen;
                if ((endSuffix >= endOfFileName)
                     || (0 == *endSuffix)
                     || ('#' == *endSuffix)
                     || ('?' == *endSuffix)) {
                    DEBUG_LOG("CParsedUrl::GetImpliedContentType. Found match. type = %d, subtype = %d",
                                info->contentType, info->contentSubType);
                    *pType = info->contentType;
                    *pSubType = info->contentSubType;
                    break;
                }
            }
        }

        info++;
    } // examining every known file suffix type.

abort:
    returnErr(err);
} // GetImpliedContentType





/////////////////////////////////////////////////////////////////////////////
//
// [OnParseToken]
//
// CParsingCallback
/////////////////////////////////////////////////////////////////////////////
ErrVal
CParsedUrl::OnParseToken(
                  void *pCallbackContext,
                  int32 tokenId,
                  int32 intValue,
                  const char *pStr,
                  int32 length) {
    ErrVal err = ENoErr;
    pCallbackContext = pCallbackContext; // Unused

    switch (tokenId) {
    case SCHEME_TOKEN:
        m_Scheme = intValue;
        m_Flags |= g_CURLSchemeList[m_Scheme].flags;
        if (0 == m_Port) {
            m_Port = g_CURLSchemeList[m_Scheme].defaultPort;
        }
        break;

    case HOST_TOKEN:
        m_pHostName = (char *) pStr;
        m_HostNameSize = length;
        break;

    case USER_NAME_TOKEN:
        m_pUserName = (char *) pStr;
        m_UserNameSize = length;
        break;

    case PATH_TOKEN:
        m_pPath = (char *) pStr;
        m_PathSize = length;
        break;

    case FRAGMENT_TOKEN:
        m_pFragment = (char *) pStr;
        m_FragmentSize = length;
        break;

    case PORT_TOKEN:
        m_Port = intValue;
        m_Flags |= PORT_IS_SPECIFIED;
        break;

    case QUERY_NAME_TOKEN:
        // Make sure we have query fields. We may already have these if
        // there already were query values.
        err = AllocateQueryFieldLists();
        if (err) {
            gotoErr(err);
        }
        if (m_NumQueryFields >= MAX_QUERY_FIELDS) {
            gotoErr(EInvalidUrl);
        }

        m_QueryNameList[m_NumQueryFields] = (char *) pStr;
        m_QueryNameSizeList[m_NumQueryFields] = length;

        m_QueryValueList[m_NumQueryFields] = NULL;
        m_QueryValueSizeList[m_NumQueryFields] = 0;

        m_NumQueryFields++;
        break;

    case QUERY_VALUE_TOKEN:
        if (m_NumQueryFields <= 0) {
            gotoErr(EInvalidUrl);
        }
        m_QueryValueList[m_NumQueryFields - 1] = (char *) pStr;
        m_QueryValueSizeList[m_NumQueryFields - 1] = length;
        break;

    default:
        break;
    } // switch (tokenId)

abort:
    returnErr(err);
} // OnParseToken







////////////////////////////////////////////////////////////////////////////////
//
// [WriteString]
//
////////////////////////////////////////////////////////////////////////////////
char *
CParsedUrl::WriteString(
                  char *pDestPtr,
                  int32 maxDestLength,
                  char *pSrcPtr,
                  int32 srcLength) {
    if (m_Flags & URL_IS_PREENCODED) {
        if (srcLength < maxDestLength) {
            strncpyex(pDestPtr, pSrcPtr, srcLength);
            pDestPtr += srcLength;
        }
    } else
    {
        pDestPtr += CStringLib::EncodeString(
                                        CStringLib::URL_ENCODING,
                                        pDestPtr,
                                        maxDestLength,
                                        pSrcPtr,
                                        srcLength);
    }

    return(pDestPtr);
} // WriteString







////////////////////////////////////////////////////////////////////////////////
//
// [UrlDecodeString]
//
////////////////////////////////////////////////////////////////////////////////
void
CParsedUrl::UrlDecodeString(char *pSrcPtr, int16 *pLength, bool *pfShrunkString) {
    if (NULL == pSrcPtr) {
        *pLength = 0;
        if (NULL != pfShrunkString) {
           *pfShrunkString = false;
        }
    } else
    {
        int16 originalLength = *pLength;
        *pLength = (int16) CStringLib::DecodeString(
                                           CStringLib::URL_ENCODING,
                                           pSrcPtr,
                                           originalLength,
                                           pSrcPtr,
                                           -1);
        if (NULL != pfShrunkString) {
           if (originalLength > *pLength)
           {
              *pfShrunkString = true;
           } else
           {
              *pfShrunkString = false;
           }
        }
    }
} // UrlDecodeString






/////////////////////////////////////////////////////////////////////////////
//
// [AllocateQueryFieldLists]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CParsedUrl::AllocateQueryFieldLists() {
    ErrVal err = ENoErr;

    // We lazily create the list of query fields.
    if (NULL == m_QueryNameList) {
        m_QueryNameList = (char **) (memAlloc(sizeof(char *) * MAX_QUERY_FIELDS));
        if (NULL == m_QueryNameList) {
            gotoErr(EFail);
        }
    }
    if (NULL == m_QueryValueList) {
        m_QueryValueList = (char **) (memAlloc(sizeof(char *) * MAX_QUERY_FIELDS));
        if (NULL == m_QueryValueList) {
            gotoErr(EFail);
        }
    }
    if (NULL == m_QueryNameSizeList) {
        m_QueryNameSizeList = (int16 *) memAlloc(sizeof(int16) * MAX_QUERY_FIELDS);
        if (NULL == m_QueryNameSizeList) {
            gotoErr(EFail);
        }
    }
    if (NULL == m_QueryValueSizeList) {
        m_QueryValueSizeList = (int16 *) memAlloc(sizeof(int16) * MAX_QUERY_FIELDS);
        if (NULL == m_QueryValueSizeList) {
            gotoErr(EFail);
        }
    }

abort:
    returnErr(err);
} // AllocateQueryFieldLists







/////////////////////////////////////////////////////////////////////////////
//
// [GetPrintableURL]
//
/////////////////////////////////////////////////////////////////////////////
char *
CParsedUrl::GetPrintableURL() {
    ErrVal err = ENoErr;

    if (NULL == m_pGeneratedAbsoluteURL) {
        err = CreatePrintedURL();
        if (err) {
            return(NULL);
        }
    }

    return(m_pGeneratedAbsoluteURL);
} // GetPrintableURL






/////////////////////////////////////////////////////////////////////////////
//
// [CreatePrintedURL]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CParsedUrl::CreatePrintedURL() {
    ErrVal err = ENoErr;
    char printStr[MAX_URL_LENGTH];

    if (m_pGeneratedAbsoluteURL) {
        gotoErr(ENoErr);
    }

    err = PrintToString(CParsedUrl::ENTIRE_URL, printStr, sizeof(printStr), NULL);
    if (err) {
        gotoErr(err);
    }

    m_AbsoluteUrlSize = strlen(printStr);
    m_pGeneratedAbsoluteURL = (char *) memAlloc(m_AbsoluteUrlSize + 2);
    if (NULL == m_pGeneratedAbsoluteURL) {
        gotoErr(EFail);
    }
    strncpyex(m_pGeneratedAbsoluteURL, printStr, m_AbsoluteUrlSize + 1);

abort:
    returnErr(err);
} // CreatePrintedURL







/////////////////////////////////////////////////////////////////////////////
//
//                     TESTING PROCEDURES
//
/////////////////////////////////////////////////////////////////////////////
#if INCLUDE_REGRESSION_TESTS

static void TestOneURL(const char *urlStr);

extern const char *g_TestURLList[];





/////////////////////////////////////////////////////////////////////////////
//
// [TestURL]
//
/////////////////////////////////////////////////////////////////////////////
void
CParsedUrl::TestURL() {
    ErrVal err = ENoErr;
    const char *test;
    CParsedUrl *pUrl;
    CParsedUrl *pBaseUrl;
    char printStr[MAX_URL_LENGTH];
    int index;

    g_DebugManager.StartModuleTest("URLs");


    pUrl = newex CParsedUrl;
    if (NULL == pUrl) {
        DEBUG_WARNING("Cannot allocate the url.");
        return;
    }
    pBaseUrl = newex CParsedUrl;
    if (NULL == pBaseUrl) {
        DEBUG_WARNING("Cannot allocate the url.");
        return;
    }

    g_DebugManager.StartTest("Trivial URL");
    TestOneURL("/abc");

    g_DebugManager.StartTest("URL with port and path and fragment and search");
    TestOneURL("http://userName@www.altavista.com:8080/path/a/b/c#frag?name1=value1&name2=value2&nameOnle1&name3=value3");

    g_DebugManager.StartTest("URL with no path and no expected <host>");
    TestOneURL("file://www.altavista.com");

    g_DebugManager.StartTest("URL with no path but rules allow a <host>");
    TestOneURL("http://www.altavista.com");

    g_DebugManager.StartTest("URL with 1 character path");
    TestOneURL("http://www.altavista.com/");

    g_DebugManager.StartTest("URL with port and 1 character path");
    TestOneURL("http://www.altavista.com:8080/");

    g_DebugManager.StartTest("URL with port and multiple character path");
    TestOneURL("http://www.altavista.com:8080/path/a/b/c");

    g_DebugManager.StartTest("URL with port and path and fragment");
    TestOneURL("http://www.altavista.com:8080/path/a/b/c#frag");

    g_DebugManager.StartTest("URL with port and path and search suffix");
    TestOneURL("http://www.altavista.com:8080/path/a/b/c?a=b");

    g_DebugManager.StartTest("URL with port and path and fragment and search");
    TestOneURL("http://www.altavista.com:8080/path/a/b/c#frag?a=b");

    g_DebugManager.StartTest("Relative url");
    TestOneURL("/path/a/b/c#frag?a=b");

    //<user>:<password>@<host>:<port>/<url-path>
    g_DebugManager.StartTest("User name but no password");
    TestOneURL("http://userName@www.altavista.com:8080/path/a/b/c#frag?a=b");

    //<user>:<password>@<host>:<port>/<url-path>
    g_DebugManager.StartTest("User name but no password");
    TestOneURL("http://userName:password@www.altavista.com:8080/path/a/b/c#frag?a=b");

    //IP Addresses
    g_DebugManager.StartTest("IP-address");

    TestOneURL("ip://127.0.0.1/");
    TestOneURL("ip://127.0.0.1");
    TestOneURL("ip://127.0.0.1:8080");
    TestOneURL("ip://127.0.0.1:8080/");

    TestOneURL("ip://www.altavista.com/");
    TestOneURL("ip://www.altavista.com");
    TestOneURL("ip://www.altavista.com:8080");
    TestOneURL("ip://www.altavista.com:8080/");

    TestOneURL("ip://userName:password@www.altavista.com/");
    TestOneURL("ip://userName:password@www.altavista.com");
    TestOneURL("ip://userName:password@www.altavista.com:8080");
    TestOneURL("ip://userName:password@www.altavista.com:8080/");

    TestOneURL("ip://userName:password@127.0.0.1.com/");
    TestOneURL("ip://userName:password@127.0.0.1.com");
    TestOneURL("ip://userName:password@127.0.0.1.com:8080");
    TestOneURL("ip://userName:password@127.0.0.1.com:8080/");


    // The parser will read this correctly, but output the suffixes in
    // the standard order, which is different than the url.
    // g_DebugManager.StartTest("URL with port and path and search and fragment");
    // TestOneURL("http://www.altavista.com:8080/path/a/b/c?a=b#frag");

    g_DebugManager.StartTest("Special * URL (used with OPTIONS http method)");
    TestOneURL("*");

    g_DebugManager.StartTest("File URL");
    TestOneURL("file://c:/dir1/dir2/fname.html");

    g_DebugManager.StartTest("Base URL");
    TestOneURL("http://www.altavista.com:8080/path/a/b/c/");



    g_DebugManager.StartTest("URL with query added later");
    const char *pLiteral = "http://www.altavista.com:8080/path/fileName.htm?a=b";
    err = pUrl->Initialize(pLiteral, strlen(pLiteral), NULL);
    if (err) {
        DEBUG_WARNING("Cannot parse the url.");
        return;
    }

    err = pUrl->PrintToString(CParsedUrl::ENTIRE_URL, printStr, sizeof(printStr), NULL);
    if (err) {
        DEBUG_WARNING("Cannot print the url.");
        return;
    }
    if (0 != strcasecmpex(pLiteral, printStr)) {
        DEBUG_WARNING("Printed url is different than the original.");
        return;
    }


    g_DebugManager.StartTest("Base URL with relative url");
    err = pBaseUrl->Initialize(
                        "http://www.altavista.com:8080/path/a/b/c/xxxxx.htm",
                        strlen("http://www.altavista.com:8080/path/a/b/c/xxxxx.htm"),
                        NULL);
    if (err) {
        DEBUG_WARNING("Cannot parse the url.");
        return;
    }
    err = pUrl->Initialize(
                    "/relativePath/relativeUrlFileName.cpp",
                    strlen("/relativePath/relativeUrlFileName.cpp"),
                    pBaseUrl);
    if (err) {
        DEBUG_WARNING("Cannot parse the url.");
        return;
    }
    err = pUrl->PrintToString(CParsedUrl::ENTIRE_URL, printStr, sizeof(printStr), NULL);
    if (err) {
        DEBUG_WARNING("Cannot print the url.");
        return;
    }
    if (0 != strcasecmpex("http://www.altavista.com:8080/relativePath/relativeUrlFileName.cpp", printStr)) {
        DEBUG_WARNING("Printed url is different than the original.");
        return;
    }



    g_DebugManager.StartTest("Base URL with trivial relative url");
    err = pBaseUrl->Initialize(
                        "http://www.altavista.com:8080/path/a/b/c/xxxxx.htm",
                        strlen("http://www.altavista.com:8080/path/a/b/c/xxxxx.htm"),
                        NULL);
    if (err) {
        DEBUG_WARNING("Cannot parse the url.");
        return;
    }
    err = pUrl->Initialize(
                    "/relativeUrlFileName.cpp",
                    strlen("/relativeUrlFileName.cpp"),
                    pBaseUrl);
    if (err) {
        DEBUG_WARNING("Cannot parse the url.");
        return;
    }
    err = pUrl->PrintToString(CParsedUrl::ENTIRE_URL, printStr, sizeof(printStr), NULL);
    if (err) {
        DEBUG_WARNING("Cannot print the pUrl->");
        return;
    }
    if (0 != strcasecmpex("http://www.altavista.com:8080/relativeUrlFileName.cpp", printStr)) {
        DEBUG_WARNING("Printed url is different than the original.");
        return;
    }


    ////////////////////////////////////////
    for (index = 0; ; index++) {
        test = g_TestURLList[index];
        if (NULL == test) {
            break;
        }

        g_DebugManager.StartTest(test);
        TestOneURL(test);
    }
} // TestURL.









/////////////////////////////////////////////////////////////////////////////
//
// [TestOneURL]
//
/////////////////////////////////////////////////////////////////////////////
static void
TestOneURL(const char *urlStr) {
    ErrVal err = ENoErr;
    char parseStr[CParsedUrl::MAX_URL_LENGTH];
    char printStr[CParsedUrl::MAX_URL_LENGTH];
    int i;
    CParsedUrl *pUrl;
    const char *pTestStr;

    pUrl = newex CParsedUrl;
    if (NULL == pUrl) {
        DEBUG_WARNING("Cannot allocate the url.");
        return;
    }

    strncpyex(parseStr, urlStr, sizeof(parseStr) - 1);

    err = pUrl->Initialize(parseStr, strlen(parseStr), NULL);
    if (err) {
        DEBUG_WARNING("Cannot parse the url.");
        return;
    }

    // Try printing to a string.
    err = pUrl->PrintToString(CParsedUrl::ENTIRE_URL, printStr, sizeof(printStr), NULL);
    if (err) {
        DEBUG_WARNING("Cannot print the url.");
        return;
    }

    if (0 != strcasecmpex(urlStr, printStr)) {
        pTestStr = urlStr;
        while (*pTestStr) {
            if ('~' == *pTestStr) {
                break;
            }
            pTestStr++;
        }

        if ('~' != *pTestStr) {
            DEBUG_WARNING("Printed url is different than the original.");
        }
        return;
    }


    // Trash the original string, to make sure the url is not affected.
    for (i = 0; i < 20; i++) {
        parseStr[i] = 0;
    }

    // Try printing to a string.
    err = pUrl->PrintToString(CParsedUrl::ENTIRE_URL, printStr, sizeof(printStr), NULL);
    if (err) {
        DEBUG_WARNING("Cannot print the url.");
        return;
    }

    if (0 != strcasecmpex(urlStr, printStr)) {
        DEBUG_WARNING("Printed url is different than the original.");
        return;
    }
} // TestOneURL.



#endif // INCLUDE_REGRESSION_TESTS



