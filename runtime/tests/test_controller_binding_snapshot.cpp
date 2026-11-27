#include "controller_binding_snapshot.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr,"line %d: %s (%s)\n",__LINE__,#value,SDL_GetError()); std::exit(1); } } while (0)
static void events() { SDL_GameControllerUpdate(); SDL_Event e; while(SDL_PollEvent(&e)) {} }
static bool button(SDL_Joystick* joystick,unsigned id,bool down) {
#ifdef PSX_SDL3
    return SDL_SetJoystickVirtualButton(joystick,(int)id,down);
#else
    return SDL_JoystickSetVirtualButton(joystick,(int)id,down)==0;
#endif
}
static bool axis_value(SDL_Joystick* joystick,unsigned id,Sint16 value) {
#ifdef PSX_SDL3
    return SDL_SetJoystickVirtualAxis(joystick,(int)id,value);
#else
    return SDL_JoystickSetVirtualAxis(joystick,(int)id,value)==0;
#endif
}
static bool detach(int id) {
#ifdef PSX_SDL3
    return SDL_DetachVirtualJoystick((SDL_JoystickID)id);
#else
    return SDL_JoystickDetachVirtual(id)==0;
#endif
}
int main() {
    /* Keep the owner's attached hardware outside this process-local fixture. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x0000/0x0000");
    CHECK(SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS)==0);
    int index[2]; SDL_Joystick* joystick[2]; SDL_GameController* pad[2];
    for (int p=0;p<2;++p) {
#ifdef PSX_SDL3
        SDL_VirtualJoystickDesc desc{};SDL_INIT_INTERFACE(&desc);
        desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;desc.naxes=6;desc.nbuttons=15;
        desc.button_mask=(1u<<15)-1;desc.axis_mask=(1u<<6)-1;
        index[p]=(int)SDL_AttachVirtualJoystick(&desc);CHECK(index[p]!=0);
        joystick[p]=SDL_OpenJoystick((SDL_JoystickID)index[p]);CHECK(joystick[p]);
        pad[p]=SDL_OpenGamepad((SDL_JoystickID)index[p]);CHECK(pad[p]);
#else
        index[p]=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,6,15,0);
        CHECK(index[p]>=0);
        joystick[p]=SDL_JoystickOpen(index[p]);CHECK(joystick[p]);
        pad[p]=SDL_GameControllerOpen(index[p]);CHECK(pad[p]);
#endif
    }
    for(unsigned button=0;button<15;++button) {
        CHECK(::button(joystick[0],button,true));events();
        CHECK(psx_controller_binding_pressed(pad[0],RI_BUTTON(button),12000));
        CHECK(!psx_controller_binding_pressed(pad[1],RI_BUTTON(button),12000));
        for(unsigned other=0;other<15;++other)
            CHECK(psx_controller_binding_pressed(pad[0],RI_BUTTON(other),12000)==(other==button));
        CHECK(::button(joystick[0],button,false));events();
        CHECK(!psx_controller_binding_pressed(pad[0],RI_BUTTON(button),12000));
    }
    /* Independent devices, remapped axes, exact threshold edges and triggers. */
    const int values[]={-32768,-12001,-12000,0,12000,12001,32767};
    for(unsigned axis=0;axis<6;++axis) for(int value : values) {
        CHECK(axis_value(joystick[0],axis,(Sint16)value));events();
        // SDL remaps unsigned trigger travel; compare its actual delivered raw axis.
        const int raw=SDL_GameControllerGetAxis(pad[0],(SDL_GameControllerAxis)axis);
        CHECK(psx_controller_binding_pressed(pad[0],RI_AXIS(axis,1),12000)==(raw>12000));
        CHECK(psx_controller_binding_pressed(pad[0],RI_AXIS(axis,0),12000)==(raw< -12000));
        CHECK(axis_value(joystick[0],axis,0));events();
    }
    CHECK(button(joystick[1],SDL_CONTROLLER_BUTTON_Y,true));events();
    CHECK(detach(index[0]));events();
    CHECK(!SDL_GameControllerGetAttached(pad[0]));
    CHECK(SDL_GameControllerGetAttached(pad[1]));
    CHECK(psx_controller_binding_pressed(pad[1],RI_BUTTON(SDL_CONTROLLER_BUTTON_Y),12000));
    CHECK(!psx_controller_binding_pressed(nullptr,RI_BUTTON(SDL_CONTROLLER_BUTTON_Y),12000));
    SDL_GameControllerClose(pad[0]);SDL_JoystickClose(joystick[0]);
    int remaining=-1;
#ifdef PSX_SDL3
    remaining=index[1];
#else
    const auto other_id=SDL_JoystickInstanceID(joystick[1]);
    for(int i=0;i<SDL_NumJoysticks();++i)if(SDL_JoystickGetDeviceInstanceID(i)==other_id)remaining=i;
#endif
    CHECK(remaining>=0);SDL_GameControllerClose(pad[1]);SDL_JoystickClose(joystick[1]);
    CHECK(detach(remaining));SDL_Quit();
    std::puts("PASS SDL snapshot adapter: button isolation, remapping, axis boundaries, trigger snapshots and disconnect isolation");
}
