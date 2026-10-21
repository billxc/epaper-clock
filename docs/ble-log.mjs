// Connection diagnostics: metadata only. Never pass packets, credentials,
// PoP, cryptographic material, device IDs or complete browser/device objects.
export function bleLog(event, details = {}) {
  console.info('[epaper-ble]', event, { at: new Date().toISOString(), ...details });
}
export function bleFailure(event, error, details = {}) {
  console.error('[epaper-ble]', event, {
    at: new Date().toISOString(), ...details,
    error: error.name, message: error.message,
  });
}
