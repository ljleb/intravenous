// Frontend-owned presentation registry. Specialized presentations register by
// stable leaf or module definition ID; the backend never names them. One
// presentation may support many definition IDs, and one definition may have
// several candidates. The generic presentation is always available.
//
// Every presentation component receives the `mode` prop (a NodeDisplayMode)
// and reads its node through useNode().

export type PresentationRegistration<Component> = {
    // Stable frontend component identity, also the key of a stored
    // preference.
    presentationId: string;
    displayName: string;
    definitionIds: readonly string[];
    component: Component;
};

export type GenericPresentationRegistration<Component> = Omit<
    PresentationRegistration<Component>, "definitionIds">;

export type PresentationSelection<Component> = {
    presentationId: string;
    displayName: string;
    component: Component;
    usedGeneric: boolean;
};

export class PresentationRegistry<Component> {
    private readonly specialized = new Map<string, PresentationRegistration<Component>>();

    constructor(private readonly generic: GenericPresentationRegistration<Component>) {
        if (!generic.presentationId) {
            throw new Error("the generic presentation requires a presentation ID");
        }
    }

    register(registration: PresentationRegistration<Component>): void {
        if (!registration.presentationId) {
            throw new Error("a presentation requires a presentation ID");
        }
        if (registration.presentationId === this.generic.presentationId
            || this.specialized.has(registration.presentationId)) {
            throw new Error(
                `presentation '${registration.presentationId}' is already registered`);
        }
        if (registration.definitionIds.length === 0
            || registration.definitionIds.some((id) => id.length === 0)) {
            throw new Error(
                `presentation '${registration.presentationId}' must name definition IDs`);
        }
        this.specialized.set(registration.presentationId, {
            ...registration,
            definitionIds: [...registration.definitionIds],
        });
    }

    // Specialized presentations of one definition, ordered by presentation
    // ID.
    candidates(definitionId: string): PresentationRegistration<Component>[] {
        return [...this.specialized.values()]
            .filter((registration) => registration.definitionIds.includes(definitionId))
            .sort((lhs, rhs) => lhs.presentationId < rhs.presentationId
                ? -1
                : lhs.presentationId > rhs.presentationId ? 1 : 0);
    }

    // Selects the preferred presentation when it is available for the
    // definition, otherwise the first specialized candidate, otherwise the
    // generic presentation. A preference may name the generic presentation.
    select(
        definitionId: string,
        preferredPresentationId?: string | null,
    ): PresentationSelection<Component> {
        if (preferredPresentationId === this.generic.presentationId) {
            return this.genericSelection();
        }
        const candidates = definitionId ? this.candidates(definitionId) : [];
        const selected = candidates.find(
            (candidate) => candidate.presentationId === preferredPresentationId)
            ?? candidates[0];
        if (!selected) {
            return this.genericSelection();
        }
        return {
            presentationId: selected.presentationId,
            displayName: selected.displayName,
            component: selected.component,
            usedGeneric: false,
        };
    }

    private genericSelection(): PresentationSelection<Component> {
        return {
            presentationId: this.generic.presentationId,
            displayName: this.generic.displayName,
            component: this.generic.component,
            usedGeneric: true,
        };
    }
}
