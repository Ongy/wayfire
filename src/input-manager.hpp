#ifndef INPUT_MANAGER_HPP
#define INPUT_MANAGER_HPP

#include <compositor.h>
#include <unordered_set>
#include <map>
#include <vector>

#include "plugin.hpp"


struct wf_gesture_recognizer;
struct key_callback_data;
struct button_callback_data;

class input_manager
{
    private:
        wayfire_grab_interface active_grab = nullptr;

        wl_listener session_listener;
        bool session_active = true;

        weston_keyboard_grab kgrab;
        weston_pointer_grab pgrab;
        weston_touch_grab tgrab;

        wf_gesture_recognizer *gr;

        void handle_gesture(wayfire_touch_gesture g);

        int gesture_id;
        struct wf_gesture_listener {
            wayfire_touch_gesture gesture;
            touch_gesture_callback* call;
            wayfire_output *output;
        };

        std::map<int, wf_gesture_listener> gesture_listeners;
        struct touch_listener {
            uint32_t mod;
            touch_callback* call;
            wayfire_output *output;
        };
        std::map<int, touch_listener> touch_listeners;

        std::vector<key_callback_data*> key_pool;
        std::vector<button_callback_data*> button_pool;
        bool is_touch_enabled();

    public:
        input_manager();
        bool grab_input(wayfire_grab_interface);
        void ungrab_input();
        bool input_grabbed();

        void toggle_session();

        void free_output_bindings(wayfire_output *output);

        void propagate_pointer_grab_axis  (weston_pointer *ptr, weston_pointer_axis_event *ev);
        void propagate_pointer_grab_motion(weston_pointer *ptr, weston_pointer_motion_event *ev);
        void propagate_pointer_grab_button(weston_pointer *ptr, uint32_t button, uint32_t state);

        void propagate_keyboard_grab_key(weston_keyboard *kdb, uint32_t key, uint32_t state);
        void propagate_keyboard_grab_mod(weston_keyboard *kbd, uint32_t depressed,
                                         uint32_t locked, uint32_t latched, uint32_t group);

        void propagate_touch_down(weston_touch*, const timespec*, int32_t, wl_fixed_t, wl_fixed_t);
        void grab_send_touch_down(weston_touch*, int32_t, wl_fixed_t, wl_fixed_t);
        void propagate_touch_up(weston_touch*, const timespec*, int32_t);
        void grab_send_touch_up(weston_touch*, int32_t);
        void propagate_touch_motion(weston_touch*, const timespec*, int32_t, wl_fixed_t, wl_fixed_t);
        void grab_send_touch_motion(weston_touch*, int32_t, wl_fixed_t, wl_fixed_t);

        void check_touch_bindings(weston_touch*, wl_fixed_t sx, wl_fixed_t sy);

        void end_grabs();

        weston_binding *add_key(uint32_t mod, uint32_t key, key_callback *, wayfire_output *output);
        void rem_key(weston_binding *binding);
        void rem_key(key_callback *callback);

        weston_binding *add_button(uint32_t mod, uint32_t button,
                button_callback *, wayfire_output *output);
        void rem_button(weston_binding *binding);
        void rem_button(button_callback *callback);

        int add_touch(uint32_t mod, touch_callback*, wayfire_output *output);
        void rem_touch(int32_t id);
        void rem_touch(touch_callback*);

        int add_gesture(const wayfire_touch_gesture& gesture,
                        touch_gesture_callback* callback, wayfire_output *output);
        void rem_gesture(int id);
        void rem_gesture(touch_gesture_callback*);
};

#endif /* end of include guard: INPUT_MANAGER_HPP */
