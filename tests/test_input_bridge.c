/* Controllers in input_bridge.c, driven through SDL's virtual joysticks:
 * the first controller is port 0 and the next moves down when it goes away,
 * the controls file remaps buttons, the radial dead zone, and the triggers.
 *
 *   tests/test_input_bridge.sh
 */
#include "../port/io/input_bridge.c"
#include <assert.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
u32 cellGcm_flip_request_count(void) { return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

/* A virtual controller, laid out in SDL_GameControllerButton/Axis order so
 * SDL's automatic mapping for it is the standard one. */
static SDL_Joystick* attach(void)
{
    const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX,
                                                SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1, 0);
    assert(index >= 0);
    SDL_Joystick* js = SDL_JoystickOpen(index);
    assert(js);
    /* A trigger is a full-range axis that SDL maps to 0..32767: released is the minimum. */
    SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
    SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
    return js;
}

/* By instance: the device order of virtual joysticks differs between SDL versions. */
static void detach(SDL_Joystick* js)
{
    const SDL_JoystickID id = SDL_JoystickInstanceID(js);
    SDL_JoystickClose(js);
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_JoystickGetDeviceInstanceID(i) == id) { assert(SDL_JoystickDetachVirtual(i) == 0); return; }
    assert(!"virtual joystick not found");
}

static void poll(void)
{
    for (int i = 0; i < 3; i++) port_pad_poll();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* the log stays in order with a failed assert */
    char path[] = "/tmp/battride-controls-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    const char text[] = "pad_cross = B\npad_circle = A\npad_l1 = LT\npad_l2 = LB\npad_select =\nstick_deadzone = 0.2\n";
    assert(write(fd, text, sizeof text - 1) == (ssize_t)(sizeof text - 1));
    close(fd);
    setenv("PORT_CONTROLS", path, 1);
    setenv("PAD_NO_KEYBOARD", "1", 1);
    assert(SDL_Init(SDL_INIT_GAMECONTROLLER) == 0);
    pad_init_backend();

    poll();
    assert(!s_host_state[0].connected);

    SDL_Joystick* first = attach();
    poll();
    assert(s_host_state[0].connected && !s_host_state[1].connected);

    /* Remapped: A reads as Circle, B as Cross; Back is unbound. */
    SDL_JoystickSetVirtualButton(first, SDL_CONTROLLER_BUTTON_A, 1);
    SDL_JoystickSetVirtualButton(first, SDL_CONTROLLER_BUTTON_BACK, 1);
    poll();
    assert(s_host_state[0].buttons == CELL_PAD_CTRL_CIRCLE);
    assert(s_host_state[0].press_circle == 255 && s_host_state[0].press_cross == 0);
    SDL_JoystickSetVirtualButton(first, SDL_CONTROLLER_BUTTON_A, 0);
    SDL_JoystickSetVirtualButton(first, SDL_CONTROLLER_BUTTON_B, 1);
    poll();
    assert(s_host_state[0].buttons == CELL_PAD_CTRL_CROSS);
    SDL_JoystickSetVirtualButton(first, SDL_CONTROLLER_BUTTON_B, 0);

    /* Dead zone 0.2: 15% travel is centre, full travel is the edge, and
     * travel past the zone starts from the centre. */
    SDL_JoystickSetVirtualAxis(first, SDL_CONTROLLER_AXIS_LEFTX, (Sint16)(0.15 * 32767));
    SDL_JoystickSetVirtualAxis(first, SDL_CONTROLLER_AXIS_RIGHTY, -32767);
    poll();
    assert(s_host_state[0].analog_lx == 128 && s_host_state[0].analog_ly == 128);
    assert(s_host_state[0].analog_ry <= 1);
    SDL_JoystickSetVirtualAxis(first, SDL_CONTROLLER_AXIS_LEFTX, (Sint16)(0.6 * 32767));
    poll();
    const int lx = s_host_state[0].analog_lx;    /* (0.6 - 0.2) / 0.8 = half travel */
    assert(lx >= 128 + 61 && lx <= 128 + 66);
    SDL_JoystickSetVirtualAxis(first, SDL_CONTROLLER_AXIS_LEFTX, 32767);
    SDL_JoystickSetVirtualAxis(first, SDL_CONTROLLER_AXIS_LEFTY, 32767);
    poll();                                      /* a corner: clamped to the circle */
    assert(s_host_state[0].analog_lx >= 128 + 87 && s_host_state[0].analog_lx <= 128 + 92);
    assert(s_host_state[0].analog_ly == s_host_state[0].analog_lx);

    /* LT and LB swapped: LT is L1 now, so L2 has no level; RT stays R2, analog. */
    SDL_JoystickSetVirtualAxis(first, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
    SDL_JoystickSetVirtualAxis(first, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 0);     /* half way */
    poll();
    assert(s_host_state[0].buttons == (CELL_PAD_CTRL_L1 | CELL_PAD_CTRL_R2));
    assert(s_host_state[0].trigger_l2 == 0);
    assert(s_host_state[0].trigger_r2 >= 126 && s_host_state[0].trigger_r2 <= 129);

    /* A second controller is port 1; when the first goes, it becomes port 0. */
    SDL_Joystick* second = attach();
    poll();
    assert(s_host_state[1].connected);
    SDL_JoystickSetVirtualButton(second, SDL_CONTROLLER_BUTTON_Y, 1);
    detach(first);
    poll();
    assert(s_host_state[0].connected && !s_host_state[1].connected);
    assert(s_host_state[0].buttons == CELL_PAD_CTRL_TRIANGLE);

    detach(second);
    poll();
    assert(!s_host_state[0].connected);
    unlink(path);
    printf("test_input_bridge: ok\n");
    return 0;
}
