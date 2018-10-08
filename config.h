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

#ifndef _CONFIG_H_
#define _CONFIG_H_

class CConfigFile;



/////////////////////////////////////////////////////////////////////////////
class CConfigSection
{
public:
    CConfigSection();
    virtual ~CConfigSection();

    bool GetBool(const char *pValueName, bool fDefault);
    int32 GetInt(const char *pValueName, int32 defaultVal);
    CConfigSection *GetSection(const char *pValueName);
    char *GetString(
                const char *pValueName,
                char *defaultValue);
    void GetPathname(
                const char *pValueName,
                char *result,
                int maxResultLen,
                char *defaultValue);

    ErrVal SetString(const char *pValueName, const char *pStr);
    ErrVal SetBool(const char *pValueName, bool value);
    ErrVal SetInt(const char *pValueName, int32 value);
    ErrVal SetPathNameString(const char *pValueName, char *pStr);

    CConfigSection *GetNextSection() {return(m_pNextSection);}

private:
    friend class CConfigFile;

    ///////////////////////////////////
    class CValue
    {
    public:
       CValue();
       virtual ~CValue();

       char             *m_pName;
       int32            m_NameLen;

       char             *m_pStringValue;
       int32            m_StringValueLength;
       CConfigSection   *m_pSubsection;

       CValue           *m_pPrevValue;
       CValue           *m_pNextValue;
     }; // CValue


    CValue *FindValue(CValue *pPrevOption, const char *pValueName);
    ErrVal SetValue(const char *pValueName, const char *pValueData);
    ErrVal AddValue(
                const char *pValueName,
                const char *pValueData,
                CValue **ppResultValue);

    OSIndependantLock   m_OSLock;

    char                *m_pName;
    int32               m_NameLen;

    CValue              *m_pFirstValue;
    CValue              *m_pLastValue;

    CConfigFile         *m_pOwnerDatabase;

    CConfigSection      *m_pNextSection;
}; // CConfigSection






/////////////////////////////////////////////////////////////////////////////
class CConfigFile
{
public:
#if INCLUDE_REGRESSION_TESTS
    static void TestConfig();
#endif

    CConfigFile();
    virtual ~CConfigFile();

    void ReadProductConfig(CProductInfo *pProductInfo);
    ErrVal ReadConfigFile(const char *pFileName);
    ErrVal Save();

    CConfigSection *FindSection(const char *pValueName, bool fCreateIfMissing);
    CConfigSection *FindOrCreateSection(const char *pValueName) {return(FindSection(pValueName, true));}
    ErrVal CreateSection(
                    CConfigSection *pParentGroup,
                    const char *pName,
                    CConfigSection **ppResult);
    ErrVal RemoveSection(CConfigSection *pTargetGroup);

private:
    friend class CConfigSection;

    enum CConfigFileConstants {
        CHANGES_NEED_SAVE   = 0x0001,
    };

    // Reading
    void ReadAndParseFile();
    void RecordPathVariableValues();
    ErrVal ReadOneElement(
                    CConfigSection *pParentGroup,
                    char *pSrcPtr,
                    char **ppResumePtr);
    ErrVal ReadSubSection(
                    CConfigSection *pParentGroup,
                    char *pStopElement,
                    bool fIsCloseElement,
                    char *pStartNameAttribute,
                    char *pStopNameAttribute,
                    char **ppResumePtr);
    ErrVal ReadSimpleValue(
                    CConfigSection *pParentGroup,
                    char *pStartElementBody,
                    char *pStopElement,
                    char *pStartNameAttribute,
                    char *pStopNameAttribute,
                    char **ppResumePtr);

    // Writing
   ErrVal WriteOneSection(
                    CConfigSection *pGroup,
                    int32 indentLevel,
                    char *pFileBuffer,
                    char *pEndBuffer,
                    char *pDestPtr,
                    char **ppResumePtr);
    ErrVal CheckSpaceInWriteBuffer(
                    char *pFileBuffer,
                    char **ppDestPtr,
                    char *pEndBuffer,
                    int32 neededSpace);
    static int32 WriteStringWithEscapes(
                    char *pDestPtr,
                    char *pSrcPtr,
                    int32 maxDestLength);
    static int32 WriteStringAndRemoveEscapes(
                    char *pDestPtr,
                    const char *pSrcPtr,
                    int32 maxDestLength,
                    bool fRemoveEscapeChars);


    int32               m_ConfigFlags;
    OSIndependantLock   m_OSLock;

    char                *m_pInstallDirName;
    char                *m_pFileName;
    char                *m_pFinishedFileName;
    char                *m_pInProgressFileName;
    CSimpleFile         m_FileHandle;

    CConfigSection      *m_pFirstSection;
    CConfigSection      *m_pLastSection;
}; // CConfigFile


extern CConfigFile *g_Config;


#endif // _CONFIG_H_


