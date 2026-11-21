// Generated from the compiler's neutral node-event protocol. Do not edit.
import { defineNodeEvent } from "../nodeEvents";

export type SetBeatGridPayload = { readonly "bpm": number; readonly "beats_per_bar": number; readonly "subdivisions": ReadonlyArray<number> };
export const SetBeatGrid = defineNodeEvent<SetBeatGridPayload>({"nominalId":"c:$@N@iv@S@SetBeatGrid","definitionFingerprint":"b2a36c5cb2dc68f3"}, {"fields":[{"name":"bpm","value":{"bitWidth":64,"kind":"floatingPoint"}},{"name":"beats_per_bar","value":{"bitWidth":32,"kind":"unsignedInteger"}},{"name":"subdivisions","value":{"element":{"bitWidth":32,"kind":"unsignedInteger"},"kind":"sequence"}}],"kind":"aggregate"});
