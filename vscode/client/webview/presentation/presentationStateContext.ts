import { inject, provide, type InjectionKey } from "vue";
import type { PresentationState } from "../../src/graphScope";

type PresentationStateCallbacks = {
    serialize(): PresentationState | null;
    restore(state: PresentationState | undefined): void;
};
type RegisterPresentationState = (callbacks: PresentationStateCallbacks) => (saveNow?: boolean) => void;
const key: InjectionKey<RegisterPresentationState> = Symbol("presentationState");

export function providePresentationState(register: RegisterPresentationState) { provide(key, register); }

export function usePresentationState(callbacks: PresentationStateCallbacks): (saveNow?: boolean) => void {
    const register = inject(key, null);
    if (register) return register(callbacks);
    callbacks.restore(undefined);
    return () => {};
}
