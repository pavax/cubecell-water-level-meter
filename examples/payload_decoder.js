/**
 * Payload decoder for CubeCell water level (Port 2, 10 bytes, big-endian).
 *
 * Drop-in for The Things Stack / ChirpStack v4 (`decodeUplink`).
 * Older stacks: use `Decoder(bytes, port)` below the same way.
 */

function u16(bytes, i) {
  return (bytes[i] << 8) | bytes[i + 1];
}

function sensorStatusLabel(code) {
  if (code === 1) return "ok";
  if (code === 0) return "missing";
  if (code === 9) return "timeout";
  return "unknown";
}

function decodePayload(bytes, port) {
  if (port !== 2) {
    return { errors: ["unexpected port " + port] };
  }
  if (!bytes || bytes.length !== 10) {
    return { errors: ["expected 10 bytes, got " + (bytes ? bytes.length : 0)] };
  }

  const status1 = bytes[8];
  const status2 = bytes[9];
  return {
    data: {
      counter: u16(bytes, 0),
      battery_mV: u16(bytes, 2),
      distance1_mm: u16(bytes, 4),
      distance2_mm: u16(bytes, 6),
      status1: status1,
      status1_label: sensorStatusLabel(status1),
      status2: status2,
      status2_label: sensorStatusLabel(status2),
      level1_ok: status1 === 1,
      level2_ok: status2 === 1,
    },
  };
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
