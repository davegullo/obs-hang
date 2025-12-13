/*
Hang MoQ Source for OBS
Copyright (C) 2024 OBS Plugin Template

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
#include <util/threading.h>
#include <util/platform.h>
#include <media-io/video-io.h>
#include <media-io/audio-io.h>

// Include the moq library header
#include <moq.h>

// Include our headers
#include "hang-source.h"
#include "vaapi-decoder.h"
#include "audio-decoder.h"

static const char *hang_source_get_name(void *type_data);
static void *hang_source_create(obs_data_t *settings, obs_source_t *source);
static void hang_source_destroy(void *data);
static void hang_source_update(void *data, obs_data_t *settings);
static void hang_source_activate(void *data);
static void hang_source_deactivate(void *data);
static void hang_source_video_render(void *data, gs_effect_t *effect);
static uint32_t hang_source_get_width(void *data);
static uint32_t hang_source_get_height(void *data);
static obs_properties_t *hang_source_get_properties(void *data);
static void hang_source_get_defaults(obs_data_t *settings);

// FFmpeg audio decoder functions (declared in audio-decoder.h)

// MoQ callback functions
static void on_catalog(void *user_data, const char *catalog_json);
static void on_video(void *user_data, int32_t track, const uint8_t *data, uintptr_t size, uint64_t pts, bool keyframe);
static void on_audio(void *user_data, int32_t track, const uint8_t *data, uintptr_t size, uint64_t pts);
static void on_error(void *user_data, int32_t code);


struct obs_source_info hang_source_info = {
	.id = "hang_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
	.get_name = hang_source_get_name,
	.create = hang_source_create,
	.destroy = hang_source_destroy,
	.update = hang_source_update,
	.activate = hang_source_activate,
	.deactivate = hang_source_deactivate,
	.video_render = hang_source_video_render,
	.get_width = hang_source_get_width,
	.get_height = hang_source_get_height,
	.get_properties = hang_source_get_properties,
	.get_defaults = hang_source_get_defaults,
	.icon_type = OBS_ICON_TYPE_MEDIA,
};

static const char *hang_source_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("HangSource");
}

static void *hang_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct hang_source *context = bzalloc(sizeof(struct hang_source));
	context->source = source;

	// Initialize threading primitives
	pthread_mutex_init(&context->frame_mutex, NULL);
	pthread_cond_init(&context->frame_cond, NULL);
	pthread_mutex_init(&context->audio_mutex, NULL);
	pthread_cond_init(&context->audio_cond, NULL);

	// Initialize frame storage
	context->current_frame_data = NULL;
	context->current_frame_size = 0;
	context->current_frame_width = 0;
	context->current_frame_height = 0;

	// Initialize queues
	context->frame_queue_cap = 16;
	context->frame_queue = bzalloc(sizeof(struct obs_source_frame *) * context->frame_queue_cap);
	context->audio_queue_cap = 16;
	context->audio_queue = bzalloc(sizeof(struct obs_source_audio *) * context->audio_queue_cap);

	hang_source_update(context, settings);
	return context;
}

static void hang_source_destroy(void *data)
{
	struct hang_source *context = data;

	// Stop the source first
	hang_source_deactivate(context);

	// Clean up MoQ resources
	if (context->subscription_id > 0) {
		moq_subscribe_close(context->subscription_id);
	}
	if (context->session_id > 0) {
		moq_session_close(context->session_id);
	}

	// Clean up decoders
	vaapi_decoder_destroy(context);
	audio_decoder_destroy(context);

	// Clean up video resources
	if (context->texture) {
		gs_texture_destroy(context->texture);
	}

	// Clean up frame data
	if (context->current_frame_data) {
		bfree(context->current_frame_data);
	}

	// Clean up queues
	for (size_t i = 0; i < context->frame_queue_len; i++) {
		obs_source_frame_free(context->frame_queue[i]);
	}
	for (size_t i = 0; i < context->audio_queue_len; i++) {
		// Free the audio data channels
		struct obs_source_audio *audio = context->audio_queue[i];
		for (int ch = 0; ch < MAX_AV_PLANES && audio->data[ch]; ch++) {
			bfree((void *)audio->data[ch]);
		}
		bfree(audio);
	}

	bfree(context->frame_queue);
	bfree(context->audio_queue);

	// Clean up threading primitives
	pthread_mutex_destroy(&context->frame_mutex);
	pthread_cond_destroy(&context->frame_cond);
	pthread_mutex_destroy(&context->audio_mutex);
	pthread_cond_destroy(&context->audio_cond);

	// Clean up strings
	bfree(context->url);
	bfree(context->broadcast_path);

	bfree(context);
}

static void hang_source_update(void *data, obs_data_t *settings)
{
	struct hang_source *context = data;

	const char *url = obs_data_get_string(settings, "url");
	const char *broadcast_path = obs_data_get_string(settings, "broadcast");

	// Check if settings changed
	bool url_changed = !context->url || strcmp(context->url, url) != 0;
	bool broadcast_changed = !context->broadcast_path || strcmp(context->broadcast_path, broadcast_path) != 0;

	if (!url_changed && !broadcast_changed) {
		return;
	}

	// Stop current connection
	hang_source_deactivate(context);

	// Update settings
	bfree(context->url);
	bfree(context->broadcast_path);
	context->url = bstrdup(url);
	context->broadcast_path = bstrdup(broadcast_path);

	// Reconnect if we have valid settings
	if (url_changed || broadcast_changed) {
		if (context->url && context->broadcast_path && strlen(context->url) > 0 && strlen(context->broadcast_path) > 0) {
			hang_source_activate(context);
		}
	}
}

static void hang_source_activate(void *data)
{
	struct hang_source *context = data;

	if (context->active || !context->url || !context->broadcast_path ||
	    strlen(context->url) == 0 || strlen(context->broadcast_path) == 0) {
		return;
	}

	// Basic URL validation - ensure URL has at least scheme://host
	if (strstr(context->url, "://") == NULL) {
		obs_log(LOG_ERROR, "Invalid URL format: %s (must include scheme like https://)", context->url);
		return;
	}

	const char *host_start = strstr(context->url, "://");
	if (host_start && strlen(host_start + 3) == 0) {
		obs_log(LOG_ERROR, "Invalid URL: %s (missing host)", context->url);
		return;
	}

	obs_log(LOG_INFO, "Activating hang source with URL: %s, broadcast: %s", context->url, context->broadcast_path);

	// Create MoQ session
	context->session_id = moq_session_connect(context->url, NULL, NULL);
	if (context->session_id <= 0) {
		obs_log(LOG_ERROR, "Failed to create MoQ session");
		return;
	}

	// Initialize decoders
	if (!vaapi_decoder_init(context)) {
		obs_log(LOG_ERROR, "Failed to initialize VA-API decoder");
		goto cleanup;
	}

	if (!audio_decoder_init(context)) {
		obs_log(LOG_ERROR, "Failed to initialize audio decoder");
		goto cleanup;
	}

	// Create subscription
	context->subscription_id = moq_subscribe_create(
		context->session_id,
		context->broadcast_path,
		on_catalog,
		on_video,
		on_audio,
		on_error,
		context
	);

	if (context->subscription_id <= 0) {
		obs_log(LOG_ERROR, "Failed to create MoQ subscription");
		goto cleanup;
	}

	context->active = true;
	obs_log(LOG_INFO, "Hang source activated successfully");
	return;

cleanup:
	if (context->subscription_id > 0) {
		moq_subscribe_close(context->subscription_id);
		context->subscription_id = 0;
	}
	if (context->session_id > 0) {
		moq_session_close(context->session_id);
		context->session_id = 0;
	}
	vaapi_decoder_destroy(context);
	audio_decoder_destroy(context);
}

static void hang_source_deactivate(void *data)
{
	struct hang_source *context = data;

	if (!context->active) {
		return;
	}

	obs_log(LOG_INFO, "Deactivating hang source");

	context->active = false;

	// Close subscription and session
	if (context->subscription_id > 0) {
		moq_subscribe_close(context->subscription_id);
		context->subscription_id = 0;
	}
	if (context->session_id > 0) {
		moq_session_close(context->session_id);
		context->session_id = 0;
	}

	// Clean up decoders
	vaapi_decoder_destroy(context);
	audio_decoder_destroy(context);

	// Clear current frame
	pthread_mutex_lock(&context->frame_mutex);
	if (context->current_frame_data) {
		bfree(context->current_frame_data);
		context->current_frame_data = NULL;
		context->current_frame_size = 0;
		context->current_frame_width = 0;
		context->current_frame_height = 0;
	}

	// Clear queues
	for (size_t i = 0; i < context->frame_queue_len; i++) {
		obs_source_frame_free(context->frame_queue[i]);
	}
	context->frame_queue_len = 0;
	pthread_mutex_unlock(&context->frame_mutex);

	pthread_mutex_lock(&context->audio_mutex);
	for (size_t i = 0; i < context->audio_queue_len; i++) {
		// Free the audio data channels
		struct obs_source_audio *audio = context->audio_queue[i];
		for (int ch = 0; ch < MAX_AV_PLANES && audio->data[ch]; ch++) {
			bfree((void *)audio->data[ch]);
		}
		bfree(audio);
	}
	context->audio_queue_len = 0;
	pthread_mutex_unlock(&context->audio_mutex);

	obs_log(LOG_INFO, "Hang source deactivated");
}

static obs_properties_t *hang_source_get_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "url", obs_module_text("URL"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "broadcast", obs_module_text("Broadcast"), OBS_TEXT_DEFAULT);

	return props;
}

static void hang_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "url", "");
	obs_data_set_default_string(settings, "broadcast", "");
}

static void hang_source_video_render(void *data, gs_effect_t *effect)
{
	struct hang_source *context = data;

	if (!context->active) {
		return;
	}

	// Get the current frame data
	pthread_mutex_lock(&context->frame_mutex);
	if (context->current_frame_data && context->current_frame_width > 0 && context->current_frame_height > 0) {
		uint32_t width = context->current_frame_width;
		uint32_t height = context->current_frame_height;

		// Create or update texture if needed
		if (!context->texture || context->width != width || context->height != height) {
			if (context->texture) {
				gs_texture_destroy(context->texture);
			}
			context->texture = gs_texture_create(width, height, GS_RGBA, 1, NULL, GS_DYNAMIC);
			context->width = width;
			context->height = height;
		}

		if (context->texture) {
			// Validate RGBA data (check if it's not all black)
			bool has_data = false;
			for (size_t i = 0; i < context->current_frame_size && i < 10000; i += 4) {
				if (context->current_frame_data[i] > 0 || context->current_frame_data[i+1] > 0 ||
				    context->current_frame_data[i+2] > 0) {
					has_data = true;
					break;
				}
			}
			obs_log(LOG_DEBUG, "RGBA data validation: has_data=%d, size=%zu", has_data, context->current_frame_size);

			// Upload frame data to texture
			gs_texture_set_image(context->texture, context->current_frame_data, width * 4, false);
			obs_log(LOG_DEBUG, "Texture upload completed for %dx%d", width, height);

			// Render the texture
			gs_eparam_t *param = gs_effect_get_param_by_name(effect, "image");
			if (param) {
				gs_effect_set_texture(param, context->texture);
				gs_draw_sprite(context->texture, 0, width, height);
				obs_log(LOG_DEBUG, "Sprite drawn successfully");
			} else {
				obs_log(LOG_ERROR, "Effect parameter 'image' not found");
			}
		} else {
			obs_log(LOG_ERROR, "No texture available for rendering");
		}
	}
	pthread_mutex_unlock(&context->frame_mutex);
}

static uint32_t hang_source_get_width(void *data)
{
	struct hang_source *context = data;
	return context->current_frame_width > 0 ? context->current_frame_width : 1920;
}

static uint32_t hang_source_get_height(void *data)
{
	struct hang_source *context = data;
	return context->current_frame_height > 0 ? context->current_frame_height : 1080;
}

// MoQ callback implementations
static void on_catalog(void *user_data, const char *catalog_json)
{
	UNUSED_PARAMETER(user_data);
	obs_log(LOG_INFO, "Received catalog: %s", catalog_json);

	// TODO: Parse catalog JSON to configure decoders
	// For now, assume default settings
}

static void on_video(void *user_data, int32_t track, const uint8_t *data, uintptr_t size, uint64_t pts, bool keyframe)
{
	struct hang_source *context = user_data;

	if (!context->active) {
		return;
	}

	obs_log(LOG_DEBUG, "Received video frame: track=%d, size=%zu, pts=%llu, keyframe=%d",
		track, size, pts, keyframe);

	// Decode video frame using VA-API
	if (vaapi_decoder_decode(context, data, size, pts, keyframe)) {
		// Frame was decoded and queued
	}
}

static void on_audio(void *user_data, int32_t track, const uint8_t *data, uintptr_t size, uint64_t pts)
{
	struct hang_source *context = user_data;

	if (!context->active) {
		return;
	}

	obs_log(LOG_DEBUG, "Received audio frame: track=%d, size=%zu, pts=%llu", track, size, pts);

	// Decode audio frame using FFmpeg
	if (audio_decoder_decode(context, data, size, pts)) {
		// Audio was decoded and queued
	}
}

static void on_error(void *user_data, int32_t code)
{
	UNUSED_PARAMETER(user_data);
	obs_log(LOG_ERROR, "MoQ error: %d", code);

	// TODO: Handle reconnection logic
}

