<script setup lang="ts">
import { computed, ref } from "vue";
import { beatBpm, beatGridTicks, readBeatGrid, type BeatGridTick } from "../../src/beatGrid";
import { gridGuideStrengths } from "../../src/gridGuideStrength";
import { useOptionalGraphEditor } from "../graph/context";
import { useTimelineWindow } from "../timeline/timelineContext";
import { usePlayheadSeek } from "../timeline/usePlayheadSeek";
import type { NodeDisplayMode } from "./displayMode";
import GenericPresentation from "./GenericPresentation.vue";
import BeatsSettings from "./BeatsSettings.vue";
import LaneSettings from "./LaneSettings.vue";
import NodeLane from "./NodeLane.vue";
import { useNode } from "./nodeContext";
import { useBeatGridEditing } from "./beatGridEditingContext";
import { usePresentationActive } from "./observationContext";
import GridGuides from "../timeline/GridGuides.vue";
import { usePresentationSize } from "./presentationSize";

const props = defineProps<{ mode: NodeDisplayMode }>();
const node = useNode();
const time = useTimelineWindow();
const editor = useOptionalGraphEditor();
const { edits, version } = useBeatGridEditing();
const surface = ref<HTMLElement | null>(null);
const size = usePresentationSize(surface);
const width = computed(() => size.value?.width ?? 0);
const drawing = usePresentationActive();
const grid = computed(() => {
    const construction = node.value.construction;
    return construction?.definitionId === "builtin.beats" ? readBeatGrid(construction.arguments) : null;
});
const editable = computed(() => props.mode === "editable" && !!editor?.scope.value?.editable
    && !!node.value.address && node.value.loadState === "ready" && !!grid.value);
const preview = computed(() => {
    void version.value;
    return edits.view(node.value.address) ?? grid.value;
});
const editError = computed(() => {
    void version.value;
    return edits.error(node.value.address);
});
const ticks = computed(() => {
    if (!drawing.value) return [];
    const window = time?.value;
    return preview.value && window?.sampleRate ? beatGridTicks(preview.value, {
        beginSeconds: window.beginSeconds, spanSeconds: window.spanSeconds ?? window.endSeconds - window.beginSeconds,
    }, window.sampleRate, width.value) : [];
});
const tickKey = ({ sample }: BeatGridTick) => String(sample);
const tickPosition = ({ x }: BeatGridTick) => x;
const tickStart = ({ depth }: BeatGridTick) => depth === 0 ? "0%" : depth === 1 ? "40%" : `${100 - 40 / depth}%`;
const strengths = computed(() => gridGuideStrengths(ticks.value, tickPosition));
const beginSeek = usePlayheadSeek(surface, () => time?.value, () => ticks.value.map(({ sample }) => sample), () => {
    const grid = preview.value;
    if (!grid) return;
    let cached: { first: bigint; length: bigint; targets: bigint[] } | undefined;
    return { marks({ first, length }, sampleRate, width) {
        if (cached?.first !== first || cached.length !== length) {
            const targets = beatGridTicks(grid, { beginSample: first, spanSeconds: Number(length) / sampleRate }, sampleRate, width)
                .map(({ sample }) => sample);
            cached = { first, length, targets };
        }
        return cached.targets;
    } };
});
const labels = computed(() => {
    if (props.mode !== "editable") return [];
    let nextX = 0;
    return ticks.value.filter(({ bar, x }) => {
        if (bar === undefined || x < nextX || x > width.value - 36) return false;
        nextX = x + Math.max(60, 25 + String(bar).length * 7);
        return true;
    });
});
const status = computed(() => {
    if (editError.value) return editError.value;
    const value = preview.value;
    if (value) return [
        `${beatBpm(value)} BPM`, `${value.beatsPerBar} beats/bar`,
        value.subdivisions.length ? `Subdivision ${value.subdivisions.join(" × ")}` : "No subdivisions",
    ].join(" · ");
    return node.value.errorMessage || node.value.constructionError
        || (node.value.construction ? "Beat grid preview unavailable for these arguments" : "Waiting for accepted Beats arguments");
});

</script>

<template>
    <NodeLane v-if="time" :mode="mode" flush-content>
        <div ref="surface" class="beat-grid" :data-status-hint="status" data-status-shortcuts="Click/drag seek · Space ▶/Ⅱ"
            @pointerdown="beginSeek">
            <svg class="beat-grid-marks" role="img" aria-label="Beats grid preview" :aria-description="status">
                <GridGuides :marks="ticks" :strengths="strengths" :mark-key="tickKey" :position="tickPosition" :start="tickStart" />
                <text v-for="tick in labels" :key="String(tick.sample)" :x="tick.x" y="100%" dx="4" dy="-4">{{ tick.bar }}</text>
            </svg>
            <LaneSettings v-if="editable && preview && node.address" :label="`${beatBpm(preview)} BPM`"
                dialog-label="Beat grid settings" :hint="status" :width="236" :preferred-height="260" v-slot="{ close }">
                <BeatsSettings :target="node.address" :grid="preview" @done="close" />
            </LaneSettings>
            <span v-else-if="mode === 'editable'" class="beat-grid-settings">{{ status }}</span>
        </div>
    </NodeLane>
    <GenericPresentation v-else :mode="mode" />
</template>

<style scoped>
.beat-grid { position: relative; width: 100%; height: 100%; overflow: hidden; user-select: none; touch-action: none; cursor: ew-resize; }
.beat-grid-marks {
    position: absolute; inset: 0; width: 100%; height: 100%; pointer-events: none;
    --iv-grid-color: var(--iv-port-output);
    --iv-grid-max-opacity: .88;
}
.beat-grid-marks text { fill: var(--vscode-descriptionForeground); font-size: 10px; font-variant-numeric: tabular-nums; }
.beat-grid-settings {
    position: absolute; top: 2px; right: 4px; max-width: calc(100% - 8px); overflow: hidden; text-overflow: ellipsis;
    border: 1px solid transparent; padding: 1px 4px; border-radius: 3px; white-space: nowrap; font: inherit; font-size: 10px;
    color: var(--vscode-descriptionForeground); background: var(--vscode-editor-background);
}
</style>
