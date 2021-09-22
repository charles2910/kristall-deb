#ifndef GENERICPROTOCOLCLIENT_HPP
#define GENERICPROTOCOLCLIENT_HPP

#include "cryptoidentity.hpp"

#include <QObject>
#include <QAbstractSocket>

enum class RequestState : int;

class ProtocolHandler : public QObject
{
    Q_OBJECT
public:
    enum NetworkError {
        UnknownError, //!< There was an unhandled network error
        ProtocolViolation, //!< The server responded with something unexpected and violated the protocol
        HostNotFound, //!< The host was not found by the client
        ConnectionRefused, //!< The host refused connection on that port
        ResourceNotFound, //!< The requested resource was not found on the server
        BadRequest, //!< Our client misbehaved and did a request the server cannot understand
        ProxyRequest, //!< We requested a proxy operation, but the server does not allow that
        InternalServerError,
        InvalidClientCertificate,
        UntrustedHost, //!< We don't know the host, and we don't trust it
        MistrustedHost, //!< We know the host and it's not the server identity we've seen before
        Unauthorized, //!< The requested resource could not be accessed.
        TlsFailure, //!< Unspecified TLS failure
        Timeout, //!< The network connection timed out.
    };
    enum RequestOptions {
        Default = 0,
        IgnoreTlsErrors = 1,
    };
public:
    explicit ProtocolHandler(QObject *parent = nullptr);

    virtual bool supportsScheme(QString const & scheme) const = 0;

    virtual bool startRequest(QUrl const & url, RequestOptions options) = 0;

    virtual bool isInProgress() const = 0;

    virtual bool cancelRequest() = 0;

    virtual bool enableClientCertificate(CryptoIdentity const & ident);
    virtual void disableClientCertificate();
signals:
    //! We successfully transferred some bytes from the server
    void requestProgress(qint64 transferred);

    //! The request completed with the given data and mime type
    void requestComplete(QByteArray const & data, QString const & mime);

    //! The state of the request has changed
    void requestStateChange(RequestState state);

    //! Server redirected us to another URL
    void redirected(QUrl const & uri, bool is_permanent);

    //! The server needs some information from the user to process this query.
    void inputRequired(QString const & user_query, bool is_sensitive);

    //! There was an error while processing the request
    void networkError(NetworkError error, QString const & reason);

    //! The server wants us to use a client certificate
    void certificateRequired(QString const & info);

    //! The server uses TLS and has a certificate.
    void hostCertificateLoaded(QSslCertificate const & cert);
protected:
    void emitNetworkError(QAbstractSocket::SocketError error_code, QString const & textual_description);
};

#endif // GENERICPROTOCOLCLIENT_HPP
