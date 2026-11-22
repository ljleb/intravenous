import { addressFromKey, addressKey, isProjectInstance, readGraphViewState, type GraphScope, type GraphScopeNode, type NodeAddress } from "./graphScope";
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

export type GraphLane = { node: GraphScopeNode; depth: number; rowKey: string; parentRowKey?: string };

function visibleLaneAddress(node: GraphScopeNode): string {
    return addressKey(node.membershipAddress ?? node.address);
}

function mergeModuleChildren(source: readonly GraphScopeNode[], project: readonly GraphScopeNode[],
    positions: ReadonlyMap<string, number>): readonly GraphScopeNode[] {
    const projectPositions = project.map((node) => positions.get(addressKey(node.address)) ?? Infinity);
    const children: GraphScopeNode[] = [];
    let next = 0;
    for (const node of source) {
        const position = positions.get(visibleLaneAddress(node)) ?? -Infinity;
        while (next < project.length && projectPositions[next] < position)
            children.push(project[next++]);
        children.push(node);
    }
    return children.concat(project.slice(next));
}

export function expandedGraphLanes(scope: GraphScope, order: readonly string[],
    expanded: Pick<ReadonlySet<string>, "has">, parents = readGraphViewState(scope.presentation).laneParents): GraphLane[] {
    const childNodes = new Map(scope.childScopes?.map(({ subject, nodes }) => [addressKey(subject), nodes]));
    const unavailable = new Set(scope.unavailableLaneParents?.map(addressKey));
    const roots = new Set(scope.nodes.map(({ address }) => address.instanceId));
    const positions = new Map(order.map((key, index) => [key, index]));
    const hosted = new Map<string, GraphScopeNode[]>();
    const topLevel: GraphScopeNode[] = [];
    const ordered = orderLanes(scope.nodes, order);
    for (const node of ordered) {
        const parent = parents[addressKey(node.address)];
        if (parent && !unavailable.has(parent) && roots.has(addressFromKey(parent).instanceId)) {
            const siblings = hosted.get(parent);
            if (siblings) siblings.push(node);
            else hosted.set(parent, [node]);
        } else topLevel.push(node);
    }
    const lanes: GraphLane[] = [];
    const append = (nodes: readonly GraphScopeNode[], depth: number, parentKey?: string) => {
        for (const node of nodes) {
            const key = addressKey(node.address);
            const membership = visibleLaneAddress(node);
            const rowKey = parentKey === undefined ? membership : `${parentKey}/${membership}`;
            lanes.push({ node, depth, rowKey, parentRowKey: parentKey });
            if (!expanded.has(key)) continue;
            const children = childNodes.get(key);
            const placed = hosted.get(membership);
            const siblings = children && placed ? mergeModuleChildren(children, placed, positions) : children ?? placed;
            if (siblings) append(siblings, depth + 1, rowKey);
        }
    };
    append(topLevel, 0);
    return lanes;
}

function laneKey({ node }: GraphLane): string { return addressKey(node.address); }
function placementKey({ node }: GraphLane): string { return visibleLaneAddress(node); }

const editableLane = ({ node }: GraphLane) => isProjectInstance(node.address);

export function movableLaneSelection(lanes: readonly GraphLane[], selected: ReadonlySet<string>): GraphLane[] {
    const roots: GraphLane[] = [];
    let selectedDepth = Infinity;
    for (const lane of lanes) {
        if (lane.depth > selectedDepth) continue;
        selectedDepth = Infinity;
        if (!editableLane(lane) || !selected.has(laneKey(lane))) continue;
        roots.push(lane);
        selectedDepth = lane.depth;
    }
    return roots;
}

export type LaneDestination = { parent?: GraphLane; before?: string; after?: string };
type LaneMove = LaneDestination & { keys: string[] };

function applyLaneMoves(order: readonly string[], parents: Readonly<Record<string, string>>, moves: readonly LaneMove[]) {
    if (!moves.length) return;
    const nextParents = { ...parents };
    for (const { keys, parent } of moves) {
        for (const key of keys) {
            if (parent) nextParents[key] = placementKey(parent);
            else delete nextParents[key];
        }
    }
    const insertions = moves.flatMap(({ keys, before, after }) => keys.map((key) => ({ key, before, after })));
    const nextOrder = insertLanes(order, insertions);
    const parentChanged = insertions.some(({ key }) => parents[key] !== nextParents[key]);
    if (parentChanged || nextOrder.length !== order.length || nextOrder.some((key, index) => key !== order[index]))
        return { order: nextOrder, parents: nextParents };
}

function laneSiblings(lanes: readonly GraphLane[]): GraphLane[][] {
    const groups = new Map<string | undefined, GraphLane[]>();
    for (const lane of lanes) {
        const group = groups.get(lane.parentRowKey);
        if (group) group.push(lane);
        else groups.set(lane.parentRowKey, [lane]);
    }
    return [...groups.values()];
}

function placeSiblingRuns(order: readonly string[], siblings: readonly string[], included: ReadonlySet<string>): string[] {
    return insertLanes(order, laneRuns(siblings, included).flatMap(({ begin, end }) =>
        siblings.slice(begin, end).map((key) => end < siblings.length
            ? { key, before: siblings[end] }
            : { key, after: siblings[begin - 1] })));
}

function saveExistingSiblingPositions(order: readonly string[], siblings: readonly string[]): readonly string[] {
    // Keep saved anchors stable across occurrences of the same C++ child.
    // If its source order changed, record the current sibling order before moving.
    const positions = new Map(order.map((key, index) => [key, index]));
    const missing = new Set<string>();
    let previous = -1;
    for (const key of siblings) {
        const position = positions.get(key);
        if (position === undefined) missing.add(key);
        else {
            if (previous > position) return mergeLaneOrder(order, siblings);
            previous = position;
        }
    }
    return missing.size ? placeSiblingRuns(order, siblings, missing) : order;
}

export function moveSelectedSiblingLanes(lanes: readonly GraphLane[], order: readonly string[],
    selected: ReadonlySet<string>, direction: -1 | 1): readonly string[] {
    let next = order;
    for (const group of laneSiblings(lanes)) {
        const movable = new Set(group.filter((lane) => editableLane(lane) && selected.has(laneKey(lane))).map(placementKey));
        if (!movable.size) continue;
        const keys = group.map(placementKey);
        const moved = moveSelectedLanes(keys, movable, direction);
        if (keys !== moved) next = placeSiblingRuns(saveExistingSiblingPositions(next, keys), moved, movable);
    }
    return next;
}

export function moveLaneNesting(lanes: readonly GraphLane[], order: readonly string[],
    parents: Readonly<Record<string, string>>, selected: ReadonlySet<string>, direction: -1 | 1) {
    const roots = movableLaneSelection(lanes, selected);
    const moving = new Set(roots.map(laneKey));
    const moves: LaneMove[] = [];
    if (direction > 0) {
        for (const group of laneSiblings(lanes)) {
            let destination: LaneMove | undefined;
            for (const lane of group) {
                const key = laneKey(lane);
                if (moving.has(key)) {
                    if (destination) destination.keys.push(key);
                    continue;
                }
                destination = lane.node.isModule ? { parent: lane, keys: [] } : undefined;
                if (destination) moves.push(destination);
            }
        }
    } else {
        const byRow = new Map(lanes.map((lane) => [lane.rowKey, lane]));
        for (const lane of roots) {
            const formerParent = lane.parentRowKey ? byRow.get(lane.parentRowKey) : undefined;
            if (!formerParent) continue;
            let parent = formerParent.parentRowKey ? byRow.get(formerParent.parentRowKey) : undefined;
            while (parent && !parent.node.isModule)
                parent = parent.parentRowKey ? byRow.get(parent.parentRowKey) : undefined;
            moves.push({ keys: [laneKey(lane)], parent,
                after: editableLane(formerParent) ? laneKey(formerParent) : undefined });
        }
    }
    return applyLaneMoves(order, parents, moves.filter(({ keys }) => keys.length));
}

export function dropSelectedLanes(lanes: readonly GraphLane[], order: readonly string[],
    parents: Readonly<Record<string, string>>, selected: ReadonlySet<string>, destination: LaneDestination) {
    const roots = movableLaneSelection(lanes, selected);
    if (!roots.length) return;
    const parent = destination.parent;
    if (parent && (!parent.node.isModule || roots.some(({ rowKey }) =>
        parent.rowKey === rowKey || parent.rowKey.startsWith(rowKey + "/")))) return;
    return applyLaneMoves(order, parents, [{ ...destination, keys: roots.map(laneKey) }]);
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

export function insertLanes(order: readonly string[],
    insertions: readonly { key: string; before?: string; after?: string }[]): string[] {
    const inserted = new Set(insertions.map(({ key }) => key));
    const existing = order.filter((key) => !inserted.has(key));
    const anchors = new Set(existing);
    const preceding = new Map<string, string[]>();
    const following = new Map<string, string[]>();
    const end: string[] = [];
    for (const { key, before, after } of insertions) {
        const anchor = before ?? after;
        if (anchor === undefined || !anchors.has(anchor)) end.push(key);
        else {
            const insertionsByAnchor = before === undefined ? following : preceding;
            const group = insertionsByAnchor.get(anchor);
            if (group) group.push(key);
            else insertionsByAnchor.set(anchor, [key]);
        }
    }
    return existing.flatMap((key) => [...(preceding.get(key) ?? []), key, ...(following.get(key) ?? [])]).concat(end);
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
