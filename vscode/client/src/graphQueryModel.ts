import { LogicalNode, SourcePosition, SourceRange, SourceSpan } from "./graphModel";

export type QueryShape = {
    filePath: string;
    ranges: SourceRange[];
};

function positionKey(position: SourcePosition): number {
    return (position.line * 1000000) + position.column;
}

export function collectPrimarySourceSpans(nodes: LogicalNode[]): SourceSpan[] {
    const spans: SourceSpan[] = [];
    const seen = new Set<string>();

    for (const node of nodes) {
        if (!node || !Array.isArray(node.sourceSpans)) {
            continue;
        }
        for (const span of node.sourceSpans) {
            if (!span || !span.filePath || !span.start || !span.end) {
                continue;
            }
            const key = [
                span.filePath,
                span.start.line,
                span.start.column,
                span.end.line,
                span.end.column,
            ].join(":");
            if (seen.has(key)) {
                continue;
            }
            seen.add(key);
            spans.push(span);
        }
    }

    return spans;
}

export function sortNodesByRelevance(nodes: LogicalNode[], query: QueryShape | null): LogicalNode[] {
    if (!query) {
        return nodes;
    }

    const scoreNode = (node: LogicalNode): [number, number, number, number] => {
        let best: [number, number, number, number] | null = null;
        for (const span of node.sourceSpans || []) {
            if (span.filePath !== query.filePath) {
                continue;
            }
            const spanStart = positionKey(span.start);
            const spanEnd = positionKey(span.end);
            const spanLength = Math.max(spanEnd - spanStart, 0);

            for (let rangeIndex = 0; rangeIndex < query.ranges.length; ++rangeIndex) {
                const range = query.ranges[rangeIndex];
                const rangeStart = positionKey(range.start);
                const rangeEnd = positionKey(range.end);
                const boundaryDistance = Math.abs(spanStart - rangeStart) + Math.abs(spanEnd - rangeEnd);
                const score: [number, number, number, number] =
                    [rangeIndex, boundaryDistance, spanLength, spanStart];
                if (
                    !best ||
                    score[0] < best[0] ||
                    (score[0] === best[0] && (
                        score[1] < best[1] ||
                        (score[1] === best[1] && (
                            score[2] < best[2] ||
                            (score[2] === best[2] && score[3] < best[3])
                        ))
                    ))
                ) {
                    best = score;
                }
            }
        }

        return best || [
            Number.MAX_SAFE_INTEGER,
            Number.MAX_SAFE_INTEGER,
            Number.MAX_SAFE_INTEGER,
            Number.MAX_SAFE_INTEGER,
        ];
    };

    return [...nodes].sort((left, right) => {
        const a = scoreNode(left);
        const b = scoreNode(right);
        if (a[0] !== b[0]) {
            return a[0] - b[0];
        }
        if (a[1] !== b[1]) {
            return a[1] - b[1];
        }
        if (a[2] !== b[2]) {
            return a[2] - b[2];
        }
        if (a[3] !== b[3]) {
            return a[3] - b[3];
        }
        return String(left.id).localeCompare(String(right.id));
    });
}
