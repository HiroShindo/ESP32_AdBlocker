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
