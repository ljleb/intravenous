import { inject, provide, type InjectionKey, type Ref } from "vue";

import type { SemanticNode } from "./semanticNode";

const nodeKey: InjectionKey<Readonly<Ref<SemanticNode>>> = Symbol("iv.node");

// Called by the presentation host for the node it presents.
export function provideNode(node: Readonly<Ref<SemanticNode>>): void {
    provide(nodeKey, node);
}

// The semantic node of the enclosing presentation host. Templates read it
// directly (`node.ports`); scripts read `node.value`.
export function useNode(): Readonly<Ref<SemanticNode>> {
    const node = inject(nodeKey, null);
    if (!node) {
        throw new Error("useNode() requires an enclosing node presentation host");
    }
    return node;
}
