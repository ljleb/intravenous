// Presentation-only motion in linear amplitude. Changing the display scale
// does not reset a held peak or change the meaning of the captured data.
export type MeterMotion = { peak: number; rms: number };
export class MeterBallistics {
    private peak = 0;
    private rms = 0;
    private targetPeak = 0;
    private targetRms = 0;
    private velocity = 0;
    private acceleration = 0;
    private holdUntil = 0;
    private time = 0;
    private initialized = false;
    constructor(private readonly holdMilliseconds = 120,
        private readonly relativeAcceleration = 4) {}
    reset(peak: number, rms: number, now: number): MeterMotion {
        this.initialized = true;
        this.peak = this.targetPeak = peak;
        this.rms = this.targetRms = rms;
        this.velocity = 0;
        this.acceleration = peak * this.relativeAcceleration;
        this.holdUntil = now + this.holdMilliseconds;
        this.time = now;
        return this.read();
    }
    update(peak: number, rms: number, now: number): MeterMotion {
        if (!this.initialized) return this.reset(peak, rms, now);
        this.advance(now);
        this.targetPeak = peak;
        this.targetRms = rms;
        if (peak >= this.peak) {
            this.peak = peak;
            this.velocity = 0;
            this.acceleration = peak * this.relativeAcceleration;
            this.holdUntil = now + this.holdMilliseconds;
        }
        return this.read();
    }
    advance(now: number): MeterMotion {
        const seconds = Math.max(0, now - this.time) / 1000;
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
        const release = this.targetRms >= this.rms ? .015 : .10;
        this.rms = this.targetRms + (this.rms - this.targetRms) * Math.exp(-seconds / release);
        if (Math.abs(this.rms - this.targetRms) <= this.epsilon(this.targetRms)) this.rms = this.targetRms;
        this.time = Math.max(this.time, now);
        return this.read();
    }
    private epsilon(value: number): number { return Math.max(1e-7, value * 1e-6); }
    private read(): MeterMotion { return { peak: this.peak, rms: this.rms }; }
    get moving(): boolean {
        return this.peak > this.targetPeak
            || Math.abs(this.rms - this.targetRms) > this.epsilon(this.targetRms);
    }
}
