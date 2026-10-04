#include "local_mouse_policy.h"
#include <cstdio>
#include <initializer_list>
#include <limits>

#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
namespace {
constexpr uint64_t Ms=1000000;
bool guest=true, capture_ok=true, active=false, held=false;
unsigned captures=0, releases=0, notices=0, resets=0;
unsigned hold_presses=0, hold_releases=0;
psx::MouseControl suppressed=psx::MouseControl::None;
bool capture(void*,bool on) { if(on) ++captures; else ++releases; return !on || capture_ok; }
void suppress(void*,psx::MouseControl c) { suppressed=c; }
void notice(void*,const char*) { ++notices; }
int eligible(uint32_t buttons) { return guest && (buttons&9)==9; }
void event(const PSXModMouseEvent* e) {
    if(e->type==PSX_MOD_MOUSE_RESET || e->type==PSX_MOD_MOUSE_ACQUIRED) { active=held=false; ++resets; }
    if(e->type==PSX_MOD_MOUSE_MOTION) active=true;
    if(e->type==PSX_MOD_MOUSE_HOLD_PRESS) { ++hold_presses;held=true;active=false; }
    if(e->type==PSX_MOD_MOUSE_HOLD_RELEASE) { ++hold_releases;active=held=false; }
}
void sample(uint64_t,PSXModMouseOutput* out) {
    out->override_right=1; out->rx=active?250:128; out->ry=active?64:128;
}
}
int main() {
    psx::LocalMousePolicy policy({nullptr,capture,suppress,notice});
    psx::LocalMouseHost host;
    host.live=host.focused=host.connected=host.analog=true;
    uint8_t x=99,y=77; policy.update(0,host); policy.sample(0,x,y); CHECK(x==99 && y==77);
    PSXModMousePolicy callbacks{sizeof(PSXModMousePolicy),PSX_MOD_MOUSE_HOLD_RIGHT,eligible,event,sample};
    CHECK(policy.install(&callbacks)); CHECK(!policy.install(&callbacks));
    policy.update(10*Ms,host);
    CHECK(policy.control(10*Ms,10*Ms,psx::MouseControl::Left,true,false));
    CHECK(policy.captured() && suppressed==psx::MouseControl::Left && !active);
    policy.sample(10*Ms,x,y); CHECK(x==128 && y==128 && notices==0);
    policy.motion(11*Ms,11*Ms,0.2,0.1); policy.sample(11*Ms,x,y); CHECK(x==250 && y==64);
    policy.sample(11*Ms,x,y); CHECK(x==250 && y==64);
    CHECK(policy.control(0,11*Ms,psx::MouseControl::Escape,true,false));
    CHECK(!policy.captured() && !active && suppressed==psx::MouseControl::Escape);
    CHECK(notices==0); // Normal activation, movement and emergency release are silent.
    CHECK(!policy.control(12*Ms,12*Ms,psx::MouseControl::Escape,true,false));
    uint64_t t=20*Ms;
    auto acquire=[&]() {
        t+=10*Ms;policy.update(t,host);
        policy.control(t,t,psx::MouseControl::Left,false,false);
        return policy.control(t,t,psx::MouseControl::Left,true,false);
    };
    CHECK(acquire());
    policy.control(t+Ms,t+Ms,psx::MouseControl::Right,true,false); CHECK(held && !active);
    policy.motion(t+2*Ms,t+2*Ms,30,0); CHECK(active);
    policy.control(UINT64_MAX,0,psx::MouseControl::Right,false,false); CHECK(!held && !active);
    policy.motion(t+3*Ms,t+3*Ms,30,0); CHECK(active);
    policy.control(t+4*Ms,t+4*Ms,psx::MouseControl::Right,true,false);
    policy.control(t+6*Ms,t+6*Ms,psx::MouseControl::Right,false,false); CHECK(!active);
    policy.motion(t+5*Ms,t+6*Ms,30,0); CHECK(!policy.captured() && !active);
    // Every lifecycle host gate neutralizes; restored eligibility needs a click.
    for(int gate=0;gate<7;++gate) {
        policy.reset();CHECK(acquire());policy.motion(t+Ms,t+Ms,30,0);
        auto off=host;
        if(gate==0)off.focused=false; if(gate==1)off.live=false;
        if(gate==2)off.connected=false;if(gate==3)off.analog=false;
        if(gate==4)off.native_right=true;if(gate==5)off.start=true;
        if(gate==6)guest=false;
        policy.update(t+2*Ms,off); CHECK(!policy.captured() && !active);
        guest=true;policy.update(t+3*Ms,host);
        CHECK(!policy.control(t+3*Ms,t+3*Ms,psx::MouseControl::Left,true,false));
        policy.motion(t+4*Ms,t+4*Ms,60,0); CHECK(!active && !policy.captured());
        x=1;y=255;policy.sample(t+4*Ms,x,y); CHECK(x==1 && y==255);
    }
    policy.reset();CHECK(acquire());
    policy.motion(t+2*Ms,t+Ms,10,0);CHECK(!policy.captured()); // future
    CHECK(acquire());policy.motion(t+Ms,t+Ms,10,0);
    policy.motion(t,t+Ms,10,0);CHECK(!policy.captured()); // backwards
    CHECK(acquire());policy.motion(t,t+251*Ms,10,0);CHECK(!policy.captured()); // stale
    CHECK(acquire());policy.motion(t,t,std::numeric_limits<double>::quiet_NaN(),0);CHECK(!policy.captured());
    CHECK(acquire());policy.motion(t+Ms,t+Ms,10,0);
    t+=300*Ms;policy.update(t,host);CHECK(!policy.captured() && !active);
    CHECK(!policy.control(t-Ms,t,psx::MouseControl::Left,true,false)); // no stall backlog rearm
    CHECK(acquire()); policy.control(t+Ms,t+Ms,psx::MouseControl::Right,true,false);
    policy.reset(); CHECK(acquire()); CHECK(!held);
    policy.motion(t+Ms,t+Ms,20,0); CHECK(active && !held); // held across reset cannot latch
    policy.control(t+2*Ms,t+2*Ms,psx::MouseControl::Right,false,false);
    policy.control(t+3*Ms,t+3*Ms,psx::MouseControl::Right,true,false);CHECK(held);
    policy.reset(); host.hold_conflict=true;CHECK(acquire());
    policy.control(t+Ms,t+Ms,psx::MouseControl::Right,false,false);
    policy.control(t+2*Ms,t+2*Ms,psx::MouseControl::Right,true,false);CHECK(!held);
    const unsigned before=notices;policy.update(t+3*Ms,host);CHECK(notices==before);

    // Exact conflict -> acquire OTHER button -> hold down -> motion -> up.
    // A rejected physical edge must never send HOLD_RELEASE or kill a flick.
    policy.reset();
    policy.control(t+4*Ms,t+4*Ms,psx::MouseControl::Right,false,false);
    CHECK(acquire());
    const unsigned conflict_presses=hold_presses, conflict_releases=hold_releases;
    policy.control(t+Ms,t+Ms,psx::MouseControl::Right,true,false);
    policy.motion(t+2*Ms,t+2*Ms,30,0);CHECK(active && !held);
    policy.control(t+3*Ms,t+3*Ms,psx::MouseControl::Right,false,false);
    policy.sample(t+3*Ms,x,y);
    CHECK(policy.captured() && active && x==250 && y==64);
    CHECK(hold_presses==conflict_presses && hold_releases==conflict_releases);

    // A physical hold already down before acquisition is also unaccepted.
    policy.reset();host.hold_conflict=false;
    auto unfocused=host;unfocused.focused=false;
    policy.update(t+4*Ms,unfocused);
    policy.control(t+4*Ms,t+4*Ms,psx::MouseControl::Right,true,false);
    CHECK(!policy.captured());
    CHECK(acquire());
    const unsigned blocked_presses=hold_presses, blocked_releases=hold_releases;
    policy.motion(t+Ms,t+Ms,30,0);CHECK(active && !held);
    policy.control(t+2*Ms,t+2*Ms,psx::MouseControl::Right,false,false);
    CHECK(policy.captured() && active && !held);
    CHECK(hold_presses==blocked_presses && hold_releases==blocked_releases);
    policy.control(t+3*Ms,t+3*Ms,psx::MouseControl::Right,true,false);
    CHECK(held && !active && hold_presses==blocked_presses+1);
    policy.motion(t+4*Ms,t+4*Ms,30,0);CHECK(active && held);
    policy.control(UINT64_MAX,0,psx::MouseControl::Right,false,false);
    CHECK(!active && !held && hold_releases==blocked_releases+1);

    // If an accepted hold becomes conflicting, release it exactly once.
    policy.reset();CHECK(acquire());
    policy.control(t+Ms,t+Ms,psx::MouseControl::Right,true,false);
    policy.motion(t+2*Ms,t+2*Ms,30,0);CHECK(held && active);
    const unsigned accepted_releases=hold_releases;
    auto conflicting=host;conflicting.hold_conflict=true;
    policy.update(t+3*Ms,conflicting);
    CHECK(!held && !active && hold_releases==accepted_releases+1);
    policy.motion(t+4*Ms,t+4*Ms,30,0);CHECK(active && !held);
    policy.control(t+5*Ms,t+5*Ms,psx::MouseControl::Right,false,false);
    CHECK(active && hold_releases==accepted_releases+1);

    // Activation belongs only to LEFT, and lasts only through its release.
    policy.reset();host.hold_conflict=false;
    policy.control(t+6*Ms,t+6*Ms,psx::MouseControl::Right,false,false);
    for (auto button : {psx::MouseControl::Middle, psx::MouseControl::Right,
                        psx::MouseControl::X1, psx::MouseControl::X2}) {
        CHECK(!policy.control(t+7*Ms,t+7*Ms,button,true,false));
        CHECK(!policy.captured());
        policy.control(t+8*Ms,t+8*Ms,button,false,false);
    }
    const unsigned normal_notices=notices;
    CHECK(acquire());CHECK(!active);
    policy.motion(t+Ms,t+Ms,30,0);CHECK(active);
    CHECK(policy.control(UINT64_MAX,0,psx::MouseControl::Left,false,false));
    CHECK(!policy.captured() && !active && !held);
    x=99;y=77;policy.sample(t+Ms,x,y);CHECK(x==99 && y==77);
    CHECK(policy.control(t+2*Ms,t+2*Ms,psx::MouseControl::Left,true,false));
    policy.sample(t+2*Ms,x,y);CHECK(x==128 && y==128); // rapid re-press has no stale motion
    policy.motion(t+3*Ms,t+3*Ms,30,0);CHECK(active);
    policy.control(t+4*Ms,t+4*Ms,psx::MouseControl::Right,true,false);CHECK(held && !active);
    policy.motion(t+5*Ms,t+5*Ms,30,0);CHECK(held && active);
    CHECK(policy.control(t+6*Ms,t+6*Ms,psx::MouseControl::Left,false,false));
    CHECK(!policy.captured() && !active && !held); // RIGHT can never outlive LEFT
    CHECK(policy.control(t+7*Ms,t+7*Ms,psx::MouseControl::Left,true,false));
    CHECK(!held && !active); // RIGHT remains physically down, so requires a fresh edge
    policy.motion(t+8*Ms,t+8*Ms,30,0);CHECK(active && !held);
    policy.control(t+9*Ms,t+9*Ms,psx::MouseControl::Right,false,false);
    CHECK(active);CHECK(policy.control(t+10*Ms,t+10*Ms,psx::MouseControl::Left,false,false));
    CHECK(notices==normal_notices);

    // A press while ineligible cannot activate later merely because eligibility returns.
    auto absent=host;absent.focused=false;
    policy.update(t+11*Ms,absent);
    CHECK(!policy.control(t+11*Ms,t+11*Ms,psx::MouseControl::Left,true,false));
    policy.update(t+12*Ms,host);
    CHECK(!policy.control(t+12*Ms,t+12*Ms,psx::MouseControl::Left,true,false));
    CHECK(!policy.captured());
    policy.control(t+13*Ms,t+13*Ms,psx::MouseControl::Left,false,false);
    CHECK(policy.control(t+14*Ms,t+14*Ms,psx::MouseControl::Left,true,false));
    CHECK(policy.captured() && !active);
    CHECK(policy.control(t+15*Ms,t+15*Ms,psx::MouseControl::Left,false,false));
    t+=20*Ms;

    // SDL may drain several ordered edges after their physical timestamps.
    // A normal release must not reject a later re-press in that same drain.
    for (uint64_t delay : {10*Ms, 50*Ms, 200*Ms}) {
        psx::LocalMousePolicy batch({nullptr,capture,suppress,notice});
        CHECK(batch.install(&callbacks));
        const uint64_t base=1000*Ms+delay*10;
        batch.update(base+10*Ms,host);
        CHECK(batch.control(base+10*Ms,base+10*Ms,psx::MouseControl::Left,true,false));
        batch.motion(base+15*Ms,base+15*Ms,30,0);
        batch.control(base+16*Ms,base+16*Ms,psx::MouseControl::Right,true,false);
        batch.motion(base+17*Ms,base+17*Ms,30,0); CHECK(active && held);
        batch.update(base+20*Ms+delay,host);
        CHECK(batch.control(base+20*Ms,base+20*Ms+delay,psx::MouseControl::Left,false,false));
        CHECK(!batch.captured() && !active && !held);
        x=99;y=77;batch.sample(base+20*Ms+delay,x,y);CHECK(x==99 && y==77);
        batch.update(base+21*Ms+delay,host);
        CHECK(batch.control(base+25*Ms,base+21*Ms+delay,psx::MouseControl::Left,true,false));
        CHECK(batch.captured() && !active && !held);
        batch.sample(base+21*Ms+delay,x,y);CHECK(x==128 && y==128);
        batch.motion(base+26*Ms,base+22*Ms+delay,30,0);CHECK(active && !held);
        batch.control(base+27*Ms,base+23*Ms+delay,psx::MouseControl::Right,false,false);
        CHECK(active && !held); // A physically held RIGHT cannot latch or cancel this flick.
        CHECK(batch.control(base+28*Ms,base+24*Ms+delay,psx::MouseControl::Left,false,false));
        CHECK(!batch.captured() && !active && !held);
        batch.clear();
    }

    // A release after an interruption cannot lower its processing-time barrier.
    for (int gate=0;gate<4;++gate) {
        psx::LocalMousePolicy batch({nullptr,capture,suppress,notice});
        CHECK(batch.install(&callbacks));
        const uint64_t base=5000*Ms+gate*1000*Ms;
        batch.update(base+10*Ms,host);
        CHECK(batch.control(base+10*Ms,base+10*Ms,psx::MouseControl::Left,true,false));
        batch.motion(base+15*Ms,base+15*Ms,30,0);
        auto off=host;const uint64_t barrier=base+(gate==3?300:30)*Ms;
        if(gate==0)off.focused=false;if(gate==1)guest=false;if(gate==2)off.native_right=true;
        batch.update(barrier,off);CHECK(!batch.captured() && !active);
        guest=true;batch.update(barrier+Ms,host);
        CHECK(batch.control(base+20*Ms,barrier+2*Ms,psx::MouseControl::Left,false,false));
        CHECK(!batch.control(base+25*Ms,barrier+3*Ms,psx::MouseControl::Left,true,false));
        CHECK(!batch.captured() && !active && !held);
        batch.control(barrier+4*Ms,barrier+4*Ms,psx::MouseControl::Left,false,false);
        batch.update(barrier+5*Ms,host);
        CHECK(batch.control(barrier+5*Ms,barrier+5*Ms,psx::MouseControl::Left,true,false));
        batch.sample(barrier+5*Ms,x,y);CHECK(x==128 && y==128);
        batch.motion(barrier+6*Ms,barrier+6*Ms,30,0);CHECK(active);
        batch.clear();
    }

    // Malformed releases are still immediate, but cannot admit a queued press.
    for (uint64_t bad : {UINT64_MAX, uint64_t(14*Ms), uint64_t(0)}) {
        psx::LocalMousePolicy batch({nullptr,capture,suppress,notice});
        CHECK(batch.install(&callbacks));batch.update(10*Ms,host);
        CHECK(batch.control(10*Ms,10*Ms,psx::MouseControl::Left,true,false));
        batch.motion(15*Ms,15*Ms,30,0);batch.update(30*Ms,host);
        CHECK(batch.control(bad,30*Ms,psx::MouseControl::Left,false,false));
        CHECK(!batch.captured() && !active && !held);
        batch.update(31*Ms,host);
        CHECK(!batch.control(25*Ms,31*Ms,psx::MouseControl::Left,true,false));
        CHECK(!batch.captured());
        batch.control(32*Ms,32*Ms,psx::MouseControl::Left,false,false);
        batch.update(33*Ms,host);
        CHECK(batch.control(33*Ms,33*Ms,psx::MouseControl::Left,true,false));
        batch.sample(33*Ms,x,y);CHECK(x==128 && y==128);batch.clear();
    }

    // LEFT released while unfocused is never delivered; reconcile on regain so
    // the next click activates. A still-held button is not a new activation.
    {
        psx::LocalMousePolicy lost({nullptr,capture,suppress,notice});
        CHECK(lost.install(&callbacks));
        const uint64_t base=9000*Ms;
        lost.update(base,host);
        CHECK(lost.control(base,base,psx::MouseControl::Left,true,false) && lost.captured());
        auto away=host;away.focused=false;
        lost.update(base+10*Ms,away);CHECK(!lost.captured());
        lost.update(base+20*Ms,host);
        lost.sync_left(base+20*Ms,true);
        CHECK(!lost.control(base+21*Ms,base+21*Ms,psx::MouseControl::Left,true,false));
        lost.sync_left(base+22*Ms,false);
        lost.update(base+23*Ms,host);
        CHECK(lost.control(base+23*Ms,base+23*Ms,psx::MouseControl::Left,true,false));
        CHECK(lost.captured());
        lost.sync_left(base+24*Ms,true);CHECK(lost.captured()); // down: no-op
        lost.clear();
    }

    policy.reset();host.hold_conflict=false;capture_ok=false;CHECK(acquire());CHECK(!policy.captured());
    const unsigned failure_notices=notices;CHECK(acquire());CHECK(notices==failure_notices);
    policy.clear(); CHECK(!policy.installed() && !policy.captured());
    callbacks.struct_size=0;CHECK(!policy.install(&callbacks));
    std::printf("ordered local mouse policy safety: %u acquisitions, %u releases, %u resets\n",captures,releases,resets);
    return 0;
}
