#include <Wire.h>
#include "es8311.h"

extern "C" esp_err_t gzeek_codec_write_reg(uint16_t address, uint8_t reg, uint8_t value) {
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0 ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t gzeek_codec_read_reg(uint16_t address, uint8_t reg, uint8_t* value) {
    Wire.beginTransmission(address);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return ESP_FAIL;
    }
    if (Wire.requestFrom(address, size_t{1}, true) != 1) {
        return ESP_FAIL;
    }
    *value = static_cast<uint8_t>(Wire.read());
    return ESP_OK;
}
