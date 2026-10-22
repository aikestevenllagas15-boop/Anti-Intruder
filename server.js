// Door Alarm - WebSocket server + dashboard
// Run: npm install && npm start   (Node.js 18 or newer)

const express = require("express");
const http = require("http");
const path = require("path");
const fs = require("fs");
const crypto = require("crypto");
const { WebSocketServer } = require("ws");

const PORT = process.env.PORT || 3000;
const DEVICE_KEY = process.env.DEVICE_KEY || "guard123"; // must match the ESP32 code
const DASH_KEY = process.env.DASH_KEY || "";           // optional: protects the dashboard (open /?key=YOURKEY)
const DASH_PIN = process.env.DASH_PIN || "";           // optional: requires a PIN to view/control the dashboard
const NTFY_TOPIC = process.env.NTFY_TOPIC || "";          // optional phone alerts via ntfy.sh
const DATA_FILE = path.join(__dirname, "events.json");
const PIN_ATTEMPT_WINDOW_MS = 15 * 60 * 1000;
const MAX_PIN_ATTEMPTS = 5;
const pinAttempts = new Map();
const dashboardPinHash = crypto.createHash("sha256").update(DASH_PIN).digest();

if (DASH_PIN && DASH_PIN.length < 6) {
  throw new Error("DASH_PIN must be at least 6 characters long");
}
if (!DASH_PIN) {
  console.warn("WARNING: DASH_PIN is not set; dashboard PIN protection is disabled.");
}

const app = express();
app.use(express.static(path.join(__dirname, "public")));
const server = http.createServer(app);

const deviceWss = new WebSocketServer({ noServer: true });
const dashWss = new WebSocketServer({ noServer: true });

let history = [];
try {
  history = JSON.parse(fs.readFileSync(DATA_FILE, "utf8"));
} catch (e) {}

const locations = {}; // name -> { name, state, online, lastSeen, ws }

function snapshot() {
  return JSON.stringify({
    type: "snapshot",
    locations: Object.values(locations).map((l) => ({
      name: l.name,
      state: l.state,
      online: l.online,
      lastSeen: l.lastSeen,
    })),
    history: history.slice(0, 50),
  });
}

function broadcast() {
  const s = snapshot();
  dashWss.clients.forEach((c) => {
    if (c.readyState === 1 && c.authenticated) c.send(s);
  });
}

function addEvent(kind, location, extra = {}) {
  const ev = {
    id: Date.now() + "-" + Math.random().toString(36).slice(2, 6),
    time: new Date().toISOString(),
    kind,
    location,
    ...extra,
  };
  history.unshift(ev);
  history = history.slice(0, 200);
  fs.writeFile(DATA_FILE, JSON.stringify(history), () => {});
  return ev;
}

async function notifyPhone(location) {
  if (!NTFY_TOPIC) return;
  try {
    await fetch("https://ntfy.sh/" + encodeURIComponent(NTFY_TOPIC), {
      method: "POST",
      body: "Intrusion detected at " + location + " - " + new Date().toLocaleString(),
      headers: { Title: "DOOR ALARM ALERT", Priority: "urgent", Tags: "rotating_light" },
    });
  } catch (e) {
    console.log("Phone alert failed:", e.message);
  }
}

// ---- Route WebSocket connections by path ----
server.on("upgrade", (req, socket, head) => {
  const url = new URL(req.url, "http://localhost");
  if (url.pathname === "/device") {
    if (url.searchParams.get("key") !== DEVICE_KEY) {
      socket.write("HTTP/1.1 401 Unauthorized\r\n\r\n");
      socket.destroy();
      return;
    }
    deviceWss.handleUpgrade(req, socket, head, (ws) => {
      ws.location = (url.searchParams.get("loc") || "Unknown").slice(0, 40);
      deviceWss.emit("connection", ws, req);
    });
  } else if (url.pathname === "/dashboard") {
    if (DASH_KEY && url.searchParams.get("key") !== DASH_KEY) {
      socket.write("HTTP/1.1 401 Unauthorized\r\n\r\n");
      socket.destroy();
      return;
    }
    dashWss.handleUpgrade(req, socket, head, (ws) => dashWss.emit("connection", ws, req));
  } else {
    socket.destroy();
  }
});

// ---- ESP32 devices ----
deviceWss.on("connection", (ws) => {
  const name = ws.location;
  ws.isAlive = true;
  ws.on("pong", () => (ws.isAlive = true));

  const old = locations[name];
  if (old && old.ws && old.ws !== ws) old.ws.terminate();
  locations[name] = {
    name,
    state: old ? old.state : "unknown",
    online: true,
    lastSeen: Date.now(),
    ws,
  };
  console.log("Device connected:", name);
  broadcast();

  ws.on("message", (data) => {
    let m;
    try {
      m = JSON.parse(data.toString());
    } catch (e) {
      return;
    }
    const loc = locations[name];
    if (!loc || loc.ws !== ws) return;
    loc.lastSeen = Date.now();

    if (m.type === "status" && typeof m.state === "string") {
      const prev = loc.state;
      loc.state = m.state;
      if (prev !== "unknown" && prev !== m.state && (m.state === "armed" || m.state === "disarmed")) {
        addEvent(m.state, name);
      }
    } else if (m.type === "alert") {
      loc.state = "alarm";
      addEvent("alert", name, { distance: m.distance, baseline: m.baseline });
      console.log("ALERT at", name);
      notifyPhone(name);
    }
    broadcast();
  });

  ws.on("close", () => {
    const loc = locations[name];
    if (loc && loc.ws === ws) {
      loc.online = false;
      loc.ws = null;
      console.log("Device disconnected:", name);
      broadcast();
    }
  });
  ws.on("error", () => {});
});

// ---- Dashboard browsers ----
dashWss.on("connection", (ws, req) => {
  ws.isAlive = true;
  ws.authenticated = false;
  ws.pinRequired = Boolean(DASH_PIN);
  ws.on("pong", () => (ws.isAlive = true));
  ws.send(JSON.stringify({ type: "auth-required", pinRequired: ws.pinRequired }));

  ws.on("message", (data) => {
    let m;
    try {
      m = JSON.parse(data.toString());
    } catch (e) {
      return;
    }
    if (!ws.authenticated) {
      if (m.type !== "auth" || typeof m.pin !== "string") return;

      if (!ws.pinRequired) {
        ws.authenticated = true;
        ws.send(JSON.stringify({ type: "auth-result", ok: true }));
        ws.send(snapshot());
        return;
      }

      const forwardedFor = req.headers["x-forwarded-for"];
      const clientAddress = (typeof forwardedFor === "string" && forwardedFor.split(",")[0].trim()) ||
        req.socket.remoteAddress ||
        "unknown";
      const now = Date.now();
      let attempts = pinAttempts.get(clientAddress);
      if (!attempts || now - attempts.windowStartedAt >= PIN_ATTEMPT_WINDOW_MS) {
        attempts = { count: 0, windowStartedAt: now };
        pinAttempts.set(clientAddress, attempts);
      }
      if (attempts.count >= MAX_PIN_ATTEMPTS) {
        const retryMinutes = Math.ceil((PIN_ATTEMPT_WINDOW_MS - (now - attempts.windowStartedAt)) / 60000);
        ws.send(JSON.stringify({
          type: "auth-result",
          ok: false,
          text: `Too many failed attempts. Try again in about ${retryMinutes} minutes.`,
        }));
        return;
      }

      const submittedPinHash = crypto.createHash("sha256").update(m.pin).digest();
      if (!crypto.timingSafeEqual(submittedPinHash, dashboardPinHash)) {
        attempts.count += 1;
        ws.send(JSON.stringify({
          type: "auth-result",
          ok: false,
          text: attempts.count >= MAX_PIN_ATTEMPTS
            ? "Too many failed attempts. Try again in 15 minutes."
            : "Incorrect PIN. Please try again.",
        }));
        return;
      }
      pinAttempts.delete(clientAddress);
      ws.authenticated = true;
      ws.send(JSON.stringify({ type: "auth-result", ok: true }));
      ws.send(snapshot());
      return;
    }

    if (m.type === "cmd" && (m.cmd === "arm" || m.cmd === "disarm")) {
      const targets = m.location === "*" ? Object.values(locations) : [locations[m.location]];
      let sent = 0;
      targets.forEach((loc) => {
        if (loc && loc.online && loc.ws && loc.ws.readyState === 1) {
          loc.ws.send(JSON.stringify({ cmd: m.cmd }));
          sent++;
        }
      });
      if (!sent) ws.send(JSON.stringify({ type: "notice", text: "No device is online" }));
    }
  });
  ws.on("error", () => {});
});

// ---- Drop dead connections ----
setInterval(() => {
  [deviceWss, dashWss].forEach((wss) =>
    wss.clients.forEach((ws) => {
      if (ws.isAlive === false) return ws.terminate();
      ws.isAlive = false;
      ws.ping();
    })
  );
}, 30000);

server.listen(PORT, () => {
  console.log("Dashboard: http://localhost:" + PORT);
});
