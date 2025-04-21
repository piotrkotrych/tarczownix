document.addEventListener('DOMContentLoaded', () => {
    const systemRunningEl = document.getElementById('system-running');
    const systemIndicatorEl = document.getElementById('system-indicator');
    const startBtn = document.getElementById('start-btn');
    const stopBtn = document.getElementById('stop-btn');
    const delayForm = document.getElementById('delay-form');
    const delayInputsContainer = document.getElementById('delay-inputs');
    const loadBtn = document.getElementById('load-btn');
    const liveStateContainer = document.getElementById('live-state');

    let pairCount = 0; // Will be determined from status

    // --- API Fetch Functions ---
    const fetchData = async (url, options = {}) => {
        try {
            const response = await fetch(url, options);
            if (!response.ok) {
                throw new Error(`HTTP error! status: ${response.status}`);
            }
            return await response.json();
        } catch (error) {
            console.error('Fetch error:', error);
            alert(`Error communicating with device: ${error.message}`);
            return null;
        }
    };

    const postData = async (url, data) => {
        return fetchData(url, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(data),
        });
    };

    // --- UI Update Functions ---
    const updateSystemStatus = (isRunning) => {
        systemRunningEl.textContent = isRunning ? 'Running' : 'Stopped';
        systemIndicatorEl.className = `status-indicator ${isRunning ? 'status-on' : 'status-off'}`;
        startBtn.disabled = isRunning;
        stopBtn.disabled = !isRunning;
    };

    const updateLiveStateUI = (pairs) => {
        liveStateContainer.innerHTML = ''; // Clear previous state
        if (!pairs || pairs.length === 0) {
            liveStateContainer.textContent = 'No pair data available.';
            return;
        }
        pairs.forEach((pair, index) => {
            const relayAState = pair.relayA_on ? 'ON' : 'OFF';
            const relayBState = pair.relayB_on ? 'ON' : 'OFF';
            const inputAState = pair.inputA_pressed ? 'PRESSED' : 'Released';
            const inputBState = pair.inputB_pressed ? 'PRESSED' : 'Released';

            const card = document.createElement('div');
            card.innerHTML = `
                <article class="pair-card">
                    <header><strong>Pair ${index}</strong></header>
                    <p>Relay A (Pin ${pair.relayA}): <span class="status-indicator ${pair.relayA_on ? 'status-on' : 'status-off'}"></span> ${relayAState}</p>
                    <p>Relay B (Pin ${pair.relayB}): <span class="status-indicator ${pair.relayB_on ? 'status-on' : 'status-off'}"></span> ${relayBState}</p>
                    <p>Input A (Pin ${pair.inputA}): ${inputAState}</p>
                    <p>Input B (Pin ${pair.inputB}): ${inputBState}</p>
                </article>
            `;
            liveStateContainer.appendChild(card);
        });
    };

    const updateDelayForm = (pairs) => {
        delayInputsContainer.innerHTML = ''; // Clear previous inputs
         if (!pairs || pairs.length === 0) {
            delayInputsContainer.textContent = 'No config data available.';
            return;
        }
        pairCount = pairs.length;
        pairs.forEach((pair, index) => {
            const div = document.createElement('div');
            div.innerHTML = `
                <label for="min_delay_${index}">Pair ${index} Min Delay (ms)</label>
                <input type="number" id="min_delay_${index}" name="min_delay_${index}" value="${pair.minDelayMs}" required min="0">
                <label for="max_delay_${index}">Pair ${index} Max Delay (ms)</label>
                <input type="number" id="max_delay_${index}" name="max_delay_${index}" value="${pair.maxDelayMs}" required min="0">
            `;
            delayInputsContainer.appendChild(div);
        });
    };

    // --- Fetch and Update ---
    const fetchAndUpdateStatus = async () => {
        const status = await fetchData('/status');
        if (status) {
            updateSystemStatus(status.sequenceRunning);
            updateLiveStateUI(status.pairs);
            // Only update form if it's empty, otherwise user might be editing
            if (delayInputsContainer.children.length <= 1) { // Check if only placeholder text exists
                 updateDelayForm(status.pairs);
            }
        } else {
            // Handle error case - maybe show disconnected status
            systemRunningEl.textContent = 'Error';
            systemIndicatorEl.className = 'status-indicator status-unknown';
            liveStateContainer.textContent = 'Error loading state.';
            delayInputsContainer.textContent = 'Error loading config.';
        }
    };

    // --- Event Listeners ---
    startBtn.addEventListener('click', async () => {
        startBtn.disabled = true; // Prevent double clicks
        await fetchData('/start');
        fetchAndUpdateStatus(); // Update UI immediately
    });

    stopBtn.addEventListener('click', async () => {
        stopBtn.disabled = true; // Prevent double clicks
        await fetchData('/stop');
        fetchAndUpdateStatus(); // Update UI immediately
    });

    delayForm.addEventListener('submit', async (event) => {
        event.preventDefault();
        const delays = [];
        for (let i = 0; i < pairCount; i++) {
            const minDelay = document.getElementById(`min_delay_${i}`).value;
            const maxDelay = document.getElementById(`max_delay_${i}`).value;
            delays.push({ minDelayMs: parseInt(minDelay), maxDelayMs: parseInt(maxDelay) });
        }
        console.log("Saving delays:", delays);
        const result = await postData('/update_delays', { pairs: delays });
        if (result && result.success) {
            alert('Delays saved successfully!');
            // Optionally save to ESP32 flash
            const saveConfirm = confirm("Save these delays permanently to device flash?");
            if (saveConfirm) {
                await fetchData('/save_settings');
                alert("Settings saved to flash.");
            }
        } else {
            alert('Failed to save delays.');
        }
    });

    loadBtn.addEventListener('click', async () => {
        const loadConfirm = confirm("Load delays from device flash? This will overwrite current form values.");
        if (loadConfirm) {
            await fetchData('/load_settings');
            alert("Settings loaded from flash. Fetching updated status...");
            fetchAndUpdateStatus(); // Fetch status again to update the form
        }
    });

    // --- Initial Load and Periodic Update ---
    fetchAndUpdateStatus(); // Initial load
    setInterval(fetchAndUpdateStatus, 2000); // Update status every 2 seconds
});