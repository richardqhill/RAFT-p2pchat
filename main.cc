#include "main.hh"

ChatDialog::ChatDialog(){

    mySocket = new NetSocket();
    if(!  mySocket->bind()) {
		exit(1);
	}
    else {
		myPort = mySocket->port;
	}

    myCandidateId = myPort;
	QString myPortString = QString::number(myPort);

	qDebug() << "myPort: " << myPortString;
	qDebug() << "-------------------";

    setWindowTitle("RAFT Chat: " + myPortString);

	textview = new QTextEdit(this);
	textview->setReadOnly(true);
	textline = new QLineEdit(this);
	QVBoxLayout *layout = new QVBoxLayout();
	layout->addWidget(textview);
	layout->addWidget(textline);
	setLayout(layout);

	// Register a callback on the textline for returnPressed signal
	connect(textline, SIGNAL(returnPressed()),
		this, SLOT(gotReturnPressed()));

    // Register a callback when another p2p app sends us a message over UDP
    connect(mySocket, SIGNAL(readyRead()),
    		this, SLOT(processPendingDatagrams()));

    // Timeout waiting for leader heartbeat. If no heartbeat within waitForHeartbeat msec, attempt to become leader
    qsrand(QTime::currentTime().msec());

    //timeToWaitForHeartbeat = qrand() % 150 + 150;
    timeToWaitForHeartbeat = qrand() % 150 + 500;  // A little slower for sanity

    waitForHeartbeatTimer = new QTimer(this);
    connect(waitForHeartbeatTimer, SIGNAL(timeout()), this, SLOT(sendRequestForVotes()));
    waitForHeartbeatTimer->start(timeToWaitForHeartbeat);

    // Timer for leader to keep track of when to send heartbeats.
    // Started when follower becomes leader, timeout defined in header definitions
    sendHeartbeatTimer = new QTimer(this);
    connect(sendHeartbeatTimer, SIGNAL(timeout()), this, SLOT(sendHeartbeat()));

}

/* Called by follower whose waitForHeartbeatTimer timed out. Starts a new election */
void ChatDialog::sendRequestForVotes(){

    qDebug() << "Attempting to become leader!";

    myCurrentTerm++;
    nodesThatVotedForMe.clear();
    nodesThatVotedForMe.append(myPort);
    votedFor = myCandidateId; // probably doesn't matter
    myRole = CANDIDATE;

    // Create requestVote message
    QVariantMap outMap;
    outMap.insert(QString("type"), QVariant(QString("requestVote")));
    outMap.insert(QString("term"), QVariant(myCurrentTerm));
    outMap.insert(QString("candidateId"), QVariant(myCandidateId));
    outMap.insert(QString("lastLogIndex"), QVariant(myLastLogIndex));
    outMap.insert(QString("lastLogTerm"), QVariant(myLastLogTerm));

    sendMessageToAll(outMap);
}

/* Called by follower to process a received a RequestVote message */
void ChatDialog::processRequestVote(QVariantMap inMap, quint16 sourcePort){

    qDebug()<< "Processing Request Vote";

    // Load parameters from message
    quint16 candidateTerm = inMap.value("term").toInt();
    quint16 candidateID = inMap.value("candidateId").toInt();
    qint16 candidateLastLogIndex = inMap.value("lastLogIndex").toInt();
    qint16 candidateLastLogTerm = inMap.value("lastLogTerm").toInt();

    if(candidateTerm < myCurrentTerm) {
        qDebug() << "Vote was denied because candidate term is less than mine!";
        replyToRequestForVote(false, sourcePort);
        return;
    }

    // Rules for Servers 5.1: If RPC request or response contains term T > currentTerm,
    // set currentTerm = T and convert to follower
    if(candidateTerm > myCurrentTerm){
        myCurrentTerm = candidateTerm;
        myRole = FOLLOWER;
        sendHeartbeatTimer->stop();
    }

    // Check if candidate's log is at least as up-to-date as receiver's log
    if(candidateLastLogTerm < myLastLogTerm ||
    (candidateLastLogTerm==myLastLogTerm && candidateLastLogIndex < myLastLogIndex)){
        qDebug() << "Vote was denied because candidate's log is not as up to date as mine!";
        replyToRequestForVote(false, sourcePort);
        return;
    }

    if(votedFor == -1 || votedFor == candidateID) {

        // Convert to follower
        myRole = FOLLOWER;
        sendHeartbeatTimer->stop();

        votedFor = candidateID;
        replyToRequestForVote(true, sourcePort);

        // Reset election timer when you grant a vote to another peer
        waitForHeartbeatTimer->start(timeToWaitForHeartbeat);

        return;
    }
    else{
        qDebug() << "Vote was denied because I have already voted for someone else this term!";
        replyToRequestForVote(false, sourcePort);
        return;
    }
}

/* Called by follower to reply to a received RequestVote message */
void ChatDialog::replyToRequestForVote(bool voteGranted, quint16 sourcePort){

    // reset waitForHeartbeatTimer, according to Raft visual
    waitForHeartbeatTimer->start(timeToWaitForHeartbeat);

    // Send reply to RequestVote
    QVariantMap outMap;
    outMap.insert("type", QVariant("replyToRequestVote"));
    outMap.insert("term", QVariant(myCurrentTerm));

    if(voteGranted) {
        qDebug() << "Vote was granted!";
        outMap.insert("voteGranted", QVariant(true));
    }
    else {
        outMap.insert("voteGranted", QVariant(false));
    }

    serializeMessage(outMap, sourcePort);
}

/* Called by candidate to process a reply to their RequestVote message */
void ChatDialog::processReplyRequestVote(QVariantMap inMap, quint16 sourcePort){

    // Load parameters from message
    quint16 termFromReply = inMap.value("term").toInt();
    bool voteGranted = inMap.value("voteGranted").toBool();

    // If vote is not for the current election, ignore reply
    if(termFromReply!= myCurrentTerm)
        return;

    if(voteGranted){
        if(!nodesThatVotedForMe.contains(sourcePort))
            nodesThatVotedForMe.append(sourcePort);

        // Check if I have a majority of votes
        if(nodesThatVotedForMe.length() >= 2){                  //should be 3, changing to 2 for debugging   !!!!

            qDebug() << "I became leader!";

            myRole = LEADER;
            myLeader = myCandidateId;

            // Initialize next index, which tracks (for each server) the next log entry I should send them
            nextIndex.clear();
            for(int i = mySocket->myPortMin; i<= mySocket->myPortMax; i++){
                if(i!= myPort)
                    nextIndex[i] = myLastLogIndex+1;
            }

            matchIndex.clear();
            for(int i = mySocket->myPortMin; i<= mySocket->myPortMax; i++){
                if(i!= myPort)
                    matchIndex[i] = 0;
            }

            // send initial heartbeat and start a timer to continually send heartbeats
            sendHeartbeat();
            sendHeartbeatTimer->start(HEARTBEATTIME);
        }
    }
    else{ // Vote was denied. I should update my term to term from the reply
        myCurrentTerm = termFromReply;
    }
}



/* Called by leader to replicate its log onto its followers */
void ChatDialog::sendAppendEntriesMsg(quint16 destPort){

    // We have a separate heartbeat function. Calling this function guarantees there is at least one entry in the log

    QVariantMap outMap;
    outMap.insert(QString("type"), QVariant(QString("appendEntries")));
    outMap.insert(QString("term"), QVariant(myCurrentTerm));
    outMap.insert(QString("leaderId"), QVariant(myCandidateId));
    outMap.insert(QString("leaderCommit"), QVariant(myCommitIndex));


    // nextIndex default at 1 for empty log
    quint16 nextIndexForPort = nextIndex[destPort];
    quint16 prevLogIndex = nextIndexForPort-1; // could be 0
    outMap.insert(QString("prevLogIndex"), QVariant(prevLogIndex));


    // prevLogTerm = term of the entry at prevLogIndex (if index 0, return 0)
    quint16 myPrevLogTerm = log.value(prevLogIndex).toMap().value("term").toInt();   // UPDATE
    qDebug() << "I think myPrevLogTerm is: " + QString::number(myPrevLogTerm);

    outMap.insert(QString("prevLogTerm"), QVariant(myPrevLogTerm));

    // maybe everyone's logs can start with index 0, having term 0


    outMap.insert(QString("entries"), QVariant(log)); //don't send full log haha


    // what if there are no entries in log yet. Well that's only possible for heartbeat
    //if I have one entry, what does prev log entry return???



    serializeMessage(outMap, destPort);
}






/* Called by follower to process an AppendEntries message from leader */
void ChatDialog::processAppendEntriesMsg(QVariantMap inMap, quint16 sourcePort){





    // Load parameters from message
    quint16 leaderTerm = inMap.value("term").toInt();
    qint16  leaderID = inMap.value("leaderID").toInt();
    qint16  leaderCommit = inMap.value("leaderCommit").toInt();


    // Heartbeat may not contain these values. Default to...
    qint16  leaderPrevLogIndex = inMap.value("prevLogIndex", -1).toInt();
    qint16  leaderPrevLogTerm = inMap.value("prevLogTerm", -1).toInt();
    QVariantList entries = inMap.value("entries").toList();

    if(inMap.contains("entries")){
        qDebug() << entries;
    }


    // Only reset election timer if we receive RPC from CURRENT leader
    if(leaderTerm == myCurrentTerm)
        waitForHeartbeatTimer->start(timeToWaitForHeartbeat);



    // Debugging!
    //replyToAppendEntries(true, sourcePort);



    return;


    // use commit index to increase lastApplied


    // If I believe this is from a real leader and not an obsolete leader...
    if(leaderTerm > myCurrentTerm) { //should this also happen if leaderTerm == myCurrentTerm?

        sendHeartbeatTimer->stop(); //If I was leader, stop sending heartbeats
        myRole = FOLLOWER;
        myLeader = sourcePort;
        myCurrentTerm = leaderTerm;
        votedFor = -1; // The previous election we voted in is over, reset votedFor so we can vote in the next election

    }

    if(leaderTerm < myCurrentTerm){
        qDebug() << "Replied false to Append Entries because my current term > leader term";
        replyToAppendEntries(false, sourcePort);
        return;
    }

    // Reply false if log doesn't contain an entry at prevLogIndex whose term matches prevLogTerm
    // Make sure we do not access log at undefined index
    if(leaderPrevLogIndex < log.length()  ){

        quint16 termAtPrevLogIndex = log.at(leaderPrevLogIndex).toStringList()[1].toInt();  //?????         !!!
        if(termAtPrevLogIndex != leaderPrevLogTerm){

            qDebug() << "Replied false to Append Entries b/c terms at prevLogIndex didn't match, need more entries";
            replyToAppendEntries(false, sourcePort);
            return;
        }
    }

    // We're going to accept entries from leader, but first drop everything in my log after leaderPrevLogIndex
    while(log.length()-1 > leaderPrevLogIndex){
        log.removeLast();
    }
    //log.append(leaderEntries);


    updateCommitIndex();






}

/* Called by follower to reply to an AppendEntries message from leader */
void ChatDialog::replyToAppendEntries(bool success, quint16 sourcePort){

    QVariantMap outMap;
    outMap.insert("type", QVariant("replyToAppendEntries"));
    outMap.insert("term", QVariant(myCurrentTerm));

    if(success) {
        qDebug() << "Append entries success!";
        outMap.insert("success", QVariant(true));
    }
    else {
        outMap.insert("success", QVariant(false));
    }

    serializeMessage(outMap, sourcePort);
}



/* Called by leader to process a follower's reply to their AppendEntries message */
void ChatDialog::processAppendEntriesMsgReply(QVariantMap inMap, quint16 sourcePort){

    // Load parameters from message
    quint16 term = inMap.value("term").toInt();
    bool success = inMap.value("success").toBool();



    // if rejected because of term inconsistency, step down!


    // update nextIndex and matchIndex

    // DECIDE IF I NEED TO SEND MORE APPEND ENTRIES with more entries (farther back)


    //if true, update matchIndex


    //check if majority

    // if majority, remove this from
    // apply to state machine, update my commit index
    // UPDATE COMMIT INDEX PLZ
    // remove from committed




    return;
}




void ChatDialog::updateCommitIndex(){

    // check matchIndex
    // update myCommitIndex
    // update stateMachine

    refreshTextView();

}


void ChatDialog::processPendingDatagrams(){

    while(mySocket->hasPendingDatagrams()){
        QByteArray datagram;
        datagram.resize(mySocket->pendingDatagramSize());
        QHostAddress source;
        quint16 sourcePort;

        if(mySocket->readDatagram(datagram.data(), datagram.size(), &source, &sourcePort) != -1){

            QDataStream inStream(&datagram, QIODevice::ReadOnly);
            QVariantMap inMap;
            inStream >> inMap;

            if(inMap.contains("type")){
                if(inMap.value("type") == "requestVote"){
                    qDebug() << "Received a request vote!";
                    processRequestVote(inMap, sourcePort);
                }

                else if(inMap.value("type") == "replyToRequestVote"){
                    qDebug() << "Received a reply to my request for votes!";
                    processReplyRequestVote(inMap, sourcePort);
                }

                else if(inMap.value("type") == "appendEntries"){
                    //qDebug() << "Received an Append Entries message!";
                    processAppendEntriesMsg(inMap, sourcePort);
                }

                else if(inMap.value("type") == "replyToAppendEntries"){
                    //qDebug() << "Received a reply to my Append Entries message!";
                    processAppendEntriesMsgReply(inMap, sourcePort);
                }
            }
        }
    }
}

void ChatDialog::gotReturnPressed(){


    // NEED TO PARSE FOR SUPPORT COMMANDS


    // Store "Client" request
    QString messageID = QString(myPort) + QString(mySeqNo);
    mySeqNo++;
    QString message = QString::number(myPort) + ": " + textline->text();

    QVariantMap clientRequest;
    clientRequest.insert("messageID", messageID);
    clientRequest.insert("term", myCurrentTerm); //Is this included?
    clientRequest.insert("message", message);

    queuedClientRequests.append(clientRequest);

    textline->clear();

    // ???????

    if(myRole == LEADER){

        attemptToCommitMsg();
    }

    else{    // I AM A CLIENT


        // SEND TO "SERVER" but wait for Leader to respond with success until I move on


        //attempt to forward messages from uncommitted to leader
        qDebug() << "hi";




    }
}

void ChatDialog::attemptToCommitMsg(){


    log.append(queuedClientRequests.first());

    // Send messages AppendEntries RPC everyone
    for(int i = mySocket->myPortMin; i<= mySocket->myPortMax; i++){
        if(i!= myPort)
            sendAppendEntriesMsg(i);
    }

    //stateMachine.append(queuedClientRequests.first().first);
    refreshTextView();


//    while(!uncommittedMsgs.isEmpty()){
//
//        nodesThatVotedForCommit.clear();
//
//        uncommittedMsgs.first();
//
//        sendAppendEntriesMsg();
//
//        processPendingDatagrams();
//
//    }


}






/* Leader sends a heartbeat to all followers every 50 msec*/
void ChatDialog::sendHeartbeat(){

    //qDebug() << "Sending heartbeat as leader";
    //qDebug() << "My term is: " + QString::number(currentTerm);

    QVariantMap outMap;
    outMap.insert(QString("type"), QVariant(QString("appendEntries")));
    outMap.insert(QString("term"), QVariant(myCurrentTerm));
    outMap.insert(QString("leaderId"), QVariant(myCandidateId));
    outMap.insert(QString("leaderCommit"), QVariant(myCommitIndex));

    sendMessageToAll(outMap);

    // Prevent leader from trying to depose themselves
    waitForHeartbeatTimer->start(timeToWaitForHeartbeat);
}



/* ---------------------------------------------------------------------------------------------------------*/
// Probably good ///


void ChatDialog::refreshTextView(){

    textview->clear();
    for(int i=0; i< stateMachine.length(); i++){

        textview->append(stateMachine.at(i));
    }
}

void ChatDialog::serializeMessage(QVariantMap &outMap, quint16 destPort){

    QByteArray outData;
    QDataStream outStream(&outData, QIODevice::WriteOnly);
    outStream << outMap;

    mySocket->writeDatagram(outData.data(), outData.size(), QHostAddress::LocalHost, destPort);
}

void ChatDialog::sendMessageToAll(QVariantMap msgMap){

    for(int i = mySocket->myPortMin; i<= mySocket->myPortMax; i++){
        if(i!= myPort)
            serializeMessage(msgMap, i);
    }
}

/* Pick a range of five UDP ports to try to allocate by default, computed based on my Unix user ID.*/
NetSocket::NetSocket(){
	// This makes it trivial for up to five P2Papp instances per user to find each other on the same host,
	// barring UDP port conflicts with other applications (which are quite possible). We use the range from
	// 32768 to 49152 for this purpose.
	myPortMin = 32768 + (getuid() % 4096)*4;
	myPortMax = myPortMin + 4; // Edited to support 5 max users!
}

/* Bind one of ports between myPortMin and myPortMax */
bool NetSocket::bind(){
	for (int p = myPortMin; p <= myPortMax; p++) {
		if (QUdpSocket::bind(p)) {
			qDebug() << "bound to UDP port " << p;
			port = p;
			return true;
		}
	}
	qDebug() << "Oops, no ports in my default range " << myPortMin << "-" << myPortMax << " available";
	return false;
}

int main(int argc, char **argv){
	// Initialize Qt toolkit
	QApplication app(argc,argv);

	// Create an initial chat dialog window
	ChatDialog dialog;
	dialog.show();

	// Enter the Qt main loop; everything else is event driven
	return app.exec();
}

