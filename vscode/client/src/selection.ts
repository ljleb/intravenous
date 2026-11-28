export function updateSelection<T>(previous: ReadonlySet<T>, items: Iterable<T>,
    modifiers: { ctrlKey?: boolean; metaKey?: boolean; shiftKey?: boolean } = {}): Set<T> {
    const toggle = modifiers.ctrlKey || modifiers.metaKey;
    const next = new Set(toggle || modifiers.shiftKey ? previous : []);
    for (const item of new Set(items)) {
        if (toggle && next.has(item)) next.delete(item);
        else next.add(item);
    }
    return next;
}
