/**
 * Payload decoder for CubeCell water level (Port 2, big-endian).
 * Current: 14 bytes. Legacy: 11 or 10 bytes (no temperature).
 *
 * Drop-in for The Things Stack / ChirpStack v4 (`decodeUplink`).
 * Older stacks: use `Decoder(bytes, port)` below the same way.
 */

function u16(bytes, i) {
  return (bytes[i] << 8) | bytes[i + 1];
}

function i16(bytes, i) {
  const v = u16(bytes, i);
  return v > 0x7fff ? v - 0x10000 : v;
}

function sensorStatusLabel(code) {
  if (code === 1) return "ok";
  if (code === 0) return "missing";
  if (code === 9) return "timeout";
  return "unknown";
}

function leakStatusLabel(code) {
  if (code === 1) return "wet";
  if (code === 0) return "dry";
  return "unknown";
}

function decodePayload(bytes, port) {
  if (port !== 2) {
    return { errors: ["unexpected port " + port] };
  }
  if (!bytes || (bytes.length !== 14 && bytes.length !== 11 && bytes.length !== 10)) {
    return {
      errors: ["expected 14 bytes (or legacy 11/10), got " + (bytes ? bytes.length : 0)],
    };
  }

  const sensor1Status = bytes[8];
  const sensor2Status = bytes[9];
  const leak = bytes.length >= 11 ? bytes[10] : 0;
  const hasTemp = bytes.length >= 14;
  const tempCentiC = hasTemp ? i16(bytes, 11) : null;
  const tempStatus = hasTemp ? bytes[13] : 0;

  const data = {
    counter: u16(bytes, 0),
    battery_mV: u16(bytes, 2),
    distance1_mm: u16(bytes, 4),
    distance2_mm: u16(bytes, 6),
    sensor1_status: sensor1Status,
    sensor1_status_label: sensorStatusLabel(sensor1Status),
    sensor2_status: sensor2Status,
    sensor2_status_label: sensorStatusLabel(sensor2Status),
    sensor1_ok: sensor1Status === 1,
    sensor2_ok: sensor2Status === 1,
    leak: leak,
    leak_label: leakStatusLabel(leak),
    leak_wet: leak === 1,
  };

  if (hasTemp) {
    data.temperature_centi_C = tempCentiC;
    data.temperature_C = tempStatus === 1 ? tempCentiC / 100 : null;
    data.temp_status = tempStatus;
    data.temp_status_label = sensorStatusLabel(tempStatus);
    data.temp_ok = tempStatus === 1;
  }

  return { data: data };
}

// The Things Stack / ChirpStack v4
function decodeUplink(input) {
  return decodePayload(input.bytes, input.fPort);
}

// ChirpStack v3 / older TTN
function Decoder(bytes, port) {
  const decoded = decodePayload(bytes, port);
  return decoded.data || { error: (decoded.errors || []).join("; ") };
}

// Optional: encode interval / reset downlinks (seconds are big-endian u16).
function encodeDownlink(input) {
  const cmd = input.data || {};
  if (typeof cmd.interval_s === "number") {
    const s = Math.max(0, Math.min(0xffff, Math.round(cmd.interval_s)));
    return { bytes: [(s >> 8) & 0xff, s & 0xff], fPort: 4 };
  }
  if (cmd.reset === true) {
    return { bytes: [1], fPort: 9 };
  }
  return { errors: ["unsupported downlink; use interval_s or reset"] };
}
