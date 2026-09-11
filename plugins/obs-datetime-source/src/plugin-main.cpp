#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-datetime-source", "en-US")

extern struct obs_source_info datetime_source_info;

bool obs_module_load(void)
{
	obs_register_source(&datetime_source_info);
	blog(LOG_INFO, "[obs-datetime-source] plugin loaded");
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[obs-datetime-source] plugin unloaded");
}
