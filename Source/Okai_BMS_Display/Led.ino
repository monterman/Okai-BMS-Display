// Led.ino — dual NeoPixel status bars for the Okai BMS Display
// 2× WS2812/SK6812, 12 px each. Strip 1 = GPIO10 (packs 1+2), Strip 2 = GPIO13 (packs 3+4).
// Layout per strip:  pack A [px0-4] | gap [px5-6 dark] | pack B [px7-10] | keep-alive [px11]
//   pack A local 0=OUTER(DIN) .. 4=INNER(gap). pack B mirrors: local 0->px10 .. 3->px7.
//   px11 (both strips) is the KEEP-ALIVE INDICATOR — owned by setKeepAliveIndicator(),
//   NOT part of any pack: white ~1.5 s blink = heartbeat healthy, solid red = stalled.
// Colorblind-safe (Okabe-Ito). Boot = twin white wipe (center->out) then pack-ID flash x3.
// Non-blocking: all timing via millis(); driven from ledLoop() in the main loop().
// Keep-alive is unaffected — it runs on its own Core-1 high-prio task; this only touches
// GPIO10/13 RMT and only READS g_hbLastMs (published by the heartbeat task).
// Design source: docs/2026-06-07-okai-led-bar-research.md + docs/led-bar-mock.html.

#include "Led.h"
#include "Config.h"
#include "OkaiBMS.h"

#define LED_PER_STRIP   12
#define LED_PACK_A_PX    5            // DIN-end pack: px0..4
#define LED_PACK_B_PX    4            // far-end pack: px10..7 (px11 reserved for keep-alive)
#define KA_PX           11            // keep-alive indicator pixel (both strips)
#define LED_BRIGHT      45            // master brightness (sun/pouch friendly, easy on 3V3)

// per-pack state thresholds
#define LED_TEMP_ALARM_C    55
// 2026-07-26 - Derived from the single source of truth in Config.h so the LEDs,
// the screen, the dashboard and the CSV can never disagree about what "WARN"
// means. Previously hard-coded to the old 50/100 mV pair.
#define LED_SPREAD_WARN_MV ((uint16_t)(CELL_DELTA_WARN_V * 1000.0f))
#define LED_SPREAD_POOR_MV ((uint16_t)(CELL_DELTA_POOR_V * 1000.0f))
#define LED_SOC_LOW_PCT     15
#define LED_STALE_MS      8000UL      // no fresh packet in 8s -> treat as no-response

static Adafruit_NeoPixel sLed1(LED_PER_STRIP, LED1_PIN, NEO_GRB + NEO_KHZ800);
static Adafruit_NeoPixel sLed2(LED_PER_STRIP, LED2_PIN, NEO_GRB + NEO_KHZ800);

// LedRGB struct lives in Led.h (so arduino-cli's auto-prototypes can see it).
static const LedRGB OK_GREEN ={0,158,115},   OK_SKY   ={86,180,233}, OK_YELLOW={240,228,66},
                    OK_VERM  ={213,94,0},    OK_ORANGE={230,159,0},  OK_BLUEA ={0,114,178},
                    OK_PURPB ={204,121,167}, OK_GREY  ={136,136,136}, OK_OFF  ={0,0,0};

static uint32_t sBootStart = 0;
static bool     sBootDone  = false;

static inline LedRGB dim(LedRGB c, float k){ return (LedRGB){ (uint8_t)(c.r*k), (uint8_t)(c.g*k), (uint8_t)(c.b*k) }; }
static inline LedRGB socColor(uint8_t soc){ return soc>=40 ? OK_GREEN : (soc>=LED_SOC_LOW_PCT ? OK_ORANGE : OK_VERM); }
static inline int    fillN(uint8_t soc, int cnt){ int n=(int)lroundf(soc/100.0f*cnt); if(soc>0 && n<1) n=1; if(n>cnt) n=cnt; return n; }

// map pack-local px -> strip px ; sideA=DIN end (0..4), sideB=far end (10..7)
static inline int packPx(bool sideA, int local){ return sideA ? local : (10 - local); }
static void setPack(Adafruit_NeoPixel& s, bool sideA, const LedRGB px[], int cnt){
    for(int i=0;i<cnt;i++){ int idx=packPx(sideA,i); s.setPixelColor(idx, px[i].r, px[i].g, px[i].b); }
}

// ---- keep-alive edge-pixel indicator (px11 both strips) ----
// Reads g_hbLastMs (heartbeat task). Cannot starve the heartbeat: this runs in the
// prio-1 loopTask; the heartbeat is prio-18 and preempts it.
static void setKeepAliveIndicator(uint32_t t){
    bool healthy = (millis() - g_hbLastMs) < HB_WATCHDOG_MS;
    LedRGB c;
    if(healthy){ bool on = (t % 1500) < 150; c = on ? (LedRGB){110,110,110} : OK_OFF; } // white ~1.5s blink
    else       { c = (LedRGB){180,0,0}; }                                               // solid red = stalled
    sLed1.setPixelColor(KA_PX, c.r, c.g, c.b);
    sLed2.setPixelColor(KA_PX, c.r, c.g, c.b);
}

// ---- boot animation (non-blocking): returns true when finished ----
// px5,6 (gap) and px11 (keep-alive) stay dark during boot.
static bool ledBoot(){
    uint32_t t = millis() - sBootStart;
    const uint32_t T1 = 700, FLASH = 420; const int NF = 3;
    if(t < T1){                                       // white wipe, center -> both ends
        float prog = (float)t / T1;
        for(int s=0;s<2;s++){ Adafruit_NeoPixel& strip = s ? sLed2 : sLed1;
            for(int i=0;i<12;i++){
                if(i==5||i==6||i==KA_PX){ strip.setPixelColor(i,0,0,0); continue; }
                float d = fabsf(i - 5.5f) / 5.5f;
                uint8_t v = (d <= prog) ? 180 : 0;
                strip.setPixelColor(i, v, v, v);
            }
        }
        sLed1.show(); sLed2.show(); return false;
    }
    uint32_t ft = t - T1;
    if(ft < FLASH*NF){                                // pack-ID color flash x3
        bool on = (ft % FLASH) < 240;
        for(int s=0;s<2;s++){ Adafruit_NeoPixel& strip = s ? sLed2 : sLed1;
            for(int i=0;i<12;i++){
                if(i==5||i==6||i==KA_PX){ strip.setPixelColor(i,0,0,0); continue; }
                LedRGB c = (i<5) ? OK_BLUEA : OK_PURPB;
                if(on) strip.setPixelColor(i, c.r, c.g, c.b); else strip.setPixelColor(i,0,0,0);
            }
        }
        sLed1.show(); sLed2.show(); return false;
    }
    return true;
}

// ---- per-pack live render into out[cnt] (local 0=outer .. cnt-1=inner) ----
static void renderPackLive(const PackData& p, uint32_t t, LedRGB out[], int cnt){
    for(int i=0;i<cnt;i++) out[i] = OK_OFF;
    if(!p.valid){                                                 // EMPTY -> dim-white edge markers
        LedRGB edge = {90,90,90};
        out[0] = edge; out[cnt-1] = edge;
        return;
    }

    // FET off / no response -> grey heartbeat blip every 2s
    bool stale       = (millis() - p.lastUpdateMs) > LED_STALE_MS;
    bool dischFetOff = !(p.rawStatus & 0x02);                     // bit1 = discharge FET on
    if(stale || dischFetOff){
        LedRGB g = ((t%2000) < 140) ? OK_GREY : dim(OK_GREY, 0.15f);
        for(int i=0;i<cnt;i++) out[i] = g; return;
    }
    // Over-temp override -> fast vermillion strobe-sweep
    if(p.maxTemp >= LED_TEMP_ALARM_C){
        int sw = (t/80) % cnt;
        for(int i=0;i<cnt;i++) out[i] = (i==sw) ? OK_VERM : dim(OK_VERM, 0.12f);
        return;
    }
    // Cell under-voltage override (bit4) -> vermillion double-blink "morse"
    if(p.rawStatus & 0x10){
        uint32_t ph = t % 1400; bool on = (ph<120) || (ph>=260 && ph<380);
        for(int i=0;i<cnt;i++) out[i] = on ? OK_VERM : OK_OFF;
        return;
    }

    uint8_t soc = p.soc; int n = fillN(soc, cnt); LedRGB base = socColor(soc);
    if(p.chargeDone){                                            // CHARGE COMPLETE -> green breath
        float k = 0.45f + 0.55f*0.5f*(1.0f - cosf((float)(t%2000)/2000.0f*2.0f*PI));
        for(int i=0;i<cnt;i++) out[i] = dim(OK_GREEN, k);
    } else if(p.isCharging || p.current > 0.3f){                 // CHARGING -> sky-blue up comet
        for(int i=0;i<n;i++) out[i] = dim(OK_SKY, 0.40f);
        if(n>0){ int h = (int)((t/400) % n); out[h] = OK_SKY; }
    } else if(p.current < -0.3f){                                // DISCHARGING -> BLUE down comet, speed=current
        float a = fabsf(p.current); if(a>15) a=15;
        uint32_t spd = (uint32_t)(700.0f - a*40.0f); if(spd<120) spd=120;
        for(int i=0;i<n;i++) out[i] = dim(OK_BLUEA, 0.40f);
        if(n>0){ int h = n-1 - (int)((t/spd) % n); if(h>=0) out[h] = OK_BLUEA; }
    } else if(soc < LED_SOC_LOW_PCT){                            // LOW SOC -> vermillion pulse
        float k = sinf((float)t/1000.0f*2.0f*PI)*0.5f + 0.5f;
        int m = (n<1)?1:n; for(int i=0;i<m;i++) out[i] = dim(OK_VERM, 0.2f + 0.8f*k);
    } else {                                                     // AT REST -> static SOC fill
        for(int i=0;i<n;i++) out[i] = base;
    }

    // Imbalance overlay on the inner two px — relative to pack size (out[cnt-2], out[cnt-1])
    // Rest-gated (Config.h) — a loaded pack's spread is not a health signal.
    uint16_t spread_mV = (uint16_t)(healthDelta(p) * 1000.0f + 0.5f);
    int hi = cnt-1, lo = (cnt >= 2) ? cnt-2 : 0;
    if(spread_mV >= LED_SPREAD_POOR_MV){ if((t%280) < 140){ out[lo]=OK_VERM;   out[hi]=OK_VERM;   } }
    else if(spread_mV >= LED_SPREAD_WARN_MV){ if((t%1000) < 500){ out[lo]=OK_YELLOW; out[hi]=OK_YELLOW; } }
}

// ---- public API ----
void ledInit(){
    sLed1.begin(); sLed2.begin();
    sLed1.setBrightness(LED_BRIGHT); sLed2.setBrightness(LED_BRIGHT);
    sLed1.clear(); sLed2.clear(); sLed1.show(); sLed2.show();
    sBootStart = millis(); sBootDone = false;
}

void ledLoop(){
    static uint32_t lastFrame = 0;
    uint32_t now = millis();
    if(now - lastFrame < 25) return;            // ~40 fps cap, fully non-blocking
    lastFrame = now;

    if(!sBootDone){ if(ledBoot()) sBootDone = true; else return; }

    sLed1.clear(); sLed2.clear();               // gap px5-6 stay dark; px11 set by indicator
    LedRGB a[LED_PACK_A_PX], b[LED_PACK_B_PX];
    // packs 1/3 -> far end (sideB, px10..7); packs 2/4 -> DIN end (sideA, px0..4)
    renderPackLive(packs[0], now, b, LED_PACK_B_PX); setPack(sLed1, false, b, LED_PACK_B_PX);
    renderPackLive(packs[1], now, a, LED_PACK_A_PX); setPack(sLed1, true,  a, LED_PACK_A_PX);
    renderPackLive(packs[2], now, b, LED_PACK_B_PX); setPack(sLed2, false, b, LED_PACK_B_PX);
    renderPackLive(packs[3], now, a, LED_PACK_A_PX); setPack(sLed2, true,  a, LED_PACK_A_PX);

    setKeepAliveIndicator(now);                 // owns px11 on BOTH strips — always, regardless of pack state
    sLed1.show(); sLed2.show();
}
