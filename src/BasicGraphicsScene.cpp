#include "BasicGraphicsScene.hpp"

#include "AbstractNodeGeometry.hpp"
#include "ConnectionGraphicsObject.hpp"
#include "ConnectionIdUtils.hpp"
#include "DefaultConnectionPainter.hpp"
#include "DefaultHorizontalNodeGeometry.hpp"
#include "DefaultNodePainter.hpp"
#include "DefaultVerticalNodeGeometry.hpp"
#include "GraphicsView.hpp"
#include "NodeGraphicsObject.hpp"
#include "qdebug.h"

#include <QUndoStack>

#include <QtWidgets/QFileDialog>
#include <QtWidgets/QGraphicsSceneMoveEvent>

#include <QtCore/QBuffer>
#include <QtCore/QByteArray>
#include <QtCore/QDataStream>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QtGlobal>

#include <iostream>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <queue>

namespace QtNodes {

BasicGraphicsScene::BasicGraphicsScene(AbstractGraphModel &graphModel, QObject *parent)
    : QGraphicsScene(parent)
    , _graphModel(graphModel)
    , _nodeGeometry(std::make_unique<DefaultHorizontalNodeGeometry>(_graphModel))
    , _nodePainter(std::make_unique<DefaultNodePainter>())
    , _connectionPainter(std::make_unique<DefaultConnectionPainter>())
    , _nodeDrag(false)
    , _undoStack(new QUndoStack(this))
    , _orientation(Qt::Horizontal)
{
    setItemIndexMethod(QGraphicsScene::NoIndex);

    connect(&_graphModel,
            &AbstractGraphModel::connectionCreated,
            this,
            &BasicGraphicsScene::onConnectionCreated);

    connect(&_graphModel,
            &AbstractGraphModel::connectionDeleted,
            this,
            &BasicGraphicsScene::onConnectionDeleted);

    connect(&_graphModel,
            &AbstractGraphModel::nodeCreated,
            this,
            &BasicGraphicsScene::onNodeCreated);

    connect(&_graphModel,
            &AbstractGraphModel::nodeDeleted,
            this,
            &BasicGraphicsScene::onNodeDeleted);

    connect(&_graphModel,
            &AbstractGraphModel::nodePositionUpdated,
            this,
            &BasicGraphicsScene::onNodePositionUpdated);

    connect(&_graphModel,
            &AbstractGraphModel::nodeUpdated,
            this,
            &BasicGraphicsScene::onNodeUpdated);

    connect(this, &BasicGraphicsScene::nodeClicked, this, &BasicGraphicsScene::onNodeClicked);

    connect(&_graphModel, &AbstractGraphModel::modelReset, this, &BasicGraphicsScene::onModelReset);

    traverseGraphAndPopulateGraphicsObjects();
}

BasicGraphicsScene::~BasicGraphicsScene() = default;

AbstractGraphModel const &BasicGraphicsScene::graphModel() const
{
    return _graphModel;
}

AbstractGraphModel &BasicGraphicsScene::graphModel()
{
    return _graphModel;
}

AbstractNodeGeometry &BasicGraphicsScene::nodeGeometry()
{
    return *_nodeGeometry;
}

AbstractNodePainter &BasicGraphicsScene::nodePainter()
{
    return *_nodePainter;
}

AbstractConnectionPainter &BasicGraphicsScene::connectionPainter()
{
    return *_connectionPainter;
}

void BasicGraphicsScene::setNodePainter(std::unique_ptr<AbstractNodePainter> newPainter)
{
    _nodePainter = std::move(newPainter);
}

void BasicGraphicsScene::setConnectionPainter(std::unique_ptr<AbstractConnectionPainter> newPainter)
{
    _connectionPainter = std::move(newPainter);
}

QUndoStack &BasicGraphicsScene::undoStack()
{
    return *_undoStack;
}

std::unique_ptr<ConnectionGraphicsObject> const &BasicGraphicsScene::makeDraftConnection(
    ConnectionId const incompleteConnectionId)
{
    _draftConnection = std::make_unique<ConnectionGraphicsObject>(*this, incompleteConnectionId);

    _draftConnection->grabMouse();

    return _draftConnection;
}

void BasicGraphicsScene::getRecordTemplates(std::vector<FcpDRC::cesgrouprecord> inputRecordVector)
{
    for (const auto &headerRecord : inputRecordVector)
    {
        m_record.push_back(headerRecord);
    }
}

void BasicGraphicsScene::resetDraftConnection()
{
    _draftConnection.reset();
}

void BasicGraphicsScene::clearScene()
{
    auto const &allNodeIds = graphModel().allNodeIds();

    for (auto nodeId : allNodeIds) {
        graphModel().deleteNode(nodeId);
    }
}

NodeGraphicsObject *BasicGraphicsScene::nodeGraphicsObject(NodeId nodeId)
{
    NodeGraphicsObject *ngo = nullptr;
    auto it = _nodeGraphicsObjects.find(nodeId);
    if (it != _nodeGraphicsObjects.end()) {
        ngo = it->second.get();
    }

    return ngo;
}

ConnectionGraphicsObject *BasicGraphicsScene::connectionGraphicsObject(ConnectionId connectionId)
{
    ConnectionGraphicsObject *cgo = nullptr;
    auto it = _connectionGraphicsObjects.find(connectionId);
    if (it != _connectionGraphicsObjects.end()) {
        cgo = it->second.get();
    }

    return cgo;
}

std::vector<BasicGraphicsScene::ConnectionInfo> BasicGraphicsScene::getConnections() const {
    std::vector<ConnectionInfo> connections;

    for (const auto& [connectionId, dialogPair] : _dialogs) {
        ConnectionInfo connectionInfo;
        connectionInfo.connectionId = connectionId;
        connectionInfo.nodeIdOut = connectionId.outNodeId;
        connectionInfo.nodeIdIn = connectionId.inNodeId;
        connectionInfo.portTypeIn = PortType::In;
        connectionInfo.portTypeOut = PortType::Out;
        connectionInfo.templateName = dialogPair.second; // Store the saved template name

        connections.push_back(connectionInfo);
    }

    return connections;
}


void BasicGraphicsScene::setOrientation(Qt::Orientation const orientation)
{
    if (_orientation != orientation) {
        _orientation = orientation;

        switch (_orientation) {
        case Qt::Horizontal:
            _nodeGeometry = std::make_unique<DefaultHorizontalNodeGeometry>(_graphModel);
            break;

        case Qt::Vertical:
            _nodeGeometry = std::make_unique<DefaultVerticalNodeGeometry>(_graphModel);
            break;
        }

        onModelReset();
    }
}

QMenu *BasicGraphicsScene::createSceneMenu(QPointF const scenePos)
{
    Q_UNUSED(scenePos);
    return nullptr;
}

void BasicGraphicsScene::traverseGraphAndPopulateGraphicsObjects()
{
    auto allNodeIds = _graphModel.allNodeIds();

    // First create all the nodes.
    for (NodeId const nodeId : allNodeIds) {
        _nodeGraphicsObjects[nodeId] = std::make_unique<NodeGraphicsObject>(*this, nodeId);
    }

    // Then for each node check output connections and insert them.
    for (NodeId const nodeId : allNodeIds) {
        auto nOutPorts = _graphModel.nodeData<PortCount>(nodeId, NodeRole::OutPortCount);

        for (PortIndex index = 0; index < nOutPorts; ++index) {
            auto const &outConnectionIds = _graphModel.connections(nodeId, PortType::Out, index);

            for (auto cid : outConnectionIds) {
                _connectionGraphicsObjects[cid] = std::make_unique<ConnectionGraphicsObject>(*this,
                                                                                             cid);
            }
        }
    }
}

void BasicGraphicsScene::updateAttachedNodes(ConnectionId const connectionId,
                                             PortType const portType)
{
    auto node = nodeGraphicsObject(getNodeId(portType, connectionId));

    if (node) {
        node->update();
    }
}

void BasicGraphicsScene::removeDialog(ConnectionId const connectionId) {
    auto it = _dialogs.find(connectionId);
    if (it != _dialogs.end()) {
        // Закрываем диалог, если он открыт
        it->second.first->close();
        //it->second->close();
        // Удаляем диалог из контейнера
        _dialogs.erase(it);
    }
}

void BasicGraphicsScene::addTextUnderConnection(ConnectionId connectionId, const QString& templateText) {
    // Получаем соединение по его идентификатору
    auto connection = _connectionGraphicsObjects.find(connectionId);
    if (connection != _connectionGraphicsObjects.end()) {
        // Получаем объект соединения
        ConnectionGraphicsObject* connectionObject = connection->second.get();

        // Получаем конечные точки соединения
        //QPointF outPoint = connectionObject->out();
        QPointF inPoint = connectionObject->in();

        // Рассчитываем среднюю точку для размещения текста
        QPointF textPosition = inPoint;
        textPosition.setX(textPosition.x() - 50);
        textPosition.setY(textPosition.y() + 1); // Сдвигаем немного вниз

        // Создаем текстовый элемент
        QGraphicsTextItem* textItem = new QGraphicsTextItem(templateText);
        textItem->setPos(textPosition);
        textItem->setDefaultTextColor(Qt::white); // Устанавливаем цвет текста
        textItem->setFont(QFont("Arial", 10)); // Установливаем шрифт и размер

        // Добавляем текстовый элемент на сцену
        this->addItem(textItem);

        // Сохраняем текстовый элемент в структуре
        _textItems[connectionId] = textItem; // Сохраняем текст под соединением

        // Подключаем сигнал перемещения соединения к обновлению текста
        QPointer<QGraphicsTextItem> textItemPtr(textItem);
        connect(connectionObject, &ConnectionGraphicsObject::positionChanged, this, [textItemPtr, connectionObject]() {
            if (textItemPtr) { // Проверяем, существует ли текстовый элемент
                QPointF inPoint = connectionObject->in();
                QPointF textPosition = inPoint;
                textPosition.setX(textPosition.x() - 50);
                textPosition.setY(textPosition.y() + 1); // Сдвигаем немного вниз
                textItemPtr->setPos(textPosition);
            }
        });
    }
}

void BasicGraphicsScene::onConnectionDeleted(ConnectionId const connectionId)
{
    auto it = _connectionGraphicsObjects.find(connectionId);
    if (it != _connectionGraphicsObjects.end()) {
        _connectionGraphicsObjects.erase(it);
    }

    // Удаляем текстовый элемент, если он существует
    auto textIt = _textItems.find(connectionId);
    if (textIt != _textItems.end()) {
        QGraphicsTextItem* textItem = textIt.value();
        this->removeItem(textItem); // Удаляем текстовый элемент из сцены
        delete textItem; // Освобождаем память
        _textItems.erase(textIt); // Удаляем из структуры
    }

    // TODO: do we need it?
    if (_draftConnection && _draftConnection->connectionId() == connectionId) {
        _draftConnection.reset();
    }

    updateAttachedNodes(connectionId, PortType::Out);
    updateAttachedNodes(connectionId, PortType::In);

    // Удаляем диалоговое окно, если оно существует
    removeDialog(connectionId);

    Q_EMIT modified(this);
}

void BasicGraphicsScene::onConnectionCreated(ConnectionId const connectionId)
{
    // Создаем объект соединения
    auto connectionObject = std::make_unique<ConnectionGraphicsObject>(*this, connectionId);

    // Устанавливаем обработчик двойного клика
    connect(connectionObject.get(), &ConnectionGraphicsObject::doubleClicked, this, [this, connectionId]() {
        openDialog(connectionId);
    });

    _connectionGraphicsObjects[connectionId] = std::move(connectionObject);

    updateAttachedNodes(connectionId, PortType::Out);
    updateAttachedNodes(connectionId, PortType::In);
}

void BasicGraphicsScene::openDialog(ConnectionId const connectionId)
{
    if (_dialogs.find(connectionId) == _dialogs.end())
    {
        // Если диалог не существует, создаем его
        auto dialog = std::make_unique<QDialog>();
        dialog->setWindowTitle("Select template");
        dialog->setModal(true);

        QVBoxLayout *layout = new QVBoxLayout(dialog.get());
        QLabel *label = new QLabel("Select template:");
        layout->addWidget(label);

        QListWidget *listWidget = new QListWidget();
        layout->addWidget(listWidget);

        QStringList headers;
        for (const auto &headerRecord : m_record)
        {
            headers = headerRecord.getColumnHeaders();
            for (int i = 0; i < headers.size(); ++i)
            {
                listWidget->addItem(headers[i]);
            }
        }

        QPushButton *okButton = new QPushButton("OK");
        layout->addWidget(okButton);

        // Подключаем сигнал кнопки OK к слоту, который будет обрабатывать выбор
        connect(okButton, &QPushButton::clicked, [this, connectionId, dialog = dialog.get(), listWidget]()
        {
            // Получаем выбранный элемент из listWidget
            QListWidgetItem *selectedItem = listWidget->currentItem();
            if (selectedItem) {
                QString selectedTemplate = selectedItem->text();
                // Сохраняем выбранный шаблон в _dialogs
                _dialogs[connectionId].second = selectedTemplate;
                //qDebug() << "Selected template for connection" << connectionId << ":" << selectedTemplate;

                // Получаем цвет для выбранного заголовка
                QColor selectedColor = getColorForHeader(selectedTemplate);

                // Устанавливаем цвет соединения
                if (auto connectionObject = connectionGraphicsObject(connectionId)) {
                    connectionObject->setConnectionColor(selectedColor); // Устанавливаем цвет
                    connectionObject->update(); // Обновляем отображение
                }

                // Обновляем соединение, чтобы использовать новый цвет
                if (auto connectionObject = connectionGraphicsObject(connectionId)) {
                    connectionObject->update();
                }

                addTextUnderConnection(connectionId, selectedTemplate);
            }
            dialog->accept(); // Закрываем диалог
        });
        // Сохраняем диалог в контейнер
        _dialogs[connectionId] = std::make_pair(std::move(dialog), QString());
    }
    // Показываем диалоговое окно для данного соединения
    _dialogs[connectionId].first->show();

    Q_EMIT modified(this);
}

void BasicGraphicsScene::onNodeDeleted(NodeId const nodeId)
{
    auto it = _nodeGraphicsObjects.find(nodeId);
    if (it != _nodeGraphicsObjects.end()) {
        _nodeGraphicsObjects.erase(it);

        Q_EMIT modified(this);
    }
}

void BasicGraphicsScene::onNodeCreated(NodeId const nodeId)
{
    _nodeGraphicsObjects[nodeId] = std::make_unique<NodeGraphicsObject>(*this, nodeId);

    Q_EMIT modified(this);
}

void BasicGraphicsScene::onNodePositionUpdated(NodeId const nodeId)
{
    auto node = nodeGraphicsObject(nodeId);
    if (node) {
        node->setPos(_graphModel.nodeData(nodeId, NodeRole::Position).value<QPointF>());
        node->update();
        _nodeDrag = true;
    }
}

void BasicGraphicsScene::onNodeUpdated(NodeId const nodeId)
{
    auto node = nodeGraphicsObject(nodeId);

    if (node) {
        node->setGeometryChanged();

        _nodeGeometry->recomputeSize(nodeId);

        node->updateQWidgetEmbedPos();
        node->update();
        node->moveConnections();
    }
}

void BasicGraphicsScene::onNodeClicked(NodeId const nodeId)
{
    if (_nodeDrag) {
        Q_EMIT nodeMoved(nodeId, _graphModel.nodeData(nodeId, NodeRole::Position).value<QPointF>());
        Q_EMIT modified(this);
    }
    _nodeDrag = false;
}

void BasicGraphicsScene::onModelReset()
{
    _connectionGraphicsObjects.clear();
    _nodeGraphicsObjects.clear();

    clear();

    traverseGraphAndPopulateGraphicsObjects();
}

} // namespace QtNodes
