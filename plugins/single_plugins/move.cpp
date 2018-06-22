#include <output.hpp>
#include <debug.hpp>
#include <core.hpp>
#include <view.hpp>
#include <workspace-manager.hpp>
#include <render-manager.hpp>
#include <linux/input.h>
#include <signal-definitions.hpp>
#include "snap_signal.hpp"

class wayfire_move : public wayfire_plugin_t
{
    signal_callback_t move_request, view_destroyed;
    button_callback activate_binding;
    touch_callback touch_activate_binding;
    wayfire_view view;

    bool is_using_touch;
    bool was_client_request;
    bool enable_snap;
    int slot;
    int snap_pixels;

    int prev_x, prev_y;

    public:
        void init(wayfire_config *config)
        {
            grab_interface->name = "move";
            grab_interface->abilities_mask = WF_ABILITY_CHANGE_VIEW_GEOMETRY;

            auto section = config->get_section("move");
            wf_option button = section->get_option("activate", "<alt> BTN_LEFT");
            if (!button->as_button().valid())
                return;

            activate_binding = [=] (uint32_t, int x, int y)
            {
                is_using_touch = false;
                was_client_request = false;
                auto focus = core->get_cursor_focus();
                auto view = focus ? core->find_view(focus->get_main_surface()) : nullptr;

                if (view && view->role != WF_VIEW_ROLE_SHELL_VIEW)
                    initiate(view, x, y);
            };

            touch_activate_binding = [=] (int32_t sx, int32_t sy)
            {
                is_using_touch = true;
                was_client_request = false;
                auto focus = core->get_touch_focus();
                auto view = focus ? core->find_view(focus->get_main_surface()) : nullptr;

                if (view && view->role != WF_VIEW_ROLE_SHELL_VIEW)
                    initiate(view, sx, sy);
            };

            output->add_button(button, &activate_binding);
            output->add_touch(button->as_button().mod, &touch_activate_binding);

            enable_snap = int(*section->get_option("enable_snap", "1"));
            snap_pixels = *section->get_option("snap_threshold", "2");

            using namespace std::placeholders;
            grab_interface->callbacks.pointer.button =
            [=] (uint32_t b, uint32_t state)
            {
                /* the request usually comes with the left button ... */
                if (state == WLR_BUTTON_RELEASED && was_client_request && b == BTN_LEFT)
                    return input_pressed(state);

                if (b != button->as_button().button)
                    return;

                is_using_touch = false;
                input_pressed(state);
            };

            grab_interface->callbacks.pointer.motion = [=] (int x, int y)
            {
                input_motion(x, y);
            };

            grab_interface->callbacks.touch.motion = [=] (int32_t id, int32_t sx, int32_t sy)
            {
                if (id > 0) return;
                input_motion(sx, sy);
            };

            grab_interface->callbacks.touch.up = [=] (int32_t id)
            {
                if (id == 0)
                    input_pressed(WLR_BUTTON_RELEASED);
            };

            move_request = std::bind(std::mem_fn(&wayfire_move::move_requested), this, _1);
            output->connect_signal("move-request", &move_request);

            view_destroyed = [=] (signal_data* data)
            {
                if (get_signaled_view(data) == view)
                {
                    view = nullptr;
                    input_pressed(WLR_BUTTON_RELEASED);
                }
            };
            output->connect_signal("detach-view", &view_destroyed);
            output->connect_signal("unmap-view", &view_destroyed);
        }

        void move_requested(signal_data *data)
        {
            // TODO: Implement touch movement
            auto view = get_signaled_view(data);
            if (view)
            {
                is_using_touch = false;
                was_client_request = true;
                GetTuple(x, y, output->get_cursor_position());
                initiate(view, x, y);
            }
        }

        void initiate(wayfire_view view, int sx, int sy)
        {
            if (!view || view->destroyed)
                return;

            if (!output->workspace->
                    get_implementation(output->workspace->get_current_workspace())->
                        view_movable(view))
                return;

            if (view->get_output() != output)
                return;

            if (!output->activate_plugin(grab_interface))
                return;

            if (!grab_interface->grab()) {
                output->deactivate_plugin(grab_interface);
                return;
            }

            prev_x = sx;
            prev_y = sy;

            output->bring_to_front(view);
            if (view->maximized)
                view->maximize_request(false);
            if (view->fullscreen)
                view->fullscreen_request(view->get_output(), false);

            view->set_moving(true);

            if (enable_snap)
                slot = 0;

            this->view = view;
            output->render->auto_redraw(true);
        }

        void input_pressed(uint32_t state)
        {
            if (state != WLR_BUTTON_RELEASED)
                return;

            grab_interface->ungrab();
            output->deactivate_plugin(grab_interface);
            output->render->auto_redraw(false);

            if (view)
            {
                if (view->role == WF_VIEW_ROLE_SHELL_VIEW)
                    return;

                view->set_moving(false);
                if (enable_snap && slot != 0) {
                    snap_signal data;
                    data.view = view;
                    data.tslot = (slot_type)slot;

                    output->emit_signal("view-snap", &data);
                }
            }
        }

        int calc_slot()
        {
            auto g = output->get_full_geometry();

            bool is_left = std::abs(prev_x) <= snap_pixels;
            bool is_right = std::abs(g.width - prev_x) <= snap_pixels;
            bool is_top = std::abs(prev_y) < snap_pixels;
            bool is_bottom = std::abs(g.height - prev_y) < snap_pixels;

            if (is_left && is_top)
                return SLOT_TL;
            else if (is_left && is_bottom)
                return SLOT_BL;
            else if (is_left)
                return SLOT_LEFT;
            else if (is_right && is_top)
                return SLOT_TR;
            else if (is_right && is_bottom)
                return SLOT_BR;
            else if (is_right)
                return SLOT_RIGHT;
            else if (is_top)
                return SLOT_CENTER;
            else if (is_bottom)
                return SLOT_BOTTOM;
            else
                return 0;
        }

        void input_motion(int x, int y)
        {
            auto vg = view->get_wm_geometry();
            view->move(vg.x + x - prev_x, vg.y + y - prev_y);
            prev_x = x;
            prev_y = y;

            GetTuple(global_x, global_y, core->get_cursor_position());
            auto target_output = core->get_output_at(global_x, global_y);
            if (target_output != output)
            {
                move_request_signal req;
                req.view = view;

                auto old_g = output->get_full_geometry();
                auto new_g = target_output->get_full_geometry();
                auto wm_g = view->get_wm_geometry();

                view->move(wm_g.x + old_g.x - new_g.x, wm_g.y + old_g.y - new_g.y, false);
                view->set_moving(false);

                core->move_view_to_output(view, target_output);

                core->focus_output(target_output);
                target_output->emit_signal("move-request", &req);

                return;
            }

            /* TODO: possibly show some visual indication */
            if (enable_snap)
                slot = calc_slot();
        }
};

extern "C" {
    wayfire_plugin_t* newInstance()
    {
        return new wayfire_move();
    }
}

