#include "audio_tone.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "esp_err.h"
#include "esp_log.h"
#include "llbeacon_board.h"

#if defined(LLBEACON_BOARD_ATOMS3_LITE)
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#endif

namespace llbeacon {
namespace audio_tone {

#if defined(LLBEACON_BOARD_ATOMS3_LITE)

#define AUDIO_TONE_SAMPLE_RATE 16000
#define AUDIO_TONE_CHANNELS 2
#define AUDIO_TONE_DURATION_MS 200
#define AUDIO_TONE_VOLUME 60
#define AUDIO_TONE_AMPLITUDE 12000
#define AUDIO_TONE_CHUNK_FRAMES 128

#define AUDIO_TONE_I2C_PORT I2C_NUM_0
#define AUDIO_TONE_I2C_FREQ_HZ 100000
#define AUDIO_TONE_ES8311_ADDR ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_TONE_PI4IOE_ADDR 0x43

static constexpr float kPi = 3.14159265358979f;
static const char *TAG = "audio_tone";

static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t pi4ioe_dev;
static i2s_chan_handle_t i2s_tx_handle;
static esp_codec_dev_handle_t codec_dev;
static bool audio_ready = false;

enum class Waveform { SINE, SQUARE, SAW };

struct Tone {
    Waveform waveform;
    uint32_t frequency_hz;
};

// ボタン短押のたびに順番に切り替えるトーン。
static constexpr Tone kTones[] = {
    {Waveform::SINE, 440},
    {Waveform::SQUARE, 440},
    {Waveform::SAW, 440},
    {Waveform::SINE, 880},
    {Waveform::SQUARE, 880},
    {Waveform::SAW, 880},
    {Waveform::SINE, 1320},
    {Waveform::SQUARE, 1320},
    {Waveform::SAW, 1320},
};
static constexpr size_t kToneCount = sizeof(kTones) / sizeof(kTones[0]);
static size_t tone_index = 0;

/**
 * @brief 波形名を表示用文字列へ変換する
 *
 * @param waveform 波形
 * @return 表示用文字列
 */
static const char *waveform_name(Waveform waveform)
{
    switch (waveform) {
    case Waveform::SINE:
        return "sine";
    case Waveform::SQUARE:
        return "square";
    case Waveform::SAW:
        return "saw";
    }
    return "unknown";
}

/**
 * @brief 波形と位相から -1.0..1.0 のサンプル値を計算する
 *
 * @param waveform 波形
 * @param phase    位相(0.0 <= phase < 2*pi)
 * @return サンプル値(-1.0..1.0)
 */
static float waveform_value(Waveform waveform, float phase)
{
    switch (waveform) {
    case Waveform::SINE:
        return std::sin(phase);
    case Waveform::SQUARE:
        return phase < kPi ? 1.0f : -1.0f;
    case Waveform::SAW:
        return (phase / kPi) - 1.0f;
    }
    return 0.0f;
}

/**
 * @brief I2C master bus を初期化する
 *
 * @return 成功なら ESP_OK
 */
static esp_err_t init_i2c(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = AUDIO_TONE_I2C_PORT,
        .sda_io_num = LLBEACON_VOICE_I2C_SDA_GPIO,
        .scl_io_num = LLBEACON_VOICE_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = true,
            .allow_pd = false,
        },
    };
    return i2c_new_master_bus(&bus_cfg, &i2c_bus);
}

/**
 * @brief PI4IOE5V6408 に 1 レジスタ書き込む
 *
 * @param reg   レジスタアドレス
 * @param value 書き込む値
 * @return 成功なら ESP_OK
 */
static esp_err_t pi4ioe_write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t data[2] = {reg, value};
    return i2c_master_transmit(pi4ioe_dev, data, sizeof(data), -1);
}

/**
 * @brief PI4IOE5V6408 を初期化してアンプのミュートを解除する
 *
 * Atomic Voice Base の NS4150B アンプは PI4IOE5V6408 経由で制御されている。
 * M5Atomic-EchoBase の pi4ioe_init() 相当の設定を行う。
 *
 * @return 成功なら true
 */
static bool init_pi4ioe(void)
{
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AUDIO_TONE_PI4IOE_ADDR,
        .scl_speed_hz = AUDIO_TONE_I2C_FREQ_HZ,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = false,
        },
    };
    if (i2c_master_bus_add_device(i2c_bus, &dev_cfg, &pi4ioe_dev) != ESP_OK) {
        ESP_LOGE(TAG, "failed to add PI4IOE device");
        return false;
    }

    const uint8_t init_seq[][2] = {
        {0x07, 0x00},  // IO_PP: high-impedance
        {0x0D, 0xFF},  // IO_PULLUP
        {0x03, 0x6F},  // IO_DIR: P0 output
        {0x05, 0xFF},  // IO_OUT: unmute
    };
    for (const auto &step : init_seq) {
        if (pi4ioe_write_reg(step[0], step[1]) != ESP_OK) {
            ESP_LOGE(TAG, "failed to write PI4IOE reg 0x%02X", step[0]);
            return false;
        }
    }

    return true;
}

/**
 * @brief I2S (STD, TX only) を初期化する
 *
 * @return 成功なら ESP_OK
 */
static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear_after_cb = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &i2s_tx_handle, nullptr);
    if (err != ESP_OK) {
        return err;
    }

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_TONE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = LLBEACON_I2S_BCK_GPIO,
            .ws = LLBEACON_I2S_WS_GPIO,
            .dout = LLBEACON_I2S_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    return i2s_channel_init_std_mode(i2s_tx_handle, &std_cfg);
}

/**
 * @brief ES8311 codec と esp_codec_dev を初期化する
 *
 * @return 成功なら true
 */
static bool init_codec(void)
{
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = AUDIO_TONE_I2C_PORT,
        .addr = AUDIO_TONE_ES8311_ADDR,
        .bus_handle = i2c_bus,
        .clock_speed_hz = AUDIO_TONE_I2C_FREQ_HZ,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == nullptr) {
        ESP_LOGE(TAG, "failed to create I2C control interface");
        return false;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = nullptr,
        .tx_handle = i2s_tx_handle,
        .clk_src = 0,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (data_if == nullptr) {
        ESP_LOGE(TAG, "failed to create I2S data interface");
        return false;
    }

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = nullptr,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = false,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {},
        .no_dac_ref = false,
        .mclk_div = 256,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    if (codec_if == nullptr) {
        ESP_LOGE(TAG, "failed to create ES8311 codec interface");
        return false;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    codec_dev = esp_codec_dev_new(&dev_cfg);
    if (codec_dev == nullptr) {
        ESP_LOGE(TAG, "failed to create codec device");
        return false;
    }

    if (esp_codec_dev_set_out_vol(codec_dev, AUDIO_TONE_VOLUME) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "failed to set output volume");
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = AUDIO_TONE_CHANNELS,
        .channel_mask = 0,
        .sample_rate = AUDIO_TONE_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    if (esp_codec_dev_open(codec_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "failed to open codec device");
        return false;
    }

    return true;
}

#endif  // LLBEACON_BOARD_ATOMS3_LITE

/** @copydoc start */
void start(void)
{
#if defined(LLBEACON_BOARD_ATOMS3_LITE)
    ESP_ERROR_CHECK(init_i2c());
    if (!init_pi4ioe()) {
        ESP_LOGE(TAG, "PI4IOE init failed; speaker may stay muted");
    }
    ESP_ERROR_CHECK(init_i2s());
    if (!init_codec()) {
        ESP_LOGE(TAG, "codec init failed; audio_tone disabled");
        return;
    }
    audio_ready = true;
#endif
}

/** @copydoc play_sample */
void play_sample(void)
{
#if defined(LLBEACON_BOARD_ATOMS3_LITE)
    if (!audio_ready || codec_dev == nullptr) {
        return;
    }

    const Tone &tone = kTones[tone_index];
    tone_index = (tone_index + 1) % kToneCount;
    ESP_LOGI(TAG, "play %s %luHz", waveform_name(tone.waveform),
             static_cast<unsigned long>(tone.frequency_hz));

    static int16_t chunk[AUDIO_TONE_CHUNK_FRAMES * AUDIO_TONE_CHANNELS];
    const uint32_t total_frames = AUDIO_TONE_SAMPLE_RATE * AUDIO_TONE_DURATION_MS / 1000;
    const float phase_step = 2.0f * kPi * tone.frequency_hz / AUDIO_TONE_SAMPLE_RATE;
    float phase = 0.0f;
    uint32_t frame = 0;

    while (frame < total_frames) {
        uint32_t frames = total_frames - frame;
        if (frames > AUDIO_TONE_CHUNK_FRAMES) {
            frames = AUDIO_TONE_CHUNK_FRAMES;
        }
        for (uint32_t i = 0; i < frames; i++) {
            const int16_t sample =
                static_cast<int16_t>(AUDIO_TONE_AMPLITUDE * waveform_value(tone.waveform, phase));
            chunk[i * AUDIO_TONE_CHANNELS] = sample;
            chunk[i * AUDIO_TONE_CHANNELS + 1] = sample;
            phase += phase_step;
            if (phase >= 2.0f * kPi) {
                phase -= 2.0f * kPi;
            }
        }
        const int bytes = static_cast<int>(frames * AUDIO_TONE_CHANNELS * sizeof(int16_t));
        if (esp_codec_dev_write(codec_dev, chunk, bytes) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "failed to write tone data");
            return;
        }
        frame += frames;
    }

    // DMA に残った音を落ち着かせるため、短い無音を流す。
    std::memset(chunk, 0, sizeof(chunk));
    esp_codec_dev_write(codec_dev, chunk, sizeof(chunk));
#endif
}

}  // namespace audio_tone
}  // namespace llbeacon
