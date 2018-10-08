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

#ifndef _STRING_LIB_H_
#define _STRING_LIB_H_



/////////////////////////////////////////////////////////////////////////////
// The main library
//
// All of the functions are static methods of this class. No objects of this
// class type are ever allocated.
/////////////////////////////////////////////////////////////////////////////
class CStringLib {
public:
#if INCLUDE_REGRESSION_TESTS
    static void TestStringLib();
#endif
    // Different character encodings.
    enum StringUtilsEncodingType {
        URL_ENCODING        = 1,
        SIMPLE_ENCODING     = 2,
    };

    // These are used by lots of libraries that may manipulate strings,
    // including red-black trees, b-trees, and others. Usually they are
    // build on top of the common strLib compare functions.
    enum StringLibOptions {
        IGNORE_CASE                    = 0x01,
        NUMBER_TO_STRING_ADD_COMMAS    = 0x02,

        MAX_SEARCH_PATTERN_SIZE        = 256,
    };

    enum StringUtilsCharacterProperties {
        WHITE_SPACE_CHAR                = 0x00000001,
        WORD_CHAR                       = 0x00000002,
        NEWLINE_CHAR                    = 0x00000004,
        HEX_CHAR                        = 0x00000008,
        ALPHANUM_CHAR                   = 0x00000010,
        NUMBER_CHAR                     = 0x00000020,
        NON_NEWLINE_WHITE_SPACE_CHAR    = 0x00000040,
        URL_ENCODABLE_CHAR              = 0x00000080,
        URL_PATH_CHAR                   = 0x00000100,
        URL_HOST_CHAR                   = 0x00000200,
        URL_FRAGMENT_CHAR               = 0x00000400,
        URL_QUERY_CHAR                  = 0x00000800,
        ASCII_CHAR                      = 0x00001000,
    }; // StringUtilsCharacterProperties


    ///////////////////////////////////////////////
    // Character Property Operations
    static int32 GetCharProperties(const char *pChar, int32 length);
    static int32 IsByte(char c, int32 newFlags);


    ///////////////////////////////////////////////
    // Comparison
    static int32 UnicodeStrcmp(
                        const char *pStr1,
                        int32 str1Length,
                        const char *pStr2,
                        int32 maxStr2Length,
                        int32 options);


    ///////////////////////////////////////////////
    // Copying
    static void CopyUTF8String(
                        char *pVoidDestPtr,
                        const char *pVoidSrcPtr,
                        int32 maxLength);


    ///////////////////////////////////////////////
    // Number Operations
    static ErrVal StringToNumber(
                        const char *pStr,
                        int32 strLength,
                        int32 *num) { return(StringToNumberEx(pStr, strLength, 10, num)); }

    static ErrVal StringToNumberEx(
                        const char *pStr,
                        int32 strLength,
                        int32 base,
                        int32 *num);


    ///////////////////////////////////////////////
    // String Representation Conversions (UTF-8 <==> UTF-16)
    static ErrVal ConvertUTF16ToUTF8(
                        const WCHAR *pSrc,
                        int32 srcLengthInBytes,
                        char *pDest,
                        int32 maxDestLengthInBytes,
                        int32 *pActualDestLengthInBytes);

    static ErrVal ConvertUTF8ToUTF16(
                        const char *pSrc,
                        int32 srcLengthInBytes,
                        WCHAR *pDest,
                        int32 maxDestLengthInBytes,
                        int32 *pActualDestLengthInBytes);


    ///////////////////////////////////////////////
    // Encoding operations (escaping, etc)
    static int32 EncodeString(
                        int32 encodingType,
                        void *pDestPtrVoid,
                        int32 maxDestLength,
                        const void *pSrcPtrVoid,
                        int32 strLength);

    static int32 DecodeString(
                        int32 encodingType,
                        const void *pSrcPtrVoid,
                        int32 strLength,
                        void *pDestPtrVoid,
                        int32 maxDestLength);

    static int32 GetMaxEncodedLength(
                        int32 encodingType,
                        const void *pSrcPtrVoid,
                        int32 strLength);


    ///////////////////////////////////////////////
    // Case conversion
    static ErrVal ConvertToUpperCase(
                        const char *pChar,
                        int32 length,
                        char *pResultChar,
                        int32 maxLength,
                        int32 *pActualLength);


    ///////////////////////////////////////////////
    // String Search
    static const char *FindPatternInBuffer(
                        const char *pBuffer,
                        int32 bufferLength,
                        const char *pPattern,
                        int32 patternLength);
}; // CStringLib






/////////////////////////////////////////////////////////////////////////////
// This is used for local variables that convert betwen UTF-8 and UTF-16.
/////////////////////////////////////////////////////////////////////////////
class CTempUTF16String {
public:
    CTempUTF16String();
    ~CTempUTF16String();

    ErrVal AllocateWideStr(int32 numWideChars);
    ErrVal ConvertUTF8String(const char *pChar, int32 numBytes);

    WCHAR   *m_pWideStr;

private:
    enum {
        // This is big; it's 2X this many bytes in local stack space.
        BUILT_IN_BUFFER_SIZE_IN_WIDE_CHARS      = 512,
    };

    WCHAR   *m_pHeapBuffer;
    int32   m_HeapBufferLengthInWideChars;
    WCHAR   m_BuiltInBuffer[BUILT_IN_BUFFER_SIZE_IN_WIDE_CHARS];
}; // CTempUTF16String






/////////////////////////////////////////////////////////////////////////////
// This is used for local variables that convert betwen UTF-8 and UTF-16.
/////////////////////////////////////////////////////////////////////////////
class CTempUTF8String {
public:
    CTempUTF8String();
    ~CTempUTF8String();

    ErrVal AllocateUTF8Str(int32 numChars);
    ErrVal ConvertUTF16String(const WCHAR *pWideStr, int32 numWChars);

    char   *m_pStr;

private:
    enum {
        BUILT_IN_BUFFER_SIZE_IN_UTF8_CHARS      = 512,
    };

    char    *m_pHeapBuffer;
    int32   m_HeapBufferLengthInUTF8Chars;
    char    m_BuiltInBuffer[BUILT_IN_BUFFER_SIZE_IN_UTF8_CHARS];
}; // CTempUTF8String





/////////////////////////////////////////////////////////////////////////////
// Unicode-aware versions of standard libc string functions.
/////////////////////////////////////////////////////////////////////////////

// Prevent calls to unsafe string functions
#define strcpy  )_ERROR_UNSAFE_CALL_TO_STRCPY_USE_STRNCPYEX
#define strncpy )_ERROR_UNSAFE_CALL_TO_STRNCPY_USE_STRNCPYEX
#define strcat  )_ERROR_UNSAFE_CALL_TO_STRCAT_USE_STRNCAT
#define sprintf )_ERROR_UNSAFE_CALL_TO_SPRINTF_USE_SNPRINTF
#define strncpy )_ERROR_UNSAFE_CALL_TO_STRNCPY_USE_STRNCPYEX

#define strncpyex(pDestPtr, pSrcPtr, maxLength) CStringLib::CopyUTF8String(pDestPtr, pSrcPtr, maxLength)
#define strcasecmpex(pStr1, pStr2) CStringLib::UnicodeStrcmp(pStr1, -1, pStr2, -1, CStringLib::IGNORE_CASE)
#define strncasecmpex(pStr1, pStr2, length) CStringLib::UnicodeStrcmp(pStr1, length, pStr2, length, CStringLib::IGNORE_CASE)




/////////////////////////////////////////////////////////////////////////////
//
class CStringToIntegerMap {
public:
    const char  *m_pString;
    int32       m_Int;
}; // CStringToIntegerMap

int32 MapStringToInteger(CStringToIntegerMap *pMap, const char *pString);
int32 MapStringToIntegerEx(CStringToIntegerMap *pMap, const char *pString, int32 strLength);
const char *MapIntegerToString(CStringToIntegerMap *pMap, int32 value);

#endif // _STRING_LIB_H_


