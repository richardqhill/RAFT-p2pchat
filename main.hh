#ifndef P2PAPP_MAIN_HH
#define P2PAPP_MAIN_HH

#include <QDialog>
#include <QTextEdit>
#include <QLineEdit>
#include <QUdpSocket>
#include <QVBoxLayout>
#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QDataStream>
#include <QTimer>
#include <QTime>
#include <QElapsedTimer>
#include <unistd.h>
#include <stdlib.h>
#include <limits>

// We start Seq No at 1 because an empty entry in QVariantMap returns 0
#define SEQNOSTART 1
#define QINT64MAX std::numeric_limits<qint64>::max()

#define LEADER 1
#define CANDIDATE 2
#define FOLLOWER 3
#define HEARTBEATTIME 50

class NetSocket : public QUdpSocket
{
    Q_OBJECT

public:
    NetSocket();
    bool bind(); // Bind this socket to a P2Papp-specific default port.
    quint16 myPortMin, myPortMax, port;
};

class ChatDialog : public QDialog
{
	Q_OBJECT

public:
    ChatDialog();
    NetSocket* mySocket;

public slots:
	void gotReturnPressed();
    void processPendingDatagrams();
    void sendRequestForVotes();
    void sendHeartbeat();

    // OLD
    void antiEntropy();
    void resendRumor();

private:
	QTextEdit *textview;
	QLineEdit *textline;

    quint16 myCandidateId;
    quint16 myRole = FOLLOWER;
    qint16 myLeader = -1;
    qint16 votedFor = -1;


    quint16 timeToWaitForHeartbeat;
	QTimer *waitForHeartbeatTimer;
    QTimer *sendHeartbeatTimer;

    quint16 lastLogIndex=0;
    quint16 lastLogTerm=0;
    quint16 commitIndex=0; // SAFE TO START AT 0? // When am I updating this???
    quint16 currentTerm=0;

    QList<quint16> nodesThatVotedForMe;

    // <Server, next log entry to send to that server>
    QMap<quint16, quint16> nextIndex;

    // <Server, highest log entry known to be replicated on server>
    QMap<quint16, quint16> matchIndex;




    // List of (message, (messageID, term)) pairs
    QList<QPair<QString, QPair<QString, quint16>>> log;
    QList<QString> stateMachine;
    QList<QPair<QString, QPair<QString, quint16>>> queuedClientRequests;





    QList<QPair<QString, quint16>> committedMsgs; //commands
    QList<QPair<QString, quint16>> uncommittedMsgs;


    // add var to store chat (state machine)

    // List of (origin port + seqno, message) pairs
    //QList<QPair<QString, QString>> clientMsgs;


    void sendMessageToAll(QVariantMap msgMap);
    void processRequestVote(QVariantMap msg, quint16 sourcePort);
    void replyToRequestForVote(bool voteGranted, quint16 sourcePort);

    void processReplyRequestVote(QVariantMap inMap, quint16 sourcePort);

    void sendAppendEntriesMsg(quint16 prevLogIndex, quint16 destPort);
    void processAppendEntriesMsg(QVariantMap inMap, quint16 sourcePort);
    void processAppendEntriesMsgReply(QVariantMap inMap, quint16 sourcePort);


    void replyToAppendEntries(bool success, quint16 sourcePort);

    void attemptToCommitMsg();
    void refreshTextView();


	// OLD CODE
    quint16 myPort;
    quint32 mySeqNo;


    QTimer *resendTimer;
    QTimer *antiEntropyTimer;
    QElapsedTimer *n1Timer = nullptr;
    QElapsedTimer *n2Timer = nullptr;
    qint64 n1Time = QINT64MAX;
    qint64 n2Time = QINT64MAX;

    QString myOrigin;

    // chatLogs: <Origin, <SeqNo, Message>>
    QMap<QString, QMap<quint32, QString>> chatLogs;

    // statusMap: <Origin, QVariant(LastSeqNo + 1)>
    QVariantMap statusMap;

    // Last sent rumor message
    quint16 lastRumorPort;
    QString lastRumorOrigin;
    quint32 lastRumorSeqNo;

    quint16 pickClosestNeighbor();
    quint16 pickRandomNeighbor();
    void sendRumorMessage(QString origin, quint32 seqNo, quint16 destPort);
    void serializeMessage(QVariantMap &myMap, quint16 destPort);
    void deserializeMessage(QByteArray datagram);
    void receiveRumorMessage(QVariantMap inMap, quint16 sourcePort);
    void sendStatusMessage(quint16 destPort);
    void receiveStatusMessage(QVariantMap inMap, quint16 sourcePort);

};

#endif // P2PAPP_MAIN_HH
