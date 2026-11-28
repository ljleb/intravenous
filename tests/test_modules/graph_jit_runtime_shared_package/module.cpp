// One translation unit keeps the fixture registrations and helpers available
// to all scenarios. CTest builds this package once.
#include <intravenous/basic_nodes/resampling.h>

#include "tick_state_and_sample_feedback.inc"
#include "sample_conversion_and_feedback.inc"
#include "latency_and_event_feedback.inc"
#include "event_feedback_and_exports.inc"
#include "batch_execution_and_buffered_storage.inc"
#include "background_regions_and_compensation.inc"
#include "playback_history_and_latency.inc"
#include "retained_latent_windows.inc"
#include "batch_feedback_and_replay.inc"
#include "internal_nodes_and_automation.inc"
#include "click_recording_and_mixed_fan_in.inc"
#include "sample_cutover.inc"
#include "system_audio.inc"
