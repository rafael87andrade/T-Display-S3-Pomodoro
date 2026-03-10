#define TOUCH_MODULES_CST_SELF
#include <Arduino.h>
#include <LovyanGFX.h>
#include <TouchLib.h>
#include <Wire.h>
#include <lvgl.h>

#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

// Fontes
LV_FONT_DECLARE(ui_font_be75);

// --- Rede & mDNS ---
#include "secrets.h"
WebServer server(80);

// --- Historico de Sessoes (Tabela do Dia) ---
struct PomodoroSession {
  String type; // "Focus" ou "Break"
  int durationMins;
};
PomodoroSession historyTracker[30]; // Ate 30 sessoes por dia
int sessionCount = 0;
int totalFocusMins = 0;

// --- Configuration ---
static const uint16_t screenWidth = 320;
static const uint16_t screenHeight = 170;

// Pins for LilyGo T-Display S3 Parallel
#define PIN_POWER_ON 15
#define PIN_IIC_SCL 17
#define PIN_IIC_SDA 18
#define PIN_TOUCH_INT 16
#define PIN_TOUCH_RES 21

#ifndef CTS826_SLAVE_ADDRESS
#define CTS826_SLAVE_ADDRESS 0x15
#endif

// Themes
struct ThemePalette {
  const char *name;
  uint32_t bg;
  uint32_t text;
  uint32_t primary;
  uint32_t secondary;
  const char *htmlPrimary; // For Web UI
};

ThemePalette themes[4] = {
    {"Dark Teal (Original)", 0x1A1A1A, 0xEAEAEA, 0x006D5B, 0x404040,
     "#00b193ff"},
    {"Crimson Red", 0x1A1A1A, 0xEAEAEA, 0x990000, 0x404040, "#e4265fff"},
    {"Oceanic Blue", 0x1A1A1A, 0xEAEAEA, 0x005580, 0x404040, "#0083c5ff"},
    {"Midnight Purple", 0x100820, 0xEAEAEA, 0x6600cc, 0x2A1B40, "#8321e6ff"}};
int currentThemeIndex = 0;
int currentBrightness = 255;

// States
enum AppState { IDLE, FOCUS, BREAK };
AppState currentState = IDLE;
bool autoContinue = false;

// --- LovyanGFX Configuration ---
class LGFX : public lgfx::LGFX_Device {
public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.pin_wr = 8;
      cfg.pin_rd = 9;
      cfg.pin_rs = 7;
      cfg.pin_d0 = 39;
      cfg.pin_d1 = 40;
      cfg.pin_d2 = 41;
      cfg.pin_d3 = 42;
      cfg.pin_d4 = 45;
      cfg.pin_d5 = 46;
      cfg.pin_d6 = 47;
      cfg.pin_d7 = 48;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = 6;
      cfg.pin_rst = 5;
      cfg.pin_busy = -1;
      cfg.offset_rotation = 1;
      cfg.offset_x = 35;
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      cfg.panel_width = 170;
      cfg.panel_height = 320;
      _panel.config(cfg);
    }
    setPanel(&_panel);
    {
      auto cfg = _bl.config();
      cfg.pin_bl = 38;
      cfg.invert = false;
      cfg.freq = 22000;
      cfg.pwm_channel = 7;
      _bl.config(cfg);
      _panel.setLight(&_bl);
    }
  }

private:
  lgfx::Bus_Parallel8 _bus;
  lgfx::Panel_ST7789 _panel;
  lgfx::Light_PWM _bl;
};

// Globals
LGFX tft;
TouchLib touch(Wire, PIN_IIC_SDA, PIN_IIC_SCL, CTS826_SLAVE_ADDRESS,
               PIN_TOUCH_RES);

// LVGL v9 Memory Buffer
static lv_display_t *disp;
#define DRAW_BUF_SIZE (screenWidth * screenHeight / 10 * (16 / 8))
static uint8_t buf[DRAW_BUF_SIZE];

// UI Objects
lv_obj_t *ui_screen;
lv_obj_t *ui_label_clock;
lv_obj_t *ui_label_mode;
lv_obj_t *ui_btn_action;
lv_obj_t *ui_label_action;
lv_obj_t *ui_dots[30];

// Timer Logic
const uint32_t presetTimes[] = {15, 25, 45};
int currentPresetIndex = 1;
uint32_t totalTimeSec = 25 * 60;
int32_t remainingTimeSec = 25 * 60;
uint32_t lastTick = 0;
bool isPaused = true;

// --- LVGL Integration ---

void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  tft.pushImage(area->x1, area->y1, w, h, (uint16_t *)px_map);
  lv_display_flush_ready(disp);
}

void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
  if (touch.read()) {
    TP_Point t = touch.getPoint(0);
    data->state = LV_INDEV_STATE_PR;

    // Se clicar na esquerda registra na direita, o eixo X do touch
    // está invertido em relação ao LovyanGFX (tft.setRotation(2)).
    data->point.x = screenWidth - t.x;
    data->point.y = t.y;

    // Monitoramento do botão Home capacitivo
    // Geralmente em telas LilyGo ele registra uma coordenada fantasma fora da
    // tela (ex: Y > 320 ou X > 320) Descomente isto caso queira inspecionar as
    // coordenadas cruas no leitor Serial da IDE
    // Serial.printf("Touch RAW X:%d Y:%d -> LVGL X:%d Y:%d\n", t.x, t.y,
    // data->point.x, data->point.y);

    // Logica do Botao Home Tátil:
    // Retorna a aplicacao imediatamente para o status inicial.
    if (t.y > 300 || t.x > 300) {
      static uint32_t lastHomePress = 0;
      if (millis() - lastHomePress > 500) {
        lastHomePress = millis();
        Serial.println("HOME BUTTON PRESSED - RESETTING TIMER!");
        set_state(IDLE);
        remainingTimeSec = totalTimeSec;
        update_clock_ui();
      }
    }

  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

// --- UI Helpers ---

void update_clock_ui() {
  int mins = abs(remainingTimeSec) / 60;
  int secs = abs(remainingTimeSec) % 60;
  lv_label_set_text_fmt(ui_label_clock, "%02d:%02d", mins, secs);

  // Update Dots
  float progress = 0;
  if (currentState == FOCUS) {
    progress = (float)(totalTimeSec - remainingTimeSec) / totalTimeSec;
  } else if (currentState == BREAK) {
    progress = (float)remainingTimeSec / (5 * 60); // Break is 5 mins
  }

  int activeDots = (int)(progress * 30);
  for (int i = 0; i < 30; i++) {
    if (i < activeDots) {
      lv_obj_set_style_bg_color(
          ui_dots[i], lv_color_hex(themes[currentThemeIndex].primary), 0);
      lv_obj_set_style_bg_opa(ui_dots[i], 255, 0);
    } else {
      lv_obj_set_style_bg_color(
          ui_dots[i], lv_color_hex(themes[currentThemeIndex].secondary), 0);
      lv_obj_set_style_bg_opa(ui_dots[i], 80, 0);
    }
  }
}

void set_state(AppState next) {
  currentState = next;
  if (next == IDLE) {
    lv_label_set_text(ui_label_mode, "READY");
    lv_label_set_text(ui_label_action, LV_SYMBOL_PLAY);
    isPaused = true;
  } else if (next == FOCUS) {
    lv_label_set_text(ui_label_mode, "FOCUS TIME");
    lv_label_set_text(ui_label_action, LV_SYMBOL_PAUSE);
    isPaused = false;
  } else if (next == BREAK) {
    lv_label_set_text(ui_label_mode, "BREAK TIME");
    lv_label_set_text(ui_label_action, LV_SYMBOL_REFRESH);
    remainingTimeSec = 0;
    isPaused = false;
  }

  // All states use circular button now
  lv_obj_set_size(ui_btn_action, 65, 65);
  lv_obj_set_style_radius(ui_btn_action, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_text_font(ui_label_action, &lv_font_montserrat_14, 0);
  lv_obj_center(ui_label_action);
  update_clock_ui();
}

// --- Callbacks ---

static void btn_preset_cb(lv_event_t *e) {
  if (currentState != IDLE)
    return; // Only allow change when stopped

  currentPresetIndex = (currentPresetIndex + 1) % 3;
  totalTimeSec = presetTimes[currentPresetIndex] * 60;
  remainingTimeSec = totalTimeSec;

  update_clock_ui();
}

static void btn_action_cb(lv_event_t *e) {
  if (currentState == IDLE) {
    set_state(FOCUS);
  } else if (currentState == FOCUS) {
    isPaused = !isPaused;
    lv_label_set_text(ui_label_action,
                      isPaused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
  } else if (currentState == BREAK) {
    set_state(IDLE);
    remainingTimeSec = totalTimeSec;
    update_clock_ui();
  }
}

// --- UI Creation ---

void create_ui() {
  ui_screen = lv_screen_active();
  lv_obj_set_style_bg_color(ui_screen,
                            lv_color_hex(themes[currentThemeIndex].bg), 0);

  // 1. Progress Dots (Horizontal line at the top)
  for (int i = 0; i < 30; i++) {
    ui_dots[i] = lv_obj_create(ui_screen);
    lv_obj_set_size(ui_dots[i], 6, 6);
    lv_obj_set_style_radius(ui_dots[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(
        ui_dots[i], lv_color_hex(themes[currentThemeIndex].secondary), 0);
    lv_obj_set_style_bg_opa(ui_dots[i], 100, 0);
    lv_obj_set_style_border_width(ui_dots[i], 0, 0);
    lv_obj_set_pos(ui_dots[i], 44 + (i * 8), 15);
  }

  // 2. Clock Label (Large, Left)
  ui_label_clock = lv_label_create(ui_screen);
  lv_obj_set_style_text_font(ui_label_clock, &ui_font_be75, 0);
  lv_obj_set_style_text_color(ui_label_clock,
                              lv_color_hex(themes[currentThemeIndex].text), 0);
  lv_obj_align(ui_label_clock, LV_ALIGN_LEFT_MID, 35, 15);
  lv_label_set_text(ui_label_clock, "25:00");

  // Make the clock interactive for cycling presets
  lv_obj_add_flag(ui_label_clock, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(ui_label_clock, 20);
  lv_obj_add_event_cb(ui_label_clock, btn_preset_cb, LV_EVENT_CLICKED, NULL);

  // 3. Mode Label (Above clock)
  ui_label_mode = lv_label_create(ui_screen);
  lv_obj_set_style_text_font(ui_label_mode, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(ui_label_mode, lv_color_hex(0x666666), 0);
  lv_obj_align_to(ui_label_mode, ui_label_clock, LV_ALIGN_OUT_TOP_LEFT, 5, -2);
  lv_label_set_text(ui_label_mode, "FOCUS TIME");

  // 4. Unified Circular Action Button
  ui_btn_action = lv_button_create(ui_screen);
  lv_obj_set_size(ui_btn_action, 65, 65);
  lv_obj_set_style_radius(ui_btn_action, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(ui_btn_action, lv_color_hex(0x262626), 0);
  lv_obj_set_style_shadow_width(ui_btn_action, 0, 0);
  lv_obj_add_flag(ui_btn_action, LV_OBJ_FLAG_ADV_HITTEST);
  lv_obj_align(ui_btn_action, LV_ALIGN_RIGHT_MID, -35, 15);

  ui_label_action = lv_label_create(ui_btn_action);
  lv_obj_set_style_text_color(
      ui_label_action, lv_color_hex(themes[currentThemeIndex].primary), 0);
  lv_obj_set_style_text_font(ui_label_action, &lv_font_montserrat_14, 0);
  lv_label_set_text(ui_label_action, LV_SYMBOL_PLAY);
  lv_obj_center(ui_label_action);

  lv_obj_add_event_cb(ui_btn_action, btn_action_cb, LV_EVENT_CLICKED, NULL);
}

void apply_theme() {
  lv_obj_set_style_bg_color(ui_screen,
                            lv_color_hex(themes[currentThemeIndex].bg), 0);
  lv_obj_set_style_text_color(ui_label_clock,
                              lv_color_hex(themes[currentThemeIndex].text), 0);
  lv_obj_set_style_text_color(
      ui_label_action, lv_color_hex(themes[currentThemeIndex].primary), 0);
  update_clock_ui();
}

// --- Web Dashboard API ---
const char *htmlPage = R"WEB_PAGE(
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>ZenTime Dashboard</title>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600&family=Space+Grotesk:wght@700&display=swap" rel="stylesheet">
    <style>
        :root {
            --bg: #03040B;
            --card-bg: #111827;
            --primary: #29DDC7;
            --text-main: #EDF2F7;
            --text-secondary: #94A3B8;
            --btn-start-bg: #CEEFEB;
            --btn-start-text: #396A64;
            --btn-reset-bg: #9A1E39;
            --btn-reset-text: #FFFFFF;
        }

        body { 
            font-family: 'Inter', sans-serif; 
            background: var(--bg); 
            color: var(--text-main); 
            text-align: center; 
            padding: 20px; 
            margin: 0;
            display: flex;
            flex-direction: column;
            align-items: center;
        }

        .container { max-width: 440px; width: 100%; }

        .header {
            display: flex;
            align-items: center;
            gap: 10px;
            margin-bottom: 30px;
            font-weight: 600;
            font-size: 1.1em;
        }

        .card { 
            background: var(--card-bg); 
            backdrop-filter: blur(12px);
            padding: 24px; 
            border-radius: 32px; 
            margin-bottom: 20px; 
            border: 1px solid rgba(255,255,255,0.05);
        }

        .timer-container {
            position: relative;
            width: 256px;
            height: 256px;
            margin: 10px auto 30px;
            display: flex;
            flex-direction: column;
            justify-content: center;
            align-items: center;
        }

        .timer-svg {
            position: absolute;
            top: 0; left: 0;
            width: 100%; height: 100%;
        }

        .timer-base-ring {
            fill: none;
            stroke: #233841;
            stroke-width: 9;
            stroke-dasharray: 2 14;
        }

        .timer-progress-ring {
            fill: none;
            stroke: var(--primary);
            stroke-width: 9;
            stroke-linecap: round;
            transition: stroke-dashoffset 0.8s;
            transform: rotate(-90deg);
            transform-origin: 50% 50%;
        }

        .time-display { 
            font-family: 'Space Grotesk', sans-serif;
            font-size: 55px; 
            font-weight: 700; 
            color: var(--text-main); 
            line-height: 1;
            letter-spacing: -1.96px;
            z-index: 2;
        }

        .status-label {
            font-size: 14px;
            color: rgba(237, 242, 247, 0.5);
            text-transform: uppercase;
            letter-spacing: 0.43px;
            margin-top: 4px;
            z-index: 2;
            font-weight: 500;
        }

        .main-controls { display: flex; gap: 12px; margin-bottom: 20px; }

        button { 
            border: none; 
            cursor: pointer; 
            transition: 0.2s; 
            font-weight: 600;
            display: flex;
            align-items: center;
            justify-content: center;
            gap: 8px;
            font-size: 0.9em;
        }

        .btn-play { 
            background: var(--btn-start-bg); 
            color: var(--btn-start-text); 
            padding: 16px; 
            border-radius: 9999px; 
            flex-grow: 1;
            text-transform: uppercase;
            letter-spacing: 0.96px;
            font-size: 14px;
        }

        .btn-reset { 
            background: var(--btn-reset-bg); 
            color: var(--btn-reset-text); 
            width: 54px; height: 54px; 
            border-radius: 9999px;
            display: flex;
            align-items: center;
            justify-content: center;
        }

        button:hover { opacity: 0.9; transform: translateY(-1px); }
        button:active { transform: scale(0.98); }

        .control-row {
            display: flex;
            justify-content: space-between;
            align-items: center;
            background: rgba(255,255,255,0.02);
            padding: 12px 16px;
            border-radius: 16px;
            margin-bottom: 12px;
            border: 1px solid rgba(255,255,255,0.05);
        }

        .control-label { font-size: 0.95em; color: var(--text-main); }

        .switch { position: relative; display: inline-block; width: 44px; height: 24px; }
        .switch input { opacity: 0; width: 0; height: 0; }
        .slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background-color: #1F2937; transition: .4s; border-radius: 34px; }
        .slider:before { position: absolute; content: ""; height: 18px; width: 18px; left: 3px; bottom: 3px; background-color: white; transition: .4s; border-radius: 50%; }
        input:checked + .slider { background-color: #4ADE80; }
        input:checked + .slider:before { transform: translateX(20px); }

        .define-row { display: flex; gap: 8px; margin-top: 10px; }
        .input-group {
            flex: 1;
            display: flex;
            align-items: center;
            background: #F1F5F9;
            border-radius: 9999px;
            border: 1px solid var(--primary);
            padding: 0 16px;
        }
        input[type="number"] { 
            background: transparent; 
            border: none;
            color: #396A64;
            padding: 12px 0;
            flex-grow: 1;
            font-size: 16px;
            text-align: center;
            outline: none;
        }
        .input-divider { width: 1px; height: 20px; background: var(--primary); }
        .btn-define {
            background: #CEEFEB;
            color: #396A64;
            padding: 0 24px;
            border-radius: 9999px;
            text-transform: uppercase;
            font-size: 14px;
            letter-spacing: 0.96px;
        }

        .card h3 { text-align: left; font-size: 1.1em; margin: 0 0 20px 0; display: flex; align-items: center; gap: 10px; }
        .bright-row { text-align: left; margin-bottom: 20px; }
        .bright-header { display: flex; justify-content: space-between; margin-bottom: 10px; font-size: 0.9em; color: var(--text-secondary); }
        input[type="range"] { 
            width: 100%; 
            accent-color: var(--primary);
            background: #233841;
            height: 4px;
            border-radius: 2px;
        }

        .theme-grid { display: flex; gap: 12px; margin-top: 15px; }
        .theme-btn { 
            width: 34px; height: 34px; 
            border-radius: 50%; 
            cursor: pointer; 
            border: 2px solid transparent; 
            transition: 0.2s;
        }
        .theme-active { border-color: white; transform: scale(1.1); box-shadow: 0 0 12px rgba(255,255,255,0.2); }

        /* History list styles */
        .history-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 24px; }
        .history-header h3 { margin: 0; font-size: 1.1em; display: flex; align-items: center; gap: 10px; }
        .clear-btn { color: var(--primary); font-size: 0.85em; cursor: pointer; font-weight: 500; }
        
        .history-list { display: flex; flex-direction: column; gap: 20px; }
        .history-item { display: flex; align-items: center; gap: 16px; text-align: left; }
        .history-icon { 
            width: 44px; height: 44px; 
            background: rgba(255,255,255,0.03); 
            border-radius: 50%; 
            display: flex; 
            align-items: center; 
            justify-content: center;
            color: var(--primary);
            border: 1px solid rgba(255,255,255,0.05);
        }
        .history-content { flex-grow: 1; }
        .history-title { font-weight: 600; font-size: 0.95em; color: var(--text-main); }
        .history-status { font-size: 0.8em; color: var(--text-secondary); margin-top: 2px; }
        .history-time { font-weight: 500; font-size: 0.9em; color: var(--text-secondary); }
        .empty-history { padding: 40px; color: var(--text-secondary); font-size: 0.95em; }

    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <svg width="24" height="24" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M12 22C17.5228 22 22 17.5228 22 12C22 6.47715 17.5228 2 12 2C6.47715 2 2 6.47715 2 12C2 17.5228 6.47715 22 12 22Z" stroke="#29DDC7" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/><path d="M12 6V12L16 14" stroke="#29DDC7" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/></svg>
            Pomodoro
        </div>

        <div class="card">
            <div class="timer-container">
                <svg class="timer-svg" viewBox="0 0 256 256">
                    <circle class="timer-base-ring" cx="128" cy="128" r="120"></circle>
                    <circle id="timer-progress-path" class="timer-progress-ring" cx="128" cy="128" r="120" stroke-dasharray="753.98 753.98" stroke-dashoffset="0"></circle>
                </svg>
                <div class="time-display" id="clock">25:00</div>
                <div class="status-label" id="mode">FOCUS TIME</div>
            </div>

            <div class="main-controls">
                <button class="btn-play" onclick="sendAction('playpause')" id="btn-play">
                    <svg width="17" height="17" viewBox="0 0 17 17" fill="none"><path d="M6.25 12.0833L12.0833 8.33333L6.25 4.58333V12.0833Z" fill="currentColor"></path><path d="M8.33333 16.6667C7.18056 16.6667 6.09722 16.4479 5.08333 16.0104C4.06944 15.5729 3.1875 14.9792 2.4375 14.2292C1.6875 13.4792 1.09375 12.5972 0.65625 11.5833C0.21875 10.5694 0 9.48611 0 8.33333C0 7.18056 0.21875 6.09722 0.65625 5.08333C1.09375 4.06944 1.6875 3.1875 2.4375 2.4375C3.1875 1.6875 4.06944 1.09375 5.08333 0.65625C6.09722 0.21875 7.18056 0 8.33333 0C9.48611 0 10.5694 0.21875 11.5833 0.65625C12.5972 1.09375 13.4792 1.6875 14.2292 2.4375C14.9792 3.1875 15.5729 4.06944 16.0104 5.08333C16.4479 6.09722 16.6667 7.18056 16.6667 8.33333C16.6667 9.48611 16.4479 10.5694 16.0104 11.5833C15.5729 12.5972 14.9792 13.4792 14.2292 14.2292C13.4792 14.9792 12.5972 15.5729 11.5833 16.0104C10.5694 16.4479 9.48611 16.6667 8.33333 16.6667ZM8.33333 15C10.1944 15 11.7708 14.3542 13.0625 13.0625C14.3542 11.7708 15 10.1944 15 8.33333C15 6.47222 14.3542 4.89583 13.0625 3.60417C11.7708 2.3125 10.1944 1.66667 8.33333 1.66667C6.47222 1.66667 4.89583 2.3125 3.60417 3.60417C2.3125 4.89583 1.66667 6.47222 1.66667 8.33333C1.66667 10.1944 2.3125 11.7708 3.60417 13.0625C4.89583 14.3542 6.47222 15 8.33333 15Z" fill="currentColor"></path></svg>
                    START / RESUME
                </button>
                <button class="btn-reset" onclick="sendAction('reset')">
                    <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M23 4v6h-6M1 20v-6h6M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"/></svg>
                </button>
            </div>

            <div class="control-row">
                <span class="control-label">Ciclo contínuo</span>
                <label class="switch">
                    <input type="checkbox" id="auto-toggle" onclick="sendAction('autotoggle')">
                    <span class="slider"></span>
                </label>
            </div>

            <div class="define-row">
                <div class="input-group">
                    <input type="number" id="custom-mins" value="25" min="1" max="180">
                    <div class="input-divider"></div>
                </div>
                <button class="btn-define" onclick="setCustom()">DEFINIR</button>
            </div>
        </div>

        <div class="card">
            <h3>
                <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="#29DDC7" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1 0 2.83 2 2 0 0 1-2.83 0l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-2 2 2 2 0 0 1-2-2v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83 0 2 2 0 0 1 0-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1-2-2 2 2 0 0 1 2-2h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 0-2.83 2 2 0 0 1 2.83 0l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 2-2 2 2 0 0 1 2 2v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 0 2 2 0 0 1 0 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 2 2 2 2 0 0 1-2 2h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg>
                aparência
            </h3>
            
            <div class="bright-row">
                <div class="bright-header">
                    <span>Brilho da Tela</span>
                    <span id="bright-val">100%</span>
                </div>
                <input type="range" min="1" max="255" id="brightness" oninput="updateBrightText(this.value)" onchange="setBrightness(this.value)">
            </div>

            <div style="text-align: left;">
                <span style="font-size: 0.9em; color: var(--text-secondary);">Cor do tema</span>
                <div class="theme-grid" id="themes-container">
                    <!-- Dynamic Themes -->
                </div>
            </div>
        </div>

        <div class="card">
            <div class="history-header">
                <h3>
                    <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="var(--primary)" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 22C17.5228 22 22 17.5228 22 12C22 6.47715 17.5228 2 12 2C6.47715 2 2 6.47715 2 12C2 17.5228 6.47715 22 12 22Z"/><path d="M12 6V12L16 14"/></svg>
                    Sessões (<span id="total-focus">0</span>m focado)
                </h3>
                <span class="clear-btn" onclick="sendAction('clear')">Limpar</span>
            </div>
            <div class="history-list" id="history-body"></div>
            <div id="empty-msg" class="empty-history">Nenhuma sessão registrada ainda.</div>
        </div>
    </div>

    <script>
        let currentThemeId = -1;

        const updateBrightText = (val) => {
            document.getElementById('bright-val').innerText = Math.round((val/255)*100) + '%';
        }

        const updateUI = () => {
            fetch('/api/status').then(r => r.json()).then(data => {
                document.getElementById('clock').innerText = data.clock;
                document.getElementById('mode').innerText = data.mode;
                document.getElementById('total-focus').innerText = data.totalFocus;
                document.getElementById('auto-toggle').checked = data.autoContinue;
                
                // Update Progress Ring (Circumference for r=120 is ~753.98)
                const circ = 753.98;
                const path = document.getElementById('timer-progress-path');
                let progress = 0;

                if (data.totalSec > 0) {
                    if (data.mode === "BREAK TIME") {
                        // For break, progress fills as we count UP to 5 min
                        progress = data.remaining / (5 * 60);
                    } else {
                        // For focus, progress fills as we count DOWN (elapsed / total)
                        progress = (data.totalSec - data.remaining) / data.totalSec;
                    }
                }
                
                // Safeguard progress
                progress = Math.min(Math.max(progress, 0), 1);
                
                // Dashoffset calculation: 0 is FULL, circ is EMPTY
                // We want it to FILL UP as progress goes 0 -> 1
                path.style.strokeDashoffset = circ * (1 - progress);
                
                if (document.activeElement !== document.getElementById('brightness')) {
                     document.getElementById('brightness').value = data.brightness;
                     updateBrightText(data.brightness);
                }

                if (currentThemeId !== data.themeIdx) {
                    currentThemeId = data.themeIdx;
                    let thmHtml = '';
                    data.themes.forEach((t, idx) => {
                        let active = idx === data.themeIdx ? 'theme-active' : '';
                        thmHtml += `<div class="theme-btn ${active}" style="background-color: ${t.primary}" onclick="setTheme(${idx})" title="${t.name}"></div>`;
                    });
                    document.getElementById('themes-container').innerHTML = thmHtml;
                    document.documentElement.style.setProperty('--primary', data.themes[data.themeIdx].primary);
                }
                
                let listHtml = '';
                data.history.forEach(h => {
                    const icon = h.type === "Focus" ? 
                        '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M22 11.08V12a10 10 0 1 1-5.93-9.14"/><polyline points="22 4 12 14.01 9 11.01"/></svg>' : 
                        '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M20.84 4.61a5.5 5.5 0 0 0-7.78 0L12 5.67l-1.06-1.06a5.5 5.5 0 0 0-7.78 7.78l1.06 1.06L12 21.23l7.78-7.78 1.06-1.06a5.5 5.5 0 0 0 0-7.78z"/></svg>';
                    
                    listHtml += `
                        <div class="history-item">
                            <div class="history-icon">${icon}</div>
                            <div class="history-content">
                                <div class="history-title">${h.type.toUpperCase()} TIME</div>
                                <div class="history-status">Concluído</div>
                            </div>
                            <div class="history-time">${h.durationMins} min</div>
                        </div>
                    `;
                });
                document.getElementById('history-body').innerHTML = listHtml;
                document.getElementById('empty-msg').style.display = data.history.length ? 'none' : 'block';
                
                document.getElementById('btn-play').innerHTML = data.isPaused ? 
                    '<svg width="20" height="20" viewBox="0 0 24 24" fill="currentColor"><path d="M8 5.14v14l11-7-11-7z"/></svg> START / RESUME' : 
                    '<svg width="20" height="20" viewBox="0 0 24 24" fill="currentColor"><path d="M6 19h4V5H6v14zm8-14v14h4V5h-4z"/></svg> PAUSE';
            });
        }

        const sendAction = (cmd) => {
            fetch('/api/action?cmd=' + cmd, { method: 'POST' }).then(() => updateUI());
        }

        const setCustom = () => {
            let m = document.getElementById('custom-mins').value;
            fetch('/api/custom?mins=' + m, { method: 'POST' }).then(() => updateUI());
        }

        const setBrightness = (val) => {
            fetch('/api/custom?brightness=' + val, { method: 'POST' });
        }

        const setTheme = (idx) => {
            fetch('/api/custom?theme=' + idx, { method: 'POST' }).then(() => updateUI());
        }

        setInterval(updateUI, 1000);
        updateUI();
    </script>
</body>
</html>
)WEB_PAGE";

void setup_routing() {
  server.on("/", HTTP_GET, []() { server.send(200, "text/html", htmlPage); });

  server.on("/api/status", HTTP_GET, []() {
    int mins = abs(remainingTimeSec) / 60;
    int secs = abs(remainingTimeSec) % 60;
    char clockStr[10];
    sprintf(clockStr, "%02d:%02d", mins, secs);

    String modeStr = "READY";
    if (currentState == FOCUS)
      modeStr = "FOCUS TIME";
    if (currentState == BREAK)
      modeStr = "BREAK TIME";

    String json = "{";
    json += "\"clock\":\"" + String(clockStr) + "\",";
    json += "\"mode\":\"" + modeStr + "\",";
    json += "\"isPaused\":" + String(isPaused ? "true" : "false") + ",";
    json += "\"autoContinue\":" + String(autoContinue ? "true" : "false") + ",";
    json += "\"totalFocus\":" + String(totalFocusMins) + ",";
    json += "\"brightness\":" + String(currentBrightness) + ",";
    json += "\"themeIdx\":" + String(currentThemeIndex) + ",";
    json += "\"remaining\":" + String(remainingTimeSec) + ",";
    json += "\"totalSec\":" + String(totalTimeSec) + ",";

    json += "\"themes\":[";
    for (int i = 0; i < 4; i++) {
      json += "{\"name\":\"" + String(themes[i].name) + "\",\"primary\":\"" +
              String(themes[i].htmlPrimary) + "\"}";
      if (i < 3)
        json += ",";
    }
    json += "],";

    json += "\"history\":[";
    for (int i = 0; i < sessionCount; i++) {
      json += "{\"type\":\"" + historyTracker[i].type +
              "\",\"durationMins\":" + String(historyTracker[i].durationMins) +
              "}";
      if (i < sessionCount - 1)
        json += ",";
    }
    json += "]}";

    server.send(200, "application/json", json);
  });

  server.on("/api/action", HTTP_POST, []() {
    if (server.hasArg("cmd")) {
      String cmd = server.arg("cmd");
      if (cmd == "playpause") {
        btn_action_cb(NULL);
      } else if (cmd == "reset") {
        set_state(IDLE);
        remainingTimeSec = totalTimeSec;
        update_clock_ui();
      } else if (cmd == "autotoggle") {
        autoContinue = !autoContinue;
      } else if (cmd == "clear") {
        sessionCount = 0;
        totalFocusMins = 0;
      }
    }
    server.send(200, "text/plain", "OK");
  });

  server.on("/api/custom", HTTP_POST, []() {
    if (server.hasArg("mins")) {
      int m = server.arg("mins").toInt();
      if (m > 0) {
        set_state(IDLE);
        totalTimeSec = m * 60;
        remainingTimeSec = totalTimeSec;
        update_clock_ui();
      }
    }
    if (server.hasArg("brightness")) {
      currentBrightness = server.arg("brightness").toInt();
      tft.setBrightness(currentBrightness);
    }
    if (server.hasArg("theme")) {
      int tIdx = server.arg("theme").toInt();
      if (tIdx >= 0 && tIdx < 4) {
        currentThemeIndex = tIdx;
        apply_theme();
      }
    }
    server.send(200, "text/plain", "OK");
  });

  server.begin();
}

// --- Arduino Lifecycle ---

void setup() {
  Serial.begin(115200);
  delay(1000); // 1s boot delay matching original
  Serial.println("\n\n--- Starting ZenTime LovyanGFX ---");

  // Power On LCD PMIC
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);
  delay(200);

  // Initialize LovyanGFX Display
  tft.init();
  tft.setSwapBytes(true); // <--- Corrigir a ordem High/Low byte (Verde/Rosa)!
  tft.setRotation(
      2); // Rotation 2 matches T-Display-S3-Piano orientation, where left is
          // right and right is left usually on S3 when held horizontally
  tft.setBrightness(255);
  tft.fillScreen(TFT_BLACK);

  // Inicializa Touch
  pinMode(PIN_TOUCH_RES, OUTPUT);
  digitalWrite(PIN_TOUCH_RES, LOW);
  delay(200);
  digitalWrite(PIN_TOUCH_RES, HIGH);
  delay(200);

  Wire.begin(PIN_IIC_SDA, PIN_IIC_SCL);
  if (touch.init()) {
    Serial.println("CST816 Detectado.");
    touch.setRotation(1); // Mapeamento do library original
  } else {
    Serial.println("Erro: Touch nao responde.");
  }

  Serial.println("Initializing LVGL...");
  lv_init();
  lv_tick_set_cb(millis);

  disp = lv_display_create(screenWidth, screenHeight);
  lv_display_set_flush_cb(disp, my_disp_flush);
  lv_display_set_buffers(disp, buf, NULL, sizeof(buf),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touchpad_read);

  Serial.println("Creating UI...");
  create_ui();
  set_state(IDLE);

  // Inicializar WiFi silenciosamente (não bloqueia se falhar)
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 15) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.println(WiFi.localIP());
    if (MDNS.begin("pomodoro")) {
      Serial.println("mDNS responder started: http://pomodoro.local");
    }
    setup_routing();
  } else {
    Serial.println("\nWiFi Failed -> Continuing offline.");
  }

  Serial.println("Setup Complete.");
}

void loop() {
  lv_timer_handler();

  // State Logic
  if (!isPaused && (millis() - lastTick >= 1000)) {
    lastTick = millis();

    if (currentState == FOCUS) {
      remainingTimeSec--;
      if (remainingTimeSec <= 0) {
        // Gravar no histórico da Sessão
        if (sessionCount < 30) {
          historyTracker[sessionCount].type = "Focus";
          historyTracker[sessionCount].durationMins = totalTimeSec / 60;
          totalFocusMins += (totalTimeSec / 60);
          sessionCount++;
        }
        set_state(BREAK);
      }
    } else if (currentState == BREAK) {
      remainingTimeSec++;
      if (remainingTimeSec >= 5 * 60) {
        if (sessionCount < 30) {
          historyTracker[sessionCount].type = "Break";
          historyTracker[sessionCount].durationMins = 5;
          sessionCount++;
        }
        if (autoContinue) {
          set_state(FOCUS);
          remainingTimeSec = totalTimeSec;
        } else {
          set_state(IDLE);
          remainingTimeSec = totalTimeSec;
        }
      }
    }
    update_clock_ui();
  }

  // Handle Web Server Requests
  server.handleClient();

  delay(5);
}
