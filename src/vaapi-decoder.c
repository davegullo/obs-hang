/*
VA-API Hardware Video Decoder for OBS Hang Source
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
#include <graphics/graphics.h>
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <fcntl.h>
#include <unistd.h>

// VA-API headers for hardware acceleration
#include <va/va.h>
#include <va/va_drm.h>

#include "hang-source.h"

// Function declarations
static bool vaapi_decode_frame(struct vaapi_decoder *decoder, const uint8_t *data, size_t size, uint64_t pts, struct hang_source *context);
static bool software_decode_frame(struct vaapi_decoder *decoder, const uint8_t *data, size_t size, uint64_t pts, struct hang_source *context);
static void store_decoded_frame(struct hang_source *context, uint8_t *data, size_t size, uint32_t width, uint32_t height);
static bool convert_mp4_nal_units_to_annex_b(const uint8_t *data, size_t size, uint8_t **out_data, size_t *out_size);

struct vaapi_decoder {
	VADisplay va_display;
	VAConfigID va_config;
	VAContextID va_context;
	VABufferID va_buffer;
	VAProfile va_profile;

	// FFmpeg decoder for software fallback if needed
	AVCodecContext *codec_ctx;
	struct SwsContext *sws_ctx;

	// Video format information
	uint32_t width;
	uint32_t height;
	enum AVPixelFormat pix_fmt;
};

static bool vaapi_init_display(struct vaapi_decoder *decoder);
static bool vaapi_create_config(struct vaapi_decoder *decoder);
static bool vaapi_create_context(struct vaapi_decoder *decoder);
static void vaapi_cleanup(struct vaapi_decoder *decoder);
static bool vaapi_decode_frame(struct vaapi_decoder *decoder, const uint8_t *data, size_t size, uint64_t pts, struct hang_source *context);

bool vaapi_decoder_init(struct hang_source *context)
{
	struct vaapi_decoder *decoder = bzalloc(sizeof(struct vaapi_decoder));
	context->vaapi_context = decoder;

	// Try to initialize VA-API hardware acceleration
	if (!vaapi_init_display(decoder)) {
		obs_log(LOG_WARNING, "VA-API display initialization failed, falling back to software decoding");
		goto software_fallback;
	}

	if (!vaapi_create_config(decoder)) {
		obs_log(LOG_WARNING, "VA-API config creation failed, falling back to software decoding");
		vaapi_cleanup(decoder);
		goto software_fallback;
	}

	if (!vaapi_create_context(decoder)) {
		obs_log(LOG_WARNING, "VA-API context creation failed, falling back to software decoding");
		vaapi_cleanup(decoder);
		goto software_fallback;
	}

	obs_log(LOG_INFO, "VA-API decoder initialized successfully");
	return true;

software_fallback:
	// Initialize FFmpeg software decoder as fallback
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec) {
		obs_log(LOG_ERROR, "H.264 codec not found");
		bfree(decoder);
		context->vaapi_context = NULL;
		return false;
	}

	decoder->codec_ctx = avcodec_alloc_context3(codec);
	if (!decoder->codec_ctx) {
		obs_log(LOG_ERROR, "Failed to allocate codec context");
		bfree(decoder);
		context->vaapi_context = NULL;
		return false;
	}

	if (avcodec_open2(decoder->codec_ctx, codec, NULL) < 0) {
		obs_log(LOG_ERROR, "Failed to open codec");
		avcodec_free_context(&decoder->codec_ctx);
		bfree(decoder);
		context->vaapi_context = NULL;
		return false;
	}

	decoder->pix_fmt = AV_PIX_FMT_YUV420P; // Default format
	obs_log(LOG_INFO, "FFmpeg software decoder initialized as fallback");
	return true;
}

void vaapi_decoder_destroy(struct hang_source *context)
{
	struct vaapi_decoder *decoder = context->vaapi_context;
	if (!decoder) {
		return;
	}

	vaapi_cleanup(decoder);

	if (decoder->codec_ctx) {
		avcodec_free_context(&decoder->codec_ctx);
	}

	if (decoder->sws_ctx) {
		sws_freeContext(decoder->sws_ctx);
	}

	bfree(decoder);
	context->vaapi_context = NULL;
}

bool vaapi_decoder_decode(struct hang_source *context, const uint8_t *data, size_t size, uint64_t pts, bool keyframe)
{
	UNUSED_PARAMETER(keyframe);
	struct vaapi_decoder *decoder = context->vaapi_context;
	if (!decoder) {
		return false;
	}

	// Try VA-API first, fallback to FFmpeg if not available
	if (decoder->va_display) {
		obs_log(LOG_DEBUG, "Using VA-API decoder");
		return vaapi_decode_frame(decoder, data, size, pts, context);
	} else if (decoder->codec_ctx) {
		obs_log(LOG_DEBUG, "Using software decoder");
		return software_decode_frame(decoder, data, size, pts, context);
	} else {
		obs_log(LOG_DEBUG, "No decoder available - va_display=%p, codec_ctx=%p", decoder->va_display, decoder->codec_ctx);
		return false;
	}
}

static bool vaapi_init_display(struct vaapi_decoder *decoder)
{
	// Try to get DRM device
	int drm_fd = -1;
	const char *drm_device = "/dev/dri/renderD128"; // Common Intel GPU

	drm_fd = open(drm_device, O_RDWR);
	if (drm_fd < 0) {
		obs_log(LOG_DEBUG, "Failed to open DRM device %s: %s", drm_device, strerror(errno));
		return false;
	}

	decoder->va_display = vaGetDisplayDRM(drm_fd);
	if (!decoder->va_display) {
		close(drm_fd);
		return false;
	}

	int major, minor;
	VAStatus status = vaInitialize(decoder->va_display, &major, &minor);
	if (status != VA_STATUS_SUCCESS) {
		obs_log(LOG_DEBUG, "VA-API initialization failed: %s", vaErrorStr(status));
		vaTerminate(decoder->va_display);
		close(drm_fd);
		return false;
	}

	obs_log(LOG_INFO, "VA-API initialized: version %d.%d", major, minor);
	return true;
}

static bool vaapi_create_config(struct vaapi_decoder *decoder)
{
	VAProfile profiles[] = {
		VAProfileH264High,
		VAProfileH264Main,
		VAProfileHEVCMain,
		VAProfileHEVCMain10,
		VAProfileAV1Profile0,
		VAProfileAV1Profile1,
	};

	for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++) {
		VAStatus status = vaCreateConfig(decoder->va_display, profiles[i], VAEntrypointVLD, NULL, 0, &decoder->va_config);
		if (status == VA_STATUS_SUCCESS) {
			decoder->va_profile = profiles[i];
			obs_log(LOG_INFO, "VA-API config created for profile %d", profiles[i]);
			return true;
		}
	}

	obs_log(LOG_DEBUG, "No supported VA-API profiles found");
	return false;
}

static bool vaapi_create_context(struct vaapi_decoder *decoder)
{
	// Create a context with default resolution, will be updated when we know the actual size
	decoder->width = 1920;
	decoder->height = 1080;

	VASurfaceID surfaces[1]; // We'll allocate surfaces as needed

	VAStatus status = vaCreateContext(decoder->va_display, decoder->va_config, decoder->width, decoder->height, 0, surfaces, 0, &decoder->va_context);
	if (status != VA_STATUS_SUCCESS) {
		obs_log(LOG_DEBUG, "VA-API context creation failed: %s", vaErrorStr(status));
		return false;
	}

	return true;
}

static void vaapi_cleanup(struct vaapi_decoder *decoder)
{
	if (decoder->va_context) {
		vaDestroyContext(decoder->va_display, decoder->va_context);
		decoder->va_context = 0;
	}

	if (decoder->va_config) {
		vaDestroyConfig(decoder->va_display, decoder->va_config);
		decoder->va_config = 0;
	}

	if (decoder->va_display) {
		vaTerminate(decoder->va_display);
		decoder->va_display = NULL;
	}
}

static bool vaapi_decode_frame(struct vaapi_decoder *decoder, const uint8_t *data, size_t size, uint64_t pts, struct hang_source *context)
{
	UNUSED_PARAMETER(decoder);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(pts);
	UNUSED_PARAMETER(context);

	// This is a placeholder VA-API decode implementation
	// In a real implementation, you would:
	// 1. Create VA surfaces for decoded frames
	// 2. Submit compressed data to VA-API
	// 3. Wait for decode completion
	// 4. Map the surface to CPU memory or use VA-API for rendering

	obs_log(LOG_DEBUG, "VA-API decode not implemented");
	return false;
}

static bool convert_mp4_nal_units_to_annex_b(const uint8_t *data, size_t size, uint8_t **out_data, size_t *out_size)
{
	// Estimate output size (add 4 bytes for each start code, remove 4 bytes for each length)
	size_t estimated_size = size + 1024; // Add some padding
	uint8_t *buffer = bzalloc(estimated_size);
	if (!buffer) {
		return false;
	}

	size_t out_pos = 0;
	size_t pos = 0;

	while (pos + 4 <= size) {
		// Read 4-byte length
		uint32_t nal_length = (data[pos] << 24) | (data[pos + 1] << 16) | (data[pos + 2] << 8) | data[pos + 3];
		pos += 4;

		if (pos + nal_length > size) {
			obs_log(LOG_ERROR, "Invalid NAL length: %u (pos=%zu, size=%zu)", nal_length, pos, size);
			bfree(buffer);
			return false;
		}

		// Check if we need more space
		if (out_pos + 4 + nal_length > estimated_size) {
			estimated_size = out_pos + 4 + nal_length + 1024;
			uint8_t *new_buffer = brealloc(buffer, estimated_size);
			if (!new_buffer) {
				bfree(buffer);
				return false;
			}
			buffer = new_buffer;
		}

		// Write start code
		buffer[out_pos++] = 0x00;
		buffer[out_pos++] = 0x00;
		buffer[out_pos++] = 0x00;
		buffer[out_pos++] = 0x01;

		// Copy NAL data
		memcpy(buffer + out_pos, data + pos, nal_length);
		out_pos += nal_length;
		pos += nal_length;
	}

	*out_data = buffer;
	*out_size = out_pos;
	return true;
}

static bool software_decode_frame(struct vaapi_decoder *decoder, const uint8_t *data, size_t size, uint64_t pts, struct hang_source *context)
{
	obs_log(LOG_DEBUG, "Software decoding frame: size=%zu, pts=%llu", size, pts);

	// For MP4 H.264 (avc1), convert length-prefixed NAL units to start-code format
	uint8_t *converted_data = NULL;
	size_t converted_size = 0;

	if (!convert_mp4_nal_units_to_annex_b(data, size, &converted_data, &converted_size)) {
		obs_log(LOG_ERROR, "Failed to convert NAL units");
		return false;
	}

	AVPacket *packet = av_packet_alloc();
	if (!packet) {
		obs_log(LOG_ERROR, "Failed to allocate AVPacket");
		bfree(converted_data);
		return false;
	}

	packet->data = converted_data;
	packet->size = converted_size;
	packet->pts = pts;

	int ret = avcodec_send_packet(decoder->codec_ctx, packet);
	av_packet_free(&packet);

	if (ret < 0) {
		obs_log(LOG_ERROR, "Error sending packet to decoder: %s", av_err2str(ret));
		av_packet_free(&packet);
		bfree(converted_data);
		return false;
	}

	AVFrame *frame = av_frame_alloc();
	if (!frame) {
		obs_log(LOG_ERROR, "Failed to allocate AVFrame");
		return false;
	}

	ret = avcodec_receive_frame(decoder->codec_ctx, frame);
	if (ret < 0) {
		if (ret != AVERROR(EAGAIN)) {
			obs_log(LOG_ERROR, "Error receiving frame from decoder: %s", av_err2str(ret));
		}
		av_frame_free(&frame);
		av_packet_free(&packet);
		bfree(converted_data);
		return false;
	}

	// Convert frame to RGBA for OBS
	if (!decoder->sws_ctx) {
		decoder->sws_ctx = sws_getContext(
			frame->width, frame->height, (enum AVPixelFormat)frame->format,
			frame->width, frame->height, AV_PIX_FMT_RGBA,
			SWS_BILINEAR | SWS_FULL_CHR_H_INP | SWS_FULL_CHR_H_INT, NULL, NULL, NULL);
		if (!decoder->sws_ctx) {
			obs_log(LOG_ERROR, "Failed to create SWS context");
			av_frame_free(&frame);
			return false;
		}
	}

	// Allocate buffer for RGBA data
	size_t rgba_size = frame->width * frame->height * 4; // 4 bytes per pixel for RGBA
	uint8_t *rgba_data = bzalloc(rgba_size);
	if (!rgba_data) {
		obs_log(LOG_ERROR, "Failed to allocate RGBA buffer");
		av_frame_free(&frame);
		bfree(converted_data);
		return false;
	}

	// Convert frame to RGBA
	uint8_t *dst_data[4] = {rgba_data, NULL, NULL, NULL};
	int dst_linesize[4] = {frame->width * 4, 0, 0, 0};

	int scale_ret = sws_scale(decoder->sws_ctx, (const uint8_t * const *)frame->data, frame->linesize,
	          0, frame->height, dst_data, dst_linesize);

	if (scale_ret < 0) {
		obs_log(LOG_ERROR, "sws_scale failed: %s", av_err2str(ret));
		bfree(rgba_data);
		av_frame_free(&frame);
		bfree(converted_data);
		return false;
	}

	// Store the decoded frame
	store_decoded_frame(context, rgba_data, rgba_size, frame->width, frame->height);

	obs_log(LOG_DEBUG, "Stored decoded RGBA frame for OBS: %dx%d", frame->width, frame->height);

	av_frame_free(&frame);
	bfree(converted_data);
	return true;
}

static void store_decoded_frame(struct hang_source *context, uint8_t *data, size_t size, uint32_t width, uint32_t height)
{
	pthread_mutex_lock(&context->frame_mutex);

	// Free previous frame data
	if (context->current_frame_data) {
		bfree(context->current_frame_data);
	}

	// Store new frame data
	context->current_frame_data = data;
	context->current_frame_size = size;
	context->current_frame_width = width;
	context->current_frame_height = height;

	pthread_mutex_unlock(&context->frame_mutex);
}


