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

#ifndef _STRING_PARSE_H_
#define _STRING_PARSE_H_

class CParsingContext;



////////////////////////////////////////////////////////////////////////////////
// This is the callback that is invoked by the parser whenever it finds a
// token.
class CParsingCallback {
public:
    virtual ErrVal OnParseToken(
                           void *pCallbackContext,
                           int32 tokenId,
                           int32 intValue,
                           const char *pStr,
                           int32 length) {
        pCallbackContext = pCallbackContext; // Unused.
        tokenId = tokenId; // Unused.
        intValue = intValue; // Unused.
        pStr = pStr; // Unused.
        length = length; // Unused.
        return(ENoErr);
    }
}; // CParsingCallback




////////////////////////////////////////////////////////////////////////////////
class CParseGrammar {
public:
    ///////////////////////////
    enum RuleType {
        STOP_RULE,
        SILENT_RULE,
        TOKEN_RULE,
        INTEGER_RULE
    }; // RuleType


    ///////////////////////////
    enum CParseGrammarConstants {
        DEFAULT_PATH_LENGTH             = 1024,
        DEFAULT_MAX_PENDING_CALLBACKS   = 64,
    }; // CParseGrammarConstants


    ///////////////////////////
    class CParseRule {
    public:
        NEWEX_IMPL()

        RuleType    m_RuleType;

        const char  *m_pVariableName;
        const char  *m_pPattern;

        int32       m_TokenId;
        int32       m_TokenValue;
    }; // CParseRule


    CParseGrammar(CParseGrammar::CParseRule *pRuleList);
    virtual ~CParseGrammar();
    NEWEX_IMPL()

    bool ParseString(
                const char *pText,
                int32 textLength,
                CParsingCallback *pCallback,
                void *pContext);


    // These are used for more advanced applications, like when
    // we need to parse a string several times.
    bool ParseStringEx(
                const char *pText,
                int32 textLength,
                CParsingCallback *pCallback,
                void *pContext,
                char **ppEndPtr);

private:
    class CParseState;
    class CParserVariable;
    class CStateTransition;

    ///////////////////////////
    enum CStateTransitionFlags {
        MAX_RULES_PER_STATE         = 20,
        MAX_OPTIONALS_PER_RULE      = 32,
    };

    ///////////////////////////
    enum TransitionType {
        CHAR_PATTERN,
        VARIABLE_PATTERN,
    };

    ///////////////////////////
    enum StackFrameFunction {
        STATE_TRANSITION,
        EXPANDING_VARIABLE,
        EXPANDING_OPTIONAL_PATTERN,
    }; // StackFrameFunction

    ///////////////////////////
    class CPathStep {
    public:
        StackFrameFunction      m_Function;
        const char              *m_pDebugString;

        const char              *m_pStartText;

        CParseState             *m_pStateAfterVariable;

        int32                   m_NumPendingCallbacks;

        CParseState             *m_pCurrentState;
        CStateTransition        *m_pCurrentTransition;
    }; // CPathStep

    ///////////////////////////
    class CPendingCallback {
    public:
        CParseRule              *m_pRule;
        const char              *m_pStartText;
        const char              *m_pStopText;
    }; // CPendingCallback

    ///////////////////////////
    class CParsingContext {
    public:
        CParsingContext();
        NEWEX_IMPL()

        CParseState             *m_pStateAtEndOfPreviousBuffer;

        CParsingCallback        *m_pCallback;
        void                    *m_pCallbackContext;

        CPathStep               m_DefaultPath[DEFAULT_PATH_LENGTH];
        CPathStep               *m_pPath;
        int32                   m_MaxPathLength;
        int32                   m_PathLength;

        CPendingCallback        m_DefaultPendingCallbacks[DEFAULT_MAX_PENDING_CALLBACKS];
        CPendingCallback        *m_pPendingCallbacks;
        int32                   m_MaxNumPendingCallbacks;
        int32                   m_NumPendingCallbacks;
    }; // CParsingContext


    ///////////////////////////
    // This is a single state transition.
    class CStateTransition {
    public:
        NEWEX_IMPL()

        TransitionType          m_TransitionType;
        bool                    m_fKleene;

        char                    *m_pPatternCharList;
        int32                   m_NumPatternChars;
        int32                   m_PatternCharType;
        CParserVariable         *m_pPatternVariable;

        CParseState             *m_pNextState;

        const char              *m_pDebugString;
        CStateTransition        *m_pNextTransition;

        CStateTransition        *m_pNextOptionalTransition;
    }; // CStateTransition

    ///////////////////////////
    // This is one possible state of the tokenizer.
    class CParseState {
    public:
        NEWEX_IMPL()

        CParseRule              *m_pFinishRule;
        CStateTransition        *m_TransitionList;
        CStateTransition        *m_pOptionalList;

        const char              *m_pDebugString;

        CParseState             *m_pNextStateInGrammar;
    }; // CParseState

    ///////////////////////////
    // This is a variable that is bound to a substring in the text we parse.
    class CParserVariable {
    public:
        NEWEX_IMPL()

        const char              *m_pName;
        int32                   m_NameLength;
        CParseState             *m_pStartState;

        CParserVariable         *m_pNextVariable;
    }; // CParserVariable


    ///////////////////////////////////////////////
    // Grammar Methods
    ///////////////////////////////////////////////
    ErrVal InitializeRules();

    ErrVal AddRule(CParseRule *pRule);

    ErrVal ParseRuleExpansion(
                        const char *pString,
                        CParseState *pState,
                        CParseState **ppFinishState);

    CParserVariable *AddVariable(
                        const char *pStartName,
                        const char **pStopName);

    ErrVal ParseAlternativeList(
                        const char *pStartAlternative,
                        int32 *pMatchFlags,
                        char *pCharList,
                        int32 *pNumChars,
                        bool *pfKleene,
                        const char **ppResultStopAlternative);

    CStateTransition *ParseOptionalPattern(
                        const char *pStartAlternative,
                        const char **ppResultStopAlternative);

    CStateTransition *ParseWhitespacePattern(
                        const char *pStartAlternative,
                        const char **ppResultStopAlternative);

    char GetPatternChar(const char **pPattern);


    ///////////////////////////////////////////////
    // Parsing Methods
    ///////////////////////////////////////////////
    bool ParseSubString(
                void *pContext,
                CStateTransition *pFirstTransition,
                const char *pText,
                const char *pEndText,
                const char **pEndStr);

    bool MatchTransition(
                CParsingContext *pContext,
                CStateTransition *pTransition,
                const char *pText,
                const char *pEndText,
                const char **ppNewText);

    ErrVal FirePendingCallbacks(
                CParsingContext *pContext);

    ErrVal GoToNextStep(
                CParsingContext *pContext,
                CParseState *pNewState,
                const char *pText,
                const char *pEndText,
                const char **ppResumeText);

    ErrVal FinishPathAfterMatch(
                CParsingContext *pContext,
                int32 topStackFrame,
                const char *pText,
                const char *pEndText,
                const char **ppResumeText);

    void BacktrackFromMismatch(
                CParsingContext *pContext,
                int32 topStackFrame);

    void InitializeContext(
                CParsingContext *pContext,
                CParsingCallback *pCallback,
                void *pCallbackContext);
    void ShutdownContext(CParsingContext *pContext);

    int32               m_Options;
    bool                m_InitializedRules;

    CParseRule          *m_pRuleList;
    CParserVariable     *m_pVariableList;
    CParseState         *m_pStateList;

    CParserVariable     *m_StartVariable;
}; // CParseGrammar



// These macros allow you to define grammars.
// A sample grammar definition is:
// DEFINE_GRAMMAR(g_UrlGrammar)
//    DEFINE_RULE("", "<schemeName>://<hostname>[:<port>]/<path>"),
//
//    DEFINE_TOKEN("schemeName", "http", SCHEME_TOKEN, URL_SCHEME_HTTP),
//    DEFINE_TOKEN("schemeName", "https", SCHEME_TOKEN, URL_SCHEME_HTTPS),
//
//    DEFINE_TOKEN("hostname", HOSTNAME_TOKEN, 0),
//    DEFINE_TOKEN("path", PATH_TOKEN, 0),
//    DEFINE_TOKEN("port", PORTNUM_TOKEN, 0),
// STOP_GRAMMAR;
//
// The following characters are reserved, and need to be escaped when they
// appear in a grammar definition: <, >, [, ]
//
#define DEFINE_GRAMMAR(varName) static CParseGrammar::CParseRule varName##RuleList[] = {
#define STOP_GRAMMAR(varName) { CParseGrammar::STOP_RULE, NULL, NULL, -1, -1 } }; CParseGrammar varName(varName##RuleList);

#define DEFINE_RULE(pVarName, pValue)  { CParseGrammar::SILENT_RULE, pVarName, pValue, -1, -1 },
#define DEFINE_TOKEN(pVarName, pValue, tokenId, tokenValue)  { CParseGrammar::TOKEN_RULE, pVarName, pValue, tokenId, tokenValue },
#define DEFINE_INTEGER_TOKEN(pVarName, pValue, tokenId)  { CParseGrammar::INTEGER_RULE, pVarName, pValue, tokenId, -1 },



#endif // _STRING_PARSE_H_


