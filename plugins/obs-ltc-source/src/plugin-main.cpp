#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-ltc-source", "en-US")

extern struct obs_source_info ltc_source_info;

bool obs_module_load(void)
{
	obs_register_source(&ltc_source_info);
	blog(LOG_INFO, "[obs-ltc-source] plugin loaded");
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[obs-ltc-source] plugin unloaded");
}
