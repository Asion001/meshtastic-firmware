#include "CardputerAdvAudioOutputI2S.h"

#if defined(M5STACK_CARDPUTER_ADV) && defined(ARCH_ESP32)

#include "DebugConfiguration.h"
#include <driver/i2s.h>
#include <esp_heap_caps.h>

// ESP8266Audio's ESP32 I2S backend continues into i2s_set_pin() after
// i2s_driver_install() fails. Cardputer ADV has no PSRAM and its shared SD
// support consumes some DMA-capable memory, so that behavior turns a harmless
// allocation failure into a null-pointer panic on the first keyboard sound.
CardputerAdvAudioOutputI2S::CardputerAdvAudioOutputI2S()
    : AudioOutputI2S(1, AudioOutputI2S::EXTERNAL_I2S, 4, AudioOutputI2S::APLL_DISABLE)
{
}

bool CardputerAdvAudioOutputI2S::begin()
{
    if (i2sOn)
        return true;

    const i2s_config_t config = {
        .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 44100,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 128,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        .bits_per_chan = I2S_BITS_PER_CHAN_DEFAULT,
    };

    esp_err_t result = i2s_driver_install(static_cast<i2s_port_t>(portNo), &config, 0, nullptr);
    if (result != ESP_OK) {
        LOG_ERROR("Cardputer ADV I2S unavailable (%d; DMA free=%u largest=%u)", static_cast<int>(result),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
        return false;
    }

    const i2s_pin_config_t pins = {
        .mck_io_num = mclkPin,
        .bck_io_num = bclkPin,
        .ws_io_num = wclkPin,
        .data_out_num = doutPin,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };
    result = i2s_set_pin(static_cast<i2s_port_t>(portNo), &pins);
    if (result == ESP_OK)
        result = i2s_zero_dma_buffer(static_cast<i2s_port_t>(portNo));
    if (result != ESP_OK) {
        LOG_ERROR("Cardputer ADV I2S setup failed (%d)", static_cast<int>(result));
        i2s_driver_uninstall(static_cast<i2s_port_t>(portNo));
        return false;
    }

    i2sOn = true;
    return SetRate(hertz);
}

#endif
