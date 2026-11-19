import { addressKey, type NodeAddress } from "./graphScope";
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
