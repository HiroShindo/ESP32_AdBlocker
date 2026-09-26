# DNS 同時問い合わせ停止問題 (2026-09-25)

## 状況
- 実機は v3.2 フォーク (AsyncUDP + getaddrinfo) で、upstream v3.5 (dnsTask + queryUpstream) とは別コードだった
- upstream v3.5 をマージ (WiFi 安定化パッチ・JST-9 は維持)
- 小修正1: A/AAAA 以外のクエリは上流に問い合わせず即 NODATA (ブロック判定は従来通り)
- SO_RCVBUF: sdkconfig で LWIP_UDP_RECVMBOX_SIZE=6 (件数上限) 固定。SO_RCVBUF はバイト数上限のみで効かない見込み → 実機で確認予定
- CACHE_SIZE は upstream v3.5 で既に 100

## 基準値 (v3.5 + 小修正、実機、~/python_prg/dns_burst_test.py で1回ずつ送信・再送なし)
- A: N=1 42ms / N=4 全応答 / N=8 5/8 / N=16 7/16 / N=32 7/32 / N=64 8/64 (残りは lwIP mbox=6 で破棄)
- HTTPS 16件: 10/16 (上流に聞かなくなり13msで返るが、それでも取りこぼす = 受信キューあふれは処理速度だけでは防げない)
- AAAA 16件: 7/16、中央値 230ms (A と AAAA の2回上流問い合わせ)
- 起動から DNS 復帰まで約2.5分 (useSnap=0 のため毎回ブロックリスト再ダウンロード)

## 結果 (非同期フォワーダ + raw pcb 受信リング, 実機 v3.5+)
- 一斉送信・再送なし: N=16 16/16 (max 57ms)、N=32 32/32 (max 74ms)、N=64 52/64 (max 94ms、残りは lwIP tcpip mbox=32 で破棄)
- 1秒後に再送あり(リゾルバ相当): N=64 全応答 max 約1.1秒、N=100 全応答 max 約1.1秒
- Mac の OS リゾルバ経由 (getaddrinfo、A+AAAA): N=16/17 中央値 50〜80ms、N=32 最大 約1.1秒、N=64/100 約1.1〜3秒 (改善前は N=17 で 72秒)
- SO_RCVBUF: 未採用。LWIP_UDP_RECVMBOX_SIZE=6 は件数上限で変更不可のため。raw pcb + 自前リングで回避
- useSnap=1 に設定済みだが、v3.5 は復元後も必ずダウンロード完了まで prepDNS() を呼ばない (起動〜DNS復帰 約3分は変わらず)

## WiFi 復旧の穴 (2026-09-25 13:00 に ESP32 が 12 分間オフライン、電源入れ直しで復旧)
- Buffalo AP のログ: 13:00:23 に ESP32 (3c:0f:02:f4:fa:e0) が deauth、以後 13:12:02 の電源再投入まで接続試行なし。AP は正常、他端末は普通に再接続
- 原因 (コード上の仮説、実機では未再現): setAutoReconnect(false) のため復旧は pingTimeout だけが頼りだが、WL_NO_SSID_AVAIL のときは何もしなかった。pingTimeout は WDT も更新するので再起動もされなかった
- 修正: NO_SSID_AVAIL でも再接続を試行。ping失敗が続き再接続を3回試しても戻らなければ doRestart() で本体再起動 (再起動しても RAM ログは残る)
- 11:27 / 11:34 にも ~7分おきに ESP32 が自分で切断→再接続していた (ping失敗による再接続)。なぜ ping が落ちるかは未調査
- 監視: ~/python_prg/esp32_watch.sh → ~/python_prg/esp32_watch.log (10秒おき)

## TODO
- [x] v3.5+小修正を実機に書き込み、16/32/64 同時テストで基準値を取る
- [ ] 非同期フォワーダ設計案を提示 → 承認後に実装
- [ ] 再テスト (最大応答 1 秒未満が合格)
- [ ] upstream への PR 案

## 2026-09-26 起動時WiFi失敗からの復旧不能 (17時ごろ発見、電源入れ直しで復旧)
- 事象: 04:00 の定時ブロックリスト再読込中に statusCheckTask が Double exception でクラッシュ→再起動。再起動後 `Association refused too many times` で STA 接続に失敗し、STA+AP のまま何もせず放置 (LAN上に不在、iPhone に ESP32_AdBlocker_<MAC逆順> のAPが見えた)
- 原因1: startPing() はゲートウェイ未確定だと開始しない → 初回接続に失敗すると ping コールバック (再接続の唯一の入口) が存在せず永遠に復旧しない。9/25 のコミット d92dd80 は ping 動作中の経路しか直していなかった
- 原因2 (推測): ブロックリストの TLS ダウンロード + スナップショット保存が 4KB スタックの statusCheckTask 上で動いている。クラッシュは esp_wifi_internal_tx/esf_buf_alloc 内 (バックトレースを ELF で解読)
- 修正: wifiWatchTask を追加 (ping 監視が無く STA 未接続なら 30秒ごとに disconnect→startWifi(false)、8回失敗で doRestart)。STATUS_STACK_SIZE 4KB→8KB、statusCheckTask の残スタックを警告ログ
- 別件: `Snap rename blsnap.bin.tmp -> blsnap.bin failed` でスナップショットが一度も保存されていない。原因未特定のため errno / tmp サイズ / 空き容量を出す診断ログを追加 (次に発生したらログを見る)
- 未解明: 13:00 ごろ Buffalo AP (192.168.0.3) のログに ESP32 の deauth が毎日出る (9/25 13:00:23, 9/26 13:00:09)。AP 側の定時処理の可能性
- 追加 (9/26 夜): STA 切断イベントのログに理由コード・名前・RSSI を出すようにした (`WiFi Station disconnected, reason N (NAME), rssi X`)。13:00 ごろの切断が AP 側都合 (AUTH_EXPIRE/ASSOC_LEAVE 等) か ESP32 側かを、Aterm のログ無しで ESP32 の Check Log から判定するため。Chrome からは Aterm(192.168.0.3) だけ ERR_ADDRESS_UNREACHABLE になる (curl/Safari は可、原因未特定) ので Aterm の管理画面には頼らない

## 2026-09-26 夜: 13:00クラッシュの原因整理と追加修正
- 13:00 JST の切断は Aterm 側の定時処理ではなく ESP32 自身のクラッシュ跡だった。ESP32 の時計が UTC で動いており (configTzTime が loadConfig より前に呼ばれ GMT0 になっていた)、alarmHour=4 が 04:00 UTC = 13:00 JST に動いていた。定時更新 (Scheduled load) の直後に statusCheckTask が Double exception。9/25 13:00:23、9/26 13:00:09 の Aterm ログの deauth はその再起動
- 検証: statusCheckTask のスタックを 8KB にした版で 19:00 JST に定時更新を走らせ (alarmHour を一時的に 10=UTC で設定)、クラッシュ・再起動・DNS断なし (監視470回)。1回のみなので確定ではない。翌 04:00 JST の更新で再確認する
- 修正: prefs.cpp で timezone 読込時に setenv TZ + tzset (以後 log 時刻・alarmHour・週次再起動 (Tue 02:00) がすべて JST 基準)。alarmHour は暫定 19 (=04:00 JST) から本来の 4 に戻す
- 修正: Web画面の Reload ボタンが再起動ループを起こす問題 (doRestart が応答前に呼ばれ、ブラウザが要求を再送)。webServer.cpp の controlHandler で zLoad は先に応答を返してから処理
- 解決 (2026-09-27): スナップショット保存の失敗 (`rename ... errno 16 EBUSY, tmp size 0KB`) の原因は、書き込み後に `seek(0)` でヘッダを書き直していたこと。LittleFS は巻き戻して書き直すとファイル全体をコピーし直すため、820KB のファイルにさらに約820KBの空きが要る (空きは 1444KB/1536KB)。足りずに close で書き込みが失敗し、空ファイルが残っていた (エラーは出ない)。自己テストで 600KB は seek あり/なし両方 OK、850KB は seek ありだけ 0B になることを確認。修正: 2パスに分け (1回目は I/O なしで長さと CRC を計算、2回目でヘッダ→本体を順に書く)、rename 前に書いたサイズを検証。実機で保存成功→再起動で `Restored 55380 domains` を確認 (起動1.4秒)
- 補足: ESP32 の AP (ESP32_AdBlocker_<MAC逆順>) は allowAP=1 のため STA 接続中でも常時出ている (異常ではない)。Chrome から Aterm(192.168.0.3) が ERR_ADDRESS_UNREACHABLE になる件は macOS のローカルネットワーク許可ダイアログを承認したら解消
- 9/26 19:57〜20:00 Aterm (Buffalo WSR-5400XE6, 192.168.0.3) が原因不明で再起動し Wi-Fi が約3分停止。ESP32 は再起動せず自力復旧したが、再接続 3/3 回目でぎりぎりだった (Aterm ログ: BOOT の記録、再起動でログ消去。電源周りに問題なし。ファームは最新 Ver.1.15 で更新なし。Buffalo の自動更新は初期設定で毎日 04:00〜04:59 に 2〜3 分停止させるので、翌 04:00 JST の検証と重なる可能性あり)
- 対応: NET_RESTART_ATTEMPTS を 3→6 (約5分まで待ってから ESP32 を再起動)
- 検証結果 (2026-09-27 04:00 JST): 定時更新 (`Scheduled load of latest blocklist` 04:00:27) がクラッシュ・再起動なしで完了。監視 04:00〜04:15 は ping/DNS 失敗 0 回、Aterm の 04:00 自動更新の影響もなし。13:00 問題は解決と判断し esp32_watch.sh は停止 (ログ ~/python_prg/esp32_watch.log は保存)。スナップショット rename の errno 16 は引き続き未解決
