#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

struct MumaAlarm {
    int hour = 7;
    int minute = 0;
    bool enabled = false;
    std::string label = "Alarma";
    int last_triggered_day = -1;
};

class MumaHub {
public:
    static MumaHub& GetInstance() {
        static MumaHub instance;
        return instance;
    }

    MumaHub(const MumaHub&) = delete;
    MumaHub& operator=(const MumaHub&) = delete;

    void Start();
    void RegisterMcpTools();

    // Controles rápidos (gestos táctiles, botones, MCP y panel web)
    void ToggleClockView();
    void SetClockView(bool enabled);
    void AdjustBrightness(int delta);
    void SetBrightnessManual(int value);
    void AdjustVolume(int delta);
    void ToggleMicMute();
    void StopAlarm();

    bool IsAlwaysOn() const { return always_on_.load(); }
    bool IsMicMuted() const { return mic_muted_.load(); }
    bool IsAlarmRinging() const { return alarm_ringing_.load(); }
    std::string GetIpAddress() const;

private:
    MumaHub() = default;

    static void MainTaskEntry(void* arg);
    static void WeatherTaskEntry(void* arg);
    void MainLoop();
    void FetchWeatherNow();
    bool GeocodeCity(const std::string& city_name, double& out_lat, double& out_lon,
                     std::string& out_resolved_name);

    void LoadSettings();
    void SaveSettings();
    void InitUiOverlay();
    void UpdateUiOverlay();
    void CheckAlarmsAndTimer();
    void CheckNightMode(bool force);
    void TriggerAlarm(const std::string& reason);
    void StartNetworkServices();
    void StartHttpServer();

    static esp_err_t HandleRoot(httpd_req_t* req);
    static esp_err_t HandleApiState(httpd_req_t* req);
    static esp_err_t HandleApiConfig(httpd_req_t* req);
    static esp_err_t HandleApiAction(httpd_req_t* req);

    static std::string UrlEncode(const std::string& value);
    static const char* WeatherCodeToSpanish(int code);

    mutable std::mutex mutex_;
    TaskHandle_t main_task_ = nullptr;
    TaskHandle_t weather_task_ = nullptr;
    httpd_handle_t server_ = nullptr;

    // Configuración persistente (valores iniciales por defecto)
    std::string city_ = "San Nicolas de los Garza";
    double latitude_ = 25.7417;
    double longitude_ = -100.3022;
    bool night_enabled_ = true;
    int night_start_hour_ = 19;  // 7:00 PM
    int night_end_hour_ = 8;     // 8:00 AM
    int day_brightness_ = 80;
    int night_brightness_ = 10;
    int current_brightness_ = 80;
    int last_night_state_ = -1;

    std::atomic<bool> always_on_{true};
    std::atomic<bool> mic_muted_{false};
    std::atomic<bool> clock_view_enabled_{true};
    std::atomic<bool> alarm_ringing_{false};
    std::atomic<bool> weather_busy_{false};

    // Estado de clima y red
    bool network_started_ = false;
    bool weather_valid_ = false;
    float temp_c_ = 0.0f;
    int humidity_ = 0;
    int weather_code_ = 0;
    std::string ip_str_ = "Sin Wi-Fi";
    int weather_countdown_s_ = 0;

    // Alarmas y temporizador
    MumaAlarm alarms_[3];
    bool timer_active_ = false;
    int timer_remaining_s_ = 0;
    std::string timer_label_ = "Temporizador";
    std::string ringing_reason_ = "Alarma";
    int ringing_elapsed_ms_ = 0;
    int ringing_sound_cooldown_ms_ = 0;

    // Objetos LVGL (capa superpuesta en reposo)
    bool ui_ready_ = false;
    bool overlay_visible_ = false;
    lv_obj_t* clock_layer_ = nullptr;
    lv_obj_t* lbl_time_ = nullptr;
    lv_obj_t* lbl_date_ = nullptr;
    lv_obj_t* lbl_weather_ = nullptr;
    lv_obj_t* lbl_city_ = nullptr;
    lv_obj_t* lbl_footer_ = nullptr;
    lv_obj_t* lbl_badge_ = nullptr;
    lv_obj_t* alarm_banner_ = nullptr;
    lv_obj_t* lbl_alarm_text_ = nullptr;
};
