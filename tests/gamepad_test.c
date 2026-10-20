/*
 * gamepad_test.c — window_sdl.c's gamepad input, checked with SDL's virtual
 * joystick under the dummy video and audio drivers (no window, no device).
 * Built and run by CTest whenever SDL2 is found.
 */
#include "ng_internal.h"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *what, int got, int want) {
    if (got != want) { fprintf(stderr, "FAIL %s: got 0x%02x, want 0x%02x\n", what, got, want); failures++; }
}

static ng_input_t frame(void) {
    ng_input_t in;
    memset(&in, 0, sizeof in);
    window_frame(NULL, &in);
    return in;
}

static SDL_Joystick *attach(void) {
    SDL_VirtualJoystickDesc d;
    SDL_zero(d);
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = SDL_CONTROLLER_AXIS_MAX;
    d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    d.name = "virtual pad";
    int dev = SDL_JoystickAttachVirtualEx(&d);
    return dev < 0 ? NULL : SDL_JoystickOpen(dev);
}

int main(void) {
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    if (window_open("gamepad test", 1) != 0) return 1;

    SDL_Joystick *p1 = attach(), *p2 = attach();
    if (!p1 || !p2) { fprintf(stderr, "FAIL: no virtual joystick: %s\n", SDL_GetError()); return 1; }
    ng_input_t in = frame();   /* the pads' "added" events */
    expect("idle p1", in.p1, 0);
    expect("idle p2", in.p2, 0);

    /* player 1: D-pad up, stick right, A and Y, Start, Back */
    SDL_JoystickSetVirtualButton(p1, SDL_CONTROLLER_BUTTON_DPAD_UP, 1);
    SDL_JoystickSetVirtualAxis(p1, SDL_CONTROLLER_AXIS_LEFTX, 32767);
    SDL_JoystickSetVirtualButton(p1, SDL_CONTROLLER_BUTTON_A, 1);
    SDL_JoystickSetVirtualButton(p1, SDL_CONTROLLER_BUTTON_Y, 1);
    SDL_JoystickSetVirtualButton(p1, SDL_CONTROLLER_BUTTON_START, 1);
    SDL_JoystickSetVirtualButton(p1, SDL_CONTROLLER_BUTTON_BACK, 1);
    /* player 2: B and X, a stick nudge inside the dead zone */
    SDL_JoystickSetVirtualButton(p2, SDL_CONTROLLER_BUTTON_B, 1);
    SDL_JoystickSetVirtualButton(p2, SDL_CONTROLLER_BUTTON_X, 1);
    SDL_JoystickSetVirtualAxis(p2, SDL_CONTROLLER_AXIS_LEFTY, 5000);
    in = frame();
    expect("p1", in.p1, NG_UP | NG_RIGHT | NG_A | NG_D);
    expect("start1", in.start1, 1);
    expect("coin1", in.coin1, 1);
    expect("p2", in.p2, NG_B | NG_C);
    expect("start2", in.start2, 0);

    /* unplugging player 1 releases its buttons; player 2 stays player 2 */
    SDL_JoystickID id1 = SDL_JoystickInstanceID(p1);
    SDL_JoystickClose(p1);
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_JoystickGetDeviceInstanceID(i) == id1) SDL_JoystickDetachVirtual(i);
    in = frame();
    expect("p1 after unplug", in.p1, 0);
    expect("p2 after unplug", in.p2, NG_B | NG_C);

    window_close();
    if (failures) return 1;
    printf("gamepad: all checks pass\n");
    return 0;
}
