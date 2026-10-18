// Board configuration derived from Waveshare's ESP32-S3 e-Paper 3.97 demo.
#include "axp_prot.h"
#include "i2c_bsp.h"
#include "esp_log.h"
#include "esp_err.h"
#include "XPowersAXP2101.tpp"

static const char *TAG = "axp2101";
static XPowersAXP2101 axp2101;

static int AXP2101_SLAVE_Read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len)
{
    esp_err_t ret = i2c_read_reg(devAddr, regAddr, data, len);
    if (ret == ESP_OK) {
        return 0;
    }
    ESP_LOGE(TAG, "I2C read failed: addr=0x%02X, reg=0x%02X, len=%d, err=%d", devAddr, regAddr, len, ret);
    return -1;
}

static int AXP2101_SLAVE_Write(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len)
{
    esp_err_t ret = i2c_write_reg(devAddr, regAddr, data, len);
    if (ret == ESP_OK) {
        return 0;
    }
    ESP_LOGE(TAG, "I2C write failed: addr=0x%02X, reg=0x%02X, len=%d, err=%d", devAddr, regAddr, len, ret);
    return -1;
}

void axp_init(void)
{
    ESP_ERROR_CHECK(axp2101.begin(AXP2101_SLAVE_ADDRESS, AXP2101_SLAVE_Read,
                                 AXP2101_SLAVE_Write) ? ESP_OK : ESP_FAIL);
    ESP_LOGI(TAG, "Init PMU SUCCESS!");

    // Preserve the official board's power and charging settings. Do not infer
    // unused rails from the removal of audio support.
    axp2101.setVbusVoltageLimit(XPOWERS_AXP2101_VBUS_VOL_LIM_4V36);
    axp2101.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_1500MA);
    axp2101.setSysPowerDownVoltage(2600);
    axp2101.setDC1Voltage(3300);
    axp2101.setALDO1Voltage(3300);
    axp2101.setALDO2Voltage(3300);
    axp2101.setALDO3Voltage(3300);

    axp2101.setPowerKeyPressOffTime(XPOWERS_POWEROFF_4S);
    axp2101.setPowerKeyPressOnTime(XPOWERS_POWERON_1S);

    axp2101.enableTemperatureMeasure();
    axp2101.enableBattDetection();
    axp2101.enableVbusVoltageMeasure();
    axp2101.enableBattVoltageMeasure();
    axp2101.enableSystemVoltageMeasure();
    axp2101.setChargingLedMode(XPOWERS_CHG_LED_OFF);

    axp2101.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
    axp2101.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_200MA);
    axp2101.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
    axp2101.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);

    axp2101.setButtonBatteryChargeVoltage(3000);
    axp2101.enableButtonBatteryCharge();
    axp2101.setLowBatWarnThreshold(10);
    axp2101.setLowBatShutdownThreshold(5);
    axp2101.fuelGaugeControl(true, true);
}

int get_battery_power(void)
{
    return axp2101.getBatteryPercent();
}

bool enapwrstate(uint8_t tab)
{
    switch (tab) {
        case DC1:
            return axp2101.enableDC1();
        case ALDO1:
            return axp2101.enableALDO1();
        case ALDO2:
            return axp2101.enableALDO2();
        case ALDO3:
            return axp2101.enableALDO3();
        case ALDO4:
            return axp2101.enableALDO4();
        default:
            return false;
    }
}

bool disapwrstate(uint8_t tab)
{
    switch (tab) {
        case DC1:
            ESP_LOGW(TAG, "DC1 must remain enabled for normal operation");
            return false;
        case ALDO1:
            return axp2101.disableALDO1();
        case ALDO2:
            return axp2101.disableALDO2();
        case ALDO3:
            return axp2101.disableALDO3();
        case ALDO4:
            return axp2101.disableALDO4();
        default:
            return false;
    }
}

void axp_pwr_off(void)
{
    // No audio cleanup or its 600 ms delay in this application.
    // The caller must finish display/I2C activity before cutting power.
    axp2101.shutdown();
}

bool get_usb_connected(void)
{
    return axp2101.isVbusIn();
}
