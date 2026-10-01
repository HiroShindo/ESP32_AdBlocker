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
- 起動時のDNS復旧を短縮 (2026-09-27): スナップショットから復元できた場合、起動時のブロックリスト再ダウンロードを省略して復元済みリストのまま prepDNS() へ進む (ダウンロードは同じ領域を作り直すため、DNS を先に起動すると競合する)。実機で起動から約10秒で DNS 応答、doubleclick.net / ads.google.com は 0.0.0.0 でブロックを確認 (以前は約3分)。リストは次の定時更新 (04:00 JST) で更新されるので最大約1日古い
- 追加修正 (2026-09-28朝、04:00定時更新の実ログから発見): 問題A スナップショット保存の空き容量チェックが「無圧縮の最悪ケース見積り」で、実際の圧縮後サイズ(約-20%)を考慮していなかったため、既存スナップショット(820KB)がある状態では1.5MBパーティションの空き(約620KB)で常に不足判定となり、9/27の修正後も保存が毎回スキップされていた。修正: 2パス目の実測サイズで判定し、新規保存前に旧スナップショットを削除して新旧同時に場所を取らないようにした(書き込み中のクラッシュで旧スナップショットを失うトレードオフは許容、loadSnapshot()は元々欠落を許容する設計)。
- 問題B setAlarm()が`<`判定のため、ちょうどアラーム秒に checkAlarm() が走ると当日を「次回」と誤認し、進行中の定時ダウンロード完了直後に即座に2回目の定時更新が発火していた(9/28 04:00:00と04:02:38の2回のダウンロード)。修正: `<=`に変更。
- 検証: オフラインロジックテストで問題Bの境界条件を確認。問題Aは一時的なテスト用フック(dbgSched、検証後削除)で `alarmHour` を6に変更→実機でScheduled経路を強制実行し、既存スナップショットがある状態で `Snapshot saved: 54782 domains, 1040KB -> 812KB` を確認(スキップなし)。再起動でその新スナップショットからの復元・DNS応答・ブロックも正常。alarmHourとfileURLcは元の値(4、正規のblocklist URL)に復元済み。
- 新たに発見 (未修正): `downloadBlockList()` で `https.begin(wclient, fileURL)` が失敗した場合(不正なURL等)、直前のTLS接続成功時に立てた `res=true` がリセットされず、ダウンロードゼロ件のまま「成功」として扱われる。この状態だと `loadBlockList()` の失敗時フォールバック(スナップショット復元)を通らず、ブロックリストが空のまま `saveSnapshot()` が`Snap skip: tiny` で保存もスキップされる。通常運用では fileURL は不正にならないため発生しないが、設定ミス時に無音でブロックが無効化される。次の対応候補。
- 解決 (2026-09-28): 前項で見つけた downloadBlockList() の res 未リセット不具合を修正。TLS接続成功時にセットされる res=true を、実際のHTTP GET開始前 (`https.begin(wclient, fileURL)` の直前) で false にリセットするようにした。検証: 一時テストフック(dbgSched)+ RAM上でのみ fileURL を不正値にして Scheduled 経路を強制実行し、修正前と同じ `Could not connect to 1` エラーで今度はフォールバックの `loadSnapshot()` が正しく走り `Scheduled load failed - serving previous 54782-domain list` になることを確認(以前は `Snap skip: tiny` で偽の成功扱いだった)。fileURLc/alarmHour を正規の値に戻し、DNS応答・ブロックも正常なことを確認済み。テストフックは削除済み。

## 2026-09-30 LAN・Wi-Fi まわりの切り分け (ESP32 コードの変更なし)
- DHCP 固定割当の確認: Buffalo=192.168.0.3 / nasne=.25 / ESP32=.27 は IP と MAC が記録どおり。Mac は今日は 192.168.0.22 (DHCP のため変わる)
- Chrome で Buffalo の管理画面に入れなかった件: 原因はアドレスバーの補完。履歴に「nasne HOME - http://192.168.0.3」が残っていて、`http://192.168.0.3/` と打つと nasne のページ (`192.168.0.3:64210`, 何も応答しない) に飛んでいた。実際の nasne HOME は `192.168.0.25:64210/nasne_home/index_pc.html`。curl では Buffalo の 80/443 とも 200 で届いていた (機器の故障ではない)。対処は候補の ✕ で履歴削除、または `http://192.168.0.3/login.html` を直接開く
- 上記とは別件として、macOS のローカルネットワーク許可 (Chrome が複数の LAN 機器に届かなくなる) は再発の可能性がある。切り分け・修復用に `~/python_prg/chrome_lan_fix.sh` を作成 (`--check` は診断のみ。修復側の tccutil reset は未実行で動作未確認)。システム設定のローカルネットワークに Google Chrome が約15個並ぶのは、アプリの実体ごとに項目が増えるためと推測 (未確認)。全てオンなので動作に問題なし
- 「aterm-d68960-g」(2.4GHz) が Mac の一覧で見つからない件: CoreWLAN のスキャンでは ch7 / -64dBm で見えており、ESP32 も -33dBm で接続中。電波は出ていた。原因は一瞬の取りこぼしと 2.4GHz の混雑 (ch1 に2台、ch7 に2台、ch9 に1台) と推測。のちに Mac も「-g」に接続できた
- `allowAP` を 1→0 に変更 (`/control?allowAP=0` → `/control?save=1` → `/control?reset=1`)。再起動後も `allowAP:0` のままで不揮発を確認、Mac のスキャンから `ESP32_AdBlocker_E0FAF4020F3C` が消え、STA は IP .27 / -33dBm で再接続。`allowAP=0` では AP は「SSID が見つからないとき」だけ起動する (utils.cpp:401)。SSID は見えるが接続できない場合 (パスワード変更・DHCP失敗) は AP が出ないので、復旧は USB 書き込みになる。元に戻すには `/control?allowAP=1` → `/control?save=1` → 再起動
- 上の 2026-09-27 付近の補足「AP は allowAP=1 のため常時出ている」は、本日 allowAP=0 にしたため現在は当てはまらない

## 2026-10-01 13:03 Buffalo 再起動による Wi-Fi 断の切り分け (ESP32 コードの変更なし)
- 事象: 13:03 ごろ Wi-Fi が切れ、約2分40秒後に復旧。Mac の HyperSBI も同時に回線ロストになった (被害は ESP32 だけではない)
- ESP32 のログ (RAM ログは `/control?displayLog=1` で取得): 13:03:09 `BEACON_TIMEOUT` (rssi -27)、13:03:12〜13:05:09 `NO_AP_FOUND`、再接続 3/6 回目の 13:05:49 に復帰。ESP32 は再起動していない (稼働20時間超)
- Buffalo のログ (管理画面からダウンロードした logfile.log): `BOOT WSR-5400XE6 boot up successfully!!` あり。時計が初期値 (2023/01/01) から始まり、13:04:20 に NTP で補正。13:05:05 から端末が再接続。**Buffalo が 13:03 ごろに再起動したと確定**。再起動の理由はログに出ていない (停電の記録なし)
- 9/26 19:57 に続く2回目の原因不明の再起動 (どちらもファーム Ver.1.15)。DFS は無関係 (5GHz は ch36 固定で DFS 対象外、ログにレーダー検知の記録なし、ESP32 は 2.4GHz)
- 電源: アダプター DKS1202 は 12V 2.0A、本体の入力も 12V 2.0A で一致。最大消費電力 16.2W ≒ 約1.35A なので容量は十分 (Buffalo 公式ページに DC 出力の記載はなし)。温度も正常、保証なし
- 13:37:55〜13:42:38 の約5分の全断は、ユーザーがアダプターの抜き挿し (壁のコンセントへの付け替え) をした操作のため故障ではない。このとき ESP32 は再接続 6/6 回目 (13:43:20) でぎりぎり復帰。実際の再起動 (約2分40秒) なら余裕がある
- 監視: ~/python_prg/esp32_watch.sh を 9/30 10:20 から継続中 (10秒ごとに esp / ap / gw / net1 / net2 / DNS / HTTPS を ~/python_prg/esp32_watch.log へ)。全項目が LOST/FAIL になる塊が Buffalo 停止の目安。単発1行の失敗は取りこぼしの可能性があり断定しない
- 検証: 13:43 以降、壁のコンセントに直接接続した状態で再発の有無を見る。数日〜1週間再発しなければ電源環境 (タップなど) が原因の可能性が高い。再発したらアダプター交換→それでも再発なら本体交換 (保証なし)。有線化は不可能
- 未確認: 再起動の根本原因、DKS1202 が純正付属品か、Buffalo の「自動再起動」設定の有無
