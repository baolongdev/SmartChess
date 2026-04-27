const DEVICE_NAME = "SmartChess-ESP32S3";
const SERVICE_UUID = "3f0e0001-70a1-4f8a-a6a3-51e9590e9f20";
const FEN_CHAR_UUID = "3f0e0002-70a1-4f8a-a6a3-51e9590e9f20";
const CMD_CHAR_UUID = "3f0e0003-70a1-4f8a-a6a3-51e9590e9f20";
const LOG_CHAR_UUID = "3f0e0004-70a1-4f8a-a6a3-51e9590e9f20";
const DB_KEY = "smartchess_games";
const SETTINGS_DB_KEY = "smartchess_settings";
const DEFAULT_START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
const LICHESS_POLL_TIMEOUT_MS = 45000;

const PIECE_UNICODE = {
  P: "♙",
  N: "♘",
  B: "♗",
  R: "♖",
  Q: "♕",
  K: "♔",
  p: "♟",
  n: "♞",
  b: "♝",
  r: "♜",
  q: "♛",
  k: "♚",
};

const BTN_LABELS = {
  connect: "scan + connect",
  disconnect: "disconnect",
  start: "start",
  stop: "stop",
  resign: "resign",
  draw: "draw",
  clear: "clear history",
  flip: "flip board",
};

let bleDevice = null;
let bleServer = null;
let fenCharacteristic = null;
let cmdCharacteristic = null;
let logCharacteristic = null;

let connected = false;
let sessionOn = false;
let flipped = false;

let fenHistory = [];
let fenCursor = -1;
let fenTimestamps = [];
let currentGame = null;

let sessionStartMs = null;
let sessionAccumulatedMs = 0;
let turnStartMs = null;
let lastMoveDeltaMs = 0;
let uiTick = null;
let audioCtx = null;
let wheelLockUntilMs = 0;
let lastRenderedFen = null;
let resignActionPending = false;
let wrongTurnAlarmTimer = null;
const textDecoder = new TextDecoder();
let webLogCount = 0;
let toastTimer = null;
let cmdAckPending = null;
let cmdInFlight = false;
let disconnectIntentional = false;
let bleDisconnectHandler = null;
let pendingLichessGameId = null;
let pendingLichessStartedMs = 0;
let lichessPollActive = false;
let hasSessionStarted = false;
let currentSessionGameId = null;

const els = {
  bleStatus: document.getElementById("bleStatus"),
  sessionStatus: document.getElementById("sessionStatus"),
  message: document.getElementById("message"),
  deviceName: document.getElementById("deviceName"),

  btnScan: document.getElementById("btnScan"),
  btnStart: document.getElementById("btnStart"),
  btnResign: document.getElementById("btnResign"),
  btnDraw: document.getElementById("btnDraw"),
  btnClear: document.getElementById("btnClear"),
  btnFlip: document.getElementById("btnFlip"),
  btnCopyFen: document.getElementById("btnCopyFen"),
  btnOpenLichess: document.getElementById("btnOpenLichess"),
  btnCopyLichess: document.getElementById("btnCopyLichess"),
  btnExportFen: document.getElementById("btnExportFen"),

  btnFirst: document.getElementById("btnFirst"),
  btnPrev: document.getElementById("btnPrev"),
  btnNext: document.getElementById("btnNext"),
  btnLast: document.getElementById("btnLast"),

  gamesCount: document.getElementById("gamesCount"),

  board: document.getElementById("board"),
  boardArea: document.querySelector(".board-area"),
  rankLabels: document.getElementById("rankLabels"),
  fileLabels: document.getElementById("fileLabels"),

  fenList: document.getElementById("fenList"),
  fenCount: document.getElementById("fenCount"),
  logList: document.getElementById("logList"),
  logCount: document.getElementById("logCount"),
  toast: document.getElementById("toast"),
  resignModal: document.getElementById("resignModal"),
  btnResignWhite: document.getElementById("btnResignWhite"),
  btnResignBlack: document.getElementById("btnResignBlack"),
  btnResignCancel: document.getElementById("btnResignCancel"),
  counter: document.getElementById("counter"),
  currentFen: document.getElementById("currentFen"),
  currentLichess: document.getElementById("currentLichess"),

  infoTurn: document.getElementById("infoTurn"),
  infoCastling: document.getElementById("infoCastling"),
  infoEP: document.getElementById("infoEP"),
  infoMove: document.getElementById("infoMove"),
  infoTurnTimer: document.getElementById("infoTurnTimer"),
  infoSessionTimer: document.getElementById("infoSessionTimer"),
  infoPositions: document.getElementById("infoPositions"),
  infoAvgMove: document.getElementById("infoAvgMove"),
  infoLastDelta: document.getElementById("infoLastDelta"),
};

function setMessage(msg, isError = false) {
  els.message.textContent = msg;
  els.message.style.color = isError ? "#ff8e8e" : "#ffb454";
}

function sleep(ms) {
  return new Promise((resolve) => window.setTimeout(resolve, ms));
}

function showToast(text, level = "err") {
  if (!els.toast) return;

  if (toastTimer !== null) {
    window.clearTimeout(toastTimer);
    toastTimer = null;
  }

  els.toast.className = `toast ${level}`;
  els.toast.textContent = text;
  els.toast.classList.add("show");

  toastTimer = window.setTimeout(() => {
    els.toast.classList.remove("show");
  }, 2600);
}

function openResignModal() {
  if (!els.resignModal) return;
  els.resignModal.classList.add("show");
  els.resignModal.setAttribute("aria-hidden", "false");
}

function closeResignModal() {
  if (!els.resignModal) return;
  els.resignModal.classList.remove("show");
  els.resignModal.setAttribute("aria-hidden", "true");
}

function prettifyCmdError(ackText) {
  if (!ackText) return "";

  const raw = ackText.replace(/^ERR:\s*/i, "").trim();
  const parts = raw.split(":");
  const head = parts[0] || "";

  if (head === "START_FAILED") {
    const reason = parts[1] || "UNKNOWN";
    const detail = parts.slice(2).join(":");

    if (reason === "MISSING_PIECES") {
      if (detail) {
        return `Missing piece UID at: ${detail}. Place piece(s) correctly then START again.`;
      }
      return "Missing pieces detected on initial setup. Check all 32 start squares then START again.";
    }

    if (reason === "DUPLICATE_UID") {
      return detail
        ? `Duplicate RFID UID detected (${detail}). Fix tags / positions then START again.`
        : "Duplicate RFID UID detected. Fix tags / positions then START again.";
    }

    return `START failed: ${reason}${detail ? ` (${detail})` : ""}`;
  }

  return "";
}

function addLogLine(level, text) {
  if (!els.logList) return;

  const row = document.createElement("div");
  row.className = `log-item ${level}`;

  const ts = new Date().toLocaleTimeString("en-GB", { hour12: false });
  row.innerHTML = `<span class="ts">${ts}</span><span class="lv">${level}</span><span class="msg"></span>`;
  row.querySelector(".msg").textContent = text;

  els.logList.prepend(row);
  webLogCount += 1;
  if (els.logCount) {
    els.logCount.textContent = String(webLogCount);
  }
}

function addLogLineDedup(level, text) {
  if (!els.logList) return;
  const first = els.logList.firstElementChild;
  if (first) {
    const firstMsg = first.querySelector(".msg")?.textContent || "";
    const firstLv = first.querySelector(".lv")?.textContent || "";
    if (firstMsg === text && firstLv === level) {
      return;
    }
  }
  addLogLine(level, text);
}

function isAckMessage(text) {
  return /^(OK:|ERR:)/i.test((text || "").trim());
}

function isGattDisconnectedError(message) {
  return /gatt server is disconnected|cannot perform gatt operations|not connected|device\.gatt\.connect/i.test(String(message || ""));
}

async function sendDeviceCommand(cmd) {
  const cmdChar = cmdCharacteristic;
  if (!cmdChar) {
    throw new Error("Command characteristic is not available");
  }

  if (!bleDevice?.gatt?.connected) {
    throw new Error("BLE disconnected. Click Scan + Connect, then retry.");
  }

  if (cmdAckPending) {
    throw new Error("GATT operation already in progress");
  }

  let settled = false;
  cmdAckPending = cmd;
  cmdInFlight = true;
  updateStatus();

  const settle = () => {
    if (settled) return;
    settled = true;
    cmdAckPending = null;
    cmdInFlight = false;
    updateStatus();
  };

  try {
    const payload = new TextEncoder().encode(cmd);
    if (typeof cmdChar.writeValueWithoutResponse === "function") {
      await cmdChar.writeValueWithoutResponse(payload);
    } else {
      await cmdChar.writeValue(payload);
    }

    let ackText = "";
    let ackErr = "";
    for (let attempt = 0; attempt < 4; attempt++) {
      try {
        const ack = await cmdChar.readValue();
        const candidate = textDecoder.decode(ack).trim();
        if (isAckMessage(candidate)) {
          ackText = candidate;
          ackErr = "";
          break;
        }
        ackText = candidate;
        ackErr = "ACK_NOT_READY";
        await sleep(50 + attempt * 35);
      } catch (err) {
        const emsg = err?.message || String(err);
        if (isGattDisconnectedError(emsg)) {
          if (connected && typeof bleDisconnectHandler === "function") {
            bleDisconnectHandler();
          }
          throw new Error("BLE disconnected during command. Reconnect and retry.");
        }
        ackErr = emsg;
        const retryable = /already in progress|unknown reason|busy|operation failed/i.test(emsg);
        if (!retryable || attempt === 3) {
          break;
        }
        await sleep(70 + attempt * 50);
      }
    }

    if (!isAckMessage(ackText)) {
      await sleep(80);
      for (let attempt = 0; attempt < 6; attempt++) {
        try {
          const ack2 = await cmdChar.readValue();
          const v2 = textDecoder.decode(ack2).trim();
          if (isAckMessage(v2)) {
            ackText = v2;
            ackErr = "";
            break;
          }
          ackText = v2;
        } catch (err) {
          const emsg = err?.message || String(err);
          if (isGattDisconnectedError(emsg)) {
            if (connected && typeof bleDisconnectHandler === "function") {
              bleDisconnectHandler();
            }
            throw new Error("BLE disconnected during command. Reconnect and retry.");
          }
          ackErr = emsg;
          const retryable = /already in progress|unknown reason|busy|operation failed/i.test(emsg);
          if (!retryable || attempt === 5) {
            break;
          }
        }
        await sleep(80 + attempt * 40);
      }
    }

    if (!ackText && ackErr) {
      ackText = `ACK_READ_ERR: ${ackErr}`;
    }

    if (!isAckMessage(ackText)) {
      ackText = `ERR: ACK_INVALID:${ackText || ackErr || "NO_ACK"}`;
    }

    addLogLine(ackText.startsWith("OK:") ? "ok" : "warn", `[CMD] ${cmd} => ${ackText || "NO_ACK"}`);
    return ackText;
  } finally {
    settle();
  }
}

function onDeviceLogNotify(event) {
  const line = textDecoder.decode(event.target.value).trim();
  if (!line) return;
  const level = /\bERR:|FAILED|UNKNOWN|NO_GAME|START_FAILED|NO_ACK/i.test(line) ? "err" : "info";
  addLogLine(level, line);

  if (/WRONG_TURN/i.test(line)) {
    startWrongTurnAlarm();
  }
  if (/MOVE_CONFIRMED|RETURNED_SOURCE|FALLBACK_RETURN_SOURCE|STOPPED|STARTED/i.test(line)) {
    stopWrongTurnAlarm();
  }

  if (pendingLichessGameId !== null) {
    const uploadedMatch = line.match(/\[LICHESS\]\s+Uploaded\s+(https?:\/\/\S+)/i);
    if (uploadedMatch) {
      const url = uploadedMatch[1];
      patchGameRecord(pendingLichessGameId, {
        lichessSync: "done",
        lichessUrl: url,
        lichessError: "",
        lichessUpdatedAt: new Date().toISOString(),
      });
      pendingLichessGameId = null;
    } else if (/\[LICHESS\]\s+Upload failed status=/i.test(line) || /\[LICHESS\]\s+Upload response missing url/i.test(line)) {
      patchGameRecord(pendingLichessGameId, {
        lichessSync: "error",
        lichessError: "UPLOAD_FAILED",
        lichessUpdatedAt: new Date().toISOString(),
      });
      pendingLichessGameId = null;
    }
  }

  if (level === "err") {
    if (/START_FAILED:MISSING_PIECES:/i.test(line)) {
      const detail = line.split("START_FAILED:MISSING_PIECES:")[1] || "";
      const msg = `Missing piece UID at: ${detail}. Place piece(s) correctly then START again.`;
      showToast(msg, "err");
    } else if (/START_FAILED:DUPLICATE_UID:/i.test(line)) {
      const detail = line.split("START_FAILED:DUPLICATE_UID:")[1] || "";
      const msg = `Duplicate RFID UID detected (${detail}). Fix tags / positions then START again.`;
      showToast(msg, "err");
    } else {
      showToast(line, "err");
    }
  } else if (/WARN|OUTSIDE|FALSE_LIFT|NO_GAME/i.test(line)) {
    showToast(line, "warn");
  }
}

function ensureAudioContext() {
  const AudioCtx = window.AudioContext || window.webkitAudioContext;
  if (!AudioCtx) return null;
  if (!audioCtx) audioCtx = new AudioCtx();
  return audioCtx;
}

function playFenChangeSound() {
  const ctx = ensureAudioContext();
  if (!ctx) return;

  const play = () => {
    const now = ctx.currentTime;
    const osc = ctx.createOscillator();
    const gain = ctx.createGain();

    osc.type = "triangle";
    osc.frequency.setValueAtTime(700, now);
    osc.frequency.exponentialRampToValueAtTime(520, now + 0.12);

    gain.gain.setValueAtTime(0.0001, now);
    gain.gain.exponentialRampToValueAtTime(0.045, now + 0.01);
    gain.gain.exponentialRampToValueAtTime(0.0001, now + 0.14);

    osc.connect(gain);
    gain.connect(ctx.destination);
    osc.start(now);
    osc.stop(now + 0.15);
  };

  if (ctx.state === "suspended") {
    ctx.resume().then(play).catch(() => {
    });
    return;
  }

  if (ctx.state === "running") {
    play();
  }
}

function playWrongTurnAlarmPulse() {
  const ctx = ensureAudioContext();
  if (!ctx) return;

  const play = () => {
    const now = ctx.currentTime;
    const osc = ctx.createOscillator();
    const gain = ctx.createGain();

    osc.type = "sawtooth";
    osc.frequency.setValueAtTime(980, now);
    osc.frequency.exponentialRampToValueAtTime(720, now + 0.24);

    gain.gain.setValueAtTime(0.0001, now);
    gain.gain.exponentialRampToValueAtTime(0.06, now + 0.012);
    gain.gain.exponentialRampToValueAtTime(0.0001, now + 0.24);

    osc.connect(gain);
    gain.connect(ctx.destination);
    osc.start(now);
    osc.stop(now + 0.26);
  };

  if (ctx.state === "suspended") {
    ctx.resume().then(play).catch(() => {
    });
    return;
  }

  if (ctx.state === "running") {
    play();
  }
}

function startWrongTurnAlarm() {
  if (wrongTurnAlarmTimer !== null) return;
  playWrongTurnAlarmPulse();
  wrongTurnAlarmTimer = window.setInterval(playWrongTurnAlarmPulse, 650);
}

function stopWrongTurnAlarm() {
  if (wrongTurnAlarmTimer === null) return;
  window.clearInterval(wrongTurnAlarmTimer);
  wrongTurnAlarmTimer = null;
}

function onBoardWheel(e) {
  if (fenHistory.length === 0) return;

  const now = Date.now();
  if (now < wheelLockUntilMs) {
    e.preventDefault();
    return;
  }

  if (Math.abs(e.deltaY) < 3) return;
  e.preventDefault();

  ensureAudioContext();

  if (e.deltaY > 0) {
    goPrev();
  } else {
    goNext();
  }

  wheelLockUntilMs = now + 120;
}

function formatDuration(ms) {
  const totalMs = Math.max(0, Math.floor(ms));
  const hours = Math.floor(totalMs / 3600000);
  const minutes = Math.floor((totalMs % 3600000) / 60000);
  const seconds = Math.floor((totalMs % 60000) / 1000);
  const millis = totalMs % 1000;

  const mm = String(minutes).padStart(2, "0");
  const ss = String(seconds).padStart(2, "0");
  const mmm = String(millis).padStart(3, "0");

  if (hours > 0) {
    return `${String(hours).padStart(2, "0")}:${mm}:${ss}.${mmm}`;
  }
  return `${mm}:${ss}.${mmm}`;
}

function formatCompactDuration(ms) {
  const totalSeconds = Math.max(0, Math.floor(ms / 1000));
  const minutes = Math.floor(totalSeconds / 60);
  const seconds = totalSeconds % 60;
  return `${minutes}m${seconds}s`;
}

function getSessionElapsedMs(now = Date.now()) {
  let elapsed = sessionAccumulatedMs;
  if (sessionOn && sessionStartMs !== null) {
    elapsed += now - sessionStartMs;
  }
  return Math.max(0, elapsed);
}

function updateStatsUI() {
  const now = Date.now();
  const sessionElapsed = getSessionElapsedMs(now);
  const turnElapsed = sessionOn && turnStartMs !== null ? now - turnStartMs : 0;

  const positions = fenHistory.length;
  const moveCount = Math.max(0, positions - 1);
  const avgMoveMs = moveCount > 0 ? sessionElapsed / moveCount : 0;

  els.infoTurnTimer.textContent = formatDuration(turnElapsed);
  els.infoSessionTimer.textContent = formatDuration(sessionElapsed);
  els.infoPositions.textContent = String(positions);
  els.infoAvgMove.textContent = formatDuration(avgMoveMs);
  els.infoLastDelta.textContent = formatDuration(lastMoveDeltaMs);
}

function startUiTick() {
  if (uiTick !== null) return;
  uiTick = window.setInterval(updateStatsUI, 100);
}

function setBtnCheckedText(btn, label, checked) {
  btn.textContent = `[${checked ? "x" : " "}] ${label}`;
}

function syncActionButtons() {
  setBtnCheckedText(els.btnScan, connected ? BTN_LABELS.disconnect : BTN_LABELS.connect, connected);
  els.btnScan.classList.toggle("btn-scan", !connected);
  els.btnScan.classList.toggle("btn-end", connected);
  setBtnCheckedText(els.btnStart, sessionOn ? BTN_LABELS.stop : BTN_LABELS.start, sessionOn);
  els.btnStart.classList.toggle("btn-start", !sessionOn);
  els.btnStart.classList.toggle("btn-end", sessionOn);
  setBtnCheckedText(els.btnResign, BTN_LABELS.resign, false);
  setBtnCheckedText(els.btnDraw, BTN_LABELS.draw, false);
  setBtnCheckedText(els.btnClear, BTN_LABELS.clear, false);
  setBtnCheckedText(els.btnFlip, BTN_LABELS.flip, flipped);
}

function updateStatus() {
  els.bleStatus.textContent = connected ? "Connected" : "Disconnected";
  els.bleStatus.className = `pill ${connected ? "connected" : "disconnected"}`;

  els.sessionStatus.textContent = sessionOn ? "Session On" : "Session Off";
  els.sessionStatus.className = `pill ${sessionOn ? "on" : "off"}`;

  els.btnScan.disabled = cmdInFlight;
  els.btnStart.disabled = !connected || cmdInFlight;
  els.btnResign.disabled = !connected || !sessionOn || cmdInFlight;
  els.btnDraw.disabled = !connected || !sessionOn || cmdInFlight;

  syncActionButtons();
  updateStatsUI();
  renderLichessLive();
}

function decodeFEN(fen) {
  const parts = fen.trim().split(/\s+/);
  if (parts.length < 6) return null;
  return {
    placement: parts[0],
    side: parts[1],
    castling: parts[2],
    ep: parts[3],
    halfmove: parts[4],
    fullmove: parts[5],
  };
}

function boardFromPlacement(placement) {
  const rows = placement.split("/");
  if (rows.length !== 8) return null;
  const out = [];
  for (const row of rows) {
    const arr = [];
    for (const ch of row) {
      if (/\d/.test(ch)) {
        const n = Number(ch);
        for (let i = 0; i < n; i++) arr.push(".");
      } else {
        arr.push(ch);
      }
    }
    if (arr.length !== 8) return null;
    out.push(arr);
  }
  return out;
}

function orientedSquare(r, c) {
  if (!flipped) return { rr: r, cc: c };
  return { rr: 7 - r, cc: 7 - c };
}

function renderLabels() {
  const ranks = flipped ? [1, 2, 3, 4, 5, 6, 7, 8] : [8, 7, 6, 5, 4, 3, 2, 1];
  const files = flipped ? ["h", "g", "f", "e", "d", "c", "b", "a"] : ["a", "b", "c", "d", "e", "f", "g", "h"];

  els.rankLabels.innerHTML = ranks.map((v) => `<span>${v}</span>`).join("");
  els.fileLabels.innerHTML = files.map((v) => `<span>${v}</span>`).join("");
}

function renderBoardFromFen(fen, { silent = false } = {}) {
  const parsed = decodeFEN(fen);
  if (!parsed) return;

  const board = boardFromPlacement(parsed.placement);
  if (!board) return;

  const fenChanged = fen !== lastRenderedFen;

  els.board.innerHTML = "";
  for (let r = 0; r < 8; r++) {
    for (let c = 0; c < 8; c++) {
      const { rr, cc } = orientedSquare(r, c);
      const piece = board[rr][cc];
      const dark = (r + c) % 2 === 1;

      const square = document.createElement("div");
      square.className = `square ${dark ? "dark" : "light"}`;

      if (piece !== ".") {
        const span = document.createElement("span");
        span.className = `piece ${piece === piece.toUpperCase() ? "white" : "black"}`;
        span.textContent = PIECE_UNICODE[piece] || piece;
        square.appendChild(span);
      }

      els.board.appendChild(square);
    }
  }

  const turnIsWhite = parsed.side === "w";
  els.infoTurn.textContent = turnIsWhite ? "White" : "Black";
  els.infoTurn.className = `v turn-${turnIsWhite ? "white" : "black"}`;
  els.infoCastling.textContent = parsed.castling;
  els.infoEP.textContent = parsed.ep === "-" ? "None" : parsed.ep;
  els.infoMove.textContent = `${parsed.fullmove} (half: ${parsed.halfmove})`;
  els.currentFen.textContent = fen;

  if (fenChanged) {
    lastRenderedFen = fen;
    if (!silent) playFenChangeSound();
  }
}

function refreshHistoryUI() {
  els.fenCount.textContent = `(${fenHistory.length})`;
  els.counter.textContent = fenHistory.length === 0 ? "0 / 0" : `${fenCursor + 1} / ${fenHistory.length}`;
  if (els.btnExportFen) {
    els.btnExportFen.disabled = fenHistory.length === 0;
  }

  els.fenList.innerHTML = "";
  fenHistory.forEach((fen, idx) => {
    const item = document.createElement("div");
    item.className = `fen-item ${idx === fenCursor ? "active" : ""}`;
    item.onclick = () => {
      fenCursor = idx;
      renderBoardFromFen(fenHistory[fenCursor]);
      refreshHistoryUI();
    };

    item.innerHTML = `
      <div class="fen-header">
        <span>#${idx + 1}</span>
        <span>${decodeFEN(fen)?.side === "w" ? "White" : "Black"}</span>
      </div>
      <div class="fen-text">${fen}</div>
    `;
    els.fenList.appendChild(item);
  });

  updateStatsUI();
}

function pushFenIfChanged(fen) {
  if (!fen || decodeFEN(fen) === null) return;
  if (fenHistory.length > 0 && fenHistory[fenHistory.length - 1] === fen) {
    return;
  }

  const now = Date.now();
  if (fenTimestamps.length > 0) {
    lastMoveDeltaMs = now - fenTimestamps[fenTimestamps.length - 1];
  } else {
    lastMoveDeltaMs = 0;
  }

  fenHistory.push(fen);
  fenTimestamps.push(now);
  fenCursor = fenHistory.length - 1;
  turnStartMs = now;
  renderBoardFromFen(fen);
  refreshHistoryUI();
}

function onFenNotify(event) {
  const value = new TextDecoder().decode(event.target.value).trim();
  pushFenIfChanged(value);
}

function readLocalSettings() {
  try {
    const raw = localStorage.getItem(SETTINGS_DB_KEY);
    if (!raw) return null;
    const parsed = JSON.parse(raw);
    if (!parsed || typeof parsed !== "object") return null;
    return parsed;
  } catch (e) {
    return null;
  }
}

function writeLocalSettings(payload) {
  try {
    localStorage.setItem(SETTINGS_DB_KEY, JSON.stringify(payload));
  } catch (e) {
  }
}

function parsePipePayload(text) {
  if (!text) return null;
  const normalized = text.replace(/^OK:\s*/i, "").trim();
  if (!normalized) return null;

  const parts = normalized.split("|");
  const head = (parts.shift() || "").trim();
  const info = {};
  for (const part of parts) {
    const eqIndex = part.indexOf("=");
    if (eqIndex <= 0) continue;
    const key = part.slice(0, eqIndex).trim().toLowerCase();
    const value = part.slice(eqIndex + 1).trim();
    if (key) {
      info[key] = value;
    }
  }

  return { head, info, normalized };
}

function parseLichessStatusPayload(text) {
  const parsed = parsePipePayload(text);
  if (!parsed) return null;

  const headUpper = (parsed.head || "").toUpperCase();
  if (!headUpper.startsWith("LICHESS")) {
    return null;
  }

  let state = String(parsed.info.state || "").toUpperCase();
  if (!state) {
    if (headUpper.includes("QUEUED")) {
      state = "QUEUED";
    } else {
      state = "IDLE";
    }
  }

  return {
    state,
    url: parsed.info.url || "",
    err: parsed.info.err || "",
    ply: parseInt(parsed.info.ply, 10) || 0,
  };
}

function parseCfgPayload(text) {
  if (!text) return null;
  const normalized = text.replace(/^OK:\s*/i, "").trim();
  if (!(normalized.startsWith("CFG") || normalized.startsWith("CFG_SAVED"))) {
    return null;
  }

  const parts = normalized.split("|");
  const info = {};
  for (const part of parts) {
    const eqIndex = part.indexOf("=");
    if (eqIndex > 0) {
      const key = part.slice(0, eqIndex).trim();
      const value = part.slice(eqIndex + 1).trim();
      if (key) {
        info[key] = value;
      }
    }
  }

  const normalizeBool = (v, fallback = false) => {
    if (v === undefined || v === null || v === "") return fallback;
    const token = String(v).trim().toLowerCase();
    return token === "1" || token === "true" || token === "on" || token === "yes";
  };

  return {
    version: parseInt(info.ver, 10) || 1,
    verbose: normalizeBool(info.verbose, false),
    continuous: normalizeBool(info.continuous, true),
    letters: normalizeBool(info.letters, true),
    updatedAt: new Date().toISOString(),
    source: "esp32",
  };
}

function buildCfgSetCommand(settings) {
  const ver = Math.max(1, Number(settings.version) || 1);
  const verbose = settings.verbose ? 1 : 0;
  const continuous = settings.continuous ? 1 : 0;
  const letters = settings.letters ? 1 : 0;
  return `CFG_SET|VER=${ver}|VERBOSE=${verbose}|CONTINUOUS=${continuous}|LETTERS=${letters}`;
}

function shouldPushLocalSettings(localCfg, espCfg) {
  if (!localCfg) return false;
  if (!espCfg) return true;
  const localVer = Number(localCfg.version) || 0;
  const espVer = Number(espCfg.version) || 0;
  if (localVer > espVer) return true;
  if (localVer < espVer) return false;

  return (
    Boolean(localCfg.verbose) !== Boolean(espCfg.verbose) ||
    Boolean(localCfg.continuous) !== Boolean(espCfg.continuous) ||
    Boolean(localCfg.letters) !== Boolean(espCfg.letters)
  );
}

async function syncSettingsWithDevice() {
  if (!cmdCharacteristic || !connected) return;

  const localBefore = readLocalSettings();

  let deviceCfg = null;
  try {
    const cfgAck = await sendDeviceCommand("CFG_GET");
    deviceCfg = parseCfgPayload(cfgAck);
  } catch (e) {
    addLogLine("warn", `[CFG] Read failed: ${e?.message || String(e)}`);
    return;
  }

  if (!deviceCfg) {
    return;
  }

  if (!localBefore) {
    writeLocalSettings(deviceCfg);
    addLogLine("info", `[CFG] Pulled ver=${deviceCfg.version}`);
    return;
  }

  if (!shouldPushLocalSettings(localBefore, deviceCfg)) {
    writeLocalSettings(deviceCfg);
    return;
  }

  const nextVer = Math.max((Number(localBefore.version) || 0), (Number(deviceCfg?.version) || 0)) + 1;
  const payload = {
    version: nextVer,
    verbose: Boolean(localBefore.verbose),
    continuous: Boolean(localBefore.continuous),
    letters: Boolean(localBefore.letters),
  };

  try {
    const saveAck = await sendDeviceCommand(buildCfgSetCommand(payload));
    const saved = parseCfgPayload(saveAck);
    if (saved) {
      writeLocalSettings(saved);
      addLogLine("ok", `[CFG] Synced ver=${saved.version}`);
    }
  } catch (e) {
    addLogLine("warn", `[CFG] Push failed: ${e?.message || String(e)}`);
  }
}

async function sendBleText(text) {
  if (!fenCharacteristic) throw new Error("BLE char not ready");
  const enc = new TextEncoder().encode(text);

  if (typeof fenCharacteristic.writeValueWithoutResponse === "function") {
    await fenCharacteristic.writeValueWithoutResponse(enc);
  } else {
    await fenCharacteristic.writeValue(enc);
  }
}

async function connectBle() {
  if (!navigator.bluetooth) {
    throw new Error("Web Bluetooth not supported in this browser.");
  }

  if (bleDevice && bleDisconnectHandler) {
    try {
      bleDevice.removeEventListener("gattserverdisconnected", bleDisconnectHandler);
    } catch (e) {
    }
  }

  bleDevice = await navigator.bluetooth.requestDevice({
    filters: [{ name: DEVICE_NAME }],
    optionalServices: [SERVICE_UUID],
  });

  bleDisconnectHandler = () => {
    stopWrongTurnAlarm();
    abortPendingLichess("BLE_DISCONNECTED");
    hasSessionStarted = false;
    currentSessionGameId = null;
    connected = false;
    sessionOn = false;
    sessionStartMs = null;
    sessionAccumulatedMs = 0;
    turnStartMs = null;
    lastMoveDeltaMs = 0;
    cmdCharacteristic = null;
    logCharacteristic = null;
    updateStatus();
    setMessage("BLE disconnected", !disconnectIntentional);
    if (disconnectIntentional) {
      addLogLineDedup("info", "[BLE] Disconnected");
    } else {
      addLogLineDedup("err", "[BLE] Disconnected by device");
      showToast("[BLE] Disconnected by device", "err");
    }
    resetBoardUiToDefault();
    disconnectIntentional = false;
    cmdAckPending = null;
  };
  bleDevice.addEventListener("gattserverdisconnected", bleDisconnectHandler);

  const connectAndGetService = async () => {
    bleServer = await bleDevice.gatt.connect();
    return bleServer.getPrimaryService(SERVICE_UUID);
  };

  let service;
  try {
    service = await connectAndGetService();
  } catch (err) {
    const msg = String(err?.message || err || "");
    const retryable = /gatt server is disconnected|not connected|failed to execute 'getprimaryservice'/i.test(msg);
    if (!retryable) {
      throw err;
    }
    await sleep(120);
    service = await connectAndGetService();
  }

  fenCharacteristic = await service.getCharacteristic(FEN_CHAR_UUID);

  try {
    cmdCharacteristic = await service.getCharacteristic(CMD_CHAR_UUID);
    addLogLineDedup("ok", "[PROTO] CMD characteristic ready");
  } catch (e) {
    cmdCharacteristic = null;
    addLogLineDedup("warn", `[PROTO] CMD characteristic missing: ${e?.message || String(e)}`);
  }

  try {
    logCharacteristic = await service.getCharacteristic(LOG_CHAR_UUID);
    await logCharacteristic.startNotifications();
    logCharacteristic.addEventListener("characteristicvaluechanged", onDeviceLogNotify);
    addLogLineDedup("ok", "[PROTO] LOG notify enabled");
  } catch (e) {
    logCharacteristic = null;
    addLogLineDedup("warn", `[PROTO] LOG characteristic missing: ${e?.message || String(e)}`);
  }

  await fenCharacteristic.startNotifications();
  fenCharacteristic.addEventListener("characteristicvaluechanged", onFenNotify);

  const initial = new TextDecoder().decode(await fenCharacteristic.readValue()).trim();

  connected = true;
  updateStatus();
  els.deviceName.textContent = `Device: ${bleDevice.name || bleDevice.id}`;
  setMessage("Connected. You can press START.");
  addLogLineDedup("info", "[BLE] Connected");
  await syncSettingsWithDevice();
  if (initial && initial !== "-") {
    pushFenIfChanged(initial);
  }
}

async function disconnectBle() {
  stopWrongTurnAlarm();
  abortPendingLichess("BLE_DISCONNECTED");
  hasSessionStarted = false;
  currentSessionGameId = null;

  if (fenCharacteristic) {
    try {
      fenCharacteristic.removeEventListener("characteristicvaluechanged", onFenNotify);
    } catch (e) {
    }
  }

  if (logCharacteristic) {
    try {
      logCharacteristic.removeEventListener("characteristicvaluechanged", onDeviceLogNotify);
      await logCharacteristic.stopNotifications();
    } catch (e) {
    }
  }

  disconnectIntentional = true;

  if (bleDevice?.gatt?.connected) {
    bleDevice.gatt.disconnect();
  }

  if (bleDevice && bleDisconnectHandler) {
    try {
      bleDevice.removeEventListener("gattserverdisconnected", bleDisconnectHandler);
    } catch (e) {
    }
  }

  connected = false;
  sessionOn = false;
  sessionStartMs = null;
  sessionAccumulatedMs = 0;
  turnStartMs = null;
  lastMoveDeltaMs = 0;
  fenTimestamps = [];
  cmdCharacteristic = null;
  logCharacteristic = null;
  cmdAckPending = null;
  updateStatus();
  resetBoardUiToDefault();
  setMessage("Disconnected", false);
  addLogLineDedup("info", "[BLE] Disconnected");
}

async function startSession() {
  stopWrongTurnAlarm();
  hasSessionStarted = false;
  currentSessionGameId = null;
  renderLichessLive();

  let ack = "";
  if (cmdCharacteristic) {
    ack = await sendDeviceCommand("START");
    if (!ack.startsWith("OK:")) {
      const pretty = prettifyCmdError(ack);
      if (pretty) {
        addLogLine("err", `[START] ${pretty}`);
        showToast(pretty, "err");
      }
      sessionOn = false;
      sessionStartMs = null;
      sessionAccumulatedMs = 0;
      turnStartMs = null;
      updateStatus();
      resetBoardUiToDefault();
      throw new Error(ack);
    }
  } else {
    await sendBleText("START");
    addLogLine("warn", "[PROTO] Fallback to FEN char write for START");
  }

  sessionAccumulatedMs = 0;
  sessionStartMs = Date.now();
  turnStartMs = sessionStartMs;
  lastMoveDeltaMs = 0;
  startGameRecording();
  hasSessionStarted = true;
  currentSessionGameId = null;

  sessionOn = true;
  updateStatus();
  setMessage(ack ? `START sent (${ack})` : "START sent to board");
}

async function endSession(endOverride = null) {
  stopWrongTurnAlarm();

  let ack = "";
  if (cmdCharacteristic) {
    ack = await sendDeviceCommand("STOP");
    if (!ack.startsWith("OK:")) {
      throw new Error(ack);
    }
  } else {
    await sendBleText("STOP");
    addLogLine("warn", "[PROTO] Fallback to FEN char write for STOP");
  }

  if (sessionStartMs !== null) {
    sessionAccumulatedMs += Date.now() - sessionStartMs;
    sessionStartMs = null;
  }

  const finalFen = (els.currentFen.textContent || "").trim();
  const savedGameId = finishGameRecording(ack, finalFen, endOverride);
  currentSessionGameId = savedGameId;

  sessionOn = false;
  updateStatus();
  resetBoardUiToDefault();

  if (savedGameId !== null && savedGameId !== undefined) {
    void queueLichessPublishForGame(savedGameId);
  }

  if (endOverride?.endType === "resign") {
    const sideLabel = endOverride.resignSide === "black" ? "Black" : "White";
    setMessage(`${sideLabel} resigned. Game saved.`);
    return;
  }

  if (endOverride?.endType === "draw") {
    setMessage("Draw agreed. Game saved.");
    return;
  }

  setMessage(ack ? `STOP sent (${ack})` : "STOP sent to board");
}

function clearHistory() {
  fenHistory = [];
  fenTimestamps = [];
  fenCursor = -1;
  lastRenderedFen = null;
  els.currentFen.textContent = "-";
  els.infoTurn.textContent = "-";
  els.infoTurn.className = "v";
  els.infoCastling.textContent = "-";
  els.infoEP.textContent = "-";
  els.infoMove.textContent = "-";
  turnStartMs = sessionOn ? Date.now() : null;
  lastMoveDeltaMs = 0;
  if (!sessionOn) {
    hasSessionStarted = false;
    currentSessionGameId = null;
  }
  renderLichessLive();
  refreshHistoryUI();
  syncActionButtons();
}

function resetBoardUiToDefault() {
  fenHistory = [];
  fenTimestamps = [];
  fenCursor = -1;
  lastRenderedFen = null;
  lastMoveDeltaMs = 0;
  turnStartMs = null;

  renderBoardFromFen(DEFAULT_START_FEN, { silent: true });
  refreshHistoryUI();
}

function flipBoard() {
  flipped = !flipped;
  renderLabels();

  if (fenCursor >= 0) {
    renderBoardFromFen(fenHistory[fenCursor]);
    return;
  }

  const liveFen = (els.currentFen.textContent || "").trim();
  if (decodeFEN(liveFen)) {
    renderBoardFromFen(liveFen);
  } else {
    renderBoardFromFen(DEFAULT_START_FEN);
  }

  syncActionButtons();
}

function goFirst() {
  if (fenHistory.length === 0) return;
  fenCursor = 0;
  renderBoardFromFen(fenHistory[fenCursor]);
  refreshHistoryUI();
}

function goPrev() {
  if (fenHistory.length === 0) return;
  fenCursor = Math.max(0, fenCursor - 1);
  renderBoardFromFen(fenHistory[fenCursor]);
  refreshHistoryUI();
}

function goNext() {
  if (fenHistory.length === 0) return;
  fenCursor = Math.min(fenHistory.length - 1, fenCursor + 1);
  renderBoardFromFen(fenHistory[fenCursor]);
  refreshHistoryUI();
}

function goLast() {
  if (fenHistory.length === 0) return;
  fenCursor = fenHistory.length - 1;
  renderBoardFromFen(fenHistory[fenCursor]);
  refreshHistoryUI();
}

async function copyFen() {
  const fen = els.currentFen.textContent || "";
  if (!fen || fen === "-") return;
  await navigator.clipboard.writeText(fen);
  setMessage("FEN copied");
}

function openLichessLink() {
  const url = getCurrentSessionLichessUrl();
  if (!url) {
    setMessage("No Lichess link yet", true);
    return;
  }
  window.open(url, "_blank", "noopener,noreferrer");
}

async function copyLichessLink() {
  const url = getCurrentSessionLichessUrl();
  if (!url) {
    setMessage("No Lichess link yet", true);
    return;
  }

  await navigator.clipboard.writeText(url);
  setMessage("Lichess link copied");
}

function exportFenHistory() {
  if (fenHistory.length === 0) {
    setMessage("No FEN history to export", true);
    return;
  }

  const lines = [
    "# SmartChess FEN Export",
    `# exported_at=${new Date().toISOString()}`,
    `# total=${fenHistory.length}`,
    "",
  ];

  fenHistory.forEach((fen, idx) => {
    const ts = fenTimestamps[idx] ? new Date(fenTimestamps[idx]).toISOString() : "-";
    lines.push(`${idx + 1}\t${ts}\t${fen}`);
  });

  const blob = new Blob([lines.join("\n")], { type: "text/plain;charset=utf-8" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  const tsFile = new Date().toISOString().replace(/[:.]/g, "-");
  a.href = url;
  a.download = `smartchess-fen-history-${tsFile}.txt`;
  document.body.appendChild(a);
  a.click();
  a.remove();
  URL.revokeObjectURL(url);

  setMessage(`Exported ${fenHistory.length} FEN entries`);
  addLogLine("ok", `[EXPORT] FEN history exported (${fenHistory.length})`);
}

function startGameRecording() {
  currentGame = {
    startTimeMs: Date.now(),
    moves: 0,
    endFen: "",
  };
}

function parseGameResult(resultText) {
  if (!resultText) return null;

  const normalized = resultText.replace(/^OK:\s*/i, "").trim();
  if (!normalized.startsWith("STOPPED")) return null;

  const parts = normalized.split("|");
  const info = {};
  for (const part of parts) {
    const eqIndex = part.indexOf("=");
    if (eqIndex > 0) {
      const key = part.slice(0, eqIndex).trim();
      const value = part.slice(eqIndex + 1).trim();
      if (key) {
        info[key] = value;
      }
    }
  }

  return {
    moves: parseInt(info.moves, 10) || 0,
    timeStr: info.time || "0m0s",
    fen: info.fen || "",
  };
}

function readGamesDb() {
  try {
    const stored = localStorage.getItem(DB_KEY);
    if (!stored) return [];
    const parsed = JSON.parse(stored);
    if (Array.isArray(parsed)) return parsed;
    if (parsed && Array.isArray(parsed.games)) return parsed.games;
    return [];
  } catch (e) {
    return [];
  }
}

function getGameRecordById(gameId) {
  if (gameId === null || gameId === undefined) {
    return null;
  }
  const games = readGamesDb();
  for (let i = games.length - 1; i >= 0; i--) {
    if (String(games[i]?.id) === String(gameId)) {
      return games[i] || null;
    }
  }
  return null;
}

function getCurrentSessionLichessUrl() {
  const game = getGameRecordById(currentSessionGameId);
  return String(game?.lichessUrl || "").trim();
}

function renderLichessLive() {
  if (!els.currentLichess) {
    return;
  }

  if (!hasSessionStarted) {
    els.currentLichess.textContent = "-";
    if (els.btnOpenLichess) els.btnOpenLichess.disabled = true;
    if (els.btnCopyLichess) els.btnCopyLichess.disabled = true;
    return;
  }

  const game = getGameRecordById(currentSessionGameId);
  const url = getCurrentSessionLichessUrl();
  const sync = String(game?.lichessSync || "").toLowerCase();

  let display = "[in progress]";
  if (url) {
    display = url;
  } else if (!game) {
    display = sessionOn ? "[in progress]" : "[waiting result]";
  } else if (sync === "queued" || sync === "pending") {
    display = "[pending upload]";
  } else if (sync === "error") {
    display = `[error: ${game?.lichessError || "UPLOAD_FAILED"}]`;
  } else if (sync === "skipped") {
    display = "[upload skipped]";
  } else {
    display = "[waiting result]";
  }

  els.currentLichess.textContent = display;
  if (els.btnOpenLichess) {
    els.btnOpenLichess.disabled = !url;
  }
  if (els.btnCopyLichess) {
    els.btnCopyLichess.disabled = !url;
  }
}

function updateGamesCount() {
  if (!els.gamesCount) return;
  const games = readGamesDb();
  els.gamesCount.textContent = String(games.length);
}

function patchGameRecord(gameId, patch) {
  if (gameId === null || gameId === undefined) {
    return false;
  }

  try {
    const games = readGamesDb();
    let updated = false;
    for (let i = 0; i < games.length; i++) {
      if (String(games[i]?.id) !== String(gameId)) {
        continue;
      }
      games[i] = {
        ...games[i],
        ...patch,
      };
      updated = true;
      break;
    }

    if (!updated) {
      return false;
    }

    localStorage.setItem(DB_KEY, JSON.stringify(games));
    updateGamesCount();
    renderLichessLive();
    return true;
  } catch (e) {
    return false;
  }
}

function finishGameRecording(resultText, fallbackFen, endOverride = null) {
  const parsed = parseGameResult(resultText);

  let endType = "stop";
  let result = "draw";
  let resignSide = "";

  if (endOverride?.endType === "resign") {
    endType = "resign";
    resignSide = endOverride.resignSide === "black" ? "black" : "white";
    result = resignSide === "white" ? "black" : "white";
  } else if (endOverride?.endType === "draw") {
    endType = "draw";
    result = "draw";
  } else if (parsed?.fen) {
    const decoded = decodeFEN(parsed.fen);
    if (decoded) {
      if (decoded.side === "w") result = "black";
      else if (decoded.side === "b") result = "white";
    }
  }

  const finalFen = parsed?.fen || fallbackFen || fenHistory[fenHistory.length - 1] || "";
  const elapsedMs = getSessionElapsedMs();
  const moves = parsed?.moves ?? Math.max(0, fenHistory.length - 1);
  const timeStr = parsed?.timeStr || formatCompactDuration(elapsedMs);
  const gameId = Date.now();
  let saved = false;

  const fenTrail = fenHistory.map((fen, idx) => {
    const parsedFen = decodeFEN(fen);
    return {
      index: idx + 1,
      moveNumber: Math.floor(idx / 2) + 1,
      sideToMove: parsedFen ? parsedFen.side : "-",
      fen: fen,
      timestamp: fenTimestamps[idx] ? new Date(fenTimestamps[idx]).toISOString() : "",
    };
  });

  try {
    const existing = readGamesDb();
    existing.push({
      id: gameId,
      gameNumber: existing.length + 1,
      playedAt: new Date().toISOString(),
      result: result,
      endType: endType,
      resignSide: resignSide,
      moves: moves,
      timeStr: timeStr,
      endFen: finalFen,
      fenTrail: fenTrail,
      lichessUrl: "",
      lichessSync: "pending",
      lichessError: "",
      lichessUpdatedAt: "",
    });
    localStorage.setItem(DB_KEY, JSON.stringify(existing));
    updateGamesCount();
    saved = true;
  } catch (err) {
    console.error("DB save failed:", err);
  }

  currentGame = null;
  return saved ? gameId : null;
}

function normalizeAckError(ackText) {
  if (!ackText) return "UNKNOWN";
  return ackText.replace(/^ERR:\s*/i, "").trim() || "UNKNOWN";
}

function abortPendingLichess(reason) {
  if (pendingLichessGameId === null) {
    return;
  }
  patchGameRecord(pendingLichessGameId, {
    lichessSync: "error",
    lichessError: reason || "ABORTED",
    lichessUpdatedAt: new Date().toISOString(),
  });
  pendingLichessGameId = null;
}

async function pumpLichessStatus() {
  if (lichessPollActive) {
    return;
  }

  lichessPollActive = true;
  try {
    while (pendingLichessGameId !== null) {
      if (Date.now() - pendingLichessStartedMs > LICHESS_POLL_TIMEOUT_MS) {
        patchGameRecord(pendingLichessGameId, {
          lichessSync: "error",
          lichessError: "TIMEOUT",
          lichessUpdatedAt: new Date().toISOString(),
        });
        addLogLine("warn", "[LICHESS] Upload timeout");
        pendingLichessGameId = null;
        break;
      }

      if (!connected || !cmdCharacteristic) {
        patchGameRecord(pendingLichessGameId, {
          lichessSync: "error",
          lichessError: "BLE_DISCONNECTED",
          lichessUpdatedAt: new Date().toISOString(),
        });
        pendingLichessGameId = null;
        break;
      }

      if (sessionOn || cmdInFlight) {
        await sleep(250);
        continue;
      }

      let ack = "";
      try {
        ack = await sendDeviceCommand("LICHESS_STATUS");
      } catch (err) {
        await sleep(350);
        continue;
      }

      const status = parseLichessStatusPayload(ack);
      if (!status) {
        await sleep(700);
        continue;
      }

      if (status.state === "DONE") {
        patchGameRecord(pendingLichessGameId, {
          lichessSync: "done",
          lichessUrl: status.url || "",
          lichessError: "",
          lichessUpdatedAt: new Date().toISOString(),
        });
        if (status.url) {
          addLogLine("ok", `[LICHESS] Uploaded ${status.url}`);
          showToast(`Lichess uploaded: ${status.url}`, "info");
        } else {
          addLogLine("warn", "[LICHESS] DONE but URL missing");
        }
        pendingLichessGameId = null;
        break;
      }

      if (status.state === "ERROR") {
        patchGameRecord(pendingLichessGameId, {
          lichessSync: "error",
          lichessError: status.err || "UPLOAD_FAILED",
          lichessUpdatedAt: new Date().toISOString(),
        });
        addLogLine("warn", `[LICHESS] Upload failed (${status.err || "UPLOAD_FAILED"})`);
        pendingLichessGameId = null;
        break;
      }

      await sleep(1000);
    }
  } finally {
    lichessPollActive = false;
  }
}

async function queueLichessPublishForGame(gameId) {
  if (gameId === null || gameId === undefined) {
    return;
  }

  if (!connected || !cmdCharacteristic) {
    const reason = connected ? "CMD_NOT_AVAILABLE" : "BLE_DISCONNECTED";
    patchGameRecord(gameId, {
      lichessSync: "skipped",
      lichessError: reason,
      lichessUpdatedAt: new Date().toISOString(),
    });
    return;
  }

  let queueAck = "";
  try {
    queueAck = await sendDeviceCommand("LICHESS_PUBLISH");
  } catch (err) {
    patchGameRecord(gameId, {
      lichessSync: "error",
      lichessError: err?.message || String(err),
      lichessUpdatedAt: new Date().toISOString(),
    });
    addLogLine("warn", `[LICHESS] Queue failed (${err?.message || String(err)})`);
    return;
  }

  if (!queueAck.startsWith("OK:")) {
    const errText = normalizeAckError(queueAck);
    patchGameRecord(gameId, {
      lichessSync: "error",
      lichessError: errText,
      lichessUpdatedAt: new Date().toISOString(),
    });
    addLogLine("warn", `[LICHESS] Queue rejected (${errText})`);
    return;
  }

  patchGameRecord(gameId, {
    lichessSync: "queued",
    lichessError: "",
    lichessUpdatedAt: new Date().toISOString(),
  });

  if (pendingLichessGameId !== null && String(pendingLichessGameId) !== String(gameId)) {
    patchGameRecord(pendingLichessGameId, {
      lichessSync: "error",
      lichessError: "REPLACED_BY_NEW_GAME",
      lichessUpdatedAt: new Date().toISOString(),
    });
  }

  pendingLichessGameId = gameId;
  pendingLichessStartedMs = Date.now();
  addLogLine("info", `[LICHESS] ${queueAck.replace(/^OK:\s*/i, "").trim()}`);
  void pumpLichessStatus();
}

async function resignSession(resignSide) {
  if (resignActionPending) return;
  if (resignSide !== "white" && resignSide !== "black") return;

  resignActionPending = true;
  const winner = resignSide === "white" ? "black" : "white";
  try {
    addLogLine("warn", `[GAME] ${resignSide.toUpperCase()} resigned`);
    await endSession({
      endType: "resign",
      resignSide,
    });
    addLogLine("ok", `[GAME] ${winner.toUpperCase()} wins by resignation`);
  } finally {
    resignActionPending = false;
  }
}

async function drawSession() {
  const agreed = window.confirm("Confirm draw? The game will be stopped and saved as draw.");
  if (!agreed) return;

  addLogLine("info", "[GAME] Draw agreed");
  await endSession({ endType: "draw" });
}

els.btnScan.addEventListener("click", async () => {
  try {
    if (connected) {
      await disconnectBle();
    } else {
      await connectBle();
    }
  } catch (e) {
    setMessage(e.message || String(e), true);
  }
});

els.btnStart.addEventListener("click", async () => {
  try {
    if (sessionOn) {
      await endSession();
    } else {
      await startSession();
    }
  } catch (e) {
    setMessage(e.message || String(e), true);
  }
});

els.btnResign.addEventListener("click", async () => {
  try {
    openResignModal();
  } catch (e) {
    setMessage(e.message || String(e), true);
  }
});

els.btnDraw.addEventListener("click", async () => {
  try {
    await drawSession();
  } catch (e) {
    setMessage(e.message || String(e), true);
  }
});

if (els.btnResignWhite) {
  els.btnResignWhite.addEventListener("click", async () => {
    closeResignModal();
    try {
      await resignSession("white");
    } catch (e) {
      setMessage(e.message || String(e), true);
    }
  });
}

if (els.btnResignBlack) {
  els.btnResignBlack.addEventListener("click", async () => {
    closeResignModal();
    try {
      await resignSession("black");
    } catch (e) {
      setMessage(e.message || String(e), true);
    }
  });
}

if (els.btnResignCancel) {
  els.btnResignCancel.addEventListener("click", closeResignModal);
}

if (els.resignModal) {
  els.resignModal.addEventListener("click", (e) => {
    if (e.target === els.resignModal) {
      closeResignModal();
    }
  });
}

els.btnClear.addEventListener("click", clearHistory);
els.btnFlip.addEventListener("click", flipBoard);
els.btnCopyFen.addEventListener("click", copyFen);
if (els.btnOpenLichess) {
  els.btnOpenLichess.addEventListener("click", openLichessLink);
}
if (els.btnCopyLichess) {
  els.btnCopyLichess.addEventListener("click", copyLichessLink);
}
if (els.btnExportFen) {
  els.btnExportFen.addEventListener("click", exportFenHistory);
}

els.btnFirst.addEventListener("click", goFirst);
els.btnPrev.addEventListener("click", goPrev);
els.btnNext.addEventListener("click", goNext);
els.btnLast.addEventListener("click", goLast);

window.addEventListener("keydown", (e) => {
  if (e.key === "Escape") {
    closeResignModal();
    return;
  }

  ensureAudioContext();

  if (e.key === "ArrowLeft") goPrev();
  if (e.key === "ArrowRight") goNext();
  if (e.key.toLowerCase() === "f") flipBoard();
});

if (els.boardArea) {
  els.boardArea.addEventListener("wheel", onBoardWheel, { passive: false });
}

window.addEventListener("pointerdown", ensureAudioContext, { once: true });

renderLabels();
renderBoardFromFen(DEFAULT_START_FEN, { silent: true });
updateGamesCount();
startUiTick();
updateStatus();
setMessage("Click Scan + Connect to start.");
