/*
 *  © 2026 Phil Claridge
 *
 *  This file is part of CommandStation-EX
 *
 *  This is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  It is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with CommandStation.  If not, see <https://www.gnu.org/licenses/>.
 */

// A complete page, served at /throttle. It talks to the station over the
// WebSocket on port 2560, so the speeds shown are the ones the station holds,
// whichever throttle set them.

const char Throttle_html[]=R"???(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>DCC-EX Throttle</title>
<style>
body { font-family: sans-serif; margin: 1em auto 6em; padding: 0 1em; max-width: 40em; }
table { border-collapse: collapse; width: 100%; }
th, td { text-align: left; padding: 0.3em 0.5em; border-bottom: 1px solid #ccc; }
/* 16px stops an iPhone zooming in when a field takes focus; manipulation
   stops a quick second tap being read as a zoom */
button, input { font-size: 16px; touch-action: manipulation; }
button { min-height: 44px; min-width: 44px; padding: 0.5em 0.8em; }
td button { margin: 0.2em 0.5em 0.2em 0; }
.train span { flex: 1 1 12em; }
@media (max-width: 30em) { th, td { padding: 0.3em 0.2em; } .train span { flex-basis: 100%; } }
button.danger:enabled { color: #b00; border: 1px solid #b00; background: #fff; }
button[aria-pressed=true]:enabled { color: #fff; background: #333; }
input[type=number] { width: 6em; min-height: 44px; box-sizing: border-box; padding: 0.4em; }
input[type=range] { width: 100%; height: 44px; }
input[type=range]::-webkit-slider-thumb { -webkit-appearance: none; width: 44px; height: 44px; margin-top: -19px; border-radius: 50%; background: #333; }
input[type=range]::-webkit-slider-runnable-track { height: 6px; background: #ccc; }
input[type=range]::-moz-range-thumb { width: 44px; height: 44px; border: 0; border-radius: 50%; background: #333; }
input[type=range]::-moz-range-track { height: 6px; background: #ccc; }
input[type=range] { -webkit-appearance: none; background: none; }
/* A train is a line of text and its buttons; on a phone the buttons wrap
   under the text */
.train { display: flex; flex-wrap: wrap; align-items: center; gap: 0.3em 0.5em; padding: 0.3em 0.5em; border-bottom: 1px solid #ccc; }
.train.selected { background: #eee; }
#functions button { margin: 0.2em 0.4em 0.2em 0; }
#functions { margin: 0; }
summary { min-height: 44px; line-height: 44px; cursor: pointer; }
#status.down { font-weight: bold; color: #b00; }
#powerNote { font-weight: bold; padding: 0.8em; border: 2px solid #b00; }
#estop { position: fixed; left: 0; right: 0; bottom: 0; width: 100%; min-height: 72px; padding-bottom: env(safe-area-inset-bottom); border: 0; background: #ddd; font-size: 1.3em; font-weight: bold; }
#estop:enabled { color: #fff; background: #c00; }
#speedText { display: block; font-size: 2em; font-weight: bold; }
#speedRow { display: flex; align-items: center; gap: 0.5em; }
#speedRow button { min-width: 52px; min-height: 52px; font-size: 1.5em; }
</style>
</head>
<body>
<p id="status" class="down">Connecting...</p>

<div id="powerBox">
<h2>Track power</h2>
<p>
  <output id="power">Unknown</output>
  <button id="powerOn" aria-pressed="false">On</button>
  <button id="powerOff" aria-pressed="false">Off</button>
</p>
<p id="powerNote" hidden></p>
</div>

<div id="needsPower" hidden>
<h2>Drive</h2>
<p>
  <label>Add or drive loco
    <input type="number" id="address" min="1" max="10239" step="1" inputmode="numeric" placeholder="address">
  </label>
  <button id="addressGo">Drive</button>
</p>
<fieldset id="drive" disabled hidden>
  <legend id="driving"></legend>
  <p>Speed <output id="speedText">0</output></p>
  <div id="speedRow">
    <button id="slower" aria-label="One step slower">&minus;</button>
    <input type="range" id="speed" min="0" max="126" value="0">
    <button id="faster" aria-label="One step faster">+</button>
  </div>
  <p>
    <button id="reverse" aria-pressed="false">Reverse</button>
    <button id="forward" aria-pressed="true">Forward</button>
    <button id="stop">Stop</button>
  </p>
  <details>
    <summary id="functionsTitle">Functions</summary>
    <p id="functions"></p>
  </details>
</fieldset>
<button id="estop" disabled>Emergency stop all</button>

<h2>Trains being driven</h2>
<div id="trains"></div>
<p id="noTrains">None yet. Enter an address above.</p>

<h2>Roster</h2>
<table>
  <thead><tr><th>Address</th><th>Name</th><th></th></tr></thead>
  <tbody id="roster"></tbody>
</table>
<p>Locos are added to the roster in the <a href="/Roster.html">Roster Editor</a>.</p>
</div>

<script>
"use strict";
const SEND_GAP_MS = 50;     // the station takes one frame per network read
const RECONNECT_MS = 2000;
const WAKE_CHECK_MS = 2000; // reply time allowed when the page comes back
const SLIDER_HOLD_MS = 500; // own slider is left alone this long after a touch
const ESTOP = -1;
const FUNCTION_BUTTONS = 9; // F0-F8

const byId = (id) => document.getElementById(id);
const trains = new Map();      // address -> { speed, forward, functions }
const tracks = new Map();      // track letter -> powered
const rosterNames = new Map(); // address -> name
let socket = null;
let selected = 0;
let sliderTouched = 0;
let trainRows = ""; // the addresses the trains table was last built for
let wakeCheck = 0; // timer while a link check is waiting for its reply

// --- sending: one command per frame, paced, and a slider drag replaces its
// own unsent command so a slow link keeps only the latest speed
const queue = []; // { key, command }
let sending = false;

function send(command, key) {
  const waiting = key ? queue.find((entry) => entry.key === key) : null;
  if (waiting) waiting.command = command;
  else queue.push({ key, command });
  if (!sending) sendNext();
}

function sendNext() {
  sending = false;
  if (!queue.length || !socket || socket.readyState !== WebSocket.OPEN) return;
  socket.send(queue.shift().command);
  sending = true;
  setTimeout(sendNext, SEND_GAP_MS);
}

// --- the station's replies

// <l cab 0 speedByte functions>. Bit 7 of the byte is forward; below that,
// 0 is stop, 1 is emergency stop, and 2-127 are speeds 1-126.
// The function map is a bitmap, bit n being Fn.
function decodeLoco(speedByte, functions) {
  const value = speedByte & 0x7f;
  return {
    speed: value === 1 ? ESTOP : Math.max(value - 1, 0),
    forward: speedByte >= 128,
    functions: functions,
  };
}

// <D CABS> is the only list of the loco table. It is one multi-line reply:
// a line for each loco, of which s= is the speed now, f= the function map and
// t= the speed commanded.
function readCabList(text) {
  if (wakeCheck) {
    clearTimeout(wakeCheck);
    wakeCheck = 0;
    byId("status").textContent = "Connected";
    byId("status").className = "";
  }
  trains.clear();
  for (const match of text.matchAll(/Loco=(\d+)\s+s=\d+\s+f=(\d+)\s+t=(\d+)/g)) {
    if (Number(match[1]) > 0) {
      trains.set(Number(match[1]), decodeLoco(Number(match[3]), Number(match[2])));
    }
  }
  showTrains();
}

function readLine(line) {
  let match = line.match(/^<l (\d+) -?\d+ (\d+) (-?\d+)>$/);
  if (match) {
    // Address 0 is "no such loco" or an all-stop, so it carries no state.
    if (Number(match[1]) > 0) {
      trains.set(Number(match[1]), decodeLoco(Number(match[2]), Math.max(Number(match[3]), 0)));
      showTrains();
    }
    return;
  }
  // Power comes as one line per track letter, then a summary line. A bare
  // <p0> is everything off; the other summaries repeat what the letters said.
  match = line.match(/^<p([01]) ([A-H])>$/);
  if (match) {
    tracks.set(match[2], match[1] === "1");
    showPower();
    return;
  }
  if (line === "<p0>") {
    for (const letter of tracks.keys()) tracks.set(letter, false);
    showPower();
    return;
  }
  match = line.match(/^<- (\d+)>$/);
  if (match) {
    trains.delete(Number(match[1]));
    showTrains();
    return;
  }
  match = line.match(/^<jR((?: \d+)*)>$/);
  if (match) {
    const ids = match[1].trim().split(" ").filter(Boolean).map(Number);
    rosterNames.clear();
    for (const id of ids) {
      rosterNames.set(id, "");
      send("<JR " + id + ">");
    }
    showRoster();
    return;
  }
  match = line.match(/^<jR (\d+) "(.*)" ".*">$/);
  if (match) {
    rosterNames.set(Number(match[1]), match[2]);
    showRoster();
    showTrains();
  }
}

function readFrame(text) {
  const cabList = text.match(/<\* LocoSlots[\s\S]*?\*>/);
  if (cabList) readCabList(cabList[0]);
  // Frames from a WebSocket end their lines in \r
  text.split(/[\r\n]+/).forEach((line) => readLine(line.trim()));
}

// --- the link

function connect() {
  // The station's handshake always names this subprotocol, so it must be asked for
  const link = new WebSocket("ws://" + location.hostname + ":2560", "DCCEX");
  socket = link;
  link.onopen = () => {
    byId("status").textContent = "Connected";
    byId("status").className = "";
    showDrive();
    send("<s>"); // reports the power on each track
    send("<D CABS>");
    send("<JR>");
  };
  link.onmessage = (event) => readFrame(event.data);
  link.onclose = () => {
    if (link === socket) linkDown();
  };
}

function linkDown() {
  clearTimeout(wakeCheck);
  wakeCheck = 0;
  socket = null;
  queue.length = 0;
  byId("status").textContent = "Not connected to the station: trains keep running. Retrying...";
  byId("status").className = "down";
  showDrive();
  setTimeout(connect, RECONNECT_MS);
}

// A phone that slept may hold a socket that is dead without the browser saying
// so. Only a fresh reply proves the link: frames from before the sleep do not.
function checkLink() {
  if (wakeCheck || !socket || socket.readyState !== WebSocket.OPEN) return;
  const link = socket;
  byId("status").textContent = "Checking...";
  byId("status").className = "down";
  wakeCheck = setTimeout(() => {
    linkDown();
    link.close();
  }, WAKE_CHECK_MS);
  showDrive();
  send("<D CABS>");
}

// --- the screen

function speedText(train) {
  if (train.speed === ESTOP) return "Emergency stop";
  return train.speed + " (" + Math.round(train.speed * 100 / 126) + "%)";
}

function cell(row, text) {
  const td = row.insertCell();
  td.textContent = text;
  return td;
}

// The list is rebuilt only when a loco joins or leaves. A speed change
// rewrites the text in place, because replacing a button while a finger is
// on it loses the tap.
function showTrains() {
  const list = byId("trains");
  const addresses = [...trains.keys()].sort((a, b) => a - b);
  if (addresses.join(",") !== trainRows) {
    trainRows = addresses.join(",");
    list.textContent = "";
    for (const address of addresses) {
      const row = document.createElement("div");
      row.appendChild(document.createElement("span"));
      addButton(row, "Select", () => select(address));
      addButton(row, "Stop", () => stop(address));
      addButton(row, "Forget", () => forget(address), "danger");
      list.appendChild(row);
    }
  }
  addresses.forEach((address, index) => {
    const train = trains.get(address);
    const row = list.children[index];
    const name = rosterNames.get(address);
    row.className = address === selected ? "train selected" : "train";
    row.firstChild.textContent = "Loco " + address + (name ? " " + name : "") + ": " +
      speedText(train) + (train.speed === ESTOP ? "" : train.forward ? " forward" : " reverse");
    row.children[1].setAttribute("aria-pressed", address === selected);
  });
  byId("noTrains").hidden = addresses.length > 0;
  showPower();
  showDrive();
}

function addButton(parent, label, onClick, className) {
  const button = document.createElement("button");
  button.textContent = label;
  if (className) button.className = className;
  button.onclick = onClick;
  parent.appendChild(button);
}

function showRoster() {
  const body = byId("roster");
  body.textContent = "";
  for (const address of [...rosterNames.keys()].sort((a, b) => a - b)) {
    const row = body.insertRow();
    cell(row, address);
    cell(row, rosterNames.get(address));
    addButton(row.insertCell(), "Drive", () => drive(address));
  }
  if (!rosterNames.size) {
    const td = cell(body.insertRow(), "No locos in the roster yet.");
    td.colSpan = 3;
  }
}

function showPower() {
  const on = [...tracks.keys()].filter((letter) => tracks.get(letter)).sort();
  byId("power").textContent = on.length ? "On (track " + on.join(", ") + ")" : "Off";
  byId("powerOn").setAttribute("aria-pressed", on.length > 0);
  byId("powerOff").setAttribute("aria-pressed", on.length === 0);
  // With the power off nothing below can move a train, so it is hidden and
  // the note says why. Speeds are held by the station through a power cut,
  // so the note also warns of locos that will move when it comes back.
  const moving = [...trains.values()].filter((train) => train.speed > 0).length;
  byId("needsPower").hidden = on.length === 0;
  // Off: the power buttons lead the page, being the only thing to do. On:
  // they go to the foot, so the trains stay on a phone's first screen.
  const box = byId("powerBox");
  const wanted = on.length ? null : byId("needsPower");
  if (on.length ? box.previousElementSibling !== byId("needsPower") : box.nextElementSibling !== wanted) {
    byId("needsPower").parentNode.insertBefore(box, on.length ? byId("needsPower").nextSibling : wanted);
  }
  byId("powerNote").hidden = on.length > 0 || !tracks.size;
  byId("powerNote").textContent = "Track power is off. Turn it on to drive." +
    (moving ? " " + moving + (moving === 1 ? " loco has" : " locos have") +
      " a speed set and will move when the power comes on." : "");
}

// Built once and then only marked on or off, for the same reason as the rows.
function showFunctions(train) {
  const box = byId("functions");
  for (let n = box.children.length; n < FUNCTION_BUTTONS; n++) {
    addButton(box, n === 0 ? "F0 light" : "F" + n, () => toggleFunction(n));
  }
  const on = [];
  for (let n = 0; n < FUNCTION_BUTTONS; n++) {
    const isOn = ((train.functions >> n) & 1) === 1;
    box.children[n].setAttribute("aria-pressed", isOn);
    if (isOn) on.push("F" + n);
  }
  // The closed heading still says what is on
  byId("functionsTitle").textContent = "Functions" + (on.length ? ": " + on.join(", ") + " on" : "");
}

function showDrive() {
  const train = trains.get(selected) || { speed: 0, forward: true, functions: 0 };
  const connected = socket !== null && socket.readyState === WebSocket.OPEN && !wakeCheck;
  byId("powerOn").disabled = !connected;
  byId("powerOff").disabled = !connected;
  showFunctions(train);
  byId("drive").hidden = !selected;
  byId("drive").disabled = !connected;
  byId("driving").textContent = "Loco " + selected + " " + (rosterNames.get(selected) || "");
  byId("forward").setAttribute("aria-pressed", train.forward);
  byId("reverse").setAttribute("aria-pressed", !train.forward);
  byId("estop").disabled = !connected;
  if (Date.now() - sliderTouched > SLIDER_HOLD_MS) {
    byId("speed").value = Math.max(train.speed, 0);
    byId("speedText").textContent = speedText(train);
  }
}

// --- what the user can do

function select(address) {
  selected = address;
  sliderTouched = 0;
  byId("address").value = address || "";
  // A loco the station already holds replies with its speed and functions;
  // one it does not hold replies with address 0, which is ignored.
  if (address) send("<t " + address + ">");
  showTrains();
}

function toggleFunction(n) {
  const train = trains.get(selected);
  const on = train ? ((train.functions >> n) & 1) === 1 : false;
  send("<F " + selected + " " + n + " " + (on ? 0 : 1) + ">");
}

// From the address box or the roster. A new address is added to the
// station's list at speed 0, so it shows under Trains being driven at once;
// a known one is just selected.
function drive(address) {
  if (!Number.isInteger(address) || address < 1 || address > 10239) return;
  if (!trains.has(address)) send("<t " + address + " 0 1>", "speed" + address);
  select(address);
}

function setSpeed(speed, forward) {
  send("<t " + selected + " " + speed + " " + (forward ? 1 : 0) + ">", "speed" + selected);
}

// Stops any loco in the list, not only the one being driven, keeping its direction.
function stop(address) {
  const train = trains.get(address);
  if (address === selected) sliderTouched = 0;
  send("<t " + address + " 0 " + (train && !train.forward ? 0 : 1) + ">", "speed" + address);
}

function isForward() {
  const train = trains.get(selected);
  return train ? train.forward : true;
}

function forget(address) {
  if (!confirm("Forget loco " + address + "? It is emergency stopped and no longer refreshed.")) return;
  send("<- " + address + ">");
  if (address === selected) select(0);
}

byId("address").onchange = () => drive(Number(byId("address").value));
byId("addressGo").onclick = () => drive(Number(byId("address").value));
byId("powerOn").onclick = () => send("<1 MAIN>");
byId("powerOff").onclick = () => send("<0>");
function sliderMoved() {
  const speed = Number(byId("speed").value);
  sliderTouched = Date.now();
  byId("speedText").textContent = speedText({ speed: speed });
  setSpeed(speed, isForward());
}

// One speed step per tap, for fine control where a thumb on the slider is coarse
function nudge(step) {
  byId("speed").value = Math.min(Math.max(Number(byId("speed").value) + step, 0), 126);
  sliderMoved();
}

byId("speed").oninput = sliderMoved;
byId("forward").onclick =() => setSpeed(Number(byId("speed").value), true);
byId("reverse").onclick = () => setSpeed(Number(byId("speed").value), false);
byId("stop").onclick = () => stop(selected);
// <!> emergency stops every loco the station holds, whichever throttle
// started it; the station then reports each one, which updates the list.
byId("estop").onclick = () => { sliderTouched = 0; send("<!>"); };
byId("slower").onclick = () => nudge(-1);
byId("faster").onclick = () => nudge(1);
document.addEventListener("visibilitychange", () => {
  if (!document.hidden) checkLink();
});
window.addEventListener("pageshow", checkLink);

connect();
</script>
</body>
</html>
)???";

// The config menu pastes a page's HTML into its own panel, where a whole page
// with a script cannot run, so the menu gets this link to the page instead.
const char ThrottleMenu_html[]=R"???(<p><a href="/throttle" target="_blank">Open the throttle</a> in a new tab.</p>
<p>It drives locos by address and follows changes made by any other throttle.</p>
)???";
