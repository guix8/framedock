# サードパーティーに関する表示

[English](THIRD_PARTY_LICENSES.md)

この文書は第三者由来のコードと依存関係の説明です．ライセンス本文の日本語訳では
ありません．再配布条件は同梱した各ライセンスの原文を参照してください．

## [Frame Perf Overlay](https://github.com/sasaken1102r/frame-perf-overlay)（MIT）

`perf/` は sasaken1102r と貢献者による Frame Perf Overlay v0.4.0 を `git subtree`
で取り込んだものを元に，上流のコミット `066a5c8` の機能（PR #5 の表示: CPU・メモリの
上位，小さく表示しないカード，システムを隠す）を手で移し，Frame Dock 用に変更した
ものです（色補正ページ，韓国語，自動更新の削除，framedock という名前）．Frame Dock のダッシュボードのタブ，性能パネル，
描画，Vulkan テクスチャのアップロード，OpenVR オーバーレイのコードはここから
来ています．MIT ライセンスは [`perf/LICENSE`](perf/LICENSE)，その第三者表示
（OpenVR ヘッダー，LGPL-2.1 の libnl，フォント）は
[`perf/THIRD_PARTY_LICENSES.md`](perf/THIRD_PARTY_LICENSES.md) にあります．

## [frameeyeosc](https://github.com/sasaken1102r/frameeyeosc)（MIT）

Frame Dock 以前の FrameColor では，ダッシュボードの描画，Vulkanテクスチャの
アップロード，OpenVRオーバーレイを扱う `src/draw.*`，`src/vk_texture.*`，
`src/vr_overlay.*` がframeeyeoscのコードを改変したもので，ダッシュボードの
デザインも参考にしました．Frame Dock では代わりに Frame Perf Overlay の同じ
役割のコードを使います．視線に合わせたシャープは frameeyeosc の視線追跡の処理を
改変したものです：SteamVR の eye-server 共有メモリの読み取り（`src/eye_server.*`），
視線の角度，なめらかにする処理とまばたきの扱い（`src/gaze_map.*` の
`GazeSmoother`，1-euro フィルター，デッドゾーン），補正の点の測り方と装着時の
中心の取り直しの規則（`src/gaze_fit.*`．9 点の格子による補正そのものは
Frame Dock 独自です）．原作者はkonsti219で，リンク先の
フォークにはsasaken1102rによる変更が含まれます．著作権表示と許諾文は
[`third_party/frameeyeosc-LICENSE`](third_party/frameeyeosc-LICENSE) に保存しています．

## [Frametop](https://github.com/Frametop/frametop)（MIT）

ゲーム中にもう一方の手のレーザーで Frame Dock を操作する機能（ドック）は，
DeeJanuz による Frametop 0.2.2 が Steam Frame の SteamVR について調べたことと，
ゲーム中にコントローラーでパネルを使えるようにする `screens/vr.cpp` の方法に
ならっています．手のレーザーがパネルを指している間だけ
`VROverlayFlags_MakeOverlaysInteractiveIfVisible` を立てること（離れるときは
入るときより広い余白と 0.3 秒の猶予），レーザーの向きをレンダーモデルの `tip`
コンポーネントから `GetComponentState` で求めること（ゲーム中は
`GetComponentStateForDevicePath` が失敗する），オーバーレイのマウススケールを
テクスチャの大きさにして SteamVR の当たり判定をパネルに合わせること，つかんだ
コントローラーにパネルを固定して運ぶことです．レーザーの先端の取得
（`VrOverlay::tipOffset`）と平らなパネルへの光線の当たり（`dockmath::hitPanel`）は
`TipOffset` / `LaserPose` と `RayOnSurface` を改変したものです．ブラウザーパネルの
VR キーボード（`browser/vr_keyboard.*`）は Frametop の `screens/keyboard.cpp` の設計
（CPU で描くキーボード，キー単位の段，持って運べる上端のバー，レーザーで入力）に
ならっています．コードは新しく書いたものです（cairo，韓国語の配列）．著作権表示と許諾文は
[`third_party/frametop-LICENSE`](third_party/frametop-LICENSE) に保存しています．

## libjpeg-turbo（システムのライブラリ）

ブラウザーパネルは Chrome の画面配信の JPEG を SteamOS の libjpeg-turbo
（`libturbojpeg`，IJG / BSD-3-Clause / zlib ライセンス）で動的リンクして展開します．
このリポジトリには含めていません．

## framecorder（同梱しません）

任意の録画ボタンは，ヘッドセットに framecorder（Steam Frame 用の別の録画ツール，
`~/.local/bin/framecorder`）が入っているときに，それを起動・停止します．
framecorder は Frame Dock の一部ではなく，再配布もしません．Frame Dock は実行するだけです．

## Google Chrome（同梱しません）

ブラウザーパネルは，インストール済みの Google Chrome（既定はシステムの flatpak
`com.google.Chrome`）を Chrome DevTools Protocol で操作します．Chrome は Frame Dock
の一部ではなく，再配布もしません．Chrome の規約が適用されます．

## [OpenVR](https://github.com/ValveSoftware/openvr)（BSD-3-Clause）

`third_party/openvr/openvr.h` はOpenVR SDKのヘッダーです．ライセンスは
[`third_party/openvr/LICENSE`](third_party/openvr/LICENSE) に保存しています．

## [vkBasalt](https://github.com/DadSchoorse/vkBasalt)（zlib）

`third_party/vkbasalt/` のVulkanレイヤーは，vkBasaltの
[コミット `4f97f09`](https://github.com/DadSchoorse/vkBasalt/commit/4f97f09ffe91900e6ca136cc26cf7966f8f6970d)
を元にした，3D LUT機能のみを残す改変版です．
zlibライセンスは
[`third_party/vkbasalt/LICENSE`](third_party/vkbasalt/LICENSE) に保存しています．
FrameColorでは対象を `vrcompositor` に限定し，ユーザー設定に応じてLUTを
更新します．また，使用しないエフェクト，入力フック，深度キャプチャ，
画像ローダー，ReShadeコンパイラーの機能を除いています．

ソースと配布用アーカイブには，NVIDIA FXAA，SMAA，DLS，ReShade
コンパイラー，stb画像ローダー，SPIRV-Headersのファイルは含まれません．
残した二つのGLSLシェーダーはvkBasalt由来です．`lut.frag.glsl` には，
vkBasaltの `cas.frag.glsl` にあるシャープ化処理も含まれます．これはAMD
FidelityFX Contrast Adaptive Sharpeningの実装です（MIT，Copyright (c)
2017-2019 Advanced Micro Devices, Inc.）．ライセンス表記はそのシェーダー内に
保存しています．

## システム側の依存関係

[Cairo](https://www.cairographics.org/)，[FreeType](https://freetype.org/)，
[Vulkan](https://www.vulkan.org/)，[libnl](https://github.com/thom311/libnl)
（LGPL-2.1，Wi-Fi のリンクの読み取り），[libpulse](https://www.freedesktop.org/wiki/Software/PulseAudio/)
（LGPL-2.1，サウンドの操作），[libsystemd](https://systemd.io/)（LGPL-2.1，Camera Switch
との sd-bus），[libcurl](https://curl.se/)（curl ライセンス，デバイスのページの任意の機能），
libjpeg-turbo（`libturbojpeg`，上記），C/C++ランタイムはSteam Frame側にあるものを
動的リンクで利用します．どれもこのリポジトリと配布用アーカイブには含めていません．

## アイコンとサムネイル

`contrib/icons/framedock-*.png` とダッシュボードのサムネイルは，Frame Dock
自身が描いたものです（`framedock --dump-settings-png ... --thumbnail-png`，
ダッシュボードのタブと同じ描画コード）．第三者の画像は含みません．文字は
システムのNoto Sans CJKフォント（SIL Open Font License 1.1）で描いています．
フォント自体は含みません．

## README の画像

`docs/images/` のスクリーンショットと動画は Frame Dock 自身を写したもので，
リリースのアーカイブには含みません．ブラウザーの動画で再生しているのは
[Big Buck Bunny](https://peach.blender.org/)（© Blender Foundation，CC BY 3.0）です．
