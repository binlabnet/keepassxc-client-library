#include "connector_p.h"
#include <QtCore/QDebug>
#include <QtCore/QJsonDocument>
#include <QtCore/QStandardPaths>
#include <chrono>
using namespace KPXCClient;

const QVersionNumber Connector::minimumKeePassXCVersion{2, 3, 0};

Connector::Connector(QObject *parent) :
	QObject{parent},
	_cryptor{new SodiumCryptor{this}},
	_disconnectTimer{new QTimer{this}}
{
	using namespace std::chrono_literals;
	_disconnectTimer->setInterval(500ms);
	_disconnectTimer->setSingleShot(true);
	_disconnectTimer->setTimerType(Qt::CoarseTimer);
	connect(_disconnectTimer, &QTimer::timeout,
			this, &Connector::disconnectFromKeePass);
}

bool Connector::isConnected() const
{
	return _connectPhase == PhaseConnected;
}

bool Connector::isConnecting() const
{
	return _connectPhase == PhaseConnecting;
}

SodiumCryptor *Connector::cryptor() const
{
	return _cryptor;
}

void Connector::connectToKeePass(const QString &target)
{
	if(_process) {
		emit error(Client::Error::ClientAlreadyConnected);
		return;
	}

	if(!_cryptor->createKeys()){
		emit error(Client::Error::ClientKeyGenerationFailed);
		return;
	}
	_serverKey.deallocate();
	_clientId = _cryptor->generateRandomNonce(SecureByteArray::State::Readonly);

	_process = new QProcess{this};
	_process->setProgram(target);
	connect(_process, &QProcess::started,
			this, &Connector::started);
	connect(_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
			this, &Connector::finished);
	connect(_process, &QProcess::errorOccurred,
			this, &Connector::procError);
	connect(_process, &QProcess::readyReadStandardOutput,
			this, &Connector::stdOutReady);
	connect(_process, &QProcess::readyReadStandardError,
			this, &Connector::stdErrReady);

	_connectPhase = PhaseConnecting;
	_process->start();
}

void Connector::disconnectFromKeePass()
{
	if(!_process)
		return;

	switch(_connectPhase) {
	case PhaseConnected:
		qDebug() << "Disconnect Phase: Sending EOF";
		_connectPhase = PhaseEof;
		_process->closeWriteChannel();
		_disconnectTimer->start();
		break;
	case PhaseConnecting:
	case PhaseEof:
		qDebug() << "Disconnect Phase: Sending SIGTERM";
		_connectPhase = PhaseTerminate;
		_process->terminate();
		_disconnectTimer->start();
		break;
	case PhaseTerminate:
		qDebug() << "Disconnect Phase: Sending SIGKILL";
		_connectPhase = PhaseKill;
		_process->kill();
		_disconnectTimer->start();
		break;
	case PhaseKill:
		qDebug() << "Disconnect Phase: Dropping process";
		cleanup();
		break;
	default:
		Q_UNREACHABLE();
		break;
	}
}

void Connector::sendEncrypted(const QString &action, QJsonObject message, bool triggerUnlock)
{
	auto nonce = _cryptor->generateRandomNonce();
	message[QStringLiteral("action")] = action;
#ifdef KPXCCLIENT_MSG_DEBUG
	qDebug() << "[[SEND PLAIN MESSAGE]]" << message;
#endif
	auto encData = _cryptor->encrypt(QJsonDocument{message}.toJson(QJsonDocument::Compact),
									 _serverKey,
									 nonce);

	QJsonObject msgData;
	msgData[QStringLiteral("action")] = action;
	msgData[QStringLiteral("message")] = QString::fromUtf8(encData.toBase64());
	msgData[QStringLiteral("nonce")] = nonce.toBase64();
	msgData[QStringLiteral("clientID")] = _clientId.toBase64();
	msgData[QStringLiteral("triggerUnlock")] = QVariant{triggerUnlock}.toString();

	nonce.increment();
	nonce.makeReadonly();
	_allowedNonces.insert(nonce);

	sendMessage(msgData);
}

void Connector::started()
{
	_connectPhase = PhaseConnected;
	auto nonce = _cryptor->generateRandomNonce();
	QJsonObject keysMessage;
	keysMessage[QStringLiteral("action")] = QStringLiteral("change-public-keys");
	keysMessage[QStringLiteral("publicKey")] = _cryptor->publicKey().toBase64();
	keysMessage[QStringLiteral("nonce")] = nonce.toBase64();
	keysMessage[QStringLiteral("clientID")] = _clientId.toBase64();

	nonce.increment();
	nonce.makeReadonly();
	_allowedNonces.insert(nonce);

	sendMessage(keysMessage);
}

void Connector::finished(int exitCode, QProcess::ExitStatus exitStatus)
{
	qInfo() << "Connection closed with code:" << exitCode
			<< "(Status:" << exitStatus << ")";
	cleanup();
	emit disconnected();
}

void Connector::procError(QProcess::ProcessError error)
{
	qCritical() << error << _process->errorString();
}

void Connector::stdOutReady()
{
	// read the message
	const auto encMessage = readMessageData();
#ifdef KPXCCLIENT_MSG_DEBUG
	qDebug() << "[[RECEIVE RAW MESSAGE]]" << encMessage;
#endif
	if(encMessage.isEmpty())
		return;

	// verify message
	const auto action = encMessage[QStringLiteral("action")].toString();
	if(!performChecks(action, encMessage))
		return;

	// handle special messages
	if(action == QStringLiteral("change-public-keys")) {
		handleChangePublicKeys(encMessage[QStringLiteral("publicKey")].toString());
		return;
	} else if(action == QStringLiteral("database-locked")) {
		emit locked();
		return;
	} else if(action == QStringLiteral("database-unlocked")) {
		emit unlocked();
		return;
	}

	//verify nonce
	const auto kpNonce = SecureByteArray::fromBase64(encMessage[QStringLiteral("nonce")].toString(), SecureByteArray::State::Readonly);
	if(!_allowedNonces.remove(kpNonce)) {
		emit messageFailed(action, Client::Error::ClientReceivedNonceInvalid);
		return;
	}

	// decrypt message
	const auto plainData = _cryptor->decrypt(QByteArray::fromBase64(encMessage[QStringLiteral("message")].toString().toUtf8()),
											 _serverKey,
											 kpNonce);
	QJsonParseError error;
	const auto message = QJsonDocument::fromJson(plainData, &error).object();
	if(error.error != QJsonParseError::NoError){
		emit messageFailed(action, Client::Error::ClientJsonParseError, error.errorString());
		return;
	}
#ifdef KPXCCLIENT_MSG_DEBUG
	qDebug() << "[[RECEIVE PLAIN MESSAGE]]" << message;
#endif

	// check for success
	if(!performChecks(action, message))
		return;
	emit messageReceived(action, message);
}

void Connector::stdErrReady()
{
	qWarning() << "stderr" << _process->readAllStandardError();
}

void Connector::sendMessage(const QJsonObject &message)
{
#ifdef KPXCCLIENT_MSG_DEBUG
	qDebug() << "[[SEND RAW MESSAGE]]" << message;
#endif
	const auto data = QJsonDocument{message}.toJson(QJsonDocument::Compact);
	QByteArray length{sizeof(quint32), 0};
	*reinterpret_cast<quint32*>(length.data()) = data.size();
	_process->write(length + data);
}

void Connector::cleanup()
{
	_disconnectTimer->stop();
	if(_process) {
		_process->disconnect(this);
		_process->deleteLater();
		_process = nullptr;
	}
	_cryptor->dropKeys();
	_serverKey.deallocate();
	_clientId.deallocate();
	_allowedNonces.clear();
	_connectPhase = PhaseKill;
}

QJsonObject Connector::readMessageData()
{
	// read the data
	const auto sizeData = _process->peek(sizeof(quint32));
	if(sizeData.size() != sizeof(quint32))
		return {};

	const auto size = *reinterpret_cast<const quint32*>(sizeData.constData());
	if(_process->bytesAvailable() < static_cast<qint64>(size + sizeof(quint32)))
		return {};

	_process->skip(sizeof(quint32));
	const auto data = _process->read(size);
	Q_ASSERT(data.size() == static_cast<int>(size));

	// parse json
	QJsonParseError error;
	const auto message = QJsonDocument::fromJson(data, &error).object();
	if(error.error != QJsonParseError::NoError) {
		emit this->error(Client::Error::ClientJsonParseError, error.errorString());
		return {};
	} else
		return message;
}

bool Connector::performChecks(const QString &action, const QJsonObject &message)
{
	// verify version
	if(message.contains(QStringLiteral("version"))) {
		const auto kpVersion = QVersionNumber::fromString(message[QStringLiteral("version")].toString());
		if(kpVersion < minimumKeePassXCVersion) {
			messageFailed(action, Client::Error::ClientUnsupportedVersion, kpVersion.toString());
			return false;
		}
	}

	// read success status and cancel early if error
	auto success = false;
	if(message.contains(QStringLiteral("success")))
		success = message[QStringLiteral("success")].toVariant().toBool();
	else {
		success = !message.contains(QStringLiteral("errorCode")) &&
				  !message.contains(QStringLiteral("error"));
	}
	if(!success) {
		emit messageFailed(action,
						   static_cast<Client::Error>(message[QStringLiteral("errorCode")].toVariant().toInt()),
						   message[QStringLiteral("error")].toString());
		return false;
	}

	return true;
}

void Connector::handleChangePublicKeys(const QString &publicKey)
{
	_serverKey = SecureByteArray::fromBase64(publicKey, SecureByteArray::State::Readonly);
	emit connected();
}
