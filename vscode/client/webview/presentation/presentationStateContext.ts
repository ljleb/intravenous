import { inject, provide, ref, type InjectionKey, type Ref } from "vue";
import type { SerializedPresentationState } from "../../src/presentationStateJson";

type PresentationStateCallbacks = {
    serialize(): SerializedPresentationState;
    restore(state: SerializedPresentationState): void;
};
type PresentationStateRegistration = { ready: Readonly<Ref<boolean>>; save: (saveNow?: boolean) => void };
type RegisterPresentationState = (callbacks: PresentationStateCallbacks) => PresentationStateRegistration;
const key: InjectionKey<RegisterPresentationState> = Symbol("presentationState");

export function providePresentationState(register: RegisterPresentationState) { provide(key, register); }

export function usePresentationState(callbacks: PresentationStateCallbacks): PresentationStateRegistration {
    const register = inject(key, null);
    if (register) return register(callbacks);
    callbacks.restore(null);
    return { ready: ref(true), save: () => {} };
}
