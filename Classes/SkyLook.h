//  SkyLook.h — Stage DN.3 (WORKING/day-night-cycle-plan-2026-10-31.md §5): the cycle's sky look.
//  Keyframed gradient + horizon glow + pixel stars, all inside Terrain::render's ortho sky pass.
//  Only reached while Experiments > Day/night cycle is on (SkyCycle::enabled()).
#ifndef SkyLook_h
#define SkyLook_h

namespace SkyLook {
    // Inside the sky pass's ortho bracket (extent w x h, far plane zfar): the gradient (writes depth like
    // the stock sky), the glow and the stars (blended, no depth write). The sun and moon follow
    // (SkyCycle::drawSunMoon).
    void drawSky(float w, float h, float zfar);
    // The world fog colour for the current time: the keyframe's horizon colour - 0.03 (as the stock fog).
    void fogColor(float out[4]);
}

#endif
