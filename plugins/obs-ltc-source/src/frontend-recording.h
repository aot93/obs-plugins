#pragma once

// Thin wrapper around obs-frontend-api's recording control, resolved
// dynamically at runtime (see frontend-recording.cpp) rather than linked at
// build time. Safe to call from any thread; the actual start/stop is
// marshaled onto the UI thread internally.

bool ltc_frontend_recording_active();
void ltc_frontend_recording_start();
void ltc_frontend_recording_stop();
