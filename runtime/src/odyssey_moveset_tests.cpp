#include "nebula_odyssey_moveset.h"

#include <cmath>
#include <cstring>
#include <iostream>

namespace {

using namespace galaxy::moveset;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "odyssey_moveset_tests: " << message << '\n';
    }
    return condition;
}

bool near(float a, float b, float tolerance = 1e-3f) {
    return std::fabs(a - b) <= tolerance;
}

// Mario on flat ground, gravity down -y, facing +z. Galaxy's run is 13 units per frame.
MarioView standing() {
    MarioView v;
    v.eligible = true;
    v.grounded = true;
    v.down = {0.0f, -1.0f, 0.0f};
    v.front = {0.0f, 0.0f, 1.0f};
    return v;
}

MarioView running() {
    MarioView v = standing();
    v.walk_speed = 1.0f;
    v.stick_mag = 1.0f;
    return v;
}

MarioView crouching() {
    MarioView v = running();
    v.squat = true;
    v.lvl_z = true;
    return v;
}

MarioView airborne() {
    MarioView v = standing();
    v.grounded = false;
    v.jumping = true;
    v.jump_vec = {0.0f, 10.0f, 5.0f};
    return v;
}

float units(const MarioEdit& edit) { return edit.walk_speed * 13.0f; }

// The same view with the cap button let go.
MarioView again_view(MarioView v) {
    v.trig_c = false;
    return v;
}

// Starts a crouch roll and returns the edit of its first frame.
MarioEdit crouch_roll(Moveset& m) {
    MarioView press = crouching();
    press.trig_c = true;
    return m.step(press);
}

bool test_crouch_roll() {
    bool passed = true;
    Moveset m;
    // C without a crouch does nothing on the ground; Z alone is Galaxy's crouch slide.
    MarioView run = running();
    run.trig_c = true;
    passed &= expect(!m.step(run).set_walk_speed && !m.rolling(), "C while running is not a roll");
    MarioView z = running();
    z.trig_z = true;
    z.lvl_z = true;
    passed &= expect(!m.step(z).set_walk_speed && !m.rolling(), "Z while running is Galaxy's crouch slide");
    const MarioEdit edit = crouch_roll(m);
    passed &= expect(m.rolling() && edit.anim == Anim::RollStart && edit.dust_puff &&
                         near(units(edit), kRollCrouchSpeed),
                     "crouch and C rolls at 20 units per frame");
    // It keeps going while Z is held, losing 0.2% a frame.
    MarioView held = crouching();
    const MarioEdit next = m.step(held);
    passed &= expect(next.set_walk_speed && near(units(next), kRollCrouchSpeed * kRollDecay),
                     "the roll loses 0.2% a frame");
    // From a standstill crouch too.
    Moveset s;
    MarioView still = crouching();
    still.walk_speed = 0.0f;
    still.stick_mag = 0.0f;
    still.trig_c = true;
    passed &= expect(near(units(s.step(still)), kRollCrouchSpeed), "a crouch roll from a standstill");
    return passed;
}

bool test_roll_boost() {
    bool passed = true;
    Moveset m;
    (void)crouch_roll(m);
    MarioView held = crouching();
    MarioView boost = held;
    boost.trig_c = true;
    // Too soon: nothing.
    for (int i = 0; i < kRollBoostWait - 2; ++i) (void)m.step(held);
    passed &= expect(!m.step(boost).air_gust, "a boost before 16 frames does nothing");
    (void)m.step(held);
    MarioEdit edit = m.step(boost);
    passed &= expect(edit.air_gust && edit.anim == Anim::None && near(units(edit), kRollBoostWhite),
                     "the first boost of a crouch roll is the white one, 23, with a gust and no new start");
    for (int i = 0; i < kRollBoostWait - 1; ++i) (void)m.step(held);
    edit = m.step(boost);
    passed &= expect(edit.air_gust && near(units(edit), kRollBoostYellow), "the next is yellow, 26");
    // A ground-pound roll's first boost is yellow, after 32 frames.
    Moveset p;
    MarioView landed = standing();
    landed.jumping = true;
    landed.hip_drop = true;
    (void)p.step(landed);
    landed.trig_c = true;
    landed.lvl_z = true;
    (void)p.step(landed);
    bool early = false;
    for (int i = 0; i < kRollBoostWait; ++i) early |= p.step(boost).air_gust;
    passed &= expect(!early, "a ground-pound roll cannot boost in its first 16 frames");
    for (int i = kRollBoostWait; i < kRollBoostWaitPound - 1; ++i) (void)p.step(held);
    edit = p.step(boost);
    passed &= expect(edit.air_gust && units(edit) >= kRollBoostYellow - 1e-3f,
                     "a ground-pound roll's boost waits 32 frames and is yellow");
    return passed;
}

bool test_roll_ends() {
    bool passed = true;
    // A ground-pound roll (30, so it has not slowed to a crouch by the time Z may be let go).
    Moveset m;
    MarioView landed = standing();
    landed.jumping = true;
    landed.hip_drop = true;
    (void)m.step(landed);
    landed.trig_c = true;
    landed.lvl_z = true;
    (void)m.step(landed);
    MarioView let_go = crouching();
    let_go.lvl_z = false;
    for (int i = 0; i < kRollHoldPound - 2; ++i) (void)m.step(let_go);
    passed &= expect(m.rolling(), "a roll goes on for a while even without Z");
    MarioEdit up;
    for (int i = 0; i < 3; ++i) up = m.step(let_go);
    passed &= expect(up.anim == Anim::RollEnd && !m.rolling(), "then letting go of Z gets him up");
    // Slowing under 17 leaves him crouching (no get-up).
    Moveset s;
    (void)crouch_roll(s);
    MarioView held = crouching();
    MarioEdit edit;
    int frames = 0;
    while (s.rolling() && frames < 1000) {
        edit = s.step(held);
        ++frames;
    }
    passed &= expect(!s.rolling() && edit.anim != Anim::RollEnd && std::strcmp(s.roll_end_reason(), "slowed to a crouch") == 0,
                     "a roll that slows below 17 is a crouch again");
    // A roll turns wide: at most 5 degrees a frame, whatever Galaxy did to his facing.
    Moveset turn;
    (void)crouch_roll(turn);
    (void)turn.step(held);
    MarioView swung = held;
    swung.front = {1.0f, 0.0f, 0.0f};
    const MarioEdit limited = turn.step(swung);
    passed &= expect(limited.set_front && near(limited.front.x, std::sin(5.0f * 3.14159265f / 180.0f), 1e-3f),
                     "a roll turns at most 5 degrees a frame");
    // The roll cancel: the spin, after 9 frames, keeps the speed.
    Moveset rc;
    MarioView fast = crouching();
    fast.trig_c = true;
    fast.walk_speed = 2.0f;  // 26
    (void)rc.step(fast);
    MarioView spin_now = held;
    spin_now.spin_request = true;
    passed &= expect(rc.step(spin_now).anim != Anim::RollEnd && rc.rolling(), "no roll cancel straight away");
    for (int i = 0; i < kRollCancelWait; ++i) (void)rc.step(held);
    (void)rc.step(spin_now);
    passed &= expect(!rc.rolling() && std::strcmp(rc.roll_end_reason(), "roll cancel") == 0 &&
                         rc.fast_run_speed() > kRunCap,
                     "the spin cancels the roll and keeps its speed");
    MarioView spinning = running();  // Galaxy's spin is a state of its own
    spinning.eligible = false;
    for (int i = 0; i < 30; ++i) (void)rc.step(spinning);
    passed &= expect(rc.fast_run_speed() > kRunCap, "the speed waits out the spin");
    const MarioEdit after_spin = rc.step(running());
    passed &= expect(after_spin.set_walk_speed && units(after_spin) > kRunCap && !rc.rolling(),
                     "and he runs on at it afterwards, out of the roll");
    // A wall bumps him off.
    Moveset w;
    (void)crouch_roll(w);
    MarioView wall = held;
    wall.wall_hit = true;
    const MarioEdit bumped = w.step(wall);
    passed &= expect(!w.rolling() && bumped.knock_back && bumped.knock_back_dir.z < 0.0f &&
                         (bumped.sounds & kSoundBump) != 0u,
                     "rolling into a wall knocks him back off it");
    // A bump does not end it; a fall does.
    Moveset b;
    (void)crouch_roll(b);
    MarioView bump = held;
    bump.grounded = false;
    for (int i = 0; i < kRollAirFrames; ++i) (void)b.step(bump);
    passed &= expect(b.rolling(), "a bump does not end the roll");
    for (int i = 0; i < 3; ++i) (void)b.step(bump);
    passed &= expect(!b.rolling(), "falling off a ledge ends it");
    // A skid ends it without forcing a speed against the game's brake.
    Moveset k;
    (void)crouch_roll(k);
    MarioView skid = held;
    skid.braking = true;
    passed &= expect(!k.step(skid).set_walk_speed && !k.rolling(), "a skid ends the roll");
    // The animation loops; dust follows.
    Moveset a;
    (void)crouch_roll(a);
    MarioView finished = held;
    finished.roll_anim_running = false;
    passed &= expect(a.step(finished).anim == Anim::Roll, "the roll animation loops");
    bool dust = false;
    for (int i = 0; i < kRollDustEvery; ++i) dust |= a.step(held).dust_puff;
    passed &= expect(dust, "dust while rolling");
    return passed;
}

bool test_roll_jump() {
    bool passed = true;
    Moveset m;
    (void)crouch_roll(m);
    MarioView held = crouching();
    for (int i = 0; i < kRollJumpMinFrames; ++i) (void)m.step(held);
    MarioView jump = airborne();
    jump.jump_type = kSquatJumpType;
    jump.jump_vec = {0.0f, 15.0f, 16.5f};
    jump.lvl_z = true;
    const MarioEdit edit = m.step(jump);
    passed &= expect(edit.set_jump_vec && edit.set_jump_type && edit.jump_type == kNormalJumpType &&
                         edit.anim == Anim::RollJump && edit.cancel_squat,
                     "A in a roll is a roll jump");
    passed &= expect(near(edit.jump_vec.y, 22.0f * kRollJumpHeightScale) && edit.jump_vec.z > kRollMinSpeed,
                     "a little higher than the first jump, at the roll's speed");
    MarioView falling = jump;
    falling.jump_type = kNormalJumpType;
    passed &= expect(m.step(falling).anim == Anim::None, "nothing more in the air");
    const MarioEdit land = m.step(standing());
    passed &= expect(land.anim == Anim::RollLand && land.dust_landing, "its own landing, with dust");
    // Z then A at once is the long jump.
    Moveset quick;
    (void)crouch_roll(quick);
    passed &= expect(!quick.step(jump).set_jump_vec, "Z then A at once is Galaxy's long jump");
    return passed;
}

bool test_ground_pound_jump_and_roll() {
    bool passed = true;
    Moveset m;
    MarioView pounding = standing();  // the ground pound's landing
    pounding.jumping = true;
    pounding.hip_drop = true;
    (void)m.step(pounding);
    MarioView jump = airborne();
    jump.jump_vec = {0.0f, 22.0f, 0.0f};
    const MarioEdit edit = m.step(jump);
    passed &= expect(edit.set_jump_vec && edit.jump_vec.y > 36.0f && edit.jump_type == kHighJumpType &&
                         edit.anim == Anim::HighJump,
                     "A as a ground pound lands is the ground-pound jump");
    Moveset p;
    MarioView landed = standing();
    landed.jumping = true;
    landed.hip_drop = true;
    (void)p.step(landed);
    landed.trig_c = true;
    MarioView no_z = landed;
    passed &= expect(!p.step(no_z).end_hip_drop, "C without Z as the pound lands is not a roll");
    landed.lvl_z = true;
    const MarioEdit roll = p.step(landed);
    passed &= expect(p.rolling() && roll.end_hip_drop && roll.anim == Anim::GroundPoundRoll &&
                         near(units(roll), kRollPoundSpeed) && roll.block_crouch,
                     "Z held and C as a ground pound lands rolls at 30");
    MarioView rolling = crouching();
    passed &= expect(p.step(rolling).set_walk_speed && p.rolling(), "and keeps rolling");
    passed &= expect(p.step(airborne()).anim == Anim::RollJump, "A out of it is a roll jump at once");
    return passed;
}

bool test_dive() {
    bool passed = true;
    Moveset m;
    MarioView jump = airborne();
    jump.trig_c = true;
    const MarioEdit edit = m.step(jump);
    const float rise = std::sqrt(2.0f * 1.8f * kDiveRise);
    passed &= expect(edit.set_jump_vec && m.diving() && edit.anim == Anim::Dive && !edit.hard_landing &&
                         edit.end_jump_hold && edit.jump_type == kNormalJumpType,
                     "C in the air dives");
    passed &= expect(edit.attack_frames > 0, "the dive hits enemies as a spin does");
    passed &= expect(edit.cancel_squat, "a dive leaves the long jump's crouch");
    Moveset h;
    (void)h.step(jump);
    MarioView replaced = again_view(jump);
    replaced.dive_anim_running = false;
    passed &= expect(h.step(replaced).anim == Anim::DiveHold, "the dive's pose holds until he lands");
    passed &= expect(near(edit.jump_vec.z, kDiveSpeed) && near(edit.jump_vec.y, rise, 0.01f) &&
                         near(edit.jump_vec.x, 0.0f),
                     "forward at 20, rising 182 units");
    const MarioEdit again = m.step(jump);
    passed &= expect(!again.set_jump_vec, "one dive per jump");
    passed &= expect(again.block_air_actions, "no ground pound out of a dive");
    MarioView spin_cancel = jump;
    spin_cancel.trig_c = false;
    spin_cancel.spin_request = true;
    passed &= expect(m.step(spin_cancel).block_air_actions && m.diving(),
                     "a spin input left over from just before the dive does not cancel it");
    for (int i = 0; i < kDiveSpinGuard; ++i) (void)m.step(again_view(jump));
    const MarioEdit spun = m.step(spin_cancel);
    passed &= expect(!spun.block_air_actions && spun.clear_hard_landing && !m.diving(), "the spin cancels a dive");
    (void)m.step(standing());
    (void)m.step(jump);
    // Towards the stick, whatever way he faced.
    Moveset t;
    MarioView turned = jump;
    turned.stick_mag = 1.0f;
    turned.stick_dir = {1.0f, 0.0f, 0.0f};
    const MarioEdit steered = t.step(turned);
    passed &= expect(steered.face_stick && near(steered.jump_vec.x, kDiveSpeed) && near(steered.jump_vec.z, 0.0f),
                     "the dive goes where the stick points");
    // Out of a ground pound: the pound ends in the air.
    Moveset g;
    MarioView pound = airborne();
    pound.hip_drop = true;
    pound.trig_c = true;
    const MarioEdit from_pound = g.step(pound);
    passed &= expect(from_pound.anim == Anim::Dive && from_pound.end_hip_drop_air, "ground pound then C dives");
    MarioView after_pound = airborne();  // the next frame: out of the pound, rising a little
    after_pound.jump_vec = {0.0f, 11.0f, 20.0f};
    passed &= expect(!g.step(after_pound).set_jump_vec, "the dive is not taken for a ground-pound jump");
    passed &= expect(from_pound.jump_vec.y < std::sqrt(2.0f * 1.8f * kPoundDiveRise) + 0.01f,
                     "out of a ground pound it only lifts a little");
    // Landing with Z held rolls on; without it, the belly slide (Galaxy's), with dust.
    MarioView flying = airborne();
    flying.jump_vec = {0.0f, -5.0f, 24.0f};
    (void)m.step(flying);
    MarioView land = crouching();
    const MarioEdit rolled = m.step(land);
    passed &= expect(m.rolling() && rolled.anim == Anim::RollStart && near(units(rolled), 24.0f) &&
                         rolled.dust_puff,
                     "a dive landing with Z held rolls at its speed");
    // The game may still say "jumping" on the landing frame.
    Moveset g2;
    (void)g2.step(jump);
    (void)g2.step(flying);
    MarioView touchdown = crouching();
    touchdown.jumping = true;
    (void)g2.step(touchdown);
    passed &= expect(g2.rolling(), "a dive rolls on even when the landing frame still says jumping");
    MarioView zeroed = crouching();  // Galaxy's belly landing zeroed his speed
    zeroed.walk_speed = 0.0f;
    const MarioEdit kept = g2.step(zeroed);
    passed &= expect(g2.rolling() && kept.set_walk_speed && units(kept) > kRollCrouchSpeed - 1.0f,
                     "and keeps the dive's speed through Galaxy's landing");
    Moveset n;
    (void)n.step(jump);
    (void)n.step(flying);
    MarioView touch = standing();  // Galaxy still says jumping on the landing frame
    touch.jumping = true;
    const MarioEdit belly = n.step(touch);
    passed &= expect(!n.rolling() && belly.dust_puff && belly.set_walk_speed && near(units(belly), 24.0f),
                     "without Z he slides on at the dive's speed");
    const MarioEdit tumble = n.step(standing());
    passed &= expect(tumble.anim == Anim::DiveLand, "tumbling up once Galaxy's own landing is done");
    const MarioEdit sliding = n.step(standing());
    passed &= expect(sliding.set_walk_speed && units(sliding) < 24.0f && units(sliding) > 20.0f,
                     "which eases to a stop");
    bool stopped = false;
    for (int i = 0; i < kBellySlideFrames; ++i) stopped |= n.step(standing()).stop_belly_slide;
    passed &= expect(stopped, "and he gets up after it");
    // The cap button during the belly slide rolls out of it, without Z.
    Moveset c;
    (void)c.step(jump);
    (void)c.step(flying);
    (void)c.step(standing());
    MarioView cap = standing();
    cap.trig_c = true;
    const MarioEdit out = c.step(cap);
    passed &= expect(c.rolling() && out.stop_belly_slide && out.anim == Anim::RollStart && units(out) > 20.0f,
                     "the cap button rolls out of the belly slide at the dive's speed");
    // Not in a spin or a wall slide; switched off.
    Moveset s;
    MarioView spin = jump;
    spin.jump_type = kSpinJumpType;
    MarioView spin_held = spin;  // the spin's input is still live on the frame of the dive
    spin_held.spin_request = true;
    const MarioEdit out_of_spin = s.step(spin_held);
    passed &= expect(out_of_spin.anim == Anim::Dive && out_of_spin.block_air_actions,
                     "a dive out of a spin jump, with no second spin on its frame");
    MarioView spinning = spin;
    spinning.trig_c = false;
    spinning.spin_request = true;
    (void)s.step(spinning);
    passed &= expect(s.step(spin).anim != Anim::Dive, "not out of a spin jump's air spin: it was spent");
    Moveset f;
    MarioView plain = jump;  // an ordinary jump
    passed &= expect(f.step(plain).anim == Anim::Dive, "dive");
    MarioView air_spin = plain;
    air_spin.trig_c = false;
    air_spin.spin_request = true;
    for (int i = 0; i < kDiveSpinGuard; ++i) (void)f.step(again_view(plain));
    (void)f.step(air_spin);
    passed &= expect(f.step(plain).anim == Anim::Dive, "a fresh air spin, then another dive");
    for (int i = 0; i < kDiveSpinGuard; ++i) (void)f.step(again_view(plain));
    (void)f.step(air_spin);
    passed &= expect(f.step(plain).anim != Anim::Dive, "a false spin does not give another");
    (void)f.step(standing());
    passed &= expect(f.step(plain).anim == Anim::Dive, "the ground resets it");
    Moveset off;
    Options none;
    none.dive = false;
    off.set_options(none);
    passed &= expect(!off.step(jump).set_jump_vec, "dive can be switched off");
    return passed;
}

bool test_no_roll_out_of_jumps() {
    bool passed = true;
    Moveset m;
    (void)m.step(running());
    MarioView jump = airborne();
    jump.jump_type = kSquatJumpType;
    jump.jump_vec = {0.0f, 15.0f, 16.5f};
    (void)m.step(jump);
    (void)m.step(crouching());
    passed &= expect(!m.rolling(), "landing from a long jump with Z held is Galaxy's crouch");
    Moveset n;
    (void)n.step(running());
    (void)n.step(airborne());
    (void)n.step(crouching());
    passed &= expect(!n.rolling(), "so is landing from a jump");
    return passed;
}

bool test_ineligible_mario_is_left_alone() {
    bool passed = true;
    Moveset m;
    MarioView press = crouching();
    press.trig_c = true;
    press.eligible = false;
    passed &= expect(!m.step(press).set_walk_speed && !m.rolling(), "no roll in a cutscene");
    Moveset n;
    (void)crouch_roll(n);
    MarioView lost = crouching();
    lost.eligible = false;
    passed &= expect(n.step(lost).stop_roll_anim && !n.rolling(), "losing control ends the roll");
    MarioView nan_view = airborne();
    nan_view.trig_c = true;
    nan_view.jump_vec.x = std::nanf("");
    passed &= expect(!n.step(nan_view).set_jump_vec, "a non-finite velocity is ignored");
    return passed;
}

bool test_effects() {
    bool passed = true;
    Moveset m;
    (void)m.step(standing());
    MarioView start = standing();
    start.walk_speed = 0.6f;
    start.stick_mag = 1.0f;
    passed &= expect(m.step(start).dust_puff, "a puff as he starts running");
    MarioView skid = running();
    skid.braking = true;
    passed &= expect(m.step(skid).dust_puff, "a puff as he skids");
    Moveset r;
    MarioView press = crouching();
    press.trig_c = true;
    const MarioEdit rolled = r.step(press);
    passed &= expect(rolled.set_trail && rolled.trail, "the trail starts with the roll");
    MarioView let_go = crouching();
    let_go.lvl_z = false;
    let_go.stick_mag = 0.0f;
    bool stopped = false;
    for (int i = 0; i < 100; ++i) {
        const MarioEdit e = r.step(let_go);
        stopped |= e.set_trail && !e.trail;
    }
    passed &= expect(stopped, "and stops when it is over");
    Moveset l;
    (void)l.step(running());
    MarioView long_jump = airborne();
    long_jump.jump_type = kSquatJumpType;
    passed &= expect(l.step(long_jump).air_gust, "a gust as a long jump sets off");
    return passed;
}

bool test_turn_boost() {
    bool passed = true;
    Moveset m;
    for (int i = 0; i < 300; ++i) passed &= expect(!m.step(running()).set_walk_speed, "an ordinary run is Galaxy's");
    MarioView skid = running();
    skid.braking = true;
    (void)m.step(skid);
    MarioEdit edit = m.step(running());
    passed &= expect(m.turn_boosting() && units(edit) > 13.0f, "a skid into a run the other way speeds up");
    for (int i = 0; i < 10; ++i) edit = m.step(running());
    passed &= expect(near(units(edit), kTurnBoostSpeed) && edit.set_run_anim &&
                         edit.run_anim == static_cast<std::uint8_t>(RunAnim::Dash),
                     "to 17, with the dash");
    MarioView jump = airborne();
    jump.stick_mag = 1.0f;
    passed &= expect(m.step(jump).anim == Anim::DashJump, "its first jump is the dash jump");
    bool blend = false;
    for (int i = 0; i < 200; ++i) {
        const MarioEdit e = m.step(running());
        blend |= e.set_run_anim && e.run_anim == static_cast<std::uint8_t>(RunAnim::Blend);
    }
    passed &= expect(!m.turn_boosting() && blend, "it slows back to the run and its animation");
    Moveset n;
    (void)n.step(skid);
    MarioView let_go = running();
    let_go.stick_mag = 0.2f;
    (void)n.step(let_go);
    passed &= expect(!n.turn_boosting(), "not when the stick is let go");
    return passed;
}

bool test_roll_speed_carries() {
    bool passed = true;
    Moveset m;
    MarioView press = crouching();
    press.trig_c = true;
    (void)m.step(press);
    MarioView held = crouching();
    MarioView boost = held;
    boost.trig_c = true;
    for (int i = 0; i < kRollBoostWait; ++i) (void)m.step(held);
    for (int i = 0; i < 3 * kRollBoostWait; ++i) (void)m.step(i % kRollBoostWait == 0 ? boost : held);
    MarioView up = running();
    for (int i = 0; i < 1000 && m.rolling(); ++i) (void)m.step(up);
    passed &= expect(!m.rolling() && m.fast_run_speed() > kDashFastAnimSpeed,
                     "standing up out of a fast roll keeps its speed");
    MarioView stood = up;  // Galaxy zeroed his speed as he stood up
    stood.walk_speed = 0.0f;
    (void)m.step(stood);
    passed &= expect(m.fast_run_speed() > kDashFastAnimSpeed, "the stand-up's zeroed speed does not end the fast run");
    const MarioEdit fast = m.step(up);
    passed &= expect(fast.set_walk_speed && fast.set_run_anim &&
                         fast.run_anim == static_cast<std::uint8_t>(RunAnim::DashFast),
                     "running at it with DashFast");
    bool dash = false;
    for (int i = 0; i < 200 && m.fast_run_speed() > 0.0f; ++i) {
        const MarioEdit e = m.step(up);
        dash |= e.set_run_anim && e.run_anim == static_cast<std::uint8_t>(RunAnim::Dash);
    }
    passed &= expect(dash && m.fast_run_speed() == 0.0f, "then the dash, then the run");
    // Footsteps while dashing; the roll slides and starts with its sound.
    Moveset f;
    MarioView skid = running();
    skid.braking = true;
    (void)f.step(skid);
    int steps = 0;
    for (int i = 0; i < 60; ++i) {
        const MarioEdit e = f.step(running());
        steps += ((e.sounds & kSoundStepLeft) != 0u) + ((e.sounds & kSoundStepRight) != 0u);
    }
    passed &= expect(steps >= 2, "footsteps while dashing");
    Moveset r;
    MarioView start = crouching();
    start.trig_c = true;
    passed &= expect((r.step(start).sounds & kSoundRollStart) != 0u, "a roll starts with a sound");
    passed &= expect((r.step(crouching()).sounds & kSoundRolling) != 0u, "and slides along the ground");
    return passed;
}

}  // namespace

int main() {
    bool passed = true;
    passed &= test_crouch_roll();
    passed &= test_roll_boost();
    passed &= test_roll_ends();
    passed &= test_roll_jump();
    passed &= test_ground_pound_jump_and_roll();
    passed &= test_dive();
    passed &= test_no_roll_out_of_jumps();
    passed &= test_ineligible_mario_is_left_alone();
    passed &= test_turn_boost();
    passed &= test_effects();
    passed &= test_roll_speed_carries();
    if (!passed) {
        return 1;
    }
    std::cout << "odyssey_moveset_tests: passed\n";
    return 0;
}
