#include "COM_Socket.h"
#include "COM_Node.h"
#include "COM_SocketConnection.h"
#include "COM_ExecutionSystem.h"
#include <stdio.h>

InputSocket::InputSocket(DataType datatype) :Socket(datatype) {
	this->connection = NULL;
	this->groupOutput = NULL;
	this->resizeMode = COM_SC_CENTER;
}
InputSocket::InputSocket(DataType datatype, InputSocketResizeMode resizeMode) :Socket(datatype) {
	this->connection = NULL;
	this->groupOutput = NULL;
	this->resizeMode = resizeMode;
}

InputSocket::InputSocket(InputSocket* from) :Socket(from->getDataType()) {
	this->connection = NULL;
	this->groupOutput = NULL;
	this->resizeMode = from->getResizeMode();
}

InputSocket::~InputSocket() {
    if (this->groupOutput != NULL && !this->isInsideOfGroupNode()) {
        delete this->groupOutput;
    }
    this->groupOutput = NULL;
}


int InputSocket::isInputSocket() const { return true; }
const int InputSocket::isConnected() const { return this->connection != NULL; }

void InputSocket::setConnection(SocketConnection *connection) {
	this->connection = connection;
}
SocketConnection* InputSocket::getConnection() {return this->connection;}

void InputSocket::determineResolution(unsigned int resolution[],unsigned int preferredResolution[]) {
    if (this->isConnected()){
        this->connection->getFromSocket()->determineResolution(resolution, preferredResolution);
    } else {
        return;
    }
}

DataType InputSocket::convertToSupportedDataType(DataType datatype) {
    int supportedDataTypes = getDataType();
    if (supportedDataTypes&datatype) {
        return datatype;
    }
    bool candoValue = supportedDataTypes&COM_DT_VALUE;
    bool candoVector = supportedDataTypes&COM_DT_VECTOR;
    bool candoColor = supportedDataTypes&COM_DT_COLOR;

    if (datatype == COM_DT_VALUE) {
        if (candoColor) {
            return COM_DT_COLOR;
        } else if (candoVector) {
            return COM_DT_VECTOR;
        }
    } else if (datatype == COM_DT_VECTOR) {
        if (candoColor) {
            return COM_DT_COLOR;
        } else if (candoValue) {
            return COM_DT_VALUE;
        }
    } else if (datatype == COM_DT_COLOR) {
        if (candoVector) {
            return COM_DT_VECTOR;
        } else if (candoValue) {
            return COM_DT_VALUE;
        }
    }
    printf("WARNING: Unknown how to do conversion to supported datatype\n");
    return this->getDataType();
}

void InputSocket::determineActualDataType() {
	/// @note: this method is only called for inputsocket that are not connected.
	/// @note: passes COM_DT_COLOR, the convertToSupportedDataType converts this to a capable DataType
	this->setActualDataType(this->convertToSupportedDataType(COM_DT_COLOR));
	if (this->getGroupOutputSocket()) {
		if (!this->isInsideOfGroupNode()) {
			this->getGroupOutputSocket()->determineActualDataType();
		}
	}
}

void InputSocket::notifyActualInputType(DataType datatype) {
	DataType supportedDataType = convertToSupportedDataType(datatype);
    this->setActualDataType(supportedDataType);
    this->fireActualDataTypeSet();
}

void InputSocket::fireActualDataTypeSet() {
    this->getNode()->notifyActualDataTypeSet(this, this->getActualDataType());
}
void InputSocket::relinkConnections(InputSocket *relinkToSocket) {
    this->relinkConnections(relinkToSocket, false, -1, NULL);
}

void InputSocket::relinkConnections(InputSocket *relinkToSocket, bool autoconnect, int editorNodeInputSocketIndex, bool duplicate, ExecutionSystem* graph) {
    if (!duplicate) {
        this->relinkConnections(relinkToSocket, autoconnect, editorNodeInputSocketIndex, graph);
    } else {
        if (!this->isConnected() && autoconnect) {
            Node* node = (Node*)this->getNode();
            switch (this->getActualDataType()) {
			case COM_DT_UNKNOWN:
				printf("relink autoconnect with unknown datatype\n");
			case COM_DT_COLOR:
				node->addSetColorOperation(graph, relinkToSocket, editorNodeInputSocketIndex);
                break;
            case COM_DT_VECTOR:
                node->addSetVectorOperation(graph, relinkToSocket, editorNodeInputSocketIndex);
                break;
            case COM_DT_VALUE:
                node->addSetValueOperation(graph, relinkToSocket, editorNodeInputSocketIndex);
                break;
            }
            return;
        }
        SocketConnection * newConnection = new SocketConnection();
        OutputSocket * fromSocket = this->getConnection()->getFromSocket();
        newConnection->setToSocket(relinkToSocket);
        newConnection->setFromSocket(fromSocket);
        relinkToSocket->setConnection(newConnection);
        fromSocket->addConnection(newConnection);
        graph->addSocketConnection(newConnection);
    }
}

void InputSocket::relinkConnections(InputSocket *relinkToSocket, bool autoconnect, int editorNodeInputSocketIndex, ExecutionSystem* graph) {
    if (!isConnected()) {
        if (autoconnect) {
            Node* node = (Node*)this->getNode();
            switch (this->getActualDataType()) {
			case COM_DT_UNKNOWN:
				printf("relink autoconnect with unknown datatype\n");
			case COM_DT_COLOR:
				node->addSetColorOperation(graph, relinkToSocket, editorNodeInputSocketIndex);
                break;
            case COM_DT_VECTOR:
                node->addSetVectorOperation(graph, relinkToSocket, editorNodeInputSocketIndex);
                break;
            case COM_DT_VALUE:
                node->addSetValueOperation(graph, relinkToSocket, editorNodeInputSocketIndex);
                break;
			}
        }
        return;
    }
    SocketConnection *connection = this->getConnection();
    connection->setToSocket(relinkToSocket);
    relinkToSocket->setConnection(connection);
    this->setConnection(NULL);
}

const ChannelInfo* InputSocket::getChannelInfo(const int channelnumber) {
	if (this->isConnected() && this->connection->getFromSocket()) {
		return this->connection->getFromSocket()->getChannelInfo(channelnumber);
	} else {
		return NULL;
	}
}

bool InputSocket::isStatic() {
	if (isConnected()) {
		NodeBase* node = this->getConnection()->getFromNode();
		if (node) {
			return node->isStatic();
		}
	}
	return true;
}
SocketReader* InputSocket::getReader() {
	return this->getOperation();
}

NodeOperation* InputSocket::getOperation() const {
	if (isConnected()) {
		return (NodeOperation*)this->connection->getFromSocket()->getNode();
	} else {
		return NULL;
	}
}

float* InputSocket::getStaticValues() {
	return this->getbNodeSocket()->ns.vec;
}
