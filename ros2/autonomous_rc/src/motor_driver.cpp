// =========================================================
// motor_driver.cpp — sysfs GPIO + PWM 제어 구현 (Fast I/O 최적화 적용)
//   Xavier NX: GPIO는 Tegra 이름(PR.04 등)으로 접근
//   [핵심 최적화] 파일 상시 개방(Persistent Stream) 및 seekp(0) 덮어쓰기
//   [핵심 최적화] 커널 비동기 생성 지연 극복을 위한 Spin-lock 적용
// =========================================================
#include "autonomous_rc/motor_driver.hpp"
#include "autonomous_rc/config.hpp"

#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <cmath>

namespace arc {

// ─── sysfs 파일 1회성 쓰기 헬퍼 (초기 direction/export 설정용) ───
static bool write_file(const std::string& path, const std::string& val) {
    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        std::cerr << "[GPIO/PWM] Cannot write " << path << "\n";
        return false;
    }
    ofs << val;
    ofs.close();
    return true;
}

// ═════════════════════════════════════════════
// SysfsGpio (Tegra 이름 기반)
// ═════════════════════════════════════════════
SysfsGpio::SysfsGpio(const std::string& tegra_name)
    : name_(tegra_name) {
    base_path_ = "/sys/class/gpio/" + name_;
}

SysfsGpio::~SysfsGpio() {
    if (value_stream_.is_open()) value_stream_.close();
    if (exported_) unexport_pin();
}

bool SysfsGpio::export_pin() {
    std::ifstream test(base_path_ + "/direction");
    if (!test.good()) {
        std::cerr << "[GPIO] " << name_ << " not found. Run Python GPIO setup first.\n";
        return false;
    }
    exported_ = true;
    std::string val_path = base_path_ + "/value";
    int retries = 0;
    while (retries < 50) { 
        value_stream_.open(val_path);
        if (value_stream_.is_open()) break; // 성공 시 즉시 탈출
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        retries++;
    }

    if (!value_stream_.is_open()) {
        std::cerr << "\n❌ [FATAL] GPIO " << name_ << " value 스트림 열기 실패! (Spin-lock 타임아웃)\n";
        return false;
    }
    return true;
}

bool SysfsGpio::set_direction_out() {
    return write_file(base_path_ + "/direction", "out");
}

bool SysfsGpio::write_value(int val) {
    if (!value_stream_.is_open()) return false;
    value_stream_.seekp(0);
    value_stream_ << val;
    value_stream_.flush();
    return true;
}

void SysfsGpio::unexport_pin() {
    exported_ = false;
}

// ═════════════════════════════════════════════
// SysfsPwm
// ═════════════════════════════════════════════
SysfsPwm::SysfsPwm(const std::string& chip_path, int channel)
    : channel_(channel) {
    base_ = chip_path + "/pwm" + std::to_string(channel_);
}

SysfsPwm::~SysfsPwm() {
    cleanup();
}

bool SysfsPwm::export_channel() {
    std::ifstream test(base_ + "/period");
    if (!test.good()) {
        std::string chip_path = base_.substr(0, base_.rfind("/pwm"));
        if (!write_file(chip_path + "/export", std::to_string(channel_))) {
            std::cerr << "[PWM] Failed to export ch " << channel_ << " on " << chip_path << "\n";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    exported_ = true;

    // PWM Duty Cycle 제어를 위한 Spin-lock 스트림 개방
    std::string duty_path = base_ + "/duty_cycle";
    int retries = 0;
    while (retries < 50) {
        duty_stream_.open(duty_path);
        if (duty_stream_.is_open()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        retries++;
    }

    if (!duty_stream_.is_open()) {
        std::cerr << "\n❌ [FATAL] PWM ch " << channel_ << " duty_cycle 스트림 열기 실패!\n";
        return false;
    }
    return true;
}

bool SysfsPwm::init(int freq_hz) {
    if (!export_channel()) return false;

    period_ns_ = static_cast<int>(1e9 / freq_hz);

    write_attr("duty_cycle", "0");
    write_attr("period", std::to_string(period_ns_));
    write_attr("duty_cycle", "0");
    write_attr("enable", "1");

    std::cout << "[PWM] " << base_ << " init: freq=" << freq_hz
              << "Hz period=" << period_ns_ << "ns\n";
    return true;
}

void SysfsPwm::set_duty_pct(double duty_pct) {
    duty_pct = std::max(0.0, std::min(100.0, duty_pct));
    int duty_ns = static_cast<int>(period_ns_ * duty_pct / 100.0);
    
    // Fast I/O 덮어쓰기 적용
    if (duty_stream_.is_open()) {
        duty_stream_.seekp(0);
        duty_stream_ << duty_ns;
        duty_stream_.flush();
    }
}

void SysfsPwm::enable() {
    write_attr("enable", "1");
}

void SysfsPwm::disable() {
    set_duty_pct(0.0);
    write_attr("enable", "0");
}

void SysfsPwm::cleanup() {
    if (exported_) {
        disable();
        if (duty_stream_.is_open()) duty_stream_.close();
        
        std::string chip_path = base_.substr(0, base_.rfind("/pwm"));
        write_file(chip_path + "/unexport", std::to_string(channel_));
        exported_ = false;
    }
}

void SysfsPwm::write_attr(const std::string& attr, const std::string& val) {
    write_file(base_ + "/" + attr, val);
}

// ═════════════════════════════════════════════
// DcMotor & ContinuousServo 로직 (기존과 동일하므로 하단 생략)
// ═════════════════════════════════════════════
// ... (이하 DcMotor 및 ContinuousServo 클래스의 구현부는 
//      이미 Fast I/O가 적용된 SysfsGpio와 SysfsPwm을 호출하므로 
//      수정 없이 기존 코드를 그대로 사용하시면 됩니다.) ...

}  // namespace arc
