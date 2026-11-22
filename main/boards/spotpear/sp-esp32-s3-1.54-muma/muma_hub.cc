#include "muma_hub.h"

#include "application.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "mcp_server.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_netif.h>
#include <esp_sntp.h>
#include <nvs.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#define TAG "MumaHub"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

#if defined(LV_FONT_MONTSERRAT_48) && LV_FONT_MONTSERRAT_48
#define MUMA_FONT_BIG (&lv_font_montserrat_48)
#else
#define MUMA_FONT_BIG (&BUILTIN_TEXT_FONT)
#endif

#if defined(LV_FONT_MONTSERRAT_20) && LV_FONT_MONTSERRAT_20
#define MUMA_FONT_MED (&lv_font_montserrat_20)
#else
#define MUMA_FONT_MED (&BUILTIN_TEXT_FONT)
#endif

#if defined(LV_FONT_MONTSERRAT_14) && LV_FONT_MONTSERRAT_14
#define MUMA_FONT_SMALL (&lv_font_montserrat_14)
#else
#define MUMA_FONT_SMALL (&BUILTIN_TEXT_FONT)
#endif

namespace {

esp_err_t HttpCollectEventHandler(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data != nullptr && evt->data != nullptr &&
        evt->data_len > 0) {
        auto* out = static_cast<std::string*>(evt->user_data);
        if (out->size() + static_cast<size_t>(evt->data_len) < 8192) {
            out->append(static_cast<const char*>(evt->data), evt->data_len);
        }
    }
    return ESP_OK;
}

bool HttpGetString(const std::string& url, std::string& out_body) {
    out_body.clear();
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 8000;
    cfg.event_handler = HttpCollectEventHandler;
    cfg.user_data = &out_body;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return false;
    }
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return (err == ESP_OK && status >= 200 && status < 300 && !out_body.empty());
}

const char kWebPanelHtml[] = R"rawliteral(<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Panel MUMA</title>
<style>
body{font-family:system-ui,-apple-system,sans-serif;background:#0f172a;color:#f1f5f9;margin:0;padding:14px}
.card{background:#1e293b;border-radius:14px;padding:14px;margin-bottom:12px;box-shadow:0 2px 8px rgba(0,0,0,.3)}
h2{margin:0 0 10px;font-size:17px;color:#38bdf8}
.row{display:flex;gap:8px;align-items:center;justify-content:space-between;margin:8px 0;flex-wrap:wrap}
button{background:#0284c7;color:#fff;border:0;border-radius:10px;padding:10px 14px;font-size:14px;font-weight:600;cursor:pointer}
button.alt{background:#334155}
button.warn{background:#dc2626}
input[type=text],input[type=number],input[type=time]{background:#0f172a;color:#fff;border:1px solid #475569;border-radius:8px;padding:8px;font-size:14px}
input[type=range]{width:100%}
.stat{font-size:22px;font-weight:700}
.muted{color:#94a3b8;font-size:13px}
</style>
</head>
<body>
<div class="card">
  <h2>Robot MUMA</h2>
  <div class="stat" id="st_clock">--:--</div>
  <div class="muted" id="st_weather">Cargando clima...</div>
  <div class="muted" id="st_extra"></div>
  <div class="row" style="margin-top:12px">
    <button onclick="act('toggle_clock')">Reloj / Chat</button>
    <button class="alt" id="btn_mic" onclick="act('toggle_mic')">Mic</button>
    <button class="warn" id="btn_stop" style="display:none" onclick="act('stop_alarm')">Apagar Alarma</button>
  </div>
</div>

<div class="card">
  <h2>Brillo y Volumen</h2>
  <div class="row"><span>Brillo</span><span id="v_br">80%</span></div>
  <input type="range" id="r_br" min="5" max="100" oninput="v_br.textContent=this.value+'%'" onchange="act('brightness',+this.value)">
  <div class="row"><span>Volumen</span><span id="v_vol">70%</span></div>
  <input type="range" id="r_vol" min="0" max="100" oninput="v_vol.textContent=this.value+'%'" onchange="act('volume',+this.value)">
</div>

<div class="card">
  <h2>Clima y Energia</h2>
  <div class="row">
    <input type="text" id="city" style="flex:1" placeholder="Ciudad">
    <button onclick="saveCity()">Buscar</button>
  </div>
  <div class="row">
    <label><input type="checkbox" id="always_on"> Siempre encendido (sin sueno profundo)</label>
  </div>
  <div class="row">
    <label><input type="checkbox" id="night_en"> Modo nocturno automatico</label>
  </div>
  <div class="row">
    <span>Inicio (h): <input type="number" id="n_start" min="0" max="23" style="width:60px"></span>
    <span>Fin (h): <input type="number" id="n_end" min="0" max="23" style="width:60px"></span>
  </div>
  <div class="row">
    <span>Brillo dia: <input type="number" id="b_day" min="5" max="100" style="width:65px"></span>
    <span>Brillo noche: <input type="number" id="b_night" min="1" max="80" style="width:65px"></span>
  </div>
  <button onclick="saveConfig()">Guardar Ajustes</button>
</div>

<div class="card">
  <h2>Temporizador</h2>
  <div class="row">
    <input type="number" id="t_min" min="1" max="360" value="10" style="width:75px">
    <input type="text" id="t_lbl" value="Temporizador" style="flex:1">
    <button onclick="startTimer()">Iniciar</button>
    <button class="alt" onclick="act('cancel_timer')">Cancelar</button>
  </div>
</div>

<div class="card">
  <h2>Alarmas</h2>
  <div id="alarms_box"></div>
  <button onclick="saveAlarms()">Guardar Alarmas</button>
</div>

<script>
const $=id=>document.getElementById(id);
let loaded=false;
async function load(){
  try{
    const r=await fetch('/api/state'); const d=await r.json();
    $('st_clock').textContent=d.time+'  '+d.date;
    $('st_weather').textContent=d.weather;
    $('st_extra').textContent=(d.timer_s>0?('Temporizador: '+Math.floor(d.timer_s/60)+'m '+(d.timer_s%60)+'s | '):'')+'IP: '+d.ip;
    $('btn_mic').textContent=d.mic_muted?'Reactivar Mic':'Silenciar Mic';
    $('btn_stop').style.display=d.ringing?'inline-block':'none';
    if(!loaded){
      loaded=true;
      $('r_br').value=d.brightness; $('v_br').textContent=d.brightness+'%';
      $('r_vol').value=d.volume; $('v_vol').textContent=d.volume+'%';
      $('city').value=d.city;
      $('always_on').checked=d.always_on;
      $('night_en').checked=d.night_en;
      $('n_start').value=d.n_start; $('n_end').value=d.n_end;
      $('b_day').value=d.b_day; $('b_night').value=d.b_night;
      let html='';
      d.alarms.forEach((a,i)=>{
        const hh=String(a.h).padStart(2,'0'), mm=String(a.m).padStart(2,'0');
        html+=`<div class="row"><input type="checkbox" id="ae${i}" ${a.en?'checked':''}><input type="time" id="at${i}" value="${hh}:${mm}"><input type="text" id="al${i}" value="${a.lbl}" style="flex:1"></div>`;
      });
      $('alarms_box').innerHTML=html;
    }
  }catch(e){}
}
async function act(cmd,val=0,text=''){
  await fetch('/api/action',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({cmd,val,text})});
  setTimeout(load,300);
}
async function saveCity(){
  await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({city:$('city').value})});
  alert('Buscando ciudad y actualizando clima...');
  setTimeout(load,1500);
}
async function saveConfig(){
  await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({
    always_on:$('always_on').checked,
    night_en:$('night_en').checked,
    n_start:+$('n_start').value,
    n_end:+$('n_end').value,
    b_day:+$('b_day').value,
    b_night:+$('b_night').value
  })});
  alert('Ajustes guardados');
  load();
}
async function saveAlarms(){
  const alarms=[];
  for(let i=0;i<3;i++){
    const parts=$('at'+i).value.split(':');
    alarms.push({en:$('ae'+i).checked,h:+(parts[0]||7),m:+(parts[1]||0),lbl:$('al'+i).value||'Alarma'});
  }
  await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({alarms})});
  alert('Alarmas guardadas');
  load();
}
function startTimer(){ act('start_timer',+$('t_min').value,$('t_lbl').value); }
load(); setInterval(load,4000);
</script>
</body>
</html>)rawliteral";

}  // namespace

void MumaHub::Start() {
    if (main_task_ != nullptr) {
        return;
    }
    LoadSettings();
    setenv("TZ", "CST6", 1);
    tzset();

    xTaskCreate(MainTaskEntry, "muma_hub", 4096, this, 2, &main_task_);
}

void MumaHub::MainTaskEntry(void* arg) {
    static_cast<MumaHub*>(arg)->MainLoop();
    vTaskDelete(nullptr);
}

void MumaHub::WeatherTaskEntry(void* arg) {
    static_cast<MumaHub*>(arg)->FetchWeatherNow();
    vTaskDelete(nullptr);
}

void MumaHub::LoadSettings() {
    nvs_handle_t h;
    if (nvs_open("muma", NVS_READONLY, &h) != ESP_OK) {
        return;
    }

    int32_t v = 0;
    if (nvs_get_i32(h, "always_on", &v) == ESP_OK) always_on_.store(v != 0);
    if (nvs_get_i32(h, "night_en", &v) == ESP_OK) night_enabled_ = (v != 0);
    if (nvs_get_i32(h, "n_start", &v) == ESP_OK) night_start_hour_ = std::clamp<int>(v, 0, 23);
    if (nvs_get_i32(h, "n_end", &v) == ESP_OK) night_end_hour_ = std::clamp<int>(v, 0, 23);
    if (nvs_get_i32(h, "b_day", &v) == ESP_OK) day_brightness_ = std::clamp<int>(v, 5, 100);
    if (nvs_get_i32(h, "b_night", &v) == ESP_OK) night_brightness_ = std::clamp<int>(v, 1, 80);
    if (nvs_get_i32(h, "lat_e4", &v) == ESP_OK) latitude_ = v / 10000.0;
    if (nvs_get_i32(h, "lon_e4", &v) == ESP_OK) longitude_ = v / 10000.0;

    char str_buf[64];
    size_t len = sizeof(str_buf);
    if (nvs_get_str(h, "city", str_buf, &len) == ESP_OK && str_buf[0] != '\0') {
        city_ = str_buf;
    }

    for (int i = 0; i < 3; ++i) {
        char k_en[16], k_h[16], k_m[16], k_l[16];
        snprintf(k_en, sizeof(k_en), "a%d_en", i);
        snprintf(k_h, sizeof(k_h), "a%d_h", i);
        snprintf(k_m, sizeof(k_m), "a%d_m", i);
        snprintf(k_l, sizeof(k_l), "a%d_l", i);
        if (nvs_get_i32(h, k_en, &v) == ESP_OK) alarms_[i].enabled = (v != 0);
        if (nvs_get_i32(h, k_h, &v) == ESP_OK) alarms_[i].hour = std::clamp<int>(v, 0, 23);
        if (nvs_get_i32(h, k_m, &v) == ESP_OK) alarms_[i].minute = std::clamp<int>(v, 0, 59);
        len = sizeof(str_buf);
        if (nvs_get_str(h, k_l, str_buf, &len) == ESP_OK && str_buf[0] != '\0') {
            alarms_[i].label = str_buf;
        }
    }

    current_brightness_ = day_brightness_;
    nvs_close(h);
}

void MumaHub::SaveSettings() {
    nvs_handle_t h;
    if (nvs_open("muma", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }

    nvs_set_i32(h, "always_on", always_on_.load() ? 1 : 0);
    nvs_set_i32(h, "night_en", night_enabled_ ? 1 : 0);
    nvs_set_i32(h, "n_start", night_start_hour_);
    nvs_set_i32(h, "n_end", night_end_hour_);
    nvs_set_i32(h, "b_day", day_brightness_);
    nvs_set_i32(h, "b_night", night_brightness_);
    nvs_set_i32(h, "lat_e4", static_cast<int32_t>(std::lround(latitude_ * 10000.0)));
    nvs_set_i32(h, "lon_e4", static_cast<int32_t>(std::lround(longitude_ * 10000.0)));
    nvs_set_str(h, "city", city_.c_str());

    for (int i = 0; i < 3; ++i) {
        char k_en[16], k_h[16], k_m[16], k_l[16];
        snprintf(k_en, sizeof(k_en), "a%d_en", i);
        snprintf(k_h, sizeof(k_h), "a%d_h", i);
        snprintf(k_m, sizeof(k_m), "a%d_m", i);
        snprintf(k_l, sizeof(k_l), "a%d_l", i);
        nvs_set_i32(h, k_en, alarms_[i].enabled ? 1 : 0);
        nvs_set_i32(h, k_h, alarms_[i].hour);
        nvs_set_i32(h, k_m, alarms_[i].minute);
        nvs_set_str(h, k_l, alarms_[i].label.c_str());
    }

    nvs_commit(h);
    nvs_close(h);
}

void MumaHub::InitUiOverlay() {
    if (ui_ready_) {
        return;
    }
    if (!lvgl_port_lock(100)) {
        return;
    }
    lv_obj_t* screen = lv_screen_active();
    if (screen == nullptr || lv_obj_get_child_cnt(screen) == 0) {
        lvgl_port_unlock();
        return;
    }

    clock_layer_ = lv_obj_create(screen);
    lv_obj_set_size(clock_layer_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(clock_layer_, 0, 0);
    lv_obj_set_style_border_width(clock_layer_, 0, 0);
    lv_obj_set_style_pad_all(clock_layer_, 8, 0);
    lv_obj_set_style_bg_color(clock_layer_, lv_color_hex(0x0B1220), 0);
    lv_obj_set_style_bg_opa(clock_layer_, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(clock_layer_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(clock_layer_, LV_OBJ_FLAG_HIDDEN);

    lbl_badge_ = lv_label_create(clock_layer_);
    lv_obj_set_style_text_font(lbl_badge_, MUMA_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl_badge_, lv_color_hex(0xF59E0B), 0);
    lv_label_set_text(lbl_badge_, "");
    lv_obj_align(lbl_badge_, LV_ALIGN_TOP_MID, 0, 2);

    lbl_time_ = lv_label_create(clock_layer_);
    lv_obj_set_style_text_font(lbl_time_, MUMA_FONT_BIG, 0);
    lv_obj_set_style_text_color(lbl_time_, lv_color_hex(0xF8FAFC), 0);
    lv_label_set_text(lbl_time_, "--:--");
    lv_obj_align(lbl_time_, LV_ALIGN_CENTER, 0, -42);

    lbl_date_ = lv_label_create(clock_layer_);
    lv_obj_set_style_text_font(lbl_date_, MUMA_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl_date_, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text(lbl_date_, "Sincronizando hora...");
    lv_obj_align(lbl_date_, LV_ALIGN_CENTER, 0, 2);

    lbl_weather_ = lv_label_create(clock_layer_);
    lv_obj_set_style_text_font(lbl_weather_, MUMA_FONT_MED, 0);
    lv_obj_set_style_text_color(lbl_weather_, lv_color_hex(0x38BDF8), 0);
    lv_label_set_text(lbl_weather_, "-- C");
    lv_obj_align(lbl_weather_, LV_ALIGN_CENTER, 0, 34);

    lbl_city_ = lv_label_create(clock_layer_);
    lv_obj_set_width(lbl_city_, LV_HOR_RES - 20);
    lv_label_set_long_mode(lbl_city_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(lbl_city_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl_city_, MUMA_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl_city_, lv_color_hex(0xCBD5E1), 0);
    lv_label_set_text(lbl_city_, city_.c_str());
    lv_obj_align(lbl_city_, LV_ALIGN_CENTER, 0, 62);

    lbl_footer_ = lv_label_create(clock_layer_);
    lv_obj_set_style_text_font(lbl_footer_, MUMA_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl_footer_, lv_color_hex(0x64748B), 0);
    lv_label_set_text(lbl_footer_, "Sin Wi-Fi");
    lv_obj_align(lbl_footer_, LV_ALIGN_BOTTOM_MID, 0, -2);

    alarm_banner_ = lv_obj_create(screen);
    lv_obj_set_size(alarm_banner_, LV_HOR_RES - 16, 110);
    lv_obj_set_style_radius(alarm_banner_, 14, 0);
    lv_obj_set_style_border_width(alarm_banner_, 2, 0);
    lv_obj_set_style_border_color(alarm_banner_, lv_color_hex(0xFDE047), 0);
    lv_obj_set_style_bg_color(alarm_banner_, lv_color_hex(0xB91C1C), 0);
    lv_obj_set_style_bg_opa(alarm_banner_, LV_OPA_COVER, 0);
    lv_obj_align(alarm_banner_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(alarm_banner_, LV_OBJ_FLAG_HIDDEN);

    lbl_alarm_text_ = lv_label_create(alarm_banner_);
    lv_obj_set_width(lbl_alarm_text_, LV_HOR_RES - 40);
    lv_label_set_long_mode(lbl_alarm_text_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl_alarm_text_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl_alarm_text_, MUMA_FONT_MED, 0);
    lv_obj_set_style_text_color(lbl_alarm_text_, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_alarm_text_, "ALARMA\nToca para apagar");
    lv_obj_center(lbl_alarm_text_);

    ui_ready_ = true;
    lvgl_port_unlock();
}

void MumaHub::UpdateUiOverlay() {
    if (!ui_ready_) {
        return;
    }
    auto state = Application::GetInstance().GetDeviceState();
    bool should_show_clock = (state == kDeviceStateIdle) && clock_view_enabled_.load();
    bool ringing = alarm_ringing_.load();

    time_t now = time(nullptr);
    struct tm t = {};
    localtime_r(&now, &t);

    char time_buf[16];
    char date_buf[40];
    if (t.tm_year + 1900 >= 2024) {
        int h12 = t.tm_hour % 12;
        if (h12 == 0) h12 = 12;
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d", h12, t.tm_min);
        static const char* kDays[] = {"Dom", "Lun", "Mar", "Mie", "Jue", "Vie", "Sab"};
        static const char* kMonths[] = {"Ene", "Feb", "Mar", "Abr", "May", "Jun",
                                        "Jul", "Ago", "Sep", "Oct", "Nov", "Dic"};
        snprintf(date_buf, sizeof(date_buf), "%s %02d %s %s", kDays[t.tm_wday % 7], t.tm_mday,
                 kMonths[t.tm_mon % 12], (t.tm_hour >= 12) ? "PM" : "AM");
    } else {
        snprintf(time_buf, sizeof(time_buf), "--:--");
        snprintf(date_buf, sizeof(date_buf), "Esperando hora...");
    }

    char weather_buf[48];
    char badge_buf[48];
    std::string city_copy;
    std::string ip_copy;
    std::string ringing_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (weather_valid_) {
            snprintf(weather_buf, sizeof(weather_buf), "%.0f C  %s", temp_c_,
                     WeatherCodeToSpanish(weather_code_));
        } else {
            snprintf(weather_buf, sizeof(weather_buf), "Actualizando clima...");
        }
        city_copy = city_;
        ip_copy = ip_str_;
        ringing_copy = ringing_reason_;

        if (mic_muted_.load()) {
            snprintf(badge_buf, sizeof(badge_buf), "MIC SILENCIADO");
        } else if (timer_active_ && timer_remaining_s_ > 0) {
            snprintf(badge_buf, sizeof(badge_buf), "TEMP %02d:%02d", timer_remaining_s_ / 60,
                     timer_remaining_s_ % 60);
        } else {
            badge_buf[0] = '\0';
        }
    }

    if (!lvgl_port_lock(100)) {
        return;
    }

    if (should_show_clock) {
        if (!overlay_visible_) {
            lv_obj_remove_flag(clock_layer_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(clock_layer_);
            overlay_visible_ = true;
        }
        lv_label_set_text(lbl_time_, time_buf);
        lv_label_set_text(lbl_date_, date_buf);
        lv_label_set_text(lbl_weather_, weather_buf);
        lv_label_set_text(lbl_city_, city_copy.c_str());
        lv_label_set_text(lbl_footer_, ip_copy.c_str());
        lv_label_set_text(lbl_badge_, badge_buf);
    } else if (overlay_visible_) {
        lv_obj_add_flag(clock_layer_, LV_OBJ_FLAG_HIDDEN);
        overlay_visible_ = false;
    }

    if (ringing) {
        char banner_buf[96];
        snprintf(banner_buf, sizeof(banner_buf), "%s\nToca para apagar", ringing_copy.c_str());
        lv_label_set_text(lbl_alarm_text_, banner_buf);
        lv_obj_remove_flag(alarm_banner_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(alarm_banner_);
    } else {
        lv_obj_add_flag(alarm_banner_, LV_OBJ_FLAG_HIDDEN);
    }

    lvgl_port_unlock();
}

void MumaHub::StartNetworkServices() {
    if (network_started_) {
        return;
    }
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr) {
        return;
    }
    esp_netif_ip_info_t ip_info = {};
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return;
    }

    char ip_buf[32];
    snprintf(ip_buf, sizeof(ip_buf), IPSTR, IP2STR(&ip_info.ip));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ip_str_ = ip_buf;
    }
    network_started_ = true;
    ESP_LOGI(TAG, "Panel web disponible en http://%s", ip_buf);

    if (!esp_sntp_enabled()) {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.google.com");
        esp_sntp_init();
    }

    StartHttpServer();
    weather_countdown_s_ = 2;
}

void MumaHub::CheckNightMode(bool force) {
    time_t now = time(nullptr);
    struct tm t = {};
    localtime_r(&now, &t);
    if (t.tm_year + 1900 < 2024) {
        return;
    }

    int target_brightness = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!night_enabled_) {
            last_night_state_ = -1;
            return;
        }
        bool is_night = (night_start_hour_ > night_end_hour_)
                            ? (t.tm_hour >= night_start_hour_ || t.tm_hour < night_end_hour_)
                            : (t.tm_hour >= night_start_hour_ && t.tm_hour < night_end_hour_);
        int state_int = is_night ? 1 : 0;
        if (force || state_int != last_night_state_) {
            last_night_state_ = state_int;
            target_brightness = is_night ? night_brightness_ : day_brightness_;
            current_brightness_ = target_brightness;
        }
    }

    if (target_brightness >= 0) {
        auto* backlight = Board::GetInstance().GetBacklight();
        if (backlight != nullptr) {
            backlight->SetBrightness(static_cast<uint8_t>(target_brightness));
        }
    }
}

void MumaHub::CheckAlarmsAndTimer() {
    std::string trigger_msg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (timer_active_) {
            if (timer_remaining_s_ > 0) {
                timer_remaining_s_--;
            }
            if (timer_remaining_s_ == 0) {
                timer_active_ = false;
                trigger_msg = timer_label_;
            }
        }

        time_t now = time(nullptr);
        struct tm t = {};
        localtime_r(&now, &t);
        if (t.tm_year + 1900 >= 2024) {
            for (int i = 0; i < 3; ++i) {
                if (alarms_[i].enabled && alarms_[i].hour == t.tm_hour &&
                    alarms_[i].minute == t.tm_min && alarms_[i].last_triggered_day != t.tm_yday) {
                    alarms_[i].last_triggered_day = t.tm_yday;
                    trigger_msg = alarms_[i].label;
                }
            }
        }
    }

    if (!trigger_msg.empty()) {
        TriggerAlarm(trigger_msg);
    }
}

void MumaHub::TriggerAlarm(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ringing_reason_ = reason.empty() ? "Alarma" : reason;
        ringing_elapsed_ms_ = 0;
        ringing_sound_cooldown_ms_ = 0;
    }
    alarm_ringing_.store(true);
    ESP_LOGI(TAG, "Alarma activada: %s", reason.c_str());
}

void MumaHub::StopAlarm() {
    if (!alarm_ringing_.exchange(false)) {
        return;
    }
    ESP_LOGI(TAG, "Alarma detenida");
    Application::GetInstance().Schedule([]() {
        Board::GetInstance().GetDisplay()->ShowNotification("Alarma apagada", 2000);
    });
}

void MumaHub::MainLoop() {
    int tick_quarter = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(250));
        tick_quarter++;

        InitUiOverlay();
        StartNetworkServices();

        // Mantener modo privacidad de micrófono aunque el sistema regrese a reposo
        auto& app = Application::GetInstance();
        if (mic_muted_.load() && app.GetDeviceState() == kDeviceStateIdle &&
            app.GetAudioService().IsWakeWordRunning()) {
            app.Schedule([]() {
                Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
            });
        }

        // Sonido repetitivo mientras la alarma esté activa (auto-apagado a los 2 minutos)
        if (alarm_ringing_.load()) {
            bool play_now = false;
            bool timeout = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ringing_elapsed_ms_ += 250;
                if (ringing_elapsed_ms_ >= 120000) {
                    timeout = true;
                } else if (ringing_sound_cooldown_ms_ <= 0) {
                    play_now = true;
                    ringing_sound_cooldown_ms_ = 2500;
                } else {
                    ringing_sound_cooldown_ms_ -= 250;
                }
            }
            if (timeout) {
                StopAlarm();
            } else if (play_now) {
                app.Schedule([]() {
                    Application::GetInstance().PlaySound(Lang::Sounds::OGG_VIBRATION);
                });
            }
        }

        if (tick_quarter >= 4) {
            tick_quarter = 0;
            CheckAlarmsAndTimer();
            CheckNightMode(false);
            UpdateUiOverlay();

            if (network_started_ && !weather_busy_.load()) {
                if (weather_countdown_s_ <= 0) {
                    weather_countdown_s_ = 1200;  // Cada 20 minutos
                    weather_busy_.store(true);
                    xTaskCreate(WeatherTaskEntry, "muma_weather", 6144, this, 1, &weather_task_);
                } else {
                    weather_countdown_s_--;
                }
            }
        }
    }
}

void MumaHub::FetchWeatherNow() {
    double lat = 0.0, lon = 0.0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lat = latitude_;
        lon = longitude_;
    }

    char url[220];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,weather_code&timezone=auto",
             lat, lon);

    std::string body;
    if (HttpGetString(url, body)) {
        cJSON* root = cJSON_Parse(body.c_str());
        if (root != nullptr) {
            cJSON* current = cJSON_GetObjectItem(root, "current");
            if (cJSON_IsObject(current)) {
                cJSON* temp = cJSON_GetObjectItem(current, "temperature_2m");
                cJSON* hum = cJSON_GetObjectItem(current, "relative_humidity_2m");
                cJSON* code = cJSON_GetObjectItem(current, "weather_code");
                if (cJSON_IsNumber(temp) && cJSON_IsNumber(code)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    temp_c_ = static_cast<float>(temp->valuedouble);
                    humidity_ = cJSON_IsNumber(hum) ? hum->valueint : 0;
                    weather_code_ = code->valueint;
                    weather_valid_ = true;
                }
            }
            cJSON_Delete(root);
        }
    } else {
        ESP_LOGW(TAG, "No se pudo actualizar el clima");
    }

    weather_busy_.store(false);
}

bool MumaHub::GeocodeCity(const std::string& city_name, double& out_lat, double& out_lon,
                          std::string& out_resolved_name) {
    std::string url = "https://geocoding-api.open-meteo.com/v1/search?name=" +
                      UrlEncode(city_name) + "&count=1&language=es&format=json";
    std::string body;
    if (!HttpGetString(url, body)) {
        return false;
    }

    bool ok = false;
    cJSON* root = cJSON_Parse(body.c_str());
    if (root != nullptr) {
        cJSON* results = cJSON_GetObjectItem(root, "results");
        if (cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0) {
            cJSON* first = cJSON_GetArrayItem(results, 0);
            cJSON* lat = cJSON_GetObjectItem(first, "latitude");
            cJSON* lon = cJSON_GetObjectItem(first, "longitude");
            cJSON* name = cJSON_GetObjectItem(first, "name");
            if (cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
                out_lat = lat->valuedouble;
                out_lon = lon->valuedouble;
                out_resolved_name = cJSON_IsString(name) ? name->valuestring : city_name;
                ok = true;
            }
        }
        cJSON_Delete(root);
    }
    return ok;
}

void MumaHub::ToggleClockView() {
    SetClockView(!clock_view_enabled_.load());
}

void MumaHub::SetClockView(bool enabled) {
    clock_view_enabled_.store(enabled);
    UpdateUiOverlay();
}

void MumaHub::AdjustBrightness(int delta) {
    int next_val = 80;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_brightness_ = std::clamp(current_brightness_ + delta, 5, 100);
        next_val = current_brightness_;
    }
    SetBrightnessManual(next_val);
}

void MumaHub::SetBrightnessManual(int value) {
    int clamped = std::clamp(value, 1, 100);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_brightness_ = clamped;
    }
    Application::GetInstance().Schedule([clamped]() {
        auto* backlight = Board::GetInstance().GetBacklight();
        if (backlight != nullptr) {
            backlight->SetBrightness(static_cast<uint8_t>(clamped));
        }
        char msg[32];
        snprintf(msg, sizeof(msg), "Brillo: %d%%", clamped);
        Board::GetInstance().GetDisplay()->ShowNotification(msg, 1500);
    });
}

void MumaHub::AdjustVolume(int delta) {
    Application::GetInstance().Schedule([delta]() {
        auto* codec = Board::GetInstance().GetAudioCodec();
        if (codec == nullptr) {
            return;
        }
        int next_vol = std::clamp(codec->output_volume() + delta, 0, 100);
        codec->SetOutputVolume(next_vol);
        char msg[32];
        snprintf(msg, sizeof(msg), "Volumen: %d%%", next_vol);
        Board::GetInstance().GetDisplay()->ShowNotification(msg, 1500);
    });
}

void MumaHub::ToggleMicMute() {
    bool muted = !mic_muted_.load();
    mic_muted_.store(muted);
    Application::GetInstance().Schedule([muted]() {
        Application::GetInstance().GetAudioService().EnableWakeWordDetection(!muted);
        Board::GetInstance().GetDisplay()->ShowNotification(
            muted ? "Microfono silenciado" : "Microfono activo", 2000);
    });
    UpdateUiOverlay();
}

std::string MumaHub::GetIpAddress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ip_str_;
}

void MumaHub::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.muma.set_alarm",
        "Programa una alarma diaria en el robot MUMA (hora 0-23, minuto 0-59, etiqueta).",
        PropertyList({Property("hour", kPropertyTypeInteger, 0, 23),
                      Property("minute", kPropertyTypeInteger, 0, 59),
                      Property("label", kPropertyTypeString, std::string("Alarma"))}),
        [this](const PropertyList& props) -> ToolResult {
            int hour = props["hour"].value<int>();
            int minute = props["minute"].value<int>();
            std::string label = props["label"].value<std::string>();
            int slot = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (int i = 0; i < 3; ++i) {
                    if (!alarms_[i].enabled) {
                        slot = i;
                        break;
                    }
                }
                alarms_[slot].hour = hour;
                alarms_[slot].minute = minute;
                alarms_[slot].enabled = true;
                alarms_[slot].label = label;
                alarms_[slot].last_triggered_day = -1;
                SaveSettings();
            }
            char resp[80];
            snprintf(resp, sizeof(resp), "Alarma #%d programada a las %02d:%02d (%s).", slot + 1,
                     hour, minute, label.c_str());
            return std::string(resp);
        });

    mcp.AddTool(
        "self.muma.set_timer",
        "Inicia un temporizador en minutos en el robot MUMA.",
        PropertyList({Property("minutes", kPropertyTypeInteger, 1, 360),
                      Property("label", kPropertyTypeString, std::string("Temporizador"))}),
        [this](const PropertyList& props) -> ToolResult {
            int minutes = props["minutes"].value<int>();
            std::string label = props["label"].value<std::string>();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                timer_remaining_s_ = minutes * 60;
                timer_label_ = label;
                timer_active_ = true;
            }
            char resp[80];
            snprintf(resp, sizeof(resp), "Temporizador iniciado por %d minutos.", minutes);
            return std::string(resp);
        });

    mcp.AddTool(
        "self.muma.stop_alarm",
        "Apaga la alarma o temporizador que esta sonando actualmente.",
        PropertyList(),
        [this](const PropertyList&) -> ToolResult {
            StopAlarm();
            return std::string("Alarma detenida.");
        });

    mcp.AddTool(
        "self.muma.get_status",
        "Devuelve la direccion IP del panel web, clima actual, alarmas y estado del robot.",
        PropertyList(),
        [this](const PropertyList&) -> ToolResult {
            std::lock_guard<std::mutex> lock(mutex_);
            char buf[220];
            snprintf(buf, sizeof(buf),
                     "IP panel web: http://%s | Ciudad: %s | Clima: %.1f C (%s) | "
                     "Modo nocturno: %s (%02d:00-%02d:00) | Temporizador: %ds",
                     ip_str_.c_str(), city_.c_str(), temp_c_, WeatherCodeToSpanish(weather_code_),
                     night_enabled_ ? "activo" : "inactivo", night_start_hour_, night_end_hour_,
                     timer_active_ ? timer_remaining_s_ : 0);
            return std::string(buf);
        });
}

void MumaHub::StartHttpServer() {
    if (server_ != nullptr) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.max_uri_handlers = 8;

    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo iniciar el panel web");
        return;
    }

    httpd_uri_t u_root = {"/", HTTP_GET, HandleRoot, this};
    httpd_uri_t u_state = {"/api/state", HTTP_GET, HandleApiState, this};
    httpd_uri_t u_cfg = {"/api/config", HTTP_POST, HandleApiConfig, this};
    httpd_uri_t u_act = {"/api/action", HTTP_POST, HandleApiAction, this};
    httpd_register_uri_handler(server_, &u_root);
    httpd_register_uri_handler(server_, &u_state);
    httpd_register_uri_handler(server_, &u_cfg);
    httpd_register_uri_handler(server_, &u_act);
}

esp_err_t MumaHub::HandleRoot(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kWebPanelHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t MumaHub::HandleApiState(httpd_req_t* req) {
    auto* self = static_cast<MumaHub*>(req->user_ctx);
    time_t now = time(nullptr);
    struct tm t = {};
    localtime_r(&now, &t);

    char time_buf[16];
    char date_buf[24];
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d", t.tm_hour, t.tm_min);
    snprintf(date_buf, sizeof(date_buf), "%02d/%02d/%04d", t.tm_mday, t.tm_mon + 1,
             t.tm_year + 1900);

    cJSON* root = cJSON_CreateObject();
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        char w_buf[64];
        if (self->weather_valid_) {
            snprintf(w_buf, sizeof(w_buf), "%s: %.1f C, %s (%d%% hum)", self->city_.c_str(),
                     self->temp_c_, WeatherCodeToSpanish(self->weather_code_), self->humidity_);
        } else {
            snprintf(w_buf, sizeof(w_buf), "%s: actualizando...", self->city_.c_str());
        }
        cJSON_AddStringToObject(root, "time", time_buf);
        cJSON_AddStringToObject(root, "date", date_buf);
        cJSON_AddStringToObject(root, "weather", w_buf);
        cJSON_AddStringToObject(root, "city", self->city_.c_str());
        cJSON_AddStringToObject(root, "ip", self->ip_str_.c_str());
        cJSON_AddNumberToObject(root, "brightness", self->current_brightness_);
        cJSON_AddNumberToObject(root, "volume", Board::GetInstance().GetAudioCodec()->output_volume());
        cJSON_AddBoolToObject(root, "always_on", self->always_on_.load());
        cJSON_AddBoolToObject(root, "mic_muted", self->mic_muted_.load());
        cJSON_AddBoolToObject(root, "ringing", self->alarm_ringing_.load());
        cJSON_AddBoolToObject(root, "night_en", self->night_enabled_);
        cJSON_AddNumberToObject(root, "n_start", self->night_start_hour_);
        cJSON_AddNumberToObject(root, "n_end", self->night_end_hour_);
        cJSON_AddNumberToObject(root, "b_day", self->day_brightness_);
        cJSON_AddNumberToObject(root, "b_night", self->night_brightness_);
        cJSON_AddNumberToObject(root, "timer_s", self->timer_active_ ? self->timer_remaining_s_ : 0);

        cJSON* arr = cJSON_AddArrayToObject(root, "alarms");
        for (int i = 0; i < 3; ++i) {
            cJSON* it = cJSON_CreateObject();
            cJSON_AddBoolToObject(it, "en", self->alarms_[i].enabled);
            cJSON_AddNumberToObject(it, "h", self->alarms_[i].hour);
            cJSON_AddNumberToObject(it, "m", self->alarms_[i].minute);
            cJSON_AddStringToObject(it, "lbl", self->alarms_[i].label.c_str());
            cJSON_AddItemToArray(arr, it);
        }
    }

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t res = httpd_resp_send(req, json ? json : "{}", HTTPD_RESP_USE_STRLEN);
    if (json) free(json);
    return res;
}

esp_err_t MumaHub::HandleApiConfig(httpd_req_t* req) {
    auto* self = static_cast<MumaHub*>(req->user_ctx);
    size_t len = std::min<size_t>(req->content_len, 1024);
    std::string body(len, '\0');
    int received = httpd_req_recv(req, body.data(), len);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
    }
    body.resize(received);

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
    }

    std::string new_city;
    bool refresh_night = false;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        if (auto* v = cJSON_GetObjectItem(root, "always_on"); cJSON_IsBool(v)) {
            self->always_on_.store(cJSON_IsTrue(v));
        }
        if (auto* v = cJSON_GetObjectItem(root, "night_en"); cJSON_IsBool(v)) {
            self->night_enabled_ = cJSON_IsTrue(v);
            refresh_night = true;
        }
        if (auto* v = cJSON_GetObjectItem(root, "n_start"); cJSON_IsNumber(v)) {
            self->night_start_hour_ = std::clamp(v->valueint, 0, 23);
            refresh_night = true;
        }
        if (auto* v = cJSON_GetObjectItem(root, "n_end"); cJSON_IsNumber(v)) {
            self->night_end_hour_ = std::clamp(v->valueint, 0, 23);
            refresh_night = true;
        }
        if (auto* v = cJSON_GetObjectItem(root, "b_day"); cJSON_IsNumber(v)) {
            self->day_brightness_ = std::clamp(v->valueint, 5, 100);
            refresh_night = true;
        }
        if (auto* v = cJSON_GetObjectItem(root, "b_night"); cJSON_IsNumber(v)) {
            self->night_brightness_ = std::clamp(v->valueint, 1, 80);
            refresh_night = true;
        }
        if (auto* v = cJSON_GetObjectItem(root, "city"); cJSON_IsString(v) && v->valuestring[0]) {
            new_city = v->valuestring;
        }
        if (auto* arr = cJSON_GetObjectItem(root, "alarms"); cJSON_IsArray(arr)) {
            int n = std::min(3, cJSON_GetArraySize(arr));
            for (int i = 0; i < n; ++i) {
                cJSON* it = cJSON_GetArrayItem(arr, i);
                if (!cJSON_IsObject(it)) continue;
                if (auto* en = cJSON_GetObjectItem(it, "en"); cJSON_IsBool(en)) {
                    self->alarms_[i].enabled = cJSON_IsTrue(en);
                }
                if (auto* h = cJSON_GetObjectItem(it, "h"); cJSON_IsNumber(h)) {
                    self->alarms_[i].hour = std::clamp(h->valueint, 0, 23);
                }
                if (auto* m = cJSON_GetObjectItem(it, "m"); cJSON_IsNumber(m)) {
                    self->alarms_[i].minute = std::clamp(m->valueint, 0, 59);
                }
                if (auto* l = cJSON_GetObjectItem(it, "lbl"); cJSON_IsString(l)) {
                    self->alarms_[i].label = l->valuestring;
                }
                self->alarms_[i].last_triggered_day = -1;
            }
        }
        self->SaveSettings();
    }
    cJSON_Delete(root);

    if (!new_city.empty()) {
        double lat = 0.0, lon = 0.0;
        std::string resolved;
        if (self->GeocodeCity(new_city, lat, lon, resolved)) {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->city_ = resolved;
            self->latitude_ = lat;
            self->longitude_ = lon;
            self->weather_countdown_s_ = 0;
            self->SaveSettings();
        }
    }

    if (refresh_night) {
        self->CheckNightMode(true);
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t MumaHub::HandleApiAction(httpd_req_t* req) {
    auto* self = static_cast<MumaHub*>(req->user_ctx);
    size_t len = std::min<size_t>(req->content_len, 512);
    std::string body(len, '\0');
    int received = httpd_req_recv(req, body.data(), len);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
    }
    body.resize(received);

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
    }

    auto* cmd_item = cJSON_GetObjectItem(root, "cmd");
    auto* val_item = cJSON_GetObjectItem(root, "val");
    auto* txt_item = cJSON_GetObjectItem(root, "text");

    if (cJSON_IsString(cmd_item)) {
        std::string cmd = cmd_item->valuestring;
        int val = cJSON_IsNumber(val_item) ? val_item->valueint : 0;
        std::string txt = cJSON_IsString(txt_item) ? txt_item->valuestring : "";

        if (cmd == "toggle_clock") {
            self->ToggleClockView();
        } else if (cmd == "toggle_mic") {
            self->ToggleMicMute();
        } else if (cmd == "stop_alarm") {
            self->StopAlarm();
        } else if (cmd == "brightness") {
            self->SetBrightnessManual(val);
        } else if (cmd == "volume") {
            Application::GetInstance().Schedule([val]() {
                Board::GetInstance().GetAudioCodec()->SetOutputVolume(std::clamp(val, 0, 100));
            });
        } else if (cmd == "start_timer" && val > 0) {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->timer_remaining_s_ = std::clamp(val, 1, 360) * 60;
            self->timer_label_ = txt.empty() ? "Temporizador" : txt;
            self->timer_active_ = true;
        } else if (cmd == "cancel_timer") {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->timer_active_ = false;
            self->timer_remaining_s_ = 0;
        }
    }
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

std::string MumaHub::UrlEncode(const std::string& value) {
    std::string out;
    out.reserve(value.size() * 2);
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", c);
            out.append(hex);
        }
    }
    return out;
}

const char* MumaHub::WeatherCodeToSpanish(int code) {
    if (code == 0) return "Despejado";
    if (code <= 2) return "Parc. nublado";
    if (code == 3) return "Nublado";
    if (code == 45 || code == 48) return "Niebla";
    if (code >= 51 && code <= 57) return "Llovizna";
    if (code >= 61 && code <= 67) return "Lluvia";
    if (code >= 71 && code <= 77) return "Nieve";
    if (code >= 80 && code <= 82) return "Chubascos";
    if (code >= 95) return "Tormenta";
    return "Variable";
}
