#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "../Logic/SettingsManager.h"

class SettingsManager;
class Microphone;
class GameManager;
class Target;
class InputManager;
class RelayManager;

class NetworkManager {
public:
    NetworkManager();
    void begin();
    void update();
    bool getNextCommand(int& targetId, String& action);
    bool takePendingConfig(Config& config);
    void broadcastStatus(String json);
    void broadcastEvent(const char* type, const String& payload);
    String getDiagnosticsJson();
    String getLogsJson();

    void setSettingsManager(SettingsManager* settingsManager);
    void setMicrophone(Microphone* microphone);
    void setGameManager(GameManager* gameManager);
    void setTargets(Target* t1, Target* t2, Target* t3);
    void setInputManager(InputManager* inputManager);
    void setRelayManager(RelayManager* relayManager);

private:
    DNSServer _dnsServer;
    AsyncWebServer _server;
    AsyncWebSocket _ws;
    portMUX_TYPE _queueMux;

    struct QueuedCommand {
        int targetId;
        char action[32];
    };

    static const size_t COMMAND_QUEUE_SIZE = 8;
    QueuedCommand _commandQueue[COMMAND_QUEUE_SIZE];
    size_t _commandHead;
    size_t _commandTail;
    size_t _commandCount;

    bool _hasPendingConfig;
    Config _pendingConfig;

    SettingsManager* _settingsManager = nullptr;
    Microphone* _microphone = nullptr;
    GameManager* _gameManager = nullptr;
    Target* _t1 = nullptr;
    Target* _t2 = nullptr;
    Target* _t3 = nullptr;
    InputManager* _inputManager = nullptr;
    RelayManager* _relayManager = nullptr;

    void _setupRoutes();
    bool _enqueueCommand(int targetId, const String& action);
    void _enqueueConfig(const Config& config);
    void _handleWebSocketMessage(void *arg, uint8_t *data, size_t len);
    void _onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
};
