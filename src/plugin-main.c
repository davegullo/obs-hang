/*
Plugin Name
Copyright (C) <Year> <Developer> <Email Address>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>
#include <moq.h>
#include "hang-source.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

const char *obs_module_name(void)
{
	return PLUGIN_NAME;
}

const char *obs_module_description(void)
{
	return "Hang MoQ Source for OBS Studio";
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "Hang MoQ plugin loading (version %s)", PLUGIN_VERSION);

	// Initialize MoQ logging
	int log_result = moq_log_level("info", 4);
	if (log_result != 0) {
		obs_log(LOG_WARNING, "Failed to initialize MoQ logging: %d", log_result);
	}

	// Register the hang source
	obs_register_source(&hang_source_info);
	obs_log(LOG_INFO, "Hang source registered successfully");

	obs_log(LOG_INFO, "Hang MoQ plugin loaded successfully");
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
