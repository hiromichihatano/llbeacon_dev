#pragma once

#include <cstdint>

namespace llbeacon {
namespace audio_tone {

/**
 * @brief 再生可能な波形
 */
enum class Waveform { SINE, SQUARE, SAW };

/**
 * @brief 1 command で再生できる note の最大数
 */
static constexpr size_t kMaxNotes = 16;

/**
 * @brief シーケンスを構成する 1 音（または無音）
 */
struct Note {
    uint32_t frequency_hz;  ///< 周波数(Hz)。0 は無音
    uint32_t duration_ms;   ///< 長さ(ms)
    uint8_t volume;         ///< 個別音量(0-100)
};

/**
 * @brief 1回の tone 再生要求の設定
 */
struct ToneConfig {
    Waveform waveform;      ///< 波形
    uint32_t note_count;    ///< 有効な note 数(1..kMaxNotes)
    Note notes[kMaxNotes];  ///< 先頭から順に再生する note
};

/**
 * @brief 音声再生の現在状態
 */
struct Status {
    bool supported;          ///< このボードで音声が使えるか
    bool playing;            ///< 再生中か
    uint8_t master_volume;   ///< master volume(0-100)
    ToneConfig last_tone;    ///< 最後に受け付けた tone 設定
};

/**
 * @brief 音声再生モジュールを初期化する
 *
 * AtomS3 Lite + Atomic Voice Base では I2C / I2S / ES8311 を初期化し、
 * 再生専用タスクを起動する。他のボードでは何もしない。
 */
void start(void);

/**
 * @brief tone シーケンスを再生要求する
 *
 * 再生は専用タスクで非同期に行われる。再生中に呼んだ場合は false を返す。
 * 各 note の最終音量は master volume と note.volume の積になる。
 *
 * @param config 再生設定
 * @return 要求を受け付けたなら true
 */
bool tone(const ToneConfig &config);

/**
 * @brief 再生中の tone を停止する
 */
void stop(void);

/**
 * @brief master volume を設定する
 *
 * @param volume master volume(0-100、範囲外は 0-100 に丸める)
 */
void set_master_volume(uint8_t volume);

/**
 * @brief 現在状態を取得する
 *
 * @return 現在状態のスナップショット
 */
Status get_status(void);

/**
 * @brief このボードで音声再生が利用可能か返す
 *
 * @return 利用可能なら true
 */
bool is_supported(void);

/**
 * @brief 波形名を表示用文字列へ変換する
 *
 * @param waveform 波形
 * @return 表示用文字列
 */
const char *waveform_name(Waveform waveform);

}  // namespace audio_tone
}  // namespace llbeacon
