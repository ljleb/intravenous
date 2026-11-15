import { inject, provide, type InjectionKey, type Ref } from "vue";

import type { SemanticNode } from "./semanticNode";
import type { NodeEvent } from "../../src/nodeEvents";
import type { NodeEventGesture } from "../../src/nodeEventMessaging";

export type PresentedNode = SemanticNode & { send(event: NodeEvent): Promise<void>; gesture(): NodeEventGesture };

const nodeKey: InjectionKey<Readonly<Ref<PresentedNode>>> = Symbol("iv.node");

// Called by the presentation host for the node it presents.
export function provideNode(node: Readonly<Ref<PresentedNode>>): void {
    provide(nodeKey, node);
}

// The semantic node of the enclosing presentation host. Templates read it
// directly (`node.ports`); scripts read `node.value`.
export function useNode(): Readonly<Ref<PresentedNode>> {
    const node = inject(nodeKey, null);
    if (!node) {
        throw new Error("useNode() requires an enclosing node presentation host");
    }
    return node;
}
