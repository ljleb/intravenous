/**
 * A frontend-only lane presentation.  The backend supplies `typeId` and
 * opaque state; the host supplies layout, viewport, transport and messaging.
 * Plugin renderers are deliberately dependency-free functions because they
 * are installed into the lanes webview at creation time.
 */
export type LanePresentationPlugin = {
    typeId: string;
    css?: string;
    render: (context: any) => boolean;
};
