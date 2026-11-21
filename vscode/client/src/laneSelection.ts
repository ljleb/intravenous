import { addressKey, isProjectInstance, type GraphScope, type GraphScopeNode, type NodeAddress } from "./graphScope";
import { updateSelection } from "./selection";

export function orderLanes<T extends { address: NodeAddress }>(lanes: readonly T[], order: readonly string[]): T[] {
    const remaining = new Map(lanes.map((lane) => [addressKey(lane.address), lane]));
    const ordered: T[] = [];
    for (const key of order) {
        const lane = remaining.get(key);
        if (!lane) continue;
        ordered.push(lane);
        remaining.delete(key);
    }
    return [...ordered, ...remaining.values()];
}

export type GraphLane = { node: GraphScopeNode; depth: number; rowKey: string };

export function expandedGraphLanes(scope: GraphScope, order: readonly string[],
    expanded: Pick<ReadonlySet<string>, "has">): GraphLane[] {
    const childNodes = new Map(scope.childScopes?.map(({ subject, nodes }) => [addressKey(subject), nodes]));
    const lanes: GraphLane[] = [];
    const append = (nodes: readonly GraphScopeNode[], depth: number, parentKey?: string) => {
        for (const node of nodes) {
            const key = addressKey(node.address);
            const membership = addressKey(node.membershipAddress ?? node.address);
            const rowKey = parentKey === undefined ? membership : `${parentKey}/${membership}`;
            lanes.push({ node, depth, rowKey });
            const children = childNodes.get(key);
            if (children && expanded.has(key)) append(children, depth + 1, rowKey);
        }
    };
    append(orderLanes(scope.nodes, order), 0);
    return lanes;
}

export function selectedProjectLaneIds(lanes: readonly GraphLane[], selected: ReadonlySet<string>): string[] {
    return lanes.filter(({ node }) => isProjectInstance(node.address) && selected.has(addressKey(node.address)))
        .map(({ node }) => node.address.instanceId);
}

export function selectLaneRange(selected: ReadonlySet<string>, order: readonly string[], anchor: string | null,
    clicked: string, modifiers: { shiftKey?: boolean; ctrlKey?: boolean; metaKey?: boolean } = {}) {
    const first = anchor === null ? -1 : order.indexOf(anchor);
    const last = order.indexOf(clicked);
    if (modifiers.shiftKey && first >= 0 && last >= 0) {
        const range = order.slice(Math.min(first, last), Math.max(first, last) + 1);
        return { selected: new Set(modifiers.ctrlKey || modifiers.metaKey ? [...selected, ...range] : range), anchor };
    }
    return { selected: updateSelection(selected, [clicked], modifiers), anchor: clicked };
}

export function moveSelectedLanes(order: readonly string[], selected: ReadonlySet<string>, direction: -1 | 1): readonly string[] {
    const next = [...order];
    let moved = false;
    for (let i = direction < 0 ? 1 : next.length - 2; i >= 0 && i < next.length; i -= direction) {
        const neighbor = i + direction;
        if (neighbor >= next.length || neighbor < 0 || !selected.has(next[i]) || selected.has(next[neighbor])) continue;
        [next[i], next[neighbor]] = [next[neighbor], next[i]];
        moved = true;
    }
    return moved ? next : order;
}

export function mergeLaneOrder(saved: readonly string[], visible: readonly string[]): string[] {
    const present = new Set(visible);
    let next = 0;
    const result = saved.map((key) => present.has(key) ? visible[next++] : key);
    return result.concat(visible.slice(next));
}

export function insertLanes(order: readonly string[], insertions: readonly { key: string; after?: string }[]): string[] {
    const inserted = new Set(insertions.map(({ key }) => key));
    const existing = order.filter((key) => !inserted.has(key));
    const anchors = new Set(existing);
    const following = new Map<string, string[]>();
    const end: string[] = [];
    for (const { key, after } of insertions) {
        if (after === undefined || !anchors.has(after)) end.push(key);
        else {
            const group = following.get(after) ?? [];
            group.push(key);
            following.set(after, group);
        }
    }
    return existing.flatMap((key) => [key, ...(following.get(key) ?? [])]).concat(end);
}

export function laneRuns(order: readonly string[], included: ReadonlySet<string>): Array<{ begin: number; end: number }> {
    const runs: Array<{ begin: number; end: number }> = [];
    for (let i = 0; i < order.length; ++i) {
        if (!included.has(order[i])) continue;
        const last = runs.at(-1);
        if (last?.end === i) last.end = i + 1;
        else runs.push({ begin: i, end: i + 1 });
    }
    return runs;
}
