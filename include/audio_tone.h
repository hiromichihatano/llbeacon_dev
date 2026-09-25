#pragma once

namespace llbeacon {
namespace audio_tone {

/**
 * @brief スピーカー(Atomic Voice Base)を初期化する
 *
 * AtomS3 Lite + Atomic Voice Base (A149) でのみ有効。それ以外のボードでは
 * 何もしない。I2C/ES8311/PI4IOE/I2S を初期化し、小音量で再生可能な状態にする。
 *
 * @note app_main() から一度だけ呼び出すこと。
 */
void start(void);

/**
 * @brief 短いトーンを再生する
 *
 * 呼び出しごとに波形(sine / square / saw)と周波数を順番に切り替えて再生する。
 * start() が成功していない場合は何もしない。
 */
void play_sample(void);

}  // namespace audio_tone
}  // namespace llbeacon
