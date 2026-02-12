#include "NetworkManager.h"

#include <AsyncJson.h>

#include "../Logic/SettingsManager.h"
#include "../Hardware/Microphone.h"
#include "../Logic/DebugLogger.h"
#include "../Logic/GameManager.h"
#include "../Logic/ModeCompetition.h"
#include "../Hardware/Target.h"
#include "../Hardware/InputManager.h"
#include "../Hardware/RelayManager.h"

static int clampInt(int value, int minValue, int maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

static void sanitizeConfig(Config& cfg) {
    cfg.micThreshold = clampInt(cfg.micThreshold, 100, 4095);
    cfg.t1Delay = clampInt(cfg.t1Delay, 0, 60000);
    cfg.t1Duration = clampInt(cfg.t1Duration, 100, 60000);
    cfg.t2Delay = clampInt(cfg.t2Delay, 0, 60000);
    cfg.t2Duration = clampInt(cfg.t2Duration, 100, 60000);
    cfg.t3Delay = clampInt(cfg.t3Delay, 0, 60000);
    cfg.t3Duration = clampInt(cfg.t3Duration, 100, 60000);
    cfg.targetTimeoutMs = clampInt(cfg.targetTimeoutMs, 500, 120000);
}

static const char* toStateString(TargetState state) {
    switch (state) {
        case HIDDEN: return "HIDDEN";
        case MOVING_SHOW: return "MOVING_SHOW";
        case SHOWN: return "SHOWN";
        case MOVING_HIDE: return "MOVING_HIDE";
        case ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

static const char* toCompetitionStateString(CompetitionState state) {
    switch (state) {
        case WAITING_START: return "WAITING_START";
        case WAITING_MIC: return "WAITING_MIC";
        case RUNNING_SEQUENCE: return "RUNNING_SEQUENCE";
        case FINISHED: return "FINISHED";
        default: return "UNKNOWN";
    }
}

NetworkManager::NetworkManager() : _server(80), _ws("/ws") {
}

void NetworkManager::setSettingsManager(SettingsManager* settingsManager) {
    _settingsManager = settingsManager;
}

void NetworkManager::setMicrophone(Microphone* microphone) {
    _microphone = microphone;
}

void NetworkManager::setGameManager(GameManager* gameManager) {
    _gameManager = gameManager;
}

void NetworkManager::setTargets(Target* t1, Target* t2, Target* t3) {
    _t1 = t1;
    _t2 = t2;
    _t3 = t3;
}

void NetworkManager::setInputManager(InputManager* inputManager) {
    _inputManager = inputManager;
}

void NetworkManager::setRelayManager(RelayManager* relayManager) {
    _relayManager = relayManager;
}

void NetworkManager::begin() {
    // Initialize LittleFS
    if(!LittleFS.begin(false)){
        Serial.println("An Error has occurred while mounting LittleFS");
        return;
    }

    // Start WiFi AP
    WiFi.softAP("TARCZOWNIX");
    Serial.println("AP Started: TARCZOWNIX");
    Serial.println(WiFi.softAPIP());

    // Start DNS Server
    _dnsServer.start(53, "*", WiFi.softAPIP());

    // Setup WebSocket
    _ws.onEvent(std::bind(&NetworkManager::_onEvent, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5, std::placeholders::_6));
    _server.addHandler(&_ws);

    // Setup Routes
    _setupRoutes();

    // Start Server
    _server.begin();
}

void NetworkManager::update() {
    _dnsServer.processNextRequest();
    _ws.cleanupClients();
}

void NetworkManager::setCommandCallback(CommandCallback callback) {
    _commandCallback = callback;
}

void NetworkManager::broadcastStatus(String json) {
    _ws.textAll(json);
}

void NetworkManager::broadcastEvent(const char* type, const String& payload) {
    String out = String("{\"type\":\"") + type + "\",\"data\":" + payload + "}";
    _ws.textAll(out);
}

String NetworkManager::getDiagnosticsJson() {
    JsonDocument doc;
    doc["uptimeMs"] = millis();

    if (_gameManager) {
        doc["mode"] = _gameManager->getModeName();
        if (_gameManager->getCompetitionMode()) {
            doc["competitionState"] = toCompetitionStateString(_gameManager->getCompetitionMode()->getState());
        }
    }

    if (_relayManager) {
        doc["relayShadow"] = _relayManager->getShadowRegister();
    }

    if (_inputManager) {
        doc["inputRaw"] = _inputManager->getRaw();
        doc["inputStable"] = _inputManager->getStable();
    }

    if (_t1 && _t2 && _t3) {
        JsonObject targets = doc["targets"].to<JsonObject>();
        JsonObject t1 = targets["1"].to<JsonObject>();
        t1["state"] = toStateString(_t1->getState());
        t1["pendingMove"] = _t1->isPendingMove();
        t1["pendingShow"] = _t1->isPendingShow();

        JsonObject t2 = targets["2"].to<JsonObject>();
        t2["state"] = toStateString(_t2->getState());
        t2["pendingMove"] = _t2->isPendingMove();
        t2["pendingShow"] = _t2->isPendingShow();

        JsonObject t3 = targets["3"].to<JsonObject>();
        t3["state"] = toStateString(_t3->getState());
        t3["pendingMove"] = _t3->isPendingMove();
        t3["pendingShow"] = _t3->isPendingShow();
    }

    String out;
    serializeJson(doc, out);
    return out;
}

String NetworkManager::getLogsJson() {
    return DebugLogger::instance().getJson();
}

void NetworkManager::_setupRoutes() {
    _server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    _server.on("/api/settings", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (!_settingsManager) {
            request->send(503, "application/json", "{\"error\":\"settings_unavailable\"}");
            return;
        }
        request->send(200, "application/json", _settingsManager->getJson());
    });

    auto* settingsPost = new AsyncCallbackJsonWebHandler("/api/settings", [this](AsyncWebServerRequest* request, JsonVariant& json) {
        if (!_settingsManager) {
            request->send(503, "application/json", "{\"error\":\"settings_unavailable\"}");
            return;
        }

        JsonObject obj = json.as<JsonObject>();
        Config& cfg = _settingsManager->getConfig();

        if (obj["micThreshold"].is<int>()) cfg.micThreshold = obj["micThreshold"].as<int>();
        if (obj["t1Delay"].is<int>()) cfg.t1Delay = obj["t1Delay"].as<int>();
        if (obj["t1Duration"].is<int>()) cfg.t1Duration = obj["t1Duration"].as<int>();
        if (obj["t2Delay"].is<int>()) cfg.t2Delay = obj["t2Delay"].as<int>();
        if (obj["t2Duration"].is<int>()) cfg.t2Duration = obj["t2Duration"].as<int>();
        if (obj["t3Delay"].is<int>()) cfg.t3Delay = obj["t3Delay"].as<int>();
        if (obj["t3Duration"].is<int>()) cfg.t3Duration = obj["t3Duration"].as<int>();
        if (obj["targetTimeoutMs"].is<int>()) cfg.targetTimeoutMs = obj["targetTimeoutMs"].as<int>();

        sanitizeConfig(cfg);

        _settingsManager->save();

        if (_microphone) {
            _microphone->setThreshold(cfg.micThreshold);
        }

        if (_gameManager) {
            _gameManager->applyConfig(cfg);
        }

        request->send(200, "application/json", _settingsManager->getJson());
    });
    _server.addHandler(settingsPost);

    _server.on("/mic-status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (!_microphone) {
            request->send(503, "application/json", "{\"error\":\"microphone_unavailable\"}");
            return;
        }

        JsonDocument doc;
        doc["adcPin"] = _microphone->getAdcPin();
        doc["threshold"] = _microphone->getThreshold();
        doc["baseline"] = _microphone->getBaseline();
        doc["value"] = _microphone->getLastValue();
        doc["peak"] = _microphone->getLastPeak();
        doc["updatedMs"] = _microphone->getLastUpdateMs();

        String out;
        serializeJson(doc, out);
        request->send(200, "application/json", out);
    });

    _server.on("/api/diagnostics", HTTP_GET, [this](AsyncWebServerRequest* request) {
        request->send(200, "application/json", getDiagnosticsJson());
    });

    _server.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "application/json", DebugLogger::instance().getJson());
    });

    _server.on("/api/logs/clear", HTTP_POST, [](AsyncWebServerRequest* request) {
        DebugLogger::instance().clear();
        request->send(200, "application/json", "{\"ok\":true}");
    });

    _server.onNotFound([](AsyncWebServerRequest *request) {
        request->redirect("/");
    });
}

void NetworkManager::_onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_DATA) {
        AwsFrameInfo *info = (AwsFrameInfo*)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
            _handleWebSocketMessage(arg, data, len);
        }
    }
}

void NetworkManager::_handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, data, len);

    if (error) {
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.f_str());
        DebugLogger::instance().log("WS json error: %s", error.f_str());
        return;
    }

    int targetId = doc["target"];
    String cmd = doc["cmd"];

    DebugLogger::instance().log("WS cmd: target=%d cmd=%s", targetId, cmd.c_str());
    if (_commandCallback) {
        _commandCallback(targetId, cmd);
    }
}
