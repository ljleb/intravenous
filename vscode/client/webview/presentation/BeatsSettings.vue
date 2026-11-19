<script setup lang="ts">
import { computed } from "vue";
import { beatBpm, defaultBeatBpm, maximumBeatBpm, maximumBeatsPerBar,
    maximumBeatSubdivisionCount, minimumBeatBpm, readBeatGrid, type BeatGrid } from "../../src/beatGrid";
import { addressKey, type NodeAddress } from "../../src/graphScope";
import { useTimelineWindow } from "../timeline/timelineContext";
import { useBeatGridEditing } from "./beatGridEditingContext";
import Knob from "./Knob.vue";

const props = defineProps<{ target: NodeAddress; grid: BeatGrid }>();
const emit = defineEmits<{ done: [] }>();
const time = useTimelineWindow();
const { edits, version } = useBeatGridEditing();
const preview = computed(() => {
    void version.value;
    return edits.view(props.target) ?? props.grid;
});
const editError = computed(() => {
    void version.value;
    return edits.error(props.target);
});
type SettingKey = "bpm" | "beatsPerBar" | number;
type Setting = {
    key: SettingKey; label: string; value: number; min: number; max: number; defaultValue: number; integer?: boolean;
};
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

function updateGrid(bpm: number, beatsPerBar: number, subdivisions: readonly number[], continuous = false) {
    const next = readBeatGrid([bpm, beatsPerBar, subdivisions]);
    if (next) edits.update(props.target, next, continuous);
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
    void edits.finish(props.target);
}
</script>

<template>
    <div v-for="setting in settings" :key="setting.key" class="beat-setting">
        <span>{{ setting.label }}</span>
        <span class="beat-setting-value">{{ setting.value }}</span>
        <Knob :identity="`${addressKey(target)}:beats:${setting.key}`" :label="setting.label" :value="setting.value"
            :disabled="setting.max < setting.min" :limits="{ ...setting, wholeByDefault: true, scrubSpeed: .5 }"
            :face-limits="{ min: setting.min, defaultValue: setting.defaultValue }"
            :apply="(value, live) => setValue(setting.key, value, live)" :end-edit="finishEdit"
            :edit-active="() => !!edits.view(target) && !editError" />
        <button v-if="typeof setting.key === 'number'" type="button" tabindex="-1" :aria-label="`Remove ${setting.label.toLowerCase()}`"
            @click="removeSubdivision(setting.key)">×</button>
    </div>
    <footer class="beat-settings-actions">
        <button type="button" tabindex="-1" :disabled="divisions * 2 > allowedDivisions" @click="addSubdivision">Add subdivision</button>
        <button type="button" tabindex="-1" @click="emit('done')">Done</button>
    </footer>
    <span v-if="editError" role="alert" class="beat-settings-error">{{ editError }}</span>
</template>

<style scoped>
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
