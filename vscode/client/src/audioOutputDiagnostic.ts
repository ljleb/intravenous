import type { AudioDeviceSelection } from "./workspaceRpc";

export function audioOutputDiagnostic(output: AudioDeviceSelection): string {
    if (output.deviceId === null) return "Intravenous audio output: disabled; Tick uses the silent fallback clock";
    const device = `${output.name ?? output.deviceId} [${output.deviceId}]`;
    return `Intravenous audio output: ${device}; ${output.available ? "active" : "unavailable; Tick uses the silent fallback clock"}`
        + (output.error ? `; ${output.error}` : "");
}
