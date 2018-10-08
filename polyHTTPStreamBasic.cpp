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
// HTTP Stream Module
//
/////////////////////////////////////////////////////////////////////////////

#include "osIndependantLayer.h"
#include "config.h"
#include "log.h"
#include "debugging.h"
#include "memAlloc.h"
#include "refCount.h"
#include "threads.h"
#include "stringLib.h"
#include "stringParse.h"
#include "queue.h"
#include "jobQueue.h"
#include "rbTree.h"
#include "nameTable.h"
#include "url.h"
#include "blockIO.h"
#include "asyncIOStream.h"
#include "polyHTTPStream.h"


FILE_DEBUGGING_GLOBALS(LOG_LEVEL_DEFAULT, 0);


/////////////////////////////////////////////
enum ReadAction {
    PARSE_BUFFER                = 0,
    GET_MORE_DATA               = 1,
    FINISHED_DOCUMENT           = 2,
    REDIRECT_REQUEST            = 3,
};


enum {
    /////////////////////////////////////////////
    // These are the op-codes for an HTTP request message.
    HTTP_GET_MSG                    = 0,
    HTTP_HEAD_MSG                   = 1,
    HTTP_POST_MSG                   = 2,
    HTTP_PUT_MSG                    = 3,
    HTTP_DELETE_MSG                 = 4,
    HTTP_LINK_MSG                   = 5,
    HTTP_OPTIONS_MSG                = 6,
    HTTP_TRACE_MSG                  = 7,
    HTTP_RESPONSE_MSG               = 8,

    /////////////////////////////////////////////
    // Status Codes
    FIRST_INFORMATIONAL_STATUS_CODE = 100, // These are for HTTP 1.1 ONLY.
    LAST_INFORMATIONAL_STATUS_CODE  = 199,
    FIRST_SUCCESS_STATUS_CODE       = 200,
    LAST_SUCCESS_STATUS_CODE        = 299,
    FIRST_REDIRECTION_STATUS_CODE   = 300,
    LAST_REDIRECTION_STATUS_CODE    = 399,
    FIRST_CLIENT_ERROR_STATUS_CODE  = 400,
    LAST_CLIENT_ERROR_STATUS_CODE   = 499,
    FIRST_SERVER_ERROR_STATUS_CODE  = 500,
    LAST_SERVER_ERROR_STATUS_CODE   = 599,

    CONTINUE_STATUS                 = 100,
    SWITCH_PROTOCOL_STATUS          = 101,
    OK_STATUS                       = 200,

    MOVED_PERMANENTLY_STATUS        = 301,
    MOVED_TEMPORARILY_STATUS        = 302,
    SEE_OTHER_STATUS                = 303,
    NOT_MODIFIED_STATUS             = 304,
    USE_PROXY_STATUS                = 305,
    SWITCH_PROXY_STATUS             = 306,

    /////////////////////////////////////////////
    // Misc
    NO_VALUE                        = -1,

    /////////////////////////////////////////////
    // The flags for ReceiveHeaderData
    EXPECT_HEADER_TO_BE_RESPONSE    = 0x0001,
    EXPECT_HEADER_TO_BE_REQUEST     = 0x0002,

    /////////////////////////////////////////////
    MAX_HTTP_DOC_SIZE           = 5000000,
    MAX_CHUNK_HEADER_SIZE       = 128, // 1024,
    MAX_REASONABLE_REDIRECTS    = 5,

    /////////////////////////////////////////////
    // These are the flags.
    READ_LAST_CHUNK_HEADER      = 0x0001,
    DISCONNECT_ON_CLOSE         = 0x0002,
    WRITING_RESPONSE            = 0x0004,
    SENT_RESPONSE_HEADER        = 0x0008,
    READING_RESPONSE            = 0x0010,
    CONNECTED_TO_PEER           = 0x0020,
};


/////////////////////////////////////////////////////
// The types of token that are recognized by the parser as it
// reads a header
#define OP_TOKEN                        100
#define CONTENT_TYPE_TOKEN              101
#define CONTENT_SUBTYPE_TOKEN           102
#define PARAM_NAME_TOKEN                103
#define PARAM_VALUE_TOKEN               104
#define ENCODING_TYPE_TOKEN             105
#define TRANSFER_ENCODING_TYPE_TOKEN    106
#define CONNECTION_COMMAND_TOKEN        107
#define EXPECT_COMMAND_TOKEN            108
#define KEEP_ALIVE_ARG_TOKEN            109
#define RANGE_UNITS_TOKEN               110
#define RFC_850_LONG_DAY_NAME_TOKEN     111
#define DAY_TOKEN                       113
#define SHORT_MONTH_TOKEN               114
#define TWO_DIGIT_YEAR_TOKEN            115
#define FOUR_DIGIT_YEAR_TOKEN           116
#define HOUR_TOKEN                      117
#define MINUTES_TOKEN                   118
#define SECONDS_TOKEN                   119
#define TIMEZONE_TOKEN                  120
#define RFC822_SHORT_DAY_NAME_TOKEN     121
#define ANSI_SHORT_DAY_NAME_TOKEN       122




#define MAX_KNOWN_REQUEST_OP_NAME_LENGTH  8

static char g_HTTPClientSoftwareName[256];
static struct sockaddr_in g_HTTPProxyAddress;
static bool g_fUseHTTPProxy = false;
static int32 g_HTTPProxyPort = 0;

static char g_HttpVersionPrefix[] = "HTTP";
static int32 g_HttpVersionPrefixLen = 4;

static char g_HttpUrlScheme[] = "http://";
static int32 g_HttpUrlSchemeSize = 7;

// Days of the week are 0-based.
static const char *g_ShortDayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *g_ShortMonthNames[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// These are some common names that are added to the name dictionary
// of each header. This lets code use these pre-formatted name IDs to
// lookup a name in any header. As a small side-benefit, it also saves
// us the time of "discovering" these standard names with each new header.
static CNameTable *m_pGlobalNameList = NULL;
static const char *g_BuiltInNames[] = {
    "Date",
    "Content-Length",
    "Content-Type",
    "Location",
    NULL
};


// This is used in several translation tables below.
class CStringIDMapping {
public:
    const char      *m_pText;
    int32           m_TextLength;
    int32           m_Id;
}; // CStringIDMapping

static const char *GetTextForId(int32 id, CStringIDMapping *mapping, int32 *pLength);



/////////////////////////////////////////////////////
// The grammar for parsing the various date formats.
DEFINE_GRAMMAR(g_DateGrammar)
    // RFC 850 Date. Example: "Sunday, 06-Nov-94 08:49:37 GMT"
    DEFINE_RULE("", "<rfc850LongDayName>,*(whitespace)<TwoDigitDay>-<shortMonthName>-<TwoDigitYear>*(whitespace)<Hour>:<Minutes>:<Seconds>*(whitespace)<TimeZone>")

    // RFC 822 (RFC 1123). Example: "Sun, 06 Nov 1994 08:49:37 GMT"
    DEFINE_RULE("", "<rfc822ShortDayName>*(whitespace)<TwoDigitDay>*(whitespace)<shortMonthName>*(whitespace)<FourDigitYear>*(whitespace)<Hour>:<Minutes>:<Seconds>*(whitespace)<TimeZone>")

    // ANSI C Date. Example: "Sun Nov 6 08:49:37 1994"
    DEFINE_RULE("", "<ansiShortDayName>*(whitespace)<shortMonthName>*(whitespace)<VariableDigitDay>*(whitespace)<Hour>:<Minutes>:<Seconds>*(whitespace)<FourDigitYear>")

    DEFINE_TOKEN("<rfc850LongDayName>", "Monday", RFC_850_LONG_DAY_NAME_TOKEN, 0)
    DEFINE_TOKEN("<rfc850LongDayName>", "Tuesday", RFC_850_LONG_DAY_NAME_TOKEN, 1)
    DEFINE_TOKEN("<rfc850LongDayName>", "Wednesday", RFC_850_LONG_DAY_NAME_TOKEN, 2)
    DEFINE_TOKEN("<rfc850LongDayName>", "Thursday", RFC_850_LONG_DAY_NAME_TOKEN, 3)
    DEFINE_TOKEN("<rfc850LongDayName>", "Friday", RFC_850_LONG_DAY_NAME_TOKEN, 4)
    DEFINE_TOKEN("<rfc850LongDayName>", "Saturday", RFC_850_LONG_DAY_NAME_TOKEN, 5)
    DEFINE_TOKEN("<rfc850LongDayName>", "Sunday", RFC_850_LONG_DAY_NAME_TOKEN, 6)

    DEFINE_INTEGER_TOKEN("<TwoDigitDay>", "(DIGIT)(DIGIT)", DAY_TOKEN)
    DEFINE_INTEGER_TOKEN("<VariableDigitDay>", "*(DIGIT)", DAY_TOKEN)

    DEFINE_INTEGER_TOKEN("<TwoDigitYear>", "(DIGIT)(DIGIT)", TWO_DIGIT_YEAR_TOKEN)
    DEFINE_INTEGER_TOKEN("<FourDigitYear>", "(DIGIT)(DIGIT)(DIGIT)(DIGIT)", FOUR_DIGIT_YEAR_TOKEN)

    DEFINE_INTEGER_TOKEN("<Hour>", "(DIGIT)(DIGIT)", HOUR_TOKEN)
    DEFINE_INTEGER_TOKEN("<Minutes>", "(DIGIT)(DIGIT)", MINUTES_TOKEN)
    DEFINE_INTEGER_TOKEN("<Seconds>", "(DIGIT)(DIGIT)", SECONDS_TOKEN)

    DEFINE_TOKEN("<shortMonthName>", "Jan", SHORT_MONTH_TOKEN, 0)
    DEFINE_TOKEN("<shortMonthName>", "Feb", SHORT_MONTH_TOKEN, 1)
    DEFINE_TOKEN("<shortMonthName>", "Mar", SHORT_MONTH_TOKEN, 2)
    DEFINE_TOKEN("<shortMonthName>", "Apr", SHORT_MONTH_TOKEN, 3)
    DEFINE_TOKEN("<shortMonthName>", "May", SHORT_MONTH_TOKEN, 4)
    DEFINE_TOKEN("<shortMonthName>", "Jun", SHORT_MONTH_TOKEN, 5)
    DEFINE_TOKEN("<shortMonthName>", "Jul", SHORT_MONTH_TOKEN, 6)
    DEFINE_TOKEN("<shortMonthName>", "Aug", SHORT_MONTH_TOKEN, 7)
    DEFINE_TOKEN("<shortMonthName>", "Sep", SHORT_MONTH_TOKEN, 8)
    DEFINE_TOKEN("<shortMonthName>", "Oct", SHORT_MONTH_TOKEN, 9)
    DEFINE_TOKEN("<shortMonthName>", "Nov", SHORT_MONTH_TOKEN, 10)
    DEFINE_TOKEN("<shortMonthName>", "Dec", SHORT_MONTH_TOKEN, 11)

    DEFINE_TOKEN("<TimeZone>", "UT", TIMEZONE_TOKEN, 0) // Universal time
    DEFINE_TOKEN("<TimeZone>", "GMT", TIMEZONE_TOKEN, 1)
    DEFINE_TOKEN("<TimeZone>", "EST", TIMEZONE_TOKEN, 2) // Eastern
    DEFINE_TOKEN("<TimeZone>", "EDT", TIMEZONE_TOKEN, 3)
    DEFINE_TOKEN("<TimeZone>", "CST", TIMEZONE_TOKEN, 4) // Central
    DEFINE_TOKEN("<TimeZone>", "CDT", TIMEZONE_TOKEN, 5)
    DEFINE_TOKEN("<TimeZone>", "MST", TIMEZONE_TOKEN, 6) // Mountain
    DEFINE_TOKEN("<TimeZone>", "MDT", TIMEZONE_TOKEN, 7)
    DEFINE_TOKEN("<TimeZone>", "PST", TIMEZONE_TOKEN, 8) // Pacific
    DEFINE_TOKEN("<TimeZone>", "PDT", TIMEZONE_TOKEN, 9)

    DEFINE_TOKEN("<rfc822ShortDayName>", "Sun,", RFC822_SHORT_DAY_NAME_TOKEN, 0)
    DEFINE_TOKEN("<rfc822ShortDayName>", "Mon,", RFC822_SHORT_DAY_NAME_TOKEN, 1)
    DEFINE_TOKEN("<rfc822ShortDayName>", "Tue,", RFC822_SHORT_DAY_NAME_TOKEN, 2)
    DEFINE_TOKEN("<rfc822ShortDayName>", "Wed,", RFC822_SHORT_DAY_NAME_TOKEN, 3)
    DEFINE_TOKEN("<rfc822ShortDayName>", "Thu,", RFC822_SHORT_DAY_NAME_TOKEN, 4)
    DEFINE_TOKEN("<rfc822ShortDayName>", "Fri,", RFC822_SHORT_DAY_NAME_TOKEN, 5)
    DEFINE_TOKEN("<rfc822ShortDayName>", "Sat,", RFC822_SHORT_DAY_NAME_TOKEN, 6)

    DEFINE_TOKEN("<ansiShortDayName>", "Sun ", ANSI_SHORT_DAY_NAME_TOKEN, 0)
    DEFINE_TOKEN("<ansiShortDayName>", "Mon ", ANSI_SHORT_DAY_NAME_TOKEN, 1)
    DEFINE_TOKEN("<ansiShortDayName>", "Tue ", ANSI_SHORT_DAY_NAME_TOKEN, 2)
    DEFINE_TOKEN("<ansiShortDayName>", "Wed ", ANSI_SHORT_DAY_NAME_TOKEN, 3)
    DEFINE_TOKEN("<ansiShortDayName>", "Thu ", ANSI_SHORT_DAY_NAME_TOKEN, 4)
    DEFINE_TOKEN("<ansiShortDayName>", "Fri ", ANSI_SHORT_DAY_NAME_TOKEN, 5)
    DEFINE_TOKEN("<ansiShortDayName>", "Sat ", ANSI_SHORT_DAY_NAME_TOKEN, 6)
STOP_GRAMMAR(g_DateGrammar);




/////////////////////////////////////////////////////
// These are the Request lines for request messages.
//   GET. Retrieve the data named by the URL.
//   HEAD. Get response header for the data named by the URL.
//   POST. Send data to the server for action, typically uploading a url.
//   PUT. Send data to the server for *storage*.
//   DELETE. Delete the resource named by the url.
//   LINK. Create links between the specified URL's.
DEFINE_GRAMMAR(g_RequestOpGrammar)
    DEFINE_TOKEN("", "GET", OP_TOKEN, HTTP_GET_MSG)
    DEFINE_TOKEN("", "HEAD", OP_TOKEN, HTTP_HEAD_MSG)
    DEFINE_TOKEN("", "POST", OP_TOKEN, HTTP_POST_MSG)
    DEFINE_TOKEN("", "PUT", OP_TOKEN, HTTP_PUT_MSG)
    DEFINE_TOKEN("", "DELETE", OP_TOKEN, HTTP_DELETE_MSG)
    DEFINE_TOKEN("", "LINK", OP_TOKEN, HTTP_LINK_MSG)
    DEFINE_TOKEN("", "OPTIONS", OP_TOKEN, HTTP_OPTIONS_MSG)
    DEFINE_TOKEN("", "TRACE", OP_TOKEN, HTTP_TRACE_MSG)
STOP_GRAMMAR(g_RequestOpGrammar);

CStringIDMapping g_RequestOpList[] = {
    { "GET", -1, HTTP_GET_MSG },
    { "HEAD", -1, HTTP_HEAD_MSG },
    { "POST", -1, HTTP_POST_MSG },
    { "PUT", -1, HTTP_PUT_MSG },
    { "DELETE", -1, HTTP_DELETE_MSG },
    { "LINK", -1, HTTP_LINK_MSG },
    { "OPTIONS", -1, HTTP_OPTIONS_MSG },
    { "TRACE", -1, HTTP_TRACE_MSG },
    { "NULL", -1, 0 }
};




/////////////////////////////////////////////////////
// These are the recognized content types. We allow headers to
// have any content type, but we only expect to know what to
// do with these.
DEFINE_GRAMMAR(g_HTTPContentTypeGrammar)
    DEFINE_RULE("", "<type>/<subtype>[;<paramList>]")

    DEFINE_TOKEN("<type>", "*", CONTENT_TYPE_TOKEN, CONTENT_TYPE_ANY)
    DEFINE_TOKEN("<type>", "application", CONTENT_TYPE_TOKEN, CONTENT_TYPE_APPLICATION)
    DEFINE_TOKEN("<type>", "audio", CONTENT_TYPE_TOKEN, CONTENT_TYPE_AUDIO)
    DEFINE_TOKEN("<type>", "image", CONTENT_TYPE_TOKEN, CONTENT_TYPE_IMAGE)
    DEFINE_TOKEN("<type>", "message", CONTENT_TYPE_TOKEN, CONTENT_TYPE_MESSAGE)
    DEFINE_TOKEN("<type>", "multipart", CONTENT_TYPE_TOKEN, CONTENT_TYPE_MULTIPART)
    DEFINE_TOKEN("<type>", "text", CONTENT_TYPE_TOKEN, CONTENT_TYPE_TEXT)
    DEFINE_TOKEN("<type>", "video", CONTENT_TYPE_TOKEN, CONTENT_TYPE_VIDEO)
    DEFINE_TOKEN("<type>", "software", CONTENT_TYPE_TOKEN, CONTENT_TYPE_SOFTWARE)

    DEFINE_TOKEN("<subtype>", "*", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_ANY)

    DEFINE_TOKEN("<subtype>", "html", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_TEXT_HTML)
    DEFINE_TOKEN("<subtype>", "plain", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_TEXT_PLAIN)
    DEFINE_TOKEN("<subtype>", "enriched", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_TEXT_ENRICHED)
    DEFINE_TOKEN("<subtype>", "xml", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_TEXT_XML)
    DEFINE_TOKEN("<subtype>", "pdf", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_TEXT_PDF)

    DEFINE_TOKEN("<subtype>", "soap+xml", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_APPLICATION_SOAPXML)
    DEFINE_TOKEN("<subtype>", "dime", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_APPLICATION_DIME)
    DEFINE_TOKEN("<subtype>", "x-www-form-urlencoded", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_APPLICATION_UUENCODED_FORM)
    DEFINE_TOKEN("<subtype>", "msword", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_APPLICATION_MSWORD)
    DEFINE_TOKEN("<subtype>", "postscript", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_APPLICATION_POSTSCRIPT)
    DEFINE_TOKEN("<subtype>", "rtf", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_APPLICATION_RTF)
    DEFINE_TOKEN("<subtype>", "zip", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_APPLICATION_ZIP)
    DEFINE_TOKEN("<subtype>", "octet-stream", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_APPLICATION_OCTET_STREAM)
    DEFINE_TOKEN("<subtype>", "wordperfect5.1", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_APPLICATION_WORDPERFECT)
    DEFINE_TOKEN("<subtype>", "vnd.ms-excel", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_APPLICATION_EXCEL)
    DEFINE_TOKEN("<subtype>", "vnd.ms-powerpoint", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_APPLICATION_POWERPOINT)
    DEFINE_TOKEN("<subtype>", "x-comet", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_APPLICATION_XCOMET)

    DEFINE_TOKEN("<subtype>", "gif", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_IMAGE_GIF)
    DEFINE_TOKEN("<subtype>", "jpeg", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_IMAGE_JPEG)
    DEFINE_TOKEN("<subtype>", "x-xbitmap", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_IMAGE_XBITMAP)
    DEFINE_TOKEN("<subtype>", "pjpeg", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_IMAGE_PJPEG)

    DEFINE_TOKEN("<subtype>", "RFC822", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_MESSAGE_RFC822)
    DEFINE_TOKEN("<subtype>", "partial", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_MESSAGE_PARTIAL)
    DEFINE_TOKEN("<subtype>", "external", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_MESSAGE_EXTERNAL)

    DEFINE_TOKEN("<subtype>", "mixed", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_MULTIPART_MIXED)
    DEFINE_TOKEN("<subtype>", "parallel", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_MULTIPART_PARALLEL)
    DEFINE_TOKEN("<subtype>", "digest", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_MULTIPART_DIGEST)
    DEFINE_TOKEN("<subtype>", "alternative", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_MULTIPART_ALTERNATIVE)
    DEFINE_TOKEN("<subtype>", "appledouble", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_MULTIPART_APPLEDOUBLE)

    DEFINE_TOKEN("<subtype>", "mpeg", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_VIDEO_MPEG)
    DEFINE_TOKEN("<subtype>", "quicktime", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_VIDEO_QUICKTIME)
    DEFINE_TOKEN("<subtype>", "x-msvideo", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_VIDEO_MSVIDEO)
    DEFINE_TOKEN("<subtype>", "avi", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_VIDEO_AVI)
    DEFINE_TOKEN("<subtype>", "wmv", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_VIDEO_WMV)
    DEFINE_TOKEN("<subtype>", "mpg", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_VIDEO_MPEG)

    DEFINE_TOKEN("<subtype>", "basic", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_AUDIO_BASIC)
    DEFINE_TOKEN("<subtype>", "wav", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_AUDIO_WAV)
    DEFINE_TOKEN("<subtype>", "wma", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_AUDIO_WMA)
    DEFINE_TOKEN("<subtype>", "rma", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_AUDIO_RMA)
    DEFINE_TOKEN("<subtype>", "mp3", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_AUDIO_MP3)
    DEFINE_TOKEN("<subtype>", "asf", CONTENT_SUBTYPE_TOKEN, CONTENT_SUBTYPE_AUDIO_ASF)

    DEFINE_RULE("<paramList>", "<paramEntry>[,<paramEntry>]")
    DEFINE_RULE("<paramEntry>", "<paramName>[=<paramValue>]")
    DEFINE_TOKEN("<paramName>", "*(URL_PATH_CHAR)", PARAM_NAME_TOKEN, 0)
    DEFINE_TOKEN("<paramValue>", "*(URL_PATH_CHAR)", PARAM_VALUE_TOKEN, 0)
STOP_GRAMMAR(g_HTTPContentTypeGrammar);



CStringIDMapping g_ContentTypeStrings[] = {
    { "*", -1, CONTENT_TYPE_ANY },
    { "application", -1, CONTENT_TYPE_APPLICATION },
    { "audio", -1, CONTENT_TYPE_AUDIO },
    { "image", -1, CONTENT_TYPE_IMAGE },
    { "message", -1, CONTENT_TYPE_MESSAGE },
    { "multipart", -1, CONTENT_TYPE_MULTIPART },
    { "text", -1, CONTENT_TYPE_TEXT },
    { "video", -1, CONTENT_TYPE_VIDEO },
    { "software", -1, CONTENT_TYPE_SOFTWARE },
    { "*", -1, CONTENT_SUBTYPE_ANY },
    { "soap+xml", -1, CONTENT_SUBTYPE_APPLICATION_SOAPXML },
    { "dime", -1, CONTENT_SUBTYPE_APPLICATION_DIME },
    { "x-www-form-urlencoded", -1, CONTENT_SUBTYPE_APPLICATION_UUENCODED_FORM },
    { "msword", -1, CONTENT_SUBTYPE_APPLICATION_MSWORD },
    { "postscript", -1, CONTENT_SUBTYPE_APPLICATION_POSTSCRIPT },
    { "rtf", -1, CONTENT_SUBTYPE_APPLICATION_RTF },
    { "zip", -1, CONTENT_SUBTYPE_APPLICATION_ZIP },
    { "octet-stream", -1, CONTENT_SUBTYPE_APPLICATION_OCTET_STREAM },
    { "wordperfect5.1", -1, CONTENT_SUBTYPE_APPLICATION_WORDPERFECT },
    { "vnd.ms-excel", -1, CONTENT_SUBTYPE_APPLICATION_EXCEL },
    { "vnd.ms-powerpoint", -1, CONTENT_SUBTYPE_APPLICATION_POWERPOINT },
    { "x-comet", -1, CONTENT_SUBTYPE_APPLICATION_XCOMET },
    { "gif", -1, CONTENT_SUBTYPE_IMAGE_GIF },
    { "jpeg", -1, CONTENT_SUBTYPE_IMAGE_JPEG },
    { "x-xbitmap", -1, CONTENT_SUBTYPE_IMAGE_XBITMAP },
    { "pjpeg", -1, CONTENT_SUBTYPE_IMAGE_PJPEG },
    { "RFC822", -1, CONTENT_SUBTYPE_MESSAGE_RFC822 },
    { "partial", -1, CONTENT_SUBTYPE_MESSAGE_PARTIAL },
    { "external", -1, CONTENT_SUBTYPE_MESSAGE_EXTERNAL },
    { "mixed", -1, CONTENT_SUBTYPE_MULTIPART_MIXED },
    { "parallel", -1, CONTENT_SUBTYPE_MULTIPART_PARALLEL },
    { "digest", -1, CONTENT_SUBTYPE_MULTIPART_DIGEST },
    { "alternative", -1, CONTENT_SUBTYPE_MULTIPART_ALTERNATIVE },
    { "appledouble", -1, CONTENT_SUBTYPE_MULTIPART_APPLEDOUBLE },
    { "html", -1, CONTENT_SUBTYPE_TEXT_HTML },
    { "plain", -1, CONTENT_SUBTYPE_TEXT_PLAIN },
    { "enriched", -1, CONTENT_SUBTYPE_TEXT_ENRICHED },
    { "xml", -1, CONTENT_SUBTYPE_TEXT_XML },
    { "pdf", -1, CONTENT_SUBTYPE_TEXT_PDF },
    { "mpeg", -1, CONTENT_SUBTYPE_VIDEO_MPEG },
    { "quicktime", -1, CONTENT_SUBTYPE_VIDEO_QUICKTIME },
    { "x-msvideo", -1, CONTENT_SUBTYPE_VIDEO_MSVIDEO },
    { "avi", -1, CONTENT_SUBTYPE_VIDEO_AVI },
    { "wmv", -1, CONTENT_SUBTYPE_VIDEO_WMV },
    { "mpg", -1, CONTENT_SUBTYPE_VIDEO_MPEG },
    { "basic", -1, CONTENT_SUBTYPE_AUDIO_BASIC },
    { "wav", -1, CONTENT_SUBTYPE_AUDIO_WAV },
    { "wma", -1, CONTENT_SUBTYPE_AUDIO_WMA },
    { "rma", -1, CONTENT_SUBTYPE_AUDIO_RMA },
    { "mp3", -1, CONTENT_SUBTYPE_AUDIO_MP3 },
    { "asf", -1, CONTENT_SUBTYPE_AUDIO_ASF },
    { NULL, -1, 0 }
};



/////////////////////////////////////////////////////////////////////////////
// This is one line in an HTTP header.
class CHttpHeaderLine {
public:
    CHttpHeaderLine() { m_pValue = NULL; }
    virtual ~CHttpHeaderLine() { if (NULL != m_pValue) { memFree(m_pValue); } }
    NEWEX_IMPL()

    CDictionaryEntry    *m_pName;

    int64               m_StartPosition;
    int32               m_Length;
    char                *m_pValue;

    CHttpHeaderLine     *m_pNext;
}; // CHttpHeaderLine



/////////////////////////////////////////////////////////////////////////////
class CHttpContentType {
public:
    NEWEX_IMPL()

    int16               m_Type;
    int16               m_SubType;

    // These are the params.
    int16               m_CharSet;
}; // CHttpContentType




/////////////////////////////////////////////////////////////////////////////
// All asynch events are translated into calls on this object.
class CPolyHttpStreamBasic : public CPolyHttpStream,
                                public CRefCountImpl,
                                public CParsingCallback,
                                public CAsyncIOEventHandler {
public:
    CPolyHttpStreamBasic();
    virtual ~CPolyHttpStreamBasic();
    NEWEX_IMPL()

    //////////////////////////////
    // CRefCountInterface
    PASS_REFCOUNT_TO_REFCOUNTIMPL()

    //////////////////////////////
    // CPolyHttpStream: Write Outgoing requests
    virtual void ReadHTTPDocument(
                        CParsedUrl *pUrl,
                        CPolyHttpCallback *pCallback,
                        void *pCallbackContext);
    virtual void SendHTTPPost(
                        CParsedUrl *pUrl,
                        CAsyncIOStream *pDocument,
                        int32 contentType,
                        int32 contentSubType,
                        int32 contentLength,
                        CPolyHttpCallback *pCallback,
                        void *pCallbackContext);

    //////////////////////////////
    // CPolyHttpStream: Accessors
    virtual int32 GetStatusCode() { return(m_StatusCode); }
    virtual ErrVal GetContentType(int16 *pType, int16 *pSubType);
    virtual ErrVal GetIOStream(
                        CAsyncIOStream **ppSrcStream,
                        int64 *pStartPosition,
                        int32 *pLength);
    virtual CParsedUrl *GetUrl();
    virtual void CloseStreamToURL();

    //////////////////////////////
    // CAsyncIOEventHandler
    virtual void OnReadyToRead(
                    ErrVal err,
                    int64 totalBytesAvailable,
                    CAsyncIOStream *pAsyncIOStream,
                    void *pCallbackContext);
    virtual void OnFlush(
                    ErrVal err,
                    CAsyncIOStream *pAsyncIOStream,
                    void *pCallbackContext);
    virtual void OnOpenAsyncIOStream(
                    ErrVal err,
                    CAsyncIOStream *pAsyncIOStream,
                    void *pCallbackContext);
    virtual void OnStreamDisconnect(
                    ErrVal err,
                    CAsyncIOStream *pAsyncIOStream,
                    void *pContext);

    //////////////////////////////
    // CParsingCallback
    virtual ErrVal OnParseToken(
                        void *pCallbackContext,
                        int32 tokenId,
                        int32 intValue,
                        const char *pStr,
                        int32 length);


    ////////////////////////////////////////
    enum CHttpAsyncState {
        // NOTE: WRITING_RESPONSE is NOT a state. To support bi-directional
        // streams, we may write a response (and wait for it to asynch flush)
        // while we are also asynchronously waiting for the next read.
        IDLE                        = 0,
        CONNECTING_TO_SERVER        = 1,
        WRITING_REQUEST             = 2,
        READING_HEADER              = 3,
        READING_BODY                = 4,
        READING_DIME_HEADER         = 5,
        READING_DIME_BODY           = 6,
        READING_CHUNK               = 7,
    }; // CHttpAsyncState

    ErrVal ReadHeader(ReadAction *pAction, CRefLock **ppHeldLock);
    ErrVal ReadBodyData(ErrVal resultErr, ReadAction *pAction);
    ErrVal ReadChunks(ErrVal resultErr, ReadAction *pAction);

    ErrVal OpenStreamToURL(CParsedUrl *url);
    void SendRequestAfterConnecting();
    ErrVal FollowRedirection();

    ErrVal InternalInitialize(
                CParsedUrl *pUrl,
                CPolyHttpCallback *pCallback,
                void *pCallbackContext);

    ErrVal PrepareHeaderForRequest(int opCode, bool fSpecial);

    void FinishAsynchOp(ErrVal resultErr, bool fWriteOp);

    void InitHeader(bool fParsingNewHeader);

    // Writes do not flush the stream, since a body may follow the header.
    // As a result, they are synchronous functions.
    ErrVal WriteRequestToStream(CParsedUrl *url, CAsyncIOStream *pStream);
    ErrVal ParseResponseHeader();

    // Start reading new header data. It reports when we have read a
    // complete header.
    ErrVal ReceiveHeaderData(
                    int32 flags,
                    bool *pHeaderComplete,
                    bool *pfResponseHeader);
    ErrVal CheckState();

    ErrVal GetStringHeader(
                    const char *pName,
                    char *pTempBuffer,
                    int maxBufferLength,
                    char **pResultPtr,
                    int32 *ptextLength);

    ErrVal AddStringHeader(
                    const char *pName,
                    const char *pValue,
                    int32 valueLength);

    ErrVal GetIntegerHeader(const char *pName, int32 *pValue, bool *pFoundIt);
    ErrVal AddIntegerHeader(const char *pName, int64 value);

    CHttpHeaderLine *FindHeader(
                        CHttpHeaderLine *prevHeader,
                        const char *pName);
    ErrVal GetHeaderLineValue(
                        CHttpHeaderLine *pLine,
                        char *pTempBuffer,
                        int maxBufferLength,
                        char **pResultPtr,
                        int32 *pTextLength);

    bool FindEndOfHeader(
                    int32 startBufferPos,
                    char *pPtr,
                    char *pEndPtr);

    ErrVal GetContentTypeHeader(
                    const char *pName,
                    CHttpContentType *pTypeList,
                    int maxTypes,
                    int32 *pNumTypes);

    ErrVal AddContentTypeHeader(
                    const char *pName,
                    CHttpContentType *pTypeList,
                    int numTypes);

    ErrVal WriteHeaderLines(
                    CAsyncIOStream *pAsyncIOStream,
                    CParsedUrl *url);

    ErrVal ParseHeader();

    ErrVal WriteDate(
                    char *pHeader,
                    char *pEndHeader,
                    char **ppStopPtr,
                    CDateTime *pDate);

    ErrVal GotoEndOfHTTPLine();
    ErrVal WriteEndOfHTTPLine(CAsyncIOStream *pStream);

    ErrVal ParseHTTPVersion(bool *pRecognized);
    ErrVal ParseVersionNum(
                    int32 *majorVersion,
                    int32 *minorVersion,
                    bool *pRecognized);

    ErrVal WriteVersionNum(
                    CAsyncIOStream *pStream,
                    int32 major,
                    int32 minor);


    int32               m_HttpOp;
    CHttpAsyncState     m_HttpState;
    int32               m_HttpStreamFlags;
    CRefLock            *m_pLock;
    bool                m_fKeepAlive;

    CParsedUrl          *m_pUrl;

    CAsyncIOStream      *m_pAsyncIOStream;
    // This is a stream we send as the body of a POST.
    CAsyncIOStream      *m_pSendAsyncIOStream;

    int32               m_StatusCode;
    char                *m_pAcceptLanguage;

    CHttpHeaderLine     *m_pHeaderLines;
    CNameTable   *m_pNameList;

    // The callback for all events.
    CPolyHttpCallback   *m_pCallback;
    void                *m_pCallbackContext;

    // Additional state for reading a response.
    int8                m_NumRedirects;
    bool                m_fReadingResponse;
    int32               m_HeaderAssumptions;
    int32               m_ContentLengthToLoad;

    // This is the state for reading chunks.
    int32               m_CurrentChunkNum;
    int64               m_StartChunkHeaderPosition;

    int64               m_RequestURLStartPos;
    int32               m_RequestURLLength;

    int64               m_MessageStartPos;
    int64               m_StartHeaderPos;
    int64               m_StartBodyPos;

    int32               m_HttpOpInSendRequest;

    int32               m_HttpMajorVersion;
    int32               m_HttpMinorVersion;


    // This is the state for asynchronously loading a document.
    int32               m_numBytesAvailable;
    int32               m_numBytesProcessed;
    char                m_EndLineChars[4];
    int32               m_NumEndLineChars;

    // State of parsing.
    int32               m_ParseIntValue;
    int32               m_ParsedMonth;
    int32               m_ParsedHours;
    int32               m_ParsedMinutes;
    int32               m_ParsedSeconds;
    int32               m_ParsedDate;
    int32               m_ParsedYear;
    char                m_ParsedTimeZone[5];
    CHttpContentType    *m_pParsingContentType;
}; // CPolyHttpStreamBasic.






/////////////////////////////////////////////////////////////////////////////
//
// [InitializeBasicHTTPStreamGlobalState]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
InitializeBasicHTTPStreamGlobalState(CProductInfo *pVersion) {
    ErrVal err = ENoErr;
    char *pHTTPProxyName;
    char *pEndPtr;

    // Look if there is a proxy. If there is, then use it for
    // all outgoing communication.
    g_fUseHTTPProxy = NetIO_GetLocalProxySettings(&pHTTPProxyName, &g_HTTPProxyPort);
    if (g_fUseHTTPProxy) {
        err = NetIO_LookupHost(
                        pHTTPProxyName,
                        (int16) g_HTTPProxyPort,
                        &g_HTTPProxyAddress);
        if (err) {
            g_fUseHTTPProxy = false;
            err = ENoErr;
        }
    }

    err = pVersion->PrintToString(
                        g_HTTPClientSoftwareName,
                        sizeof(g_HTTPClientSoftwareName),
                        CProductInfo::PRINT_SOFTWARE_NAME
                            | CProductInfo::PRINT_RELEASE_TYPE
                            | CProductInfo::PRINT_BUILD_NUMBER,
                        &pEndPtr);
    if (err) {
        gotoErr(err);
    }

    // Create the built-in name list.
    m_pGlobalNameList = newex CNameTable;
    if (NULL == m_pGlobalNameList) {
        gotoErr(EFail);
    }

    err = m_pGlobalNameList->Initialize(CStringLib::IGNORE_CASE, 8);
    if (err) {
        gotoErr(err);
    }

    m_pGlobalNameList->AddDictionaryEntryList(g_BuiltInNames);


abort:
    returnErr(err);
} // InitializeBasicHTTPStreamGlobalState.







/////////////////////////////////////////////////////////////////////////////
//
// [AllocateSimpleStream]
//
/////////////////////////////////////////////////////////////////////////////
CPolyHttpStream *
CPolyHttpStream::AllocateSimpleStream() {
    CPolyHttpStreamBasic *pBasicStream = NULL;

    pBasicStream = newex CPolyHttpStreamBasic;
    if (NULL == pBasicStream) {
        return(NULL);
    }

    return(pBasicStream);
} // AllocateSimpleStream





/////////////////////////////////////////////////////////////////////////////
//
// [CPolyHttpStreamBasic]
//
/////////////////////////////////////////////////////////////////////////////
CPolyHttpStreamBasic::CPolyHttpStreamBasic() {
    m_HttpState = IDLE;
    m_HttpStreamFlags = 0;
    m_pLock = NULL;
    m_fKeepAlive = false;

    m_pAsyncIOStream = NULL;
    m_pUrl = NULL;

    m_pCallback = NULL;
    m_pCallbackContext = NULL;

    m_NumRedirects = 0;
    m_HeaderAssumptions = 0;
    m_fReadingResponse = false;

    m_pAcceptLanguage = NULL;

    m_StartChunkHeaderPosition = -1;
    m_CurrentChunkNum = 0;

    m_pSendAsyncIOStream = NULL;

    m_pHeaderLines = NULL;
    m_pNameList = NULL;

    InitHeader(false);
} // CPolyHttpStreamBasic





/////////////////////////////////////////////////////////////////////////////
//
// [~CPolyHttpStreamBasic]
//
/////////////////////////////////////////////////////////////////////////////
CPolyHttpStreamBasic::~CPolyHttpStreamBasic() {
    CloseStreamToURL();

    InitHeader(false);
    if (NULL != m_pNameList) {
        delete m_pNameList;
    }

    RELEASE_OBJECT(m_pLock);
    RELEASE_OBJECT(m_pCallback);
    RELEASE_OBJECT(m_pUrl);
    RELEASE_OBJECT(m_pSendAsyncIOStream);
} // ~CPolyHttpStreamBasic






/////////////////////////////////////////////////////////////////////////////
//
// [ReadHTTPDocument]
//
// CPolyHttpStream
/////////////////////////////////////////////////////////////////////////////
void
CPolyHttpStreamBasic::ReadHTTPDocument(
                            CParsedUrl *pUrl,
                            CPolyHttpCallback *pCallback,
                            void *pCallbackContext) {
    ErrVal err = ENoErr;
    SignObject("CPolyHttpStreamBasic::ReadHTTPDocument");

    err = InternalInitialize(pUrl, pCallback, pCallbackContext);
    if (err) {
        gotoErr(err);
    }

    // Fill in the header.
    PrepareHeaderForRequest(HTTP_GET_MSG, false);
    if (err) {
        gotoErr(err);
    }

    err = OpenStreamToURL(pUrl);
    if (err) {
        gotoErr(err);
    }
    // This resumes in OnOpenAsyncIOStream.

abort:
    if (err) {
        FinishAsynchOp(err, false);
    }
} // ReadHTTPDocument.







/////////////////////////////////////////////////////////////////////////////
//
// [SendHTTPPost]
//
// CPolyHttpStream
/////////////////////////////////////////////////////////////////////////////
void
CPolyHttpStreamBasic::SendHTTPPost(
                            CParsedUrl *pUrl,
                            CAsyncIOStream *pDocument,
                            int32 contentType,
                            int32 contentSubType,
                            int32 contentLength,
                            CPolyHttpCallback *pCallback,
                            void *pCallbackContext) {
    ErrVal err = ENoErr;
    CHttpContentType typeList;
    SignObject("CPolyHttpStreamBasic::SendHTTPPost");

    err = InternalInitialize(pUrl, pCallback, pCallbackContext);
    if (err) {
        gotoErr(err);
    }

    if (m_pSendAsyncIOStream != pDocument) {
        RELEASE_OBJECT(m_pSendAsyncIOStream);
        m_pSendAsyncIOStream = pDocument;
        ADDREF_OBJECT(m_pSendAsyncIOStream);
    }

    // Fill in the header.
    err = PrepareHeaderForRequest(HTTP_POST_MSG, true);
    if (err) {
        gotoErr(err);
    }

    typeList.m_Type = contentType;
    typeList.m_SubType = contentSubType;
    err = AddContentTypeHeader("Content-Type", &typeList, 1);
    if (err) {
        gotoErr(err);
    }
    if (contentLength > 0) {
        err = AddIntegerHeader("Content-Length", contentLength);
        if (err) {
            gotoErr(err);
        }
    }

    // If we are already connected, then we are using a previously
    // established connection with the Keep-alive. In that case,
    // just send the new message. Otherwise, we first have to connect
    // to the server.
    if (m_HttpStreamFlags & CONNECTED_TO_PEER) {
        SendRequestAfterConnecting();
    } else
    {
        err = OpenStreamToURL(pUrl);
        if (err) {
            gotoErr(err);
        }
        // This resumes in OnOpenAsyncIOStream.
    }

abort:
    if (err) {
        FinishAsynchOp(err, false);
    }
} // SendHTTPPost.







/////////////////////////////////////////////////////////////////////////////
//
// [PrepareHeaderForRequest]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::PrepareHeaderForRequest(int opCode, bool fSpecial) {
    ErrVal err = ENoErr;
    SignObject("CPolyHttpStreamBasic::PrepareHeaderForRequest");


    // Fill in the header.
    InitHeader(false);
    m_HttpMajorVersion = 1;
    // We need to specify 1.1 so servers that send multi-part
    // responses will talk to us.
    m_HttpMinorVersion = 1;
    m_HttpOp = opCode;

    if (NULL != g_HTTPClientSoftwareName) {
        err = AddStringHeader(
                             "User-Agent",
                             g_HTTPClientSoftwareName,
                             strlen(g_HTTPClientSoftwareName));
        if (err) {
            gotoErr(err);
        }
    }

    if (fSpecial) {
       gotoErr(err);
    }

    err = AddStringHeader("Accept", "*/*", -1);
    if (err) {
        gotoErr(err);
    }

    if (NULL != m_pAcceptLanguage) {
        err = AddStringHeader("Accept-Language", m_pAcceptLanguage, -1);
        if (err) {
            gotoErr(err);
        }
    }

abort:
    returnErr(err);
} // PrepareHeaderForRequest.








/////////////////////////////////////////////////////////////////////////////
//
// [InternalInitialize]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::InternalInitialize(
                            CParsedUrl *pUrl,
                            CPolyHttpCallback *pCallback,
                            void *pCallbackContext) {
    ErrVal err = ENoErr;
    SignObject("CPolyHttpStreamBasic::InternalInitialize");

    if (NULL == pUrl) {
        gotoErr(EFail);
    }

    if (!(m_HttpStreamFlags & CONNECTED_TO_PEER)) {
        RELEASE_OBJECT(m_pAsyncIOStream);
        RELEASE_OBJECT(m_pLock);
    }

    if ((m_pCallback != pCallback) && (NULL != pCallback)) {
        RELEASE_OBJECT(m_pCallback);
        m_pCallback = pCallback;
        ADDREF_OBJECT(m_pCallback);
        m_pCallbackContext = pCallbackContext;
    }

    RELEASE_OBJECT(m_pUrl);
    m_pUrl = pUrl;
    ADDREF_OBJECT(m_pUrl);

abort:
    returnErr(err);
} // InternalInitialize








/////////////////////////////////////////////////////////////////////////////
//
// [OpenStreamToURL]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::OpenStreamToURL(CParsedUrl *pUrl) {
    ErrVal err = ENoErr;
    AutoLock(m_pLock);
    SignObject("CPolyHttpStreamBasic::OpenStreamToURL");

    // Check the arguments.
    if (NULL == pUrl) {
        gotoErr(EFail);
    }

    // This happens when we try to open an HTTPS or similar
    // scheme.
    if (CParsedUrl::URL_SCHEME_HTTPS == pUrl->m_Scheme) {
        gotoErr(EHTTPSRequired);
    }
    if ((CParsedUrl::URL_SCHEME_HTTP != pUrl->m_Scheme)
         && (CParsedUrl::URL_SCHEME_URN != m_pUrl->m_Scheme)) {
        char debugBuffer[1024];
        char *pDestPtr;
        pUrl->PrintToString(
                CParsedUrl::ENTIRE_URL,
                debugBuffer,
                sizeof(debugBuffer),
                &pDestPtr);
        DEBUG_LOG("Malformed URL. scheme = %d, string = %s", pUrl->m_Scheme, debugBuffer);
        gotoErr(ENoResponse);
    }

    if ((NULL == pUrl->m_pHostName)
        || (pUrl->m_HostNameSize <= 0)) {
        gotoErr(EFail);
    }

    // If we are going through a proxy, then find the
    // address of the end server.
    if (g_fUseHTTPProxy) {
        memFree(pUrl->m_pSockAddr);
        pUrl->m_pSockAddr = (struct sockaddr_in *) memAlloc(sizeof(struct sockaddr_in));
        if (NULL == pUrl->m_pSockAddr) {
            gotoErr(EFail);
        }

        *(pUrl->m_pSockAddr) = g_HTTPProxyAddress;
    }

    m_HttpState = CONNECTING_TO_SERVER;

    err = CAsyncIOStream::OpenAsyncIOStream(pUrl, 0, this, NULL);
    if (err) {
        gotoErr(err);
    }
    // This continues in OnOpenAsyncIOStream.

abort:
    returnErr(err);
} // OpenStreamToURL.





/////////////////////////////////////////////////////////////////////////////
//
// [CloseStreamToURL]
//
/////////////////////////////////////////////////////////////////////////////
void
CPolyHttpStreamBasic::CloseStreamToURL() {
    AutoLock(m_pLock);
    SignObject("CPolyHttpStreamBasic::CloseStreamToURL");

    if (m_pAsyncIOStream) {
        m_pAsyncIOStream->Close();
        m_HttpStreamFlags &= ~CONNECTED_TO_PEER;
        RELEASE_OBJECT(m_pAsyncIOStream);
    }
} // CloseStreamToURL.






/////////////////////////////////////////////////////////////////////////////
//
// [OnOpenAsyncIOStream]
//
// This is called when we open a stream connection to the host.
// We haven't written or read any data yet.
/////////////////////////////////////////////////////////////////////////////
void
CPolyHttpStreamBasic::OnOpenAsyncIOStream(
                            ErrVal err,
                            CAsyncIOStream *pAsyncIOStream,
                            void *pCallbackContext) {
    CRefLock *pHeldLock = NULL;
    CPolyHttpCallback *pReconnectCallback = NULL;
    void *pReconnectCallbackContext = NULL;
    SignObject("CPolyHttpStreamBasic::OnOpenAsyncIOStream");

    // Unused
    pCallbackContext = pCallbackContext;


    if (CONNECTING_TO_SERVER != m_HttpState) {
        DEBUG_WARNING("Unexpected OnOpenAsyncIOStream.");
    }
    m_HttpState = IDLE;

    if (m_pAsyncIOStream != pAsyncIOStream) {
        RELEASE_OBJECT(m_pAsyncIOStream);
        m_pAsyncIOStream = pAsyncIOStream;
        ADDREF_OBJECT(m_pAsyncIOStream);
    }

    if (pAsyncIOStream) {
        // We may be following a redirect, so discard the previous lock
        RELEASE_OBJECT(m_pLock);
        m_pLock = pAsyncIOStream->GetLock();
    }

    if (m_pLock) {
        m_pLock->Lock();
        pHeldLock = m_pLock;
    }

    if (err) {
        gotoErr(err);
    }
    m_HttpStreamFlags |= CONNECTED_TO_PEER;

    SendRequestAfterConnecting();

abort:
    // To avoid deadlock, release the lock before calling the callback.
    if (pHeldLock) {
        pHeldLock->Unlock();
        pHeldLock = NULL;
    }

    if (err) {
        FinishAsynchOp(err, false);
    }
    if (NULL != pReconnectCallback) {
        pReconnectCallback->OnReconnect(err, this, pReconnectCallbackContext);
    }
} // OnOpenAsyncIOStream.







/////////////////////////////////////////////////////////////////////////////
//
// [SendRequestAfterConnecting]
//
// This is called when we open a stream connection to the host.
// We haven't written or read any data yet.
/////////////////////////////////////////////////////////////////////////////
void
CPolyHttpStreamBasic::SendRequestAfterConnecting() {
    ErrVal err = ENoErr;
    CRefLock *pHeldLock = NULL;
    SignObject("CPolyHttpStreamBasic::SendRequestAfterConnecting");

    if (m_pLock) {
        m_pLock->Lock();
        pHeldLock = m_pLock;
    }

    m_HttpState = WRITING_REQUEST;

    if (m_pCallback) {
        err = m_pCallback->AdjustHTTPRequestHeader(
                                this, // pHTTPStream,
                                m_pAsyncIOStream,
                                m_pCallbackContext);
        if (err) {
            gotoErr(err);
        }
    } // if (m_pCallback)

    err = WriteRequestToStream(m_pUrl, m_pAsyncIOStream);
    if (err) {
        gotoErr(err);
    }

    // Send the body if there is one.
    if (m_pSendAsyncIOStream) {
        bool fNoCopy = true;

        // Be careful!
        // CopyStream will cause a flush, which sends the packet.
        // Only do this if we can split the data across packets (it's not UDP)
        // and if there is enough data in the body that the no-copy savings
        // are great enough to send an extra packet.
        if (m_pSendAsyncIOStream->GetDataLength() < 400) {
            fNoCopy = false;
        }

        err = m_pSendAsyncIOStream->CopyStream(
                                        m_pAsyncIOStream,
                                        m_pSendAsyncIOStream->GetDataLength(), // totalBytesToTransfer
                                        fNoCopy);
        if (err) {
            gotoErr(err);
        }
    } // if (m_pSendAsyncIOStream)


    if (m_pCallback) {
        err = m_pCallback->SendHTTPRequestBody(
                                this, // pHTTPStream,
                                m_pAsyncIOStream,
                                m_pCallbackContext);
        if (err) {
            gotoErr(err);
        }
    } // if (m_pCallback)


    m_pAsyncIOStream->Flush();
    // This is continued in OnFlush.

abort:
    // To avoid deadlock, release the lock before calling the callback.
    if (pHeldLock) {
        pHeldLock->Unlock();
        pHeldLock = NULL;
    }

    if (err) {
        FinishAsynchOp(err, false);
    }
} // SendRequestAfterConnecting.







/////////////////////////////////////////////////////////////////////////////
//
// [OnFlush]
//
/////////////////////////////////////////////////////////////////////////////
void
CPolyHttpStreamBasic::OnFlush(
                            ErrVal err,
                            CAsyncIOStream *pAsyncIOStream,
                            void *pCallbackContext) {
    int32 numBytesAvailable;
    CRefLock *pHeldLock = NULL;
    SignObject("CPolyHttpStreamBasic::OnFlush");

    // Unused
    pAsyncIOStream = pAsyncIOStream;
    pCallbackContext = pCallbackContext;

    DEBUG_LOG("CPolyHttpStreamBasic::OnFlush. err = %d", err);

    if (m_pLock) {
        m_pLock->Lock();
        pHeldLock = m_pLock;
    }

    if (m_HttpStreamFlags & WRITING_RESPONSE) {
        m_HttpStreamFlags &= ~WRITING_RESPONSE;
        gotoErr(ENoErr);
    } else if (WRITING_REQUEST == m_HttpState) {
        // Be careful, a request may have been sent while we were writing our
        // request, so they may pass on the network. The next header we read
        // may be a response to this request, or a new request from the peer.
        m_HttpState = READING_HEADER;

        numBytesAvailable = (int32) (m_pAsyncIOStream->GetDataLength());
        if (numBytesAvailable > 0) {
            OnReadyToRead(
                    err,
                    numBytesAvailable,
                    m_pAsyncIOStream,
                    NULL); // void *pCallbackContext
        } else {
           m_pAsyncIOStream->ListenForMoreBytes();
        }

        // This is continued in OnReadyToRead.
        gotoErr(ENoErr);
    } else
    {
        gotoErr(EFail);
    }

abort:
    // To avoid deadlock, release the lock before calling the callback.
    if (pHeldLock) {
        pHeldLock->Unlock();
        pHeldLock = NULL;
    }

    if (err) {
        FinishAsynchOp(err, true);
    }
} // OnFlush.






/////////////////////////////////////////////////////////////////////////////
//
// [OnReadyToRead]
//
// This is called when the AsyncIOStream reads another chunk of data.
/////////////////////////////////////////////////////////////////////////////
void
CPolyHttpStreamBasic::OnReadyToRead(
                            ErrVal resultErr,
                            int64 totalBytesAvailable,
                            CAsyncIOStream *pAsyncIOStream,
                            void *pCallbackContext) {
    ErrVal err = ENoErr;
    CRefLock *pHeldLock = NULL;
    ReadAction action = PARSE_BUFFER;
    SignObject("CPolyHttpStreamBasic::OnReadyToRead");

    // Unused
    pAsyncIOStream = pAsyncIOStream;
    pCallbackContext = pCallbackContext;

    DEBUG_LOG("CPolyHttpStreamBasic::OnReadyToRead. err = %d, totalBytesAvailable = %d",
         (int32) resultErr, (int32) totalBytesAvailable);

    if (m_pLock) {
        m_pLock->Lock();
        pHeldLock = m_pLock;
    }

    // Check if the timeout fired. If so, then abort.
    if (NULL == m_pAsyncIOStream) {
        gotoErr(EFail);
    }

    // EOF is normal for a web server that does not
    // provide a content-length in the header; it just
    // closes the connection when it has provided the
    // entire page.
    //
    // However, if this connection is being kept alive, then
    // an EOF means the peer has closed the connection.
    if ((resultErr)
        && ((EEOF != resultErr) || (m_fKeepAlive))) {
        gotoErr(resultErr);
    }

    // Do not allow a malicious server to force us to accept more
    // data than is reasonable. If there are attachments, however, then
    // all bets are off. That is used for transferring really large files
    // (like virtual machine disk files).
    if (totalBytesAvailable >= MAX_HTTP_DOC_SIZE) {
        gotoErr(EHTTPDocTooLarge);
    }


    // A single buffer may contain several things, like an http header, and a payload.
    action = PARSE_BUFFER;
    while (PARSE_BUFFER == action) {
        switch (m_HttpState) {
        /////////////////////////////////////
        case READING_HEADER:
            err = ReadHeader(&action, &pHeldLock);
            DEBUG_LOG("CPolyHttpStreamBasic::OnReadyToRead. Call ReadHeader. err = %d, action = %d",
               (int32) err, action);
            break;

        /////////////////////////////////////
        case READING_BODY:
            err = ReadBodyData(resultErr, &action);
            DEBUG_LOG("CPolyHttpStreamBasic::OnReadyToRead. Call ReadBodyData. err = %d, action = %d",
               (int32) err, action);
            break;

        /////////////////////////////////////
        case READING_CHUNK:
            err = ReadChunks(resultErr, &action);
            break;

        /////////////////////////////////////
        default:
            gotoErr(EFail);
            break;
        } // switch (m_HttpState)

        if (err) {
            gotoErr(err);
        }
    } // while (PARSE_BUFFER == action)

abort:
    if (GET_MORE_DATA == action) {
       // If we got cutoff from the server but we still need to read
       // more, then we lost the page.
       if (resultErr) {
           err = resultErr;
       } else {
           m_pAsyncIOStream->ListenForMoreBytes();
       }
    }

    // To avoid deadlock, we may need to release the lock before calling a callback.
    if (pHeldLock) {
        pHeldLock->Unlock();
        pHeldLock = NULL;
    }

    if ((err) || (FINISHED_DOCUMENT == action)) {
        FinishAsynchOp(err, false);
    }
} // OnReadyToRead.







/////////////////////////////////////////////////////////////////////////////
//
// [ReadHeader]
//
// This is called when the AsyncIOStream reads another chunk of data.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::ReadHeader(ReadAction *pAction, CRefLock **ppHeldLock) {
    ErrVal err = ENoErr;
    bool fFoundHeaderEnd = false;
    char TempBuffer[CParsedUrl::MAX_URL_LENGTH];
    char *pStr;
    bool fFoundIt;
    int32 strLength;
    int16 contentType;
    int16 contentSubType;
    SignObject("CPolyHttpStreamBasic::ReadHeader");


    err = ReceiveHeaderData(
                        m_HeaderAssumptions,
                        &fFoundHeaderEnd,
                        &m_fReadingResponse);
    if (err) {
        gotoErr(err);
    }

    if (!fFoundHeaderEnd) {
        *pAction = GET_MORE_DATA;
        gotoErr(ENoErr);
    }

    // After the first response, we no longer can make any safe
    // assumptions. The next message may be either a request or a
    // response.
    m_HeaderAssumptions = 0;

    // Process any connection directives. These tell us what to
    // do with the connection when we are done. Do this now,
    // before we potentially discard the header.
    // By default, we try to leave the connection open.
    m_HttpStreamFlags &= ~DISCONNECT_ON_CLOSE;
    err = GetStringHeader(
                    "Connection",
                    TempBuffer,
                    sizeof(TempBuffer),
                    &pStr,
                    &strLength);
    if (err) {
        gotoErr(err);
    }
    if (pStr) {
        if (NULL != CStringLib::FindPatternInBuffer(pStr, strLength, "Close", -1)) {
            m_HttpStreamFlags |= DISCONNECT_ON_CLOSE;
        }
        if (NULL != CStringLib::FindPatternInBuffer(pStr, strLength, "Keep-Alive", -1)) {
            m_HttpStreamFlags &= ~DISCONNECT_ON_CLOSE;
        }
    }

    // Check the response code.
    // We may not want to do anything more with this stream if we
    // follow a redirection.
    if (m_fReadingResponse) {
        if ((MOVED_PERMANENTLY_STATUS == m_StatusCode)
            || (MOVED_TEMPORARILY_STATUS == m_StatusCode)
            || (USE_PROXY_STATUS == m_StatusCode)
            || (SWITCH_PROXY_STATUS == m_StatusCode)) {
            // To avoid deadlock, we may need to release the lock before calling a callback.
            if ((ppHeldLock) && (*ppHeldLock)) {
                (*ppHeldLock)->Unlock();
                *ppHeldLock = NULL;
            }

            *pAction = REDIRECT_REQUEST;

            err = FollowRedirection();
            gotoErr(err);
        } // process a HTTP Redirect response
    } // if (m_fReadingResponse)

    // Otherwise, at this point, we have successfully read
    // a complete header. We may also have some or all of the
    // body.

    // If we are receiving a request, then extract the requested URL.
    if (!m_fReadingResponse) {
        // Get the URL the user requested.
        err = m_pAsyncIOStream->GetPtr(
                                m_RequestURLStartPos,
                                m_RequestURLLength,
                                TempBuffer,
                                sizeof(TempBuffer) - 1,
                                &pStr);
        if (err) {
            gotoErr(err);
        }

        // Parse the requested URL. HTTP 1.1 may pass the entire
        // URL, including the hostname, not just the path.
        err = m_pUrl->Initialize(pStr, m_RequestURLLength, NULL);
        if (err) {
            gotoErr(err);
        }
    } // if (!m_fReadingResponse)


    err = GetIntegerHeader("Content-Length", &m_ContentLengthToLoad, &fFoundIt);
    if (err) {
        gotoErr(err);
    }
    if (!fFoundIt) {
        m_ContentLengthToLoad = -1;
    }

    // Check if this is chunked. A lot of HTTP 1.1 transfers seem to be.
    err = GetStringHeader(
                    "Transfer-Encoding",
                    TempBuffer,
                    sizeof(TempBuffer) - 1,
                    &pStr,
                    &strLength);
    if (err) {
        gotoErr(err);
    }

    // If the content is chunked, then use the chunk headers to
    // tell how much more data to read.
    if ((pStr)
        && (NULL != CStringLib::FindPatternInBuffer(pStr, strLength, "chunked", -1))) {
        m_HttpStreamFlags &= ~READ_LAST_CHUNK_HEADER;
        m_CurrentChunkNum = 0;
        m_StartChunkHeaderPosition = m_StartBodyPos;

        m_HttpState = READING_CHUNK;

        gotoErr(ENoErr);
    } // CHUNKED


    // Check if the content contains DIME attachments.
    err = GetContentType(&contentType, &contentSubType);
    if (err) {
        gotoErr(err);
    }

    // Otherwise, this is not chunked and not DIME.
    m_HttpState = READING_BODY;

abort:
    returnErr(err);
} // ReadHeader.








/////////////////////////////////////////////////////////////////////////////
//
// [ReadBodyData]
//
// This is called when the AsyncIOStream reads another chunk of data.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::ReadBodyData(ErrVal resultErr, ReadAction *pAction) {
    ErrVal err = ENoErr;
    int32 bytesToLoad = 0;
    int64 totalDataLength;
    SignObject("CPolyHttpStreamBasic::ReadBodyData");


    DEBUG_LOG("CPolyHttpStreamBasic::ReadBodyData. m_ContentLengthToLoad = %d",
          (int32) m_ContentLengthToLoad);
    totalDataLength = m_pAsyncIOStream->GetDataLength();
    DEBUG_LOG("CPolyHttpStreamBasic::ReadBodyData. totalDataLength = %d",
          (int32) totalDataLength);

    // Check if we have read the entire file. This may be true
    // even if EOF == resultErr
    //////////////////////////////////////////
    if (m_ContentLengthToLoad >= 0) {
        bytesToLoad = m_ContentLengthToLoad
               + (int32) (m_StartBodyPos - m_StartHeaderPos);

        if (m_pAsyncIOStream->GetDataLength() >= bytesToLoad) {
            *pAction = FINISHED_DOCUMENT;
            gotoErr(ENoErr);
        }

        // If we still need to read more of the body, then any error, even
        // an EOF is bad.
        if (resultErr) {
            gotoErr(resultErr);
        }
    } // if (contentLength >= 0)
    //////////////////////////////////////////
    else { // if ((m_ContentLengthToLoad < 0)
        // EOF is normal for a web server that does not
        // provide a content-length in the header; it just
        // closes the connection when it has provided the
        // entire page.
        if ((EEOF == resultErr) && (m_fReadingResponse)) {
            m_HttpStreamFlags &= ~CONNECTED_TO_PEER;

            *pAction = FINISHED_DOCUMENT;
            gotoErr(ENoErr);
        }

        if (resultErr) {
            gotoErr(resultErr);
        }
    } // if (contentLength < 0)

    *pAction = GET_MORE_DATA;

abort:
    returnErr(err);
} // ReadBodyData.







/////////////////////////////////////////////////////////////////////////////
//
// [FollowRedirection]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::FollowRedirection() {
    ErrVal err = ENoErr;
    char tempBuffer[CParsedUrl::MAX_URL_LENGTH];
    char *pUrl = NULL;
    int32 urlLength;
    CParsedUrl *pRedirectUrl = NULL;
    CRefLock *pHeldLock = NULL;
    SignObject("CPolyHttpStreamBasic::FollowRedirection");

    if (m_pLock) {
        m_pLock->Lock();
        pHeldLock = m_pLock;
    }

    err = GetStringHeader(
                            "Location",
                            tempBuffer,
                            sizeof(tempBuffer),
                            &pUrl,
                            &urlLength);
    if (err) {
        gotoErr(err);
    }
    if (NULL == pUrl) {
        gotoErr(ENoResponse);
    }

    // Sometimes, sites give a relative URL as their redirect.
    // As a result, we have to use the current URL as a base.
    pRedirectUrl = newex CParsedUrl;
    if (NULL == pRedirectUrl) {
        gotoErr(EFail);
    }
    err = pRedirectUrl->Initialize(pUrl, urlLength, m_pUrl);
    if (err) {
        gotoErr(err);
    }

    {
       char *buf = (char *) memAlloc(urlLength + 2);
       memcpy(buf, pUrl, urlLength);
       buf[urlLength] = 0;
       DEBUG_LOG("CPolyHttpStreamBasic - Redirect to: %s", buf);
       memFree(buf);
    }

    if ((NULL == pRedirectUrl->m_pHostName)
        || (pRedirectUrl->m_HostNameSize <= 0)) {
        gotoErr(ENoResponse);
    }

    // Avoid cyclic redirects.
    if ((pRedirectUrl->Equal(CParsedUrl::ENTIRE_URL, m_pUrl))
        || (m_NumRedirects >= MAX_REASONABLE_REDIRECTS)) {
        gotoErr(ENoResponse);
    }
    m_NumRedirects += 1;

    RELEASE_OBJECT(m_pUrl);
    m_pUrl = pRedirectUrl;
    pRedirectUrl = NULL;

    // Fill in the header.
    PrepareHeaderForRequest(m_HttpOpInSendRequest, false);
    if (err) {
        gotoErr(err);
    }

    // Release the lock, because we are discarding the connection.
    if (pHeldLock) {
        pHeldLock->Unlock();
        pHeldLock = NULL;
    }
    CloseStreamToURL();

    err = OpenStreamToURL(m_pUrl);
    if (err) {
        gotoErr(err);
    }

abort:
    if (pHeldLock) {
        pHeldLock->Unlock();
        pHeldLock = NULL;
    }

    RELEASE_OBJECT(pRedirectUrl);
    returnErr(err);
} // FollowRedirection.







/////////////////////////////////////////////////////////////////////////////
//
// [FinishAsynchOp]
//
/////////////////////////////////////////////////////////////////////////////
void
CPolyHttpStreamBasic::FinishAsynchOp(ErrVal resultErr, bool fWriteOp) {
    CPolyHttpCallback *pCallback = NULL;
    void *pCallbackContext = NULL;
    CAsyncIOStream *pStreamToBeClosed = NULL;
    SignObject("CPolyHttpStreamBasic::FinishAsynchOp");


    { ///////////////////////////////////////////
        AutoLock(m_pLock);

        m_HttpState = IDLE;

        // Once we have completed a command, discard the source for a POST body.
        // We may send different requests with different contents if we reuse
        // an http 1.1 connection.
        RELEASE_OBJECT(m_pSendAsyncIOStream);

        if (resultErr) {
            pStreamToBeClosed = m_pAsyncIOStream;
            m_HttpStreamFlags &= ~CONNECTED_TO_PEER;

#if DD_DEBUG
            // A good place to put a breakpoint.
            resultErr = resultErr;
#endif // DD_DEBUG
        }

        if ((fWriteOp) && (!m_fKeepAlive)) {
            pStreamToBeClosed = m_pAsyncIOStream;
            m_HttpStreamFlags &= ~CONNECTED_TO_PEER;
        }

        // Save the callback. The callback may start a new asynch op before returning.
        pCallback = m_pCallback;
        ADDREF_OBJECT(pCallback);
        pCallbackContext = m_pCallbackContext;
    } ///////////////////////////////////////////


    // When we don't send a body, we need to close to mark the end of
    // the document. Even if we do send a body, we don't want to use the
    // resources to keep a stream open longer than necessary.
    if (NULL != pStreamToBeClosed) {
        pStreamToBeClosed->Close();
    }

    if (NULL != pCallback) {
        if (fWriteOp) {
            pCallback->OnWriteHTTPDocument(resultErr, this, pCallbackContext);
        } else {
            pCallback->OnReadHTTPDocument(resultErr, this, pCallbackContext);
        }

        RELEASE_OBJECT(pCallback);
    }

    /////////////////////////////////////////////////////
    {
        AutoLock(m_pLock);

        // We either read a response, or else we finished sending a response.
        // In either case, this request/response transaction is over.
        if ((m_fKeepAlive) && (!resultErr)) {
            //<><>PrepareToReadNextRequest();
        }
    }
    /////////////////////////////////////////////////////
} // FinishAsynchOp.








/////////////////////////////////////////////////////////////////////////////
//
// [ReadChunks]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::ReadChunks(ErrVal resultErr, ReadAction *pAction) {
    ErrVal err = ENoErr;
    char tempBuffer[MAX_CHUNK_HEADER_SIZE];
    char *pChunkHeader;
    char *pPtr;
    char *pEndPtr;
    int32 size;
    int32 chunkSize;
    int32 chunkHeaderSize;
    int32 trailerSize;
    int32 dataToRemoveFromPreviousChunk;
    bool fFoundEntireChunkHeader;
    AutoLock(m_pLock);
    SignObject("CPolyHttpStreamBasic::ReadChunks");



    if (resultErr) {
        if (EEOF == resultErr) {
            *pAction = FINISHED_DOCUMENT;
            resultErr = ENoErr;
        }
        gotoErr(resultErr);
    }

    // Each iteration reads one chunk. We stop when we find either
    // a partially downloaded chunk or the last chunk.
    while (1) {
        err = m_pAsyncIOStream->SetPosition(m_StartChunkHeaderPosition);
        if (err) {
            if (EEOF == err) {
                *pAction = GET_MORE_DATA;
                err = ENoErr;
            }

            gotoErr(err);
        }

        size = (int32) (m_pAsyncIOStream->GetDataLength() - m_StartChunkHeaderPosition);
        if (size > MAX_CHUNK_HEADER_SIZE) {
            size = MAX_CHUNK_HEADER_SIZE;
        }

        err = m_pAsyncIOStream->GetPtr(
                                m_StartChunkHeaderPosition,
                                size,
                                tempBuffer,
                                sizeof(tempBuffer),
                                &pChunkHeader);
        if (err) {
            if (EEOF == err) {
                *pAction = GET_MORE_DATA;
                err = ENoErr;
            }
            gotoErr(err);
        }


        // If we read the last chunk, then we are just looking for the trailer.
        if (m_HttpStreamFlags & READ_LAST_CHUNK_HEADER) {
            // Keep searching until we find the end of the
            // trailer or run out of buffer.
            pPtr = pChunkHeader;
            pEndPtr = pChunkHeader + size;
            while (pPtr < pEndPtr) {
                // We found a CR-LF sequence.
                if (((pPtr + 2) <= pEndPtr)
                    && (CStringLib::IsByte(*pPtr, CStringLib::NEWLINE_CHAR))
                    && (CStringLib::IsByte(*(pPtr + 1), CStringLib::NEWLINE_CHAR))) {
                    *pAction = FINISHED_DOCUMENT;

                    // Remove the chunk trailer so the data is smooth and uninterrupted.
                    pPtr += 2;
                    trailerSize = pPtr - pChunkHeader;
                    err = m_pAsyncIOStream->RemoveNBytes(
                                            m_StartChunkHeaderPosition,
                                            trailerSize);
                    gotoErr(err);
                } // Find a CR-LF sequence.

                pPtr++;
            } // while (pPtr < pEndPtr)

            *pAction = GET_MORE_DATA;
            gotoErr(ENoErr);
        } // (m_HttpStreamFlags & READ_LAST_CHUNK_HEADER)


        // Otherwise, this is the start of a normal chunk.
        // Read the chunk size.
        pPtr = pChunkHeader;
        pEndPtr = pChunkHeader + size;
        while ((pPtr < pEndPtr) && (CStringLib::IsByte(*pPtr, CStringLib::HEX_CHAR))) {
            pPtr++;
        }
        err = CStringLib::StringToNumberEx(pChunkHeader, pPtr - pChunkHeader, 16, &chunkSize);
        if (err) {
            // We couldn't reads the chunk size. This means the chunk header
            // is malformed, so we cannot correctly read the document.
            gotoErr(err);
        }

        // Skip over the transfer extension. The chunk header is
        // terminated by a CR-LF sequence.
        fFoundEntireChunkHeader = false;
        while ((pPtr < pEndPtr) && (!(CStringLib::IsByte(*pPtr, CStringLib::NEWLINE_CHAR)))) {
            pPtr++;
        }
        if ((pPtr + 2) <= pEndPtr) {
            fFoundEntireChunkHeader = true;
            if (CStringLib::IsByte(*pPtr, CStringLib::NEWLINE_CHAR)) {
                pPtr++;
            }
            if (CStringLib::IsByte(*pPtr, CStringLib::NEWLINE_CHAR)) {
                pPtr++;
            }
        }

        // If we didn't hit the end of the chunk, then quit. We will
        // try again on the next chunk.
        if (!fFoundEntireChunkHeader) {
            *pAction = GET_MORE_DATA;
            break;
        }

        // The next chunk begins after this chunk ends.
        chunkHeaderSize = pPtr - pChunkHeader;

        // Remove the chunk header so the data is smooth and uninterrupted.
        // If this is the second or later chunk, then also remove the
        // CR-LF that terminated the previous chunk.
        dataToRemoveFromPreviousChunk = 0;
        if (m_CurrentChunkNum > 0) {
            dataToRemoveFromPreviousChunk = 2;
        }
        err = m_pAsyncIOStream->RemoveNBytes(
                            m_StartChunkHeaderPosition - dataToRemoveFromPreviousChunk,
                            chunkHeaderSize + dataToRemoveFromPreviousChunk);
        if (err) {
            gotoErr(err);
        }

        // Get the position of the next chunk.
        m_CurrentChunkNum += 1;
        m_StartChunkHeaderPosition += chunkSize;

        // This chunk will end with a CR-LF.
        if (chunkSize > 0) {
            m_StartChunkHeaderPosition += 2;
        }

        m_StartChunkHeaderPosition
            = m_StartChunkHeaderPosition - dataToRemoveFromPreviousChunk;

        // The last chunk has size 0.
        if (0 == chunkSize) {
            m_HttpStreamFlags |= READ_LAST_CHUNK_HEADER;
        }
    } // Read each chunk.

abort:
    returnErr(err);
} // ReadChunks.







/////////////////////////////////////////////////////////////////////////////
//
// [GetUrl]
//
/////////////////////////////////////////////////////////////////////////////
CParsedUrl *
CPolyHttpStreamBasic::GetUrl() {
    if (m_pUrl) {
        ADDREF_OBJECT(m_pUrl);
    }

    return(m_pUrl);
} // GetUrl.






/////////////////////////////////////////////////////////////////////////////
//
// [GetIOStream]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::GetIOStream(
                        CAsyncIOStream **ppSrcStream,
                        int64 *pStartPosition,
                        int32 *pLength) {
    ErrVal err = ENoErr;
    int32 contentLength;
    bool fFoundIt;

    if (NULL == m_pAsyncIOStream) {
        gotoErr(EFail);
    }

    if (NULL != ppSrcStream) {
        *ppSrcStream = m_pAsyncIOStream;
        ADDREF_OBJECT(m_pAsyncIOStream);

        err = m_pAsyncIOStream->SetPosition(m_StartBodyPos);
        // Handle the special case of an empty request.
        // This is important and common, and is not an error.
        if ((EEOF == err)
            && (m_StartBodyPos >= m_pAsyncIOStream->GetDataLength())) {
            err = ENoErr;
        }
        if (err) {
            gotoErr(err);
        }
    }
    if (NULL != pStartPosition) {
        *pStartPosition = m_StartBodyPos;
    }
    if (NULL != pLength) {
        err = GetIntegerHeader(
                            "Content-Length",
                            &contentLength,
                            &fFoundIt);
        if (err) {
            gotoErr(err);
        }
        if (!fFoundIt) {
            contentLength = (int32) (m_pAsyncIOStream->GetDataLength()
                                        - m_StartBodyPos);
            if (contentLength < 0) {
                contentLength = 0;
            }
        }

        *pLength = contentLength;
    }

abort:
    returnErr(err);
} // GetIOStream.






/////////////////////////////////////////////////////////////////////////////
//
// [OnStreamDisconnect]
//
/////////////////////////////////////////////////////////////////////////////
void
CPolyHttpStreamBasic::OnStreamDisconnect(ErrVal err, CAsyncIOStream *pAsyncIOStream, void *pContext) {
    CPolyHttpCallback *pEventCallback = NULL;
    void *pEventCallbackContext = NULL;
    SignObject("CPolyHttpStreamBasic::OpenStreamToURL");

    pAsyncIOStream = pAsyncIOStream;
    pContext = pContext;

    ///////////////////////////////////////////////////////
    {
        AutoLock(m_pLock);

        pEventCallback = m_pCallback;
        pEventCallbackContext = m_pCallbackContext;
        ADDREF_OBJECT(pEventCallback);
    }
    ///////////////////////////////////////////////////////


    if (NULL != pEventCallback) {
        pEventCallback->OnPeerDisconnect(err, this, pEventCallbackContext);
        RELEASE_OBJECT(pEventCallback);
    }
} // OnStreamDisconnect






/////////////////////////////////////////////////////////////////////////////
//
// [InitHeader]
//
// This InitHeaders all header fields to their default.
/////////////////////////////////////////////////////////////////////////////
void
CPolyHttpStreamBasic::InitHeader(bool fChangeFromSendToReceive) {
    // Discard any previously allocated data structures.
    while (NULL != m_pHeaderLines) {
        CHttpHeaderLine *pHeaderLine = m_pHeaderLines;
        m_pHeaderLines = m_pHeaderLines->m_pNext;
        delete pHeaderLine;
    }

    // Reset the state.
    m_StatusCode = OK_STATUS;

    m_RequestURLStartPos = -1;
    m_RequestURLLength = 0;

    m_MessageStartPos = -1;
    m_StartHeaderPos = -1;

    m_HttpMajorVersion = 1;
    m_HttpMinorVersion = 0;

    if (!fChangeFromSendToReceive) {
        m_HttpOp = HTTP_GET_MSG;
        m_HttpOpInSendRequest = HTTP_GET_MSG;

        m_numBytesAvailable = 0;
        m_numBytesProcessed = 0;
        m_EndLineChars[0] = 0;
        m_NumEndLineChars = 0;
        m_StartBodyPos = 0;
    }
} // InitHeader.








/////////////////////////////////////////////////////////////////////////////
//
// [WriteRequestToStream]
//
// This does not flush the write, since a body may follow the header.
// As a result, it is synchronous.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::WriteRequestToStream(CParsedUrl *url, CAsyncIOStream *pStream) {
    ErrVal err = ENoErr;

    // Don't call RunChecks because we do not have an open stream yet.
    // Be careful. Sometimes the URL may not have a host name, like
    // a Gnutella request.
    if ((NULL == url) || (NULL == pStream)) {
        gotoErr(EFail);
    }

    // Save enough state so we can properly parse a response.
    // It needs to know what kind of request it sent
    // (ie HEAD or GET) to know how to parse the response.
    m_HttpOpInSendRequest = m_HttpOp;

    // Write the request command.
    err = pStream->printf("%s ", GetTextForId(m_HttpOp, g_RequestOpList, NULL));
    if (err) {
        gotoErr(err);
    }

    // If we are going through a proxy, then the url should look
    // like: host:port/foo.html
    if ((g_fUseHTTPProxy)
        && (url->m_pHostName)
        && (url->m_HostNameSize > 0)) {
        err = pStream->Write(g_HttpUrlScheme, g_HttpUrlSchemeSize);
        if (err) {
            gotoErr(err);
        }
        err = pStream->Write(url->m_pHostName, url->m_HostNameSize);
        if (err) {
            gotoErr(err);
        }
        if (CParsedUrl::DEFAULT_PORT_HTTP != url->m_Port) {
            err = pStream->printf(":%d", url->m_Port);
            if (err) {
                gotoErr(err);
            }
        } // writing the port.
    } // writing the proxy.


    // Write the URL. This either stands alone, or extends the
    // URL begun with the proxy host name.
    if ((NULL == url->m_pPath) || (0 == url->m_pPath[0])) {
        if (HTTP_OPTIONS_MSG == m_HttpOp) {
            err = pStream->PutByte('*');
            if (err) {
                gotoErr(err);
            }
        } else {
            err = pStream->PutByte('/');
            if (err) {
                gotoErr(err);
            }
        }
    } else
    {
        char *pEndPtr;
        char tempBuffer[2040];

        if (('/' != url->m_pPath[0]) && (g_fUseHTTPProxy)) {
            err = pStream->PutByte('/');
            if (err) {
                gotoErr(err);
            }
        }

        err = url->PrintToString(
                        CParsedUrl::PATH_AND_SUFFIX,
                        tempBuffer,
                        sizeof(tempBuffer),
                        &pEndPtr);
        if (err) {
            gotoErr(err);
        }
        // It's null-terminated, so back up one.
        pEndPtr--;

        err = pStream->Write(tempBuffer, pEndPtr - tempBuffer);
        if (err) {
            gotoErr(err);
        }
    }

    err = pStream->PutByte(' ');
    if (err) {
        gotoErr(err);
    }


    // Write the HTTP version. Version 0.9 HTTP does not do this.
    if (m_HttpMajorVersion >= 1) {
        err = pStream->printf("%s/", g_HttpVersionPrefix);
        if (err) {
           gotoErr(err);
        }
        err = WriteVersionNum(pStream, m_HttpMajorVersion, m_HttpMinorVersion);
        if (err) {
           gotoErr(err);
        }
    }

    err = WriteEndOfHTTPLine(pStream);
    if (err) {
        gotoErr(err);
    }

    // Write the header lines.
    err = WriteHeaderLines(pStream, url);
    if (err) {
        gotoErr(err);
    }

abort:
    returnErr(err);
} // WriteRequestToStream.




/////////////////////////////////////////////////////////////////////////////
//
// [ParseResponseHeader]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::ParseResponseHeader() {
    ErrVal err = ENoErr;
    bool foundIt;
    char returnCodeBuffer[4];

    InitHeader(true);

    // Allocate and initialize the new empty name list for this header.
    if (NULL == m_pNameList) {
        m_pNameList = newex CNameTable;
        if (NULL == m_pNameList) {
            gotoErr(EFail);
        }
        err = m_pNameList->InitializeWithParentNameTable(m_pGlobalNameList);
        if (err) {
            gotoErr(err);
        }
    } // if (NULL == m_pNameList)

    m_StatusCode = OK_STATUS;
    m_HttpOp = HTTP_RESPONSE_MSG;

    // The HTTP 1.1 spec says that we should ignore any CRLF chars
    // before the first line of the header. These may separate
    // sequential HTTP messages when a connection is reused.
    err = m_pAsyncIOStream->SkipWhileCharType(CStringLib::NEWLINE_CHAR);
    if (err) {
        gotoErr(err);
    }

    // In HTTP 1.1, it is useful to know where each message
    // in a connection starts.
    m_MessageStartPos = m_pAsyncIOStream->GetPosition();
    m_StartHeaderPos = m_MessageStartPos;

    // Full responses start with a version. Simple responses
    // immediately start with the data.
    foundIt = false;
    err = ParseHTTPVersion(&foundIt);
    if (err) {
        gotoErr(err);
    }
    if (!foundIt) {
        m_HttpMajorVersion = 0;
        m_HttpMinorVersion = 9;
        gotoErr(ENoErr);
    }

    // A space (there may be several) separates the version string
    // from the status code.
    err = m_pAsyncIOStream->SkipWhileCharType(CStringLib::NON_NEWLINE_WHITE_SPACE_CHAR);
    if (err) {
        gotoErr(err);
    }

    // Read the status code
    err = m_pAsyncIOStream->Read(returnCodeBuffer, 3);
    if (err) {
        gotoErr(err);
    }
    returnCodeBuffer[3] = 0;
    sscanf(returnCodeBuffer, "%d", &m_StatusCode);

    // Go to the beginning of the status phrase.
    err = m_pAsyncIOStream->SkipWhileCharType(CStringLib::NON_NEWLINE_WHITE_SPACE_CHAR);
    if (err) {
        gotoErr(err);
    }

    // Skip the text message; all we need is the numeric status code.
    err = GotoEndOfHTTPLine();
    if (err) {
        gotoErr(err);
    }

    // Read the header. This will force the data to be read in
    // from the network. It may timeout if the server takes too
    // long to respond.
    err = ParseHeader();
    if (err) {
        gotoErr(err);
    }

abort:
    returnErr(err);
} // ParseResponseHeader.








/////////////////////////////////////////////////////////////////////////////
//
// [ReceiveHeaderData]
//
// This is called by the higher level http stream code. It determines whether
// we have read a complete header yet. The header is then read to get the total
// message length.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::ReceiveHeaderData(
                    int32 flags,
                    bool *pHeaderComplete,
                    bool *pfResponseHeader) {
    ErrVal err = ENoErr;
    char *pPtr;
    int32 bufferLen;
    bool fFoundHeaderEnd;

    if (NULL == pHeaderComplete) {
        gotoErr(EFail);
    }
    *pHeaderComplete = false;

    m_numBytesAvailable = (int32) (m_pAsyncIOStream->GetDataLength());

    // Each iteration examines one buffer. We use GetPtrRef to get
    // a pointer to the raw AsyncIOStream buffer.
    while (m_numBytesProcessed < m_numBytesAvailable) {
        err = m_pAsyncIOStream->GetPtrRef(m_numBytesProcessed, -1, &pPtr, &bufferLen);
        if (err) {
            gotoErr(err);
        }
        if ((NULL == pPtr) || (0 == bufferLen)) {
            break;
        }

        // If this is the first buffer, then decide whether it is a response
        // or a request. Depending on how loose I want to be with HTTP 1.1
        // recycling connections, requests and responses may be interleaved
        // on a single connection. The HTTP spec is stricter, but don't assume
        // the peer is well-behaved.
        if (0 == m_numBytesProcessed) {
            *pfResponseHeader = false;

            // A normal response will look like this:
            //
            //   HTTP/1.0 200 OK
            //
            // or else this:
            //
            //   HTTP 200 OK
            //
            // Sometimes a version string in a response will be "HTTP" with
            // no "/1.0". This is not a fatal error.
            if ((bufferLen >= g_HttpVersionPrefixLen)
                && !(flags & EXPECT_HEADER_TO_BE_REQUEST)
                && (0 == strncasecmpex(g_HttpVersionPrefix, pPtr, g_HttpVersionPrefixLen))) {
                *pfResponseHeader = true;
            }
            // If it is not a normal response, then it is *probably* a request.
            // Be careful here. Some requests may have unusual commands, beyond the
            // usual GET, POST, PUT, etc. For example, they may have proprietary commands.
            // So, we cannot assume that we will recognize the command.
            else if ((bufferLen >= MAX_KNOWN_REQUEST_OP_NAME_LENGTH)
                    && (g_RequestOpGrammar.ParseStringEx(pPtr, bufferLen, NULL, NULL, NULL))
                    && !(flags & EXPECT_HEADER_TO_BE_RESPONSE)) {
                *pfResponseHeader = false;
            }
            // Now we are in trouble. We don't recognize the message as either a request
            // or a response. Version 0.9 HTTP servers do not return a header, the
            // response is just the body of the document.
            // If we expect this to be a request or response, then use our assumption.
            // For example, we may never have sent a request over this stream, so it
            // should never be a response.
            else if (flags & EXPECT_HEADER_TO_BE_REQUEST) {
                *pfResponseHeader = false;
            }
            else if (flags & EXPECT_HEADER_TO_BE_RESPONSE) {
                *pfResponseHeader = true;
                *pHeaderComplete = true;

                m_StatusCode = OK_STATUS;
                m_MessageStartPos = NO_VALUE;
                m_HttpMajorVersion = 0;
                m_HttpMinorVersion = 9;
                m_StartHeaderPos = 0;
                m_StartBodyPos = 0;
                goto abort;
            }
            else {
                // Otherwise, we don't know what to expect and the message is unrecognized. This
                // really is an error. If we don't know what to expect, then we are allowing
                // both requests and responses on the stream. Often this happens when we are
                // talking to another SOAP client, so (i) this should be at least HTTP 1.1,
                // and (ii) this should be using recognized (non-proprietary) Op names.
                // If it does not have those, then this is broken.
                gotoErr(EInvalidHttpHeader);
            }
        } // if (0 == m_numBytesProcessed)

        fFoundHeaderEnd = FindEndOfHeader(m_numBytesProcessed, pPtr, pPtr + bufferLen);
        m_numBytesProcessed += bufferLen;

        if (fFoundHeaderEnd) {
            *pHeaderComplete = true;

            err = m_pAsyncIOStream->SetPosition(0);
            if (err) {
                gotoErr(err);
            }

            if (*pfResponseHeader) {
                err = ParseResponseHeader();
            }
            //else if (!(*pfResponseHeader)) { err = ParseRequestHeader(pAsyncIOStream); }

            gotoErr(err);
        } // processing a complete header.
    } // examining every buffer.

abort:
    // ENoResponse happens when we have an http 1.1 error.
    if ((EEOF == err) || (ENoResponse == err)) {
        err = ENoErr;
    }

    returnErr(err);
} // ReceiveHeaderData.






/////////////////////////////////////////////////////////////////////////////
//
// [FindEndOfHeader]
//
/////////////////////////////////////////////////////////////////////////////
bool
CPolyHttpStreamBasic::FindEndOfHeader(int32 startBufferPos, char *pPtr, char *pEndPtr) {
    char *pStartBufferPtr = pPtr;
    bool fFoundDoubleNewline = false;


    if ((NULL == pPtr) || (NULL == pEndPtr)) {
        return(false);
    }

    // Try to read as complete an ending as possible.
    // The header ends when there is a double newline.
    // The newlines can be \r\n or \n\r or just \n.
    while (pPtr < pEndPtr) {
        if (CStringLib::IsByte(*pPtr, CStringLib::NEWLINE_CHAR)) {
            m_EndLineChars[m_NumEndLineChars] = *pPtr;
            m_NumEndLineChars += 1;

            // \n\n is a double newline.
            if ((2 == m_NumEndLineChars)
                && ('\n' == m_EndLineChars[0])
                && ('\n' == m_EndLineChars[1])) {
                fFoundDoubleNewline = true;
            }
            // \n\r\n\r or \r\n\r\n is a double newline.
            else if ((4 == m_NumEndLineChars)
                && ('\n' == m_EndLineChars[0])
                && ('\r' == m_EndLineChars[1])
                && ('\n' == m_EndLineChars[2])
                && ('\r' == m_EndLineChars[3])) {
                fFoundDoubleNewline = true;
            }
            else if ((4 == m_NumEndLineChars)
                && ('\r' == m_EndLineChars[0])
                && ('\n' == m_EndLineChars[1])
                && ('\r' == m_EndLineChars[2])
                && ('\n' == m_EndLineChars[3])) {
                fFoundDoubleNewline = true;
            }

            if (fFoundDoubleNewline) {
                m_StartBodyPos = startBufferPos + (pPtr - pStartBufferPtr) + 1;
                return(true);
            }
        } // processing a newline character.
        else {
            // A non-newline means we start looking for the start of the next
            // sequences of newlines.
            m_NumEndLineChars = 0;
        }

        pPtr += 1;
    } // processing every character.

    return(false);
} // FindEndOfHeader.







/////////////////////////////////////////////////////////////////////////////
//
// [CheckState]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::CheckState() {
    ErrVal err = ENoErr;

    if (m_pAsyncIOStream) {
        err = m_pAsyncIOStream->CheckState();
        if (err) {
            gotoErr(err);
        }
    }

abort:
    returnErr(err);
} // CheckState.





/////////////////////////////////////////////////////////////////////////////
//
// [GetStringHeader]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::GetStringHeader(
                    const char *pName,
                    char *pTempBuffer,
                    int maxBufferLength,
                    char **pResultPtr,
                    int32 *pTextLength) {
    ErrVal err = ENoErr;
    CHttpHeaderLine *pLine = NULL;

    if ((NULL == pName)
        || (NULL == pResultPtr)
        || (NULL == pTextLength)) {
        gotoErr(EFail);
    }
    *pResultPtr = NULL;
    *pTextLength = 0;


    pLine = FindHeader(NULL, pName);
    if (NULL == pLine) {
        gotoErr(ENoErr);
    }

    err = GetHeaderLineValue(
                    pLine,
                    pTempBuffer,
                    maxBufferLength,
                    pResultPtr,
                    pTextLength);

abort:
    returnErr(err);
} // GetStringHeader







/////////////////////////////////////////////////////////////////////////////
//
// [AddStringHeader]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::AddStringHeader(const char *pName, const char *pValue, int32 valueLength) {
    ErrVal err = ENoErr;
    CHttpHeaderLine *pLine = NULL;
    CDictionaryEntry *pNameInfo;


    if ((NULL == pName) || (NULL == pValue)) {
        gotoErr(EFail);
    }
    if (valueLength < 0) {
        valueLength = strlen(pValue);
    }

    // Allocate and initialize the new empty name list
    // for this header.
    if (NULL == m_pNameList) {
        m_pNameList = newex CNameTable;
        if (NULL == m_pNameList) {
            gotoErr(EFail);
        }
        err = m_pNameList->InitializeWithParentNameTable(m_pGlobalNameList);
        if (err) {
            gotoErr(err);
        }
    } // if (NULL == m_pNameList)

    pNameInfo = m_pNameList->AddDictionaryEntry(pName, strlen(pName));
    if (NULL == pNameInfo) {
        gotoErr(EFail);
    }


    pLine = newex CHttpHeaderLine;
    if (NULL == pLine) {
        gotoErr(EFail);
    }

    pLine->m_pName = pNameInfo;
    pLine->m_StartPosition = -1;
    pLine->m_Length = 0;
    pLine->m_pValue = NULL;
    pLine->m_pNext = NULL;

    if (pValue) {
        pLine->m_Length = valueLength;
        pLine->m_pValue = (char *) memAlloc(valueLength + 1);
        if (NULL == pLine->m_pValue) {
            gotoErr(EFail);
        }
        memcpy(pLine->m_pValue, pValue, valueLength);
        pLine->m_pValue[valueLength] = 0;
    }

    pLine->m_pNext = m_pHeaderLines;
    m_pHeaderLines = pLine;
    pLine = NULL;

abort:
    if (pLine) {
        delete pLine;
    }

    returnErr(err);
} // AddStringHeader







/////////////////////////////////////////////////////////////////////////////
//
// [GetIntegerHeader]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::GetIntegerHeader(const char *pName, int32 *pValue, bool *pFoundIt) {
    ErrVal err = ENoErr;
    char headerBuffer[1024];
    char *pHeader = NULL;
    char *pEndHeader;
    char *pEndWord;
    int32 headerLength = 0;

    if ((NULL == pName)
        || (NULL == pValue)
        || (NULL == pFoundIt)) {
        gotoErr(EFail);
    }
    *pFoundIt = false;

    err = GetStringHeader(
                pName,
                headerBuffer,
                sizeof(headerBuffer),
                &pHeader,
                &headerLength);
    if ((err) || (NULL == pHeader) || (headerLength <= 0)) {
        gotoErr(err);
    }


    pEndHeader = pHeader + headerLength;
    pEndWord = pHeader;
    while ((pEndWord < pEndHeader)
            && (CStringLib::IsByte(*pEndWord, CStringLib::NUMBER_CHAR))) {
        pEndWord += 1;
    }

    err = CStringLib::StringToNumber(pHeader, pEndWord - pHeader, pValue);
    if (err) {
        gotoErr(err);
    }
    *pFoundIt = true;

abort:
    returnErr(err);
} // GetIntegerHeader.








/////////////////////////////////////////////////////////////////////////////
//
// [AddIntegerHeader]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::AddIntegerHeader(const char *pName, int64 value) {
    ErrVal err = ENoErr;
    char headerBuffer[16];
    int32 value32;
    int32 length;

    if (NULL == pName) {
        gotoErr(EFail);
    }

    value32 = (int32) value;
    snprintf(headerBuffer, sizeof(headerBuffer) - 1, "%d", value32);
    length = strlen(headerBuffer);

    err = AddStringHeader(pName, headerBuffer, length);

abort:
    returnErr(err);
} // AddIntegerHeader.










/////////////////////////////////////////////////////////////////////////////
//
// [FindHeader]
//
/////////////////////////////////////////////////////////////////////////////
CHttpHeaderLine *
CPolyHttpStreamBasic::FindHeader(CHttpHeaderLine *prevHeader, const char *pName) {
    ErrVal err = ENoErr;
    CHttpHeaderLine *pLine = NULL;
    CDictionaryEntry *pNameInfo = NULL;


    if (NULL == pName) {
        gotoErr(EFail);
    }
    if (NULL == m_pNameList) {
        gotoErr(err);
    }

    if (prevHeader) {
        pLine = prevHeader->m_pNext;
    } else
    {
        pLine = m_pHeaderLines;
    }

    if (NULL != m_pNameList) {
        pNameInfo = m_pNameList->LookupDictionaryEntry(pName, strlen(pName));
    }
    if (NULL == pNameInfo) {
        gotoErr(ENoErr);
    }


    while (NULL != pLine) {
        if (pNameInfo == pLine->m_pName) {
            return(pLine);
        }
        pLine = pLine->m_pNext;
    }

abort:
    return(NULL);
} // FindHeader







/////////////////////////////////////////////////////////////////////////////
//
// [GetHeaderLineValue]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::GetHeaderLineValue(
                    CHttpHeaderLine *pLine,
                    char *pTempBuffer,
                    int maxBufferLength,
                    char **pResultPtr,
                    int32 *pTextLength) {
    ErrVal err = ENoErr;
    char *pPtr;
    char *pEndPtr;

    if ((NULL == pLine)
        || (NULL == pTempBuffer)
        || (NULL == pResultPtr)
        || (NULL == pTextLength)) {
        gotoErr(EFail);
    }
    *pResultPtr = NULL;
    *pTextLength = 0;


    if (NULL != pLine->m_pValue) {
        *pTextLength = strlen(pLine->m_pValue);
        *pResultPtr = pLine->m_pValue;
    } else if (pLine->m_StartPosition >= 0) {
        *pTextLength = pLine->m_Length;
         err = m_pAsyncIOStream->GetPtr(
                                pLine->m_StartPosition,
                                pLine->m_Length,
                                pTempBuffer,
                                maxBufferLength,
                                pResultPtr);
         if (err)
         {
             gotoErr(err);
         }
    }

    // Strip any whitespace from the beginning and end.
    if ((NULL != *pResultPtr) && (*pTextLength > 0)) {
        pPtr = *pResultPtr;
        pEndPtr = pPtr + *pTextLength;
        while ((pPtr < pEndPtr) && (CStringLib::IsByte(*pPtr, CStringLib::WHITE_SPACE_CHAR))) {
           pPtr++;
        }
        while ((pPtr < pEndPtr) && (CStringLib::IsByte(*(pEndPtr - 1), CStringLib::WHITE_SPACE_CHAR))) {
           pEndPtr--;
        }
        *pResultPtr = pPtr;
        *pTextLength = pEndPtr - pPtr;
    }

abort:
    returnErr(err);
} // GetHeaderLineValue







/////////////////////////////////////////////////////////////////////////////
//
// [WriteHeaderLines]
//
// The order of fields does not matter, but it is good
// practice to start with general fields, and then follow
// that with fields specific to a request or response.
// Each field starts a new line, and is the field name
// followed immediately by a colon and one space character
// and the value.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::WriteHeaderLines(CAsyncIOStream *pStream, CParsedUrl *pUrl) {
    ErrVal err = ENoErr;
    CHttpHeaderLine *pLine;


    if (NULL == pStream) {
        gotoErr(EFail);
    }


    // DATE
    // HTTP 1.1 Requires a data in all responses for caches to use.
    // Requests should only send a date if they include a body, (like
    // a POST or PUT)
    if ((HTTP_RESPONSE_MSG == m_HttpOp)
         || (HTTP_POST_MSG == m_HttpOp)
         || (HTTP_PUT_MSG == m_HttpOp)) {
        CDateTime LocalDate;
        char dateBuffer[256];
        char *pStopHeader;

        err = pStream->Write("Date: ", 6);
        if (err) {
            gotoErr(err);
        }

        LocalDate.GetLocalDateAndTime();
        err = WriteDate(
                dateBuffer,
                dateBuffer + sizeof(dateBuffer),
                &pStopHeader,
                &LocalDate);
        if (err) {
            gotoErr(err);
        }

        err = pStream->Write(dateBuffer, pStopHeader - dateBuffer);
        if (err) {
            gotoErr(err);
        }

        err = WriteEndOfHTTPLine(pStream);
        if (err) {
            gotoErr(err);
        }
    }


    // HTTP 1.1 requires a host in all requests. This is the host of
    // the content-origin server, not any proxy.
    if ((NULL != pUrl)
        && (pUrl->m_pHostName)
        && (pUrl->m_HostNameSize > 0)) {
        err = pStream->Write("Host: ", 6);
        if (err) {
            gotoErr(err);
        }

        err = pStream->Write(pUrl->m_pHostName, pUrl->m_HostNameSize);
        if (err) {
            gotoErr(err);
        }

        err = WriteEndOfHTTPLine(pStream);
        if (err) {
            gotoErr(err);
        }
    }


    pLine = m_pHeaderLines;
    while (NULL != pLine) {
        if ((pLine->m_pName) && (pLine->m_pName->m_pName)) {
            err = pStream->Write(pLine->m_pName->m_pName, pLine->m_pName->m_NameLength);
            if (err) {
                gotoErr(err);
            }
            err = pStream->printf(": ");
            if (err) {
               gotoErr(err);
            }

            if (NULL != pLine->m_pValue) {
                err = pStream->Write(pLine->m_pValue, pLine->m_Length);
                if (err) {
                    gotoErr(err);
                }
            }

            err = WriteEndOfHTTPLine(pStream);
            if (err) {
                gotoErr(err);
            }
        } // if ((pLine->m_pName) && (pLine->m_pName->m_pName))

        pLine = pLine->m_pNext;
    } // while (NULL != pLine)


    // A blank line marks the end of the header.
    err = WriteEndOfHTTPLine(pStream);
    if (err) {
        gotoErr(err);
    }

abort:
    returnErr(err);
} // WriteHeaderLines.







/////////////////////////////////////////////////////////////////////////////
//
// [ParseHeader]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::ParseHeader() {
    ErrVal err = ENoErr;
    char c;
    CHttpHeaderLine *pHeaderLine;
    CHttpHeaderLine *pPrevHeaderLine;
    char nameBuffer[300];
    char *pDestPtr;
    char *pEndDestPtr;


    if (NULL == m_pAsyncIOStream) {
        gotoErr(EFail);
    }
    pPrevHeaderLine = NULL;

    // This loop reads all of the headers. Headers may appear
    // in any order, and there may be any number of headers.
    // One header may appear several times if its body is a
    // list of comma-separated values. The headers are separated
    // from the body by an empty line.
    while (1) {
        // If the next line only contains a newline, then we have
        // hit a blank line, which marks the separation of the header
        // from the body.
        err = m_pAsyncIOStream->PeekByte(&c);
        if (err) {
            gotoErr(err);
        }

        if (CStringLib::IsByte(c, CStringLib::NEWLINE_CHAR)) {
            err = GotoEndOfHTTPLine();
            if (err) {
                gotoErr(err);
            }

            gotoErr(ENoErr);
        } // checking if we hit the end of the header.


        // A field may wrap lines, but it is discouraged. Normally, if we
        // know the format of the field value, so we parse a multi-line
        // field. If the previous token was undefined, however, then we
        // don't know the format of the value, so it may have multi lines.
        // Do NOT copy this to the strange header text, since we cannot
        // guarantee that it will be restored in the correct order.
        if (CStringLib::IsByte(c, CStringLib::NON_NEWLINE_WHITE_SPACE_CHAR)) {
            err = GotoEndOfHTTPLine();
            if (err) {
                gotoErr(err);
            }

            if (NULL != pPrevHeaderLine) {
               pPrevHeaderLine->m_Length
                     = (int32) (m_pAsyncIOStream->GetPosition() - pPrevHeaderLine->m_StartPosition);
            }

            continue;
        } // skipping a continuation of the previous multi-line field.


        // Allocate a header.
        pHeaderLine = newex CHttpHeaderLine;
        if (NULL == pHeaderLine) {
            gotoErr(EFail);
        }
        pHeaderLine->m_pName = NULL;
        pHeaderLine->m_StartPosition = -1;
        pHeaderLine->m_Length = 0;
        pHeaderLine->m_pValue = NULL;
        pHeaderLine->m_pNext = m_pHeaderLines;
        m_pHeaderLines = pHeaderLine;

        // Read the header line name
        pDestPtr = nameBuffer;
        pEndDestPtr = pDestPtr + sizeof(nameBuffer) - 1;
        while (pDestPtr < pEndDestPtr) {
            err = m_pAsyncIOStream->GetByte(&c);
            if (err) {
                gotoErr(err);
            }

            if (':' == c) {
                break;
            }
            *(pDestPtr++) = c;
        } // while (pDestPtr < pEndDestPtr)

        *pDestPtr = 0;
        pHeaderLine->m_pName = m_pNameList->AddDictionaryEntry(nameBuffer, pDestPtr - nameBuffer);
        if (NULL == pHeaderLine->m_pName) {
            gotoErr(EFail);
        }

        // Skip to the body of the line.
        err = m_pAsyncIOStream->SkipWhileCharType(CStringLib::NON_NEWLINE_WHITE_SPACE_CHAR);
        if (err) {
            gotoErr(err);
        }

        // Skip over the line, but record its boundaries.
        pHeaderLine->m_StartPosition = m_pAsyncIOStream->GetPosition();
        err = GotoEndOfHTTPLine();
        if (err) {
            gotoErr(err);
        }
        pHeaderLine->m_Length
           = (int32) (m_pAsyncIOStream->GetPosition() - pHeaderLine->m_StartPosition);
        pPrevHeaderLine = pHeaderLine;
    } // reading all of the headers.

abort:
    returnErr(err);
} // ParseHeader.






/////////////////////////////////////////////////////////////////////////////
//
// [WriteDate]
//
// We can parse dates in any of three formats, but there is one suggested
// format, so we always generate dates in the preferred format. This is
// the RFC 822 (RFC 1123) form: Sun, 06 Nov 1994 08:49:37 GMT
//
// HTTP 1.1 now REQUIRES that we only generate RFC 1123 form dates, this is
// no longer a suggestion. We still have to be able to accept all 3 formats,
// however.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::WriteDate(char *pHeader, char *pEndHeader, char **ppStopPtr, CDateTime *pDate) {
    ErrVal err = ENoErr;
    int16 tensDigit;
    int16 onesDigit;
    int16 intValue;
    int32 dayOfWeek;
    int32 month;
    int32 hour;
    int32 minutes;
    int32 seconds;
    int32 date;
    int32 year;
    const char *tzNameStr;

    if ((NULL == pHeader)
        || (NULL == pEndHeader)
        || (NULL == ppStopPtr)
        || (NULL == pDate)) {
        gotoErr(EFail);
    }


    pDate->GetValue(
               &dayOfWeek,
               &month,
               &hour,
               &minutes,
               &seconds,
               &date,
               &year,
               &tzNameStr);

    strncpyex(pHeader, g_ShortDayNames[dayOfWeek], pEndHeader - pHeader);
    pHeader += strlen(pHeader);
    *(pHeader++) = ',';
    *(pHeader++) = ' ';

    // Date digits have a leading zero if they are less than 10.
    if ((pHeader + 3) >= pEndHeader) {
        gotoErr(EEOF);
    }
    snprintf(pHeader, pEndHeader - pHeader, "%2d ", date);
    pHeader += 3;

    // Month
    strncpyex(pHeader, g_ShortMonthNames[month], pEndHeader - pHeader);
    pHeader += strlen(pHeader);
    *(pHeader++) = ' ';


    // Year
    if ((pHeader + 5) >= pEndHeader) {
        gotoErr(EEOF);
    }
    if (year >= 2000) {
        *(pHeader++) = '2';
        *(pHeader++) = '0';
        intValue = year - 2000;
    } else
    {
        *(pHeader++) = '1';
        *(pHeader++) = '9';
        intValue = year - 1900;
    }
    // Print the year. Do this by hand to make sure we do not
    // try to print more than 2 digits.
    tensDigit = intValue / 10;
    onesDigit = intValue - (10 * tensDigit);
    *(pHeader++) = '0' + tensDigit;
    *(pHeader++) = '0' + onesDigit;
    *(pHeader++) = ' ';

    if (-1 == hour) {
        if ((pHeader + 1) >= pEndHeader) {
            gotoErr(EEOF);
        }

        *(pHeader++) = '?';
    } else
    {
        if ((pHeader + 8) >= pEndHeader) {
            gotoErr(EEOF);
        }

        // Hour digits have a leading zero if they are less than 10.
        tensDigit = hour / 10;
        onesDigit = hour - (10 * tensDigit);
        *(pHeader++) = '0' + tensDigit;
        *(pHeader++) = '0' + onesDigit;
        *(pHeader++) = ':';

        // Minute digits have a leading zero if they are less than 10.
        tensDigit = minutes / 10;
        onesDigit = minutes - (10 * tensDigit);
        *(pHeader++) = '0' + tensDigit;
        *(pHeader++) = '0' + onesDigit;
        *(pHeader++) = ':';

        // Second digits have a leading zero if they are less than 10.
        tensDigit = seconds / 10;
        onesDigit = seconds - (10 * tensDigit);
        *(pHeader++) = '0' + tensDigit;
        *(pHeader++) = '0' + onesDigit;
    }

    // The HTTP 1.1 spec says that all dates MUST be in GMT
    if ((pHeader + 4) >= pEndHeader) {
        gotoErr(EEOF);
    }
    *(pHeader++) = ' ';
    *(pHeader++) = 'G';
    *(pHeader++) = 'M';
    *(pHeader++) = 'T';

    *pHeader = 0;
    *ppStopPtr = pHeader;

abort:
    returnErr(err);
} // WriteDate







/////////////////////////////////////////////////////////////////////////////
//
// [ParseHTTPVersion]
//
// Parse the http version in a request or response.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::ParseHTTPVersion(bool *pRecognized) {
    ErrVal err = ENoErr;
    char versionPrefixBuffer[8];
    char c;

    if (NULL == m_pAsyncIOStream) {
        gotoErr(EFail);
    }
    *pRecognized = true;

    err = m_pAsyncIOStream->Read(versionPrefixBuffer, g_HttpVersionPrefixLen);
    if (err) {
        gotoErr(err);
    }
    if (0 != strncasecmpex(versionPrefixBuffer, g_HttpVersionPrefix, g_HttpVersionPrefixLen)) {
        int64 startPos = m_pAsyncIOStream->GetPosition() - g_HttpVersionPrefixLen;

        m_pAsyncIOStream->SetPosition(startPos);
        m_HttpMajorVersion = 0;
        m_HttpMinorVersion = 9;
        *pRecognized = true;
        gotoErr(ENoErr);
    }

    // Sometimes a version string in a response will be "HTTP" with
    // no "/1.0". This is not a fatal error.
    err = m_pAsyncIOStream->GetByte(&c);
    if (err) {
        gotoErr(err);
    }
    if ('/' != c) {
        (void) m_pAsyncIOStream->UnGetByte();
        *pRecognized = true;
        gotoErr(ENoErr);
    }

    err = ParseVersionNum(&m_HttpMajorVersion, &m_HttpMinorVersion, pRecognized);
    if (err) {
        gotoErr(err);
    }

abort:
    if ((err) && (pRecognized)) {
        *pRecognized = false;
    }

    returnErr(err);
} // ParseHTTPVersion.







/////////////////////////////////////////////////////////////////////////////
//
// [ParseVersionNum]
//
// This parses a version number, and is used for both HTTP
// and MIME versions.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::ParseVersionNum(
                int32 *majorVersion,
                int32 *minorVersion,
                bool *pRecognized
               ) {
    ErrVal err = ENoErr;
    char c;
    bool seenOneFractionDigit;

    if ((NULL == m_pAsyncIOStream) || (NULL == pRecognized)) {
        gotoErr(EFail);
    }
    *pRecognized = true;
    *majorVersion = 0;
    *minorVersion = 0;
    seenOneFractionDigit = false;

    // Read the major release digit.
    while (1) {
        err = m_pAsyncIOStream->GetByte(&c);
        if (err) {
            gotoErr(err);
        }

        if ('.' == c) {
            goto readMinorVersionDigit;
        }

        if (!(CStringLib::IsByte(c, CStringLib::NUMBER_CHAR))) {
            *pRecognized = false;
            gotoErr(err);
        }

        // Ignore leading zeros.
        if ('0' != c) {
            c = c - '0';
            *majorVersion = c;
            break;
        }
    } // reading the integer.

    // Read the '.' that separates integer from fraction.
    err = m_pAsyncIOStream->GetByte(&c);
    if (err) {
        gotoErr(err);
    }
    if ('.' != c) {
        *pRecognized = false;
        gotoErr(err);
    }

readMinorVersionDigit:

    // Read the minor release digit.
    while (1) {
        err = m_pAsyncIOStream->GetByte(&c);
        if (err) {
            gotoErr(err);
        }

        if (!(CStringLib::IsByte(c, CStringLib::NUMBER_CHAR))) {
            if (!seenOneFractionDigit) {
                *pRecognized = false;
                gotoErr(err);
            }

            (void) m_pAsyncIOStream->UnGetByte();
            break;
        }

        // Ignore leading zeros.
        if (c != '0') {
            *minorVersion = (c - '0');
            break;
        }
        seenOneFractionDigit = true;
    } // reading the fraction.

abort:
    if (err) {
        *majorVersion = -1;
        *minorVersion = -1;
        *pRecognized = false;
    }

    returnErr(err);
} // ParseVersionNum.






/////////////////////////////////////////////////////////////////////////////
//
// [WriteVersionNum]
//
// This writes a version number, and is used for both HTTP
// and MIME versions. This will never generate leading zeros.
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::WriteVersionNum(
                    CAsyncIOStream *pStream,
                    int32 major,
                    int32 minor
                   ) {
    ErrVal err = ENoErr;

    if (NULL == pStream) {
        gotoErr(EFail);
    }

    if (-1 != major) {
        err = pStream->PutByte('0' + ((uchar) major));
        if (err) {
            gotoErr(err);
        }
    }

    err = pStream->PutByte('.');
    if (err) {
        gotoErr(err);
    }

    if (-1 != minor) {
        err = pStream->PutByte('0' + ((uchar) minor));
        if (err) {
            gotoErr(err);
        }
    }

abort:
    returnErr(err);
} // WriteVersionNum.








/////////////////////////////////////////////////////////////////////////////
//
// [GotoEndOfHTTPLine]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::GotoEndOfHTTPLine() {
    ErrVal err = ENoErr;
    char c1;
    char c2;

    if (NULL == m_pAsyncIOStream) {
        gotoErr(EFail);
    }

    // Read the first CR or LF. Be careful, we may not be at the
    // the line, so we may have to skip over some whitespace
    // or other text.
    err = m_pAsyncIOStream->SkipUntilCharType(CStringLib::NEWLINE_CHAR);
    if (err) {
        gotoErr(err);
    }

    // Read the newline char we stopped at.
    err = m_pAsyncIOStream->GetByte(&c1);
    if (err) {
        gotoErr(err);
    }

    // It is legal (though ill-advised) to terminate a line with
    // just a carriage return or just a newline. Moreover, the
    // newline, return characters may be out of order. Moreover,
    // routines that handle line wrap, may
    // leave us on the second newline character, not the first.
    err = m_pAsyncIOStream->PeekByte(&c2);
    // This may just mean we hit the end of the file or stream.
    // It is not an error.
    if (err) {
        gotoErr(ENoErr);
    }

    // Normally, c1 is a CR and c2 is a LF. Note, some servers,
    // (like www.best.com) terminate the header with LFLF rather
    // than CRLFCRLF. If the 2 characters are the same (either
    // both are LF or both are CR), then we only use 1 to mark
    // the end of the line. Leave the second to mark the end
    // of an empty line, which marks the end of the header.
    if ((CStringLib::IsByte(c2, CStringLib::NEWLINE_CHAR)) && (c1 != c2)) {
        err = m_pAsyncIOStream->GetByte(&c2);
        if (err) {
            gotoErr(err);
        }
    }

abort:
    returnErr(err);
} // GotoEndOfHTTPLine.





/////////////////////////////////////////////////////////////////////////////
//
// [WriteEndOfHTTPLine]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::WriteEndOfHTTPLine(CAsyncIOStream *pStream) {
    ErrVal err = ENoErr;

    if (NULL == pStream) {
        gotoErr(EFail);
    }

    // WARNING! Be really careful that the order is \r\n.
    // A lot of http servers cannot handle \n\r.
    err = pStream->PutByte('\r');
    if (err) {
        gotoErr(err);
    }

    err = pStream->PutByte('\n');
    if (err) {
        gotoErr(err);
    }

abort:
    returnErr(err);
} // WriteEndOfHTTPLine







/////////////////////////////////////////////////////////////////////////////
//
// [GetContentTypeHeader]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::GetContentTypeHeader(
                                const char *pName,
                                CHttpContentType *pTypeList,
                                int maxTypes,
                                int32 *pNumTypes) {
    ErrVal err = ENoErr;
    bool foundToken = true;
    char headerBuffer[1024];
    char *pHeader;
    char *pEndHeader;
    char *pResumeParsePointer;
    int32 headerLength;
    CHttpHeaderLine *pHeaderLine;


    if ((NULL == pName)
        || (NULL == pTypeList)
        || (NULL == pNumTypes)) {
        gotoErr(EFail);
    }
    *pNumTypes = 0;

    // A single header may appear several times. Each iteration
    // of this loop finds one occurrence.
    pHeaderLine = NULL;
    while (*pNumTypes < maxTypes) {
        pHeaderLine = FindHeader(pHeaderLine, pName);
        if (NULL == pHeaderLine) {
            break;
        }

        err = GetHeaderLineValue(
                    pHeaderLine,
                    headerBuffer,
                    sizeof(headerBuffer),
                    &pHeader,
                    &headerLength);
        if (err) {
            gotoErr(err);
        }

        // Each loop iteration reads 1 token from the current header.
        // A single header line may contain several types.
        pEndHeader = pHeader + headerLength;
        while ((pHeader < pEndHeader) && (*pNumTypes < maxTypes)) {
            m_pParsingContentType = &(pTypeList[*pNumTypes]);
            m_pParsingContentType->m_CharSet = (int16) -1;

            // Go to the next item on the list.
            while ((pHeader < pEndHeader)
                    && ((CStringLib::IsByte(*pHeader, CStringLib::WHITE_SPACE_CHAR))
                        || (',' == *pHeader))) {
                pHeader += 1;
            }

            foundToken = g_HTTPContentTypeGrammar.ParseStringEx(
                                       pHeader,
                                       pEndHeader - pHeader,
                                       this,
                                       NULL,
                                       &pResumeParsePointer);
            if (foundToken) {
                *pNumTypes += 1;
            }

            while ((pHeader < pEndHeader)
                  && (';' != *pHeader)
                  && (!(CStringLib::IsByte(*pHeader, CStringLib::NEWLINE_CHAR)))) {
                pHeader += 1;
            }
            while ((pHeader < pEndHeader)
               && ((';' == *pHeader) || (CStringLib::IsByte(*pHeader, CStringLib::WHITE_SPACE_CHAR)))) {
                pHeader += 1;
            }
        } // Read each token on the current header line.
    } // Read each matching header line.

abort:
    returnErr(err);
} // GetContentTypeHeader.







/////////////////////////////////////////////////////////////////////////////
//
// [AddContentTypeHeader]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::AddContentTypeHeader(
                                const char *pName,
                                CHttpContentType *pTypeList,
                                int numTypes) {
    ErrVal err = ENoErr;
    char headerBuffer[1024];
    int32 index;
    char *pHeader;
    char *pEndHeader;
    int32 length;
    const char *pStr;
    CHttpContentType *pContentType;

    if ((NULL == pName) || (NULL == pTypeList)) {
        gotoErr(EFail);
    }

    pHeader = headerBuffer;
    pEndHeader = headerBuffer + sizeof(headerBuffer) - 1;
    for (index = 0; index < numTypes; index++) {
        pContentType = &(pTypeList[index]);

        pStr = GetTextForId(pContentType->m_Type, g_ContentTypeStrings, &length);
        if ((pHeader + length + 2) >= pEndHeader) {
            gotoErr(EFail);
        }
        strncpyex(pHeader, pStr, length);
        pHeader += length;

        // There MUST be NO white-space between the type and the subtype.
        *(pHeader++) = '/';

        pStr = GetTextForId(pContentType->m_SubType, g_ContentTypeStrings, &length);
        if ((pHeader + length + 2) >= pEndHeader) {
            gotoErr(EFail);
        }
        strncpyex(pHeader, pStr, length);
        pHeader += length;

        if ((index + 1) < numTypes) {
            if ((pHeader + 3) >= pEndHeader) {
                gotoErr(EFail);
            }

            *(pHeader++) = ',';
            *(pHeader++) = ' ';
        }
    } // reading until we get to the comma.

    *pHeader = 0;
    err = AddStringHeader(pName, pHeader, pHeader - headerBuffer);

abort:
    returnErr(err);
} // AddContentTypeHeader.







/////////////////////////////////////////////////////////////////////////////
//
// [GetContentType]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::GetContentType(int16 *pType, int16 *pSubType) {
    ErrVal err = ENoErr;
    CHttpContentType contentType;
    int32 numTypes;

    if ((NULL == pType) || (NULL == pSubType)) {
        gotoErr(EFail);
    }

    *pType = -1;
    *pSubType = -1;

    err = GetContentTypeHeader("Content-Type", &contentType, 1, &numTypes);
    if (err) {
        gotoErr(err);
    }

    if (1 == numTypes) {
        *pType = contentType.m_Type;
        *pSubType = contentType.m_SubType;
        gotoErr(ENoErr);
    }

    if (NULL != m_pUrl) {
        err = m_pUrl->GetImpliedContentType(pType, pSubType);
        if (err) {
            gotoErr(ENoErr);
        }
    }

abort:
    returnErr(err);
} // GetContentType






/////////////////////////////////////////////////////////////////////////////
//
// [GetTextForId]
//
/////////////////////////////////////////////////////////////////////////////
const char *
GetTextForId(int32 id, CStringIDMapping *mapping, int32 *pLength) {
    if (NULL == mapping) {
        if (NULL != pLength) {
            *pLength = 0;
        }
        return("");
    }

    while (NULL != mapping->m_pText) {
        if (id == mapping->m_Id) {
            if (NULL != pLength) {
                if (mapping->m_TextLength < 0) {
                    mapping->m_TextLength = strlen(mapping->m_pText);
                }
                *pLength = mapping->m_TextLength;
            }
            return(mapping->m_pText);
        }
        mapping++;
    }

    if (NULL != pLength) {
        *pLength = 0;
    }
    return("");
} // GetTextForId






/////////////////////////////////////////////////////////////////////////////
//
// [OnParseToken]
//
// CParsingCallback
/////////////////////////////////////////////////////////////////////////////
ErrVal
CPolyHttpStreamBasic::OnParseToken(
                  void *pCallbackContext,
                  int32 tokenId,
                  int32 intValue,
                  const char *pStr,
                  int32 length) {
    m_ParseIntValue = intValue;
    length = length; // Unused
    pCallbackContext = pCallbackContext; // Unused

    switch (tokenId) {
    /////////////////////////////////////////////
    case DAY_TOKEN:
       m_ParsedDate = intValue;
       break;

    /////////////////////////////////////////////
    case SHORT_MONTH_TOKEN:
       m_ParsedMonth = intValue;
       break;

    /////////////////////////////////////////////
    case TWO_DIGIT_YEAR_TOKEN:
        // Y2K conversion.
        if (intValue < 50) {
            intValue += 2000;
        } else {
            intValue += 1900;
        }
        m_ParsedYear = intValue;
        break;

    /////////////////////////////////////////////
    case FOUR_DIGIT_YEAR_TOKEN:
       m_ParsedYear = intValue;
       break;

    /////////////////////////////////////////////
    case HOUR_TOKEN:
       m_ParsedHours = intValue;
       break;

    /////////////////////////////////////////////
    case MINUTES_TOKEN:
       m_ParsedMinutes = intValue;
       break;

    /////////////////////////////////////////////
    case SECONDS_TOKEN:
       m_ParsedSeconds = intValue;
       break;

    /////////////////////////////////////////////
    case TIMEZONE_TOKEN:
       m_ParsedTimeZone[0] = pStr[0];
       m_ParsedTimeZone[1] = pStr[1];
       m_ParsedTimeZone[2] = pStr[2];
       break;

    /////////////////////////////////////////////
    case CONTENT_TYPE_TOKEN:
       m_pParsingContentType->m_Type = (int16) intValue;
       break;

    /////////////////////////////////////////////
    case CONTENT_SUBTYPE_TOKEN:
       m_pParsingContentType->m_SubType = (int16) intValue;
       break;

    /////////////////////////////////////////////
    case RFC822_SHORT_DAY_NAME_TOKEN:
    case ANSI_SHORT_DAY_NAME_TOKEN:
    case RFC_850_LONG_DAY_NAME_TOKEN:
    default:
        break;
    } // switch (tokenId)

    returnErr(ENoErr);
} // OnParseToken



