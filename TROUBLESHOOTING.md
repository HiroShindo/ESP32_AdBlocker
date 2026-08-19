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
