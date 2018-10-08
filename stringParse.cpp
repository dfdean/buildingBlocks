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
// String Parsing Module
//
// This module defines CParseGrammar - A parser object that parses strings
// using a grammar definition. The grammar syntax is based on the IETF
// Augmented BNF used in RFCs and W3C specs.
//
//       [RFC2234] Crocker, D.H. and P. Overell,
//       "Augmented BNF for Syntax Specifications: ABNF", 
//       RFC 2234, November 1997.
//
// This pattern matching mechanism resembles a graph search more than anything.
// I replaced the stack of a previous grammar with a "path" that is all the
// steps we have taken so far. The goal is to find a path through the space
// of states from the initial state to a terminating state. Each step will
// go to a new state. Each state represents one state in the grammar. For
// example, to match "CAT", there are 3 states, one for "C", one for "A",
// and one for "T". This probably has the exact same effect as a typical 
// parser, but it was easier for me to reason about.
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

FILE_DEBUGGING_GLOBALS(LOG_LEVEL_DEFAULT, 0);


class CCharTypeName {
public:
    NEWEX_IMPL()

    const char  *m_pPattern;
    int32       m_PatternLength;
    int32       m_Flags;
};
static CCharTypeName g_AlternativePatterns[] = {
   { "whitespace", -1, CStringLib::WHITE_SPACE_CHAR },
   { "newline", -1, CStringLib::NEWLINE_CHAR },
   { "hex", -1, CStringLib::HEX_CHAR },
   { "ALPHANUM", -1, CStringLib::ALPHANUM_CHAR },
   { "DIGIT", -1, CStringLib::NUMBER_CHAR },
   { "WORD_CHAR", -1, CStringLib::WORD_CHAR },
   { "URL_PATH_CHAR", -1, CStringLib::URL_PATH_CHAR },
   { "URL_HOST_CHAR", -1, CStringLib::URL_HOST_CHAR },
   { "URL_FRAGMENT_CHAR", -1, CStringLib::URL_FRAGMENT_CHAR },
   { "URL_QUERY_CHAR", -1, CStringLib::URL_QUERY_CHAR },
   { NULL, -1, 0 }
};

static const char g_KleeneStarChar              = '*';
static const char g_OpenAlternativeChar         = '(';
static const char g_CloseAlternativeChar        = ')';
static const char g_AlternativeSeparatorChar    = '/';
static const char g_OpenOptionalChar            = '[';
static const char g_CloseOptionalChar           = ']';
static const char g_OpenVariableNameChar        = '<';
static const char g_CloseVariableNameChar       = '>';


///////////////////////////////////////////
class CReentrantParsingCallback : public CParsingCallback {
public:
    virtual ErrVal OnParseToken(
                           void *pCallbackContext,
                           int32 tokenId,
                           int32 intValue,
                           const char *pStr,
                           int32 length);

    int32   m_TokenId;
    int32   m_IntValue;
}; // CReentrantParsingCallback





////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
CParseGrammar::CParsingContext::CParsingContext() {
    m_pCallback = NULL;

    m_pPath = m_DefaultPath;
    m_MaxPathLength = CParseGrammar::DEFAULT_PATH_LENGTH;
    m_PathLength = 0;

    m_pPendingCallbacks = m_DefaultPendingCallbacks;
    m_MaxNumPendingCallbacks = CParseGrammar::DEFAULT_MAX_PENDING_CALLBACKS;
    m_NumPendingCallbacks = 0;

    m_pStateAtEndOfPreviousBuffer = NULL;
} // CParsingContext








////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
CParseGrammar::CParseGrammar(CParseGrammar::CParseRule *pRuleList) {
    m_InitializedRules = false;
    m_pRuleList = pRuleList;
    m_pVariableList = NULL;
    m_Options = CStringLib::IGNORE_CASE;

    m_StartVariable = NULL;
} // CParseGrammar






////////////////////////////////////////////////////////////////////////////////
//
////////////////////////////////////////////////////////////////////////////////
CParseGrammar::~CParseGrammar() {
} // ~CParseGrammar






////////////////////////////////////////////////////////////////////////////////
//
// [InitializeContext]
//
////////////////////////////////////////////////////////////////////////////////
void
CParseGrammar::InitializeContext(
                    CParsingContext *pContext,
                    CParsingCallback *pCallback,
                    void *pCallbackContext) {
    if (NULL == pContext) {
        return;
    }

    pContext->m_pCallback = pCallback;
    pContext->m_pCallbackContext = pCallbackContext;

    pContext->m_pPath = pContext->m_DefaultPath;
    pContext->m_MaxPathLength = DEFAULT_PATH_LENGTH;
    pContext->m_PathLength = 0;

    pContext->m_pPendingCallbacks = pContext->m_DefaultPendingCallbacks;
    pContext->m_MaxNumPendingCallbacks = DEFAULT_MAX_PENDING_CALLBACKS;
    pContext->m_NumPendingCallbacks = 0;

    pContext->m_pStateAtEndOfPreviousBuffer = NULL;
} // InitializeContext




////////////////////////////////////////////////////////////////////////////////
//
// [ShutdownContext]
//
////////////////////////////////////////////////////////////////////////////////
void
CParseGrammar::ShutdownContext(CParsingContext *pContext) {
    if (NULL == pContext) {
        return;
    }

    if ((pContext->m_pPendingCallbacks)
        && (pContext->m_pPendingCallbacks != pContext->m_DefaultPendingCallbacks)) {
        memFree(pContext->m_pPendingCallbacks);
    }
} // ShutdownContext






////////////////////////////////////////////////////////////////////////////////
//
// [ParseString]
//
////////////////////////////////////////////////////////////////////////////////
bool
CParseGrammar::ParseString(
                  const char *pText,
                  int32 textLength,
                  CParsingCallback *pCallback,
                  void *pContext) {
    return(ParseStringEx(pText, textLength, pCallback, pContext, NULL));
} // ParseString






////////////////////////////////////////////////////////////////////////////////
//
// [ParseStringEx]
//
////////////////////////////////////////////////////////////////////////////////
bool
CParseGrammar::ParseStringEx(
                  const char *pText,
                  int32 textLength,
                  CParsingCallback *pCallback,
                  void *pContext,
                  char **ppEndPtr) {
    ErrVal err = ENoErr;
    bool fMatch = false;
    CParsingContext context;
    const char *pEndText;

    InitializeContext(&context, pCallback, pContext);

    // If this is a different set of rules, or if we have not
    // been called with rules yet, then convert the list of rules
    // to a finite-state-machine.
    if (!m_InitializedRules) {
        err = InitializeRules();
        if (err) {
            gotoErr(err);
        }
    } // if (!m_InitializedRules)

    // Start at the beginning state.
    pEndText = (const char *) (((char *) pText) + textLength);
    context.m_pStateAtEndOfPreviousBuffer = m_StartVariable->m_pStartState;
    context.m_PathLength = 0;
    context.m_NumPendingCallbacks = 0;
    err = GoToNextStep(
                &context,
                m_StartVariable->m_pStartState,
                pText,
                pEndText,
                (const char **) &pText);
    if (err) {
        gotoErr(err);
    }

    // Be careful. Sometimes the first state will have some optional rules that
    // match the whole string. In that case, we are done.
    if (pText < pEndText) {
        fMatch = ParseSubString(
                        &context,
                        NULL,
                        pText,
                        pEndText,
                        (const char **) ppEndPtr);
    } else
    {
        fMatch = true;
    }
    if (fMatch) {
        FirePendingCallbacks(&context);
    }

abort:
    ShutdownContext(&context);

    return(fMatch);
} // ParseStringEx






////////////////////////////////////////////////////////////////////////////////
//
// [InitializeRules]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CParseGrammar::InitializeRules() {
    ErrVal err = ENoErr;
    CParseGrammar::CParseRule *pCurrentRule;

    // If this is a different set of rules, or if we have not
    // been called with rules yet, then convert the list of rules
    // to a finite-state-machine.
    if (!m_InitializedRules) {
        m_pStateList = NULL;

        pCurrentRule = m_pRuleList;
        while ((NULL != pCurrentRule) && (NULL != pCurrentRule->m_pPattern)) {
            err = AddRule(pCurrentRule);
            if (err) {
                gotoErr(err);
            }
            pCurrentRule++;
        }

        if (NULL == m_StartVariable) {
            gotoErr(EFail);
        }

        m_InitializedRules = true;
    } // if (!m_InitializedRules)

abort:
    returnErr(err);
} // InitializeRules








////////////////////////////////////////////////////////////////////////////////
//
// [GoToNextStep]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CParseGrammar::GoToNextStep(
                    CParsingContext *pContext,
                    CParseState *pNewState,
                    const char *pText,
                    const char *pEndText,
                    const char **ppResumeText) {
    ErrVal err = ENoErr;
    CPathStep *pStep;
    int32 index;
    int32 numOptionals;
    int32 savedCallPathLength;
    CStateTransition * optionalList[MAX_OPTIONALS_PER_RULE];
    bool fOneOptionalMatched = false;
    bool fSubMatch = false;
    CStateTransition *pOptionalTransition;


    if (NULL != ppResumeText) {
        *ppResumeText = pText;
    }
    if (pContext->m_PathLength >= pContext->m_MaxPathLength) {
        gotoErr(EFail);
    }

    pStep = &(pContext->m_pPath[pContext->m_PathLength]);
    pContext->m_PathLength += 1;

    pStep->m_Function = STATE_TRANSITION;
    if (NULL != pNewState) {
        pStep->m_pDebugString = pNewState->m_pDebugString;
    } else
    {
        pStep->m_pDebugString = NULL;
    }
    pStep->m_pStartText = pText;
    pStep->m_pStateAfterVariable = NULL;
    pStep->m_pCurrentState = pNewState;
    if (NULL != pNewState) {
        pStep->m_pCurrentTransition = pNewState->m_TransitionList;
    } else
    {
        pStep->m_pCurrentTransition = NULL;
    }
    pStep->m_NumPendingCallbacks = pContext->m_NumPendingCallbacks;

    if ((NULL == pNewState)
        || (NULL == pNewState->m_pOptionalList)) {
        gotoErr(ENoErr);
    }

    // Make a private list. We will remove non-Kleene optionals from this
    // list as they are matched.
    numOptionals = 0;
    pOptionalTransition = pNewState->m_pOptionalList;
    while ((NULL != pOptionalTransition) && (numOptionals < MAX_OPTIONALS_PER_RULE)) {
        optionalList[numOptionals] = pOptionalTransition;
        pOptionalTransition = pOptionalTransition->m_pNextOptionalTransition;
        numOptionals++;
    }

    // Keep looping while there are optionals and at least one matches.
    while ((numOptionals > 0) && (pText < pEndText)) {
        fSubMatch = false;

        // Try to match each optional.
        index = 0;
        while (index < numOptionals) {
            pOptionalTransition = optionalList[index];
            index++;

            // First, do a test run to see if there is a match.
            savedCallPathLength = pContext->m_PathLength;

            err = GoToNextStep(pContext, NULL, pText, pEndText, NULL);
            if (err) {
                gotoErr(err);
            }
            pStep = &(pContext->m_pPath[pContext->m_PathLength - 1]);
            pStep->m_Function = EXPANDING_OPTIONAL_PATTERN;
            pStep->m_pDebugString = pOptionalTransition->m_pDebugString;

            fOneOptionalMatched = ParseSubString(
                                        pContext,
                                        pOptionalTransition,
                                        pText,
                                        pEndText,
                                        &pText);

            pContext->m_PathLength = savedCallPathLength;
            if (fOneOptionalMatched) {
                fSubMatch = true;

                if (NULL != ppResumeText) {
                    *ppResumeText = pText;
                }

                // If this is not a kleene, then it can only match once.
                if (!(pOptionalTransition->m_fKleene)) {
                    // Go back to the optional we just matched.
                    index--;
                    if (index < (numOptionals - 1))
                    {
                        optionalList[index] = optionalList[numOptionals - 1];
                    }
                    numOptionals--;
                    index = 0;
                } // if (!(pOptionalTransition->m_fKleene))
            }
        } // while (index < numOptionals)

        // If no optionals matched in this entire iteration, then stop trying.
        if (!fSubMatch) {
            break;
        }
    } // while (numOptionals > 0)

abort:
    returnErr(err);
} // GoToNextStep








////////////////////////////////////////////////////////////////////////////////
//
// [FinishPathAfterMatch]
//
// This does a series of removals. Basically, we want to remove completed steps
// until we find a state that has a transition that we have not yet taken.
////////////////////////////////////////////////////////////////////////////////
ErrVal
CParseGrammar::FinishPathAfterMatch(
                        CParsingContext *pContext,
                        int32 firstStep,
                        const char *pText,
                        const char *pEndText,
                        const char **ppResumeText) {
    ErrVal err = ENoErr;
    CPathStep *pStep;
    CPathStep *pStartStep;
    int32 startStepIndex;
    CPendingCallback *pPendingCallback;
    const char *pStartText;

    if (NULL != ppResumeText) {
        *ppResumeText = pText;
    }


    while (pContext->m_PathLength >= firstStep) //<> This used to be >
    {
        pStep = &(pContext->m_pPath[pContext->m_PathLength - 1]);

        // If this was a match, and we just completed this state,
        // then fire the rule for the state we just completed.
        if ((NULL != pStep->m_pCurrentState)
            && (NULL != pStep->m_pCurrentState->m_pFinishRule)
            && (SILENT_RULE != pStep->m_pCurrentState->m_pFinishRule->m_RuleType)
            && (NULL != pContext->m_pCallback)) {
            // Grow the list of callbacks if we need to.
            if (pContext->m_NumPendingCallbacks >= pContext->m_MaxNumPendingCallbacks) {
                int32 newNumPendingCallbacks;
                CPendingCallback *pNewCallbackList = NULL;

                newNumPendingCallbacks = pContext->m_MaxNumPendingCallbacks * 2;

                // We cannot do a realloc, since the initial list may not be
                // allocated but instead from the built-in array.
                pNewCallbackList = (CPendingCallback *) memAlloc(newNumPendingCallbacks * sizeof(CPendingCallback));
                if (NULL == pNewCallbackList) {
                    gotoErr(EFail);
                }
                memcpy(
                    pNewCallbackList,
                    pContext->m_pPendingCallbacks,
                    pContext->m_NumPendingCallbacks * sizeof(CPendingCallback));
                if (pContext->m_pPendingCallbacks != pContext->m_DefaultPendingCallbacks) {
                    memFree(pContext->m_pPendingCallbacks);
                }
                pContext->m_pPendingCallbacks = pNewCallbackList;
                pContext->m_MaxNumPendingCallbacks = newNumPendingCallbacks;
            }

            pStartText = pStep->m_pStartText;
            for (startStepIndex = pContext->m_PathLength - 1;
                    startStepIndex >= 0;
                    startStepIndex--) {
                pStartStep = &(pContext->m_pPath[startStepIndex]);
                if (EXPANDING_VARIABLE == pStartStep->m_Function) {
                    pStartText = pStartStep->m_pStartText;
                    break;
                }
            }

            pPendingCallback = &(pContext->m_pPendingCallbacks[pContext->m_NumPendingCallbacks]);
            pContext->m_NumPendingCallbacks += 1;

            pPendingCallback->m_pRule = pStep->m_pCurrentState->m_pFinishRule;
            pPendingCallback->m_pStartText = pStartText;
            pPendingCallback->m_pStopText = pText;
        } // fire a rule

        // Since we had a match, we are done exploring the possibilities of
        // this state. If this is a variable, then we will take this transition.
        // If it is not a variable, then we are done with this state.
        if (EXPANDING_VARIABLE == pStep->m_Function) {
            pStep->m_Function = STATE_TRANSITION;
            err = GoToNextStep(
                        pContext,
                        pStep->m_pStateAfterVariable,
                        pText,
                        pEndText,
                        ppResumeText);
            if (err) {
                gotoErr(err);
            }
            break;
        }

        pContext->m_PathLength = pContext->m_PathLength - 1;
    } // while (pContext->m_PathLength > 0)

abort:
    returnErr(err);
} // FinishPathAfterMatch










////////////////////////////////////////////////////////////////////////////////
//
// [BacktrackFromMismatch]
//
// This is called after we get a mismatch. It does a series of backtracks, and
// tries to find the next state that has a possible different transition.
////////////////////////////////////////////////////////////////////////////////
void
CParseGrammar::BacktrackFromMismatch(
                        CParsingContext *pContext,
                        int32 firstStep) {
    CPathStep *pStep;

    // If we didn't get a match, then find the next state we can try.
    // Note, we mismatched, so we can close any open variable. We only
    // want to find the next state that has an alternative.
    while (pContext->m_PathLength >= firstStep) {
        pStep = &(pContext->m_pPath[pContext->m_PathLength - 1]);

        pContext->m_NumPendingCallbacks = pStep->m_NumPendingCallbacks;

        if (EXPANDING_OPTIONAL_PATTERN == pStep->m_Function) {
            break;
        }
        if ((NULL != pStep->m_pCurrentTransition)
            && (NULL != pStep->m_pCurrentTransition->m_pNextTransition)) {
            pStep->m_pCurrentTransition
                = pStep->m_pCurrentTransition->m_pNextTransition;
            break;
        }

        // No more possibilities at this level. Backtrack one step.
        pContext->m_PathLength = pContext->m_PathLength - 1;
    } // while (TRUE)
} // BacktrackFromMismatch








////////////////////////////////////////////////////////////////////////////////
//
// [ParseSubString]
//
// This is the main component of the pattern matching code.
// It basically does a depth-first-search through the space of states.
////////////////////////////////////////////////////////////////////////////////
bool
CParseGrammar::ParseSubString(
                        void *pVoidContext,
                        CStateTransition *pFirstTransition,
                        const char *pText,
                        const char *pEndText,
                        const char **ppEndPtr) {
    ErrVal err = ENoErr;
    bool fMatch = false;
    bool fSubMatch = false;
    CPathStep *pStep;
    CStateTransition *pTransition;
    CParsingContext *pContext = (CParsingContext *) pVoidContext;
    int32 firstStep;
    bool fFollowingTransitionFromState = false;

    if ((NULL == pContext)
        || (NULL == pText)
        || (NULL == pEndText)) {
        gotoErr(EFail);
    }

    if (NULL != ppEndPtr) {
        *ppEndPtr = (char *) pText;
    }

    // There are cases, like when the initial rule is just optional patterns,
    // that there is nothing to do once we start parsing the real pattern.
    // If this is the case, then do nothing.
    if (pContext->m_PathLength <= 0) {
        gotoErr(ENoErr);
    }

    // Now, do the actual parsing. Keep looping until we hit a final state.
    firstStep = pContext->m_PathLength;
    while (1) {
        pStep = &(pContext->m_pPath[pContext->m_PathLength - 1]);

        if (NULL != pFirstTransition) {
            pTransition = pFirstTransition;
            pFirstTransition = NULL;
            fFollowingTransitionFromState = false;
        } else { // if (NULL == pFirstTransition)
            // Get the next possible state transition.
            // If there are none, then we have to stop. This is either a
            // partial match, complete match, or a mismatch.
            if (NULL == pStep->m_pCurrentTransition) {
                // If the current state has no transitions at all, then this
                // is either a partial match or a complete match. Backtrack the path
                // until we either hit the first path step, or else hit a variable.
                // If we hit a variable, then this is just a partial match. If
                // we hit the top of the path, then this is a complete match.
                if ((NULL == pStep->m_pCurrentState)
                    || (NULL == pStep->m_pCurrentState->m_TransitionList)) {
                    err = FinishPathAfterMatch(
                                        pContext,
                                        firstStep,
                                        pText,
                                        pEndText,
                                        &pText);
                    if (err) {
                        gotoErr(err);
                    }
                } else {
                    BacktrackFromMismatch(pContext, firstStep);
                    if (pContext->m_PathLength >= firstStep) {
                        pStep = &(pContext->m_pPath[pContext->m_PathLength - 1]);
                        pText = pStep->m_pStartText;
                    }
                }

                // Check if this was a complete match.
                pStep = &(pContext->m_pPath[pContext->m_PathLength - 1]);
                if ((pContext->m_PathLength < firstStep) //<> This used to be <=
                    || (EXPANDING_OPTIONAL_PATTERN == pStep->m_Function)) {
                    if ((fMatch) && (NULL != ppEndPtr)) {
                        *ppEndPtr = (char *) pText;
                    }
                    goto abort;
                }

                continue;
            } // if (NULL == pStep->m_pCurrentState->m_TransitionList)

            // Otherwise, we have a possible transition from the current state.
            pTransition = pStep->m_pCurrentTransition;
            fFollowingTransitionFromState = true;
        } // if (NULL == pFirstTransition)

        // Check if this transition will match. Note, this may be a weak
        // attempt. The transition may appear to match, but it will later
        // lead to a mismatch in some future state.
        fSubMatch = MatchTransition(
                              pContext,
                              pTransition,
                              pText,
                              pEndText,
                              &pText);

        // If it didn't match, then try the next possible transition from
        // the current state.
        if (fSubMatch) {
            fMatch = true;
            if ((VARIABLE_PATTERN == pTransition->m_TransitionType)
                && (NULL != pTransition->m_pPatternVariable)
                && (NULL != pTransition->m_pPatternVariable->m_pStartState)) {
                err = GoToNextStep(
                            pContext,
                            pTransition->m_pPatternVariable->m_pStartState,
                            pText,
                            pEndText,
                            &pText);
                if (err) {
                    gotoErr(err);
                }
                pStep = &(pContext->m_pPath[pContext->m_PathLength - 1]);
                pStep->m_Function = EXPANDING_VARIABLE;
                pStep->m_pStateAfterVariable = pTransition->m_pNextState;
                pStep->m_pDebugString = pTransition->m_pDebugString;
            } else {
                // Follow the transition.
                err = GoToNextStep(
                            pContext,
                            pTransition->m_pNextState,
                            pText,
                            pEndText,
                            &pText);
                if (err) {
                    gotoErr(err);
                }
            }
        } else { // if (!fSubMatch)
            fMatch = false;
            if (fFollowingTransitionFromState) {
                pStep->m_pCurrentTransition = pTransition->m_pNextTransition;
            }
            else {
                break;
            }
        }
    } // while (TRUE)

abort:
    if ((fMatch) && (NULL != ppEndPtr)) {
        *ppEndPtr = (char *) pText;
    }

    return(fMatch);
} // ParseSubString








////////////////////////////////////////////////////////////////////////////////
//
// [MatchTransition]
//
////////////////////////////////////////////////////////////////////////////////
bool
CParseGrammar::MatchTransition(
                        CParsingContext *pContext,
                        CStateTransition *pTransition,
                        const char *pText,
                        const char *pEndText,
                        const char **ppNewText) {
    bool fTotalMatch = false;
    bool fSingleMatch = false;
    int32 bytesInChar;
    int32 compareResult;
    int32 charNum;


    ////////////////////////////////////////////////////////////////
    if ((CHAR_PATTERN == pTransition->m_TransitionType)
            && (pText < pEndText)) {
        // If a transition is a Kleene, then it can match as many times
        // as possible. In this case, keep iterating until it no longer matches.
        // Otherwise, it may match at most once.
        // Each iteration matches any one alternative once.
        while (pText < pEndText) {
            bytesInChar = 1;
            fSingleMatch = false;

            // It may match one of a list of 1 or more specific characters.
            for (charNum = 0; charNum < pTransition->m_NumPatternChars; charNum++) {
                compareResult = CStringLib::UnicodeStrcmp(
                                                pText,
                                                1,
                                                &(pTransition->m_pPatternCharList[charNum]),
                                                1,
                                                CStringLib::IGNORE_CASE);
                if (0 == compareResult) {
                    fSingleMatch = true;
                    break;
                } // if (0 == compareResult)
            } // for (charNum = 0; charNum < pTransition->m_NumPatternChars; charNum++)

            // Alternatively, it may have a general character type.
            // For example, you can use this to specify "all whitespace characters",
            // or "all numbers".
            if ((!fSingleMatch)
                && (0 != pTransition->m_PatternCharType)
                && (CStringLib::IsByte(*pText, pTransition->m_PatternCharType))) {
                fSingleMatch = true;
            }

            // Stop looking when there is a mismatch.
            // Note, that if this is a Kleene, then fTotalMatch may
            // still be true.
            if (!fSingleMatch) {
                break;
            }

            // Otherwise, we got a match. We may or may not try to get more matches.
            fTotalMatch = true;
            pText = pText + bytesInChar;
            if (!(pTransition->m_fKleene)) {
                break;
            }
        } // while (pText < pEndText)
    } // (CHAR_PATTERN == pTransition->m_TransitionType)
    ////////////////////////////////////////////////////////////////
    else if ((VARIABLE_PATTERN == pTransition->m_TransitionType)
        && (NULL != pTransition->m_pPatternVariable)
        && (NULL != pTransition->m_pPatternVariable->m_pStartState)
        && (pContext->m_PathLength < pContext->m_MaxPathLength)) {
        fTotalMatch = true;
    }

    if ((fTotalMatch) && (NULL != ppNewText)) {
       *ppNewText = pText;
    }

    return(fTotalMatch);
} // MatchTransition









////////////////////////////////////////////////////////////////////////////////
//
// [FirePendingCallbacks]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CParseGrammar::FirePendingCallbacks(CParsingContext *pContext) {
    ErrVal err = ENoErr;
    CParseRule *pRule;
    const char *pStartText;
    const char *pStopText;
    int32 numberValue;
    int textLength;
    CPendingCallback *pPendingCallback;
    int callbackNum;

    if (NULL == pContext->m_pCallback) {
        goto abort;
    }

    for (callbackNum = 0; callbackNum < pContext->m_NumPendingCallbacks; callbackNum++) {
        pPendingCallback = &(pContext->m_pPendingCallbacks[callbackNum]);
        pRule = pPendingCallback->m_pRule;
        pStartText = pPendingCallback->m_pStartText;
        pStopText = pPendingCallback->m_pStopText;

        textLength = (int32) (((char *) pStopText) - (char *) (pStartText));
        switch (pRule->m_RuleType) {
        case SILENT_RULE:
            break;

        case TOKEN_RULE:
            pContext->m_pCallback->OnParseToken(
                                        pContext->m_pCallbackContext,
                                        pRule->m_TokenId,
                                        pRule->m_TokenValue,
                                        pStartText,
                                        textLength);
            break;

        case INTEGER_RULE:
            err = CStringLib::StringToNumber((const char *) pStartText, textLength, &numberValue);
            if (err) {
                gotoErr(err);
            }

            pContext->m_pCallback->OnParseToken(
                                        pContext->m_pCallbackContext,
                                        pRule->m_TokenId,
                                        numberValue,
                                        (const char *) pStartText,
                                        textLength);
            break;

        default:
            break;
        }
    }

    pContext->m_NumPendingCallbacks = 0;

abort:
    returnErr(err);
} // FirePendingCallbacks






////////////////////////////////////////////////////////////////////////////////
//
// [AddRule]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CParseGrammar::AddRule(CParseGrammar::CParseRule *pRule) {
    ErrVal err = ENoErr;
    CParseState *pState = NULL;
    CParserVariable *pVariable;


    if (NULL == pRule) {
        gotoErr(EFail);
    }

    // Look for the state that this rule starts from.
    pVariable = AddVariable(pRule->m_pVariableName, NULL);
    if (NULL == pVariable) {
        gotoErr(EFail);
    }

    // If there is no state already defined, then create one.
    pState = pVariable->m_pStartState;
    if (NULL == pState) {
        pState = newex CParseState;
        if (NULL == pState) {
            gotoErr(EFail);
        }
        g_MainMem.DontCountMemoryAsLeaked((char *) pState);

        pState->m_TransitionList = NULL;
        pState->m_pOptionalList = NULL;
        pState->m_pFinishRule = NULL;
        pState->m_pDebugString = NULL;
        pState->m_pNextStateInGrammar = m_pStateList;
        m_pStateList = pState;

        pVariable->m_pStartState = pState;
    } // if (NULL == pState)

    // Now, expand the string into states.
    err = ParseRuleExpansion(pRule->m_pPattern, pState, &pState);
    if (err) {
       gotoErr(err);
    }

    if (NULL != pState) {
        ASSERT(NULL == pState->m_pFinishRule);
        pState->m_pFinishRule = pRule;
    }

abort:
    returnErr(err);
} // AddRule







////////////////////////////////////////////////////////////////////////////////
//
// [ParseRuleExpansion]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CParseGrammar::ParseRuleExpansion(
                              const char *pString,
                              CParseState *pState,
                              CParseState **ppFinishState) {
    ErrVal err = ENoErr;
    CStateTransition *pTransition = NULL;
    CStateTransition *pOptionalPattern = NULL;
    CParseState *pNewState = NULL;
    CParserVariable *pVariable = NULL;
    TransitionType transitionType;
    const char *pSaveString = NULL;
    int32 compareResult;
    int32 matchCharFlags;
    char matchCharList[256];
    bool fKleene;
    int32 numChars;
    int32 charNum;


    // Now, expand the string into states.
    while ((pState) && (*pString)) {
        // Reset the description of a transition before defining the
        // next transition. Every new transition may not override everything.
        matchCharFlags = 0;
        numChars = 0;
        fKleene = false;

        pSaveString = pString;
        if (NULL == pState->m_pDebugString) {
            pState->m_pDebugString = pString;
        }

        /////////////////////////////////////////
        if (g_OpenVariableNameChar == *pString) {
            pVariable = AddVariable(pString, &pString);
            if (NULL == pVariable) {
                gotoErr(EFail);
            }

            transitionType = VARIABLE_PATTERN;
        } // if (g_OpenVariableNameChar == *pString)
        /////////////////////////////////////////
        else if (' ' == *pString) {
            pOptionalPattern = ParseWhitespacePattern(pString, &pString);
            if (NULL == pOptionalPattern) {
                gotoErr(EFail);
            }

            // These hang off the current state, they are not transitions to a
            // new and different state.
            pOptionalPattern->m_pNextOptionalTransition = pState->m_pOptionalList;
            pState->m_pOptionalList = pOptionalPattern;
            continue;
        }
        /////////////////////////////////////////
        else if ((g_OpenAlternativeChar == *pString)
                || ((g_KleeneStarChar == pString[0])
                    && (g_OpenAlternativeChar == pString[1]))) {
            err = ParseAlternativeList(
                            pString,
                            &matchCharFlags,
                            matchCharList,
                            &numChars,
                            &fKleene,
                            &pString);
            if (err) {
                gotoErr(err);
            }
            transitionType = CHAR_PATTERN;
        }
        /////////////////////////////////////////
        else if ((g_OpenOptionalChar == *pString)
                || ((g_KleeneStarChar == pString[0])
                    && (g_OpenOptionalChar == pString[1]))) {
            pOptionalPattern = ParseOptionalPattern(pString, &pString);
            if (NULL == pOptionalPattern) {
                gotoErr(EFail);
            }

            // These hang off the current state, they are not transitions to a
            // new and different state.
            pOptionalPattern->m_pNextOptionalTransition = pState->m_pOptionalList;
            pState->m_pOptionalList = pOptionalPattern;
            continue;
        }
        /////////////////////////////////////////
        else {
            matchCharList[0] = GetPatternChar(&pString);
            numChars = 1;
            transitionType = CHAR_PATTERN;
        }

        // Check if a transition for this character already exists. If
        // so, then we just share the next state with this new string.
        pTransition = pState->m_TransitionList;
        while (pTransition) {
            if (transitionType == pTransition->m_TransitionType) {
                if ((CHAR_PATTERN == pTransition->m_TransitionType)
                    && (pTransition->m_NumPatternChars > 0)
                    && (1 == numChars)
                    && (0 == matchCharFlags)) {
                    for (charNum = 0; charNum < pTransition->m_NumPatternChars; charNum++)
                    {
                        compareResult = CStringLib::UnicodeStrcmp(
                                                        &(matchCharList[0]),
                                                        1,
                                                        &(pTransition->m_pPatternCharList[charNum]),
                                                        1,
                                                        m_Options);
                        if (compareResult == 0)
                        {
                            break;
                        }
                    } // for (charNum = 0; charNum < pTransition->m_NumPatternChars; charNum++)
                } // if (CHAR_PATTERN == pTransition->m_TransitionType)
                else if ((VARIABLE_PATTERN == pTransition->m_TransitionType)
                        && (pVariable == pTransition->m_pPatternVariable)) {
                    break;
                } // if (VARIABLE_PATTERN == pTransition->m_TransitionType)
            } // if (transitionType == pTransition->m_TransitionType)

            pTransition = pTransition->m_pNextTransition;
        } // while (pTransition)

        // If there is no transition, then make one. We will also have to make a new state.
        if (NULL == pTransition) {
            pTransition = newex CStateTransition;
            if (NULL == pTransition) {
                gotoErr(EFail);
            }
            g_MainMem.DontCountMemoryAsLeaked((char *) pTransition);

            pTransition->m_fKleene = fKleene;
            pTransition->m_pNextOptionalTransition = NULL;
            pTransition->m_pDebugString = pSaveString;
            pTransition->m_TransitionType = transitionType;
            pTransition->m_PatternCharType = matchCharFlags;
            pTransition->m_NumPatternChars = numChars;
            if (CHAR_PATTERN == transitionType) {
                pTransition->m_NumPatternChars = numChars;
                pTransition->m_pPatternCharList = new char[numChars];
                if (NULL == pTransition->m_pPatternCharList) {
                    gotoErr(EFail);
                }
                for (charNum = 0; charNum < numChars; charNum++) {
                    pTransition->m_pPatternCharList[charNum] = matchCharList[charNum];
                }
                pTransition->m_PatternCharType = matchCharFlags;
                pTransition->m_pPatternVariable = NULL;
            }
            else if (VARIABLE_PATTERN == pTransition->m_TransitionType) {
                pTransition->m_pPatternVariable = pVariable;
            }

            pNewState = newex CParseState;
            if (NULL == pNewState) {
                gotoErr(EFail);
            }
            g_MainMem.DontCountMemoryAsLeaked((char *) pNewState);

            pNewState->m_pFinishRule = NULL;
            pNewState->m_TransitionList = NULL;
            pNewState->m_pOptionalList = NULL;
            pNewState->m_pDebugString = NULL;
            pNewState->m_pNextStateInGrammar = m_pStateList;
            m_pStateList = pNewState;

            // This is a transition from the old state to the new state.
            pTransition->m_pNextState = pNewState;
            pTransition->m_pNextTransition = pState->m_TransitionList;
            pState->m_TransitionList = pTransition;
        } // if (NULL == pTransition)

        pState = pTransition->m_pNextState;
    } // while ((pState) && (*pString))


    if (NULL != ppFinishState) {
        *ppFinishState = pState;
    }

abort:
    returnErr(err);
} // ParseRuleExpansion







////////////////////////////////////////////////////////////////////////////////
//
// [AddVariable]
//
////////////////////////////////////////////////////////////////////////////////
CParseGrammar::CParserVariable *
CParseGrammar::AddVariable(
                    const char *pStartName,
                    const char **pResultStopName) {
    ErrVal err = ENoErr;
    CParserVariable *pVariable;
    bool fStartVariable = false;
    const char *pStopName;
    int nameLength = 0;

    // Look for the state that this rule starts from.
    if ((NULL == pStartName) || (0 == *pStartName)) {
        fStartVariable = true;
        pVariable = m_StartVariable;
    } else
    {
        fStartVariable = false;
        while (g_OpenVariableNameChar == *pStartName) {
            pStartName++;
        }
        pStopName = pStartName;
        while ((*pStopName) && (g_CloseVariableNameChar != *pStopName)) {
            pStopName++;
        }
        nameLength = pStopName - pStartName;
        if (NULL != pResultStopName) {
            *pResultStopName = pStopName;
            if (g_CloseVariableNameChar == *pStopName) {
                *pResultStopName += 1;
            }
        }

        pVariable = m_pVariableList;
        while (NULL != pVariable) {
            if ((NULL != pVariable->m_pName)
                    && (nameLength == pVariable->m_NameLength)
                    && (0 == strncasecmpex(pStartName, pVariable->m_pName, nameLength))) {
                break;
            }
            pVariable = pVariable->m_pNextVariable;
        } // while (NULL != pVariable)
    }

    // If we don't have a variable for this rule yet, then create one.
    if (NULL == pVariable) {
        pVariable = newex CParserVariable;
        if (NULL == pVariable) {
            gotoErr(EFail);
        }
        g_MainMem.DontCountMemoryAsLeaked((char *) pVariable);

        pVariable->m_pStartState = NULL;
        if (fStartVariable) {
            pVariable->m_pName = NULL;
            pVariable->m_NameLength = -1;
            pVariable->m_pNextVariable = NULL;
            m_StartVariable = pVariable;
        } else {
            pVariable->m_pName = strndupex(pStartName, nameLength);
            if (NULL == pVariable->m_pName) {
                gotoErr(EFail);
            }
            g_MainMem.DontCountMemoryAsLeaked((char *) pVariable->m_pName);
            pVariable->m_NameLength = nameLength;

            pVariable->m_pNextVariable = m_pVariableList;
            m_pVariableList = pVariable;
        }
    }

abort:
    return(pVariable);
} // AddVariable







////////////////////////////////////////////////////////////////////////////////
//
// [ParseAlternativeList]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CParseGrammar::ParseAlternativeList(
                    const char *pStartAlternativeList,
                    int32 *pMatchFlags,
                    char *pCharList,
                    int32 *pNumChars,
                    bool *pfKleene,
                    const char **ppResultStopAlternative) {
    ErrVal err = ENoErr;
    const char *pStartAlternate;
    const char *pStopAlternate;
    CCharTypeName *pPattern;
    int32 alternateLength;
    int32 numMatchChars = 0;


    *pMatchFlags = 0;
    *pfKleene = false;
    *pNumChars = 0;

    if (g_KleeneStarChar == *pStartAlternativeList) {
        *pfKleene = true;
        pStartAlternativeList++;
    }
    if (g_OpenAlternativeChar == *pStartAlternativeList) {
        pStartAlternativeList++;
    }

    // Now, read through every alternate character.
    // Each iteration reads one alternate.
    pStartAlternate = pStartAlternativeList;
    while ((*pStartAlternate)
            && (g_CloseAlternativeChar != *pStartAlternate)) {
        // Skip whitespace and separators.
        while ((g_AlternativeSeparatorChar == *pStartAlternate)
                || (' ' == *pStartAlternate)
                || ('\n' == *pStartAlternate)
                || ('\r' == *pStartAlternate)
                || ('\t' == *pStartAlternate)) {
            pStartAlternate += 1;
        }

        // Find the end of this alternate.
        pStopAlternate = pStartAlternate;
        while ((g_AlternativeSeparatorChar != *pStopAlternate)
                && (g_CloseAlternativeChar != *pStopAlternate)
                && ('\r' != *pStopAlternate)
                && ('\n' != *pStopAlternate)
                && ('\t' != *pStopAlternate)
                && (' ' != *pStopAlternate)) {
            if ('\\' == *pStopAlternate) {
                pStopAlternate++;
            }
            pStopAlternate += 1;
        }
        alternateLength = pStopAlternate - pStartAlternate;
        if (alternateLength <= 0) {
            break;
        }


        // Check if this matches any fixed pattern.
        pPattern = g_AlternativePatterns;
        while (NULL != pPattern->m_pPattern) {
            // Initialize it the first time we use it.
            if (-1 == pPattern->m_PatternLength) {
                pPattern->m_PatternLength = strlen(pPattern->m_pPattern);
            }

            if ((alternateLength == pPattern->m_PatternLength)
                && (0 == strncasecmpex(pStartAlternate, pPattern->m_pPattern, alternateLength))) {
                break;
            }

            pPattern++;
        } // while (NULL != pPattern->m_pPattern)

        // Initialize this alternate
        if ((NULL != pPattern) && (NULL != pPattern->m_pPattern)) {
            *pMatchFlags |= pPattern->m_Flags;
        } else {
            pCharList[numMatchChars] = GetPatternChar(&pStartAlternate);
            numMatchChars += 1;
        }

        // Resume parsing on the next alternate.
        pStartAlternate = pStopAlternate;
    } // while (pStartAlternate < pStopPtr)


    if (NULL != ppResultStopAlternative) {
        if (g_CloseAlternativeChar == *pStartAlternate) {
            pStartAlternate++;
        }
        *ppResultStopAlternative = pStartAlternate;
    }

    pCharList[numMatchChars] = 0;
    *pNumChars = numMatchChars;

    return(err);
} // ParseAlternativeList









////////////////////////////////////////////////////////////////////////////////
//
// [ParseOptionalPattern]
//
////////////////////////////////////////////////////////////////////////////////
CParseGrammar::CStateTransition *
CParseGrammar::ParseOptionalPattern(
                    const char *pStartOptional,
                    const char **ppResultStopOptional) {
    ErrVal err = ENoErr;
    CStateTransition *pTransition = NULL;
    const char *pStopOptional;
    bool fKleeneStar = false;
    CParseState *pFinishState = NULL;
    CParseState startState;
    char *pOptionPattern = NULL;
    int nesting;
    int optionalLength;

    if (g_KleeneStarChar == *pStartOptional) {
        fKleeneStar = true;
        pStartOptional++;
        while (' ' == *pStartOptional) {
            pStartOptional++;
        }
    }
    if (g_OpenOptionalChar == *pStartOptional) {
        pStartOptional++;
    }

    // Find the end of this optional. Be careful, since
    // several optionals may be nested.
    pStopOptional = pStartOptional;
    nesting = 1;
    while ((*pStopOptional) && (nesting > 0)) {
        if ('\\' == *pStopOptional) {
            pStopOptional++;
        } else if (g_CloseOptionalChar == *pStopOptional) {
            nesting -= 1;
        } else if (g_OpenOptionalChar == *pStopOptional) {
            nesting += 1;
        }

        pStopOptional += 1;
    } // while (g_CloseOptionalChar != *pStopOptional)


    // Make this substring a NULL-terminated string. Resume
    // parsing just after it finishes.
    optionalLength = pStopOptional - pStartOptional;
    if ((optionalLength > 1)
        && (g_CloseOptionalChar == pStartOptional[optionalLength - 1])) {
        optionalLength--;
    }
    pOptionPattern = strndupex(pStartOptional, optionalLength);
    if (NULL == pOptionPattern) {
        gotoErr(EFail);
    }    
    g_MainMem.DontCountMemoryAsLeaked((char *) pOptionPattern);

    // Parse this optional pattern, and hang its transitions off a temporary
    // state.
    startState.m_pFinishRule = NULL;
    startState.m_TransitionList = NULL;
    startState.m_pOptionalList = NULL;
    err = ParseRuleExpansion(pOptionPattern, &startState, &pFinishState);
    if (err) {
        gotoErr(err);
    }

    // Now, put the transitions into the optional object.
    pTransition = startState.m_TransitionList;
    pTransition->m_fKleene = fKleeneStar;
    ASSERT(NULL == startState.m_pFinishRule);

    if (NULL != ppResultStopOptional) {
        *ppResultStopOptional = pStopOptional;
    }

abort:
    // Leak pOptionPattern. The parsing code assumes we can point
    // to variable names in the grammar.

    if (err) {
        delete pTransition;
        pTransition = NULL;
    }

    return(pTransition);
} // ParseOptionalPattern








////////////////////////////////////////////////////////////////////////////////
//
// [ParseWhitespacePattern]
//
////////////////////////////////////////////////////////////////////////////////
CParseGrammar::CStateTransition *
CParseGrammar::ParseWhitespacePattern(
                    const char *pStartOptional,
                    const char **ppResultStopOptional) {
    ErrVal err = ENoErr;
    CStateTransition *pTransition = NULL;
    CParseState *pNewState = NULL;

    while (CStringLib::IsByte(*pStartOptional, CStringLib::WHITE_SPACE_CHAR)) {
        pStartOptional++;
    }
    if (NULL != ppResultStopOptional) {
        *ppResultStopOptional = pStartOptional;
    }


    pTransition = newex CStateTransition;
    if (NULL == pTransition) {
        gotoErr(EFail);
    }
    g_MainMem.DontCountMemoryAsLeaked((char *) pTransition);

    pTransition->m_fKleene = true;
    pTransition->m_pNextOptionalTransition = NULL;
    pTransition->m_pDebugString = "SPECIAL Kleene for a space";
    pTransition->m_TransitionType = CHAR_PATTERN;
    pTransition->m_PatternCharType = CStringLib::WHITE_SPACE_CHAR;
    pTransition->m_NumPatternChars = 0;
    pTransition->m_pPatternVariable = NULL;
    pTransition->m_pNextTransition = NULL;

    pNewState = newex CParseState;
    if (NULL == pNewState) {
        gotoErr(EFail);
    }
    g_MainMem.DontCountMemoryAsLeaked((char *) pNewState);

    pNewState->m_pFinishRule = NULL;
    pNewState->m_TransitionList = NULL;
    pNewState->m_pOptionalList = NULL;
    pNewState->m_pDebugString = "Finish state for special Kleene for a space";
    pNewState->m_pNextStateInGrammar = m_pStateList;
    m_pStateList = pNewState;

    // This is a transition from the old state to the new state.
    pTransition->m_pNextState = pNewState;

abort:
    if (err) {
        delete pTransition;
        pTransition = NULL;
    }

    return(pTransition);
} // ParseWhitespacePattern









////////////////////////////////////////////////////////////////////////////////
//
// [GetPatternChar]
//
////////////////////////////////////////////////////////////////////////////////
char
CParseGrammar::GetPatternChar(const char **pPattern) {
    char c;
    const char *pStr;

    pStr = *pPattern;
    c = *pStr;
    pStr += 1;

    if ('\\' == c) {
        c = *pStr;
        pStr += 1;

        if ('n' == c) {
            c = '\n';
        } else if ('r' == c) {
            c = '\r';
        } else if ('t' == c) {
            c = '\t';
        }
    }

    *pPattern = pStr;

    return(c);
} // GetPatternChar







////////////////////////////////////////////////////////////////////////////////
//
// [OnParseToken]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CReentrantParsingCallback::OnParseToken(
                                void *pCallbackContext,
                                int32 tokenId,
                                int32 intValue,
                                const char *pStr,
                                int32 length) {
    pCallbackContext = pCallbackContext; // Unused
    pStr = pStr; // Unused.
    length = length; // Unused.

    m_TokenId = tokenId;
    m_IntValue = intValue;
    return(ENoErr);
}; // OnParseToken






