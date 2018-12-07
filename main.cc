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
    qsrand(QTime::currentTime().msec());
	myOrigin = QString::number(qrand() % 100 + 1) + QString::number(0) + QString::number(myPort);

	qDebug() << "myOrigin: " << myOrigin;
	qDebug() << "-------------------";

    setWindowTitle("P2Papp " + myOrigin);

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

    antiEntropyTimer = new QTimer(this);
    connect(antiEntropyTimer, SIGNAL(timeout()), this, SLOT(antiEntropy()));
    antiEntropyTimer->start(10000);

    resendTimer = new QTimer(this);
    connect(resendTimer, SIGNAL(timeout()), this, SLOT(resendRumor()));
}

void ChatDialog::gotReturnPressed(){

    textview->setTextColor(QColor("blue"));
	textview->append("Me: " + textline->text());
    textview->setTextColor(QColor("black"));

	QString msg = textline->text();

	QMap<quint32, QString> chatLogEntry;
	chatLogEntry.insert(mySeqNo, msg);

	if(!chatLogs.contains(myOrigin)){
		chatLogs.insert(myOrigin, chatLogEntry);
		statusMap.insert(myOrigin, QVariant(mySeqNo +1));
	}
	else{
		chatLogs[myOrigin].insert(mySeqNo, msg);
		statusMap[myOrigin] = QVariant(mySeqNo + 1);
	}

	sendRumorMessage(myOrigin, mySeqNo, pickClosestNeighbor());
	mySeqNo += 1;

	textline->clear();
}

quint16 ChatDialog::pickClosestNeighbor(){

    qDebug() << "Picking closest neighbor";

    int maxAttempts = 10;
    quint16 neighborPort;

    if(myPort == mySocket->myPortMin)
        neighborPort = myPort+1;

    else if(myPort == mySocket->myPortMax)
        neighborPort = myPort-1;

    // Send ping, collect ping reply to determine which neighbor is closer
    else{
        quint16 n1 = myPort - 1;
        quint16 n2 = myPort + 1;

        n1Timer = new QElapsedTimer();
        n1Timer->start();
        n2Timer = new QElapsedTimer();
        n2Timer->start();

        QVariantMap pingMapN1, pingMapN2;
        pingMapN1.insert(QString("Ping"), QVariant(1));
        pingMapN2.insert(QString("Ping"), QVariant(2));

        // It's possible to NOT get a response from one neighbor or both neighbors.
        // Need max attempts in case both neighbors are down or else hangs forever.
        int attempts = 0;
        while (n1Time == QINT64MAX && n2Time == QINT64MAX && attempts <= maxAttempts) {
            qDebug() << "Sending pings to both neighbors";
            serializeMessage(pingMapN1, n1);
            serializeMessage(pingMapN2, n2);
            processPendingDatagrams();

            // Sleep for 0.1 seconds to allow for time for ping reply
            usleep(100);
            attempts++;
        }

        qDebug() << "N1 response took" << QString::number(n1Time) << "milliseconds";
        qDebug() << "N2 response took" << QString::number(n2Time) << "milliseconds";

        if(attempts == maxAttempts)
            neighborPort = pickRandomNeighbor();
        else if (n1Time < n2Time)
            neighborPort = n1;
        else
            neighborPort = n2;

        // Reset timer states
        attempts =0;
        delete n1Timer;
        delete n2Timer;
        n1Timer = nullptr;
        n2Timer = nullptr;
        n1Time = QINT64MAX;
        n2Time = QINT64MAX;
    }
    qDebug() << "Picked neighbor" << QString::number(neighborPort);
    return neighborPort;
}

quint16 ChatDialog::pickRandomNeighbor() {

    quint16 neighborPort;

    if(myPort == mySocket->myPortMin)
        neighborPort = myPort+1;

    else if(myPort == mySocket->myPortMax)
        neighborPort = myPort-1;

    else if (qrand() % 2 == 0)
        neighborPort = myPort+1;

    else
        neighborPort = myPort-1;

    //qDebug() << "Picked neighbor " + QString::number(neighborPort);
    return neighborPort;
}

void ChatDialog::sendRumorMessage(QString origin, quint32 seqNo, quint16 destPort){

	if(!chatLogs.contains(origin) || !chatLogs[origin].contains(seqNo))
		return;

	QVariantMap rumorMap;
	rumorMap.insert(QString("ChatText"), chatLogs[origin].value(seqNo));
	rumorMap.insert(QString("Origin"), origin);
	rumorMap.insert(QString("SeqNo"), seqNo);

	qDebug() << "Sending Origin/seqNo " + origin + "/" + QString::number(seqNo) + " to: " + QString::number(destPort);

	serializeMessage(rumorMap, destPort);

    // Start Timer looking for response
    resendTimer->start(2000);
    lastRumorPort = destPort;
    lastRumorOrigin = origin;
    lastRumorSeqNo = seqNo;
}

void ChatDialog::serializeMessage(QVariantMap &outMap, quint16 destPort){

	QByteArray outData;
	QDataStream outStream(&outData, QIODevice::WriteOnly);
	outStream << outMap;

    mySocket->writeDatagram(outData.data(), outData.size(), QHostAddress::LocalHost, destPort);
}

void ChatDialog::antiEntropy() {
    //qDebug() << "Anti Entropy!";
    sendStatusMessage(pickRandomNeighbor());
}

void ChatDialog::resendRumor(){
    qDebug() << "Resending rumor because no status message reply!";
    sendRumorMessage(lastRumorOrigin, lastRumorSeqNo, lastRumorPort);
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

            // If we receive Ping, send Ping Reply
            if(inMap.contains("Ping")){

                qDebug() << "Sending PingReply!";

                QVariantMap pingReply;
                pingReply.insert(QString("PingReply"),inMap["Ping"]);
                serializeMessage(pingReply, sourcePort);
            }

            // If we receive Ping Reply, update timers
            else if(inMap.contains("PingReply")){

                qDebug() << "Received Ping Reply: " << inMap["PingReply"];

                if(inMap["PingReply"] == QVariant(1)) {
                    if(n1Time == QINT64MAX && n1Timer != nullptr)
                        n1Time = n1Timer->elapsed();
                }
                else {
                    if(n2Time == QINT64MAX && n2Timer != nullptr)
                        n2Time = n2Timer->elapsed();
                }
            }

            else if(inMap.contains("ChatText"))
                receiveRumorMessage(inMap, sourcePort);

            else if(inMap.contains("Want")){

                // If received a status message from the peer I sent a rumor to, stop the resend timer
                if(sourcePort == lastRumorPort)
                    resendTimer->stop();

                receiveStatusMessage(inMap, sourcePort);
            }
		}
	}
}

void ChatDialog::receiveRumorMessage(QVariantMap inMap, quint16 sourcePort){

    QString origin = inMap.value("Origin").value <QString> ();
	quint32 seqNo = inMap.value("SeqNo").value <quint32> ();
	QString msg = inMap.value("ChatText").value <QString> ();

    qDebug() << "I got a rumor message: " + msg + " from: " + QString::number(sourcePort);

	QMap<quint32, QString> chatLogEntry;
	chatLogEntry.insert(seqNo, msg);

	// For convenience, do not store any message with OOO seq number but reply with status message
	// If chatLogs does not contain messages from this origin, we expect message with seqNo 1
	if(!chatLogs.contains(origin)){
		if(seqNo != SEQNOSTART) {
            sendStatusMessage(sourcePort);
            sendRumorMessage(origin, seqNo, pickRandomNeighbor());
            return;
        }

        chatLogs.insert(origin, chatLogEntry);
        statusMap.insert(origin, QVariant(seqNo + 1));
        textview->append(origin + ": " + msg);

	}
	//If chatLogs *does* contain this origin, we expect message with seqNo = last seq num + 1
	else{
		quint32 lastSeqNum = chatLogs[origin].keys().last();

		if (seqNo == lastSeqNum + 1){
			chatLogs[origin].insert(seqNo, msg);
			statusMap[origin] = QVariant(seqNo + 1);
			textview->append(origin + ": " + msg);
		}
	}
	sendStatusMessage(sourcePort);
	sendRumorMessage(origin, seqNo, pickRandomNeighbor());
}

void ChatDialog::sendStatusMessage(quint16 destPort){

	QVariantMap statusMessage;
	statusMessage.insert(QString("Want"), statusMap);
	serializeMessage(statusMessage, destPort);

}

void ChatDialog::receiveStatusMessage(QVariantMap inMap, quint16 sourcePort){

	QVariantMap recvStatusMap = inMap["Want"].value <QVariantMap> ();
	QList<QString> recvOriginList = recvStatusMap.keys();
    QList<QString> myOriginList = statusMap.keys();

    //qDebug() << "I got a status message from: " + QString::number(sourcePort);
    //qDebug() << "recvStatusMap: " << recvStatusMap;
    //qDebug() << "myStatusMap: " << statusMap;

	for(int i=0; i < myOriginList.count(); i++){
	    if(!recvOriginList.contains(myOriginList[i])) {
            sendRumorMessage(myOriginList[i], SEQNOSTART, sourcePort);
            return;
        }
	}

	for(int i=0; i< recvOriginList.count(); i++){

	    quint32 sourceSeqNoForOrigin = recvStatusMap[recvOriginList[i]].value <quint32> ();
	    quint32 mySeqNoForOrigin = statusMap[recvOriginList[i]].value <quint32> ();

        if(sourceSeqNoForOrigin == mySeqNoForOrigin)
            continue;

        else if(sourceSeqNoForOrigin > mySeqNoForOrigin) {
            sendStatusMessage(sourcePort);
            return;
        }

        else if(sourceSeqNoForOrigin < mySeqNoForOrigin) {

            // 0 means sourceMap is empty for this Origin since seqNo starts at 1!
            if(sourceSeqNoForOrigin == 0)
                sendRumorMessage(recvOriginList[i], SEQNOSTART, sourcePort);
            else
                sendRumorMessage(recvOriginList[i], sourceSeqNoForOrigin, sourcePort);
            return;
        }
    }

    // Neither peer appears to have any messages the other has not yet seen.
    // Flip a coin: pick new random neighbor to start rumormongering with or ceases the rumormongering process.
    if (qrand() % 2 == 0){
        sendStatusMessage(pickRandomNeighbor());
    }
}

/* Pick a range of four UDP ports to try to allocate by default, computed based on my Unix user ID.*/
NetSocket::NetSocket(){
	// This makes it trivial for up to four P2Papp instances per user to find each other on the same host,
	// barring UDP port conflicts with other applications (which are quite possible). We use the range from
	// 32768 to 49151 for this purpose.
	myPortMin = 32768 + (getuid() % 4096)*4;
	myPortMax = myPortMin + 4; // Edit to support 5 max users!

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

