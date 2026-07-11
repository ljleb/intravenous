export type WorkspaceNotificationParams = Record<string, unknown>;
export type WorkspaceNotificationHandler<T extends WorkspaceNotificationParams = WorkspaceNotificationParams> =
    (params: T) => void | Promise<void>;

type HandlerSet = Set<WorkspaceNotificationHandler>;

export class WorkspaceNotificationRouter {
    private readonly handlers = new Map<string, HandlerSet>();

    subscribe<T extends WorkspaceNotificationParams>(
        method: string,
        handler: WorkspaceNotificationHandler<T>,
    ): { dispose(): void } {
        const handlers = this.handlers.get(method) || new Set<WorkspaceNotificationHandler>();
        handlers.add(handler as WorkspaceNotificationHandler);
        this.handlers.set(method, handlers);

        return {
            dispose: () => {
                const current = this.handlers.get(method);
                if (!current) {
                    return;
                }
                current.delete(handler as WorkspaceNotificationHandler);
                if (current.size === 0) {
                    this.handlers.delete(method);
                }
            },
        };
    }

    async dispatch(method: string, params: WorkspaceNotificationParams): Promise<void> {
        const handlers = this.handlers.get(method);
        if (!handlers || handlers.size === 0) {
            return;
        }

        for (const handler of handlers) {
            await handler(params);
        }
    }
}
