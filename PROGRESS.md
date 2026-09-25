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

## TODO
- [x] v3.5+小修正を実機に書き込み、16/32/64 同時テストで基準値を取る
- [ ] 非同期フォワーダ設計案を提示 → 承認後に実装
- [ ] 再テスト (最大応答 1 秒未満が合格)
- [ ] upstream への PR 案
