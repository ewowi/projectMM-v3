// LightCrafter16: eight columns down, then the same eight back up, one continuous run.

class LightCrafter16Layout {
  int ledsPerPin = 10;
  byte pinsAreColumns = 0;

  int dimensions() { return 2; }

  string tags() { return "🚥"; }

  void defineControls() {
    addControl("ledsPerPin", ledsPerPin, 1, 2047); // lights on each of the sixteen strips
    addControl("pinsAreColumns", pinsAreColumns, 0, 1); // swap which axis the strips run along
  }

  // One strip along `column`, running from `from` toward `to`.
  void strip(int column, int from, int to) {
    int dir = 1;
    if (from > to) { dir = 0 - 1; }
    int y = from;
    for (int n = 0; n < ledsPerPin; n = n + 1) {
      if (pinsAreColumns == 1) { addLight(column, y, 0); } else { addLight(y, column, 0); }
      y = y + dir;
    }
  }

  void placeLights() {
    // Out along columns 0..7 with each strip running backwards, then home along 7..0 running
    // forwards: the boustrophedon a single cable makes when it has to end where it started.
    for (int c = 0; c < 8; c = c + 1) { strip(c, ledsPerPin - 1, 0); }
    for (int n = 0; n < 8; n = n + 1) {
      int c = 7 - n;
      strip(c, ledsPerPin, ledsPerPin * 2 - 1);
    }
  }
}
