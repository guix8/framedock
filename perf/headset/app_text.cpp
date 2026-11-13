// Frame Dock's UI strings. See app_text.h. One row per key, in the enum's order: Korean, English, Japanese.
#include "app_text.h"

#include <atomic>
#include <iterator>

namespace {

std::atomic<int> gLanguage {static_cast<int>(Language::Ko)};

struct Row {
    Tx key;
    const char* ko;
    const char* en;
    const char* ja;
};

const Row kRows[] = {
    // ---- The dashboard header and its pages ----
    {Tx::PageBrowser, "브라우저", "Browser", "ブラウザ"},
    {Tx::PageDevice, "기기", "Device", "デバイス"},
    // ---- The Browser page ----
    {Tx::BrowserCardPicture, "화면", "Picture", "画面"},
    {Tx::BrowserRowFps, "초당 프레임", "Frame rate", "フレーム"},
    {Tx::BrowserRowResolution, "해상도", "Resolution", "解像度"},
    {Tx::BrowserRowMode, "실행 방식", "Runs as", "実行方式"},
    {Tx::BrowserRowClosedSound, "닫으면 소리", "When closed", "閉じたら"},
    {Tx::BrowserRowRemovedVideo, "벗으면 영상", "Headset off", "外したら"},
    {Tx::BrowserHeadless, "창 없이", "No window", "ウィンドウなし"},
    {Tx::BrowserWindow, "창", "Window", "ウィンドウ"},
    {Tx::BrowserKeep, "계속", "Keep", "続ける"},
    {Tx::BrowserPause, "멈춤", "Pause", "止める"},
    {Tx::BrowserRowIdle, "자동 종료", "Auto close", "自動終了"},
    {Tx::BrowserRowPlace, "위치", "Place", "位置"},
    {Tx::BrowserRowNow, "지금", "Now", "今すぐ"},
    {Tx::BrowserIdleMinutes, "{0}분", "{0} min", "{0} 分"},
    {Tx::BrowserIdleHint, "패널을 닫고 소리도 없으면 이 시간 뒤에 Chrome을 꺼요",
     "With the panel closed and no sound, Chrome closes after this long",
     "パネルを閉じて音もなければ、この時間で Chrome を終了します"},
    {Tx::BrowserGuideTitle, "사용법", "How to use", "使い方"},
    {Tx::BrowserGuideTap, "짧게 누르면 클릭, 끌면 스크롤해요", "Press to click, drag to scroll",
     "短く押すとクリック、ドラッグでスクロール"},
    {Tx::BrowserGuideSelect, "0.5초 누른 채 끌면 글자를 선택해요", "Hold 0.5 s and drag to select text",
     "0.5 秒押したままドラッグで文字を選択"},
    {Tx::BrowserGuideMenu, "0.5초 눌렀다 떼면 메뉴가 열려요: 복사 · 붙여넣기 · 전체 선택 · 링크 열기",
     "Hold 0.5 s and let go: Copy · Paste · Select all · Open link",
     "0.5 秒押して離すとメニュー: コピー・貼り付け・すべて選択・リンクを開く"},
    {Tx::BrowserGuideThumbstick, "썸스틱을 누르면 바로 메뉴가 열려요", "Click the thumbstick for the menu at once",
     "サムスティックを押すとすぐメニューが開きます"},
    {Tx::BrowserGuideMove, "위쪽 손잡이를 누른 채 옮겨요(그립도 돼요)", "Hold the top bar (or the grip) to move it",
     "上の取っ手を押したまま動かします（グリップでも可）"},
    {Tx::BrowserGuidePinned, "방에 고정하면 어느 손으로든 가리키고 옮길 수 있어요",
     "Pinned to the room, either hand can point and move",
     "部屋に固定すると、どちらの手でも指して動かせます"},
    {Tx::BrowserHome, "홈: {0}", "Home: {0}", "ホーム: {0}"},
    {Tx::BrowserHomeFromPage, "지금 페이지를 홈으로", "Make this page home", "今のページをホームに"},
    {Tx::BrowserStopChrome, "Chrome 끄기", "Quit Chrome", "Chrome を終了"},
    {Tx::BrowserStopChromeAgain, "한 번 더 누르면 끄기", "Press again", "もう一度で終了"},
    {Tx::BrowserStartChrome, "Chrome 켜기", "Start Chrome", "Chrome を起動"},
    {Tx::BrowserNeedsDock, "도크 탭에서 게임 중 조작을 켜야 브라우저 패널이 보여요",
     "Turn on in-game controls on the Dock page to see the browser panel",
     "ドックのページでゲーム中の操作をオンにすると、ブラウザパネルが表示されます"},
    {Tx::PaletteControls, "조작 (패널에서 바로 눌러요)", "Controls (press them on the panel)", "操作（パネルで直接押せます）"},
    {Tx::BrowserPageNote,
     "닫을 때 소리를 멈추면 성능 패널 아래 영상 조작 줄이 나오지 않아요 · 1080p는 CPU를 더 써요 · 실행 방식은 Chrome을 "
     "다시 켜면 바뀌어요",
     "With the sound paused when closed, no video controls under the performance panel · 1080p uses more CPU · "
     "The way Chrome runs changes when it restarts",
     "閉じたときに音を止めると、性能パネルの下に動画の操作は出ません · 1080p は CPU を多く使います · "
     "実行方式は Chrome の再起動で切り替わります"},
    // ---- Chrome's state ----
    {Tx::ChromeOff, "꺼짐", "Off", "オフ"},
    {Tx::ChromeStarting, "시작하는 중", "Starting", "起動中"},
    {Tx::ChromeRunning, "실행 중 (PID {0})", "Running (PID {0})", "実行中（PID {0}）"},
    {Tx::ChromeRunningPipe, "실행 중 (PID {0}, 파이프)", "Running (PID {0}, pipe)", "実行中（PID {0}、パイプ）"},
    {Tx::ChromeRunningPort, "실행 중 (PID {0}, 포트)", "Running (PID {0}, port)", "実行中（PID {0}、ポート）"},
    {Tx::ChromeClosing, "닫는 중", "Closing", "終了中"},
    {Tx::ChromeRestarting, "다시 시작하는 중 ({0}번째)", "Restarting (try {0})", "再起動中（{0} 回目）"},
    {Tx::ChromeFailed, "실패: {0}", "Failed: {0}", "失敗: {0}"},
    // ---- The Dock panel ----
    {Tx::DockHandleHint, "여기를 누르고 있으면 옮길 수 있어요", "Hold here to move it", "ここを押したままで移動"},
    {Tx::DockFullTitle, "전체 설정", "All settings", "すべての設定"},
    {Tx::DockAdjustHint, "조정하는 동안 도크는 이 자리에 있어요. 다 되면 완료를 누르세요",
     "The dock stays here while you adjust. Press Done when finished",
     "調整中、ドックはここに留まります。終わったら「完了」を押してください"},
    {Tx::DockDeviceTitle, "소리 · 패스스루", "Sound · Passthrough", "サウンド・パススルー"},
    {Tx::DockQuitChrome, "끄기", "Quit", "終了"},
    {Tx::DockQuitAgain, "한 번 더 누르면 Chrome을 꺼요", "Press again to quit Chrome", "もう一度押すと Chrome を終了します"},
    {Tx::DockFull, "전체", "All", "すべて"},
    {Tx::DockQuick, "← 빠른", "← Quick", "← クイック"},
    {Tx::DockCollapse, "접기", "Collapse", "たたむ"},
    {Tx::DockDone, "완료", "Done", "完了"},
    {Tx::DockUsing, "사용 중: {0}", "In use: {0}", "使用中: {0}"},
    {Tx::DockUnsaved, " (저장하지 않은 변경 있음)", " (unsaved changes)", "（未保存の変更あり）"},
    {Tx::DockNoActive, "사용 중인 프로필 없음", "No profile in use", "使用中のプロファイルなし"},
    {Tx::DockAwayHint, "다른 곳을 가리키면 컨트롤러가 다시 게임으로 가요",
     "Point elsewhere and the controller goes back to the game", "ほかを指すと、コントローラーはゲームに戻ります"},
    {Tx::DockHeadset, "헤드셋", "Headset", "ヘッドセット"},
    {Tx::DockLeftHand, "왼손", "Left", "左手"},
    {Tx::DockRightHand, "오른손", "Right", "右手"},
    {Tx::DockOff, "꺼짐", "Off", "オフ"},
    // ---- The 기기 page ----
    {Tx::DeviceSound, "소리", "Sound", "サウンド"},
    {Tx::DeviceOutput, "출력", "Output", "出力"},
    {Tx::DeviceVolume, "음량", "Volume", "音量"},
    {Tx::DeviceMute, "음소거", "Mute", "ミュート"},
    {Tx::DeviceChecking, "확인 중…", "Checking…", "確認中…"},
    {Tx::DeviceNoOutputs, "출력 장치를 찾지 못했어요", "No output device found", "出力デバイスが見つかりません"},
    {Tx::DeviceOnlyOutput, "다른 출력 장치가 없어요", "No other output device", "ほかの出力デバイスはありません"},
    {Tx::DeviceSpeakers, "헤드셋 스피커", "Headset speakers", "ヘッドセットのスピーカー"},
    {Tx::DevicePassthrough, "패스스루", "Passthrough", "パススルー"},
    {Tx::DevicePassView, "화면", "View", "表示"},
    {Tx::DevicePassOff, "끔", "Off", "オフ"},
    {Tx::DevicePassMono, "흑백", "Mono", "モノクロ"},
    {Tx::DevicePassColor, "컬러", "Color", "カラー"},
    {Tx::DevicePassOn, "켬", "On", "オン"},
    {Tx::DevicePassNote1, "컬러와 흑백은 Camera Switch로 바꿔요.", "Color and mono switch through Camera Switch.",
     "カラーとモノクロは Camera Switch で切り替えます。"},
    {Tx::DevicePassNote2, "여기서 바꾸면 자동 야간 전환이 2시간 멈춰요.",
     "Switching here pauses its automatic night switch for 2 hours.",
     "ここで切り替えると、夜間の自動切り替えが 2 時間止まります。"},
    {Tx::DevicePassNoCameraSwitch, "Camera Switch가 실행 중이 아니어서 켜고 끄기만 할 수 있어요(색은 그대로)",
     "Camera Switch isn't running: on and off only (the color stays as it is)",
     "Camera Switch が動いていないので、オン・オフだけできます（色はそのまま）"},
    {Tx::DevicePassUnknown, "지금 상태를 읽지 못했어요", "Couldn't read the current state", "今の状態を読み取れませんでした"},
    {Tx::DevicePassRemembered, "색은 마지막으로 고른 값이에요", "The color is the one picked last", "色は最後に選んだものです"},
    {Tx::DevicePassColorUnknown, "색은 읽을 수 없어요", "The color can't be read", "色は読み取れません"},
    // ---- PC 계정 맞추기 (Frame Sync) ----
    {Tx::SyncTitle, "PC 계정 맞추기", "Match the PC's account", "PC のアカウント合わせ"},
    {Tx::SyncShutdown, "PC 끄기", "PC off", "PC オフ"},
    {Tx::SyncCancel, "취소", "Cancel", "キャンセル"},
    {Tx::SyncOthers, "다른 계정으로 하려면", "To use another account", "ほかのアカウントにするには"},
    {Tx::SyncChoose, "선택", "Select", "選択"},
    {Tx::SyncNoOthers, "다른 계정이 없어요", "No other accounts", "ほかのアカウントはありません"},
    {Tx::SyncMore, "외 {0}개", "{0} more", "ほか {0} 件"},
    {Tx::SyncNoSettings, "설정 파일이 없어요(설치를 확인해 주세요)", "No settings file (check the install)",
     "設定ファイルがありません（インストールを確認してください）"},
    {Tx::SyncPcWaking, "● 집 PC 켜는 중", "● Home PC starting", "● 自宅の PC を起動中"},
    {Tx::SyncStreaming, "스트리밍 중", "Streaming", "ストリーミング中"},
    {Tx::SyncInGame, "게임 중", "In a game", "ゲーム中"},
    {Tx::SyncAskGame, "집 PC에서 게임이 실행 중이에요. 바꾸면 게임이 꺼져요. 계속할까요?",
     "A game runs on the home PC. Switching closes it. Continue?",
     "自宅の PC でゲームが動いています。切り替えるとゲームは終了します。続けますか?"},
    {Tx::SyncContinue, "계속", "Continue", "続ける"},
    {Tx::SyncAskShutdown, "집 PC를 끌까요?", "Turn the home PC off?", "自宅の PC の電源を切りますか?"},
    {Tx::SyncTurnOff, "끄기", "Turn off", "切る"},
    {Tx::SyncSwitchedFromList, "집 PC를 바꿨어요. 프레임 계정은 스팀 메뉴에서 직접 바꿔 주세요.",
     "The home PC was switched. Switch the Frame's account yourself in the Steam menu.",
     "自宅の PC を切り替えました。Frame のアカウントは Steam メニューで切り替えてください。"},
    {Tx::SyncUnreachable, "집 PC에 연결할 수 없어요. PC 전원·방화벽·에이전트를 확인해 주세요.",
     "Can't reach the home PC. Check its power, firewall and agent.",
     "自宅の PC に接続できません。電源・ファイアウォール・エージェントを確認してください。"},
    {Tx::SyncAuth, "앱 열쇠가 맞지 않아요. 열쇠를 다시 넣어 주세요.", "The app key doesn't match. Put the key in again.",
     "アプリのキーが合いません。キーを入れ直してください。"},
    {Tx::SyncLogin, "집 PC에 로그인한 사람이 없어요. 자동 로그인을 확인해 주세요.",
     "Nobody is logged in on the home PC. Check its automatic login.",
     "自宅の PC にログインしていません。自動ログインを確認してください。"},
    {Tx::SyncResult, "집 PC에서 전환 결과가 오지 않아요. 집 PC의 스팀 화면을 확인해 주세요.",
     "The home PC didn't say how the switch went. Check Steam on the home PC.",
     "自宅の PC から切り替えの結果が届きません。自宅の PC の Steam を確認してください。"},
    {Tx::SyncCancelled, "취소했어요.", "Cancelled.", "キャンセルしました。"},
    {Tx::SyncAgentHttp, "집 PC 에이전트 오류(HTTP {0})", "Home PC agent error (HTTP {0})",
     "自宅の PC のエージェントのエラー（HTTP {0}）"},
    {Tx::SyncUnexpected, "예상하지 못한 오류: {0}", "Unexpected error: {0}", "予期しないエラー: {0}"},
    {Tx::SyncStripAuth, "● 집 PC 연결 오류(앱 열쇠)", "● Home PC: connection error (app key)",
     "● 自宅の PC: 接続エラー（アプリのキー）"},
    {Tx::SyncStripError, "● 집 PC 연결 오류", "● Home PC: connection error", "● 自宅の PC: 接続エラー"},
    {Tx::SyncStripChecking, "집 PC 확인 중…", "Checking the home PC…", "自宅の PC を確認中…"},
    {Tx::SyncStripOn, "● 집 PC 켜짐", "● Home PC on", "● 自宅の PC: オン"},
    {Tx::SyncStripOff, "● 집 PC 꺼짐", "● Home PC off", "● 自宅の PC: オフ"},
    {Tx::SyncBigError, "연결 오류 — 아래 안내를 확인해 주세요", "Connection error: see below",
     "接続エラー。下の案内を確認してください"},
    {Tx::SyncBigNoFrameAccount, "프레임에 로그인한 계정을 찾지 못했어요", "No account is logged in on the Frame",
     "Frame にログインしているアカウントが見つかりません"},
    {Tx::SyncBigNotOnPc, "{0} 계정이 집 PC 스팀에 없어요", "{0} isn't on the home PC's Steam",
     "{0} は自宅の PC の Steam にありません"},
    {Tx::SyncBigWake, "집 PC 켜고 {0}로 스팀 켜기", "Start the home PC and Steam as {0}",
     "自宅の PC を起動して {0} で Steam を起動"},
    {Tx::SyncBigSwitch, "집 PC를 {0}로 바꾸기", "Switch the home PC to {0}", "自宅の PC を {0} に切り替え"},
    {Tx::SyncBigReady, "{0} · 스트리밍 준비됨", "{0} · Ready to stream", "{0} · ストリーミングの準備完了"},
    {Tx::SyncStepWaking, "켜는 중", "Starting", "起動中"},
    {Tx::SyncStepLogin, "로그인 대기", "Waiting for login", "ログイン待ち"},
    {Tx::SyncStepSwitching, "스팀 전환 중", "Switching Steam", "Steam を切り替え中"},
    {Tx::SyncStepDone, "완료", "Done", "完了"},
    {Tx::SyncQuitting, "Frame Dock을 끄는 중", "Frame Dock is quitting", "Frame Dock を終了中"},
    {Tx::SyncShutdownSoon, "집 PC를 20초 뒤에 꺼요.", "The home PC turns off in 20 s.",
     "自宅の PC は 20 秒後に電源が切れます。"},
    {Tx::SyncShutdownFailed, "끄지 못했어요.", "Couldn't turn it off.", "電源を切れませんでした。"},
    // ---- The browser panel ----
    {Tx::BrowserTitle, "브라우저", "Browser", "ブラウザ"},
    {Tx::BrowserClose, "닫기", "Close", "閉じる"},
    {Tx::BrowserQuit, "끄기", "Quit", "終了"},
    {Tx::BrowserAgain, "한 번 더", "Again", "もう一度"},
    {Tx::BrowserDragScroll, "끌기: 스크롤", "Drag: scroll", "ドラッグ: スクロール"},
    {Tx::BrowserDragSelect, "끌기: 선택", "Drag: select", "ドラッグ: 選択"},
    {Tx::BrowserUnpin, "고정 풀기", "Unpin", "固定を解除"},
    {Tx::BrowserPin, "방에 고정", "Pin to room", "部屋に固定"},
    {Tx::BrowserCancel, "취소", "Cancel", "キャンセル"},
    {Tx::BrowserOk, "확인", "OK", "OK"},
    {Tx::BrowserAddressHint, "검색어 또는 주소", "Search or address", "検索語またはアドレス"},
    {Tx::BrowserTranslate, "번역", "Translate", "翻訳"},
    {Tx::BrowserView, "보기", "View", "表示"},
    {Tx::BrowserFavorites, "즐겨찾기", "Favorites", "お気に入り"},
    {Tx::BrowserKeyboard, "키보드", "Keyboard", "キーボード"},
    {Tx::BrowserLoading, "페이지를 불러오는 중…", "Loading the page…", "ページを読み込み中…"},
    {Tx::BrowserFavoritesEmpty, "☆를 누르면 보고 있는 페이지가 여기에 저장돼요", "Press ☆ to save the page you're on here",
     "☆ を押すと、見ているページがここに保存されます"},
    {Tx::BrowserFavoritesFull, "10개까지 저장돼요. ✕로 하나 지운 뒤 다시 눌러 주세요",
     "Up to 10 are kept. Remove one with ✕ and press again",
     "保存できるのは 10 件までです。✕ で 1 件消してからもう一度押してください"},
    {Tx::BrowserFavoritesUnsaved, "파일에 저장하지 못했어요 (앱을 끄면 사라져요)",
     "Couldn't save to the file (gone when the app quits)",
     "ファイルに保存できませんでした（アプリを終了すると消えます）"},
    {Tx::BrowserViewSite, "버전", "Site", "サイト"},
    {Tx::BrowserViewDesktop, "데스크톱", "Desktop", "PC 版"},
    {Tx::BrowserViewMobile, "모바일", "Mobile", "モバイル版"},
    {Tx::BrowserViewZoom, "확대", "Zoom", "拡大"},
    {Tx::BrowserViewZoomReset, "원래대로", "Reset", "元に戻す"},
    {Tx::BrowserViewAspect, "비율", "Aspect", "比率"},
    {Tx::BrowserViewAspectNote, "비율을 바꾸면 Chrome이 같은 페이지로 다시 떠요(영상은 처음부터)",
     "A new aspect reopens Chrome on the same page (videos start over)",
     "比率を変えると Chrome が同じページで開き直します（動画は最初から）"},
    {Tx::BrowserMenuCopy, "복사", "Copy", "コピー"},
    {Tx::BrowserMenuPaste, "붙여넣기", "Paste", "貼り付け"},
    {Tx::BrowserMenuSelectAll, "전체 선택", "Select all", "すべて選択"},
    {Tx::BrowserMenuOpenLink, "링크 열기", "Open link", "リンクを開く"},
    {Tx::BrowserDialogLeave, "이 페이지를 떠날까요? 입력한 내용이 사라질 수 있어요",
     "Leave this page? What you entered may be lost", "このページを離れますか? 入力した内容は消えることがあります"},
    {Tx::BrowserDialogPrompt, "페이지의 질문 (확인은 기본값으로 답해요): {0}",
     "The page asks (OK answers with its default): {0}", "ページからの質問（OK で既定の答え）: {0}"},
    {Tx::BrowserDialogAsk, "페이지의 질문: {0}", "The page asks: {0}", "ページからの質問: {0}"},
    {Tx::BrowserStatusOff, "Chrome이 꺼져 있어요. 새로고침을 누르면 시작해요", "Chrome is off. Press reload to start it",
     "Chrome はオフです。再読み込みを押すと起動します"},
    {Tx::BrowserStatusStarting, "Chrome을 시작하는 중…", "Starting Chrome…", "Chrome を起動中…"},
    {Tx::BrowserStatusConnecting, "Chrome에 연결하는 중…", "Connecting to Chrome…", "Chrome に接続中…"},
    {Tx::BrowserStatusClosing, "Chrome을 닫는 중…", "Closing Chrome…", "Chrome を終了中…"},
    {Tx::BrowserStatusRestarting, "Chrome을 다시 시작하는 중… ({0}번째)", "Restarting Chrome… (try {0})",
     "Chrome を再起動中…（{0} 回目）"},
    {Tx::BrowserStatusFailed, "Chrome을 열지 못했어요", "Couldn't open Chrome", "Chrome を開けませんでした"},
    {Tx::BrowserStatusRetry, "새로고침을 누르면 다시 시도해요", "Press reload to try again", "再読み込みを押すともう一度試します"},
    {Tx::BrowserKeyboardAddress, "주소 · 검색", "Address · Search", "アドレス・検索"},
    {Tx::BrowserKeyboardPage, "키보드 · 페이지에 입력", "Keyboard · Typing into the page", "キーボード・ページに入力"},
    {Tx::BrowserThreadFailed, "브라우저 스레드를 시작하지 못했어요: {0}", "Couldn't start the browser's thread: {0}",
     "ブラウザのスレッドを開始できませんでした: {0}"},
    // ---- Chrome's errors ----
    {Tx::ChromeNoProfile, "Chrome 프로필 폴더가 없어요", "No folder for Chrome's profile",
     "Chrome のプロファイルのフォルダーがありません"},
    {Tx::ChromeStartFailed, "Chrome을 시작하지 못했어요: {0}", "Couldn't start Chrome: {0}", "Chrome を起動できませんでした: {0}"},
    {Tx::ChromeExited, "Chrome이 종료됐어요 ({0})", "Chrome quit ({0})", "Chrome が終了しました（{0}）"},
    {Tx::ChromePipeClosed, "DevTools 파이프가 닫혔어요: {0}", "The DevTools pipe closed: {0}",
     "DevTools のパイプが閉じました: {0}"},
    {Tx::ChromeNotAnswering, "Chrome이 응답하지 않아요", "Chrome doesn't answer", "Chrome が応答しません"},
    {Tx::ChromeDevToolsFailed, "DevTools에 연결하지 못했어요: {0}", "Couldn't connect to DevTools: {0}",
     "DevTools に接続できませんでした: {0}"},
    {Tx::ChromeDevToolsLost, "DevTools 연결이 끊겼어요: {0}", "Lost the DevTools connection: {0}",
     "DevTools との接続が切れました: {0}"},
    {Tx::ChromeDevToolsSilent, "DevTools가 응답하지 않아요", "DevTools doesn't answer", "DevTools が応答しません"},
    {Tx::ChromePageFailed, "페이지를 열지 못했어요", "Couldn't open the page", "ページを開けませんでした"},
    {Tx::ChromePageFailedWhy, "페이지를 열지 못했어요: {0}", "Couldn't open the page: {0}", "ページを開けませんでした: {0}"},
    {Tx::ChromeAttachFailed, "페이지에 연결하지 못했어요", "Couldn't connect to the page", "ページに接続できませんでした"},
    {Tx::ChromeAttachFailedWhy, "페이지에 연결하지 못했어요: {0}", "Couldn't connect to the page: {0}",
     "ページに接続できませんでした: {0}"},
    {Tx::ChromeCrashBlank, "Chrome이 계속 종료되어 빈 페이지를 열었어요: {0}",
     "Chrome kept quitting, so a blank page was opened: {0}", "Chrome の終了が続いたので、空白のページを開きました: {0}"},
    {Tx::ChromeCrashHome, "Chrome이 계속 종료되어 홈을 열었어요: {0}", "Chrome kept quitting, so home was opened: {0}",
     "Chrome の終了が続いたので、ホームを開きました: {0}"},
    {Tx::ChromePageHung, "페이지가 계속 멈춰서 빈 페이지를 열었어요: {0}",
     "The page kept crashing, so a blank page was opened: {0}", "ページが止まり続けたので、空白のページを開きました: {0}"},
    // ---- The VR keyboard ----
    {Tx::KeyboardTitle, "키보드", "Keyboard", "キーボード"},
    {Tx::KeyboardHandleHint, "손잡이를 누르고 있으면 함께 옮겨져요", "Hold the bar to move it along",
     "取っ手を押したままで一緒に動きます"},
    {Tx::KeyboardClose, "닫기", "Close", "閉じる"},
    {Tx::KeyboardHintHangul, "글자는 바로 입력돼요. 조합 중인 글자는 여기에 보여요",
     "Letters go in at once. The syllable being put together shows here",
     "文字はすぐ入力されます。組み立て中の文字はここに出ます"},
    {Tx::KeyboardHintOther, "누르는 대로 입력돼요", "Keys go in as you press them", "押したとおりに入力されます"},
    {Tx::KeyboardComposing, "조합 중  ", "Composing  ", "組み立て中  "},
    {Tx::KeyboardHangul, "한글", "Korean", "韓国語"},
    {Tx::KeyboardEnglish, "English", "English", "英語"},
    {Tx::KeyboardSymbols, "기호", "Symbols", "記号"},
    {Tx::KeyboardSymbolsBadge, "숫자 · 기호", "Numbers · Symbols", "数字・記号"},
    {Tx::KeyboardClear, "지우기", "Clear", "クリア"},
    {Tx::KeyboardLatinKey, "영", "A", "A"},
    // ---- The HUD's video controls ----
    {Tx::MediaPlaying, "재생 중", "Playing", "再生中"},
    {Tx::MediaPaused, "멈춤", "Paused", "一時停止"},
    {Tx::MediaPausedByRemoval, "벗어서 멈춤", "Paused: headset off", "外したので停止"},
    {Tx::MediaBack, "−10초", "−10 s", "−10秒"},
    {Tx::MediaForward, "+10초", "+10 s", "+10秒"},
    {Tx::MediaPause, "멈추기", "Pause", "停止"},
    {Tx::MediaPlay, "재생", "Play", "再生"},
    {Tx::MediaMute, "음소거", "Mute", "ミュート"},
    {Tx::MediaBrowser, "브라우저", "Browser", "ブラウザ"},
    // ---- Profiles ----
    {Tx::ProfileDefaultName, "프로필 {0}", "Profile {0}", "プロファイル {0}"},
    {Tx::ProfileBasic, "기본", "Basic", "基本"},
    {Tx::ProfileDetailed, "상세", "Detailed", "詳細"},
    {Tx::ProfileWriteFailed, "파일을 쓰지 못했어요", "couldn't write the file", "ファイルを書き込めませんでした"},
    {Tx::ProfileTooMany, "프로필은 {0}개까지 만들 수 있어요", "up to {0} profiles", "プロファイルは {0} 個までです"},
    {Tx::ProfileMissing, "그 프로필이 없어요", "that profile isn't there", "そのプロファイルはありません"},
    {Tx::ProfileDeleteFailed, "파일을 지우지 못했어요", "couldn't delete the file", "ファイルを削除できませんでした"},
    {Tx::ProfileConfigSaveFailed, "설정 파일을 저장하지 못했어요", "couldn't save the settings file",
     "設定ファイルを保存できませんでした"},
    // ---- Notices ----
    {Tx::NoticeHeadsetLow, "헤드셋 배터리 {0} 남았어요", "Headset battery at {0}", "ヘッドセットの電池が残り {0}"},
    {Tx::NoticeHeadsetLowTime, "헤드셋 배터리 {0} · {1} 남았어요", "Headset battery at {0} · {1} left",
     "ヘッドセットの電池が残り {0} · {1}"},
    {Tx::NoticeHeadsetCritical, "헤드셋 배터리 {0} · 곧 꺼질 수 있어요. 충전해 주세요",
     "Headset battery at {0} · It may turn off soon. Charge it",
     "ヘッドセットの電池が残り {0} · まもなく切れます。充電してください"},
    {Tx::NoticeHeadsetCriticalTime, "헤드셋 배터리 {0} · {1} 남았어요. 충전해 주세요",
     "Headset battery at {0} · {1} left. Charge it", "ヘッドセットの電池が残り {0} · {1}。充電してください"},
    {Tx::NoticeLeftControllerLow, "왼손 컨트롤러 배터리 {0} 남았어요", "Left controller battery at {0}",
     "左手のコントローラーの電池が残り {0}"},
    {Tx::NoticeRightControllerLow, "오른손 컨트롤러 배터리 {0} 남았어요", "Right controller battery at {0}",
     "右手のコントローラーの電池が残り {0}"},
    // ---- The first-run guide (the approved mockup's words) ----
    {Tx::GuideTitle, "Frame Dock을 켰어요", "Frame Dock is on", "Frame Dock を起動しました"},
    {Tx::GuideWristLabel, "손목", "Wrist", "手首"},
    {Tx::GuideWrist, "성능 패널은 왼손 손목에 붙어 있어요. 손목을 돌려 보세요",
     "The performance panel is on your left wrist. Turn it to look",
     "性能パネルは左の手首に付いています。手首を回して見てください"},
    {Tx::GuidePressLabel, "누르기", "Press", "押す"},
    {Tx::GuidePress, "다른 손 레이저로 패널을 가리키고 트리거: 도크가 열려요",
     "Point at it with the other hand and pull the trigger: the dock opens",
     "反対の手のレーザーでパネルを指してトリガー: ドックが開きます"},
    {Tx::GuideMoveLabel, "옮기기", "Move", "移動"},
    {Tx::GuideMove, "트리거를 0.6초 누른 채 움직이면 패널이 따라와요", "Hold the trigger for 0.6 s and move: the panel follows",
     "トリガーを 0.6 秒押したまま動かすと、パネルがついてきます"},
    {Tx::GuideSettingsLabel, "설정", "Settings", "設定"},
    {Tx::GuideSettings, "SteamVR 대시보드 아래 Frame Dock 탭", "The Frame Dock tab at the bottom of the SteamVR dashboard",
     "SteamVR ダッシュボードの下にある Frame Dock タブ"},
    {Tx::GuideFooter, "패널을 한 번 누르면 사라져요 · 다시 보기: 도크 탭의 사용법",
     "Goes away once you press the panel · See it again: Dock tab, How to use",
     "パネルを一度押すと消えます · もう一度見るには: ドックのページの「使い方」"},
    {Tx::DockRecord, "녹화", "Record", "録画"},
    {Tx::DockClip, "클립", "Clip", "クリップ"},
};

static_assert(std::size(kRows) == static_cast<size_t>(Tx::Count), "one row per key");

}  // namespace

Language uiLanguage() {
    return static_cast<Language>(gLanguage.load(std::memory_order_relaxed));
}

void setUiLanguage(Language language) {
    gLanguage.store(static_cast<int>(language), std::memory_order_relaxed);
}

const char* tr(Tx key, Language language) {
    const size_t i = static_cast<size_t>(key);
    if (i >= std::size(kRows)) return "";
    const Row& row = kRows[i];
    switch (language) {
        case Language::En: return row.en;
        case Language::Ja: return row.ja;
        case Language::Ko: break;
    }
    return row.ko;
}

const char* tr(Tx key) {
    return tr(key, uiLanguage());
}

std::string formatText(const std::string& pattern, std::initializer_list<std::string> args) {
    std::string out;
    out.reserve(pattern.size() + 16);
    for (size_t i = 0; i < pattern.size(); ++i) {
        // "{N}" with a single digit: the N-th argument
        if (pattern[i] == '{' && i + 2 < pattern.size() && pattern[i + 1] >= '0' && pattern[i + 1] <= '9' &&
            pattern[i + 2] == '}') {
            const size_t n = static_cast<size_t>(pattern[i + 1] - '0');
            if (n < args.size()) {
                out += *(args.begin() + n);
                i += 2;
                continue;
            }
        }
        out += pattern[i];
    }
    return out;
}

std::string trf(Tx key, std::initializer_list<std::string> args) {
    return formatText(tr(key), args);
}

bool textTableComplete(std::string& problem) {
    for (size_t i = 0; i < std::size(kRows); ++i) {
        const Row& row = kRows[i];
        if (static_cast<size_t>(row.key) != i) {
            problem = "row " + std::to_string(i) + " is key " + std::to_string(static_cast<int>(row.key));
            return false;
        }
        for (const char* text : {row.ko, row.en, row.ja}) {
            if (text == nullptr || text[0] == '\0') {
                problem = "key " + std::to_string(i) + " has an empty string";
                return false;
            }
        }
    }
    return true;
}
