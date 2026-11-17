// Presentation-only motion in linear amplitude. Changing the display scale
// does not reset a held peak or change the meaning of the captured data.
export type MeterMotion = { peak: number };
export class MeterBallistics {
    private peak = 0;
    private targetPeak = 0;
    private velocity = 0;
    private acceleration = 0;
    private holdUntil = 0;
    private time = 0;
    private initialized = false;
    constructor(private readonly holdMilliseconds = 120,
        private readonly relativeAcceleration = 4) {}
    reset(peak: number, now: number): MeterMotion {
        this.initialized = true;
        this.peak = this.targetPeak = peak;
        this.velocity = 0;
        this.acceleration = peak * this.relativeAcceleration;
        this.holdUntil = now + this.holdMilliseconds;
        this.time = now;
        return this.read();
    }
    update(peak: number, now: number): MeterMotion {
        if (!this.initialized) return this.reset(peak, now);
        this.advance(now);
        this.targetPeak = peak;
        if (peak >= this.peak) {
            this.peak = peak;
            this.velocity = 0;
            this.acceleration = peak * this.relativeAcceleration;
            this.holdUntil = now + this.holdMilliseconds;
        }
        return this.read();
    }
    advance(now: number): MeterMotion {
        const fallSeconds = Math.max(0, now - Math.max(this.time, this.holdUntil)) / 1000;
        if (this.peak > this.targetPeak) {
            this.peak = Math.max(this.targetPeak, this.peak - this.velocity * fallSeconds
                - .5 * this.acceleration * fallSeconds * fallSeconds);
            this.velocity += this.acceleration * fallSeconds;
            if (this.peak === this.targetPeak) {
                this.velocity = 0;
                this.acceleration = this.peak * this.relativeAcceleration;
            }
        }
        this.time = Math.max(this.time, now);
        return this.read();
    }
    private read(): MeterMotion { return { peak: this.peak }; }
    get moving(): boolean {
        return this.peak > this.targetPeak;
    }
}
