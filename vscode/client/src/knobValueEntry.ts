import { circularKnobRange, normalizeKnobValue, type KnobLimits } from "./knobScrub";

export type KnobValueRange = Pick<KnobLimits, "min" | "max" | "integer" | "geometry">;
export type KnobValueEntry = {
    id: string;
    label: string;
    draft: string;
    error: string;
    anchor: { left: number; top: number; bottom: number };
};

export function parseKnobValue(draft: string, range: KnobValueRange): number | string {
    const value = Number(draft);
    const circular = circularKnobRange(range);
    if (!draft.trim() || !Number.isFinite(value) ||
        (!circular && (value < (range.min ?? -Infinity) || value > (range.max ?? Infinity))))
        return "Enter a finite value within the input limits.";
    if (range.integer && !Number.isInteger(value)) return "Enter a whole number.";
    return circular ? normalizeKnobValue(value, range) : value;
}

export class KnobValueEditing<Owner> {
    private active: { view: KnobValueEntry; owner?: Owner; range: () => KnobValueRange; apply: (value: number) => void } | null = null;
    constructor(private readonly newId: () => string, private readonly changed: () => void) {}
    open(label: string, value: number, anchor: KnobValueEntry["anchor"], range: () => KnobValueRange,
        apply: (value: number) => void, owner?: Owner): void {
        this.active = { owner, range, apply, view: { id: this.newId(), label, draft: String(value), error: "", anchor } };
        this.changed();
    }
    view(): KnobValueEntry | null { return this.active ? { ...this.active.view } : null; }
    owner(): Owner | undefined { return this.active?.owner; }
    setDraft(draft: string): void {
        if (!this.active) return;
        this.active.view.draft = draft; this.active.view.error = ""; this.changed();
    }
    accept(): boolean {
        const edit = this.active;
        if (!edit) return false;
        const value = parseKnobValue(edit.view.draft, edit.range());
        if (typeof value === "string") { edit.view.error = value; this.changed(); return false; }
        this.active = null;
        edit.apply(value);
        this.changed();
        return true;
    }
    cancel(): void {
        if (!this.active) return;
        this.active = null; this.changed();
    }
}
