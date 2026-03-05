/* AADongle Baby Monitor — Phone Web UI */

const API_BASE = window.location.origin;
const STREAM_URL = `${API_BASE}/stream`;

let currentZoom = 1.0;
const ZOOM_MIN = 1.0;
const ZOOM_MAX = 3.4;
const ZOOM_STEP = 0.2;
let irEnabled = false;
let lastBabyState = 'unknown';
let alertDismissedForState = null;

/* Elements */
const stream = document.getElementById('stream');
const statusIndicator = document.getElementById('status-indicator');
const statusText = document.getElementById('status-text');
const zoomDisplay = document.getElementById('zoom-level');
const btnZoomIn = document.getElementById('btn-zoom-in');
const btnZoomOut = document.getElementById('btn-zoom-out');
const btnIR = document.getElementById('btn-ir');
const btnMode = document.getElementById('btn-mode');
const selMode = document.getElementById('sel-mode');
const chkIR = document.getElementById('chk-ir');
const btnOTA = document.getElementById('btn-ota');
const settingsPanel = document.getElementById('settings-panel');
const aiAlert = document.getElementById('ai-alert');
const aiAlertText = document.getElementById('ai-alert-text');
const aiAlertDismiss = document.getElementById('ai-alert-dismiss');

/* Stream connection */
function connectStream() {
    stream.src = STREAM_URL + '?t=' + Date.now();
    stream.onload = () => {
        statusIndicator.classList.add('connected');
        statusText.textContent = 'Live';
    };
    stream.onerror = () => {
        statusIndicator.classList.remove('connected');
        statusText.textContent = 'Reconnecting...';
        setTimeout(connectStream, 2000);
    };
}

/* API calls */
async function apiCall(endpoint, data) {
    try {
        const resp = await fetch(`${API_BASE}/api${endpoint}`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(data),
        });
        return await resp.json();
    } catch (e) {
        console.error('API error:', e);
        return null;
    }
}

function updateZoom(newZoom) {
    currentZoom = Math.max(ZOOM_MIN, Math.min(ZOOM_MAX, newZoom));
    zoomDisplay.textContent = currentZoom.toFixed(1) + 'x';
    apiCall('/zoom', { level: currentZoom });
}

/* Button handlers */
btnZoomIn.addEventListener('click', () => updateZoom(currentZoom + ZOOM_STEP));
btnZoomOut.addEventListener('click', () => updateZoom(currentZoom - ZOOM_STEP));

btnIR.addEventListener('click', () => {
    irEnabled = !irEnabled;
    btnIR.classList.toggle('active', irEnabled);
    chkIR.checked = irEnabled;
    apiCall('/ir', { enabled: irEnabled });
});

btnMode.addEventListener('click', () => {
    settingsPanel.classList.toggle('hidden');
});

selMode.addEventListener('change', () => {
    apiCall('/mode', { mode: parseInt(selMode.value) });
});

chkIR.addEventListener('change', () => {
    irEnabled = chkIR.checked;
    btnIR.classList.toggle('active', irEnabled);
    apiCall('/ir', { enabled: irEnabled });
});

btnOTA.addEventListener('click', async () => {
    btnOTA.textContent = 'Checking...';
    const result = await apiCall('/ota/check', {});
    if (result && result.available) {
        if (confirm(`Update available: ${result.version}. Install?`)) {
            apiCall('/ota/install', {});
            btnOTA.textContent = 'Installing...';
        }
    } else {
        btnOTA.textContent = 'Up to date';
        setTimeout(() => { btnOTA.textContent = 'Check Update'; }, 2000);
    }
});

/* Pinch-to-zoom on touch devices */
let initialDistance = 0;
let initialZoom = 1.0;

stream.addEventListener('touchstart', (e) => {
    if (e.touches.length === 2) {
        e.preventDefault();
        const dx = e.touches[0].clientX - e.touches[1].clientX;
        const dy = e.touches[0].clientY - e.touches[1].clientY;
        initialDistance = Math.hypot(dx, dy);
        initialZoom = currentZoom;
    }
}, { passive: false });

stream.addEventListener('touchmove', (e) => {
    if (e.touches.length === 2) {
        e.preventDefault();
        const dx = e.touches[0].clientX - e.touches[1].clientX;
        const dy = e.touches[0].clientY - e.touches[1].clientY;
        const distance = Math.hypot(dx, dy);
        const scale = distance / initialDistance;
        updateZoom(initialZoom * scale);
    }
}, { passive: false });

/* AI alert popup */
const AI_ALERTS = {
    sleeping: { text: 'Baby appears to be sleeping', css: 'alert-sleeping' },
    absent:   { text: 'Baby not detected in frame', css: 'alert-absent' },
    alert:    { text: 'Alert: Check on baby!', css: 'alert-distress' },
};

function showAiAlert(state) {
    const info = AI_ALERTS[state];
    if (!info) return;
    aiAlert.className = info.css;
    aiAlertText.textContent = info.text;
}

function hideAiAlert() {
    aiAlert.className = 'hidden';
}

function handleBabyState(state) {
    if (state === lastBabyState) return;
    lastBabyState = state;
    /* New state — reset dismissal so popup can appear */
    if (alertDismissedForState !== state) {
        alertDismissedForState = null;
    }
    if (AI_ALERTS[state] && alertDismissedForState !== state) {
        showAiAlert(state);
    } else {
        hideAiAlert();
    }
}

aiAlertDismiss.addEventListener('click', () => {
    alertDismissedForState = lastBabyState;
    hideAiAlert();
});

/* Init */
connectStream();

/* Periodic status check */
setInterval(async () => {
    try {
        const resp = await fetch(`${API_BASE}/api/status`);
        if (resp.ok) {
            const data = await resp.json();
            document.getElementById('fw-version').textContent = data.version || 'v1.0.0';
            if (data.ai && data.baby_state) {
                handleBabyState(data.baby_state);
            }
        }
    } catch (e) { /* ignore */ }
}, 3000);
