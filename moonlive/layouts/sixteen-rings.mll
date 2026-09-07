// Sixteen rings scattered across a wall, each a circle of lights at its own spot.

class SixteenRingsLayout {
  byte ringLights = 24;
  byte radius = 5;
  byte spread = 1;

  int dimensions() { return 2; }

  string tags() { return "🚥"; }

  void defineControls() {
    addControl("ringLights", ringLights, 3, 255); // lights around each ring
    addControl("radius", radius, 1, 60); // how wide a ring is
    addControl("spread", spread, 1, 10); // spreads the whole arrangement out
  }

  // One ring at a spot on the wall.
  void ring(int cx, int cy) {
    for (int i = 0; i < ringLights; i = i + 1) {
      addLight(cx * spread + scale(cos(i * turn(ringLights)), radius * 2 + 1),
               cy * spread + scale(sin(i * turn(ringLights)), radius * 2 + 1), 0);
    }
  }

  void placeLights() {
    // The centers are hand-placed rather than laid out on a grid: this is one physical
    // installation, and these are where its rings actually hang.
    ring(59, 23);
    ring(70, 28);
    ring(59, 10);
    ring(59, 34);
    ring(47, 17);
    ring(59, 46);
    ring(41, 5);
    ring(47, 39);
    ring(35, 17);
    ring(41, 53);
    ring(22, 10);
    ring(35, 39);
    ring(22, 23);
    ring(22, 46);
    ring(10, 28);
    ring(22, 34);
  }
}
