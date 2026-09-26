#include "audio_tone.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "esp_err.h"
#include "esp_log.h"
#include "llbeacon_board.h"

#if defined(LLBEACON_BOARD_ATOMS3_LITE)
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#endif

namespace llbeacon {
namespace audio_tone {

const char *waveform_name(Waveform waveform)
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

#if defined(LLBEACON_BOARD_ATOMS3_LITE)

#define AUDIO_TONE_SAMPLE_RATE 16000
#define AUDIO_TONE_CHANNELS 2
#define AUDIO_TONE_BASE_AMPLITUDE 20000
#define AUDIO_TONE_DEFAULT_MASTER_VOLUME 60
#define AUDIO_TONE_CODEC_VOLUME 60
#define AUDIO_TONE_CHUNK_FRAMES 512
#define AUDIO_TONE_REPEAT_GAP_MS 50

#define AUDIO_TONE_I2C_PORT I2C_NUM_0
#define AUDIO_TONE_I2C_FREQ_HZ 100000
#define AUDIO_TONE_ES8311_ADDR ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_TONE_PI4IOE_ADDR 0x43

#define AUDIO_TONE_BEEP_QUEUE_LENGTH 1
#define AUDIO_TONE_TASK_STACK_SIZE 4096
#define AUDIO_TONE_TASK_PRIORITY (tskIDLE_PRIORITY + 2)

static constexpr float kPi = 3.14159265358979f;
static constexpr float k2Pi = 2.0f * kPi;
static const char *TAG = "audio_tone";

static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t pi4ioe_dev;
static i2s_chan_handle_t i2s_tx_handle;
static esp_codec_dev_handle_t codec_dev;
static bool audio_ready = false;

static QueueHandle_t beep_queue;
static TaskHandle_t beep_task_handle;
static std::atomic<bool> playing{false};
static std::atomic<bool> stop_requested{false};
static std::atomic<uint32_t> master_volume{AUDIO_TONE_DEFAULT_MASTER_VOLUME};

static BeepConfig last_config = {
    .waveform = Waveform::SINE,
    .frequency_hz = 880,
    .duration_ms = 200,
    .count = 1,
    .volume = 100,
};

/**
 * @brief PolyBLEP によるエッジ補正値を計算する
 *
 * 矩形波 / saw wave の不連続点（エッジ）で発生する折り返しノイズ
 * （エッジジッター）を抑えるため、エッジ付近のサンプルへ加算する補正項。
 *
 * @param t  補正対象サンプルの位相(0.0 <= t < 1.0、1周期を 1 とする)
 * @param dt 1サンプルあたりの位相増分(0.0 < dt < 1.0)
 * @return 補正値
 */
static float poly_blep(float t, float dt)
{
    if (dt <= 0.0f) {
        return 0.0f;
    }

    // 立ち上がりエッジ（t=0）直後のサンプルを補正する。
    if (t < dt) {
        const float s = t / dt;
        return s + s - s * s - 1.0f;
    }

    // 立ち下がりエッジ（t=1）直前のサンプルを補正する。
    if (t > 1.0f - dt) {
        const float s = (t - 1.0f) / dt;
        return s * s + s + s + 1.0f;
    }

    return 0.0f;
}

/**
 * @brief 波形と位相から -1.0..1.0 のサンプル値を計算する
 *
 * SINE は素直な sin を返す。SQUARE / SAW はナイーブ波形へ PolyBLEP 補正を
 * 加え、エイリアシングを抑えた band-limited 相当の波形にする。
 *
 * @param waveform 波形
 * @param phase    位相(0.0 <= phase < 2*pi)
 * @param dt       1サンプルあたりの位相増分(0.0 <= phase < 1.0 表記)
 * @return サンプル値(-1.0..1.0)
 */
static float waveform_value(Waveform waveform, float phase, float dt)
{
    switch (waveform) {
    case Waveform::SINE:
        return std::sin(phase);
    case Waveform::SQUARE: {
        const float t = phase / k2Pi;
        float value = phase < kPi ? 1.0f : -1.0f;
        // 立ち上がり(t=0)と立ち下がり(t=0.5)の両エッジを補正する。
        value += poly_blep(t, dt);
        float falling_edge = t + 0.5f;
        if (falling_edge >= 1.0f) {
            falling_edge -= 1.0f;
        }
        value -= poly_blep(falling_edge, dt);
        return value;
    }
    case Waveform::SAW: {
        const float t = phase / k2Pi;
        const float value = (2.0f * t) - 1.0f;
        // 立ち下がり(t=0)のエッジを補正する。
        return value - poly_blep(t, dt);
    }
    }
    return 0.0f;
}

/**
 * @brief 波形ごとの振幅補正係数を返す
 *
 * square は sine より実効エネルギーが高いため、sine と揃える係数を掛ける。
 *
 * @param waveform 波形
 * @return 振幅補正係数
 */
static float waveform_scale(Waveform waveform)
{
    switch (waveform) {
    case Waveform::SINE:
        return 1.0f;
    case Waveform::SQUARE:
        return 0.7071f;
    case Waveform::SAW:
        return 1.0f;
    }
    return 1.0f;
}

/**
 * @brief 指定フレーム数の無音を I2S へ書き込む
 *
 * @param frames 無音フレーム数
 * @return 成功なら true
 */
static bool write_silence(uint32_t frames)
{
    static int16_t zero[AUDIO_TONE_CHUNK_FRAMES * AUDIO_TONE_CHANNELS];
    std::memset(zero, 0, sizeof(zero));

    while (frames > 0) {
        uint32_t chunk_frames = frames;
        if (chunk_frames > AUDIO_TONE_CHUNK_FRAMES) {
            chunk_frames = AUDIO_TONE_CHUNK_FRAMES;
        }
        const int bytes = static_cast<int>(chunk_frames * AUDIO_TONE_CHANNELS * sizeof(int16_t));
        if (esp_codec_dev_write(codec_dev, zero, bytes) != ESP_CODEC_DEV_OK) {
            return false;
        }
        frames -= chunk_frames;
    }
    return true;
}

/**
 * @brief beep 再生専用タスク本体
 *
 * キュー経由で受け取った BeepConfig を再生する。STOP 要求は
 * stop_requested フラグで受け取り、チャンク境界で停止する。
 *
 * @param arg 未使用
 */
static void audio_tone_task(void *arg)
{
    (void)arg;

    // 波形バッファはタスクスタックに置くと大きいため static にする。
    static int16_t chunk[AUDIO_TONE_CHUNK_FRAMES * AUDIO_TONE_CHANNELS];
    BeepConfig config;

    while (true) {
        // 1件の beep 要求を受け取るまで待機する。
        if (xQueueReceive(beep_queue, &config, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // 再生中フラグを立てて、次の beep() 要求を拒否させる。
        playing.store(true);

        // 最終音量 = master volume x beep volume を線形ゲインとして計算し、
        // 波形ごとの補正係数を掛けて PCM 振幅を決める。
        const float gain = (static_cast<float>(master_volume.load()) / 100.0f) *
                           (static_cast<float>(config.volume) / 100.0f);
        const int16_t amplitude = static_cast<int16_t>(
            AUDIO_TONE_BASE_AMPLITUDE * gain * waveform_scale(config.waveform));

        // 1サンプルごとに進める位相と、beep 1回分・繰り返し間の無音のフレーム数。
        // PolyBLEP 用に、位相増分を 1周期=1.0 表記へ変換しておく。
        const float phase_step = k2Pi * config.frequency_hz / AUDIO_TONE_SAMPLE_RATE;
        const float blep_dt = phase_step / k2Pi;
        const uint32_t tone_frames = AUDIO_TONE_SAMPLE_RATE * config.duration_ms / 1000;
        const uint32_t gap_frames = AUDIO_TONE_SAMPLE_RATE * AUDIO_TONE_REPEAT_GAP_MS / 1000;

        bool write_failed = false;

        // 指定回数分の beep を繰り返す。stop 要求が来たらループを抜ける。
        for (uint32_t repeat = 0; repeat < config.count && !stop_requested.load(); repeat++) {
            // 2回目以降は、beep 同士がつながらないよう短い無音を挟む。
            if (repeat > 0 && gap_frames > 0 && !write_silence(gap_frames)) {
                write_failed = true;
                break;
            }

            // 1回の beep を生成する。phase はチャンクをまたいで引き継ぎ、
            // 途中でリセットしない（つなぎ目の不連続を防ぐ）。
            float phase = 0.0f;
            uint32_t frame = 0;
            while (frame < tone_frames && !stop_requested.load()) {
                // 残りフレームをチャンク単位に分割して書き込む。
                uint32_t frames = tone_frames - frame;
                if (frames > AUDIO_TONE_CHUNK_FRAMES) {
                    frames = AUDIO_TONE_CHUNK_FRAMES;
                }
                // 左右同じサンプルを入れ、ステレオとして書き込む。
                for (uint32_t i = 0; i < frames; i++) {
                    const int16_t sample = static_cast<int16_t>(
                        amplitude * waveform_value(config.waveform, phase, blep_dt));
                    chunk[i * AUDIO_TONE_CHANNELS] = sample;
                    chunk[i * AUDIO_TONE_CHANNELS + 1] = sample;
                    phase += phase_step;
                    // phase を 0..2pi の範囲に保つ。
                    if (phase >= k2Pi) {
                        phase -= k2Pi;
                    }
                }
                const int bytes =
                    static_cast<int>(frames * AUDIO_TONE_CHANNELS * sizeof(int16_t));
                if (esp_codec_dev_write(codec_dev, chunk, bytes) != ESP_CODEC_DEV_OK) {
                    write_failed = true;
                    stop_requested.store(true);
                    break;
                }
                frame += frames;
            }
        }

        if (write_failed) {
            ESP_LOGE(TAG, "failed to write tone data");
        }

        // DMA に残った音を落ち着かせるため、短い無音を流す。
        write_silence(AUDIO_TONE_CHUNK_FRAMES);

        // 再生完了（または停止）。次の beep を受け付けられる状態へ戻す。
        stop_requested.store(false);
        playing.store(false);
    }
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

    if (esp_codec_dev_set_out_vol(codec_dev, AUDIO_TONE_CODEC_VOLUME) != ESP_CODEC_DEV_OK) {
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

/** @copydoc start */
void start(void)
{
    // Voice Base に必要な周辺を順に初期化する。
    ESP_ERROR_CHECK(init_i2c());
    if (!init_pi4ioe()) {
        // PI4IOE が無くても codec 初期化は試す。失敗するとミュートのままになる。
        ESP_LOGE(TAG, "PI4IOE init failed; speaker may stay muted");
    }
    ESP_ERROR_CHECK(init_i2s());
    if (!init_codec()) {
        ESP_LOGE(TAG, "codec init failed; audio_tone disabled");
        return;
    }

    // beep 要求を再生タスクへ渡すキュー。同時再生は 1 件だけなので長さ 1。
    beep_queue = xQueueCreate(AUDIO_TONE_BEEP_QUEUE_LENGTH, sizeof(BeepConfig));
    if (beep_queue == nullptr) {
        ESP_LOGE(TAG, "failed to create beep queue");
        return;
    }

    // CLI タスクをブロックしないよう、再生は専用タスクで行う。
    if (xTaskCreate(audio_tone_task, "audio_tone", AUDIO_TONE_TASK_STACK_SIZE, nullptr,
                    AUDIO_TONE_TASK_PRIORITY, &beep_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "failed to create audio_tone task");
        vQueueDelete(beep_queue);
        beep_queue = nullptr;
        return;
    }

    audio_ready = true;
    ESP_LOGI(TAG, "audio_tone ready");
}

/** @copydoc beep */
bool beep(const BeepConfig &config)
{
    // 初期化済みで、かつ現在再生していない場合だけ要求を受け付ける。
    if (!audio_ready || playing.load()) {
        return false;
    }

    // CLI 以外から直接呼ばれても安全なように、音量だけは範囲を正規化する。
    BeepConfig clamped = config;
    if (clamped.volume > 100) {
        clamped.volume = 100;
    }

    // 前回の stop 要求を新しい再生に持ち越さないため、受付時にクリアする。
    // キューが満杯で受け付けられなかった場合は、保留中の stop 要求を元に戻す。
    const bool stop_was_requested = stop_requested.load();
    stop_requested.store(false);

    if (xQueueSend(beep_queue, &clamped, 0) != pdTRUE) {
        if (stop_was_requested) {
            stop_requested.store(true);
        }
        return false;
    }

    // 受け付けた設定を status 用に覚えておく。
    last_config = clamped;
    return true;
}

/** @copydoc stop */
void stop(void)
{
    // 再生タスクがチャンク境界でこのフラグを見て停止する。
    stop_requested.store(true);
}

/** @copydoc set_master_volume */
void set_master_volume(uint8_t volume)
{
    if (volume > 100) {
        volume = 100;
    }
    master_volume.store(volume);
}

/** @copydoc get_status */
Status get_status(void)
{
    const Status status = {
        .supported = audio_ready,
        .playing = playing.load(),
        .master_volume = static_cast<uint8_t>(master_volume.load()),
        .waveform = last_config.waveform,
        .frequency_hz = last_config.frequency_hz,
        .duration_ms = last_config.duration_ms,
        .count = last_config.count,
        .beep_volume = last_config.volume,
    };
    return status;
}

/** @copydoc is_supported */
bool is_supported(void)
{
    return audio_ready;
}

#else  // !LLBEACON_BOARD_ATOMS3_LITE

/** @copydoc start */
void start(void)
{
}

/** @copydoc beep */
bool beep(const BeepConfig &config)
{
    (void)config;
    return false;
}

/** @copydoc stop */
void stop(void)
{
}

/** @copydoc set_master_volume */
void set_master_volume(uint8_t volume)
{
    (void)volume;
}

/** @copydoc get_status */
Status get_status(void)
{
    const Status status = {
        .supported = false,
        .playing = false,
        .master_volume = 0,
        .waveform = Waveform::SINE,
        .frequency_hz = 880,
        .duration_ms = 200,
        .count = 1,
        .beep_volume = 100,
    };
    return status;
}

/** @copydoc is_supported */
bool is_supported(void)
{
    return false;
}

#endif  // LLBEACON_BOARD_ATOMS3_LITE

}  // namespace audio_tone
}  // namespace llbeacon
