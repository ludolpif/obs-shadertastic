static void *shadertastic_transition_create(obs_data_t *settings, obs_source_t *source) {
    struct shadertastic_transition *s = static_cast<shadertastic_transition*>(bzalloc(sizeof(struct shadertastic_transition)));
    s->source = source;
    s->effects = new transition_effects_map_t();
    s->rand_seed = (float)rand() / RAND_MAX;

    debug("MODULE PATH : %s", obs_module_file(""));
    debug("Settings : %s", obs_data_get_json(settings));

    debug("%s", obs_data_get_json(settings));

    std::vector<std::string> dirs = list_directories(obs_module_file("effects"));
    uint8_t transparent_tex_data[2 * 2 * 4] = {0};
    const uint8_t *transparent_tex = transparent_tex_data;
    s->transparent_texture = gs_texture_create(2, 2, GS_RGBA, 1, &transparent_tex, 0);
    s->transition_texrender[0] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    s->transition_texrender[1] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

    for (const auto &dir : dirs) {
        transition_effect_t effect;
        load_effect(effect, dir);
        if (effect.main_shader.effect != NULL) {
            const char *effect_label = effect.label.c_str();
            s->effects->insert(transition_effects_map_t::value_type(dir, effect));

            // Defaults must be set here and not in the transition_defaults() function.
            // as the effects are not loaded yet in transition_defaults()
            for (auto &[_, param] : effect.effect_params) {
                std::string full_param_name = param->get_full_param_name(effect.name.c_str());
                param->set_default(settings, full_param_name.c_str());
            }
        }
        else {
            debug ("NOT LOADING %s", dir.c_str());
            debug ("NOT LOADING main_shader %p", effect.main_shader.effect);
        }
    }

    obs_source_update(source, settings);
    return s;
}
#ifdef DEV_MODE
static void *shadertastic_transition_filter_create(obs_data_t *settings, obs_source_t *source) {
    struct shadertastic_transition *s = static_cast<shadertastic_transition*>(shadertastic_transition_create(settings, source));
    s->is_filter = true;
    s->filter_source_a_texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    s->filter_source_b_texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    return s;
}
#endif
//----------------------------------------------------------------------------------------------------------------------

void shadertastic_transition_destroy(void *data) {
    debug("Destroy");
    struct shadertastic_transition *s = static_cast<shadertastic_transition*>(data);

    obs_enter_graphics();
    gs_texrender_destroy(s->transition_texrender[0]);
    gs_texrender_destroy(s->transition_texrender[1]);
    obs_leave_graphics();
    debug("Destroy2");

    s->release();
    debug("Destroy3");
    bfree(data);
    debug("Destroyed");
}
#ifdef DEV_MODE
void shadertastic_transition_filter_destroy(void *data) {
    shadertastic_transition_destroy(data);
    struct shadertastic_transition *s = static_cast<shadertastic_transition*>(data);
    obs_enter_graphics();
    gs_texrender_destroy(s->filter_source_a_texrender);
    gs_texrender_destroy(s->filter_source_b_texrender);
    obs_leave_graphics();
}
#endif
//----------------------------------------------------------------------------------------------------------------------

static inline float calc_fade(float t, float mul) {
    t *= mul;
    return t > 1.0f ? 1.0f : t;
}
//----------------------------------------------------------------------------------------------------------------------

static float mix_a_fade_in_out(void *data, float t) {
    struct shadertastic_transition *s = static_cast<shadertastic_transition*>(data);
    return 1.0f - calc_fade(t, s->transition_a_mul);
}
//----------------------------------------------------------------------------------------------------------------------

static float mix_b_fade_in_out(void *data, float t) {
    struct shadertastic_transition *s = static_cast<shadertastic_transition*>(data);
    return 1.0f - calc_fade(1.0f - t, s->transition_b_mul);
}
//----------------------------------------------------------------------------------------------------------------------

static float mix_a_cross_fade(void *data, float t) {
    UNUSED_PARAMETER(data);
    return 1.0f - t;
}
//----------------------------------------------------------------------------------------------------------------------

static float mix_b_cross_fade(void *data, float t) {
    UNUSED_PARAMETER(data);
    return t;
}
//----------------------------------------------------------------------------------------------------------------------

void shadertastic_transition_update(void *data, obs_data_t *settings) {
    struct shadertastic_transition *s = static_cast<shadertastic_transition*>(data);
    //debug("Update : %s", obs_data_get_json(settings));

    s->transition_point = (float)obs_data_get_double(settings, "transition_point") / 100.0f;
    s->transition_a_mul = (1.0f / s->transition_point);
    s->transition_b_mul = (1.0f / (1.0f - s->transition_point));

    const char *selected_effect_name = obs_data_get_string(settings, "effect");
    s->selected_effect = &((*s->effects)[selected_effect_name]);

    if (s->selected_effect != NULL) {
        //debug("Selected Effect: %s", selected_effect_name);
        for (auto &[_, param] : s->selected_effect->effect_params) {
            const char *param_name = obs_data_get_string(param->get_metadata(), "name");
            char full_param_name[512];
            sprintf(full_param_name, "%s.%s", selected_effect_name, param_name);

            param->set_data_from_settings(settings, full_param_name);
            //info("Assigned value:  %s %lu", full_param_name, param.data_size);
        }
    }

    if (!obs_data_get_int(settings, "audio_fade_style")) {
        s->mix_a = mix_a_cross_fade;
        s->mix_b = mix_b_cross_fade;
    }
    else {
        s->mix_a = mix_a_fade_in_out;
        s->mix_b = mix_b_fade_in_out;
    }

    if (s->is_filter) {
        s->filter_time = obs_data_get_double(settings, "t");

        if (s->filter_scene_b != NULL) {
            obs_source_remove_active_child(s->source, s->filter_scene_b);
        }
        s->filter_scene_b = obs_scene_get_source(obs_get_scene_by_name(obs_data_get_string(settings, "scene_b")));
        if (s->filter_scene_b != NULL) {
            obs_source_add_active_child(s->source, s->filter_scene_b);
        }
        //debug("SELECTED SCENE: %s %p", obs_data_get_string(settings, "scene_b"), s->filter_scene_b);
    }
}
//----------------------------------------------------------------------------------------------------------------------

void shadertastic_transition_render_init(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy) {
    debug("shadertastic_transition_render_init");
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(t);
    UNUSED_PARAMETER(a);
    UNUSED_PARAMETER(b);
    UNUSED_PARAMETER(cx);
    UNUSED_PARAMETER(cy);
    struct shadertastic_transition *s = static_cast<shadertastic_transition*>(data);
    s->transition_texrender_buffer = 0;
    //s->transition_texture = gs_texrender_get_texture(s->transition_texrender[0]);
}
//----------------------------------------------------------------------------------------------------------------------

void shadertastic_transition_shader_render(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy) {
    struct shadertastic_transition *s = static_cast<shadertastic_transition*>(data);

    const bool previous = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(true);

    transition_effect_t *effect = s->selected_effect;

    if (effect != NULL) {
        gs_texture_t *interm_texture = s->transparent_texture;

        for (int current_step=0; current_step < effect->nb_steps - 1; ++current_step) {
            //debug("%d", current_step);
            s->transition_texrender_buffer = (s->transition_texrender_buffer+1) & 1;
            gs_texrender_reset(s->transition_texrender[s->transition_texrender_buffer]);
            gs_texrender_begin(s->transition_texrender[s->transition_texrender_buffer], cx*3, cy*3);
            effect->set_params(a, b, t, cx, cy, s->rand_seed);
            effect->set_step_params(current_step, interm_texture);
            effect->render_shader(cx, cy);
            gs_texrender_end(s->transition_texrender[s->transition_texrender_buffer]);
            s->transition_texture = gs_texrender_get_texture(s->transition_texrender[s->transition_texrender_buffer]);
            interm_texture = s->transition_texture;
        }
        effect->set_params(a, b, t, cx, cy, s->rand_seed);
        effect->set_step_params(effect->nb_steps - 1, interm_texture);
        effect->render_shader(cx, cy);
    }

    gs_enable_framebuffer_srgb(previous);

//    s->transition_texture = gs_texrender_get_texture(s->transition_texrender);
}
//----------------------------------------------------------------------------------------------------------------------

void shadertastic_transition_video_render(void *data, gs_effect_t *effect) {
    UNUSED_PARAMETER(effect);
    struct shadertastic_transition *s = static_cast<shadertastic_transition*>(data);

    if (s->transition_started) {
        obs_source_t *scene_a = obs_transition_get_source(s->source, OBS_TRANSITION_SOURCE_A);
        obs_source_t *scene_b = obs_transition_get_source(s->source, OBS_TRANSITION_SOURCE_B);

        #ifdef DEV_MODE
            reload_effect(s->selected_effect);
        #endif

        obs_transition_video_render(s->source, shadertastic_transition_render_init);

        info("Started transition: %s -> %s",
            obs_source_get_name(scene_a),
            obs_source_get_name(scene_b)
        );
        obs_source_release(scene_a);
        obs_source_release(scene_b);
        s->transitioning = true;
        s->transition_started = false;
    }

    float t = obs_transition_get_time(s->source);
    if (t >= 1.0f) {
        s->transitioning = false;
    }

    if (s->transitioning) {
        obs_transition_video_render2(s->source, shadertastic_transition_shader_render, s->transparent_texture);
    }
    else {
        enum obs_transition_target target = t < s->transition_point ? OBS_TRANSITION_SOURCE_A : OBS_TRANSITION_SOURCE_B;
        obs_transition_video_render_direct(s->source, target);
        //obs_transition_video_render_direct(s->source, OBS_TRANSITION_SOURCE_A);
    }
}
//----------------------------------------------------------------------------------------------------------------------

#ifdef DEV_MODE
void shadertastic_transition_filter_video_render(void *data, gs_effect_t *effect_unused) {
    UNUSED_PARAMETER(effect_unused);
    struct shadertastic_transition *s = static_cast<shadertastic_transition*>(data);
    const enum gs_color_space preferred_spaces[] = {
        GS_CS_SRGB,
        GS_CS_SRGB_16F,
        GS_CS_709_EXTENDED,
    };

    obs_source_t *target_source = obs_filter_get_target(s->source);
    obs_source_t *parent_source = obs_filter_get_parent(s->source);

    const enum gs_color_space source_space = obs_source_get_color_space(
        target_source,
        OBS_COUNTOF(preferred_spaces), preferred_spaces);
    const enum gs_color_format format = gs_get_format_from_space(source_space);
    uint32_t cx = obs_source_get_width(s->source);
    uint32_t cy = obs_source_get_height(s->source);
    //if (gs_texrender_begin_with_color_space(s->filter_source_a_texrender, cx, cy, source_space))

    transition_effect_t *effect = s->selected_effect;
    if (effect != NULL) {
        gs_texrender_reset(s->filter_source_a_texrender);
        gs_texrender_reset(s->filter_source_b_texrender);
        if (gs_texrender_begin(s->filter_source_a_texrender, cx, cy)) {
            obs_source_video_render(target_source);
            gs_texrender_end(s->filter_source_a_texrender);
            gs_texture_t *source_a_tex = gs_texrender_get_texture(s->filter_source_a_texrender);

            gs_texture_t *source_b_tex = s->transparent_texture;
            if (s->filter_scene_b != NULL) {
                if (gs_texrender_begin(s->filter_source_b_texrender, cx, cy)) {
                    obs_source_video_render(s->filter_scene_b);
                    gs_texrender_end(s->filter_source_b_texrender);
                    source_b_tex = gs_texrender_get_texture(s->filter_source_b_texrender);
                }
            }

            shadertastic_transition_shader_render(s, source_a_tex, source_b_tex, s->filter_time, cx, cy);
        }
    }
}
#endif
//----------------------------------------------------------------------------------------------------------------------

static bool shadertastic_transition_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio, uint32_t mixers, size_t channels, size_t sample_rate) {
    struct shadertastic_transition *s = static_cast<shadertastic_transition*>(data);
    if (!s) {
        return false;
    }

    uint64_t ts = 0;

    const bool success = obs_transition_audio_render(s->source, ts_out, audio, mixers, channels, sample_rate, s->mix_a, s->mix_b);
    if (!ts) {
        return success;
    }

    if (!*ts_out || ts < *ts_out) {
        *ts_out = ts;
    }

    return true;
}
//----------------------------------------------------------------------------------------------------------------------

obs_properties_t *shadertastic_transition_properties(void *data);

bool shadertastic_transition_properties_change_effect_callback(void *priv, obs_properties_t *props, obs_property_t *p, obs_data_t *data) {
    UNUSED_PARAMETER(priv);
    UNUSED_PARAMETER(p);
    UNUSED_PARAMETER(data);
    struct shadertastic_transition *s = static_cast<shadertastic_transition*>(priv);

    if (s->selected_effect != NULL) {
        obs_property_set_visible(obs_properties_get(props, (s->selected_effect->name + "__params").c_str()), false);
    }

    //shadertastic_transition_properties(priv);
    const char *select_effect_name = obs_data_get_string(data, "effect");
    debug("CALLBACK : %s", select_effect_name);
    auto selected_effect = s->effects->find(std::string(select_effect_name));
    if (selected_effect != s->effects->end()) {
        debug("CALLBACK : %s -> %s", select_effect_name, selected_effect->second.name.c_str());
        obs_property_set_visible(obs_properties_get(props, (selected_effect->second.name + "__params").c_str()), true);
    }

    return true;
}

static bool add_sources(void *data, obs_source_t *source)
{
    obs_property_t *p = static_cast<obs_property_t*>(data);

    if (obs_source_is_scene(source)) {
        const char *name = obs_source_get_name(source);
        debug("SOURCE %s", name);
        obs_property_list_add_string(p, name, name);
    }
	return true;
}

bool shadertastic_transition_reload_button_click(obs_properties_t *props, obs_property_t *property, void *data) {
    UNUSED_PARAMETER(props);
    UNUSED_PARAMETER(property);
    struct shadertastic_transition *s = static_cast<shadertastic_transition*>(data);

    #ifdef DEV_MODE
        reload_effect(s->selected_effect);
    #endif
    return true;
}

obs_properties_t *shadertastic_transition_properties(void *data) {
    struct shadertastic_transition *s = static_cast<shadertastic_transition*>(data);
    //struct shadertastic_transition *shadertastic_transition = data;
    obs_properties_t *props = obs_properties_create();
    //obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

    obs_property_t *p;

    // audio fade settings
    if (!s->is_filter) {
        obs_property_t *audio_fade_style = obs_properties_add_list(
            props, "audio_fade_style", obs_module_text("AudioFadeStyle"),
            OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT
        );
        obs_property_list_add_int(audio_fade_style, obs_module_text("CrossFade"), 0);
        obs_property_list_add_int(audio_fade_style, obs_module_text("FadeOutFadeIn"), 1);
    }

    // Filter settings
    if (s->is_filter) {
        obs_properties_add_float_slider(props, "t", "Time", 0.0, 1.0, 0.01);

        p = obs_properties_add_list(props, "scene_b", "Scene", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	    obs_enum_scenes(add_sources, p);
	    obs_properties_add_button(props, "reload_btn", "Reload", shadertastic_transition_reload_button_click);
    }

    // Shader mode
    p = obs_properties_add_list(props, "effect", "Effect", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    for (auto& [effect_name, effect] : *(s->effects)) {
        const char *effect_label = effect.label.c_str();
        obs_property_list_add_string(p, effect_label, effect_name.c_str());
    }
    obs_property_set_modified_callback2(p, shadertastic_transition_properties_change_effect_callback, data);

    obs_property_t *bla = obs_properties_get(props, "effect");

    for (auto& [effect_name, effect] : *(s->effects)) {
        const char *effect_label = effect.label.c_str();
        obs_properties_t *effect_group = obs_properties_create();
        //obs_properties_add_text(effect_group, "", effect_name, OBS_TEXT_INFO);
        for (auto &[_, param] : effect.effect_params) {
            std::string full_param_name = param->get_full_param_name(effect_name);
            param->render_property_ui(full_param_name.c_str(), effect_group);
        }
        obs_properties_add_group(props, (effect_name + "__params").c_str(), effect_label, OBS_GROUP_NORMAL, effect_group);
        obs_property_set_visible(obs_properties_get(props, (effect_name + "__params").c_str()), false);
    }
    if (s->selected_effect != NULL) {
        obs_property_set_visible(obs_properties_get(props, (s->selected_effect->name + "__params").c_str()), true);
    }

//    TODO ABOUT PLUGIN SHOULD GO HERE
    return props;
}
//----------------------------------------------------------------------------------------------------------------------

void shadertastic_transition_defaults(void *data, obs_data_t *settings) {
    struct shadertastic_transition *s = static_cast<shadertastic_transition*>(data);
    obs_data_set_default_double(settings, "transition_point", 50.0);
}
//----------------------------------------------------------------------------------------------------------------------

void shadertastic_transition_start(void *data) {
    struct shadertastic_transition *s = static_cast<shadertastic_transition*>(data);
    s->rand_seed = (float)rand() / RAND_MAX;
    //debug("rand_seed = %f", s->rand_seed);

    uint32_t cx = obs_source_get_width(s->source);
    uint32_t cy = obs_source_get_height(s->source);

    //obs_transition_enable_fixed(s->source, true, (uint32_t) s->duration);

    if (!s->transition_started) {
        s->transition_started = true;

        debug("Started transition of %s", obs_source_get_name(s->source));

        //obs_source_frame_t *frame_a = obs_source_get_frame(s->scene_source_a);
    }
}
//----------------------------------------------------------------------------------------------------------------------

void shadertastic_transition_stop(void *data) {
    debug("STOP");
    struct shadertastic_transition *s = static_cast<shadertastic_transition*>(data);
    if (s->transitioning) {
        s->transitioning = false;
    }
}
//----------------------------------------------------------------------------------------------------------------------

static enum gs_color_space shadertastic_transition_get_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces) {
    UNUSED_PARAMETER(count);
    UNUSED_PARAMETER(preferred_spaces);

    struct shadertastic_transition *s = static_cast<shadertastic_transition*>(data);
    return obs_transition_video_get_color_space(s->source);
}
//----------------------------------------------------------------------------------------------------------------------
