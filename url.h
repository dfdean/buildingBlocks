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

#ifndef _PARSED_URL_H_
#define _PARSED_URL_H_


/////////////////////////////////////////////////////////////////////////////
enum MimeTypes  {
    // These are the recognized mime-types.
    CONTENT_TYPE_ANY                        = 0,
    CONTENT_TYPE_APPLICATION                = 1,
    CONTENT_TYPE_AUDIO                      = 2,
    CONTENT_TYPE_IMAGE                      = 3,
    CONTENT_TYPE_MESSAGE                    = 4,
    CONTENT_TYPE_MULTIPART                  = 5,
    CONTENT_TYPE_TEXT                       = 6,
    CONTENT_TYPE_VIDEO                      = 7,
    CONTENT_TYPE_SOFTWARE                   = 8,
    NUM_CONTENT_TYPES                       = 9,

    // These are the subtypes for each type.
    CONTENT_SUBTYPE_ANY                     = 100,

    // Text subtypes, type = CONTENT_TYPE_TEXT
    CONTENT_SUBTYPE_TEXT_HTML               = 201,
    CONTENT_SUBTYPE_TEXT_PLAIN              = 202,
    CONTENT_SUBTYPE_TEXT_ENRICHED           = 203,
    CONTENT_SUBTYPE_TEXT_XML                = 204,
    CONTENT_SUBTYPE_TEXT_PDF                = 205,
    CONTENT_SUBTYPE_TEXT_DYNAMIC            = 206,

    // Application subtypes, type = CONTENT_TYPE_APPLICATION
    CONTENT_SUBTYPE_APPLICATION_SOAPXML     = 301,
    CONTENT_SUBTYPE_APPLICATION_DIME        = 302,
    CONTENT_SUBTYPE_APPLICATION_UUENCODED_FORM = 303,
    CONTENT_SUBTYPE_APPLICATION_CSS         = 304,
    CONTENT_SUBTYPE_APPLICATION_MSWORD      = 305,
    CONTENT_SUBTYPE_APPLICATION_POSTSCRIPT  = 306,
    CONTENT_SUBTYPE_APPLICATION_RTF         = 307,
    CONTENT_SUBTYPE_APPLICATION_ZIP         = 308,
    CONTENT_SUBTYPE_APPLICATION_OCTET_STREAM = 309,
    CONTENT_SUBTYPE_APPLICATION_WORDPERFECT = 310,
    CONTENT_SUBTYPE_APPLICATION_EXCEL       = 311,
    CONTENT_SUBTYPE_APPLICATION_POWERPOINT  = 312,
    CONTENT_SUBTYPE_APPLICATION_XCOMET      = 313,

    // Image subtypes, type = CONTENT_TYPE_IMAGE
    CONTENT_SUBTYPE_IMAGE_GIF               = 401,
    CONTENT_SUBTYPE_IMAGE_JPEG              = 402,
    CONTENT_SUBTYPE_IMAGE_TIF               = 403,
    CONTENT_SUBTYPE_IMAGE_XBITMAP           = 404,
    CONTENT_SUBTYPE_IMAGE_PJPEG             = 405,

    // Audio subtypes, type = CONTENT_TYPE_AUDIO
    CONTENT_SUBTYPE_AUDIO_BASIC     = 501,
    CONTENT_SUBTYPE_AUDIO_WAV       = 502,
    CONTENT_SUBTYPE_AUDIO_WMA       = 503,
    CONTENT_SUBTYPE_AUDIO_RMA       = 504,
    CONTENT_SUBTYPE_AUDIO_MP3       = 505,
    CONTENT_SUBTYPE_AUDIO_ASF       = 506,

    // Video subtypes, type = CONTENT_TYPE_VIDEO
    CONTENT_SUBTYPE_VIDEO_MPEG      = 601,
    CONTENT_SUBTYPE_VIDEO_QUICKTIME = 602,
    CONTENT_SUBTYPE_VIDEO_MSVIDEO   = 603,
    CONTENT_SUBTYPE_VIDEO_AVI       = 604,
    CONTENT_SUBTYPE_VIDEO_WMV       = 605,

    // Software subtypes, type = CONTENT_TYPE_SOFTWARE
    CONTENT_SUBTYPE_SOFTWARE_APPLICATION = 701,

    CONTENT_SUBTYPE_MESSAGE_RFC822  = 801,
    CONTENT_SUBTYPE_MESSAGE_PARTIAL = 802,
    CONTENT_SUBTYPE_MESSAGE_EXTERNAL = 803,

    CONTENT_SUBTYPE_MULTIPART_MIXED = 901,
    CONTENT_SUBTYPE_MULTIPART_PARALLEL = 902,
    CONTENT_SUBTYPE_MULTIPART_DIGEST = 903,
    CONTENT_SUBTYPE_MULTIPART_ALTERNATIVE = 904,
    CONTENT_SUBTYPE_MULTIPART_APPLEDOUBLE = 905,

    CHARSET_CONTENT_TYPE_PARAM      = 0,
}; // MimeTypes








/////////////////////////////////////////////////////////////////////////////
class CParsedUrl : public CRefCountInterface,
                     public CRefCountImpl,
                     public CParsingCallback {
public:
    enum Constants {
        MAX_URL_LENGTH          = 1100,

        DEFAULT_PORT_HTTP       = 80,

        URL_SCHEME_UNKNOWN      = 0,
        URL_SCHEME_HTTP         = 1,
        URL_SCHEME_FTP          = 2,
        URL_SCHEME_FILE         = 3,
        URL_SCHEME_IP_ADDRESS   = 4,
        URL_SCHEME_MEMORY       = 5,
        URL_SCHEME_EMPTY        = 6,
        URL_SCHEME_HTTPS        = 7,
        URL_SCHEME_URN          = 8,

        // These are flags used for URL information.
        ABSOLUTE_URL            = 0x0001,
        PORT_IS_SPECIFIED       = 0x0002,
        URL_IS_PREENCODED       = 0x0004,
        NETWORK_NAME            = 0x0008,

        BUILTIN_BUFFER_SIZE     = 64,

        // These are the parts of a URL that we can specify when
        // printing or comparing URLs.
        ENTIRE_URL              = 0,
        URL_HOST                = 1,
        URL_PATH                = 2,
        PATH_AND_SUFFIX         = 3,
    };

    static CParsedUrl *AllocateUrl(const char *pUrl);
    static CParsedUrl *AllocateUrl(const char *pUrl, int32 urlLength, const char *pBaseUrlStr);
    static CParsedUrl *AllocateFileUrl(const char *pPath);
    static CParsedUrl *AllocateMemoryUrl(const char *pPtr, int32 length, int32 numValidBytes);

#if INCLUDE_REGRESSION_TESTS
    static void TestURL();
#endif

    CParsedUrl();
    virtual ~CParsedUrl();
    NEWEX_IMPL()

    ErrVal Initialize(
               const char *pUrl,
               int32 urlLength,
               CParsedUrl *baseURL);
    ErrVal InitializeEx(
               const char *pUrl,
               int32 urlLength,
               CParsedUrl *pBaseUrl,
               const char *pBaseUrlStr,
               bool fPreEncoded);

    ErrVal PrintToString(
                int32 urlSection,
                char *destPtr,
                int32 bufferLength,
                char **destResult);

    // Information procedures
    ErrVal GetImpliedContentType(int16 *pType, int16 *pSubType);
    bool Equal(int32 urlSection, CParsedUrl *pUrl);
    char *GetPrintableURL();

    // CParsingCallback
    virtual ErrVal OnParseToken(
                        void *pCallbackContext,
                        int32 tokenId,
                        int32 intValue,
                        const char *pStr,
                        int32 length);

    static void UrlDecodeString(
                    char *pSrcPtr,
                    int16 *pLength,
                    bool *pfShrunkString);

    // CRefCountInterface
    PASS_REFCOUNT_TO_REFCOUNTIMPL()

    char        *m_pHostName;
    char        *m_pUserName;
    char        *m_pPath;
    char        *m_pFragment;

    int16       m_Flags;
    int16       m_Scheme;
    uint16      m_Port;

    int16       m_AbsoluteUrlSize;
    int16       m_HostNameSize;
    int16       m_PathSize;
    int16       m_UserNameSize;
    int16       m_FragmentSize;

    // Network address
    struct sockaddr_in  *m_pSockAddr;

    // Query fields. These lists are all NULL if m_NumQueryFields == 0.
    int16       m_NumQueryFields;
    char        **m_QueryNameList;
    char        **m_QueryValueList;
    int16       *m_QueryNameSizeList;
    int16       *m_QueryValueSizeList;

private:
    // The parsed URL may not be continuous, since it may shrink some fields
    // in place when they are decoded. As a result, clients outside this class
    // should not expect that there is a single string for the whole URL without
    // calling the print() function.
    char        m_BuiltInBuffer[BUILTIN_BUFFER_SIZE + 1];
    char        *m_pParsingBuffer;
    char        *m_pExtraParams;
    char        *m_pGeneratedAbsoluteURL;

    ErrVal CreatePrintedURL();
    ErrVal AllocateQueryFieldLists();

    char *WriteString(
                    char *pDestPtr,
                    int32 maxDestLength,
                    char *pSrcPtr,
                    int32 srcLength);
}; // CParsedUrl



#endif // _PARSED_URL_H_


