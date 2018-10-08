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

#ifndef _POLY_HTTP_STREAM_
#define _POLY_HTTP_STREAM_

class CParsedUrl;
class CPolyHttpStream;
class CHttpServerPort;



/////////////////////////////////////////////////////////////////////////////
//
// Report events to a client. These include:
//   * The completion of an asynchronous operation
//   * An event like peerDisconnect on an open stream.
//   * Points during a request/response when the client may do something special,
//      like write the contents of a POST request or a response.
//
// This will be expanded more to include (i) more events, like reporting
// redirections, and (ii) more places where the client can extend the basic
// functionality.
//
/////////////////////////////////////////////////////////////////////////////
class CPolyHttpCallback : public CRefCountInterface {
public:
    // These report the completion of an operation.
    virtual void OnReadHTTPDocument(
                        ErrVal err,
                        CPolyHttpStream *pHTTPStream,
                        void *pCallbackContext) { err = err; pHTTPStream = pHTTPStream; pCallbackContext = pCallbackContext; }

    virtual void OnWriteHTTPDocument(
                        ErrVal err,
                        CPolyHttpStream *pHTTPStream,
                        void *pCallbackContext)
         { err = err; pHTTPStream = pHTTPStream; pCallbackContext = pCallbackContext; }


    // These are events that can happen any time, either when we
    // are making a request or between requests whenever an http 1.1
    // connection is open.
    virtual void OnPeerDisconnect(
                        ErrVal err,
                        CPolyHttpStream *pHTTPStream,
                        void *pCallbackContext) { err = err; pHTTPStream = pHTTPStream; pCallbackContext = pCallbackContext; }
    virtual void OnReconnect(
                        ErrVal err,
                        CPolyHttpStream *pHTTPStream,
                        void *pCallbackContext) { err = err; pHTTPStream = pHTTPStream; pCallbackContext = pCallbackContext; }


    // These allow the caller to perform specific parts of making a request.
    virtual ErrVal AdjustHTTPRequestHeader(
                        CPolyHttpStream *pHTTPStream,
                        CAsyncIOStream *pAsyncIOStream,
                        void *pCallbackContext) { pHTTPStream = pHTTPStream; pAsyncIOStream = pAsyncIOStream; pCallbackContext = pCallbackContext; return(ENoErr); }

    virtual ErrVal SendHTTPRequestBody(
                        CPolyHttpStream *pHTTPStream,
                        CAsyncIOStream *pAsyncIOStream,
                        void *pCallbackContext) { pHTTPStream = pHTTPStream; pAsyncIOStream = pAsyncIOStream; pCallbackContext = pCallbackContext; return(ENoErr); }
}; // CPolyHttpCallback.






/////////////////////////////////////////////////////////////////////////////
// A synchronous subclass of CPolyHttpCallback.
// This is mainly used for testing.
/////////////////////////////////////////////////////////////////////////////
class CSynchPolyHttpCallback : public CPolyHttpCallback,
                                public CRefCountImpl {
public:
    CSynchPolyHttpCallback();
    virtual ~CSynchPolyHttpCallback();
    NEWEX_IMPL()

    ErrVal Initialize();
    ErrVal Wait();

    // CPolyHttpCallback
    virtual void OnReadHTTPDocument(
                    ErrVal resultErr,
                    CPolyHttpStream *pHeader,
                    void *pContext);
    virtual void OnWriteHTTPDocument(
                    ErrVal err,
                    CPolyHttpStream *pStream,
                    void *pCallbackContext);

    // CRefCountInterface
    PASS_REFCOUNT_TO_REFCOUNTIMPL()

private:
    CRefEvent     *m_pSemaphore;
    ErrVal        m_Err;
}; // CSynchPolyHttpCallback






/////////////////////////////////////////////////////////////////////////////
//
// This is the pure virtual base class for a single stream. A stream may
// last only for the duration of a single request/response, or else it may
// be kept open and reused (in HTTP 1.1 style) for several requests and
// responses.
//
// This object is used on the client side to represent an outgoing request,
// and on the server side to represent an incoming request.
//
// Each different HTTP implementation will implement this interface.
/////////////////////////////////////////////////////////////////////////////
class CPolyHttpStream : public CRefCountInterface {
public:
    static CPolyHttpStream *AllocateSimpleStream();

#if INCLUDE_REGRESSION_TESTS
    static void TestHTTPStream();
#endif

    //////////////////////////////
    // Write Outgoing requests and receive the response.
    virtual void ReadHTTPDocument(
                        CParsedUrl *pUrl,
                        CPolyHttpCallback *pCallback,
                        void *pCallbackContext) = 0;
    virtual void SendHTTPPost(
                        CParsedUrl *pUrl,
                        CAsyncIOStream *pDocument,
                        int32 contentType,
                        int32 contentSubType,
                        int32 contentLength,
                        CPolyHttpCallback *pCallback,
                        void *pCallbackContext) = 0;

    //////////////////////////////
    // Accessor Methods
    virtual int32 GetStatusCode() = 0;
    virtual ErrVal GetContentType(int16 *pType, int16 *pSubType) = 0;
    virtual ErrVal GetIOStream(
                        CAsyncIOStream **ppSrcStream,
                        int64 *pStartPosition,
                        int32 *pLength) = 0;
    virtual CParsedUrl *GetUrl() = 0;

    virtual void CloseStreamToURL() = 0;
}; // CPolyHttpStream.


ErrVal InitializeBasicHTTPStreamGlobalState(CProductInfo *pVersion);


#endif // _POLY_HTTP_STREAM_


