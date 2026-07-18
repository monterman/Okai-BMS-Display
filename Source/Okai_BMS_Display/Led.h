// Led.h — shared types/prototypes for the dual NeoPixel status bars.
// Lives in a header so arduino-cli's auto-generated prototypes (injected at the
// top of the main sketch) can see LedRGB + Adafruit_NeoPixel. Impl is in Led.ino.
#pragma once
#include <Adafruit_NeoPixel.h>

struct LedRGB { uint8_t r, g, b; };

void ledInit();   // call once in setup() (right after the keep-alive task is running)
void ledLoop();   // call every loop() — non-blocking, ~40 fps
