#ifndef KPXCCLIENT_CLIENT_H
#define KPXCCLIENT_CLIENT_H

#include <QtCore/QScopedPointer>
#include <QtCore/QObject>
#include <QtCore/QJsonObject>
#include <QtCore/QUrl>

#include "kpxcclient_global.h"
#include "idatabaseregistry.h"
#include "entry.h"

namespace KPXCClient {

class ClientPrivate;
class KPXCCLIENT_EXPORT Client : public QObject
{
	Q_OBJECT

	Q_PROPERTY(KPXCClient::IDatabaseRegistry* databaseRegistry READ databaseRegistry WRITE setDatabaseRegistry NOTIFY databaseRegistryChanged)
	Q_PROPERTY(Options options READ options WRITE setOptions NOTIFY optionsChanged)

	Q_PROPERTY(State state READ state NOTIFY stateChanged)
	Q_PROPERTY(QByteArray currentDatabase READ currentDatabase NOTIFY currentDatabaseChanged)

public:
	enum class Option {
		None = 0x00,
		AllowNewDatabase = 0x01,
		TriggerUnlock = 0x02,
		OpenOnConnect = 0x04,
		AllowDatabaseChange = 0x08,
		DisconnectOnClose = 0x10,

		Default = (Option::AllowNewDatabase | Option::TriggerUnlock | Option::OpenOnConnect)
	};
	Q_DECLARE_FLAGS(Options, Option)
	Q_FLAG(Options)

	enum class State {
		Disconnected,
		Connecting,
		Locked,
		Unlocked
	};
	Q_ENUM(State)

	enum class Error {
		UnknownError = -1,

		// KeePassXC internal errors
		KeePassDatabaseNotOpen = 0x0001,
		KeePassDatabaseHashNotReceived = 0x0002,
		KeePassPublicKeyNotReceived = 0x0003,
		KeePassCannotDecryptMessage = 0x0004,
		KeePassTimeout = 0x0005,
		KeePassActionDenied = 0x0006,
		KeePassCannotEncryptMessage = 0x0007,
		KeePassAssociationFailed = 0x0008,
		KeePassKeyChangeFailed = 0x0009,
		KeePassEncryptionKeyUnrecognized = 0x000A,
		KeePassNoSavedDatabase = 0x000B,
		KeePassIncorrectAction = 0x000C,
		KeePassEmptyMessageReceived = 0x000D,
		KeePassNoUrlProvided = 0x000E,
		KeePassNoLoginsFound = 0x000F,

		// Client library errors
		ClientAlreadyConnected = 0x00010000,
		ClientKeyGenerationFailed = 0x00020000,
		ClientReceivedNonceInvalid = 0x00030000,
		ClientJsonParseError = 0x00040000,
		ClientUnsupportedVersion = 0x00050000,
		ClientDatabaseChanged = 0x00060000,
		ClientDatabaseRejected = 0x00070000,
		ClientUnsupportedAction = 0x00080000
	};
	Q_ENUM(Error)

	explicit Client(QObject *parent = nullptr);
	~Client() override;

	IDatabaseRegistry* databaseRegistry() const;
	Options options() const;
	State state() const;
	QByteArray currentDatabase() const;

public Q_SLOTS:
	void connectToKeePass(const QString &keePassPath = QStringLiteral("keepassxc-proxy"));
	void disconnectFromKeePass();

	void openDatabase();
	void closeDatabase();

	void generatePassword();
	void getLogins(const QUrl &url,
				   const QUrl &submitUrl = {},
				   bool httpAuth = false,
				   bool searchAllDatabases = false);
	void addLogin(const QUrl &url,
				  const Entry &entry,
				  const QUrl &submitUrl = {});

	void setDatabaseRegistry(IDatabaseRegistry* databaseRegistry);
	void setOptions(Options options);

Q_SIGNALS:
	void connected(QPrivateSignal);
	void disconnected(QPrivateSignal);

	void databaseOpened(const QByteArray &dbHash, QPrivateSignal);
	void databaseClosed(QPrivateSignal);

	void passwordsGenerated(const QStringList &passwords, QPrivateSignal);
	void loginsReceived(const QList<Entry> &entries, QPrivateSignal);
	void loginAdded(QPrivateSignal);

	void databaseRegistryChanged(IDatabaseRegistry* databaseRegistry, QPrivateSignal);
	void optionsChanged(Options options, QPrivateSignal);
	void stateChanged(QPrivateSignal);
	void currentDatabaseChanged(QByteArray currentDatabase, QPrivateSignal);
	void errorOccured(Error error, const QString &message, const QString &action, bool unrecoverable, QPrivateSignal);

protected:
	virtual bool allowDatabase(const QByteArray &databaseHash) const;

private Q_SLOTS:
	void dbConnected();
	void dbDisconnected();
	void dbError(Error code, const QString &message);
	void dbLocked();
	void dbUnlocked();
	void dbMsgRecv(const QString &action, const QJsonObject &message);
	void dbMsgFail(const QString &action, Error code, const QString &message);

private:
	friend class ClientPrivate;
	QScopedPointer<ClientPrivate> d;
};

}

Q_DECLARE_OPERATORS_FOR_FLAGS(KPXCClient::Client::Options)

#endif // KPXCCLIENT_CLIENT_H
