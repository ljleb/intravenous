// Generated from the compiler's neutral node-event protocol. Do not edit.
import { defineNodeEvent } from "../nodeEvents";

export type AddPointPayload = { readonly "point": { readonly "id": number; readonly "seconds": number; readonly "value": number } };
export const AddPoint = defineNodeEvent<AddPointPayload>({"nominalId":"c:$@N@iv@N@automation@S@AddPoint","definitionFingerprint":"eaa7c7d29c7c5d9d"}, {"fields":[{"name":"point","value":{"fields":[{"name":"id","value":{"bitWidth":32,"kind":"unsignedInteger"}},{"name":"seconds","value":{"bitWidth":64,"kind":"floatingPoint"}},{"name":"value","value":{"bitWidth":32,"kind":"floatingPoint"}}],"kind":"aggregate"}}],"kind":"aggregate"});

export type MovePointPayload = { readonly "id": number; readonly "seconds": number; readonly "value": number };
export const MovePoint = defineNodeEvent<MovePointPayload>({"nominalId":"c:$@N@iv@N@automation@S@MovePoint","definitionFingerprint":"d2c84e01a665e0d0"}, {"fields":[{"name":"id","value":{"bitWidth":32,"kind":"unsignedInteger"}},{"name":"seconds","value":{"bitWidth":64,"kind":"floatingPoint"}},{"name":"value","value":{"bitWidth":32,"kind":"floatingPoint"}}],"kind":"aggregate"});

export type RemovePointPayload = { readonly "id": number };
export const RemovePoint = defineNodeEvent<RemovePointPayload>({"nominalId":"c:$@N@iv@N@automation@S@RemovePoint","definitionFingerprint":"c1b7cfa2c11beeb0"}, {"fields":[{"name":"id","value":{"bitWidth":32,"kind":"unsignedInteger"}}],"kind":"aggregate"});
