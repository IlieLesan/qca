#include<qapplication.h>
#include<qtimer.h>
#include<qsocket.h>
#include<qserversocket.h>
#include<stdio.h>
#include<unistd.h>
#include"base64.h"
#include"qca.h"

#define PROTO_NAME "foo"
#define PROTO_PORT 8001

static QString prompt(const QString &s)
{
	printf("* %s ", s.latin1());
	fflush(stdout);
	char line[256];
	fgets(line, 255, stdin);
	QString result = line;
	if(result[result.length()-1] == '\n')
		result.truncate(result.length()-1);
	return result;
}

class ClientTest : public QObject
{
	Q_OBJECT
public:
	ClientTest()
	{
		sock = new QSocket;
		connect(sock, SIGNAL(connected()), SLOT(sock_connected()));
		connect(sock, SIGNAL(connectionClosed()), SLOT(sock_connectionClosed()));
		connect(sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));
		connect(sock, SIGNAL(error(int)), SLOT(sock_error(int)));

		sasl = new QCA::SASL;
		connect(sasl, SIGNAL(clientFirstStep(const QString &, const QByteArray *)), SLOT(sasl_clientFirstStep(const QString &, const QByteArray *)));
		connect(sasl, SIGNAL(nextStep(const QByteArray &)), SLOT(sasl_nextStep(const QByteArray &)));
		connect(sasl, SIGNAL(needParams(bool, bool, bool, bool)), SLOT(sasl_needParams(bool, bool, bool, bool)));
		connect(sasl, SIGNAL(authenticated(bool)), SLOT(sasl_authenticated(bool)));
	}

	~ClientTest()
	{
		delete sock;
		delete sasl;
	}

	void start(const QString &_host, int port, const QString &user="", const QString &pass="")
	{
		mode = 0;
		host = _host;
		sock->connectToHost(host, port);

		if(!user.isEmpty()) {
			sasl->setUsername(user);
			sasl->setAuthname(user);
		}
		if(!pass.isEmpty())
			sasl->setPassword(pass);
	}

signals:
	void quit();

private slots:
	void sock_connected()
	{
		printf("Connected to server.  Awaiting mechanism list...\n");
	}

	void sock_connectionClosed()
	{
		printf("Connection closed by peer.\n");
		quit();
	}

	void sock_error(int x)
	{
		QString s;
		if(x == QSocket::ErrConnectionRefused)
			s = "connection refused / timed out";
		else if(x == QSocket::ErrHostNotFound)
			s = "host not found";
		else if(x == QSocket::ErrSocketRead)
			s = "read error";

		printf("Socket error: %s\n", s.latin1());
		quit();
	}

	void sock_readyRead()
	{
		if(sock->canReadLine()) {
			QString line = sock->readLine();
			line.truncate(line.length()-1); // chop the newline
			handleLine(line);
		}
	}

	void sasl_clientFirstStep(const QString &mech, const QByteArray *clientInit)
	{
		printf("Choosing mech: %s\n", mech.latin1());
		QString line = mech;
		if(clientInit) {
			QCString cs(clientInit->data(), clientInit->size()+1);
			line += ' ';
			line += cs;
		}
		sendLine(line);
	}

	void sasl_nextStep(const QByteArray &stepData)
	{
		QCString cs(stepData.data(), stepData.size()+1);
		QString line = "C";
		if(!stepData.isEmpty()) {
			line += ',';
			line += cs;
		}
		sendLine(line);
	}

	void sasl_needParams(bool auth, bool user, bool pass, bool realm)
	{
		QString username = prompt("Username:");
		if(user) {
			sasl->setUsername(username);
		}
		if(auth) {
			sasl->setAuthname(username);
		}
		if(pass) {
			sasl->setPassword(prompt("Password (not hidden!) :"));
		}
		if(realm) {
			sasl->setRealm(prompt("Realm:"));
		}
		sasl->continueAfterParams();
	}

	void sasl_authenticated(bool ok)
	{
		printf("SASL %s!\n", ok ? "success" : "failed");
		if(!ok)
			quit();
	}

private:
	QSocket *sock;
	QCA::SASL *sasl;
	int mode;
	QString host;

	void handleLine(const QString &line)
	{
		printf("Reading: [%s]\n", line.latin1());
		if(mode == 0) {
			// first line is the method list
			QStringList mechlist = QStringList::split(' ', line);
			++mode;

			// kick off the client
			if(!sasl->startClient(PROTO_NAME, host, mechlist)) {
				printf("Error starting client!\n");
				quit();
			}
		}
		else if(mode == 1) {
			QString type, rest;
			int n = line.find(',');
			if(n != -1) {
				type = line.mid(0, n);
				rest = line.mid(n+1);
			}
			else {
				type = line;
				rest = "";
			}

			if(type == "C") {
				QCString cs = rest.latin1();
				QByteArray buf(cs.length());
				memcpy(buf.data(), cs.data(), buf.size());
				sasl->putStep(buf);
			}
			else if(type == "E") {
				printf("Authentication failed.\n");
				quit();
				return;
			}
			else if(type == "A") {
				printf("Authentication success.\n");
				return;
			}
			else {
				printf("Bad format from peer, closing.\n");
				quit();
				return;
			}
		}
	}

	void sendLine(const QString &line)
	{
		printf("Writing: {%s}\n", line.latin1());
		QString s = line + '\n';
		QCString cs = s.latin1();
		sock->writeBlock(cs.data(), cs.length());
	}
};

class ServerTest : public QServerSocket
{
	Q_OBJECT
public:
	ServerTest(int _port) : QServerSocket(_port), port(_port)
	{
		sock = 0;
		sasl = 0;
		realm = QString::null;
	}

	~ServerTest()
	{
		delete sock;
		delete sasl;
	}

	void start()
	{
		if(!ok()) {
			printf("Error binding to port %d!\n", port);
			QTimer::singleShot(0, this, SIGNAL(quit()));
			return;
		}
		char myhostname[256];
		int r = gethostname(myhostname, sizeof(myhostname)-1);
		if(r == -1) {
			printf("Error getting hostname!\n");
			QTimer::singleShot(0, this, SIGNAL(quit()));
			return;
		}
		host = myhostname;
		printf("Listening on %s:%d ...\n", host.latin1(), port);
	}

	void newConnection(int s)
	{
		// Note: only 1 connection supported at a time in this example!
		if(sock) {
			QSocket tmp;
			tmp.setSocket(s);
			printf("Connection ignored, already have one active.\n");
			return;
		}

		printf("Connection received!  Starting SASL handshake...\n");

		sock = new QSocket;
		connect(sock, SIGNAL(connectionClosed()), SLOT(sock_connectionClosed()));
		connect(sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));
		connect(sock, SIGNAL(error(int)), SLOT(sock_error(int)));

		sasl = new QCA::SASL;
		connect(sasl, SIGNAL(nextStep(const QByteArray &)), SLOT(sasl_nextStep(const QByteArray &)));
		connect(sasl, SIGNAL(authenticated(bool)), SLOT(sasl_authenticated(bool)));

		sock->setSocket(s);
		mode = 0;
		QStringList mechlist;
		if(!sasl->startServer(PROTO_NAME, host, realm, &mechlist)) {
			printf("Error starting server!\n");
			quit();
		}
		QString str;
		bool first = true;
		for(QStringList::ConstIterator it = mechlist.begin(); it != mechlist.end(); ++it) {
			if(!first)
				str += ' ';
			str += *it;
			first = false;
		}
		sendLine(str);
	}

signals:
	void quit();

private slots:
	void sock_connectionClosed()
	{
		printf("Connection closed by peer.\n");
		close();
	}

	void sock_error(int x)
	{
		QString s;
		if(x == QSocket::ErrConnectionRefused)
			s = "connection refused / timed out";
		else if(x == QSocket::ErrHostNotFound)
			s = "host not found";
		else if(x == QSocket::ErrSocketRead)
			s = "read error";

		printf("Socket error: %s\n", s.latin1());
		close();
	}

	void sock_readyRead()
	{
		if(sock->canReadLine()) {
			QString line = sock->readLine();
			line.truncate(line.length()-1); // chop the newline
			handleLine(line);
		}
	}

	void sasl_nextStep(const QByteArray &stepData)
	{
		QCString cs(stepData.data(), stepData.size()+1);
		QString line = "C";
		if(!stepData.isEmpty()) {
			line += ',';
			line += cs;
		}
		sendLine(line);
	}

	void sasl_authenticated(bool ok)
	{
		if(ok)
			sendLine("A");
		else
			sendLine("E");
		printf("Authentication %s.\n", ok ? "success" : "failed");
	}

private:
	QSocket *sock;
	QCA::SASL *sasl;
	QString host, realm;
	int port;
	int mode;

	void handleLine(const QString &line)
	{
		printf("Reading: [%s]\n", line.latin1());
		if(mode == 0) {
			int n = line.find(' ');
			if(n != -1) {
				QString mech = line.mid(0, n);
				QCString cs = line.mid(n+1).latin1();
				QByteArray clientInit(cs.length());
				memcpy(clientInit.data(), cs.data(), clientInit.size());
				sasl->putServerFirstStep(mech, clientInit);
			}
			else
				sasl->putServerFirstStep(line);
			++mode;
		}
		else if(mode == 1) {
			QString type, rest;
			int n = line.find(',');
			if(n != -1) {
				type = line.mid(0, n);
				rest = line.mid(n+1);
			}
			else {
				type = line;
				rest = "";
			}

			if(type == "C") {
				QCString cs = rest.latin1();
				QByteArray buf(cs.length());
				memcpy(buf.data(), cs.data(), buf.size());
				sasl->putStep(buf);
			}
			else {
				printf("Bad format from peer, closing.\n");
				close();
				return;
			}
		}
	}

	void sendLine(const QString &line)
	{
		printf("Writing: {%s}\n", line.latin1());
		QString s = line + '\n';
		QCString cs = s.latin1();
		sock->writeBlock(cs.data(), cs.length());
	}

	void close()
	{
		sock->deleteLater();
		sock = 0;
		delete sasl;
		sasl = 0;
	}
};

#include"sasltest.moc"

void usage()
{
	printf("usage: sasltest client [host] [user] [pass]\n");
	printf("       sasltest server\n\n");
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv, false);

	QString host, user, pass;
	bool server;
	if(argc < 2) {
		usage();
		return 0;
	}
	QString arg = argv[1];
	if(arg == "client") {
		if(argc < 3) {
			usage();
			return 0;
		}
		host = argv[2];
		if(argc >= 4)
			user = argv[3];
		if(argc >= 5)
			pass = argv[4];
		server = false;
	}
	else if(arg == "server") {
		server = true;
	}
	else {
		usage();
		return 0;
	}

	if(!QCA::isSupported(QCA::CAP_SASL)) {
		printf("SASL not supported!\n");
		return 1;
	}

	if(server) {
		ServerTest *s = new ServerTest(PROTO_PORT);
		QObject::connect(s, SIGNAL(quit()), &app, SLOT(quit()));
		s->start();
		app.exec();
		delete s;
	}
	else {
		ClientTest *c = new ClientTest;
		QObject::connect(c, SIGNAL(quit()), &app, SLOT(quit()));
		c->start(host, PROTO_PORT, user, pass);
		app.exec();
		delete c;
	}

	return 0;
}
