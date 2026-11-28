import { inject, type InjectionKey } from "vue";
import type { NodeEventMessaging } from "../../src/nodeEventMessaging";

export const nodeEventMessagingKey: InjectionKey<NodeEventMessaging> = Symbol("iv.nodeEvents");
export function useNodeEventMessaging(): NodeEventMessaging {
    const messaging = inject(nodeEventMessagingKey, null);
    if (!messaging) throw new Error("Node presentations require the webview's node-event connection");
    return messaging;
}
