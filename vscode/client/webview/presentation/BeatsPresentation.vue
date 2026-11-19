<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from "vue";
import { beatBpm, beatGridTicks, defaultBeatBpm, maximumBeatBpm, maximumBeatsPerBar,
    maximumBeatSubdivisionCount, minimumBeatBpm, readBeatGrid } from "../../src/beatGrid";
import { addressKey } from "../../src/graphScope";
import { useOptionalGraphEditor } from "../graph/context";
import { useTimelineWindow } from "../timeline/timelineContext";
import type { NodeDisplayMode } from "./displayMode";
import GenericPresentation from "./GenericPresentation.vue";
import Knob from "./Knob.vue";
import LaneSettings from "./LaneSettings.vue";
import NodeLane from "./NodeLane.vue";
import { useNode } from "./nodeContext";
import { useBeatGridEditing } from "./beatGridEditingContext";

const props = defineProps<{ mode: NodeDisplayMode }>();
const node = useNode();
const time = useTimelineWindow();
const editor = useOptionalGraphEditor();
const { edits, version } = useBeatGridEditing();
const surface = ref<HTMLElement | null>(null);
const width = ref(0);
type SettingKey = "bpm" | "beatsPerBar" | number;
type Setting = {
    key: SettingKey; label: string; value: number; min: number; max: number; defaultValue: number; integer?: boolean;
};
const grid = computed(() => {
    const construction = node.value.construction;
    return construction?.definitionId === "iv.builtin.beats" ? readBeatGrid(construction.arguments) : null;
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
const divisions = computed(() => preview.value?.subdivisions.reduce((product, factor) => product * factor, 1) ?? 1);
const allowedDivisions = computed(() => preview.value ? maximumBeatSubdivisionCount(preview.value, time?.value.sampleRate) : 0);
const settings = computed<Setting[]>(() => {
    const value = preview.value;
    if (!value) return [];
    return [
        { key: "bpm", label: "BPM", value: beatBpm(value), min: minimumBeatBpm,
            max: maximumBeatBpm(time?.value.sampleRate, divisions.value), defaultValue: defaultBeatBpm },
        { key: "beatsPerBar", label: "Beats per bar", value: value.beatsPerBar, min: 1,
            max: maximumBeatsPerBar, defaultValue: 4, integer: true },
        ...value.subdivisions.map((factor, index) => ({ key: index, label: `Subdivision ${index + 1}`, value: factor, min: 2,
            max: Math.floor(allowedDivisions.value / (divisions.value / factor)), defaultValue: 2, integer: true })),
    ];
});
const ticks = computed(() => {
    const window = time?.value;
    return preview.value && window?.sampleRate ? beatGridTicks(preview.value, {
        beginSeconds: window.beginSeconds, spanSeconds: window.spanSeconds ?? window.endSeconds - window.beginSeconds,
    }, window.sampleRate, width.value) : [];
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

function updateGrid(bpm: number, beatsPerBar: number, subdivisions: readonly number[], continuous = false) {
    const target = node.value.address;
    const next = readBeatGrid([bpm, beatsPerBar, subdivisions]);
    if (editable.value && target && next) edits.update(target, next, continuous);
}
function setValue(key: SettingKey, value: number, continuous = false) {
    const current = preview.value;
    const setting = settings.value.find((entry) => entry.key === key);
    if (!current || !setting || setting.max < setting.min) return;
    value = Math.max(setting.min, Math.min(setting.max, value));
    const subdivisions = typeof key === "number"
        ? current.subdivisions.map((factor, index) => index === key ? value : factor) : current.subdivisions;
    updateGrid(key === "bpm" ? value : beatBpm(current), key === "beatsPerBar" ? value : current.beatsPerBar, subdivisions, continuous);
}
function removeSubdivision(key: SettingKey) {
    const value = preview.value;
    if (value) updateGrid(beatBpm(value), value.beatsPerBar, value.subdivisions.filter((_, index) => index !== key));
}
function addSubdivision() {
    const value = preview.value;
    if (value && divisions.value * 2 <= allowedDivisions.value)
        updateGrid(beatBpm(value), value.beatsPerBar, [...value.subdivisions, 2]);
}
function finishEdit() {
    if (node.value.address) void edits.finish(node.value.address);
}

let resize: ResizeObserver | undefined;
onMounted(() => {
    if (!surface.value) return;
    resize = new ResizeObserver(([entry]) => { width.value = entry.contentRect.width; });
    resize.observe(surface.value);
});
onBeforeUnmount(() => resize?.disconnect());
</script>

<template>
    <NodeLane v-if="time" :mode="mode">
        <div ref="surface" class="beat-grid" :data-status-hint="status">
            <svg class="beat-grid-marks" role="img" aria-label="Beats grid preview" :aria-description="status">
                <line v-for="tick in ticks" :key="String(tick.sample)" :x1="tick.x" :x2="tick.x"
                    :y1="tick.depth === 0 ? '0%' : tick.depth === 1 ? '40%' : `${100 - 40 / tick.depth}%`" y2="100%"
                    :class="{ bar: tick.depth === 0, beat: tick.depth === 1 }" />
                <text v-for="tick in labels" :key="String(tick.sample)" :x="tick.x" y="100%" dx="4" dy="-4">{{ tick.bar }}</text>
            </svg>
            <LaneSettings v-if="editable && preview && node.address" :label="`${beatBpm(preview)} BPM`"
                dialog-label="Beat grid settings" :hint="status" :width="236" :preferred-height="260" v-slot="{ close }">
                <div v-for="setting in settings" :key="setting.key" class="beat-setting">
                    <span>{{ setting.label }}</span>
                    <span class="beat-setting-value">{{ setting.value }}</span>
                    <Knob :identity="`${addressKey(node.address)}:beats:${setting.key}`" :label="setting.label" :value="setting.value"
                        :disabled="setting.max < setting.min" :limits="{ ...setting, wholeByDefault: true, scrubSpeed: .5 }"
                        :face-limits="{ min: setting.min, defaultValue: setting.defaultValue }"
                        :apply="(value, live) => setValue(setting.key, value, live)" :end-edit="finishEdit"
                        :edit-active="() => !!edits.view(node.address) && !editError" />
                    <button v-if="typeof setting.key === 'number'" type="button" tabindex="-1" :aria-label="`Remove ${setting.label.toLowerCase()}`"
                        @click="removeSubdivision(setting.key)">×</button>
                </div>
                <footer class="beat-settings-actions">
                    <button type="button" tabindex="-1" :disabled="divisions * 2 > allowedDivisions" @click="addSubdivision">Add subdivision</button>
                    <button type="button" tabindex="-1" @click="close">Done</button>
                </footer>
                <span v-if="editError" role="alert" class="beat-settings-error">{{ editError }}</span>
            </LaneSettings>
            <span v-else-if="mode === 'editable'" class="beat-grid-settings">{{ status }}</span>
        </div>
    </NodeLane>
    <GenericPresentation v-else :mode="mode" />
</template>

<style scoped>
.beat-grid { position: relative; width: 100%; height: 100%; overflow: hidden; user-select: none; }
.beat-grid-marks { position: absolute; inset: 0; width: 100%; height: 100%; pointer-events: none; }
.beat-grid-marks line:not(.timeline-playhead) { stroke: var(--iv-port-output); stroke-opacity: .32; }
.beat-grid-marks line.beat { stroke-opacity: .58; }
.beat-grid-marks line.bar { stroke-opacity: .9; }
.beat-grid-marks text { fill: var(--vscode-descriptionForeground); font-size: 10px; font-variant-numeric: tabular-nums; }
.beat-grid-settings {
    position: absolute; top: 2px; right: 4px; max-width: calc(100% - 8px); overflow: hidden; text-overflow: ellipsis;
    border: 1px solid transparent; padding: 1px 4px; border-radius: 3px; white-space: nowrap; font: inherit; font-size: 10px;
    color: var(--vscode-descriptionForeground); background: var(--vscode-editor-background);
}
.beat-setting { display: grid; grid-template-columns: 1fr auto 32px 24px; gap: 6px; align-items: center; }
.beat-setting-value { color: var(--vscode-descriptionForeground); font-variant-numeric: tabular-nums; }
.beat-setting, .beat-settings-actions { --knob-size: 32px; }
.beat-setting > button:not(.knob-control), .beat-settings-actions > button {
    border: 0; border-radius: 3px; padding: 3px 6px; cursor: pointer; font: inherit;
    background: var(--vscode-button-secondaryBackground); color: var(--vscode-button-secondaryForeground);
}
.beat-settings-actions button:disabled { opacity: .4; cursor: default; }
.beat-settings-actions { display: flex; justify-content: flex-end; gap: 6px; }
.beat-settings-error { color: var(--vscode-errorForeground); }
</style>
