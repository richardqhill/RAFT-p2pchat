#include "main.hh"

ChatDialog::ChatDialog(){

    mySocket = new NetSocket();
    if(!  mySocket->bind()) {
		exit(1);
	}
    else {
		myPort = mySocket->port;
	}

    mySeqNo = 1;

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

    currentTerm++;
    nodesThatVotedForMe.clear();
    votedFor = myCandidateId; // probably doesn't matter
    nodesThatVotedForMe.append(myPort);
    myRole = CANDIDATE;

    // Create requestVote message
    QVariantMap requestVoteMap;
    requestVoteMap.insert(QString("type"), QVariant(QString("requestVote")));
    requestVoteMap.insert(QString("term"), QVariant(currentTerm));
    requestVoteMap.insert(QString("candidateId"), QVariant(myCandidateId));
    requestVoteMap.insert(QString("lastLogIndex"), QVariant(lastLogIndex));
    requestVoteMap.insert(QString("lastLogTerm"), QVariant(lastLogTerm));

    sendMessageToAll(requestVoteMap);
}

/* Called by follower to process a received a RequestVote message */
void ChatDialog::processRequestVote(QVariantMap inMap, quint16 sourcePort){

    qDebug()<< "Processing Request Vote";

    quint16 candidateTerm = inMap["term"].toInt();
    quint16 candidateID = inMap["candidateId"].toInt();
    quint16 candidateLastLogIndex = inMap["lastLogIndex"].toInt();
    quint16 candidateLastLogTerm = inMap["lastLogTerm"].toInt();


    if(candidateTerm < currentTerm) {
        qDebug() << "Vote was denied because candidate term is less than mine!";
        replyToRequestForVote(false, sourcePort);
        return;
    }

    currentTerm = candidateTerm;  //??????

    // Check if candidate's log is at least as up-to-date as receiver's log
    if(candidateLastLogTerm < lastLogTerm ||
    (candidateLastLogTerm==lastLogTerm && candidateLastLogIndex < lastLogIndex)){
        qDebug() << "Vote was denied because candidate's log is not as up to date as mine!";
        replyToRequestForVote(false, sourcePort);
        return;
    }

    if(votedFor == -1 || votedFor == candidateID) {

        myRole = FOLLOWER;  //ISSUES?
        votedFor = candidateID;
        replyToRequestForVote(true, sourcePort);
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
    outMap["type"] = QString::fromStdString("replyRequestVote");
    outMap["term"] = QVariant(currentTerm);

    if(voteGranted) {
        qDebug() << "Vote was granted!";
        outMap["voteGranted"] = QVariant(true);
    }
    else {
        outMap["voteGranted"] = QVariant(false);
    }

    serializeMessage(outMap, sourcePort);
}

/* Called by candidate to process a reply to their RequestVote message */
void ChatDialog::processReplyRequestVote(QVariantMap inMap, quint16 sourcePort){

    // Load parameters from message
    quint16 termFromReply = inMap["term"].toInt();
    bool voteGranted = inMap["voteGranted"].toBool();

    // If vote is not for the current election, ignore
    if(termFromReply!= currentTerm)
        return;

    if(voteGranted){
        if(!nodesThatVotedForMe.contains(sourcePort))
            nodesThatVotedForMe.append(sourcePort);

        // Check if I have a majority of votes
        if(nodesThatVotedForMe.length() >= 2){ //should be 3, changing to 2 for debugging

            qDebug() << "I became leader!";

            myRole = LEADER;
            myLeader = myCandidateId;

            // Initialize next index, which tracks (for each server) the next log entry I should send them
            nextIndex.clear();
            for(int i = mySocket->myPortMin; i<= mySocket->myPortMax; i++){
                if(i!= myPort)
                    nextIndex[i] = lastLogIndex+1;
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
    // Vote was denied. I should update my term to term from the reply
    currentTerm = termFromReply;
}

/* Called by leader to replicate its log onto its followers */
void ChatDialog::sendAppendEntriesMsg(quint16 prevLogIndex, quint16 destPort){ //THIS NEEDS PARAMETERS

    QVariantMap outMap;
    outMap.insert(QString("type"), QVariant(QString("appendEntries")));
    outMap.insert(QString("term"), QVariant(currentTerm));
    outMap.insert(QString("leaderId"), QVariant(myCandidateId));

    outMap.insert(QString("prevLogIndex"), QVariant(prevLogIndex));
    quint16 prevLogTerm = log.at(prevLogIndex).second.second;
    outMap.insert(QString("prevLogTerm"), QVariant(prevLogTerm));

    outMap.insert(QString("leaderCommit"), QVariant(commitIndex));


    // ADD ENTRIES
    // reference nextIndex to determine how many things to send


    qDebug() << "hi";

}

/* Called by follower to process an AppendEntries message from leader */
void ChatDialog::processAppendEntriesMsg(QVariantMap inMap, quint16 sourcePort){

    // Received something from a leader, reset heartbeat timer
    waitForHeartbeatTimer->start(timeToWaitForHeartbeat);

    // Load parameters from message
    quint16 leaderTerm = inMap["term"].toInt();
    qint16  leaderID = inMap["leaderID"].toInt();
    quint16 leaderPrevLogIndex = inMap["prevLogIndex"].toInt();
    quint16 leaderPrevLogTerm = inMap["prevLogTerm"].toInt();
    quint16 leaderCommit = inMap["leaderCommit"].toInt();

    if(inMap.contains("entries")){
        return; // also get ENTRIES
        // return; // ????
    }

    // If I believe this is from a real leader and not an obsolete leader...
    if(leaderTerm >= currentTerm) {
        votedFor = -1; // The previous election we voted in is over, reset votedFor so we can vote in the next election
        currentTerm = leaderTerm;
        myRole = FOLLOWER;
        myLeader = sourcePort;
    }

    if(leaderTerm < currentTerm){
        qDebug() << "Denied Append Entries because my current term > leader term";
        replyToAppendEntries(false, sourcePort);
        return;
    }

    // Reply false if log doesn't contain an entry at prevLogIndex whose term matches prevLogTerm
    // Make sure we do not access log at undefined index
    if(leaderPrevLogIndex < log.length()  && log.at(leaderPrevLogIndex).second.second != leaderPrevLogTerm){
        qDebug() << "Denied Append Entries because terms at prevLogIndex did not match, need more entries";
        replyToAppendEntries(false, sourcePort);
        return;
    }

    // We're going to accept entries from leader, but first drop everything in my log after leaderPrevLogIndex
    while(log.length()-1 > leaderPrevLogIndex){
        log.removeLast();
    }
    //log.append(leaderEntries);

    // Check what is now committed
    // Add that to the state machine
    // Refresh display


    // IF TRUE, I SHOULD UPDATE THE DISPLAY WITH THE NEW LOG
//    textview->setTextColor(QColor("blue"));
//    textview->append("Some junk");
//    textview->setTextColor(QColor("black"));

}

/* Called by follower to reply to an AppendEntries message from leader */
void ChatDialog::replyToAppendEntries(bool success, quint16 sourcePort){

    QVariantMap outMap;
    outMap["type"] = QString::fromStdString("replyAppendEntries");
    outMap["term"] = QVariant(currentTerm);

    if(success) {
        qDebug() << "Append entries success!";
        outMap["success"] = QVariant(true);
    }
    else {
        outMap["success"] = QVariant(false);
    }

    serializeMessage(outMap, sourcePort);
}





/* Called by leader to process a follower's reply to their AppendEntries message */
void ChatDialog::processAppendEntriesMsgReply(QVariantMap inMap, quint16 sourcePort){

    // DECIDE IF I NEED TO SEND MORE APPEND ENTRIES with more entries (farther back)


    //if true, update matchIndex
    //nodesThatVotedForCommit.append(sourcePort);

    //check if majority

    // if majority, remove this from
    // add to committed
    // UPDATE COMMIT INDEX PLZ
    // remove from commmited




    return;
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
                if(inMap["type"] == "requestVote"){
                    qDebug() << "Received a request vote!";
                    processRequestVote(inMap, sourcePort);
                }

                else if(inMap["type"] == "replyRequestVote"){
                    qDebug() << "Received a request vote reply!";
                    processReplyRequestVote(inMap, sourcePort);
                }

                else if(inMap["type"] == "appendEntries"){
                    //qDebug() << "Received an append Entries message!";
                    processAppendEntriesMsg(inMap, sourcePort);
                }

                else if(inMap["type"] == "replyAppendEntries"){
                    //qDebug() << "Received an append Entries reply!";
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
    queuedClientRequests.append(qMakePair(message, qMakePair(messageID, currentTerm)));
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


    stateMachine.append(queuedClientRequests.first().first);
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










/* ---------------------------------------------------------------------------------------------------------*/
// Probably good ///

void ChatDialog::sendHeartbeat(){

    qDebug() << "Sending heartbeat as leader";
    //qDebug() << "My term is: " + QString::number(currentTerm);

    QVariantMap outMap;
    outMap.insert(QString("type"), QVariant(QString("appendEntries")));
    outMap.insert(QString("term"), QVariant(currentTerm));
    outMap.insert(QString("leaderId"), QVariant(myCandidateId));

//    outMap.insert(QString("prevLogIndex"), QVariant(prevLogIndex));
//    quint16 prevLogTerm = log.at(prevLogIndex).second.second;
//    outMap.insert(QString("prevLogTerm"), QVariant(prevLogTerm));
    outMap.insert(QString("leaderCommit"), QVariant(commitIndex));

    sendMessageToAll(outMap);

    // Prevent leader from trying to depose themselves
    waitForHeartbeatTimer->start(timeToWaitForHeartbeat);
}

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

