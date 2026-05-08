let socket = null;
let reconnectTimer = null;

function connectSocket() {
    if (socket && socket.readyState === WebSocket.OPEN) return;
    socket = new WebSocket(`ws://${location.hostname}/ws`);

    socket.onopen = function() {
        console.log("[open] Connection established");
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
        if (!reconnectTimer) {
            reconnectTimer = setTimeout(connectSocket, 1000);
        }
    };

    socket.onerror = function(error) {
        console.log(`[error] ${error.message}`);
    };
}

connectSocket();

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
        return;
    }

    if (payload.type === 'logs') {
        renderJson('log-output', payload.data);
        return;
    }
}

function renderJson(elementId, data) {
    const el = document.getElementById(elementId);
    if (!el) return;
    el.textContent = JSON.stringify(data, null, 2);
}

async function clearLogs() {
    try {
        await fetch('/api/logs/clear', { method: 'POST' });
        document.getElementById('log-output').textContent = '(cleared)';
    } catch (err) {
        document.getElementById('log-output').textContent = 'Clear error';
    }
}
