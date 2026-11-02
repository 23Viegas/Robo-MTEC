// Estado central da interface; a placa continua sendo a fonte da verdade.
const state = {
  mode: 'manual',
  direction: 'CENTRO',
  speed: 55,
  connected: false,
  emergency: false,
  sensors: [0, 0, 0, 0],
  sensorsValidated: false,
  sensorSource: 'unvalidated',
  autoBlockReason: '',
  distanceMm: null,
  distanceAvailable: false,
  distanceModel: 'not_detected',
};

const $ = (selector) => document.querySelector(selector);
const joystick = $('#joystick');
const knob = $('#joystickKnob');
const sensorElements = [...document.querySelectorAll('.sensor')];
const directionButtons = [...document.querySelectorAll('.direction-btn')];
const directionMap = {
  FRENTE: 'forward',
  RÉ: 'backward',
  ESQUERDA: 'left',
  DIREITA: 'right',
  CENTRO: 'stop',
};
const reverseDirectionMap = {
  forward: 'FRENTE',
  backward: 'RÉ',
  left: 'ESQUERDA',
  right: 'DIREITA',
  stop: 'CENTRO',
};

function hasValidStateContract(data) {
  return Boolean(
    data &&
    Number.isInteger(data.speed) && data.speed >= 0 && data.speed <= 100 &&
    (data.mode === 'manual' || data.mode === 'auto') &&
    Object.prototype.hasOwnProperty.call(reverseDirectionMap, data.direction) &&
    typeof data.emergency === 'boolean' &&
    typeof data.sensorsValidated === 'boolean' &&
    Array.isArray(data.sensors) && data.sensors.length === 4 &&
    data.sensors.every((value) => value === 0 || value === 1) &&
    typeof data.distanceAvailable === 'boolean' &&
    Number.isInteger(data.distanceMm) && data.distanceMm >= 0 &&
    ['VL53L0X', 'VL53L1X', 'not_detected'].includes(data.distanceModel)
  );
}

let lastMoveSentAt = 0;
let queuedMoveTimer = null;
let pointerIsDown = false;
let lastStatePollAt = 0;
let stateRequestInFlight = false;
let commandInFlight = false;
let commandQueued = false;
let turnStopTimer = null;
let turnPulseActive = false;

function renderConnection() {
  const label = $('#connectionLabel');
  const button = $('#connectBtn');
  const latency = $('#latency');

  label.textContent = state.connected ? 'ONLINE / ESP8266' : 'OFFLINE';
  document.querySelector('.connection').classList.toggle('offline', !state.connected);
  button.innerHTML = `<span class="btn-light"></span>${state.connected ? ' TESTAR CONEXÃO' : ' TESTAR CONEXÃO'}`;
  button.setAttribute('aria-label', 'Testar conexão com o ESP8266');
  if (!state.connected) latency.textContent = '— ms';
}

function renderSensors(values = state.sensors) {
  state.sensors = values.slice(0, 4).map((value) => Number(value) ? 1 : 0);
  sensorElements.forEach((sensor, index) => {
    const active = state.sensors[index] === 1;
    sensor.classList.toggle('active', active);
    sensor.querySelector('b').textContent = String(state.sensors[index]);
  });
  const panel = $('.sensor-panel');
  panel.classList.remove('sensor-source-real', 'sensor-source-unvalidated', 'sensor-source-demo', 'sensor-source-offline');
  panel.classList.add(`sensor-source-${state.sensorSource}`);
}

function renderDistance() {
  const panel = $('.distance-panel');
  const value = $('#distanceValue');
  const status = $('#distanceStatus');
  const model = $('#distanceModel');
  const meter = $('.distance-meter');
  const fill = $('#distanceMeterFill');
  const scaleMax = state.distanceModel === 'VL53L1X' ? 4000 : 2000;

  panel.classList.toggle('distance-offline', !state.connected);
  panel.classList.toggle('distance-error', state.connected && !state.distanceAvailable);
  model.textContent = state.distanceModel === 'not_detected' ? 'VL53' : state.distanceModel;
  meter.setAttribute('aria-valuemax', String(scaleMax));
  meter.setAttribute('aria-valuenow', state.distanceAvailable ? String(Math.min(state.distanceMm, scaleMax)) : '0');

  if (!state.connected) {
    value.textContent = '—';
    status.textContent = 'Sem comunicação com o ESP8266.';
    fill.style.width = '0%';
    return;
  }
  if (!state.distanceAvailable) {
    value.textContent = '—';
    status.textContent = state.distanceModel === 'not_detected'
      ? 'Sensor não detectado. Confira alimentação e I²C.'
      : 'Sem leitura válida recente do sensor.';
    fill.style.width = '0%';
    return;
  }

  value.textContent = String(state.distanceMm);
  status.textContent = `Leitura ao vivo · ${state.distanceModel}`;
  fill.style.width = `${Math.min(100, (state.distanceMm / scaleMax) * 100)}%`;
}

function render() {
  $('#modeReadout').textContent = state.mode.toUpperCase();
  $('#modeTag').textContent = state.mode === 'auto' ? 'AUTO (ARMADO)' : 'MANUAL';
  $('#speedReadout').textContent = state.speed;
  $('#speedOutput').textContent = `${state.speed}%`;
  $('#speedSlider').value = state.speed;
  $('#directionReadout').textContent = state.direction;
  $('#motionState').textContent = state.emergency
    ? 'EMERGÊNCIA'
    : state.direction === 'CENTRO'
      ? (state.mode === 'auto' ? 'AUTO ARMADO' : 'PARADO')
      : state.direction;

  document.querySelectorAll('.mode-btn').forEach((button) => {
    button.classList.toggle('active', button.dataset.mode === state.mode);
    button.disabled = !state.connected || state.emergency ||
      (button.dataset.mode === 'auto' && !state.sensorsValidated);
  });
  const manualDisabled = !state.connected || state.mode === 'auto' || state.emergency;
  joystick.classList.toggle('disabled', manualDisabled);
  joystick.setAttribute('aria-disabled', String(manualDisabled));
  directionButtons.forEach((button) => {
    button.disabled = manualDisabled;
  });
  $('#speedSlider').disabled = !state.connected || state.emergency;
  $('#autoStatus').textContent = state.sensorsValidated
    ? 'Sensores validados. O automático inicia somente após confirmação.'
    : (state.autoBlockReason || 'Automático bloqueado: remapeie S0–S3; D5/D6 estão reservados ao sensor de distância.');
  renderSensors();
  renderDistance();
  renderConnection();
}

function setDisconnected() {
  state.connected = false;
  state.sensorSource = 'offline';
  $('#sensorStatus').textContent = 'SEM LEITURA DO ESP8266';
  state.autoBlockReason = 'Automático indisponível enquanto o ESP8266 estiver offline.';
  render();
}

async function requestState(timeoutMs = 1800) {
  if (stateRequestInFlight) return null;
  stateRequestInFlight = true;
  const startedAt = performance.now();
  const controller = new AbortController();
  const timeoutId = setTimeout(() => controller.abort(), timeoutMs);

  try {
    const response = await fetch('/api/state', { signal: controller.signal, cache: 'no-store' });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = await response.json();
    if (!hasValidStateContract(data)) throw new Error('Resposta /api/state inválida');

    state.connected = true;
    state.speed = Number(data.speed);
    state.mode = data.mode;
    state.direction = reverseDirectionMap[data.direction] || 'CENTRO';
    state.emergency = Boolean(data.emergency);
    state.sensorsValidated = Boolean(data.sensorsValidated);
    state.sensorSource = state.sensorsValidated ? 'real' : 'unvalidated';
    state.distanceMm = Number(data.distanceMm);
    state.distanceAvailable = Boolean(data.distanceAvailable);
    state.distanceModel = data.distanceModel;
    state.autoBlockReason = state.sensorsValidated
      ? ''
      : 'Automático bloqueado: remapeie S0–S3; D5/D6 estão reservados ao VL53. Confirme também a polaridade.';
    renderSensors(data.sensors);
    $('#sensorStatus').textContent = state.sensorsValidated
      ? 'LEITURA REAL — SENSORES VALIDADOS'
      : 'LEITURA DO ESP8266 — SENSORES NÃO VALIDADOS';
    $('#latency').textContent = `${Math.round(performance.now() - startedAt)} ms`;
    render();
    return data;
  } catch (error) {
    setDisconnected();
    throw error;
  } finally {
    clearTimeout(timeoutId);
    stateRequestInFlight = false;
  }
}

async function refreshState() {
  try {
    await requestState();
  } catch (_) {
    // O estado offline é refletido por setDisconnected().
  }
}

async function sendCommand() {
  if (state.emergency || !state.connected) return false;
  if (commandInFlight) {
    commandQueued = true;
    return false;
  }
  commandInFlight = true;

  const payload = {
    direction: directionMap[state.direction] || 'stop',
    speed: Number(state.speed),
    mode: state.mode,
  };

  try {
    const response = await fetch('/api/move', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    });
    const data = await response.json();
    if (!response.ok) {
      // Um bloqueio de segurança é uma resposta válida da placa, não falha de rede.
      state.connected = true;
      if (response.status === 409) {
        state.mode = 'manual';
        state.direction = 'CENTRO';
        state.sensorsValidated = false;
        state.autoBlockReason = data.error || 'Automático bloqueado por segurança.';
      } else if (response.status === 423) {
        state.mode = 'manual';
        state.direction = 'CENTRO';
        state.emergency = true;
      } else {
        state.mode = 'manual';
        state.direction = 'CENTRO';
      }
      render();
      return false;
    }
    if (!hasValidStateContract(data)) throw new Error('Resposta /api/move inválida');
    state.connected = true;
    state.direction = reverseDirectionMap[data.direction] || 'CENTRO';
    state.mode = data.mode;
    state.emergency = Boolean(data.emergency);
    state.sensorsValidated = Boolean(data.sensorsValidated);
    state.distanceMm = Number(data.distanceMm);
    state.distanceAvailable = Boolean(data.distanceAvailable);
    state.distanceModel = data.distanceModel;
    render();
    return true;
  } catch (_) {
    // Se a rede cair durante o comando, a UI e os controles entram em offline.
    setDisconnected();
    state.direction = 'CENTRO';
    render();
    return false;
  } finally {
    commandInFlight = false;
    if (commandQueued) {
      commandQueued = false;
      if (state.connected && !state.emergency) setTimeout(() => void sendCommand(), 0);
    }
  }
}

// Limita comandos de movimento a um envio por 100 ms no máximo.
function sendMoveThrottled() {
  const elapsed = performance.now() - lastMoveSentAt;
  if (elapsed >= 100) {
    lastMoveSentAt = performance.now();
    void sendCommand();
    return;
  }

  clearTimeout(queuedMoveTimer);
  queuedMoveTimer = setTimeout(() => {
    lastMoveSentAt = performance.now();
    void sendCommand();
  }, 100 - elapsed);
}

function updateDirection(nextDirection) {
  if (state.mode === 'auto' || state.emergency) return;
  clearTimeout(turnStopTimer);
  state.direction = nextDirection;
  render();
  const isTurn = nextDirection === 'ESQUERDA' || nextDirection === 'DIREITA';
  if (!isTurn) {
    turnPulseActive = false;
    sendMoveThrottled();
    return;
  }

  // Giros são comandos únicos e não usam o limitador de movimento contínuo.
  // Inicia a contagem de parada depois que o ESP responde ao comando.
  turnPulseActive = true;
  void sendCommand().then(() => {
    if (!turnPulseActive || state.direction !== nextDirection) return;
    turnStopTimer = setTimeout(() => {
      if (turnPulseActive && state.mode === 'manual' && !state.emergency &&
          state.direction === nextDirection) {
        turnPulseActive = false;
        void sendStopImmediately();
      }
    }, 60);
  });
}

// Parada prioritária: não entra na fila normal de comandos.
async function sendStopImmediately() {
  clearTimeout(queuedMoveTimer);
  clearTimeout(turnStopTimer);
  turnPulseActive = false;
  state.direction = 'CENTRO';
  render();
  try {
    const response = await fetch('/api/move', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ direction: 'stop', speed: Number(state.speed), mode: 'manual' }),
      cache: 'no-store',
    });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
  } catch (_) {
    setDisconnected();
  }
}

async function selectMode(mode) {
  if (state.emergency) return;
  // Sair do automático sempre manda stop primeiro; não fica movimento residual.
  if (state.mode === 'auto' && mode === 'manual') {
    state.mode = 'manual';
    state.direction = 'CENTRO';
    render();
    await sendCommand();
    return;
  }

  if (mode === 'auto' && state.mode !== 'auto') {
    // A confirmação vem antes dos dois pedidos; a caixa modal não congela o heartbeat.
    const confirmed = window.confirm(
      'Confirma ativar o modo automático? Faça isso somente com a área livre e a chave física acessível.'
    );
    if (!confirmed) return;

    // O primeiro POST arma parado; o segundo confirma e inicia após a contagem.
    state.mode = 'auto';
    state.direction = 'CENTRO';
    render();
    const armed = await sendCommand();
    if (!armed || !state.connected || state.emergency || !state.sensorsValidated) return;
    await sendCommand();
    return;
  }

  state.mode = mode;
  state.direction = 'CENTRO';
  render();
  await sendCommand();
}

document.querySelectorAll('.mode-btn').forEach((button) => {
  button.addEventListener('click', () => void selectMode(button.dataset.mode));
});

$('#speedSlider').addEventListener('input', (event) => {
  state.speed = Number(event.target.value);
  render();
});
$('#speedSlider').addEventListener('change', () => {
  if (state.mode === 'manual') void sendCommand();
});

function moveJoystick(clientX, clientY) {
  if (!pointerIsDown || state.mode === 'auto' || state.emergency) return;
  const rectangle = joystick.getBoundingClientRect();
  const centerX = rectangle.left + rectangle.width / 2;
  const centerY = rectangle.top + rectangle.height / 2;
  const maximumDistance = rectangle.width * 0.28;
  let x = clientX - centerX;
  let y = clientY - centerY;
  const distance = Math.hypot(x, y);

  if (distance > maximumDistance) {
    x = (x / distance) * maximumDistance;
    y = (y / distance) * maximumDistance;
  }
  knob.style.transform = `translate(${x}px, ${y}px)`;

  if (Math.abs(x) > Math.abs(y)) {
    updateDirection(x > 0 ? 'DIREITA' : 'ESQUERDA');
  } else if (y > 20) {
    updateDirection('RÉ');
  } else if (y < -20) {
    updateDirection('FRENTE');
  } else {
    updateDirection('CENTRO');
  }
}

joystick.addEventListener('pointerdown', (event) => {
  if (state.mode === 'auto' || state.emergency || !state.connected) return;
  pointerIsDown = true;
  joystick.setPointerCapture(event.pointerId);
  moveJoystick(event.clientX, event.clientY);
});
joystick.addEventListener('pointermove', (event) => moveJoystick(event.clientX, event.clientY));

function releaseJoystick() {
  if (!pointerIsDown) return;
  pointerIsDown = false;
  knob.style.transform = '';
  if (state.mode === 'manual' && !state.emergency) {
    lastMoveSentAt = performance.now();
    void sendStopImmediately();
  }
}
['pointerup', 'pointercancel', 'lostpointercapture'].forEach((eventName) => {
  joystick.addEventListener(eventName, releaseJoystick);
});

const keyDirections = {
  w: 'FRENTE', ArrowUp: 'FRENTE',
  s: 'RÉ', ArrowDown: 'RÉ',
  a: 'ESQUERDA', ArrowLeft: 'ESQUERDA',
  d: 'DIREITA', ArrowRight: 'DIREITA',
};
window.addEventListener('keydown', (event) => {
  const nextDirection = keyDirections[event.key];
  if (!nextDirection || event.repeat || state.mode === 'auto') return;
  event.preventDefault();
  updateDirection(nextDirection);
});
window.addEventListener('keyup', (event) => {
  if (keyDirections[event.key] && state.mode === 'manual' && !state.emergency) {
    state.direction = 'CENTRO';
    render();
    void sendCommand();
  }
});
window.addEventListener('blur', () => {
  pointerIsDown = false;
  if (state.mode === 'manual' && state.direction !== 'CENTRO') {
    state.direction = 'CENTRO';
    render();
    void sendCommand();
  }
});

// Cada botão funciona enquanto estiver pressionado. Ao soltar, para imediatamente.
let buttonIsDown = false;
let activeButton = null;

function releaseDirectionButton() {
  if (!buttonIsDown) return;
  buttonIsDown = false;
  activeButton?.classList.remove('pressed');
  activeButton = null;
  if (state.mode === 'manual' && !state.emergency) {
    lastMoveSentAt = performance.now();
    void sendStopImmediately();
  }
}

// Garante a parada mesmo quando o dedo sai do botão ou da tela do navegador.
window.addEventListener('pointerup', releaseDirectionButton);
window.addEventListener('pointercancel', releaseDirectionButton);
window.addEventListener('touchend', releaseDirectionButton, { passive: true });
window.addEventListener('touchcancel', releaseDirectionButton, { passive: true });

directionButtons.forEach((button) => {
  button.addEventListener('pointerdown', (event) => {
    if (button.disabled || state.mode === 'auto' || state.emergency) return;
    event.preventDefault();
    buttonIsDown = true;
    activeButton = button;
    button.classList.add('pressed');
    button.setPointerCapture(event.pointerId);
    const pressedDirection = button.dataset.direction;
    updateDirection(pressedDirection);
  });
  ['pointerup', 'pointercancel', 'lostpointercapture'].forEach((eventName) => {
    button.addEventListener(eventName, releaseDirectionButton);
  });
});

// Simulação altera somente o painel local; não envia nada ao robô.
$('#simulateSensors').addEventListener('click', () => {
  const demoValues = state.sensors.map(() => Math.random() > 0.55 ? 1 : 0);
  state.sensorSource = 'demo';
  renderSensors(demoValues);
  $('#sensorStatus').textContent = 'DEMONSTRAÇÃO LOCAL';
});

$('#connectBtn').addEventListener('click', async () => {
  const button = $('#connectBtn');
  button.disabled = true;
  button.textContent = ' TESTANDO…';
  try {
    await requestState();
  } catch (_) {
    $('#sensorStatus').textContent = 'SEM LEITURA DO ESP8266';
  } finally {
    button.disabled = false;
    renderConnection();
  }
});

$('#emergencyBtn').addEventListener('click', async () => {
  state.direction = 'CENTRO';
  state.mode = 'manual';
  state.emergency = true;
  pointerIsDown = false;
  knob.style.transform = '';
  render();

  try {
    const response = await fetch('/api/emergency', { method: 'POST' });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = await response.json();
    if (data?.ok !== true || data?.emergency !== true) {
      throw new Error('Resposta /api/emergency inválida');
    }
    state.connected = true;
    state.emergency = true;
  } catch (_) {
    // A tela indica falha de conexão, mas não pode parar motores sem rede.
    setDisconnected();
  }
  render();
});

render();
void refreshState();
setInterval(() => {
  // Poll serializado: impede acúmulo de pedidos quando a rede está lenta.
  if (stateRequestInFlight || performance.now() - lastStatePollAt < 850) return;
  lastStatePollAt = performance.now();
  void refreshState();
}, 1000);

// Heartbeats mantêm o comando manual ou automático autorizado; ao sumirem,
// o ESP8266 para os motores por timeout.
setInterval(() => {
  const movingManually = state.mode === 'manual' && state.direction !== 'CENTRO';
  const runningAutomatically = state.mode === 'auto';
  if (state.connected && !state.emergency && !turnPulseActive &&
      (movingManually || runningAutomatically)) {
    void sendCommand();
  }
}, 400);
