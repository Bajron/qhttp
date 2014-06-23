/*
 * Copyright 2011-2014 Nikhil Marathe <nsm.nikhil@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
///////////////////////////////////////////////////////////////////////////////

#include "qhttpconnection.hpp"
#include "http-parser/http_parser.h"

#include "qhttprequest.hpp"
#include "qhttpresponse.hpp"

#include "private/qhttprequest_private.hpp"
#include "private/qhttpresponse_private.hpp"

#include <QTcpSocket>
#include <QHostAddress>

///////////////////////////////////////////////////////////////////////////////
class QHttpConnection::Private
{
public: // callback functions for http_parser_settings
    static int messageBegin(http_parser *parser);
    static int url(http_parser *parser, const char *at, size_t length);
    static int headerField(http_parser *parser, const char *at, size_t length);
    static int headerValue(http_parser *parser, const char *at, size_t length);
    static int headersComplete(http_parser *parser);
    static int body(http_parser *parser, const char *at, size_t length);
    static int messageComplete(http_parser *parser);

public:
    static QUrl createUrl(const char *urlData, const http_parser_url &urlInfo);
};

///////////////////////////////////////////////////////////////////////////////

QHttpConnection::QHttpConnection(qintptr handle, QObject *parent)
    : QObject(parent),
      m_socket(0),
      m_parser(0),
      m_parserSettings(0),
      m_request(0),
      m_transmitLen(0),
      m_transmitPos(0) {
    m_parser = (http_parser *)malloc(sizeof(http_parser));
    http_parser_init(m_parser, HTTP_REQUEST);

    m_parserSettings = new http_parser_settings();
    m_parserSettings->on_message_begin    = Private::messageBegin;
    m_parserSettings->on_url              = Private::url;
    m_parserSettings->on_header_field     = Private::headerField;
    m_parserSettings->on_header_value     = Private::headerValue;
    m_parserSettings->on_headers_complete = Private::headersComplete;
    m_parserSettings->on_body             = Private::body;
    m_parserSettings->on_message_complete = Private::messageComplete;

    m_parser->data = this;

    m_socket        = new QTcpSocket(this);
    m_socket->setSocketDescriptor(handle);

    connect(m_socket,   SIGNAL(readyRead()),
            this,       SLOT(parseRequest())
            );
    connect(m_socket,   SIGNAL(disconnected()),
            this,       SLOT(socketDisconnected())
            );
    connect(m_socket,   SIGNAL(bytesWritten(qint64)),
            this,       SLOT(updateWriteCount(qint64))
            );

#if QHTTPSERVER_MEMORY_LOG > 0
    fprintf(stderr, "%s:%s(%d): obj = %p\n", __FILE__, __FUNCTION__, __LINE__, this);
#endif
}

QHttpConnection::~QHttpConnection() {
    if ( m_parser != 0 ) {
        free(m_parser);
        m_parser = 0;
    }

    if ( m_parserSettings != 0 ) {
        delete m_parserSettings;
        m_parserSettings = 0;
    }

#if QHTTPSERVER_MEMORY_LOG > 0
    fprintf(stderr, "%s:%s(%d): obj = %p\n", __FILE__, __FUNCTION__, __LINE__, this);
#endif
}

void
QHttpConnection::socketDisconnected() {
    m_socket->deleteLater(); // safely delete m_socket
    deleteLater();
}

void
QHttpConnection::updateWriteCount(qint64 count) {
    Q_ASSERT(m_transmitPos + count <= m_transmitLen);

    m_transmitPos += count;

    if (m_transmitPos == m_transmitLen) {
        m_transmitLen = 0;
        m_transmitPos = 0;
        emit allBytesWritten();
    }
}

void
QHttpConnection::parseRequest() {
    Q_ASSERT(m_parser);

    while (m_socket->bytesAvailable()) {
        QByteArray arr = m_socket->readAll();
        http_parser_execute(m_parser, m_parserSettings, arr.constData(), arr.size());
    }
}

void
QHttpConnection::write(const QByteArray &data) {
    m_socket->write(data);
    m_transmitLen += data.size();
}

void
QHttpConnection::flush() {
    m_socket->flush();
}

void
QHttpConnection::responseDone() {
    QHttpResponse *response = qobject_cast<QHttpResponse *>(QObject::sender());
    if (response->pimp->m_last)
        m_socket->disconnectFromHost();
}


/********************
 * Static Callbacks *
 *******************/

int
QHttpConnection::Private::messageBegin(http_parser *parser) {
    QHttpConnection *theConnection = static_cast<QHttpConnection *>(parser->data);
    theConnection->m_currentHeaders.clear();
    theConnection->m_currentUrl.clear();
    theConnection->m_currentUrl.reserve(128);

    theConnection->m_request = new QHttpRequest(theConnection);
    return 0;
}

int
QHttpConnection::Private::headersComplete(http_parser *parser) {
    QHttpConnection *theConnection = static_cast<QHttpConnection *>(parser->data);
    Q_ASSERT(theConnection->m_request);

    // set method
    theConnection->m_request->pimp->m_method =
            static_cast<QHttpRequest::HttpMethod>(parser->method);

    // set version
    theConnection->m_request->pimp->m_version = QString("%1.%2")
                                                .arg(parser->http_major)
                                                .arg(parser->http_minor);

    // get parsed url
    struct http_parser_url urlInfo;
    int r = http_parser_parse_url(theConnection->m_currentUrl.constData(),
                                  theConnection->m_currentUrl.size(),
                                  parser->method == HTTP_CONNECT,
                                  &urlInfo);
    Q_ASSERT(r == 0);
    Q_UNUSED(r);

    theConnection->m_request->pimp->m_url = createUrl(
                                                theConnection->m_currentUrl.constData(),
                                                urlInfo
                                                );

    // Insert last remaining header
    theConnection->m_currentHeaders.insert(
                theConnection->m_currentHeaderField.toLower(),
                theConnection->m_currentHeaderValue.toLower()
                );
    theConnection->m_request->pimp->m_headers = theConnection->m_currentHeaders;

    // set client information
    theConnection->m_request->pimp->m_remoteAddress = theConnection->m_socket->peerAddress().toString();
    theConnection->m_request->pimp->m_remotePort    = theConnection->m_socket->peerPort();

    QHttpResponse *response = new QHttpResponse(theConnection);

    if (  parser->http_major < 1 ||
          parser->http_minor < 1 ||
          theConnection->m_currentHeaders.value("connection", "") == "close" ) {

        response->pimp->m_keepAlive = false;
        response->pimp->m_last      = true;
    }

    QObject::connect(theConnection,  &QHttpConnection::destroyed,
                     response,       &QHttpResponse::connectionClosed
                     );
    QObject::connect(response,       &QHttpResponse::done,
                     theConnection,  &QHttpConnection::responseDone);

    // we are good to go!
    emit theConnection->newRequest(theConnection->m_request, response);
    return 0;
}

int
QHttpConnection::Private::messageComplete(http_parser *parser) {
    // TODO: do cleanup and prepare for next request
    QHttpConnection *theConnection = static_cast<QHttpConnection *>(parser->data);
    Q_ASSERT(theConnection->m_request);

    theConnection->m_request->pimp->m_success = true;
    emit theConnection->m_request->end();
    return 0;
}

int
QHttpConnection::Private::url(http_parser *parser, const char *at, size_t length) {
    QHttpConnection *theConnection = static_cast<QHttpConnection *>(parser->data);
    Q_ASSERT(theConnection->m_request);

    theConnection->m_currentUrl.append(at, length);
    return 0;
}

int
QHttpConnection::Private::headerField(http_parser *parser, const char *at, size_t length) {
    QHttpConnection *theConnection = static_cast<QHttpConnection *>(parser->data);
    Q_ASSERT(theConnection->m_request);

    // insert the header we parsed previously
    // into the header map
    if (!theConnection->m_currentHeaderField.isEmpty() &&
        !theConnection->m_currentHeaderValue.isEmpty()) {
        // header names are always lower-cased
        theConnection->m_currentHeaders.insert(
                    theConnection->m_currentHeaderField.toLower(),
                    theConnection->m_currentHeaderValue.toLower()
                    );
        // clear header value. this sets up a nice
        // feedback loop where the next time
        // HeaderValue is called, it can simply append
        theConnection->m_currentHeaderField.clear();
        theConnection->m_currentHeaderValue.clear();
    }

    theConnection->m_currentHeaderField.append(at, length);
    return 0;
}

int
QHttpConnection::Private::headerValue(http_parser *parser, const char *at, size_t length) {
    QHttpConnection *theConnection = static_cast<QHttpConnection *>(parser->data);
    Q_ASSERT(theConnection->m_request);

    theConnection->m_currentHeaderValue.append(at, length);
    return 0;
}

int
QHttpConnection::Private::body(http_parser *parser, const char *at, size_t length) {
    QHttpConnection *theConnection = static_cast<QHttpConnection *>(parser->data);
    Q_ASSERT(theConnection->m_request);

    emit theConnection->m_request->data(QByteArray(at, length));
    return 0;
}


/* URL Utilities */
#define HAS_URL_FIELD(info, field) (info.field_set &(1 << (field)))

#define GET_FIELD(data, info, field)                                                               \
    QString::fromLatin1(data + info.field_data[field].off, info.field_data[field].len)

#define CHECK_AND_GET_FIELD(data, info, field)                                                     \
    (HAS_URL_FIELD(info, field) ? GET_FIELD(data, info, field) : QString())

QUrl
QHttpConnection::Private::createUrl(const char *urlData, const http_parser_url &urlInfo) {
    QUrl url;
    url.setScheme(CHECK_AND_GET_FIELD(urlData, urlInfo, UF_SCHEMA));
    url.setHost(CHECK_AND_GET_FIELD(urlData, urlInfo, UF_HOST));
    // Port is dealt with separately since it is available as an integer.
    url.setPath(CHECK_AND_GET_FIELD(urlData, urlInfo, UF_PATH));
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
    url.setQuery(CHECK_AND_GET_FIELD(urlData, urlInfo, UF_QUERY));
#else
    if (HAS_URL_FIELD(urlInfo, UF_QUERY)) {
        url.setEncodedQuery(QByteArray(urlData + urlInfo.field_data[UF_QUERY].off,
                                       urlInfo.field_data[UF_QUERY].len));
    }
#endif
    url.setFragment(CHECK_AND_GET_FIELD(urlData, urlInfo, UF_FRAGMENT));
    url.setUserInfo(CHECK_AND_GET_FIELD(urlData, urlInfo, UF_USERINFO));

    if (HAS_URL_FIELD(urlInfo, UF_PORT))
        url.setPort(urlInfo.port);

    return url;
}

#undef CHECK_AND_SET_FIELD
#undef GET_FIELD
#undef HAS_URL_FIELD

///////////////////////////////////////////////////////////////////////////////

