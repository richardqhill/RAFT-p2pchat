#include "main.hh"

ChatDialog::ChatDialog(){

    mySocket = new NetSocket();
    if(!  mySocket->bind()) {
		exit(1);
	}
    else {
		myPort = mySocket->port;
	}

	mySeqNo = 1;  // CHANGE TO 1
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

	sendRumorMessage(myOrigin, mySeqNo, pickRandomNeighbor());
	mySeqNo += 1;

	textline->clear();
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

    qDebug() << "Picked neighbor " + QString::number(neighborPort);
    return neighborPort;
}

void ChatDialog::sendRumorMessage(QString origin, quint32 seqNo, quint16 destPort){

	if(!chatLogs.contains(origin) || !chatLogs[origin].contains(seqNo))
		return;

	QVariantMap rumorMap;

	rumorMap.insert(QString("ChatText"), chatLogs[origin].value(seqNo));
	rumorMap.insert(QString("Origin"), origin);
	rumorMap.insert(QString("SeqNo"), seqNo);

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
    qDebug() << "Resending rumor because no status message!";
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

            if(inMap.contains("ChatText"))
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

	QMap<quint32, QString> chatLogEntry;
	chatLogEntry.insert(seqNo, msg);

	// For convenience, discard any message with OOO seq number
	// If chatLogs does not contain messages from this origin, we expect message with seqNo 0
	if(!chatLogs.contains(origin)){
		if(seqNo != 1)
		    return;

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

    qDebug() << "I got a status message from: " + QString::number(sourcePort);

	QVariantMap recvStatusMap = inMap["Want"].value <QVariantMap> ();
	QList<QString> recvOriginList = recvStatusMap.keys();
    QList<QString> myOriginList = statusMap.keys();

    qDebug() << "recvStatusMap: " << recvStatusMap;
    qDebug() << "myStatusMap: " << statusMap;

	for(int i=0; i < myOriginList.count(); i++){
	    if(!recvOriginList.contains(myOriginList[i])) {
            sendRumorMessage(myOriginList[i], 1, sourcePort);
            return;
        }
	}

	for(int i=0; i< recvOriginList.count(); i++){

	    quint32 sourceSeqNoForOrigin = recvStatusMap[recvOriginList[i]].value <quint32> ();
	    quint32 mySeqNoForOrigin = statusMap[recvOriginList[i]].value <quint32> ();

        if(sourceSeqNoForOrigin == mySeqNoForOrigin)
            break;

        else if(sourceSeqNoForOrigin > mySeqNoForOrigin) {
            sendStatusMessage(sourcePort);
            return;
        }

        else if(sourceSeqNoForOrigin < mySeqNoForOrigin) {
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
	myPortMax = myPortMin + 3;

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

