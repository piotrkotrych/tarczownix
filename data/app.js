let socket = null;
let reconnectTimer = null;

const SETTING_FIELDS = [
    'micThreshold',
    'targetTimeoutMs',
    't1Delay', 't1Duration',
    't2Delay', 't2Duration',
    't3Delay', 't3Duration'
];

function setConnectionState(online) {
    const el = document.getElementById('conn-status');
    if (!el) return;
    el.textContent = online ? 'connected' : 'disconnected';
    el.classList.toggle('online', online);
}

function scheduleReconnect() {
    if (reconnectTimer) return;
    reconnectTimer = setTimeout(() => {
        reconnectTimer = null;
        connectSocket();
    }, 1000);
}

function connectSocket() {
    if (socket && (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING)) return;
    // location.host (not hostname) so a non-default port survives.
    socket = new WebSocket(`ws://${location.host}/ws`);

    socket.onopen = function() {
        console.log("[open] Connection established");
        setConnectionState(true);
        if (reconnectTimer) {
            clearTimeout(reconnectTimer);
            reconnectTimer = null;
        }
    };

    socket.onmessage = function(event) {
        try {
            const payload = JSON.parse(event.data);
            if (payload && payload.type) {
                handleTypedMessage(payload);
            } else if (payload) {
                for (const [id, state] of Object.entries(payload)) {
                    updateStatus(id, state);
                }
            }
        } catch (err) {
            console.log("[message] Invalid JSON:", event.data);
        }
    };

    socket.onclose = function(event) {
        if (event.wasClean) {
            console.log(`[close] Connection closed cleanly, code=${event.code} reason=${event.reason}`);
        } else {
            console.log('[close] Connection died');
        }
        setConnectionState(false);
        scheduleReconnect();
    };

    socket.onerror = function() {
        console.log('[error] WebSocket error');
        setConnectionState(false);
        // onerror is always followed by onclose, which handles the retry.
    };
}

function sendCommand(targetId, cmd) {
    const msg = JSON.stringify({ target: targetId, cmd: cmd });
    if (socket && socket.readyState === WebSocket.OPEN) {
        socket.send(msg);
    } else {
        console.log("[send] Socket not ready");
    }
}

function setMode(modeName) {
    sendCommand(0, `mode:${modeName}`);
}

function triggerGunshot() {
    sendCommand(0, 'gunshot');
}

function createTargetCard(id) {
    const container = document.getElementById('targets');
    const card = document.createElement('div');
    card.className = 'card';
    card.innerHTML = `
        <h2>Target ${id}</h2>
        <div>
            <span id="status-${id}" class="status"></span>
            <span id="status-text-${id}" class="status-text">UNKNOWN</span>
        </div><br>
        <button class="show-btn" onclick="sendCommand(${id}, 'show')">SHOW</button>
        <button class="hide-btn" onclick="sendCommand(${id}, 'hide')">HIDE</button>
        <button class="stop-btn" onclick="sendCommand(${id}, 'stop')">STOP</button>
        <button class="reset-btn" onclick="sendCommand(${id}, 'reset')">RESET</button>
    `;
    container.appendChild(card);
}

function updateStatus(id, state) {
    const statusDiv = document.getElementById(`status-${id}`);
    const statusText = document.getElementById(`status-text-${id}`);
    if (!statusDiv) return;

    let color = 'gray';
    if (state === 'SHOWN') color = 'green';
    else if (state === 'HIDDEN') color = 'red';
    else if (state === 'MOVING_SHOW' || state === 'MOVING_HIDE') color = 'orange';
    else if (state === 'STOPPED') color = 'gray';
    else if (state === 'ERROR') color = 'black';

    statusDiv.style.backgroundColor = color;
    if (statusText) statusText.textContent = state;
}

// Initialize 3 targets
for (let i = 1; i <= 3; i++) {
    createTargetCard(i);
}

function handleTypedMessage(payload) {
    if (payload.type === 'status' && payload.data) {
        for (const [id, state] of Object.entries(payload.data)) {
            updateStatus(id, state);
        }
        return;
    }

    if (payload.type === 'diagnostics') {
        renderJson('diag-output', payload.data);
        if (payload.data) {
            setText('current-mode', payload.data.mode || '-');
            setText('current-comp-state', payload.data.competitionState ? `(${payload.data.competitionState})` : '');
        }
        return;
    }

    if (payload.type === 'logs') {
        renderJson('log-output', payload.data);
    }
}

function setText(elementId, value) {
    const el = document.getElementById(elementId);
    if (el) el.textContent = value;
}

function renderJson(elementId, data) {
    const el = document.getElementById(elementId);
    if (!el) return;
    el.textContent = JSON.stringify(data, null, 2);
}

async function loadSettings() {
    try {
        const response = await fetch('/api/settings');
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        const cfg = await response.json();
        for (const key of SETTING_FIELDS) {
            const input = document.getElementById(key);
            if (input && cfg[key] !== undefined) input.value = cfg[key];
        }
        setText('settings-msg', 'loaded');
    } catch (err) {
        setText('settings-msg', `load failed: ${err.message}`);
    }
}

async function saveSettings() {
    const body = {};
    for (const key of SETTING_FIELDS) {
        const input = document.getElementById(key);
        if (!input) continue;
        const value = parseInt(input.value, 10);
        if (Number.isNaN(value)) {
            setText('settings-msg', `${key} is not a number`);
            return;
        }
        body[key] = value;
    }

    try {
        const response = await fetch('/api/settings', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body)
        });
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        // The device clamps out-of-range values, so show what it actually stored.
        const cfg = await response.json();
        for (const key of SETTING_FIELDS) {
            const input = document.getElementById(key);
            if (input && cfg[key] !== undefined) input.value = cfg[key];
        }
        setText('settings-msg', 'saved');
    } catch (err) {
        setText('settings-msg', `save failed: ${err.message}`);
    }
}

async function clearLogs() {
    try {
        await fetch('/api/logs/clear', { method: 'POST' });
        document.getElementById('log-output').textContent = '(cleared)';
    } catch (err) {
        document.getElementById('log-output').textContent = 'Clear error';
    }
}

connectSocket();
loadSettings();
