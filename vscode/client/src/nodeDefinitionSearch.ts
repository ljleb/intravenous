export type NodeDefinitionPreviewPort = { name: string; event: boolean; output: boolean };

export type NodeDefinitionPreview = {
    id: string;
    displayName: string;
    package: string;
    tags: string[];
    ports: NodeDefinitionPreviewPort[];
    requiresConfiguration: boolean;
};

export type NodeDefinitionCatalog = { generation: string; definitions: NodeDefinitionPreview[] };
export type NodeDefinitionSearch = {
    generation: string;
    matches?: string[];
    error?: string;
    completion: {
        start: number;
        end: number;
        candidates: { label: string; insertText: string }[];
        diagnostics: { message: string; start: number; end: number }[];
    };
};

export function rankNodeDefinitions(definitions: readonly NodeDefinitionPreview[], source: string,
    matches: readonly string[] | undefined, usage: Readonly<Record<string, number>>): NodeDefinitionPreview[] {
    const text = source.trim().toLowerCase();
    const words = text.split(/\s+/).filter(Boolean);
    const matchingIds = matches === undefined ? null : new Set(matches);
    const candidates: { definition: NodeDefinitionPreview; relevance: number }[] = [];
    for (const definition of definitions) {
        if (matchingIds) {
            if (matchingIds.has(definition.id)) candidates.push({ definition, relevance: 0 });
            continue;
        }
        const name = definition.displayName.toLowerCase();
        const id = definition.id.toLowerCase();
        const terms = [...name.split(/\s+/), ...id.split("."), ...definition.tags];
        const scores = words.map((word) => name === word || id === word ? 4
            : terms.some((term) => term === word) ? 3
            : terms.some((term) => term.startsWith(word)) ? 2
            : name.includes(word) || id.includes(word) ? 1 : 0);
        if (scores.every((score) => score > 0)) {
            const exact = name === words.join(" ") || id === text;
            const relevance = exact ? 4 * words.length + 1 : scores.reduce<number>((sum, score) => sum + score, 0);
            candidates.push({ definition, relevance });
        }
    }
    return candidates.sort((a, b) => b.relevance - a.relevance
        || (Object.hasOwn(usage, b.definition.id) ? usage[b.definition.id] : 0)
            - (Object.hasOwn(usage, a.definition.id) ? usage[a.definition.id] : 0)
        || a.definition.displayName.localeCompare(b.definition.displayName)
        || a.definition.id.localeCompare(b.definition.id)).map(({ definition }) => definition);
}

const utf8 = new TextEncoder();
export function queryByteOffset(source: string, caret: number): number {
    return utf8.encode(source.slice(0, caret)).length;
}

function queryCaret(source: string, offset: number): number {
    let caret = 0;
    for (const character of source) {
        if (offset <= 0) break;
        offset -= utf8.encode(character).length;
        caret += character.length;
    }
    return caret;
}

export function applyTagCompletion(source: string, start: number, end: number, text: string) {
    const first = queryCaret(source, start);
    const last = queryCaret(source, end);
    return { source: source.slice(0, first) + text + source.slice(last), caret: first + text.length };
}
