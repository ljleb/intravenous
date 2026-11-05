export const nodePresentationDisplayModes = [
    "compact",
    "nano",
    "mini",
    "editable",
] as const;

export type NodePresentationDisplayMode = typeof nodePresentationDisplayModes[number];

export type NodePresentationDescriptor = {
    definitionId: string;
    presentationId: string;
    displayName: string;
};

// Frontend presentation providers may extend this interface with their own
// renderer/component data. The backend-facing compatibility key is the only
// field that host selection needs to understand.
export type NodePresentationProvider = {
    presentationId: string;
};

export const frameworkDefaultNodePresentationProvider = {
    presentationId: "iv.framework.default",
} as const satisfies NodePresentationProvider;

export type NodePresentationSelection<Provider extends NodePresentationProvider> = {
    descriptor: NodePresentationDescriptor | null;
    provider: Provider;
    usedDefault: boolean;
};

export class NodePresentationRegistry<Provider extends NodePresentationProvider> {
    private readonly providers = new Map<string, Provider>();

    constructor(private readonly defaultProvider: Provider) {
        if (!defaultProvider.presentationId) {
            throw new Error("the default node presentation provider requires an ID");
        }
    }

    register(provider: Provider): void {
        if (!provider.presentationId) {
            throw new Error("node presentation providers require an ID");
        }
        if (provider.presentationId === this.defaultProvider.presentationId
            || this.providers.has(provider.presentationId)) {
            throw new Error(
                `node presentation provider '${provider.presentationId}' is already registered`,
            );
        }
        this.providers.set(provider.presentationId, provider);
    }

    unregister(presentationId: string): void {
        this.providers.delete(presentationId);
    }

    available(
        definitionId: string,
        descriptors: readonly NodePresentationDescriptor[],
    ): NodePresentationDescriptor[] {
        return descriptors
            .filter((descriptor) => descriptor.definitionId === definitionId
                && this.providers.has(descriptor.presentationId))
            .sort((lhs, rhs) => lhs.presentationId < rhs.presentationId
                ? -1
                : lhs.presentationId > rhs.presentationId ? 1 : 0);
    }

    select(
        definitionId: string,
        descriptors: readonly NodePresentationDescriptor[],
        preferredPresentationId?: string,
    ): NodePresentationSelection<Provider> {
        const available = this.available(definitionId, descriptors);
        const descriptor = preferredPresentationId
            ? available.find((candidate) =>
                candidate.presentationId === preferredPresentationId)
            : available[0];
        const selected = descriptor ?? available[0];
        if (!selected) {
            return {
                descriptor: null,
                provider: this.defaultProvider,
                usedDefault: true,
            };
        }
        return {
            descriptor: selected,
            provider: this.providers.get(selected.presentationId)!,
            usedDefault: false,
        };
    }
}
