export interface LineOutput {
    appendLine(message: string): void;
}

export function formatTimestampedLogEntry(message: string, timestamp: Date): string {
    return `[${timestamp.toISOString()}] ${message}`;
}

export function timestampOutput<T extends LineOutput>(
    output: T,
    now: () => Date = () => new Date(),
): T {
    const appendLine = output.appendLine.bind(output);
    output.appendLine = (message: string) => {
        appendLine(formatTimestampedLogEntry(message, now()));
    };
    return output;
}
