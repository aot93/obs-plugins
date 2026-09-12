#include "frontend-recording.h"

#include <obs-module.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// obs-frontend-api is a separate shared library that only exists in a full,
// UI-enabled OBS Studio application. The headless libobs-only build used to
// compile this plugin (see the obs-plugin-build-test skill) never builds it,
// so we can't link against it at build time. Instead, resolve the handful of
// symbols we need at runtime against the copy the *host* OBS process has
// already loaded -- this only works when actually running inside a real OBS
// Studio, which is the plugin's only supported runtime environment anyway.

namespace {

typedef bool (*recording_active_fn)(void);
typedef void (*recording_ctrl_fn)(void);

struct FrontendApi {
	recording_active_fn recording_active = nullptr;
	recording_ctrl_fn recording_start = nullptr;
	recording_ctrl_fn recording_stop = nullptr;
	bool resolve_attempted = false;
	bool warned = false;
};

FrontendApi &api()
{
	static FrontendApi instance;
	return instance;
}

void resolve(FrontendApi &a)
{
	if (a.resolve_attempted)
		return;
	a.resolve_attempted = true;

#if defined(_WIN32)
	HMODULE mod = GetModuleHandleA("obs-frontend-api.dll");
	if (!mod)
		mod = GetModuleHandleA("obs-frontend-api");
	if (!mod)
		return;
	a.recording_active = (recording_active_fn)GetProcAddress(mod, "obs_frontend_recording_active");
	a.recording_start = (recording_ctrl_fn)GetProcAddress(mod, "obs_frontend_recording_start");
	a.recording_stop = (recording_ctrl_fn)GetProcAddress(mod, "obs_frontend_recording_stop");
#else
	// RTLD_NOLOAD: only succeeds if the host process already has this
	// library loaded -- we never want to load our own separate copy.
#if defined(__APPLE__)
	void *mod = dlopen("libobs-frontend-api.dylib", RTLD_NOLOAD | RTLD_NOW);
#else
	void *mod = dlopen("libobs-frontend-api.so", RTLD_NOLOAD | RTLD_NOW);
	if (!mod)
		mod = dlopen("libobs-frontend-api.so.0", RTLD_NOLOAD | RTLD_NOW);
#endif
	if (!mod)
		return;
	a.recording_active = (recording_active_fn)dlsym(mod, "obs_frontend_recording_active");
	a.recording_start = (recording_ctrl_fn)dlsym(mod, "obs_frontend_recording_start");
	a.recording_stop = (recording_ctrl_fn)dlsym(mod, "obs_frontend_recording_stop");
#endif
}

bool ensure_available(FrontendApi &a)
{
	resolve(a);
	if (a.recording_active && a.recording_start && a.recording_stop)
		return true;
	if (!a.warned) {
		a.warned = true;
		blog(LOG_WARNING, "[obs-ltc-source] obs-frontend-api not found in this process -- "
				   "auto start/stop recording is unavailable (requires running inside "
				   "a full OBS Studio application)");
	}
	return false;
}

struct QueuedCall {
	recording_ctrl_fn fn;
};

void run_queued_call(void *param)
{
	QueuedCall *call = (QueuedCall *)param;
	if (call->fn)
		call->fn();
	delete call;
}

} // namespace

bool ltc_frontend_recording_active()
{
	FrontendApi &a = api();
	if (!ensure_available(a))
		return false;
	return a.recording_active();
}

void ltc_frontend_recording_start()
{
	FrontendApi &a = api();
	if (!ensure_available(a))
		return;
	blog(LOG_INFO, "[obs-ltc-source] timecode running -- starting recording");
	obs_queue_task(OBS_TASK_UI, run_queued_call, new QueuedCall{a.recording_start}, false);
}

void ltc_frontend_recording_stop()
{
	FrontendApi &a = api();
	if (!ensure_available(a))
		return;
	blog(LOG_INFO, "[obs-ltc-source] timecode stopped -- stopping recording");
	obs_queue_task(OBS_TASK_UI, run_queued_call, new QueuedCall{a.recording_stop}, false);
}
