# Aligned automation bound preview windows

This records the earlier fine-window policy from the Vue client implementation
stages on 2026-11-19.
[The current automation bound preview policy](../vue_client_implementation_stages.md#automation-bound-preview-during-panning)
replaces it.

`AutomationCurvePresentation.vue` requests one aligned fine waveform window
and six coarse windows for each connected bound. The outer two are sparser.
At the widest point budget, the fine window requests 512 positions and the
coarse windows request 128 in total. The viewport can move within the aligned
window without changing the request.
