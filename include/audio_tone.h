#pragma once

#include <cstddef>
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
 * @brief 音声再生の現在状態
 */
struct Status {
    bool supported;        ///< このボードで音声が使えるか
    uint32_t queued;        ///< 再生待ちの音数
    uint8_t master_volume;  ///< master volume(0-100)
    Waveform waveform;      ///< 最後に push した波形
    Note last_note;         ///< 最後に push した音
};

/**
 * @brief 音声再生モジュールを初期化する
 *
 * Atom Lite / AtomS3 Lite + Atomic Voice Base では I2C / I2S / ES8311 を
 * 初期化し、再生専用タスクを起動する。
 */
void start(void);

/**
 * @brief 1 音（または無音）を再生キューへ追加する
 *
 * 再生は専用タスクで非同期に行われる。再生中でもキューへ追加でき、
 * 順に再生される。各音の最終音量は master volume と note.volume の積になる。
 *
 * @param waveform 波形
 * @param note     追加する音(周波数 0 は無音)
 * @return キューへ追加できたなら true(満杯なら false)
 */
bool push(Waveform waveform, const Note &note);

/**
 * @brief 再生待ちの音を全て破棄する
 *
 * 再生中の 1 音は最後まで再生される。
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
