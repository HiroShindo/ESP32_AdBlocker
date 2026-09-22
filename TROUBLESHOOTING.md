# トラブルシューティング記録

## 2026-07-17/18: Magic Connect 接続失敗 (DNSをESP32に向けると失敗)

### 症状

- Windows (Parallels上のゲストOS) から NTT Technocross の Magic Connect に接続しようとすると失敗する。
- Mac の Wi-Fi DNS設定を ESP32_AdBlocker (`192.168.0.27`) に手動で向けている状態でのみ発生。
- Mac の DNS設定を「自動」に戻す(ルーター/プロバイダのDNSを使う)と接続に成功する。
- 症状が出るのは Windows/Parallels 側のみ。macOS 側のクライアントでは同じ環境でも問題が顕在化しなかった。

### 実際の原因

**ブロックリストの誤検知ではなく、ESP32_AdBlockerファームウェア側の実装バグだった。**

まず疑ったのは magicconnect.net 系ドメインがブロックリストに誤って載っている可能性だったが、デバイス自身の `/control?wLoad=<domain>` エンドポイント(Web UIのCheckDomainボタンと同じ処理)で以下をすべて確認 → 全て `Allowed` で誤検知ではないと判明:

- `ibuki.magicconnect.net`
- `neo-gi.magicconnect.net` / `neo-gp.magicconnect.net` / `neo-pi.magicconnect.net`
- `www.magicconnect.net`

次に、Mac の `en0` で `tcpdump` によるパケットキャプチャを行い、Windows/Parallelsクライアント (192.168.0.17) が `ibuki.magicconnect.net` に対して発行した **AAAA** クエリへの ESP32 からの応答を直接観察した。その結果、レスポンスの Question セクションは AAAA を示しているのに、Answer RR は **Type A (IPv4)** のレコードを返しているという、DNSプロトコル的に不正(QTYPEとレコードタイプの不一致)な応答になっていることが判明した。

原因コードは `externalDNS.cpp` の `handleDNSpacket()`。このハンドラは受信したDNSクエリの **QTYPEを一切読み取らず**、A/AAAA/その他どんなクエリタイプであっても常に `checkBlocklist()` の結果を使った偽の **Type Aレコード**をでっち上げて返していた。

- macOSの `mDNSResponder` はこの不正な応答を許容し、並行して投げているAクエリの結果にフォールバックするため症状が出なかった。
- Windowsの `dnscache` サービスはRRタイプ/QTYPE不一致に対してより厳格で、この不正な応答を受け取ると名前解決を失敗として扱っていたとみられる。

つまり「ブロックリストの誤検知」という当初の仮説は外れており、**ファームウェアがAAAAクエリに対して常に不正な形式の応答を返す**という、magicconnect.net特有ではない一般的なバグが根本原因だった。このバグは s60sc/ESP32_AdBlocker のアップストリーム (commit `e42728d`, v3.1時点) にも存在しており、ローカルで作り込んだものではない。

### 対応内容

`externalDNS.cpp` の `handleDNSpacket()` を修正:

1. パースしたドメイン名の直後で、クエリの **QTYPEを読み取る**処理を追加 (`qtype = (rx[new_offset] << 8) | rx[new_offset + 1]`)。
2. `qtype == 0x0001` (A) のときのみ、従来通り `checkBlocklist()` の結果を使った偽のType-A応答レコードを構築する。
3. それ以外のQTYPE (主にAAAA) の場合は、ロギング/統計のために `checkBlocklist()`自体は引き続き呼び出すが、応答は **ANCOUNT=0 / NOERROR (クリーンなNODATA)** を返すようにした。これにより不正な型のレコードを返さず、クライアント側のA/AAAA並行クエリのフォールバックに正しく委ねられる。
4. レスポンスヘッダーの `nscount` / `arcount` を明示的に0クリア(従来はリクエストの値がそのままコピーされて残っていた)。今回捕捉したクエリにはEDNS0 OPTは付いていなかったが、付与されていた場合に不正な応答になるリスクがあったための予防的修正。

再フラッシュ後、以下で動作確認済み:

- `dig @192.168.0.27 <host> AAAA` → `NOERROR` / `ANSWER: 0`
- `A` クエリは従来通り正しく解決される
- ユーザー確認: Magic Connect が正常に接続できるようになった

### 今後同様の症状が出た場合の切り分け方

**特定のドメイン/サービスがWindowsクライアントからのみ、またはDNSをESP32に向けたときのみ失敗する**という症状が再発した場合:

1. **先に「このバグの再発」を疑うこと。** 「ブロックリストの誤検知」を最初の仮説にしない — 今回それで時間を使ったが、実際は誤検知ではなくファームウェアのプロトコル実装バグだった。
2. ただし、このバグ自体は本ファームウェアでは修正済み。再発する場合はまず **`externalDNS.cpp` が古い/巻き戻ったバージョンで再フラッシュされていないか**を確認する(例: アップストリームの新バージョンにアップデートして本修正が失われた、など)。
3. 切り分け手順:
   - `curl "http://192.168.0.27/control?wLoad=<domain>"` → `curl "http://192.168.0.27/control?displayLog=1"` でブロックリスト上の判定(Allowed/Blocked)を確認。誤検知でないことを先に切り分ける。
   - `tcpdump` (Mac の `en0` 等) で該当クライアントのDNSクエリと応答を直接キャプチャし、Question の QTYPE と Answer RR の Type が一致しているか確認する。不一致ならこのバグ(またはその再発)。
   - `dig @192.168.0.27 <host> AAAA` などで直接ESP32に問い合わせ、`ANSWER:0`/`NOERROR`(正常)か、Type Aのレコードが返ってきてしまうか(バグ再発)を確認する。

関連メモリ: 開発環境/ビルド手順は `esp32-adblocker-build`、ネットワーク構成は `esp32-adblocker-network` を参照。

## 2026-09-22: 中継機廃止後にBSSID固定が原因でWi-Fi接続不能

### 症状

- 自宅ルーター構成を、親機(eo-RT110)+中継機(Aterm WX1500HP、同一SSID `aterm-d68960-g/a` を再送出)から、単体AP(BUFFALO WSR-5400XE6/N)一台構成に切り替え。
- 切り替え作業でeo-RT110の無線を停止したところ、ESP32_AdBlockerがWi-Fiに再接続できなくなり、`192.168.0.27` に到達不能・広告ブロック機能が完全停止。

### 実際の原因

以前(commit `81ed4e4`, 2026-08-20)、中継機への誤接続対策として `utils.cpp` の `setWifiSTA()` に **eo-RT110本体のBSSID (`a4:de:26:37:84:df`) をハードコードした固定接続** (`WiFi.STA.connect(ST_SSID, ST_Pass, 0, ST_bssid)`) を導入していた。

今回、そのBSSID自体を持つeo-RT110の無線チップが停止(退役)したため、ESP32は「そのBSSIDを持つAPが見つかるまで永久に接続を試み続ける」状態になり、SSID自体は新AP (WSR-5400XE6/N) が同名で発信し続けていたにもかかわらず接続できなくなった。

BSSID固定を入れた本来の目的は「同一SSIDを発する複数機器のうち電波の弱い方に誤接続するのを防ぐ」ことだったが、中継機廃止により同一SSIDを発する機器が新AP一台のみになったため、この固定は目的自体が不要になっていた。加えて、固定先の機器がなくなったことで「不要になった」どころか「積極的に接続を妨げる」状態に転じていた。

### 対応内容

`utils.cpp` の `setWifiSTA()` を修正 (commit `def0f44`):

1. `ST_bssid` のハードコード定義を削除。
2. `WiFi.STA.connect(ST_SSID, ST_Pass, 0, ST_bssid)` → `WiFi.STA.connect(ST_SSID, ST_Pass)` に戻し、通常のSSID+パスワードのみの接続に変更。
3. 起動時ログの `Local patches active: BSSID pin=..., WiFi.setSleep=false` から BSSID pin の記述を削除。
4. `WiFi.setSleep(false)`(レイテンシ対策)と、起動時に同一SSIDを発する全APのBSSID/信号強度/チャネルを1行ずつ記録する診断ログ(`startWifi()` 内)はそのまま維持 — 将来また中継機/メッシュ機器を追加してSSID重複が復活した場合、この診断ログで即座に気づける。

USBシリアル経由で書き込み後、Web UIのログ (`/control?displayLog=1`) で以下を確認済み:

- `Local patches active: WiFi.setSleep=false` (BSSID pinの記述が消えている)
- `WiFi Station connection to aterm-d68960-g` → 正常接続、`192.168.0.27` に到達可能
- 実接続先BSSIDが新AP (`9E:F8:4A:36:1F:54`) であること(旧eo-RT110の `a4:de:26:37:84:df` ではない)
- 週次自動再起動のスケジュール設定、ブロックリスト読み込みも正常動作 — 他機能への影響なし

### 今後同様の症状が出た場合の切り分け方

**ルーター/AP構成を変更した直後にESP32がWi-Fiに再接続できなくなった**場合:

1. まず `utils.cpp` の `setWifiSTA()` にBSSID固定 (`WiFi.STA.connect(..., bssid)` の第4引数) が復活していないか確認する。固定先のBSSIDが変更/廃止された機器のものだと、SSID自体は生きていても永久に接続できなくなる。
2. 中継機/メッシュ機器を新たに追加してSSID重複による誤接続(弱い電波への接続)が再発した場合は、起動時ログ (`startWifi` の "Wifi stats for ..." 行、同一SSIDにつき1行ずつ出力) でどのBSSIDが競合しているか確認したうえで、必要ならcommit `81ed4e4` のパターンを参考にBSSID固定を再導入する。

関連メモリ: 開発環境/ビルド手順は `esp32-adblocker-build`、ネットワーク構成は `esp32-adblocker-network` を参照。
