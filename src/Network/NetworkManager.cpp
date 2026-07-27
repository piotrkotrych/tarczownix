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
        case STOPPED: return "STOPPED";
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

NetworkManager::NetworkManager()
    : _server(80), _ws("/ws"), _queueMux(portMUX_INITIALIZER_UNLOCKED),
      _commandHead(0), _commandTail(0), _commandCount(0), _hasPendingConfig(false),
      _filesystemReady(false) {
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
    // Normally already mounted by SettingsManager; retry here so a late failure only
    // costs the static assets. Bailing out at this point used to skip the AP entirely,
    // leaving the device unreachable with no way to diagnose it.
    _filesystemReady = SettingsManager::mountFilesystem();
    if (!_filesystemReady) {
        Serial.println("An Error has occurred while mounting LittleFS");
        DebugLogger::instance().log("LittleFS unavailable: web UI assets missing");
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

bool NetworkManager::_enqueueCommand(int targetId, const String& action) {
    char actionCopy[sizeof(_commandQueue[0].action)];
    action.toCharArray(actionCopy, sizeof(actionCopy));

    bool queued = false;
    portENTER_CRITICAL(&_queueMux);
    if (_commandCount < COMMAND_QUEUE_SIZE) {
        QueuedCommand& item = _commandQueue[_commandTail];
        item.targetId = targetId;
        strncpy(item.action, actionCopy, sizeof(item.action) - 1);
        item.action[sizeof(item.action) - 1] = '\0';
        _commandTail = (_commandTail + 1) % COMMAND_QUEUE_SIZE;
        _commandCount++;
        queued = true;
    }
    portEXIT_CRITICAL(&_queueMux);
    return queued;
}

bool NetworkManager::getNextCommand(int& targetId, String& action) {
    QueuedCommand item;
    bool hasCommand = false;

    portENTER_CRITICAL(&_queueMux);
    if (_commandCount > 0) {
        item = _commandQueue[_commandHead];
        _commandHead = (_commandHead + 1) % COMMAND_QUEUE_SIZE;
        _commandCount--;
        hasCommand = true;
    }
    portEXIT_CRITICAL(&_queueMux);

    if (!hasCommand) {
        return false;
    }

    targetId = item.targetId;
    action = item.action;
    return true;
}

void NetworkManager::_enqueueConfig(const Config& config) {
    portENTER_CRITICAL(&_queueMux);
    _pendingConfig = config;
    _hasPendingConfig = true;
    portEXIT_CRITICAL(&_queueMux);
}

bool NetworkManager::takePendingConfig(Config& config) {
    bool hasConfig = false;
    portENTER_CRITICAL(&_queueMux);
    if (_hasPendingConfig) {
        config = _pendingConfig;
        _hasPendingConfig = false;
        hasConfig = true;
    }
    portEXIT_CRITICAL(&_queueMux);
    return hasConfig;
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
    if (_filesystemReady) {
        _server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    }

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
        Config cfg = _settingsManager->getConfig();

        if (obj["micThreshold"].is<int>()) cfg.micThreshold = obj["micThreshold"].as<int>();
        if (obj["t1Delay"].is<int>()) cfg.t1Delay = obj["t1Delay"].as<int>();
        if (obj["t1Duration"].is<int>()) cfg.t1Duration = obj["t1Duration"].as<int>();
        if (obj["t2Delay"].is<int>()) cfg.t2Delay = obj["t2Delay"].as<int>();
        if (obj["t2Duration"].is<int>()) cfg.t2Duration = obj["t2Duration"].as<int>();
        if (obj["t3Delay"].is<int>()) cfg.t3Delay = obj["t3Delay"].as<int>();
        if (obj["t3Duration"].is<int>()) cfg.t3Duration = obj["t3Duration"].as<int>();
        if (obj["targetTimeoutMs"].is<int>()) cfg.targetTimeoutMs = obj["targetTimeoutMs"].as<int>();

        sanitizeConfig(cfg);

        _enqueueConfig(cfg);
        DebugLogger::instance().log("Settings update queued");

        request->send(200, "application/json", _settingsManager->toJson(cfg));
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

    // Captive-portal catch-all. Redirecting "/" itself (or anything at all when the
    // assets are missing) would bounce the browser between the same two URLs forever.
    _server.onNotFound([this](AsyncWebServerRequest* request) {
        if (!_filesystemReady || request->url() == "/") {
            request->send(200, "text/html",
                          "<h1>Tarczownix</h1><p>Web assets are not installed on the device "
                          "(run: pio run -t uploadfs).</p><p>API is available under /api/.</p>");
            return;
        }
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

    // Reject malformed frames instead of queueing a command with an empty action and a
    // target id that no mode knows what to do with.
    if (!doc["cmd"].is<const char*>()) {
        DebugLogger::instance().log("WS cmd dropped: missing 'cmd'");
        return;
    }

    String cmd = doc["cmd"].as<String>();
    cmd.trim();
    if (cmd.isEmpty()) {
        DebugLogger::instance().log("WS cmd dropped: empty 'cmd'");
        return;
    }

    const int targetId = doc["target"] | 0;
    if (targetId < 0 || targetId > 3) {
        DebugLogger::instance().log("WS cmd dropped: bad target=%d", targetId);
        return;
    }

    DebugLogger::instance().log("WS cmd: target=%d cmd=%s", targetId, cmd.c_str());
    if (!_enqueueCommand(targetId, cmd)) {
        DebugLogger::instance().log("WS cmd dropped: queue full");
    }
}
