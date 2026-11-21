import { inject, type InjectionKey, type Ref } from "vue";
import type { BeatGridEditing } from "../../src/beatGridEditing";

export const beatGridEditingKey: InjectionKey<{ edits: BeatGridEditing; version: Ref<number> }> = Symbol("iv.beatGridEditing");

export function useBeatGridEditing() {
    const context = inject(beatGridEditingKey, null);
    if (!context) throw new Error("Beats presentation requires its webview editing owner");
    return context;
}
