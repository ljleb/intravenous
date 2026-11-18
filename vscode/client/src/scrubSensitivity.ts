const detentHalfWidth = 12;
type DetentValue = { value: number; plateau: number };

// Knobs and automation points change sensitivity around the current value.
export class ScrubSensitivity {
    private exponent: number;

    constructor(private readonly baseUnitsPerPixel: number, public anchorPosition: number, public anchorValue: number,
        precise = false) {
        this.exponent = precise ? -2 : 0;
    }
    get multiplier(): number { return 2 ** this.exponent; }
    get unitsPerPixel(): number { return this.baseUnitsPerPixel * this.multiplier; }

    valueAt(position: number): number {
        return this.anchorValue + (position - this.anchorPosition) * this.unitsPerPixel;
    }
    valueAtDetent(target: number, travel: number, anchorPlateau: number): DetentValue {
        const scale = this.unitsPerPixel;
        const start = this.anchorValue < target ? (this.anchorValue - target) / scale - detentHalfWidth
            : this.anchorValue > target ? (this.anchorValue - target) / scale + detentHalfWidth : anchorPlateau;
        const end = start + travel;
        return {
            value: end < -detentHalfWidth ? target + (end + detentHalfWidth) * scale
                : end > detentHalfWidth ? target + (end - detentHalfWidth) * scale : target,
            plateau: Math.max(-detentHalfWidth, Math.min(detentHalfWidth, end)),
        };
    }
    valueAtDetents(position: number, targets: readonly number[], anchorPlateau: number): DetentValue {
        let travel = position - this.anchorPosition;
        const direction = travel < 0 ? -1 : 1;
        let result = { value: this.valueAt(position), plateau: 0 };
        // Visit visible detents in travel order. Subtract the physical width
        // of each one passed so the result is independent of event count.
        for (let i = direction > 0 ? 0 : targets.length - 1; i >= 0 && i < targets.length; i += direction) {
            const target = targets[i];
            if (direction * (target - this.anchorValue) < 0) continue;
            result = this.valueAtDetent(target, travel, anchorPlateau);
            if (direction * (result.value - target) <= 0) break;
            travel -= direction * (target === this.anchorValue
                ? detentHalfWidth - direction * anchorPlateau : 2 * detentHalfWidth);
        }
        return result;
    }
    anchorAt(position: number, value: number): void {
        this.anchorPosition = position;
        this.anchorValue = value;
    }
    change(delta: number, position: number, value: number): boolean {
        if (!delta) return false;
        this.anchorAt(position, value);
        this.exponent = Math.max(-10, Math.min(10, this.exponent - Math.sign(delta)));
        return true;
    }
    hint(): string {
        return `${Number(this.multiplier.toPrecision(6))}× · Wheel ↑×2 ↓½`;
    }
}
