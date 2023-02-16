/*
 * Modern effects for a modern Streamer
 * Copyright (C) 2019-2023 Michael Fabian Dirks
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
*/

#include "filter-dynamic-mask.hpp"
#include "strings.hpp"
#include "obs/gs/gs-helper.hpp"
#include "util/util-logging.hpp"

#include "warning-disable.hpp"
#include <array>
#include <sstream>
#include <stdexcept>
#include <vector>
#include "warning-enable.hpp"

#ifdef _DEBUG
#define ST_PREFIX "<%s> "
#define D_LOG_ERROR(x, ...) P_LOG_ERROR(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_WARNING(x, ...) P_LOG_WARN(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_INFO(x, ...) P_LOG_INFO(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_DEBUG(x, ...) P_LOG_DEBUG(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#else
#define ST_PREFIX "<filter::dynamic_mask> "
#define D_LOG_ERROR(...) P_LOG_ERROR(ST_PREFIX __VA_ARGS__)
#define D_LOG_WARNING(...) P_LOG_WARN(ST_PREFIX __VA_ARGS__)
#define D_LOG_INFO(...) P_LOG_INFO(ST_PREFIX __VA_ARGS__)
#define D_LOG_DEBUG(...) P_LOG_DEBUG(ST_PREFIX __VA_ARGS__)
#endif

// Filter to allow dynamic masking
// Allow any channel to affect any other channel
//
// Red/Green/Blue/Alpha Mask Input
// - Red Mask Output
// - Blue Mask Output
// - Green Mask Output
// - Alpha Mask Output

#define ST_I18N "Filter.DynamicMask"

#define ST_I18N_INPUT "Filter.DynamicMask.Input"
#define ST_KEY_INPUT "Filter.DynamicMask.Input"
#define ST_I18N_CHANNEL "Filter.DynamicMask.Channel"
#define ST_KEY_CHANNEL "Filter.DynamicMask.Channel"
#define ST_I18N_CHANNEL_VALUE "Filter.DynamicMask.Channel.Value"
#define ST_KEY_CHANNEL_VALUE "Filter.DynamicMask.Channel.Value"
#define ST_I18N_CHANNEL_MULTIPLIER "Filter.DynamicMask.Channel.Multiplier"
#define ST_KEY_CHANNEL_MULTIPLIER "Filter.DynamicMask.Channel.Multiplier"
#define ST_I18N_CHANNEL_INPUT "Filter.DynamicMask.Channel.Input"
#define ST_KEY_CHANNEL_INPUT "Filter.DynamicMask.Channel.Input"
#define ST_KEY_DEBUG_TEXTURE "Debug.Texture"
#define ST_I18N_DEBUG_TEXTURE ST_I18N ".Debug.Texture"
#define ST_I18N_DEBUG_TEXTURE_BASE ST_I18N_DEBUG_TEXTURE ".Base"
#define ST_I18N_DEBUG_TEXTURE_INPUT ST_I18N_DEBUG_TEXTURE ".Input"

using namespace streamfx::filter::dynamic_mask;

static constexpr std::string_view HELP_URL = "https://github.com/Xaymar/obs-StreamFX/wiki/Filter-Dynamic-Mask";

static std::pair<channel, const char*> channel_translations[] = {
	{channel::Red, S_CHANNEL_RED},
	{channel::Green, S_CHANNEL_GREEN},
	{channel::Blue, S_CHANNEL_BLUE},
	{channel::Alpha, S_CHANNEL_ALPHA},
};

data::data()
{
	auto gctx = streamfx::obs::gs::context();

	_channel_mask_fx = streamfx::obs::gs::effect::create(streamfx::data_file_path("effects/channel-mask.effect"));
}

data::~data() {}

streamfx::obs::gs::effect data::channel_mask_fx()
{
	return _channel_mask_fx;
}

std::shared_ptr<streamfx::filter::dynamic_mask::data> data::get()
{
	static std::mutex                                          instance_lock;
	static std::weak_ptr<streamfx::filter::dynamic_mask::data> weak_instance;

	std::lock_guard<std::mutex> lock(instance_lock);
	auto                        instance = weak_instance.lock();
	if (!instance) {
		instance = std::shared_ptr<streamfx::filter::dynamic_mask::data>{new streamfx::filter::dynamic_mask::data()};
		weak_instance = instance;
	}
	return instance;
}

dynamic_mask_instance::dynamic_mask_instance(obs_data_t* settings, obs_source_t* self)
	: obs::source_instance(settings, self),               //
	  _data(streamfx::filter::dynamic_mask::data::get()), //
	  _gfx_util(::streamfx::gfx::util::get()),            //
	  _translation_map(),                                 //
	  _input(),                                           //
	  _input_child(),                                     //
	  _input_vs(),                                        //
	  _input_ac(),                                        //
	  _have_base(false),                                  //
	  _base_rt(),                                         //
	  _base_tex(),                                        //
	  _base_color_space(GS_CS_SRGB),                      //
	  _base_color_format(GS_RGBA),                        //
	  _have_input(false),                                 //
	  _input_rt(),                                        //
	  _input_tex(),                                       //
	  _input_color_space(GS_CS_SRGB),                     //
	  _input_color_format(GS_RGBA),                       //
	  _have_final(false),                                 //
	  _final_rt(),                                        //
	  _final_tex(),                                       //
	  _channels(),                                        //
	  _precalc(),                                         //
	  _debug_texture(-1)                                  //
{
	update(settings);
}

dynamic_mask_instance::~dynamic_mask_instance()
{
	release();
}

void dynamic_mask_instance::load(obs_data_t* settings)
{
	update(settings);
}

void dynamic_mask_instance::migrate(obs_data_t* data, uint64_t version) {}

void dynamic_mask_instance::update(obs_data_t* settings)
{
	// Update source.
	if (const char* v = obs_data_get_string(settings, ST_KEY_INPUT); (v != nullptr) && (v[0] != '\0')) {
		if (!acquire(v))
			DLOG_ERROR("Failed to acquire Input source '%s'.", v);
	} else {
		release();
	}

	// Update data store
	for (auto kv1 : channel_translations) {
		auto found = _channels.find(kv1.first);
		if (found == _channels.end()) {
			_channels.insert({kv1.first, channel_data()});
			found = _channels.find(kv1.first);
			if (found == _channels.end()) {
				assert(found != _channels.end());
				throw std::runtime_error("Unable to insert element into data _store.");
			}
		}

		std::string chv_key = std::string(ST_KEY_CHANNEL_VALUE) + "." + kv1.second;
		found->second.value = static_cast<float_t>(obs_data_get_double(settings, chv_key.c_str()));
		_precalc.base.ptr[static_cast<size_t>(kv1.first)] = found->second.value;

		std::string chm_key = std::string(ST_KEY_CHANNEL_MULTIPLIER) + "." + kv1.second;
		found->second.scale = static_cast<float_t>(obs_data_get_double(settings, chm_key.c_str()));
		_precalc.scale.ptr[static_cast<size_t>(kv1.first)] = found->second.scale;

		vec4* ch = &_precalc.matrix.x;
		switch (kv1.first) {
		case channel::Red:
			ch = &_precalc.matrix.x;
			break;
		case channel::Green:
			ch = &_precalc.matrix.y;
			break;
		case channel::Blue:
			ch = &_precalc.matrix.z;
			break;
		case channel::Alpha:
			ch = &_precalc.matrix.t;
			break;
		default:
			break;
		}

		for (auto kv2 : channel_translations) {
			std::string ab_key = std::string(ST_KEY_CHANNEL_INPUT) + "." + kv1.second + "." + kv2.second;
			found->second.values.ptr[static_cast<size_t>(kv2.first)] =
				static_cast<float_t>(obs_data_get_double(settings, ab_key.c_str()));
			ch->ptr[static_cast<size_t>(kv2.first)] = found->second.values.ptr[static_cast<size_t>(kv2.first)];
		}
	}

	_debug_texture = obs_data_get_int(settings, ST_KEY_DEBUG_TEXTURE);
}

void dynamic_mask_instance::save(obs_data_t* settings)
{
	if (auto source = _input.lock(); source) {
		obs_data_set_string(settings, ST_KEY_INPUT, source.name().data());
	}

	for (auto kv1 : channel_translations) {
		auto found = _channels.find(kv1.first);
		if (found == _channels.end()) {
			_channels.insert({kv1.first, channel_data()});
			found = _channels.find(kv1.first);
			if (found == _channels.end()) {
				assert(found != _channels.end());
				throw std::runtime_error("Unable to insert element into data _store.");
			}
		}

		std::string chv_key = std::string(ST_KEY_CHANNEL_VALUE) + "." + kv1.second;
		obs_data_set_double(settings, chv_key.c_str(), static_cast<double_t>(found->second.value));

		std::string chm_key = std::string(ST_KEY_CHANNEL_MULTIPLIER) + "." + kv1.second;
		obs_data_set_double(settings, chm_key.c_str(), static_cast<double_t>(found->second.scale));

		for (auto kv2 : channel_translations) {
			std::string ab_key = std::string(ST_KEY_CHANNEL_INPUT) + "." + kv1.second + "." + kv2.second;
			obs_data_set_double(settings, ab_key.c_str(),
								static_cast<double_t>(found->second.values.ptr[static_cast<size_t>(kv2.first)]));
		}
	}
}

gs_color_space dynamic_mask_instance::video_get_color_space(size_t count, const gs_color_space* preferred_spaces)
{
	return _base_color_space;
}

void dynamic_mask_instance::video_tick(float time)
{
	{ // Base Information
		_have_base = false;

		std::array<gs_color_space, 1> preferred_formats = {GS_CS_SRGB};
		_base_color_space = obs_source_get_color_space(obs_filter_get_target(_self), preferred_formats.size(),
													   preferred_formats.data());
		switch (_base_color_space) {
		case GS_CS_SRGB:
			_base_color_format = GS_RGBA;
			break;
		case GS_CS_SRGB_16F:
		case GS_CS_709_EXTENDED:
		case GS_CS_709_SCRGB:
			_base_color_format = GS_RGBA16F;
			break;
		default:
			_base_color_format = GS_RGBA_UNORM;
		}

		if ((obs_source_get_output_flags(obs_filter_get_target(_self)) & OBS_SOURCE_SRGB) == OBS_SOURCE_SRGB) {
			_base_srgb = (_base_color_space <= GS_CS_SRGB_16F);
		} else {
			_base_srgb = false;
		}
	}

	if (auto input = _input.lock(); input) { // Input Information
		_have_input = false;

		std::array<gs_color_space, 1> preferred_formats = {GS_CS_SRGB};
		_input_color_space = obs_source_get_color_space(input, preferred_formats.size(), preferred_formats.data());
		switch (_input_color_space) {
		case GS_CS_SRGB:
			_input_color_format = GS_RGBA;
			break;
		case GS_CS_SRGB_16F:
		case GS_CS_709_EXTENDED:
		case GS_CS_709_SCRGB:
			_input_color_format = GS_RGBA16F;
			break;
		default:
			_input_color_format = GS_RGBA_UNORM;
		}

		if ((input.output_flags() & OBS_SOURCE_SRGB) == OBS_SOURCE_SRGB) {
			_input_srgb = (_base_color_space <= GS_CS_SRGB_16F);
		} else {
			_input_srgb = false;
		}
	} else {
		_have_input = false;
	}

	_have_final = false;
	_final_srgb = _base_srgb;
}

void dynamic_mask_instance::video_render(gs_effect_t* in_effect)
{
	gs_effect_t*  default_effect = obs_get_base_effect(obs_base_effect::OBS_EFFECT_DEFAULT);
	auto          effect         = _data->channel_mask_fx();
	obs_source_t* parent         = obs_filter_get_parent(_self);
	obs_source_t* target         = obs_filter_get_target(_self);
	uint32_t      width          = obs_source_get_base_width(target);
	uint32_t      height         = obs_source_get_base_height(target);
	auto          input          = _input.lock();

#ifdef ENABLE_PROFILING
	streamfx::obs::gs::debug_marker gdmp{streamfx::obs::gs::debug_color_source, "Dynamic Mask '%s' on '%s'",
										 obs_source_get_name(_self), obs_source_get_name(obs_filter_get_parent(_self))};
#endif

	// If there's some issue acquiring information, skip rendering entirely.
	if (!_self || !parent || !target || !width || !height) {
		_self.skip_video_filter();
		return;
	} else if (input && (!input.width() || !input.height())) {
		_self.skip_video_filter();
		return;
	}

	// Capture the base texture for later rendering.
	if (!_have_base) {
#ifdef ENABLE_PROFILING
		streamfx::obs::gs::debug_marker gdm{streamfx::obs::gs::debug_color_cache, "Base Texture"};
#endif
		// Ensure the Render Target matches the expected format.
		if (!_base_rt || (_base_rt->get_color_format() != _base_color_format)) {
			_base_rt = std::make_shared<streamfx::obs::gs::rendertarget>(_base_color_format, GS_ZS_NONE);
		}

		bool previous_srgb  = gs_framebuffer_srgb_enabled();
		auto previous_lsrgb = gs_get_linear_srgb();
		gs_set_linear_srgb(_base_srgb);
		gs_enable_framebuffer_srgb(false);

		// Begin rendering the source with a certain color space.
		if (obs_source_process_filter_begin_with_color_space(_self, _base_color_format, _base_color_space,
															 OBS_ALLOW_DIRECT_RENDERING)) {
			try {
				{
					auto op = _base_rt->render(width, height, _base_color_space);

					// Push a new blend state to stack.
					gs_blend_state_push();
					gs_reset_blend_state();
					gs_enable_blending(false);
					gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
					try {
						// Enable all channels.
						gs_enable_color(true, true, true, true);

						// Disable culling.
						gs_set_cull_mode(GS_NEITHER);

						// Disable depth testing.
						gs_enable_depth_test(false);
						gs_depth_function(GS_ALWAYS);

						// Disable stencil testing
						gs_enable_stencil_test(false);
						gs_enable_stencil_write(false);
						gs_stencil_function(GS_STENCIL_BOTH, GS_ALWAYS);
						gs_stencil_op(GS_STENCIL_BOTH, GS_KEEP, GS_KEEP, GS_KEEP);

						// Set up rendering matrix.
						gs_ortho(0, static_cast<float>(width), 0, static_cast<float>(height), -1., 1.);

						{ // Clear to black.
							vec4 clr = {0., 0., 0., 0.};
							gs_clear(GS_CLEAR_COLOR, &clr, 0., 0);
						}

						// Render the source.
						_self.process_filter_end(default_effect, width, height);

						// Pop the old blend state.
						gs_blend_state_pop();

					} catch (...) {
						gs_blend_state_pop();
						throw;
					}
				}

				_have_base = true;
				_base_rt->get_texture(_base_tex);
			} catch (const std::exception& ex) {
				_self.process_filter_end(default_effect, width, height);
				DLOG_ERROR("Failed to capture base texture: %s", ex.what());
			} catch (...) {
				_self.process_filter_end(default_effect, width, height);
				DLOG_ERROR("Failed to capture base texture.", nullptr);
			}
		}

		gs_set_linear_srgb(previous_lsrgb);
		gs_enable_framebuffer_srgb(previous_srgb);
	}

	// Capture the input texture for later rendering.
	if (!_have_input) {
		if (!input) {
			// Treat no selection as selecting the target filter.
			_have_input         = _have_base;
			_input_tex          = _base_tex;
			_input_color_format = _base_color_format;
			_input_color_space  = _base_color_space;
		} else {
#ifdef ENABLE_PROFILING
			streamfx::obs::gs::debug_marker gdm{streamfx::obs::gs::debug_color_source, "Input '%s'",
												input.name().data()};
#endif
			// Ensure the Render Target matches the expected format.
			if (!_input_rt || (_input_rt->get_color_format() != _input_color_format)) {
				_input_rt = std::make_shared<streamfx::obs::gs::rendertarget>(_input_color_format, GS_ZS_NONE);
			}

			auto previous_lsrgb = gs_get_linear_srgb();
			gs_set_linear_srgb(_input_srgb);
			bool previous_srgb = gs_framebuffer_srgb_enabled();
			gs_enable_framebuffer_srgb(false);

			try {
				{
					auto op = _input_rt->render(input.width(), input.height(), _input_color_space);

					// Push a new blend state to stack.
					gs_blend_state_push();
					gs_reset_blend_state();
					gs_enable_blending(false);
					gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
					try {
						// Enable all channels.
						gs_enable_color(true, true, true, true);

						// Disable culling.
						gs_set_cull_mode(GS_NEITHER);

						// Disable depth testing.
						gs_enable_depth_test(false);
						gs_depth_function(GS_ALWAYS);

						// Disable stencil testing
						gs_enable_stencil_test(false);
						gs_enable_stencil_write(false);
						gs_stencil_function(GS_STENCIL_BOTH, GS_ALWAYS);
						gs_stencil_op(GS_STENCIL_BOTH, GS_KEEP, GS_KEEP, GS_KEEP);

						// Set up rendering matrix.
						gs_ortho(0, static_cast<float>(input.width()), 0, static_cast<float>(input.height()), -1., 1.);

						{ // Clear to black.
							vec4 clr = {0., 0., 0., 0.};
							gs_clear(GS_CLEAR_COLOR, &clr, 0., 0);
						}

						// Render the source.
						obs_source_video_render(input);

						// Pop the old blend state.
						gs_blend_state_pop();
					} catch (...) {
						gs_blend_state_pop();
						throw;
					}
				}

				_have_input = true;
				_input_rt->get_texture(_input_tex);
			} catch (const std::exception& ex) {
				DLOG_ERROR("Failed to capture input texture: %s", ex.what());
			} catch (...) {
				DLOG_ERROR("Failed to capture input texture.", nullptr);
			}

			gs_enable_framebuffer_srgb(previous_srgb);
			gs_set_linear_srgb(previous_lsrgb);
		}
	}

	// Capture the final texture.
	if (!_have_final && _have_base) {
#ifdef ENABLE_PROFILING
		streamfx::obs::gs::debug_marker gdm{streamfx::obs::gs::debug_color_render, "Final Calculation"};
#endif

		// Ensure the Render Target matches the expected format.
		if (!_final_rt || (_final_rt->get_color_format() != _base_color_format)) {
			_final_rt = std::make_shared<streamfx::obs::gs::rendertarget>(_base_color_format, GS_ZS_NONE);
		}

		bool previous_srgb  = gs_framebuffer_srgb_enabled();
		auto previous_lsrgb = gs_get_linear_srgb();
		gs_enable_framebuffer_srgb(_final_srgb);
		gs_set_linear_srgb(_final_srgb);

		try {
			{
				auto op = _final_rt->render(width, height);

				// Push a new blend state to stack.
				gs_blend_state_push();
				gs_reset_blend_state();
				gs_enable_blending(false);
				gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
				try {
					// Enable all channels.
					gs_enable_color(true, true, true, true);

					// Disable culling.
					gs_set_cull_mode(GS_NEITHER);

					// Disable depth testing.
					gs_enable_depth_test(false);
					gs_depth_function(GS_ALWAYS);

					// Disable stencil testing
					gs_enable_stencil_test(false);
					gs_enable_stencil_write(false);
					gs_stencil_function(GS_STENCIL_BOTH, GS_ALWAYS);
					gs_stencil_op(GS_STENCIL_BOTH, GS_KEEP, GS_KEEP, GS_KEEP);

					// Set up rendering matrix.
					gs_ortho(0, 1, 0, 1, -1., 1.);

					{ // Clear to black.
						vec4 clr = {0., 0., 0., 0.};
						gs_clear(GS_CLEAR_COLOR, &clr, 0., 0);
					}

					effect.get_parameter("pMaskInputA").set_texture(_base_tex, _base_srgb);
					effect.get_parameter("pMaskInputB").set_texture(_input_tex, _input_srgb);

					effect.get_parameter("pMaskBase").set_float4(_precalc.base);
					effect.get_parameter("pMaskMatrix").set_matrix(_precalc.matrix);
					effect.get_parameter("pMaskMultiplier").set_float4(_precalc.scale);

					while (gs_effect_loop(effect.get(), "Mask")) {
						_gfx_util->draw_fullscreen_triangle();
					}

					// Pop the old blend state.
					gs_blend_state_pop();
				} catch (...) {
					gs_blend_state_pop();
					throw;
				}
			}

			_final_tex  = _final_rt->get_texture();
			_have_final = true;
		} catch (const std::exception& ex) {
			DLOG_ERROR("Failed to render final texture: %s", ex.what());
		} catch (...) {
			DLOG_ERROR("Failed to render final texture.", nullptr);
		}

		gs_set_linear_srgb(previous_lsrgb);
		gs_enable_framebuffer_srgb(previous_srgb);
	}

	// Enable texture debugging
	switch (_debug_texture) {
	case 0:
		_have_final = _have_base;
		_final_tex  = _base_tex;
		break;
	case 1:
		_have_final = _have_input;
		_final_tex  = _input_tex;
		break;
	}

	// Abort if we don't have a final render.
	if (!_have_final || !_final_tex->get_object()) {
		obs_source_skip_video_filter(_self);
		return;
	}

	// Draw source
	{
#ifdef ENABLE_PROFILING
		streamfx::obs::gs::debug_marker gdm{streamfx::obs::gs::debug_color_render, "Render"};
#endif

		// It is important that we do not modify the blend state here, as it is set correctly by OBS
		gs_set_cull_mode(GS_NEITHER);
		gs_enable_color(true, true, true, true);
		gs_enable_depth_test(false);
		gs_depth_function(GS_ALWAYS);
		gs_enable_stencil_test(false);
		gs_enable_stencil_write(false);
		gs_stencil_function(GS_STENCIL_BOTH, GS_ALWAYS);
		gs_stencil_op(GS_STENCIL_BOTH, GS_ZERO, GS_ZERO, GS_ZERO);

		const bool previous_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(gs_get_linear_srgb());

		gs_effect_t* final_effect = in_effect ? in_effect : default_effect;
		gs_eparam_t* param        = gs_effect_get_param_by_name(final_effect, "image");
		if (!param) {
			DLOG_ERROR("<filter-dynamic-mask:%s> Failed to set image param.", obs_source_get_name(_self));
			gs_enable_framebuffer_srgb(previous_srgb);
			obs_source_skip_video_filter(_self);
			return;
		} else {
			if (gs_get_linear_srgb()) {
				gs_effect_set_texture_srgb(param, _final_tex->get_object());
			} else {
				gs_effect_set_texture(param, _final_tex->get_object());
			}
		}
		while (gs_effect_loop(final_effect, "Draw")) {
			gs_draw_sprite(0, 0, width, height);
		}

		gs_enable_framebuffer_srgb(previous_srgb);
	}
}

void dynamic_mask_instance::enum_active_sources(obs_source_enum_proc_t enum_callback, void* param)
{
	if (_input)
		enum_callback(_self, _input.lock().get(), param);
}

void dynamic_mask_instance::enum_all_sources(obs_source_enum_proc_t enum_callback, void* param)
{
	if (_input)
		enum_callback(_self, _input.lock().get(), param);
}

void streamfx::filter::dynamic_mask::dynamic_mask_instance::show()
{
	if (!_input || !_self.showing() || !(_self.get_filter_parent().showing()))
		return;

	auto input = _input.lock();
	_input_vs  = streamfx::obs::source_showing_reference::add_showing_reference(input);
}

void streamfx::filter::dynamic_mask::dynamic_mask_instance::hide()
{
	_input_vs.reset();
}

void streamfx::filter::dynamic_mask::dynamic_mask_instance::activate()
{
	if (!_input || !_self.active() || !(_self.get_filter_parent().active()))
		return;

	auto input = _input.lock();
	_input_ac  = streamfx::obs::source_active_reference::add_active_reference(input);
}

void streamfx::filter::dynamic_mask::dynamic_mask_instance::deactivate()
{
	_input_ac.reset();
}

bool dynamic_mask_instance::acquire(std::string_view name)
{
	try {
		// Try and acquire the source.
		_input = streamfx::obs::weak_source(name);

		// Ensure that this wouldn't cause recursion.
		_input_child = std::make_unique<streamfx::obs::source_active_child>(_self, _input.lock());

		// Handle the active and showing stuff.
		activate();
		show();

		return true;
	} catch (...) {
		release();
		return false;
	}
}

void dynamic_mask_instance::release()
{
	// Handle the active and showing stuff.
	deactivate();
	hide();

	// Release any references.
	_input_child.reset();
	_input.reset();
}

dynamic_mask_factory::dynamic_mask_factory()
{
	_info.id           = S_PREFIX "filter-dynamic-mask";
	_info.type         = OBS_SOURCE_TYPE_FILTER;
	_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;

	support_active_child_sources(true);
	support_child_sources(true);
	support_size(false);
	support_activity_tracking(true);
	support_visibility_tracking(true);
	support_color_space(true);
	finish_setup();
	register_proxy("obs-stream-effects-filter-dynamic-mask");
}

dynamic_mask_factory::~dynamic_mask_factory() {}

const char* dynamic_mask_factory::get_name()
{
	return D_TRANSLATE(ST_I18N);
}

void dynamic_mask_factory::get_defaults2(obs_data_t* data)
{
	obs_data_set_default_int(data, ST_KEY_CHANNEL, static_cast<int64_t>(channel::Red));
	for (auto kv : channel_translations) {
		obs_data_set_default_double(data, (std::string(ST_KEY_CHANNEL_VALUE) + "." + kv.second).c_str(), 1.0);
		obs_data_set_default_double(data, (std::string(ST_KEY_CHANNEL_MULTIPLIER) + "." + kv.second).c_str(), 1.0);
		for (auto kv2 : channel_translations) {
			obs_data_set_default_double(
				data, (std::string(ST_KEY_CHANNEL_INPUT) + "." + kv.second + "." + kv2.second).c_str(), 0.0);
		}
	}
	obs_data_set_default_int(data, ST_KEY_DEBUG_TEXTURE, -1);
}

obs_properties_t* dynamic_mask_factory::get_properties2(dynamic_mask_instance* data)
{
	obs_properties_t* props = obs_properties_create();
	obs_property_t*   p;

	_translation_cache.clear();

#ifdef ENABLE_FRONTEND
	{
		obs_properties_add_button2(props, S_MANUAL_OPEN, D_TRANSLATE(S_MANUAL_OPEN),
								   streamfx::filter::dynamic_mask::dynamic_mask_factory::on_manual_open, nullptr);
	}
#endif

	{ // Input
		p = obs_properties_add_list(props, ST_KEY_INPUT, D_TRANSLATE(ST_I18N_INPUT), OBS_COMBO_TYPE_LIST,
									OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(p, "", "");
		obs::source_tracker::get()->enumerate(
			[&p](std::string name, ::streamfx::obs::source) {
				std::stringstream sstr;
				sstr << name << " (" << D_TRANSLATE(S_SOURCETYPE_SOURCE) << ")";
				obs_property_list_add_string(p, sstr.str().c_str(), name.c_str());
				return false;
			},
			obs::source_tracker::filter_video_sources);
		obs::source_tracker::get()->enumerate(
			[&p](std::string name, ::streamfx::obs::source) {
				std::stringstream sstr;
				sstr << name << " (" << D_TRANSLATE(S_SOURCETYPE_SCENE) << ")";
				obs_property_list_add_string(p, sstr.str().c_str(), name.c_str());
				return false;
			},
			obs::source_tracker::filter_scenes);
	}

	const char* pri_chs[] = {S_CHANNEL_RED, S_CHANNEL_GREEN, S_CHANNEL_BLUE, S_CHANNEL_ALPHA};
	for (auto pri_ch : pri_chs) {
		auto grp = obs_properties_create();

		{
			_translation_cache.push_back(translate_string(D_TRANSLATE(ST_I18N_CHANNEL_VALUE), D_TRANSLATE(pri_ch)));
			std::string buf = std::string(ST_KEY_CHANNEL_VALUE) + "." + pri_ch;
			p = obs_properties_add_float_slider(grp, buf.c_str(), _translation_cache.back().c_str(), -100.0, 100.0,
												0.01);
			obs_property_set_long_description(p, _translation_cache.back().c_str());
		}

		const char* sec_chs[] = {S_CHANNEL_RED, S_CHANNEL_GREEN, S_CHANNEL_BLUE, S_CHANNEL_ALPHA};
		for (auto sec_ch : sec_chs) {
			_translation_cache.push_back(translate_string(D_TRANSLATE(ST_I18N_CHANNEL_INPUT), D_TRANSLATE(sec_ch)));
			std::string buf = std::string(ST_KEY_CHANNEL_INPUT) + "." + pri_ch + "." + sec_ch;
			p = obs_properties_add_float_slider(grp, buf.c_str(), _translation_cache.back().c_str(), -100.0, 100.0,
												0.01);
			obs_property_set_long_description(p, _translation_cache.back().c_str());
		}

		{
			_translation_cache.push_back(
				translate_string(D_TRANSLATE(ST_I18N_CHANNEL_MULTIPLIER), D_TRANSLATE(pri_ch)));
			std::string buf = std::string(ST_KEY_CHANNEL_MULTIPLIER) + "." + pri_ch;
			p = obs_properties_add_float_slider(grp, buf.c_str(), _translation_cache.back().c_str(), -100.0, 100.0,
												0.01);
			obs_property_set_long_description(p, _translation_cache.back().c_str());
		}

		{
			_translation_cache.push_back(translate_string(D_TRANSLATE(ST_I18N_CHANNEL), D_TRANSLATE(pri_ch)));
			std::string buf = std::string(ST_KEY_CHANNEL) + "." + pri_ch;
			obs_properties_add_group(props, buf.c_str(), _translation_cache.back().c_str(),
									 obs_group_type::OBS_GROUP_NORMAL, grp);
		}
	}

	{
		auto grp = obs_properties_create();
		obs_properties_add_group(props, "Debug", D_TRANSLATE(S_ADVANCED), OBS_GROUP_NORMAL, grp);

		{
			auto p = obs_properties_add_list(grp, ST_KEY_DEBUG_TEXTURE, D_TRANSLATE(ST_I18N_DEBUG_TEXTURE),
											 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
			obs_property_list_add_int(p, D_TRANSLATE(S_STATE_DISABLED), -1);
			obs_property_list_add_int(p, D_TRANSLATE(ST_I18N_DEBUG_TEXTURE_BASE), 0);
			obs_property_list_add_int(p, D_TRANSLATE(ST_I18N_DEBUG_TEXTURE_INPUT), 1);
		}
	}

	return props;
}

std::string dynamic_mask_factory::translate_string(const char* format, ...)
{
	va_list vargs;
	va_start(vargs, format);
	std::vector<char> buffer(2048);
	std::size_t       len = static_cast<size_t>(vsnprintf(buffer.data(), buffer.size(), format, vargs));
	va_end(vargs);
	return std::string(buffer.data(), buffer.data() + len);
}

#ifdef ENABLE_FRONTEND
bool dynamic_mask_factory::on_manual_open(obs_properties_t* props, obs_property_t* property, void* data)
{
	try {
		streamfx::open_url(HELP_URL);
		return false;
	} catch (const std::exception& ex) {
		D_LOG_ERROR("Failed to open manual due to error: %s", ex.what());
		return false;
	} catch (...) {
		D_LOG_ERROR("Failed to open manual due to unknown error.", "");
		return false;
	}
}
#endif

std::shared_ptr<dynamic_mask_factory> _filter_dynamic_mask_factory_instance = nullptr;

void streamfx::filter::dynamic_mask::dynamic_mask_factory::initialize()
{
	try {
		if (!_filter_dynamic_mask_factory_instance)
			_filter_dynamic_mask_factory_instance = std::make_shared<dynamic_mask_factory>();
	} catch (const std::exception& ex) {
		D_LOG_ERROR("Failed to initialize due to error: %s", ex.what());
	} catch (...) {
		D_LOG_ERROR("Failed to initialize due to unknown error.", "");
	}
}

void streamfx::filter::dynamic_mask::dynamic_mask_factory::finalize()
{
	_filter_dynamic_mask_factory_instance.reset();
}

std::shared_ptr<dynamic_mask_factory> streamfx::filter::dynamic_mask::dynamic_mask_factory::get()
{
	return _filter_dynamic_mask_factory_instance;
}
