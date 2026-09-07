// SE16: sixteen strips in a grid, wired in the pair order that board's connectors use.

class Se16Layout {
  int ledsPerPin = 10;
  byte mirrored = 0;
  byte pinsAreColumns = 0;

  int dimensions() { return 2; }

  string tags() { return "🚥"; }

  void defineControls() {
    addControl("ledsPerPin", ledsPerPin, 1, 2047); // lights on each of the sixteen strips
    addControl("mirrored", mirrored, 0, 1); // 8 columns of two strips running opposite ways
    addControl("pinsAreColumns", pinsAreColumns, 0, 1); // swap which axis the strips run along
  }

  // One strip along `column`, running from `from` toward `to`. A return leg is the same call with
  // its ends swapped, which is what the wiring does at the far end of a column.
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
    if (mirrored == 1) {
      // Eight columns, each carrying an outward and a return strip: the far end folds back.
      for (int n = 0; n < 8; n = n + 1) {
        int c = 7 - n;
        strip(c, ledsPerPin, ledsPerPin * 2 - 1);
        strip(c, ledsPerPin - 1, 0);
      }
    } else {
      // Sixteen columns taken in swapped pairs, which is the order the connectors sit in.
      for (int n = 0; n < 8; n = n + 1) {
        int p = 7 - n;
        strip(p * 2, 0, ledsPerPin - 1);
        strip(p * 2 + 1, 0, ledsPerPin - 1);
      }
    }
  }
}
