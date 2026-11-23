# Automation bound previews before playback refresh

This text records the earlier retention and scheduling policy from the Vue
client implementation stages on 2026-11-19.
[Current automation bound preview policy](../vue_client_implementation_stages.md#automation-bound-preview-during-panning)
replaces it.

The presentation retains up to 14 nearby completed windows per bound for the
current source, configured generation, and data version. It clips them to the
visible span and uses the densest ready window for each interval. A complete
fine window can establish a coverage gap where a coarse preview was drawn.
Playback continues to use exact samples. `Observations` shares equivalent
requests, promotes shared prefetches with visible readers, and includes the
current data version in delivery. `GraphExecutor` checks playback and cutover
work first, then serves visible reads ahead of queued prefetch.
