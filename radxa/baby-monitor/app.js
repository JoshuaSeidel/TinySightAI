/* Baby-Cam — Phone Web UI */

const API_BASE = window.location.origin;
const STREAM_URL = `${API_BASE}/stream`;

/* Mode names must match server.py / compositor control_channel.c */
const MODE_NAMES = [
    'full_aa',
    'full_carplay',
    'full_camera',
    'split_aa_cam',
    'split_cp_cam',
];

let irMode = 'auto';     /* off | on | auto */
let aiEnabled = true;
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
const chkAI = document.getElementById('chk-ai');
const btnOTA = document.getElementById('btn-ota');
const settingsPanel = document.getElementById('settings-panel');
const aiAlert = document.getElementById('ai-alert');
const aiAlertText = document.getElementById('ai-alert-text');
const aiAlertDismiss = document.getElementById('ai-alert-dismiss');
const modeLabel = document.getElementById('mode-label');

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

/* Button handlers — Zoom */
btnZoomIn.addEventListener('click', () => {
    apiCall('/zoom', { action: 'in' });
});
btnZoomOut.addEventListener('click', () => {
    apiCall('/zoom', { action: 'out' });
});

/* Pinch-to-zoom on touch devices */
let initialDistance = 0;
let pinchAccum = 0;

stream.addEventListener('touchstart', (e) => {
    if (e.touches.length === 2) {
        e.preventDefault();
        const dx = e.touches[0].clientX - e.touches[1].clientX;
        const dy = e.touches[0].clientY - e.touches[1].clientY;
        initialDistance = Math.hypot(dx, dy);
        pinchAccum = 0;
    }
}, { passive: false });

stream.addEventListener('touchmove', (e) => {
    if (e.touches.length === 2) {
        e.preventDefault();
        const dx = e.touches[0].clientX - e.touches[1].clientX;
        const dy = e.touches[0].clientY - e.touches[1].clientY;
        const distance = Math.hypot(dx, dy);
        const delta = distance - initialDistance;
        /* Send zoom in/out commands as pinch crosses thresholds */
        pinchAccum += delta;
        initialDistance = distance;
        if (pinchAccum > 40) {
            apiCall('/zoom', { action: 'in' });
            pinchAccum = 0;
        } else if (pinchAccum < -40) {
            apiCall('/zoom', { action: 'out' });
            pinchAccum = 0;
        }
    }
}, { passive: false });

/* IR toggle button (cycles: auto → on → off → auto) */
btnIR.addEventListener('click', () => {
    const cycle = { auto: 'on', on: 'off', off: 'auto' };
    irMode = cycle[irMode] || 'auto';
    btnIR.classList.toggle('active', irMode === 'on');
    btnIR.textContent = irMode === 'auto' ? 'IR' : irMode === 'on' ? 'IR+' : 'IR-';
    apiCall('/ir', { state: irMode });
});

/* Settings panel — IR checkbox (on/off only) */
chkIR.addEventListener('change', () => {
    irMode = chkIR.checked ? 'on' : 'off';
    btnIR.classList.toggle('active', irMode === 'on');
    apiCall('/ir', { state: irMode });
});

/* Mode select */
btnMode.addEventListener('click', () => {
    settingsPanel.classList.toggle('hidden');
});

selMode.addEventListener('change', () => {
    const modeName = MODE_NAMES[parseInt(selMode.value)] || 'full_aa';
    apiCall('/mode', { mode: modeName });
});

/* AI toggle */
if (chkAI) {
    chkAI.addEventListener('change', () => {
        aiEnabled = chkAI.checked;
        apiCall('/ai', { enabled: aiEnabled });
    });
}

/* OTA */
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

/* Sync UI with compositor status */
function syncStatus(data) {
    /* Zoom display */
    if (data.zoom != null) {
        const z = (data.zoom / 100).toFixed(1);
        zoomDisplay.textContent = z + 'x';
    }
    /* Mode select */
    const modeIdx = MODE_NAMES.indexOf(data.mode);
    if (modeIdx >= 0) selMode.value = modeIdx;
    /* Mode label in status bar */
    if (modeLabel) {
        const labels = {
            full_aa: 'AA', full_carplay: 'CarPlay', full_camera: 'Camera',
            split_aa_cam: 'AA+Cam', split_cp_cam: 'CP+Cam',
        };
        modeLabel.textContent = labels[data.mode] || '';
    }
    /* IR state */
    if (data.ir) {
        irMode = data.ir;
        chkIR.checked = (irMode === 'on');
        btnIR.classList.toggle('active', irMode === 'on');
    }
    /* AI state */
    if (chkAI && data.ai != null) {
        aiEnabled = data.ai;
        chkAI.checked = aiEnabled;
    }
    if (data.ai && data.baby_state) {
        handleBabyState(data.baby_state);
    }
    /* Firmware */
    document.getElementById('fw-version').textContent = data.version || 'v1.0.0';
}

/* Init */
connectStream();

/* Periodic status check */
setInterval(async () => {
    try {
        const resp = await fetch(`${API_BASE}/api/status`);
        if (resp.ok) {
            syncStatus(await resp.json());
        }
    } catch (e) { /* ignore */ }
}, 3000);
