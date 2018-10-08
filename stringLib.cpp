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
// String Library Module
//
// This module defines some utility functions, like reading the contents
// of a file into a string.
//
// All strings in the library are UTF-8.
/////////////////////////////////////////////////////////////////////////////

#include "osIndependantLayer.h"
#include "stringLib.h"

#if WIN32
#include <mbstring.h>
#endif


/////////////////////////////////////////////////////////////////////////////
//
// Static Tables
//
// These define the static tables that describe properties of different byte
// values, and different unicode characters. These are declared in stringLib.cpp.
/////////////////////////////////////////////////////////////////////////////

// We special-case ASCII to make some of the libc functions work.
// The global tables from stringUtilsASCII.cpp
class CByteInfo {
public:
    int             m_IntVal;
    unsigned char   m_CharVal;
    unsigned char   m_LowerCaseASCIICharVal;
    unsigned char   m_UpperCaseASCIICharVal;
    int             m_Flags;
};

static int NUMBER_FLAGS = (CStringLib::WORD_CHAR
                           | CStringLib::ALPHANUM_CHAR
                           | CStringLib::NUMBER_CHAR
                           | CStringLib::URL_PATH_CHAR
                           | CStringLib::URL_HOST_CHAR
                           | CStringLib::URL_FRAGMENT_CHAR
                           | CStringLib::URL_QUERY_CHAR
                           | CStringLib::ASCII_CHAR);

static int UPPER_CASE_CHAR_FLAGS = (CStringLib::WORD_CHAR
                                    | CStringLib::ALPHANUM_CHAR
                                    | CStringLib::URL_PATH_CHAR
                                    | CStringLib::URL_HOST_CHAR
                                    | CStringLib::URL_FRAGMENT_CHAR
                                    | CStringLib::URL_QUERY_CHAR
                                    | CStringLib::ASCII_CHAR);

static int LOWER_CASE_CHAR_FLAGS = (CStringLib::WORD_CHAR
                                    | CStringLib::ALPHANUM_CHAR
                                    | CStringLib::URL_PATH_CHAR
                                    | CStringLib::URL_HOST_CHAR
                                    | CStringLib::URL_FRAGMENT_CHAR
                                    | CStringLib::URL_QUERY_CHAR
                                    | CStringLib::ASCII_CHAR);

#define CHAR(x) (unsigned char) x



CByteInfo g_ByteInfoList[] = {
      { 0x0, CHAR(0x0), CHAR(0x0), CHAR(0x0), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x1, CHAR(0x1), CHAR(0x1), CHAR(0x1), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x2, CHAR(0x2), CHAR(0x2), CHAR(0x2), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x3, CHAR(0x3), CHAR(0x3), CHAR(0x3), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x4, CHAR(0x4), CHAR(0x4), CHAR(0x4), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x5, CHAR(0x5), CHAR(0x5), CHAR(0x5), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x6, CHAR(0x6), CHAR(0x6), CHAR(0x6), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x7, CHAR(0x7), CHAR(0x7), CHAR(0x7), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x8, CHAR(0x8), CHAR(0x8), CHAR(0x8), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x9, CHAR('\t'), CHAR('\t'), CHAR('\t'), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR | CStringLib::WHITE_SPACE_CHAR | CStringLib::NON_NEWLINE_WHITE_SPACE_CHAR },
      { 0xA, CHAR('\n'), CHAR('\n'), CHAR('\n'), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR | CStringLib::NEWLINE_CHAR | CStringLib::WHITE_SPACE_CHAR },
      { 0xB, CHAR(0xB), CHAR(0xB), CHAR(0xB), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0xC, CHAR(0xC), CHAR(0xC), CHAR(0xC), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0xD, CHAR('\r'), CHAR('\r'), CHAR('\r'), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR | CStringLib::NEWLINE_CHAR | CStringLib::WHITE_SPACE_CHAR },
      { 0xE, CHAR(0xE), CHAR(0xE), CHAR(0xE), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0xF, CHAR(0xF), CHAR(0xF), CHAR(0xF), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x10, CHAR(0x10), CHAR(0x10), CHAR(0x10), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x11, CHAR(0x11), CHAR(0x11), CHAR(0x11), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x12, CHAR(0x12), CHAR(0x12), CHAR(0x12), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x13, CHAR(0x13), CHAR(0x13), CHAR(0x13), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x14, CHAR(0x14), CHAR(0x14), CHAR(0x14), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x15, CHAR(0x15), CHAR(0x15), CHAR(0x15), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x16, CHAR(0x16), CHAR(0x16), CHAR(0x16), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x17, CHAR(0x17), CHAR(0x17), CHAR(0x17), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x18, CHAR(0x18), CHAR(0x18), CHAR(0x18), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x19, CHAR(0x19), CHAR(0x19), CHAR(0x19), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x1A, CHAR(0x1A), CHAR(0x1A), CHAR(0x1A), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x1B, CHAR(0x1B), CHAR(0x1B), CHAR(0x1B), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x1C, CHAR(0x1C), CHAR(0x1C), CHAR(0x1C), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x1D, CHAR(0x1D), CHAR(0x1D), CHAR(0x1D), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x1E, CHAR(0x1E), CHAR(0x1E), CHAR(0x1E), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x1F, CHAR(0x1F), CHAR(0x1F), CHAR(0x1F), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x20, CHAR(0x20), CHAR(0x20), CHAR(0x20), CStringLib::WHITE_SPACE_CHAR | CStringLib::ASCII_CHAR | CStringLib::NON_NEWLINE_WHITE_SPACE_CHAR | CStringLib::URL_ENCODABLE_CHAR },
      { 0x21, CHAR('!'), CHAR('!'), CHAR('!'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR | CStringLib::URL_HOST_CHAR},
      { 0x22, CHAR('\"'), CHAR('\"'), CHAR('\"'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR | CStringLib::URL_ENCODABLE_CHAR },
      { 0x23, CHAR('#'), CHAR('#'), CHAR('#'), CStringLib::URL_ENCODABLE_CHAR | CStringLib::ASCII_CHAR },
      { 0x24, CHAR('$'), CHAR('$'), CHAR('$'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR },
      { 0x25, CHAR('%'), CHAR('%'), CHAR('%'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR | CStringLib::URL_ENCODABLE_CHAR },
      { 0x26, CHAR('&'), CHAR('&'), CHAR('&'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR },
      { 0x27, CHAR('\''), CHAR('\''), CHAR('\''), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR | CStringLib::WORD_CHAR | CStringLib::URL_ENCODABLE_CHAR },
      { 0x28, CHAR('('), CHAR('('), CHAR('('), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR },
      { 0x29, CHAR(')'), CHAR(')'), CHAR(')'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR },
      { 0x2A, CHAR('*'), CHAR('*'), CHAR('*'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR },
      { 0x2B, CHAR('+'), CHAR('+'), CHAR('+'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR | CStringLib::URL_ENCODABLE_CHAR },
      { 0x2C, CHAR(','), CHAR(','), CHAR(','), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR },
      { 0x2D, CHAR('-'), CHAR('-'), CHAR('-'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR | CStringLib::URL_HOST_CHAR },
      { 0x2E, CHAR('.'), CHAR('.'), CHAR('.'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR | CStringLib::URL_HOST_CHAR },
      { 0x2F, CHAR('/'), CHAR('/'), CHAR('/'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR },
      { 0x30, CHAR('0'), CHAR('0'), CHAR('0'), NUMBER_FLAGS | CStringLib::HEX_CHAR },
      { 0x31, CHAR('1'), CHAR('1'), CHAR('1'), NUMBER_FLAGS | CStringLib::HEX_CHAR },
      { 0x32, CHAR('2'), CHAR('2'), CHAR('2'), NUMBER_FLAGS | CStringLib::HEX_CHAR },
      { 0x33, CHAR('3'), CHAR('3'), CHAR('3'), NUMBER_FLAGS | CStringLib::HEX_CHAR },
      { 0x34, CHAR('4'), CHAR('4'), CHAR('4'), NUMBER_FLAGS | CStringLib::HEX_CHAR },
      { 0x35, CHAR('5'), CHAR('5'), CHAR('5'), NUMBER_FLAGS | CStringLib::HEX_CHAR },
      { 0x36, CHAR('6'), CHAR('6'), CHAR('6'), NUMBER_FLAGS | CStringLib::HEX_CHAR },
      { 0x37, CHAR('7'), CHAR('7'), CHAR('7'), NUMBER_FLAGS | CStringLib::HEX_CHAR },
      { 0x38, CHAR('8'), CHAR('8'), CHAR('8'), NUMBER_FLAGS | CStringLib::HEX_CHAR },
      { 0x39, CHAR('9'), CHAR('9'), CHAR('9'), NUMBER_FLAGS | CStringLib::HEX_CHAR },
      { 0x3A, CHAR(':'), CHAR(':'), CHAR(':'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR },
      { 0x3B, CHAR(';'), CHAR(';'), CHAR(';'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR },
      { 0x3C, CHAR('<'), CHAR('<'), CHAR('<'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR | CStringLib::URL_ENCODABLE_CHAR },
      { 0x3D, CHAR('='), CHAR('='), CHAR('='), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR },
      { 0x3E, CHAR('>'), CHAR('>'), CHAR('>'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR | CStringLib::URL_ENCODABLE_CHAR },
      { 0x3F, CHAR('?'), CHAR('?'), CHAR('?'), CStringLib::ASCII_CHAR },
      { 0x40, CHAR('@'), CHAR('@'), CHAR('@'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR },
      { 0x41, CHAR('A'), CHAR('a'), CHAR('A'), UPPER_CASE_CHAR_FLAGS | CStringLib::HEX_CHAR },
      { 0x42, CHAR('B'), CHAR('b'), CHAR('B'), UPPER_CASE_CHAR_FLAGS | CStringLib::HEX_CHAR },
      { 0x43, CHAR('C'), CHAR('c'), CHAR('C'), UPPER_CASE_CHAR_FLAGS | CStringLib::HEX_CHAR },
      { 0x44, CHAR('D'), CHAR('d'), CHAR('D'), UPPER_CASE_CHAR_FLAGS | CStringLib::HEX_CHAR },
      { 0x45, CHAR('E'), CHAR('e'), CHAR('E'), UPPER_CASE_CHAR_FLAGS | CStringLib::HEX_CHAR },
      { 0x46, CHAR('F'), CHAR('f'), CHAR('F'), UPPER_CASE_CHAR_FLAGS | CStringLib::HEX_CHAR },
      { 0x47, CHAR('G'), CHAR('g'), CHAR('G'), UPPER_CASE_CHAR_FLAGS },
      { 0x48, CHAR('H'), CHAR('h'), CHAR('H'), UPPER_CASE_CHAR_FLAGS },
      { 0x49, CHAR('I'), CHAR('i'), CHAR('I'), UPPER_CASE_CHAR_FLAGS },
      { 0x4A, CHAR('J'), CHAR('j'), CHAR('J'), UPPER_CASE_CHAR_FLAGS },
      { 0x4B, CHAR('K'), CHAR('k'), CHAR('K'), UPPER_CASE_CHAR_FLAGS },
      { 0x4C, CHAR('L'), CHAR('l'), CHAR('L'), UPPER_CASE_CHAR_FLAGS },
      { 0x4D, CHAR('M'), CHAR('m'), CHAR('M'), UPPER_CASE_CHAR_FLAGS },
      { 0x4E, CHAR('N'), CHAR('n'), CHAR('N'), UPPER_CASE_CHAR_FLAGS },
      { 0x4F, CHAR('O'), CHAR('o'), CHAR('O'), UPPER_CASE_CHAR_FLAGS },
      { 0x50, CHAR('P'), CHAR('p'), CHAR('P'), UPPER_CASE_CHAR_FLAGS },
      { 0x51, CHAR('Q'), CHAR('q'), CHAR('Q'), UPPER_CASE_CHAR_FLAGS },
      { 0x52, CHAR('R'), CHAR('r'), CHAR('R'), UPPER_CASE_CHAR_FLAGS },
      { 0x53, CHAR('S'), CHAR('s'), CHAR('S'), UPPER_CASE_CHAR_FLAGS },
      { 0x54, CHAR('T'), CHAR('t'), CHAR('T'), UPPER_CASE_CHAR_FLAGS },
      { 0x55, CHAR('U'), CHAR('u'), CHAR('U'), UPPER_CASE_CHAR_FLAGS },
      { 0x56, CHAR('V'), CHAR('v'), CHAR('V'), UPPER_CASE_CHAR_FLAGS },
      { 0x57, CHAR('W'), CHAR('w'), CHAR('W'), UPPER_CASE_CHAR_FLAGS },
      { 0x58, CHAR('X'), CHAR('x'), CHAR('X'), UPPER_CASE_CHAR_FLAGS },
      { 0x59, CHAR('Y'), CHAR('y'), CHAR('Y'), UPPER_CASE_CHAR_FLAGS },
      { 0x5A, CHAR('Z'), CHAR('z'), CHAR('Z'), UPPER_CASE_CHAR_FLAGS },
      { 0x5B, CHAR('['), CHAR('['), CHAR('['), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR | CStringLib::URL_ENCODABLE_CHAR },
      { 0x5C, CHAR('\\'), CHAR('\\'), CHAR('\\'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR | CStringLib::URL_ENCODABLE_CHAR },
      { 0x5D, CHAR(']'), CHAR(']'), CHAR(']'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR | CStringLib::URL_ENCODABLE_CHAR },
      { 0x5E, CHAR('^'), CHAR('^'), CHAR('^'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR | CStringLib::URL_ENCODABLE_CHAR },
      { 0x5F, CHAR('_'), CHAR('_'), CHAR('_'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR | CStringLib::URL_HOST_CHAR },
      { 0x60, CHAR('`'), CHAR('`'), CHAR('`'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR },
      { 0x61, CHAR('a'), CHAR('a'), CHAR('A'), LOWER_CASE_CHAR_FLAGS | CStringLib::HEX_CHAR },
      { 0x62, CHAR('b'), CHAR('b'), CHAR('B'), LOWER_CASE_CHAR_FLAGS | CStringLib::HEX_CHAR },
      { 0x63, CHAR('c'), CHAR('c'), CHAR('C'), LOWER_CASE_CHAR_FLAGS | CStringLib::HEX_CHAR },
      { 0x64, CHAR('d'), CHAR('d'), CHAR('D'), LOWER_CASE_CHAR_FLAGS | CStringLib::HEX_CHAR },
      { 0x65, CHAR('e'), CHAR('e'), CHAR('E'), LOWER_CASE_CHAR_FLAGS | CStringLib::HEX_CHAR },
      { 0x66, CHAR('f'), CHAR('f'), CHAR('F'), LOWER_CASE_CHAR_FLAGS | CStringLib::HEX_CHAR },
      { 0x67, CHAR('g'), CHAR('g'), CHAR('G'), LOWER_CASE_CHAR_FLAGS },
      { 0x68, CHAR('h'), CHAR('h'), CHAR('H'), LOWER_CASE_CHAR_FLAGS },
      { 0x69, CHAR('i'), CHAR('i'), CHAR('I'), LOWER_CASE_CHAR_FLAGS },
      { 0x6A, CHAR('j'), CHAR('j'), CHAR('J'), LOWER_CASE_CHAR_FLAGS },
      { 0x6B, CHAR('k'), CHAR('k'), CHAR('K'), LOWER_CASE_CHAR_FLAGS },
      { 0x6C, CHAR('l'), CHAR('l'), CHAR('L'), LOWER_CASE_CHAR_FLAGS },
      { 0x6D, CHAR('m'), CHAR('m'), CHAR('M'), LOWER_CASE_CHAR_FLAGS },
      { 0x6E, CHAR('n'), CHAR('n'), CHAR('N'), LOWER_CASE_CHAR_FLAGS },
      { 0x6F, CHAR('o'), CHAR('o'), CHAR('O'), LOWER_CASE_CHAR_FLAGS },
      { 0x70, CHAR('p'), CHAR('p'), CHAR('P'), LOWER_CASE_CHAR_FLAGS },
      { 0x71, CHAR('q'), CHAR('q'), CHAR('Q'), LOWER_CASE_CHAR_FLAGS },
      { 0x72, CHAR('r'), CHAR('r'), CHAR('R'), LOWER_CASE_CHAR_FLAGS },
      { 0x73, CHAR('s'), CHAR('s'), CHAR('S'), LOWER_CASE_CHAR_FLAGS },
      { 0x74, CHAR('t'), CHAR('t'), CHAR('T'), LOWER_CASE_CHAR_FLAGS },
      { 0x75, CHAR('u'), CHAR('u'), CHAR('U'), LOWER_CASE_CHAR_FLAGS },
      { 0x76, CHAR('v'), CHAR('v'), CHAR('V'), LOWER_CASE_CHAR_FLAGS },
      { 0x77, CHAR('w'), CHAR('w'), CHAR('W'), LOWER_CASE_CHAR_FLAGS },
      { 0x78, CHAR('x'), CHAR('x'), CHAR('X'), LOWER_CASE_CHAR_FLAGS },
      { 0x79, CHAR('y'), CHAR('y'), CHAR('Y'), LOWER_CASE_CHAR_FLAGS },
      { 0x7A, CHAR('z'), CHAR('z'), CHAR('Z'), LOWER_CASE_CHAR_FLAGS },
      { 0x7B, CHAR('{'), CHAR('{'), CHAR('{'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR | CStringLib::URL_ENCODABLE_CHAR },
      { 0x7C, CHAR('|'), CHAR('|'), CHAR('|'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR | CStringLib::URL_ENCODABLE_CHAR },
      { 0x7D, CHAR('}'), CHAR('}'), CHAR('}'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR | CStringLib::URL_ENCODABLE_CHAR },
      { 0x7E, CHAR('~'), CHAR('~'), CHAR('~'), CStringLib::URL_PATH_CHAR | CStringLib::ASCII_CHAR | CStringLib::URL_FRAGMENT_CHAR | CStringLib::URL_QUERY_CHAR | CStringLib::URL_ENCODABLE_CHAR },
      { 0x7F, CHAR(0x7F), CHAR(0x7F), CHAR(0x7F), CStringLib::URL_ENCODABLE_CHAR },
      { 0x80, CHAR(0x80), CHAR(0x80), CHAR(0x80), CStringLib::URL_ENCODABLE_CHAR },
      { 0x81, CHAR(0x81), CHAR(0x81), CHAR(0x81), CStringLib::URL_ENCODABLE_CHAR },
      { 0x82, CHAR(0x82), CHAR(0x82), CHAR(0x82), CStringLib::URL_ENCODABLE_CHAR },
      { 0x83, CHAR(0x83), CHAR(0x83), CHAR(0x83), CStringLib::URL_ENCODABLE_CHAR },
      { 0x84, CHAR(0x84), CHAR(0x84), CHAR(0x84), CStringLib::URL_ENCODABLE_CHAR },
      { 0x85, CHAR(0x85), CHAR(0x85), CHAR(0x85), CStringLib::URL_ENCODABLE_CHAR },
      { 0x86, CHAR(0x86), CHAR(0x86), CHAR(0x86), CStringLib::URL_ENCODABLE_CHAR },
      { 0x87, CHAR(0x87), CHAR(0x87), CHAR(0x87), CStringLib::URL_ENCODABLE_CHAR },
      { 0x88, CHAR(0x88), CHAR(0x88), CHAR(0x88), CStringLib::URL_ENCODABLE_CHAR },
      { 0x89, CHAR(0x89), CHAR(0x89), CHAR(0x89), CStringLib::URL_ENCODABLE_CHAR },
      { 0x8A, CHAR(0x8A), CHAR(0x8A), CHAR(0x8A), CStringLib::URL_ENCODABLE_CHAR },
      { 0x8B, CHAR(0x8B), CHAR(0x8B), CHAR(0x8B), CStringLib::URL_ENCODABLE_CHAR },
      { 0x8C, CHAR(0x8C), CHAR(0x8C), CHAR(0x8C), CStringLib::URL_ENCODABLE_CHAR },
      { 0x8D, CHAR(0x8D), CHAR(0x8D), CHAR(0x8D), CStringLib::URL_ENCODABLE_CHAR },
      { 0x8E, CHAR(0x8E), CHAR(0x8E), CHAR(0x8E), CStringLib::URL_ENCODABLE_CHAR },
      { 0x8F, CHAR(0x8F), CHAR(0x8F), CHAR(0x8F), CStringLib::URL_ENCODABLE_CHAR },
      { 0x90, CHAR(0x90), CHAR(0x90), CHAR(0x90), CStringLib::URL_ENCODABLE_CHAR },
      { 0x91, CHAR(0x91), CHAR(0x91), CHAR(0x91), CStringLib::URL_ENCODABLE_CHAR },
      { 0x92, CHAR(0x92), CHAR(0x92), CHAR(0x92), CStringLib::URL_ENCODABLE_CHAR },
      { 0x93, CHAR(0x93), CHAR(0x93), CHAR(0x93), CStringLib::URL_ENCODABLE_CHAR },
      { 0x94, CHAR(0x94), CHAR(0x94), CHAR(0x94), CStringLib::URL_ENCODABLE_CHAR },
      { 0x95, CHAR(0x95), CHAR(0x95), CHAR(0x95), CStringLib::URL_ENCODABLE_CHAR },
      { 0x96, CHAR(0x96), CHAR(0x96), CHAR(0x96), CStringLib::URL_ENCODABLE_CHAR },
      { 0x97, CHAR(0x97), CHAR(0x97), CHAR(0x97), CStringLib::URL_ENCODABLE_CHAR },
      { 0x98, CHAR(0x98), CHAR(0x98), CHAR(0x98), CStringLib::URL_ENCODABLE_CHAR },
      { 0x99, CHAR(0x99), CHAR(0x99), CHAR(0x99), CStringLib::URL_ENCODABLE_CHAR },
      { 0x9A, CHAR(0x9A), CHAR(0x9A), CHAR(0x9A), CStringLib::URL_ENCODABLE_CHAR },
      { 0x9B, CHAR(0x9B), CHAR(0x9B), CHAR(0x9B), CStringLib::URL_ENCODABLE_CHAR },
      { 0x9C, CHAR(0x9C), CHAR(0x9C), CHAR(0x9C), CStringLib::URL_ENCODABLE_CHAR },
      { 0x9D, CHAR(0x9D), CHAR(0x9D), CHAR(0x9D), CStringLib::URL_ENCODABLE_CHAR },
      { 0x9E, CHAR(0x9E), CHAR(0x9E), CHAR(0x9E), CStringLib::URL_ENCODABLE_CHAR },
      { 0x9F, CHAR(0x9F), CHAR(0x9F), CHAR(0x9F), CStringLib::URL_ENCODABLE_CHAR },
      { 0xA0, CHAR(0xA0), CHAR(0xA0), CHAR(0xA0), CStringLib::URL_ENCODABLE_CHAR },
      { 0xA1, CHAR(0xA1), CHAR(0xA1), CHAR(0xA1), CStringLib::URL_ENCODABLE_CHAR },
      { 0xA2, CHAR(0xA2), CHAR(0xA2), CHAR(0xA2), CStringLib::URL_ENCODABLE_CHAR },
      { 0xA3, CHAR(0xA3), CHAR(0xA3), CHAR(0xA3), CStringLib::URL_ENCODABLE_CHAR },
      { 0xA4, CHAR(0xA4), CHAR(0xA4), CHAR(0xA4), CStringLib::URL_ENCODABLE_CHAR },
      { 0xA5, CHAR(0xA5), CHAR(0xA5), CHAR(0xA5), CStringLib::URL_ENCODABLE_CHAR },
      { 0xA6, CHAR(0xA6), CHAR(0xA6), CHAR(0xA6), CStringLib::URL_ENCODABLE_CHAR },
      { 0xA7, CHAR(0xA7), CHAR(0xA7), CHAR(0xA7), CStringLib::URL_ENCODABLE_CHAR },
      { 0xA8, CHAR(0xA8), CHAR(0xA8), CHAR(0xA8), CStringLib::URL_ENCODABLE_CHAR },
      { 0xA9, CHAR(0xA9), CHAR(0xA9), CHAR(0xA9), CStringLib::URL_ENCODABLE_CHAR },
      { 0xAA, CHAR(0xAA), CHAR(0xAA), CHAR(0xAA), CStringLib::URL_ENCODABLE_CHAR },
      { 0xAB, CHAR(0xAB), CHAR(0xAB), CHAR(0xAB), CStringLib::URL_ENCODABLE_CHAR },
      { 0xAC, CHAR(0xAC), CHAR(0xAC), CHAR(0xAC), CStringLib::URL_ENCODABLE_CHAR },
      { 0xAD, CHAR(0xAD), CHAR(0xAD), CHAR(0xAD), CStringLib::URL_ENCODABLE_CHAR },
      { 0xAE, CHAR(0xAE), CHAR(0xAE), CHAR(0xAE), CStringLib::URL_ENCODABLE_CHAR },
      { 0xAF, CHAR(0xAF), CHAR(0xAF), CHAR(0xAF), CStringLib::URL_ENCODABLE_CHAR },
      { 0xB0, CHAR(0xB0), CHAR(0xB0), CHAR(0xB0), CStringLib::URL_ENCODABLE_CHAR },
      { 0xB1, CHAR(0xB1), CHAR(0xB1), CHAR(0xB1), CStringLib::URL_ENCODABLE_CHAR },
      { 0xB2, CHAR(0xB2), CHAR(0xB2), CHAR(0xB2), CStringLib::URL_ENCODABLE_CHAR },
      { 0xB3, CHAR(0xB3), CHAR(0xB3), CHAR(0xB3), CStringLib::URL_ENCODABLE_CHAR },
      { 0xB4, CHAR(0xB4), CHAR(0xB4), CHAR(0xB4), CStringLib::URL_ENCODABLE_CHAR },
      { 0xB5, CHAR(0xB5), CHAR(0xB5), CHAR(0xB5), CStringLib::URL_ENCODABLE_CHAR },
      { 0xB6, CHAR(0xB6), CHAR(0xB6), CHAR(0xB6), CStringLib::URL_ENCODABLE_CHAR },
      { 0xB7, CHAR(0xB7), CHAR(0xB7), CHAR(0xB7), CStringLib::URL_ENCODABLE_CHAR },
      { 0xB8, CHAR(0xB8), CHAR(0xB8), CHAR(0xB8), CStringLib::URL_ENCODABLE_CHAR },
      { 0xB9, CHAR(0xB9), CHAR(0xB9), CHAR(0xB9), CStringLib::URL_ENCODABLE_CHAR },
      { 0xBA, CHAR(0xBA), CHAR(0xBA), CHAR(0xBA), CStringLib::URL_ENCODABLE_CHAR },
      { 0xBB, CHAR(0xBB), CHAR(0xBB), CHAR(0xBB), CStringLib::URL_ENCODABLE_CHAR },
      { 0xBC, CHAR(0xBC), CHAR(0xBC), CHAR(0xBC), CStringLib::URL_ENCODABLE_CHAR },
      { 0xBD, CHAR(0xBD), CHAR(0xBD), CHAR(0xBD), CStringLib::URL_ENCODABLE_CHAR },
      { 0xBE, CHAR(0xBE), CHAR(0xBE), CHAR(0xBE), CStringLib::URL_ENCODABLE_CHAR },
      { 0xBF, CHAR(0xBF), CHAR(0xBF), CHAR(0xBF), CStringLib::URL_ENCODABLE_CHAR },
      { 0xC0, CHAR(0xC0), CHAR(0xC0), CHAR(0xC0), CStringLib::URL_ENCODABLE_CHAR },
      { 0xC1, CHAR(0xC1), CHAR(0xC1), CHAR(0xC1), CStringLib::URL_ENCODABLE_CHAR },
      { 0xC2, CHAR(0xC2), CHAR(0xC2), CHAR(0xC2), CStringLib::URL_ENCODABLE_CHAR },
      { 0xC3, CHAR(0xC3), CHAR(0xC3), CHAR(0xC3), CStringLib::URL_ENCODABLE_CHAR },
      { 0xC4, CHAR(0xC4), CHAR(0xC4), CHAR(0xC4), CStringLib::URL_ENCODABLE_CHAR },
      { 0xC5, CHAR(0xC5), CHAR(0xC5), CHAR(0xC5), CStringLib::URL_ENCODABLE_CHAR },
      { 0xC6, CHAR(0xC6), CHAR(0xC6), CHAR(0xC6), CStringLib::URL_ENCODABLE_CHAR },
      { 0xC7, CHAR(0xC7), CHAR(0xC7), CHAR(0xC7), CStringLib::URL_ENCODABLE_CHAR },
      { 0xC8, CHAR(0xC8), CHAR(0xC8), CHAR(0xC8), CStringLib::URL_ENCODABLE_CHAR },
      { 0xC9, CHAR(0xC9), CHAR(0xC9), CHAR(0xC9), CStringLib::URL_ENCODABLE_CHAR },
      { 0xCA, CHAR(0xCA), CHAR(0xCA), CHAR(0xCA), CStringLib::URL_ENCODABLE_CHAR },
      { 0xCB, CHAR(0xCB), CHAR(0xCB), CHAR(0xCB), CStringLib::URL_ENCODABLE_CHAR },
      { 0xCC, CHAR(0xCC), CHAR(0xCC), CHAR(0xCC), CStringLib::URL_ENCODABLE_CHAR },
      { 0xCD, CHAR(0xCD), CHAR(0xCD), CHAR(0xCD), CStringLib::URL_ENCODABLE_CHAR },
      { 0xCE, CHAR(0xCE), CHAR(0xCE), CHAR(0xCE), CStringLib::URL_ENCODABLE_CHAR },
      { 0xCF, CHAR(0xCF), CHAR(0xCF), CHAR(0xCF), CStringLib::URL_ENCODABLE_CHAR },
      { 0xD0, CHAR(0xD0), CHAR(0xD0), CHAR(0xD0), CStringLib::URL_ENCODABLE_CHAR },
      { 0xD1, CHAR(0xD1), CHAR(0xD1), CHAR(0xD1), CStringLib::URL_ENCODABLE_CHAR },
      { 0xD2, CHAR(0xD2), CHAR(0xD2), CHAR(0xD2), CStringLib::URL_ENCODABLE_CHAR },
      { 0xD3, CHAR(0xD3), CHAR(0xD3), CHAR(0xD3), CStringLib::URL_ENCODABLE_CHAR },
      { 0xD4, CHAR(0xD4), CHAR(0xD4), CHAR(0xD4), CStringLib::URL_ENCODABLE_CHAR },
      { 0xD5, CHAR(0xD5), CHAR(0xD5), CHAR(0xD5), CStringLib::URL_ENCODABLE_CHAR },
      { 0xD6, CHAR(0xD6), CHAR(0xD6), CHAR(0xD6), CStringLib::URL_ENCODABLE_CHAR },
      { 0xD7, CHAR(0xD7), CHAR(0xD7), CHAR(0xD7), CStringLib::URL_ENCODABLE_CHAR },
      { 0xD8, CHAR(0xD8), CHAR(0xD8), CHAR(0xD8), CStringLib::URL_ENCODABLE_CHAR },
      { 0xD9, CHAR(0xD9), CHAR(0xD9), CHAR(0xD9), CStringLib::URL_ENCODABLE_CHAR },
      { 0xDA, CHAR(0xDA), CHAR(0xDA), CHAR(0xDA), CStringLib::URL_ENCODABLE_CHAR },
      { 0xDB, CHAR(0xDB), CHAR(0xDB), CHAR(0xDB), CStringLib::URL_ENCODABLE_CHAR },
      { 0xDC, CHAR(0xDC), CHAR(0xDC), CHAR(0xDC), CStringLib::URL_ENCODABLE_CHAR },
      { 0xDD, CHAR(0xDD), CHAR(0xDD), CHAR(0xDD), CStringLib::URL_ENCODABLE_CHAR },
      { 0xDE, CHAR(0xDE), CHAR(0xDE), CHAR(0xDE), CStringLib::URL_ENCODABLE_CHAR },
      { 0xDF, CHAR(0xDF), CHAR(0xDF), CHAR(0xDF), CStringLib::URL_ENCODABLE_CHAR },
      { 0xE0, CHAR(0xE0), CHAR(0xE0), CHAR(0xE0), CStringLib::URL_ENCODABLE_CHAR },
      { 0xE1, CHAR(0xE1), CHAR(0xE1), CHAR(0xE1), CStringLib::URL_ENCODABLE_CHAR },
      { 0xE2, CHAR(0xE2), CHAR(0xE2), CHAR(0xE2), CStringLib::URL_ENCODABLE_CHAR },
      { 0xE3, CHAR(0xE3), CHAR(0xE3), CHAR(0xE3), CStringLib::URL_ENCODABLE_CHAR },
      { 0xE4, CHAR(0xE4), CHAR(0xE4), CHAR(0xE4), CStringLib::URL_ENCODABLE_CHAR },
      { 0xE5, CHAR(0xE5), CHAR(0xE5), CHAR(0xE5), CStringLib::URL_ENCODABLE_CHAR },
      { 0xE6, CHAR(0xE6), CHAR(0xE6), CHAR(0xE6), CStringLib::URL_ENCODABLE_CHAR },
      { 0xE7, CHAR(0xE7), CHAR(0xE7), CHAR(0xE7), CStringLib::URL_ENCODABLE_CHAR },
      { 0xE8, CHAR(0xE8), CHAR(0xE8), CHAR(0xE8), CStringLib::URL_ENCODABLE_CHAR },
      { 0xE9, CHAR(0xE9), CHAR(0xE9), CHAR(0xE9), CStringLib::URL_ENCODABLE_CHAR },
      { 0xEA, CHAR(0xEA), CHAR(0xEA), CHAR(0xEA), CStringLib::URL_ENCODABLE_CHAR },
      { 0xEB, CHAR(0xEB), CHAR(0xEB), CHAR(0xEB), CStringLib::URL_ENCODABLE_CHAR },
      { 0xEC, CHAR(0xEC), CHAR(0xEC), CHAR(0xEC), CStringLib::URL_ENCODABLE_CHAR },
      { 0xED, CHAR(0xED), CHAR(0xED), CHAR(0xED), CStringLib::URL_ENCODABLE_CHAR },
      { 0xEE, CHAR(0xEE), CHAR(0xEE), CHAR(0xEE), CStringLib::URL_ENCODABLE_CHAR },
      { 0xEF, CHAR(0xEF), CHAR(0xEF), CHAR(0xEF), CStringLib::URL_ENCODABLE_CHAR },
      { 0xF0, CHAR(0xF0), CHAR(0xF0), CHAR(0xF0), CStringLib::URL_ENCODABLE_CHAR },
      { 0xF1, CHAR(0xF1), CHAR(0xF1), CHAR(0xF1), CStringLib::URL_ENCODABLE_CHAR },
      { 0xF2, CHAR(0xF2), CHAR(0xF2), CHAR(0xF2), CStringLib::URL_ENCODABLE_CHAR },
      { 0xF3, CHAR(0xF3), CHAR(0xF3), CHAR(0xF3), CStringLib::URL_ENCODABLE_CHAR },
      { 0xF4, CHAR(0xF4), CHAR(0xF4), CHAR(0xF4), CStringLib::URL_ENCODABLE_CHAR },
      { 0xF5, CHAR(0xF5), CHAR(0xF5), CHAR(0xF5), CStringLib::URL_ENCODABLE_CHAR },
      { 0xF6, CHAR(0xF6), CHAR(0xF6), CHAR(0xF6), CStringLib::URL_ENCODABLE_CHAR },
      { 0xF7, CHAR(0xF7), CHAR(0xF7), CHAR(0xF7), CStringLib::URL_ENCODABLE_CHAR },
      { 0xF8, CHAR(0xF8), CHAR(0xF8), CHAR(0xF8), CStringLib::URL_ENCODABLE_CHAR },
      { 0xF9, CHAR(0xF9), CHAR(0xF9), CHAR(0xF9), CStringLib::URL_ENCODABLE_CHAR },
      { 0xFA, CHAR(0xFA), CHAR(0xFA), CHAR(0xFA), CStringLib::URL_ENCODABLE_CHAR },
      { 0xFB, CHAR(0xFB), CHAR(0xFB), CHAR(0xFB), CStringLib::URL_ENCODABLE_CHAR },
      { 0xFC, CHAR(0xFC), CHAR(0xFC), CHAR(0xFC), CStringLib::URL_ENCODABLE_CHAR },
      { 0xFD, CHAR(0xFD), CHAR(0xFD), CHAR(0xFD), CStringLib::URL_ENCODABLE_CHAR },
      { 0xFE, CHAR(0xFE), CHAR(0xFE), CHAR(0xFE), CStringLib::URL_ENCODABLE_CHAR },
      { 0xFF, CHAR(0xFF), CHAR(0xFF), CHAR(0xFF), CStringLib::URL_ENCODABLE_CHAR }
};



////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
int
CStringLib::GetCharProperties(const char *pChar, int32 length) {
    if (NULL == pChar) {
        return(0);
    }

    if (1 == length) {
        return((g_ByteInfoList[*((uchar *) pChar) ].m_Flags));
    } else
    {
        // This isn't a real error. It's just a marker because this is untested code.
        //<> Not a real error.
        REPORT_LOW_LEVEL_BUG();
        //<> Not a real error.

        return(0);
    }
} // GetCharProperties




////////////////////////////////////////////////////////////////////////////////
//
// [IsByte]
//
////////////////////////////////////////////////////////////////////////////////
int32
CStringLib::IsByte(char c, int32 newFlags) {
    return((g_ByteInfoList[ (uchar) c ].m_Flags) & newFlags);
}




/////////////////////////////////////////////////////////////////////////////
//
// [CopyUTF8String]
//
// This avoids several problems of the POSIX strncpy:
//
// 1. It raises a warning if the string is truncated.
//
// 2. In the official strncpy, a null character
// is not appended automatically to the copied string. This always
// NULL-terminates a string.
//
// 3. In the official strncpy, if count is greater than
// the length of strSource, the destination string is padded with
// null characters up to length count.
//
// 4. This is aware of UTF-8, and will always truncate at a character
// boundary. If the last chararacter is a multi-byte character, then
// this will truncate at the start of the last character.
//
/////////////////////////////////////////////////////////////////////////////
void
CStringLib::CopyUTF8String(char *pDestPtr, const char *pSrcPtr, int32 maxLength) {
    const char *pEndDestPtr;

    if ((NULL == pDestPtr)
        || (NULL == pSrcPtr)
        || (maxLength < 0)) {
        REPORT_LOW_LEVEL_BUG();
        return;
    }

    pEndDestPtr = pDestPtr + maxLength;

    // Leave space for the NULL-terminating char.
    // We always NULL-terminate.
    //
    // This is tricky. Sometimes, maxLength is the size
    // of the string, and we assume that the buffer is at least 1
    // character longer.
    //
    // We define the length to be the length of the source string.
    // This allows us to copy a substring from a larger string.

    // Stop at the length or the null-terminator, whichever comes first.
    while ((*pSrcPtr) && (pDestPtr < pEndDestPtr)) {
        *(pDestPtr++) = *(pSrcPtr++);
    }

    // If we stopped before the end of the string, then
    // we truncated the string. This is not a real bug;
    // we often copy a substring from a larger string.

    // We left space for the NULL-terminating char, so
    // we don't have to test whether pDestPtr >= pEndDestPtr;
    *pDestPtr = 0;
} // CopyUTF8String








/////////////////////////////////////////////////////////////////////////////
//
// [UnicodeStrcmp]
//
// This returns 0 if pStr1 equals pStr2.
// It returns -1 if pStr1 < pStr2
// It returns 1 if pStr1 > pStr2.
//
// The strings are in UTF-8 form.
//
// str1Length is the actual length in bytes.
// Every character in this range must match.
//
// maxStr2Length is the MAX possible length in bytes.
// The beginning, but not necessarily all bytes must match.
// For example, it may be the the size of the entire buffer string2
// is found in. It is also NOT the size of the characters.
// If we are matching with special options like ignoring case or special
// accent marks, then two strings of different size may match.
/////////////////////////////////////////////////////////////////////////////
int32
CStringLib::UnicodeStrcmp(
                    const char *pStr1,
                    int32 str1Length,
                    const char *pStr2,
                    int32 maxStr2Length,
                    int32 options) {
    const char *pEndStr1;
    const char *pEndStr2;
    const char *pStartStr1;
    const char *pStartStr2;
    char c1;
    char c2;
    CByteInfo *pByte1Info;
    CByteInfo *pByte2Info;
    int32 result = 0;

    // 2 NULL strings are equal.
    if ((NULL == pStr1) && (NULL == pStr2)) {
        return(0);
    }
    // NULL is less than non-NULL.
    if (NULL == pStr1) {
        return(-1);
    }
    if (NULL == pStr2) {
        return(1);
    }

    // If the sizes are < 0, then this means the strings are
    // NULL-terminated.
    if (str1Length < 0) {
        str1Length = strlen(pStr1);
    }
    if (maxStr2Length < 0) {
        maxStr2Length = strlen(pStr2);
    }
    pStartStr1 = pStr1;
    pStartStr2 = pStr2;
    pEndStr1 = pStartStr1 + str1Length;
    pEndStr2 = pStartStr2 + maxStr2Length;


    // FAST PATH
    // Try to compare the two strings as if they were ASCII strings.
    // This will cover a lot of cases, like comparing some reserved
    // keywords in formats like HTTP, XML, and more. Of course, it won't
    // work for international strings in non-standard XML or file paths
    // or other cases. If we hit a non-ASCII character, then we switch to
    // a slower but internationally correct compare.
    while ((pStr1 < pEndStr1) && (pStr2 < pEndStr2)) {
        c1 = *pStr1;
        c2 = *pStr2;
        pStr1 += 1;
        pStr2 += 1;

        // First, check if the bytes are identical. This is a match
        // no matter what, since it is the equivilent of a memcmp.
        if (c1 == c2) {
            continue;
        }

        // If they don't match, then they are not identical but
        // they may still be similar enough. First, find out if they are
        // ASCII characters that are only different case.
        pByte1Info = &(g_ByteInfoList[((uchar) c1)]);
        pByte2Info = &(g_ByteInfoList[((uchar) c2)]);
        if ((options & CStringLib::IGNORE_CASE)
                && (CStringLib::ASCII_CHAR & pByte1Info->m_Flags)
                && (CStringLib::ASCII_CHAR & pByte2Info->m_Flags)) {
            c1 = pByte1Info->m_LowerCaseASCIICharVal;
            c2 = pByte2Info->m_LowerCaseASCIICharVal;
            if (c1 == c2) {
                continue;
            }
        }

        // SLOW PATH
        // If either the pattern string or the second string are not ASCII,
        // then they may still match.
        // Do a more expensive comparison by first converting both
        // strings to unicode. Because this is expensive, we try to
        // avoid it when it is not necessary. For example,
        // if we are searching for the "]]>" sequence that marks the end
        // of a CDATA section of an XML file, or a closing element in
        // an XML file, then we need to support unicode characters, but
        // only when the pattern is unicode. In these cases, we don't
        // allow a unicode string to match a pure ASCII string. That can
        // only happen if we do things like ignore accents or match
        // different characters like the German double-s character
        // (in words like grosse) to 2 ASCII 's' characters.
        //
        // An ASCII string may match to an equivilent but different Unicode string.
        // By default, we allow non-literal matches.
        if (!(CStringLib::ASCII_CHAR & pByte1Info->m_Flags)
                || !(CStringLib::ASCII_CHAR & pByte2Info->m_Flags)) {
#if WIN32
            ErrVal err = ENoErr;
            int32 safeMaxStringLengthInBytes = (str1Length + 1) * 4;
            WCHAR *pWStr1 = NULL;
            WCHAR *pWStr2 = NULL;
            int32 actualWstr1LengthInBytes = 0;
            int32 actualWstr2LengthInBytes = 0;

            pWStr1 = (WCHAR *) malloc(safeMaxStringLengthInBytes);
            pWStr2 = (WCHAR *) malloc(safeMaxStringLengthInBytes);
            if ((NULL == pWStr1) || (NULL == pWStr2)) {
                REPORT_LOW_LEVEL_BUG();
                result = -1;
                break;
            }
            err = CStringLib::ConvertUTF8ToUTF16(
                                    pStartStr1,
                                    str1Length,
                                    pWStr1,
                                    safeMaxStringLengthInBytes,
                                    &actualWstr1LengthInBytes);
            if (err) {
                free(pWStr1);
                free(pWStr2);
                REPORT_LOW_LEVEL_BUG();
                result = -1;
                break;
            }
            pWStr1[actualWstr1LengthInBytes / sizeof(WCHAR)] = 0;

            err = CStringLib::ConvertUTF8ToUTF16(
                                    pStartStr2,
                                    maxStr2Length,
                                    pWStr2,
                                    safeMaxStringLengthInBytes,
                                    &actualWstr2LengthInBytes);
            if (err) {
                free(pWStr1);
                free(pWStr2);
                REPORT_LOW_LEVEL_BUG();
                result = -1;
                break;
            }
            pWStr2[actualWstr2LengthInBytes / sizeof(WCHAR)] = 0;

            if (options & CStringLib::IGNORE_CASE) {
                result = _wcsicmp((WCHAR *) pWStr1, (WCHAR *) pWStr2);
            } else {
                result = wcscmp((WCHAR *) pWStr1, (WCHAR *) pWStr2);
            }

            free(pWStr1);
            free(pWStr2);
#else // LINUX
            // We are about to crash. It's ok to destructively damage the strings.
            {
                //char *pStr1 = (char *) pStartStr1;
                //char *pStr2 = (char *) pStartStr2;
                //pStr1[str1Length] = 0;
                //pStr2[str1Length] = 0;
                OSIndependantLayer::PrintToConsole("Unicode string mismatch on Linux");
                //OSIndependantLayer::PrintToConsole("str1Length=%d, pStr1=%s, pStr2=%s", str1Length, pStr1, pStr2);
            }
            REPORT_LOW_LEVEL_BUG();
#endif

            // That's it, we have compared the entire strings, so we are done.
            goto finishedEntireString;
        } // Slow path UTF-16 comparison

        // Otherwise, they are both ASCII and they did not match even
        // with case folding. This is a conclusive mismatch.
        if (c1 < c2) {
            result = -1;
        } else {
            result = 1;
        }
        break;
    } // while ((pStr1 < pEndStr1) && (pStr2 < pEndStr2))


    // Check if we quit before all the required characters matched.
    // In this case, this is a mismatch, and pStr2 is a prefix of
    // pStr1. So, pStr1 is > pStr2.
    if ((0 == result) && (pStr1 < pEndStr1)) {
        result = 1;
    }
    if ((0 == result) && (pStr2 < pEndStr2)) {
        result = -1;
    }


finishedEntireString:
    return(result);
} // UnicodeStrcmp







/////////////////////////////////////////////////////////////////////////////
//
// [FindPatternInBuffer]
//
/////////////////////////////////////////////////////////////////////////////
const char *
CStringLib::FindPatternInBuffer(
                        const char *pBuffer,
                        int32 bufferLength,
                        const char *pPattern,
                        int32 patternLength) {
    const char *pLastMatch;
    int32 result;

    if ((NULL == pPattern)
         || (NULL == pBuffer)
         || (bufferLength <= 0)) {
        return(NULL);
    }

    if (patternLength < 0) {
        patternLength = strlen(pPattern);
    }
    if (0 == patternLength) {
        return(NULL);
    }

    pLastMatch = pBuffer + bufferLength;
    pLastMatch = pLastMatch - patternLength;
    while (pBuffer <= pLastMatch) {
        result = strncasecmpex(pPattern, pBuffer, patternLength);
        if (0 == result) {
            return(pBuffer);
        }

        pBuffer++;
    } // while (pBuffer < pLastMatch)

    return(NULL);
} // FindPatternInBuffer






/////////////////////////////////////////////////////////////////////////////
//
// [StringToNumberEx]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CStringLib::StringToNumberEx(
                        const char *str,
                        int32 strLength,
                        int32 base,
                        int32 *num) {
    char c;
    const char *endStr = NULL;
    int32 digitValue;
    int32 negativeValue;


    if ((!str)
        || (!num)
        || (base <= 0)
        || (base > 32)) {
        return(EFail);
    }

    if (strLength <= 0) {
        return(EValueIsNotNumber);
    }
    endStr = str + strLength;


    if ('-' == *str) {
        negativeValue = true;
        str++;
        if (str >= endStr) {
            return(EValueIsNotNumber);
        }
    } else
    {
        negativeValue = false;
    }


    *num = 0;
    // We either go to the NULL terminator of a C string
    // or to the boundary passed in as an argument.
    while (((endStr) && (str < endStr))
            || ((!endStr) && (*str))) {
        c = *(str++);

        if ((c >= '0') && (c <= '9')) {
            digitValue = c - '0';
        } else if ((c >= 'a') && (c <= 'f') && ((c - 'a') <= (base - 10))) {
            digitValue = (c - 'a') + 10;
        } else if ((c >= 'A') && (c <= 'F') && ((c - 'A') <= (base - 10))) {
            digitValue = (c - 'A') + 10;
        } else if (',' == c) {
            continue;
        } else {
            return(EValueIsNotNumber);
        }

        if (*num != 0) {
            *num = *num * base;
        }

        *num = *num + digitValue;
    }

    if (negativeValue) {
        *num = -(*num);
    }

    return(ENoErr);
} // StringToNumberEx.







/////////////////////////////////////////////////////////////////////////////
//
// [ConvertUTF16ToUTF8]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CStringLib::ConvertUTF16ToUTF8(
                        const WCHAR *pSrc,
                        int32 srcLengthInBytes,
                        char *pDest,
                        int32 maxDestLengthInBytes,
                        int32 *pActualDestLengthInBytes) {
    ErrVal err = ENoErr;
    int actualSizeInBytes = 0;

    if ((NULL == pSrc)
            || (NULL == pDest)
            || (srcLengthInBytes < 0)
            || (maxDestLengthInBytes < 0)) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
        goto abort;
    }

#if WIN32
    int32 srcLengthInWideChars;

    if (srcLengthInBytes < 0) {
        srcLengthInWideChars = wcslen((const wchar_t *) pSrc);
    } else {
        srcLengthInWideChars = srcLengthInBytes / sizeof(WCHAR);
    }

    // Returns the number of bytes
    actualSizeInBytes = WideCharToMultiByte(
                        CP_UTF8, // CodePage,
                        0, // dwFlags,
                        (LPCWSTR) pSrc, // lpWideCharStr,
                        srcLengthInWideChars, // cchWideChar,
                        pDest,
                        maxDestLengthInBytes, // cbMultiByte,
                        NULL, // lpDefaultChar,
                        NULL); // lpUsedDefaultChar
    if (actualSizeInBytes <= 0) {
        actualSizeInBytes = 0;
        err = EFail;
        REPORT_LOW_LEVEL_BUG();
    }
#elif LINUX
    err = EFail;
#endif

abort:
    if (NULL != pActualDestLengthInBytes) {
        *pActualDestLengthInBytes = actualSizeInBytes;
    }

    return(err);
} // ConvertUTF16ToUTF8






/////////////////////////////////////////////////////////////////////////////
//
// [ConvertUTF8ToUTF16]
//
/////////////////////////////////////////////////////////////////////////////
ErrVal
CStringLib::ConvertUTF8ToUTF16(
                        const char *pSrc,
                        int32 srcLengthInBytes,
                        WCHAR *pDest,
                        int32 maxDestLengthInBytes,
                        int32 *pActualDestLengthInBytes) {
    ErrVal err = ENoErr;
    int actualSizeInWChars = 0;

    if ((NULL == pSrc)
        || (NULL == pDest)
        || (srcLengthInBytes < 0)
        || (maxDestLengthInBytes < 0)) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
        goto abort;
    }


#if WIN32
    int maxDestLengthInWideChars = maxDestLengthInBytes * sizeof(WCHAR);

    if (srcLengthInBytes < 0) {
        srcLengthInBytes = strlen(pSrc);
    }

    // Returns the number of WCHARs.
    actualSizeInWChars = MultiByteToWideChar(
                                CP_UTF8, // CodePage,
                                0, // dwFlags,
                                pSrc, // lpWideCharStr,
                                srcLengthInBytes, // cchWideChar,
                                (LPWSTR) pDest,
                                maxDestLengthInWideChars); // cbMultiByte,
    if (actualSizeInWChars < 0) {
        actualSizeInWChars = 0;
        err = EFail;
        REPORT_LOW_LEVEL_BUG();
    }
#elif LINUX
    err = EFail;
#endif

abort:
    if (NULL != pActualDestLengthInBytes) {
        *pActualDestLengthInBytes = actualSizeInWChars * sizeof(WCHAR);
    }

    return(err);
} // ConvertUTF8ToUTF16








/////////////////////////////////////////////////////////////////////////////
//
// [DecodeString]
//
/////////////////////////////////////////////////////////////////////////////
int32
CStringLib::DecodeString(
                int32 encodingType,
                const void *pSrcPtrVoid,
                int32 strLength,
                void *pDestPtrVoid,
                int32 maxDestLength) {
    const char *pSrcPtr;
    const char *pEndSrcPtr;
    char *pDestPtr = NULL;
    char *pBeginDestPtr = NULL;
    char *pEndDestPtr = NULL;
    char c;
    char saveHighDigitChar;
    char highDigit = 0;
    char lowDigit = 0;

    UNUSED_PARAM(maxDestLength);
    if ((NULL == pDestPtrVoid)
        || (NULL == pSrcPtrVoid)
        || (strLength <= 0)) {
        return(0);
    }

    pSrcPtr = (const char *) pSrcPtrVoid;
    pEndSrcPtr = pSrcPtr + strLength;
    pDestPtr = (char *) pDestPtrVoid;
    pBeginDestPtr = pDestPtr;
    pEndDestPtr = pDestPtr + strLength - 1;
    while (pSrcPtr < pEndSrcPtr) {
        c = *(pSrcPtr++);

        /////////////////////////////////////////////////////////////////////
        if (('+' == c) && (URL_ENCODING == encodingType)) {
            *(pDestPtr++) = ' ';
        }
        /////////////////////////////////////////////////////////////////////
        else if (('\\' == c)
                && (pSrcPtr < pEndSrcPtr)
                && (SIMPLE_ENCODING == encodingType)) {
            // Find out what was escaped.
            c = *(pSrcPtr++);
            if ('n' == c) {
                *(pDestPtr++) = '\n';
            }
            else if ('r' == c) {
                *(pDestPtr++) = '\r';
            }
            else if ('t' == c) {
                *(pDestPtr++) = '\t';
            }
            else {
                *(pDestPtr++) = c;
            }
        } // ('\\' == c)
        /////////////////////////////////////////////////////////////////////
        else if (('%' == c) && (URL_ENCODING == encodingType)) {
            // Skip the % character.
            c = *(pSrcPtr++);

            // Convert the next 2 characters to a hex digit.
            // If this is not a hex char, then this is just a malformed URL.
            if (!(g_ByteInfoList[ (uchar) c ].m_Flags & CStringLib::HEX_CHAR)) {
                *(pDestPtr++) = '%';
                *(pDestPtr++) = c;
                continue;
            }
            saveHighDigitChar = c;

            if ((c >= 'A') && (c <= 'Z')) {
                highDigit = (10 + (c - 'A')) << 4;
            }
            else if ((c >= 'a') && (c <= 'z')) {
                highDigit = (10 + (c - 'a')) << 4;
            }
            else if (g_ByteInfoList[ (uchar) c ].m_Flags & CStringLib::NUMBER_CHAR) {
                highDigit = (c - '0') << 4;
            }
            else {
                *(pDestPtr++) = *(pSrcPtr - 1);
            }

            // Encoded characters are always 2 digits.
            c = *(pSrcPtr++);
            if (!(g_ByteInfoList[ (uchar) c ].m_Flags & CStringLib::HEX_CHAR)) {
                *(pDestPtr++) = '%';
                *(pDestPtr++) = saveHighDigitChar;
                *(pDestPtr++) = c;
                continue;
            }


            if ((c >= 'A') && (c <= 'Z')) {
                lowDigit = (10 + (c - 'A'));
            }
            else if ((c >= 'a') && (c <= 'z')) {
                lowDigit = (10 + (c - 'a'));
            }
            else if (g_ByteInfoList[ (uchar) c ].m_Flags & CStringLib::NUMBER_CHAR) {
                lowDigit = (c - '0');
            }
            else {
                *(pDestPtr++) = *(pSrcPtr - 1);
            }

            *(pDestPtr++) = (highDigit | lowDigit);
        } // ('%' == c)
        /////////////////////////////////////////////////////////////////////
        else {
            *(pDestPtr++) = c;
        }
    } // while (pSrcPtr < pEndSrcPtr)

    // Do not advance the pointer, because the terminating character
    // is not part of the string length.
    if (pDestPtr < pEndDestPtr) {
        *pDestPtr = 0;
    }

    return(pDestPtr - pBeginDestPtr);
} // DecodeString.






/////////////////////////////////////////////////////////////////////////////
//
// [GetMaxEncodedLength]
//
/////////////////////////////////////////////////////////////////////////////
int32
CStringLib::GetMaxEncodedLength(
                            int32 encodingType,
                            const void *pSrcPtrVoid,
                            int32 strLength) {
    const char *pSrcPtr;
    const char *pEndSrcPtr;
    int32 total;
    char c;

    if ((NULL == pSrcPtrVoid) || (strLength <= 0)) {
        return(0);
    }

    total = 0;
    pSrcPtr = (const char *) pSrcPtrVoid;
    pEndSrcPtr = pSrcPtr + strLength;
    while (pSrcPtr < pEndSrcPtr) {
        c = *(pSrcPtr++);

        /////////////////////////////////////////////////////////////////////
        if ((SIMPLE_ENCODING == encodingType)
            && (('\n' == c)
                || ('\r' == c)
                || ('\\' == c)
                || ('\'' == c)
                || ('\"' == c)
                || ('\t' == c))) {
            total += 2;
        }
        /////////////////////////////////////////////////////////////////////
        else if ((URL_ENCODING == encodingType)
                && (g_ByteInfoList[ (uchar) c ].m_Flags & CStringLib::URL_ENCODABLE_CHAR)) {
            total += 3;
        }
        /////////////////////////////////////////////////////////////////////
        else {
            total += 1;
        }
    } // while (pSrcPtr < pEndSrcPtr)

    return(total);
} // GetMaxEncodedLength.








/////////////////////////////////////////////////////////////////////////////
//
// [EncodeString]
//
/////////////////////////////////////////////////////////////////////////////
int32
CStringLib::EncodeString(
                    int32 encodingType,
                    void *pDestPtrVoid,
                    int32 maxDestLength,
                    const void *pSrcPtrVoid,
                    int32 strLength) {
    const char *pSrcPtr;
    const char *pEndSrcPtr;
    char *pDestPtr;
    char *pBeginDestPtr;
    char *pEndDestPtr;
    char c;
    uchar encodeChar;


    if ((NULL == pDestPtrVoid)
        || (NULL == pSrcPtrVoid)
        || (strLength <= 0)) {
        return(0);
    }

    pSrcPtr = (const char *) pSrcPtrVoid;
    pEndSrcPtr = pSrcPtr + strLength;
    pDestPtr = (char *) pDestPtrVoid;
    pBeginDestPtr = pDestPtr;
    pEndDestPtr = pDestPtr + maxDestLength - 1;
    while (pSrcPtr < pEndSrcPtr) {
        c = *(pSrcPtr++);

        ///////////////////////////////////////////////////////////////
        if (('\n' == c)
            && (SIMPLE_ENCODING == encodingType)
            && ((pDestPtr + 2) < pEndDestPtr)) {
            *(pDestPtr++) = '\\';
            *(pDestPtr++) = 'n';
        }
        ///////////////////////////////////////////////////////////////
        else if (('\r' == c)
            && (SIMPLE_ENCODING == encodingType)
            && ((pDestPtr + 2) < pEndDestPtr)) {
            *(pDestPtr++) = '\\';
            *(pDestPtr++) = 'r';
        }
        ///////////////////////////////////////////////////////////////
        else if (('\t' == c)
            && (SIMPLE_ENCODING == encodingType)
            && ((pDestPtr + 2) < pEndDestPtr)) {
            *(pDestPtr++) = '\\';
            *(pDestPtr++) = 't';
        }
        ///////////////////////////////////////////////////////////////
        else if (('\\' == c)
            && (SIMPLE_ENCODING == encodingType)
            && ((pDestPtr + 2) < pEndDestPtr)) {
            *(pDestPtr++) = '\\';
            *(pDestPtr++) = '\\';
        }
        ///////////////////////////////////////////////////////////////
        else if (('\'' == c)
            && (SIMPLE_ENCODING == encodingType)
            && ((pDestPtr + 2) < pEndDestPtr)) {
            *(pDestPtr++) = '\\';
            *(pDestPtr++) = '\'';
        }
        ///////////////////////////////////////////////////////////////
        else if (('\"' == c)
            && (SIMPLE_ENCODING == encodingType)
            && ((pDestPtr + 2) < pEndDestPtr)) {
            *(pDestPtr++) = '\\';
            *(pDestPtr++) = '\"';
        }
        ///////////////////////////////////////////////////////////////
        else if ((URL_ENCODING == encodingType)
                && (g_ByteInfoList[ (uchar) c ].m_Flags & CStringLib::URL_ENCODABLE_CHAR)
                && ((pDestPtr + 3) < pEndDestPtr)) {
            *(pDestPtr++) = '%';

            // First, strip off the sign.
            encodeChar = (uchar) c;
            // Get the upper 4 bits.
            encodeChar = encodeChar >> 4;

            // Encoded characters are always triples, so write the
            // more significant digit even if it is 0.
            if (encodeChar >= 10) {
                encodeChar = 'A' + (encodeChar - 10);
            }
            else {
                encodeChar = '0' + encodeChar;
            }

            *(pDestPtr++) = encodeChar;

            // Get the lower 4 bits.
            encodeChar = c & 0x0F;
            if (encodeChar >= 10) {
                encodeChar = 'A' + (encodeChar - 10);
            }
            else {
                encodeChar = '0' + encodeChar;
            }

            *(pDestPtr++) = encodeChar;
        }
        ///////////////////////////////////////////////////////////////
        else {
            *(pDestPtr++) = c;
        }
    }

    // Do not advance the pointer, because the terminating character
    // is not part of the string length.
    if (pDestPtr < pEndDestPtr) {
        *pDestPtr = 0;
    }

    return(pDestPtr - pBeginDestPtr);
} // EncodeString








////////////////////////////////////////////////////////////////////////////////
//
// [ConvertToUpperCase]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CStringLib::ConvertToUpperCase(
                    const char *pChar,
                    int32 length,
                    char *pResultChar,
                    int32 maxLength,
                    int32 *pActualLengthResult) {
    ErrVal err = ENoErr;
    bool fFinishedConversion = false;

    // Leave space for a null terminator character.
    maxLength = maxLength - 1;

    if ((NULL == pChar)
        || (NULL == pResultChar)
        || (maxLength <= 0)) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
        goto abort;
    }
    if (NULL != pActualLengthResult) {
        *pActualLengthResult = 0;
    }

    if (length < 0) {
        length = strlen(pChar);
    }

    // If this happens, don't panic. Try to convert as much of the string
    // as possible, and then return an error. Some functions, like computing
    // a hash, may not care if the entire string is not converted.
    if (length > maxLength) {
        err = EFail;
        length = maxLength;
    }

#if WIN32
    unsigned char *pCopiedChar;

    // _mbsupr copies in place. So, first make a copy of
    // the string that we can destroy.
    memcpy(pResultChar, pChar, length);

    // _mbsupr expects a NULL-terminated string.
    pResultChar[length] = 0;

    // Convert in place. The result is the same as the input
    // param if the conversion was successful.
    pCopiedChar = _mbsupr((unsigned char *) pResultChar);
    if (NULL == pCopiedChar) {
        REPORT_LOW_LEVEL_BUG();
        err = EFail;
    } else if (NULL != pActualLengthResult) {
        *pActualLengthResult = strlen((const char *) pCopiedChar);
    }

    fFinishedConversion = true;
#endif // WIN32

    // If we could not do the conversion properly, then do it in
    // a more simplistic (polite way of saying "incorrect") way.
    // This is currently the only way to do this on Linux, but that
    // will hoppefully change soon.
    if (!fFinishedConversion) {
        char *pSrcPtr;
        char *pEndSrcPtr;

        // If the client ignores the error, at least give them
        // the unchanged string.
        memcpy(pResultChar, pChar, length);
        if (NULL != pActualLengthResult) {
            *pActualLengthResult = length;
        }

        // Optionally convert the case of the ASCII characters.
        // This may or may not be a good idea. It does not convert
        // non-ASCII text but we only get here if we have tried and
        // failed at doing the conversion the correct way.
        // Why penalize ASCII because we cannot do the conversion correctly?
        // Besides, ASCII is an important special case, since many special
        // HTML tags, HTTP labels, and other protocol components are
        // standard ASCII. It also leaves non-ASCII characters unchanged.
        pSrcPtr = pResultChar;
        pEndSrcPtr = pResultChar + length;
        while (pSrcPtr < pEndSrcPtr) {
            if ((*pSrcPtr >= 'a') && (*pSrcPtr <= 'z')) {
                *pSrcPtr = *pSrcPtr - ('a' - 'A');
            }
            pSrcPtr++;
        }
    } // if (!fFinishedConversion)


abort:
    return(err);
} // ConvertToUpperCase










////////////////////////////////////////////////////////////////////////////////
//
// [CTempUTF16String]
//
////////////////////////////////////////////////////////////////////////////////
CTempUTF16String::CTempUTF16String() {
    m_pWideStr = NULL;
    m_pHeapBuffer = NULL;
    m_HeapBufferLengthInWideChars = 0;
} // CTempUTF16String





////////////////////////////////////////////////////////////////////////////////
//
// [~CTempUTF16String]
//
////////////////////////////////////////////////////////////////////////////////
CTempUTF16String::~CTempUTF16String() {
    if (NULL != m_pHeapBuffer) {
        free(m_pHeapBuffer);
        m_pHeapBuffer = NULL;
        m_HeapBufferLengthInWideChars = 0;
    }
} // ~CTempUTF16String





////////////////////////////////////////////////////////////////////////////////
//
// [AllocateWideStr]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CTempUTF16String::AllocateWideStr(int32 numWideChars) {
    if (numWideChars < 0) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }
    if (0 == numWideChars) {
        m_pWideStr = NULL;
        return(ENoErr);
    }

    // Try to re-use either the heap buffer or the built-in buffer.
    if ((NULL != m_pHeapBuffer) && (numWideChars < m_HeapBufferLengthInWideChars)) {
        m_pWideStr = m_pHeapBuffer;
        return(ENoErr);
    }
    if ((numWideChars + 1) < BUILT_IN_BUFFER_SIZE_IN_WIDE_CHARS) {
        m_pWideStr = m_BuiltInBuffer;
        return(ENoErr);
    }

    // Discard the old heap buffer.
    if (NULL != m_pHeapBuffer) {
        free(m_pHeapBuffer);
        m_pHeapBuffer = NULL;
        m_HeapBufferLengthInWideChars = 0;
    }

    m_pHeapBuffer = (WCHAR *) (malloc(numWideChars * sizeof(WCHAR)));
    if (NULL == m_pHeapBuffer) {
        return(EFail);
    }

    m_HeapBufferLengthInWideChars = numWideChars;
    m_pWideStr = m_pHeapBuffer;
    return(ENoErr);
} // AllocateWideStr







////////////////////////////////////////////////////////////////////////////////
//
// [ConvertUTF8String]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CTempUTF16String::ConvertUTF8String(const char *pChar, int32 numBytes) {
    ErrVal err = ENoErr;

    if (0 == numBytes) {
        m_pWideStr = NULL;
        return(ENoErr);
    }
    if (NULL == pChar) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }
    if (numBytes < 0) {
        numBytes = strlen(pChar);
    }

    err = AllocateWideStr(numBytes + 1);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        return(err);
    }

    err = CStringLib::ConvertUTF8ToUTF16(
                        pChar,
                        numBytes,
                        m_pWideStr,
                        (numBytes + 1) * sizeof(WCHAR),
                        NULL);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
    }
    m_pWideStr[numBytes] = 0;

    return(err);
} // ConvertUTF8String










////////////////////////////////////////////////////////////////////////////////
//
// [CTempUTF8String]
//
////////////////////////////////////////////////////////////////////////////////
CTempUTF8String::CTempUTF8String() {
    m_pStr = NULL;
    m_pHeapBuffer = NULL;
    m_HeapBufferLengthInUTF8Chars = 0;
} // CTempUTF8String





////////////////////////////////////////////////////////////////////////////////
//
// [~CTempUTF8String]
//
////////////////////////////////////////////////////////////////////////////////
CTempUTF8String::~CTempUTF8String() {
    if (NULL != m_pHeapBuffer) {
        free(m_pHeapBuffer);
        m_pHeapBuffer = NULL;
        m_HeapBufferLengthInUTF8Chars = 0;
    }
} // ~CTempUTF8String





////////////////////////////////////////////////////////////////////////////////
//
// [AllocateUTF8Str]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CTempUTF8String::AllocateUTF8Str(int32 numChars) {
    if (numChars < 0) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }
    if (0 == numChars) {
        m_pStr = NULL;
        return(ENoErr);
    }

    // Try to re-use either the heap buffer or the built-in buffer.
    if ((NULL != m_pHeapBuffer) && (numChars < m_HeapBufferLengthInUTF8Chars)) {
        m_pStr = m_pHeapBuffer;
        return(ENoErr);
    }
    if ((numChars + 1) < BUILT_IN_BUFFER_SIZE_IN_UTF8_CHARS) {
        m_pStr = m_BuiltInBuffer;
        return(ENoErr);
    }

    // Discard the old heap buffer.
    if (NULL != m_pHeapBuffer) {
        free(m_pHeapBuffer);
        m_pHeapBuffer = NULL;
        m_HeapBufferLengthInUTF8Chars = 0;
    }

    m_pHeapBuffer = (char *) (malloc(numChars * sizeof(char)));
    if (NULL == m_pHeapBuffer) {
        return(EFail);
    }

    m_HeapBufferLengthInUTF8Chars = numChars;
    m_pStr = m_pHeapBuffer;
    return(ENoErr);
} // AllocateUTF8Str







////////////////////////////////////////////////////////////////////////////////
//
// [ConvertUTF16String]
//
////////////////////////////////////////////////////////////////////////////////
ErrVal
CTempUTF8String::ConvertUTF16String(const WCHAR *pWideStr, int32 numWChars) {
    ErrVal err = ENoErr;
    int32 numBytes;
    int32 actualLength;

    if (0 == numWChars) {
        m_pStr = NULL;
        return(ENoErr);
    }
    if (NULL == pWideStr) {
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
    }
    // Danger. This assumes that each UTF16 expands into no more
    // than 3 UTF8 chars. That may not be true.
    if (numWChars < 0) {
#if WIN32
        numWChars = wcslen(pWideStr);
#else
        REPORT_LOW_LEVEL_BUG();
        return(EFail);
#endif
    }
    numBytes = numWChars * 3;

    err = AllocateUTF8Str(numBytes + 1);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
        return(err);
    }

    err = CStringLib::ConvertUTF16ToUTF8(
                        pWideStr,
                        numWChars * sizeof(WCHAR),
                        m_pStr,
                        numBytes + 1,
                        &actualLength);
    if (err) {
        REPORT_LOW_LEVEL_BUG();
    }
    m_pStr[actualLength] = 0;

    return(err);
} // ConvertUTF16String



/////////////////////////////////////////////////////////////////////////////
//
// [MapStringToInteger]
//
/////////////////////////////////////////////////////////////////////////////
int32
MapStringToInteger(CStringToIntegerMap *pMap, const char *pString) { return(MapStringToIntegerEx(pMap, pString, -1)); }



/////////////////////////////////////////////////////////////////////////////
//
// [MapStringToIntegerEx]
//
/////////////////////////////////////////////////////////////////////////////
int32
MapStringToIntegerEx(CStringToIntegerMap *pMap, const char *pString, int32 strLength) {
    CStringToIntegerMap *pCurrentMapping;

    if (NULL == pMap) {
        REPORT_LOW_LEVEL_BUG();
        return(-1);
    }

    // Search starting at the beginning of the table.
    pCurrentMapping = pMap;

    // A NULL string passes to the last (default) entry.
    if (NULL == pString) {
        while ((pCurrentMapping->m_pString) && (*(pCurrentMapping->m_pString))) {
            pCurrentMapping += 1;
        }
        goto done;
    }

    if (strLength < 0) {
        strLength = strlen(pString);
    }

    while ((pCurrentMapping->m_pString) && (*(pCurrentMapping->m_pString))) {
        if ((0 == strncasecmpex(pString, pCurrentMapping->m_pString, strLength))
            && (strLength == (int32) (strlen(pCurrentMapping->m_pString)))) {
            return(pCurrentMapping->m_Int);
        }
        pCurrentMapping += 1;
    }

done:
    // Return the entry we ended up at.
    // If we didn't find anything, then return the last (default) entry.
    return(pCurrentMapping->m_Int);
} // MapStringToIntegerEx







/////////////////////////////////////////////////////////////////////////////
//
// [MapIntegerToString]
//
/////////////////////////////////////////////////////////////////////////////
const char *
MapIntegerToString(CStringToIntegerMap *pMap, int32 value) {
    CStringToIntegerMap *pCurrentMapping;

    if (NULL == pMap) {
        REPORT_LOW_LEVEL_BUG();
        return(NULL);
    }

    pCurrentMapping = pMap;
    while ((pCurrentMapping->m_pString) && (*(pCurrentMapping->m_pString))) {
        if (pCurrentMapping->m_Int == value) {
            return(pCurrentMapping->m_pString);
        }
        pCurrentMapping += 1;
    }

    // If we didn't find anything, then return the last (default) entry.
    return(NULL);
} // MapIntegerToString






/////////////////////////////////////////////////////////////////////////////
//
//                              TESTING PROCEDURES
//
/////////////////////////////////////////////////////////////////////////////
#if INCLUDE_REGRESSION_TESTS

#define NUM_VALUES      200


/////////////////////////////////////////////////////////////////////////////
//
// [TestStringLib]
//
/////////////////////////////////////////////////////////////////////////////
void
CStringLib::TestStringLib() {
    ErrVal err = ENoErr;
    int32 trialNum;
    int absoluteValue;
    int32 num;
    char *finalSrc;
    char correctStr[32];
    int32 returnedNum;

    OSIndependantLayer::PrintToConsole("Test Module: String Utilities");


    ////////////////////////////////////////////////
    OSIndependantLayer::PrintToConsole("  Test: String Comparison");

    if (0 == strcasecmpex("aBcDeF12345", "aaaaaef12345")) {
        REPORT_LOW_LEVEL_BUG();
    }
    if (0 != strcasecmpex("aBcDeF12345", "abcdef12345")) {
        REPORT_LOW_LEVEL_BUG();
    }


    ////////////////////////////////////////////////
    OSIndependantLayer::PrintToConsole("  Test: String To Number Conversion");

    absoluteValue = 0;
    num = 0;
    for (trialNum = 0; trialNum < NUM_VALUES; trialNum++) {
        snprintf(correctStr, sizeof(correctStr), "%d", num);
        finalSrc = correctStr;
        while (*finalSrc) {
            finalSrc++;
        }

        err = CStringLib::StringToNumber(correctStr, finalSrc - correctStr, &returnedNum);
        if (err) {
            REPORT_LOW_LEVEL_BUG();
        }

        if (num != returnedNum) {
            REPORT_LOW_LEVEL_BUG();
        }

        absoluteValue += 9;
        if (num < 0) {
            num = absoluteValue;
        } else {
            num = -absoluteValue;
        }
    }


    absoluteValue = 0;
    num = 0;
    for (trialNum = 0; trialNum < NUM_VALUES; trialNum++) {
        snprintf(correctStr, sizeof(correctStr), "%x", num);
        finalSrc = correctStr;
        while (*finalSrc) {
            finalSrc++;
        }

        err = CStringLib::StringToNumberEx(correctStr, strlen(correctStr), 16, &returnedNum);
        if (err) {
            REPORT_LOW_LEVEL_BUG();
        }

        if (num != returnedNum) {
            REPORT_LOW_LEVEL_BUG();
        }

        absoluteValue += 9;
        if (num < 0) {
            num = absoluteValue;
        } else {
            num = -absoluteValue;
        }
    }    

    OSIndependantLayer::PrintToConsole("\n");
} // TestStringLib.


#endif // INCLUDE_REGRESSION_TESTS




