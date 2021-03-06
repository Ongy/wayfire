#include "debug.hpp"
#include "opengl.hpp"
#include "output.hpp"
#include "view.hpp"
#include "core.hpp"
#include "signal-definitions.hpp"
#include "input-manager.hpp"
#include "render-manager.hpp"
#include "workspace-manager.hpp"

#include <linux/input.h>

#include "wm.hpp"

#include <sstream>
#include <memory>
#include <dlfcn.h>
#include <algorithm>

#include <libweston-desktop.h>
#include <gl-renderer-api.h>

#include <cstring>
#include <config.hpp>

namespace
{
template<class A, class B> B union_cast(A object)
{
    union {
        A x;
        B y;
    } helper;
    helper.x = object;
    return helper.y;
}
}

/* Controls loading of plugins */
struct plugin_manager
{
    std::vector<wayfire_plugin> plugins;

    plugin_manager(wayfire_output *o, wayfire_config *config)
    {
        load_dynamic_plugins();
        init_default_plugins();

        for (auto p : plugins)
        {
            p->grab_interface = new wayfire_grab_interface_t(o);
            p->output = o;

            p->init(config);
        }
    }

    ~plugin_manager()
    {
        for (auto p : plugins)
        {
            p->fini();
            delete p->grab_interface;

            if (p->dynamic)
                dlclose(p->handle);
            p.reset();
        }
    }



    wayfire_plugin load_plugin_from_file(std::string path, void **h)
    {
        void *handle = dlopen(path.c_str(), RTLD_NOW);
        if(handle == NULL)
        {
            errio << "Can't load plugin " << path << std::endl;
            errio << "\t" << dlerror() << std::endl;
            return nullptr;
        }

        debug << "Loading plugin " << path << std::endl;

        auto initptr = dlsym(handle, "newInstance");
        if(initptr == NULL)
        {
            errio << "Missing function newInstance in file " << path << std::endl;
            errio << dlerror();
            return nullptr;
        }

        get_plugin_instance_t init = union_cast<void*, get_plugin_instance_t> (initptr);
        *h = handle;
        return wayfire_plugin(init());
    }

    void load_dynamic_plugins()
    {
        std::stringstream stream(core->plugins);
        auto path = core->plugin_path + "/wayfire/";

        std::string plugin;
        while(stream >> plugin)
        {
            if(plugin != "")
            {
                void *handle = nullptr;
                auto ptr = load_plugin_from_file(path + "/lib" + plugin + ".so", &handle);
                if(ptr)
                {
                    ptr->handle  = handle;
                    ptr->dynamic = true;
                    plugins.push_back(ptr);
                }
            }
        }
    }

    template<class T>
    wayfire_plugin create_plugin()
    {
        return std::static_pointer_cast<wayfire_plugin_t>(std::make_shared<T>());
    }

    void init_default_plugins()
    {
        plugins.push_back(create_plugin<wayfire_focus>());
        plugins.push_back(create_plugin<wayfire_close>());
        plugins.push_back(create_plugin<wayfire_exit>());
        plugins.push_back(create_plugin<wayfire_fullscreen>());
        plugins.push_back(create_plugin<wayfire_handle_focus_parent>());
    }
};

/* End plugin_manager */

const weston_gl_renderer_api *render_manager::renderer_api = nullptr;

/* Start render_manager */
render_manager::render_manager(wayfire_output *o)
{
    output = o;

    pixman_region32_init(&frame_damage);
    pixman_region32_init_rect(&single_pixel, output->handle->x, output->handle->y, 1, 1);

    view_moved_cb = [=] (signal_data *data)
    {
        auto conv = static_cast<view_geometry_changed_signal*> (data);
        assert(conv);

        if (fdamage_track_enabled && !conv->view->is_special)
            update_full_damage_tracking_view(conv->view);
    };
    output->connect_signal("view-geometry-changed", &view_moved_cb);

    viewport_changed_cb = [=] (signal_data *data)
    {
        if (fdamage_track_enabled)
        {
            fdamage_track_enabled = false;
            update_full_damage_tracking();
        }
    };
    output->connect_signal("viewport-changed", &viewport_changed_cb);
}

void render_manager::load_context()
{
    ctx = OpenGL::create_gles_context(output, core->shadersrc.c_str());
    OpenGL::bind_context(ctx);

    dirty_context = false;

    output->emit_signal("reload-gl", nullptr);
}

void render_manager::release_context()
{
    OpenGL::release_context(ctx);
    dirty_context = true;
}

render_manager::~render_manager()
{
    if (idle_redraw_source)
        wl_event_source_remove(idle_redraw_source);
    if (full_repaint_source)
        wl_event_source_remove(full_repaint_source);

    release_context();
    pixman_region32_fini(&frame_damage);
    pixman_region32_fini(&single_pixel);

    output->disconnect_signal("view-geometry-changed", &view_moved_cb);
    output->disconnect_signal("viewport-changed", &viewport_changed_cb);
}

void redraw_idle_cb(void *data)
{
    wayfire_output *output = (wayfire_output*) data;
    assert(output);

    render_manager::renderer_api->schedule_repaint(output->handle);
    output->render->idle_redraw_source = NULL;
}

void render_manager::auto_redraw(bool redraw)
{
    constant_redraw += (redraw ? 1 : -1);
    if (constant_redraw > 1) /* no change, exit */
        return;
    if (constant_redraw < 0)
    {
        constant_redraw = 0;
        return;
    }

    schedule_redraw();
}

void render_manager::schedule_redraw()
{
    auto loop = wl_display_get_event_loop(core->ec->wl_display);

    if (idle_redraw_source == NULL)
        idle_redraw_source = wl_event_loop_add_idle(loop, redraw_idle_cb, output);
}

struct wf_fdamage_track_cdata : public wf_custom_view_data
{
    weston_transform transform;
};

void render_manager::update_full_damage_tracking()
{
    if (fdamage_track_enabled)
        return;

    fdamage_track_enabled = true;
    output->workspace->for_each_view([=] (wayfire_view view)
    {
        update_full_damage_tracking_view(view);
    });
}

void render_manager::update_full_damage_tracking_view(wayfire_view view)
{
    GetTuple(vx, vy, output->workspace->get_current_workspace());
    GetTuple(vw, vh, output->workspace->get_workspace_grid_size());
    auto og = output->get_full_geometry();

    wf_fdamage_track_cdata *data;
    const std::string dname = "__fdamage_track";

    auto it = view->custom_data.find(dname);
    if (it == view->custom_data.end())
    {
        data = new wf_fdamage_track_cdata;
        view->custom_data[dname] = data;

        weston_matrix_init(&data->transform.matrix);
        wl_list_insert(&view->handle->geometry.transformation_list, &data->transform.link);
    } else
    {
        data = static_cast<wf_fdamage_track_cdata*> (it->second);
    }

    assert(data);
    weston_matrix_init(&data->transform.matrix);
    weston_matrix_scale(&data->transform.matrix, 1. / vw, 1. / vh, 1.0);

    int dx = vx * og.width;
    int dy = vy * og.height;
    weston_matrix_translate(&data->transform.matrix, dx, dy, 0);

    /* Transform the view's coordinates in a coordinate space where the (0, 0)
     * is actually the outputs top left workspacea */
    int rx = view->geometry.x + dx - og.x - view->ds_geometry.x;
    int ry = view->geometry.y + dy - og.y - view->ds_geometry.y;
    int tx = rx / vw;
    int ty = ry / vh;

    weston_matrix_translate(&data->transform.matrix, tx - rx, ty - ry, 0);
    weston_view_geometry_dirty(view->handle);
}

void render_manager::disable_full_damage_tracking()
{
    if (!fdamage_track_enabled)
        return;
    fdamage_track_enabled = false;

    const std::string dname = "__fdamage_track";
    output->workspace->for_each_view([&] (wayfire_view view)
    {
        auto it = view->custom_data.find(dname);
        if (it != view->custom_data.end())
        {
            auto data = static_cast<wf_fdamage_track_cdata*> (it->second);
            assert(data);

            wl_list_remove(&data->transform.link);
            weston_view_geometry_dirty(view->handle);

            delete data;
            view->custom_data.erase(it);
        }
    });
}

void render_manager::get_ws_damage(std::tuple<int, int> ws, pixman_region32_t *out_damage)
{
    GetTuple(vx, vy, ws);
    GetTuple(vw, vh, output->workspace->get_workspace_grid_size());
    auto og = output->get_full_geometry();

    if (!pixman_region32_selfcheck(out_damage))
        pixman_region32_init(out_damage);

    pixman_region32_intersect_rect(out_damage, &frame_damage,
                                   og.x + vx * og.width / vw,
                                   og.y + vy * og.height / vh,
                                   og.width / vw,
                                   og.height / vh);

    int nrect;
    auto rects = pixman_region32_rectangles(out_damage, &nrect);

    if (nrect > 0)
    {
        pixman_box32_t boxes[nrect];

        for (int i = 0; i < nrect; i++)
        {
            boxes[i].x1 = (rects[i].x1 - og.x) * vw;
            boxes[i].x2 = (rects[i].x2 - og.x) * vw;
            boxes[i].y1 = (rects[i].y1 - og.y) * vh;
            boxes[i].y2 = (rects[i].y2 - og.y) * vh;
        }


        pixman_region32_fini(out_damage);
        pixman_region32_init_rects(out_damage, boxes, nrect);
    }

    GetTuple(cx, cy, output->workspace->get_current_workspace());
    pixman_region32_translate(out_damage, og.x - cx * og.width, og.y - cy * og.height);
}

void render_manager::reset_renderer()
{
    renderer = nullptr;
    if (!streams_running)
        disable_full_damage_tracking();

    dirty_renderer = true;
}

void render_manager::set_renderer(render_hook_t rh)
{
    if (!rh) {
        renderer = std::bind(std::mem_fn(&render_manager::transformation_renderer), this);
    } else {
        renderer = rh;
    }
}

void render_manager::set_hide_overlay_panels(bool set)
{
    draw_overlay_panel = !set;
}

void render_manager::render_panels()
{
    auto views = output->workspace->get_panels();
    auto it = views.rbegin();
    while (it != views.rend())
    {
        auto view = *it;
        if (!view->is_hidden) /* use is_visible() when implemented */
            view->render(TEXTURE_TRANSFORM_USE_DEVCOORD);

        ++it;
    }
}

bool render_manager::paint(pixman_region32_t *damage)
{
    if (dirty_context)
        load_context();

    if (streams_running || renderer)
    {
        pixman_region32_copy(&frame_damage, damage);
        pixman_region32_subtract(&frame_damage, &frame_damage, &single_pixel);
        update_full_damage_tracking();
    }

    if (renderer)
    {
        /* this is needed so that the buffers can be swapped appropriately
         * and that screen recording can track the damage */
        pixman_region32_union(damage, damage, &output->handle->region);

        frame_was_custom_rendered = 1;
        OpenGL::bind_context(ctx);
        renderer();
        return true;
    } else {
        frame_was_custom_rendered = 0;
        return false;
    }
}

void idle_full_redraw_cb(void *data)
{
    auto output = (wayfire_output*) data;

    weston_output_damage(output->handle);
    output->render->full_repaint_source = NULL;
}

void render_manager::post_paint()
{
    run_effects();
    if (frame_was_custom_rendered && draw_overlay_panel)
        render_panels();

    if (constant_redraw)
        schedule_redraw();

    if (dirty_renderer)
    {
        if (full_repaint_source == NULL)
        {
            auto loop = wl_display_get_event_loop(core->ec->wl_display);
            full_repaint_source = wl_event_loop_add_idle(loop,
                                                         idle_full_redraw_cb, output);
        }

        dirty_renderer = false;
    }
}

void render_manager::run_effects()
{
    std::vector<effect_hook_t*> active_effects;
    for (auto effect : output_effects)
        active_effects.push_back(effect);

    for (auto& effect : active_effects)
        (*effect)();
}

void render_manager::transformation_renderer()
{
    auto views = output->workspace->get_renderable_views_on_workspace(
            output->workspace->get_current_workspace());

    OpenGL::use_device_viewport();
    GL_CALL(glClearColor(1, 0, 0, 1));
    GL_CALL(glClear(GL_COLOR_BUFFER_BIT));

    auto it = views.rbegin();
    while (it != views.rend())
    {
        auto view = *it;
        if (!view->is_hidden) /* use is_visible() when implemented */
        {
            view->render(TEXTURE_TRANSFORM_USE_DEVCOORD);
        }

        ++it;
    }
}

void render_manager::add_output_effect(effect_hook_t* hook, wayfire_view v)
{
    if (v)
        v->effects.push_back(hook);
    else
        output_effects.push_back(hook);
}

void render_manager::rem_effect(const effect_hook_t *hook, wayfire_view v)
{
    if (v)
    {
        auto it = std::remove_if(v->effects.begin(), v->effects.end(),
                                 [hook] (const effect_hook_t *h)
                                 {
                                     if (h == hook)
                                         return true;
                                     return false;
                                 });

        v->effects.erase(it, v->effects.end());
    } else
    {
        auto it = std::remove_if(output_effects.begin(), output_effects.end(),
                                 [hook] (const effect_hook_t *h)
                                 {
                                     if (h == hook)
                                         return true;
                                     return false;
                                 });

        output_effects.erase(it, output_effects.end());
    }
}

void render_manager::texture_from_workspace(std::tuple<int, int> vp,
        GLuint &fbuff, GLuint &tex)
{
    OpenGL::bind_context(output->render->ctx);

    if (fbuff == (uint)-1 || tex == (uint)-1)
        OpenGL::prepare_framebuffer(fbuff, tex);

    GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbuff));

    auto g = output->get_full_geometry();

    GetTuple(x, y, vp);
    GetTuple(cx, cy, output->workspace->get_current_workspace());

    int dx = -g.x + (cx - x)  * output->handle->width,
        dy = -g.y + (cy - y)  * output->handle->height;

    auto views = output->workspace->get_renderable_views_on_workspace(vp);
    auto it = views.rbegin();

    while (it != views.rend())
    {
        auto v = *it;
        if (v->is_visible())
        {
            if (!v->is_special)
            {
                v->geometry.x += dx;
                v->geometry.y += dy;

                v->render();

                v->geometry.x -= dx;
                v->geometry.y -= dy;
            } else
            {
                v->render();
            }
        }
        ++it;
    };

    GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
}

void render_manager::workspace_stream_start(wf_workspace_stream *stream)
{
    streams_running++;
    stream->running = true;
    stream->scale_x = stream->scale_y = 1;

    OpenGL::bind_context(output->render->ctx);

    if (stream->fbuff == (uint)-1 || stream->tex == (uint)-1)
        OpenGL::prepare_framebuffer(stream->fbuff, stream->tex);

    GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, stream->fbuff));

    GetTuple(x, y, stream->ws);
    GetTuple(cx, cy, output->workspace->get_current_workspace());

    /* TODO: this assumes we use viewports arranged in a grid
     * It would be much better to actually ask the workspace_manager
     * for view's position on the given workspace*/
    int dx = (cx - x)  * output->handle->width,
        dy = (cy - y)  * output->handle->height;

    auto views = output->workspace->get_renderable_views_on_workspace(stream->ws);
    auto it = views.rbegin();

    while (it != views.rend())
    {
        auto v = *it;
        if (v->is_visible())
        {
            if (!v->is_special)
            {
                v->geometry.x += dx;
                v->geometry.y += dy;
                v->render();
                v->geometry.x -= dx;
                v->geometry.y -= dy;
            } else
            {
                v->render();
            }
        }
        ++it;
    };

    GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
}

void render_manager::workspace_stream_update(wf_workspace_stream *stream,
                                             float scale_x, float scale_y)
{
    OpenGL::bind_context(output->render->ctx);
    auto g = output->get_full_geometry();

    GetTuple(x, y, stream->ws);
    GetTuple(cx, cy, output->workspace->get_current_workspace());

    int dx = g.x + (x - cx) * g.width,
        dy = g.y + (y - cy) * g.height;

    pixman_region32_t ws_damage;
    pixman_region32_init(&ws_damage);
    get_ws_damage(stream->ws, &ws_damage);

    float current_resolution = stream->scale_x * stream->scale_y;
    float target_resolution = scale_x * scale_y;

    float scaling = std::max(current_resolution / target_resolution,
                             target_resolution / current_resolution);

    if (scaling > 2)
    {
        pixman_region32_union_rect(&ws_damage, &ws_damage, dx, dy,
                g.width, g.height);
    }

    /* we don't have to update anything */
    if (!pixman_region32_not_empty(&ws_damage))
    {
        pixman_region32_fini(&ws_damage);
        return;
    }

    if (scale_x != stream->scale_x || scale_y != stream->scale_y)
    {
        stream->scale_x = scale_x;
        stream->scale_y = scale_y;

        pixman_region32_union_rect(&ws_damage, &ws_damage, dx, dy,
                g.width, g.height);
    }

    auto views = output->workspace->get_renderable_views_on_workspace(stream->ws);

    struct damaged_view
    {
        wayfire_view view;
        pixman_region32_t *damage;
    };

    std::vector<damaged_view> update_views;

    auto it = views.begin();

    while (it != views.end() && pixman_region32_not_empty(&ws_damage))
    {
        damaged_view dv;
        dv.view = *it;

        if (dv.view->is_visible())
        {
            dv.damage = new pixman_region32_t;
            if (dv.view->is_special)
            {
                /* make background's damage be at the target viewport */
                pixman_region32_init_rect(dv.damage,
                    dv.view->geometry.x - dv.view->ds_geometry.x + (dx - g.x),
                    dv.view->geometry.y - dv.view->ds_geometry.y + (dy - g.y),
                    dv.view->surface->width, dv.view->surface->height);
            } else
            {
                pixman_region32_init_rect(dv.damage,
                        dv.view->geometry.x - dv.view->ds_geometry.x,
                        dv.view->geometry.y - dv.view->ds_geometry.y,
                        dv.view->surface->width, dv.view->surface->height);
            }

            pixman_region32_intersect(dv.damage, dv.damage, &ws_damage);
            if (pixman_region32_not_empty(dv.damage)) {
                update_views.push_back(dv);
                /* If we are processing background, then this is not correct, as its
                 * transform.opaque isn't positioned properly. But as
                 * background is the last in the list, we don' care */
                pixman_region32_subtract(&ws_damage, &ws_damage, &dv.view->handle->transform.opaque);
            } else {
                pixman_region32_fini(dv.damage);
                delete dv.damage;
            }
        }
        ++it;
    };

    GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, stream->fbuff));

    glm::mat4 scale = glm::scale(glm::mat4(1.0), glm::vec3(scale_x, scale_y, 1));
    glm::mat4 translate = glm::translate(glm::mat4(1.0), glm::vec3(scale_x - 1, scale_y - 1, 0));
    std::swap(wayfire_view_transform::global_scale, scale);
    std::swap(wayfire_view_transform::global_translate, translate);

    auto rev_it = update_views.rbegin();
    while(rev_it != update_views.rend())
    {
        auto dv = *rev_it;

#define render_op(dx, dy) \
        dv.view->geometry.x -= dx; \
        dv.view->geometry.y -= dy; \
        dv.view->render(0, dv.damage); \
        dv.view->geometry.x += dx; \
        dv.view->geometry.y += dy;


        pixman_region32_translate(dv.damage, -(dx - g.x), -(dy - g.y));
        if (dv.view->is_special)
        {
            render_op(0, 0);
        } else
        {
            render_op(dx - g.x, dy - g.y);
        }

        pixman_region32_fini(dv.damage);
        delete dv.damage;
        ++rev_it;
    }

    std::swap(wayfire_view_transform::global_scale, scale);
    std::swap(wayfire_view_transform::global_translate, translate);

    GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    pixman_region32_fini(&ws_damage);
}

void render_manager::workspace_stream_stop(wf_workspace_stream *stream)
{
    streams_running--;
    stream->running = false;

    if (!streams_running && !renderer)
        disable_full_damage_tracking();
}

/* End render_manager */

/* Start SignalManager */

void wayfire_output::connect_signal(std::string name, signal_callback_t* callback)
{
    signals[name].push_back(callback);
}

void wayfire_output::disconnect_signal(std::string name, signal_callback_t* callback)
{
    auto it = std::remove_if(signals[name].begin(), signals[name].end(),
    [=] (const signal_callback_t *call) {
        return call == callback;
    });

    signals[name].erase(it, signals[name].end());
}

void wayfire_output::emit_signal(std::string name, signal_data *data)
{
    std::vector<signal_callback_t> callbacks;
    for (auto x : signals[name])
        callbacks.push_back(*x);

    for (auto x : callbacks)
        x(data);
}

/* End SignalManager */
/* Start output */
wayfire_output* wl_output_to_wayfire_output(uint32_t output)
{
    if (output == (uint32_t) -1)
        return core->get_active_output();

    wayfire_output *result = nullptr;
    core->for_each_output([output, &result] (wayfire_output *wo) {
        if (wo->handle->id == output)
            result = wo;
    });

    return result;
}

void shell_add_background(struct wl_client *client, struct wl_resource *resource,
        uint32_t output, struct wl_resource *surface, int32_t x, int32_t y)
{
    weston_surface *wsurf = (weston_surface*) wl_resource_get_user_data(surface);
    wayfire_view view = (wsurf ? core->find_view(wsurf) : nullptr);
    auto wo = wl_output_to_wayfire_output(output);
    if (!wo) wo = view ? view->output : nullptr;

     if (!wo || !view) {
        errio << "shell_add_background called with invalid surface or output" << std::endl;
        return;
    }

    debug << "wf_shell: add_background" << std::endl;
    wo->workspace->add_background(view, x, y);
}

void shell_add_panel(struct wl_client *client, struct wl_resource *resource,
        uint32_t output, struct wl_resource *surface)
{
    weston_surface *wsurf = (weston_surface*) wl_resource_get_user_data(surface);
    wayfire_view view = (wsurf ? core->find_view(wsurf) : nullptr);
    auto wo = wl_output_to_wayfire_output(output);
    if (!wo) wo = view ? view->output : nullptr;


    if (!wo || !wsurf) {
        errio << "shell_add_panel called with invalid surface or output" << std::endl;
        return;
    }

    debug << "wf_shell: add_panel" << std::endl;
    wo->workspace->add_panel(view);
}

void shell_configure_panel(struct wl_client *client, struct wl_resource *resource,
        uint32_t output, struct wl_resource *surface, int32_t x, int32_t y)
{
    weston_surface *wsurf = (weston_surface*) wl_resource_get_user_data(surface);
    wayfire_view view = (wsurf ? core->find_view(wsurf) : nullptr);
    auto wo = wl_output_to_wayfire_output(output);
    if (!wo) wo = view ? view->output : nullptr;

    if (!wo || !view) {
        errio << "shell_configure_panel called with invalid surface or output" << std::endl;
        return;
    }

    wo->workspace->configure_panel(view, x, y);
}

void shell_reserve(struct wl_client *client, struct wl_resource *resource,
        uint32_t output, uint32_t side, uint32_t width, uint32_t height)
{
    auto wo = wl_output_to_wayfire_output(output);

    if (!wo) {
        errio << "shell_reserve called with invalid output" << std::endl;
        return;
    }

    debug << "wf_shell: reserve" << std::endl;
    wo->workspace->reserve_workarea((wayfire_shell_panel_position)side, width, height);
}

void shell_set_color_gamma(wl_client *client, wl_resource *res,
        uint32_t output, wl_array *r, wl_array *g, wl_array *b)
{
    auto wo = wl_output_to_wayfire_output(output);
    if (!wo || !wo->handle->set_gamma) {
        errio << "shell_set_gamma called with invalid/unsupported output" << std::endl;
        return;
    }

    size_t size = wo->handle->gamma_size * sizeof(uint16_t);
    if (r->size != size || b->size != size || g->size != size) {
        errio << "gamma size is not equal to output's gamma size " << r->size << " " << size << std::endl;
        return;
    }

    size /= sizeof(uint16_t);
#ifndef ushort
#define ushort unsigned short
    wo->handle->set_gamma(wo->handle, size, (ushort*)r->data, (ushort*)g->data, (ushort*)b->data);
#undef ushort
#endif
}

void shell_output_fade_in_start(wl_client *client, wl_resource *res, uint32_t output)
{
    auto wo = wl_output_to_wayfire_output(output);
    if (!wo)
    {
        errio << "output_fade_in with wrong output!" << std::endl;
        return;
    }

    wo->emit_signal("output-fade-in-request", nullptr);
}

const struct wayfire_shell_interface shell_interface_impl {
    .add_background = shell_add_background,
    .add_panel = shell_add_panel,
    .configure_panel = shell_configure_panel,
    .reserve = shell_reserve,
    .set_color_gamma = shell_set_color_gamma,
    .output_fade_in_start = shell_output_fade_in_start
};

wayfire_output::wayfire_output(weston_output *handle, wayfire_config *c)
{
    this->handle = handle;

    render = new render_manager(this);
    plugin = new plugin_manager(this, c);

    weston_output_damage(handle);
    weston_output_schedule_repaint(handle);

    if (handle->set_dpms && c->get_section("core")->get_int("dpms_enabled", 1))
    	handle->set_dpms(handle, WESTON_DPMS_ON);
}

workspace_manager::~workspace_manager()
{ }

wayfire_output::~wayfire_output()
{
    core->input->free_output_bindings(this);

    delete workspace;
    delete plugin;
    delete render;
}

weston_geometry wayfire_output::get_full_geometry()
{
    return {handle->x, handle->y,
            handle->width, handle->height};
}

void wayfire_output::set_transform(wl_output_transform new_tr)
{
    int old_w = handle->width;
    int old_h = handle->height;
    weston_output_set_transform(handle, new_tr);

    render->ctx->width = handle->width;
    render->ctx->height = handle->height;

    for (auto resource : core->shell_clients)
        wayfire_shell_send_output_resized(resource, handle->id,
                                          handle->width, handle->height);
    emit_signal("output-resized", nullptr);

    //ensure_pointer();

    workspace->for_each_view([=] (wayfire_view view) {
        if (view->fullscreen || view->maximized) {
            auto g = get_full_geometry();
            if (view->maximized)
                g = workspace->get_workarea();

            int vx = view->geometry.x / old_w;
            int vy = view->geometry.y / old_h;

            g.x += vx * handle->width;
            g.y += vy * handle->height;

            view->set_geometry(g);
        } else {
            float px = 1. * view->geometry.x / old_w;
            float py = 1. * view->geometry.y / old_h;
            float pw = 1. * view->geometry.width / old_w;
            float ph = 1. * view->geometry.height / old_h;

            view->set_geometry(px * handle->width, py * handle->height,
			    pw * handle->width, ph * handle->height);
        }
    });
}

wl_output_transform wayfire_output::get_transform()
{
    return (wl_output_transform)handle->transform;
}

std::tuple<int, int> wayfire_output::get_screen_size()
{
    return std::make_tuple(handle->width, handle->height);
}

void wayfire_output::ensure_pointer()
{
    auto ptr = weston_seat_get_pointer(core->get_current_seat());
    if (!ptr) return;

    int px = wl_fixed_to_int(ptr->x), py = wl_fixed_to_int(ptr->y);

    auto g = get_full_geometry();
    if (!point_inside({px, py}, g)) {
        wl_fixed_t cx = wl_fixed_from_int(g.x + g.width / 2);
        wl_fixed_t cy = wl_fixed_from_int(g.y + g.height / 2);

        weston_pointer_motion_event ev;
        ev.mask |= WESTON_POINTER_MOTION_ABS;
        ev.x = wl_fixed_to_double(cx);
        ev.y = wl_fixed_to_double(cy);

        weston_pointer_move(ptr, &ev);
    }
}

void wayfire_output::activate()
{
}

void wayfire_output::deactivate()
{
    // TODO: what do we do?
}

void wayfire_output::attach_view(wayfire_view v)
{
    v->output = this;
    workspace->view_bring_to_front(v);

    auto sig_data = create_view_signal{v};
    emit_signal("attach-view", &sig_data);
}

void wayfire_output::detach_view(wayfire_view v)
{
    auto sig_data = destroy_view_signal{v};
    emit_signal("detach-view", &sig_data);

    if (v->keep_count <= 0)
        workspace->view_removed(v);

    wayfire_view next = nullptr;

    auto views = workspace->get_views_on_workspace(workspace->get_current_workspace());
    for (auto wview : views) {
        if (wview->handle != v->handle && wview->is_mapped && !wview->destroyed) {
            next = wview;
            break;
        }
    }

    if (active_view == v) {
        if (next == nullptr) {
            active_view = nullptr;
        } else {
            if (v->keep_count) {
                set_active_view(next);
            } else { /* Some plugins wants to keep the view, let it manage the position */
                focus_view(next);
            }
        }
    }
}

void wayfire_output::bring_to_front(wayfire_view v) {
    assert(v);

    weston_view_geometry_dirty(v->handle);
    weston_layer_entry_remove(&v->handle->layer_link);

    workspace->view_bring_to_front(v);

    weston_view_geometry_dirty(v->handle);
    weston_surface_damage(v->surface);
    weston_desktop_surface_propagate_layer(v->desktop_surface);
}

void wayfire_output::set_active_view(wayfire_view v)
{
    if (v == active_view)
        return;

    if (active_view && !active_view->destroyed)
        weston_desktop_surface_set_activated(active_view->desktop_surface, false);

    active_view = v;
    if (active_view) {
        weston_view_activate(v->handle, core->get_current_seat(),
                WESTON_ACTIVATE_FLAG_CLICKED | WESTON_ACTIVATE_FLAG_CONFIGURE);
        weston_desktop_surface_set_activated(v->desktop_surface, true);
    }
}

void wayfire_output::focus_view(wayfire_view v, weston_seat *seat)
{
    if (seat == nullptr)
        seat = core->get_current_seat();

    set_active_view(v);

    if (v) {
        debug << "output: " << handle->id << " focus: " << v->desktop_surface << std::endl;
        bring_to_front(v);
    } else {
        debug << "output: " << handle->id << " focus: 0" << std::endl;
        weston_seat_set_keyboard_focus(seat, NULL);
    }

    focus_view_signal data;
    data.focus = v;
    emit_signal("focus-view", &data);
}

wayfire_view wayfire_output::get_top_view()
{
    if (active_view)
        return active_view;

    wayfire_view view = nullptr;
    workspace->for_each_view([&view] (wayfire_view v) {
        if (!view)
            view = v;
    });

    return view;
}

wayfire_view wayfire_output::get_view_at_point(int x, int y)
{
    wayfire_view chosen = nullptr;

    workspace->for_each_view([x, y, &chosen] (wayfire_view v) {
        if (v->is_visible() && point_inside({x, y}, v->geometry)) {
            if (chosen == nullptr)
                chosen = v;
        }
    });

    return chosen;
}

bool wayfire_output::activate_plugin(wayfire_grab_interface owner, bool lower_fs)
{
    if (!owner)
        return false;

    if (core->get_active_output() != this)
        return false;

    if (active_plugins.find(owner) != active_plugins.end())
    {
        debug << "output: " << handle->id << " activating plugin: " << owner->name << std::endl;
        active_plugins.insert(owner);
        return true;
    }

    for(auto act_owner : active_plugins)
    {
        bool compatible = (act_owner->abilities_mask & owner->abilities_mask) == 0;
        if (!compatible)
            return false;
    }

    /* _activation_request is a special signal,
     * used to specify when a plugin is activated. It is used only internally, plugins
     * shouldn't listen for it */
    if (lower_fs && active_plugins.empty())
        emit_signal("_activation_request", (signal_data*)1);

    active_plugins.insert(owner);
    debug << "output: " << handle->id << " activating plugin: " << owner->name << std::endl;
    return true;
}

bool wayfire_output::deactivate_plugin(wayfire_grab_interface owner)
{
    auto it = active_plugins.find(owner);
    if (it == active_plugins.end())
        return true;

    active_plugins.erase(it);
    debug << "output: " << handle->id << " deactivating plugin: " << owner->name << std::endl;

    if (active_plugins.count(owner) == 0)
    {
        owner->ungrab();
        active_plugins.erase(owner);

        if (active_plugins.empty())
            emit_signal("_activation_request", nullptr);

        return true;
    }


    return false;
}

bool wayfire_output::is_plugin_active(owner_t name)
{
    for (auto act : active_plugins)
        if (act && act->name == name)
            return true;

    return false;
}

wayfire_grab_interface wayfire_output::get_input_grab_interface()
{
    for (auto p : active_plugins)
        if (p && p->is_grabbed())
            return p;

    return nullptr;
}

/* simple wrappers for core->input, as it isn't exposed to plugins */

weston_binding* wayfire_output::add_key(uint32_t mod, uint32_t key, key_callback* callback)
{
    return core->input->add_key(mod, key, callback, this);
}

weston_binding* wayfire_output::add_button(uint32_t mod, uint32_t button, button_callback* callback)
{
    return core->input->add_button(mod, button, callback, this);
}

int wayfire_output::add_touch(uint32_t mod, touch_callback* callback)
{
    return core->input->add_touch(mod, callback, this);
}

void wayfire_output::rem_touch(int32_t id)
{
    core->input->rem_touch(id);
}

int wayfire_output::add_gesture(const wayfire_touch_gesture& gesture,
                                touch_gesture_callback* callback)
{
    return core->input->add_gesture(gesture, callback, this);
}

void wayfire_output::rem_gesture(int id)
{
    core->input->rem_gesture(id);
}
