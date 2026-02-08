#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

class SettingsManager;
class Microphone;
class GameManager;
class Target;
class InputManager;
class RelayManager;

class NetworkManager {
public:
    using CommandCallback = std::function<void(int targetId, String action)>;

    NetworkManager();
    void begin();
    void update();
    void setCommandCallback(CommandCallback callback);
    void broadcastStatus(String json);

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
    CommandCallback _commandCallback;

    SettingsManager* _settingsManager = nullptr;
    Microphone* _microphone = nullptr;
    GameManager* _gameManager = nullptr;
    Target* _t1 = nullptr;
    Target* _t2 = nullptr;
    Target* _t3 = nullptr;
    InputManager* _inputManager = nullptr;
    RelayManager* _relayManager = nullptr;

    void _setupRoutes();
    void _handleWebSocketMessage(void *arg, uint8_t *data, size_t len);
    void _onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
};
