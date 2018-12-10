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
#define QINT64MAX std::numeric_limits<qint64>::max() //still need this?

#define LEADER 1
#define CANDIDATE 2
#define FOLLOWER 3
#define HEARTBEATTIME 50 //msec

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



private:
	QTextEdit *textview;
	QLineEdit *textline;

    quint16 timeToWaitForHeartbeat;
    QTimer *electionTimer;
    QTimer *sendHeartbeatTimer;

    quint16 myCandidateId;
    quint16 myRole = FOLLOWER;
    // Used by candidate in election
    QList<quint16> nodesThatVotedForMe;

    qint16 myLeader = -1;

    quint16 myCurrentTerm = 0;
    qint16 votedFor = -1;


    // should these be calculated every time? IDK
    qint16 myLastLogIndex = 0; //I'm thinking start these at negative 1
    qint16 myLastLogTerm = 0;



    qint16 myCommitIndex = 0; // SAFE TO START AT 0? // When am I updating this???
    qint16 myLastApplied = 0;


    // Log entry should be QVariantMap that stores messageID, term, and message
    // messageID = concat(myPort, mySeqNo)
    QVariantList log;
    QVariantList queuedClientRequests;



    // After commands are committed, they are executed i.e. messages are appended
    // to the state machine
    QList<QString> stateMachine;

    // <Server, next log entry to send to that server>
    // Used by leader. initializes at, decremented when gets a false
    QMap<quint16, quint16> nextIndex;

    // <Server, highest log entry known to be replicated on server>
    // Used by leader. used to keep track of commit status
    QMap<quint16, quint16> matchIndex;



    void sendMessageToAll(QVariantMap msgMap);
    void processRequestVote(QVariantMap msg, quint16 sourcePort);
    void replyToRequestForVote(bool voteGranted, quint16 sourcePort);
    void processReplyRequestVote(QVariantMap inMap, quint16 sourcePort);


    void sendAppendEntriesMsg(quint16 destPort);

    void processAppendEntriesMsg(QVariantMap inMap, quint16 sourcePort);
    void replyToAppendEntries(bool success, quint16 destPort);
    void processAppendEntriesMsgReply(QVariantMap inMap, quint16 sourcePort);
    void updateCommitIndex();

    void attemptToCommitMsg();
    void refreshTextView();


	// OLD CODE
    quint16 myPort;
    quint32 mySeqNo = 1;
    void serializeMessage(QVariantMap &myMap, quint16 destPort);
    void deserializeMessage(QByteArray datagram);


};

#endif // P2PAPP_MAIN_HH
