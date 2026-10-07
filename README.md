# m5paper-status

初代 M5Paper（ESP32、540 × 960、16 MB Flash / 8 MB PSRAM）用の
常時給電ダッシュボード。Arduino framework + PlatformIO で開発する。
M5PaperS3 は対象外。

API の受け口と外部サービスへの接続処理は M5Paper 本体に置く方針。
現時点では文字列投稿・日本語表示・再起動後の復元、Wi-Fi 接続・再接続、
mDNS、ヘルスチェック API、Google カレンダーの今日から 7 日分の表示を実装。
Codex / Claude の利用枠の取得・棒グラフ・リセット時刻表示も本体で行う。

## セットアップ

PlatformIO Core を用意して、プロジェクトのルートで実行する。

```sh
cp config.example.json config.local.json
# config.local.json の wifi.ssid / wifi.password を設定
pio run
pio run -t upload --upload-port /dev/ttyUSB0
pio device monitor --port /dev/ttyUSB0
```

ポートは接続環境に合わせて変更する。シリアル通信は 115200 baud。
Wi-Fi 未設定でもビルド・起動でき、シリアルログに設定案内を表示する。
`config.local.json` は Git 管理対象外。設定変更後は再ビルド・書き込みが必要。

## ビルド時の設定

通常はプロジェクト直下の `config.local.json` を読み込む。
ビルド時に環境変数 `M5PAPER_CONFIG` で設定ファイルを選択できる。
相対パスはプロジェクトのルートを基準に解決する。

```sh
M5PAPER_CONFIG=config.office.local.json pio run
M5PAPER_CONFIG=config.office.local.json pio run -t upload --upload-port /dev/ttyUSB0
```

書き込み時にも同じ設定ファイルを指定する。
`config.*.local.json` も Git 管理対象外。他の場所に設定を置く場合は絶対パスも使える。
設定ファイルがない場合や JSON の形式が不正な場合はビルドを停止する。
ネットワーク設定なしでビルドするには、空の値が入った `config.example.json` を指定する。

```json
{
  "hostname": "paper",
  "wifi": {
    "ssid": "your-ssid",
    "password": "your-password"
  },
  "api_tokens": {
    "device": "",
    "google_calendar": "",
    "codex": "",
    "claude": ""
  }
}
```

`hostname` は Wi-Fi のホスト名と mDNS 名に共通で使用する（省略時は `paper`）。
`.local` を付けず、1〜63 文字の英小文字・数字・ハイフンで指定する。
先頭と末尾のハイフンは使用できない。

`api_tokens` は任意の名前と文字列を追加でき、ファームウェアから
`config::apiToken("codex")` のように参照できる。未定義の名前は空文字列を返す。
`device` に空でない値を設定すると、`/api/message` の投稿・取得と `/api/calendar`・`/api/usage` の取得に
`Authorization: Bearer <deviceの値>` が必要になる。空の場合は認証なし。
`/api/health` は認証不要。`api_tokens.google_calendar` / `codex` / `claude` は使用しない。
カレンダーは下記のサービスアカウントで認証する。

設定値はビルド前に `.pio/build/m5paper/generated/build_config.h` に変換する。
コンパイラーのコマンドラインへ秘密値を渡さず、設定内容が変わると再コンパイルする。
生成ヘッダーやファームウェアには使用する秘密値が含まれるため、配布物として扱わない。
旧 `include/secrets.h` は読み込まない。

PlatformIO のパッケージとキャッシュは `.pio/core` に保存する。
初回ビルド時は依存パッケージ取得のためインターネット接続が必要。
プラットフォームと表示ライブラリのバージョンは `platformio.ini` に固定している。
PlatformIO の `esp32dev` 定義に初代 Paper の Flash / PSRAM 設定を追加し、
M5Unified に初代 Paper のボード識別を指定する。

設定読み込みの検証は `python3 -m unittest discover -s tests` で実行できる。

## Google カレンダー

Google Cloud のプロジェクトで Google Calendar API を有効にし、サービスアカウントを
作成して JSON キーをダウンロードする。プロジェクトへの IAM ロール付与や
ドメイン全体の委任は不要。キーは `credentials/` 内に保存する。
このディレクトリ全体は Git 管理対象外。キーは SD へコピーしない。

Google カレンダーの設定で表示対象のカレンダーを、そのサービスアカウントの
`client_email` に「予定の表示（すべての予定の詳細）」で共有する。
カレンダーの「設定と共有」→「カレンダーの統合」からカレンダー ID を取得する。
サービスアカウント自身の `primary` ではなく、この ID を指定する。
Workspace の共有制限で詳細を共有できない場合は、別の認証方式が必要。

`config.local.json` に以下の `calendar` オブジェクトを追加する。

```json
"calendar": {
  "id": "your-calendar-id@gmail.com",
  "service_account_file": "credentials/your-key.json",
  "refresh_seconds": 300
}
```

キーファイルの相対パスは **設定 JSON のディレクトリ** が基準。
設定を省略、または ID とファイル名を両方空にすると連携を無効にする。
更新間隔は 60〜86400 秒（既定 300 秒）。設定・キー変更後は再ビルドして書き込む。
ビルドにはキーのメールアドレスと秘密鍵が埋め込まれる。

Paper が SNTP で時刻同期後、RS256 の署名付き JWT を作り、Google の OAuth
エンドポイントでアクセストークンを取得・期限前に更新する。
スコープは `calendar.events.readonly`。Mozilla の CA 証明書バンドルを使って
HTTPS の証明書とホスト名を検証する。PC や公開エンドポイントは常時稼働不要。

- 表示期間は **日本時間の今日 0:00 から 7 日後 0:00 未満**。初版のタイムゾーンは Asia/Tokyo 固定。
- 繰り返し予定は Google API の `singleEvents=true` で展開し、キャンセルは表示しない。
- 終日は終了日を含まない。日をまたぐ予定は各日に表示し、翌日以降は「継続」と表示する。
- 今日からの 7 日を横 7 列に表示。終日予定は灰色の帯で、複数日にまたがる帯はつなげる。
  時刻付き予定は「時間 予定名」を最大 2 行で表示し、入りきらない予定は「他 N 件」。
  終日予定を先に、その後は開始時刻順に配置する。
- 50 件ずつ最大 4 ページ／200 件を取得。上限到達時はカレンダー下端に通知する。
- 通信は別タスクで行う。取得失敗時は RAM 内の前回データを保持し、60 秒後に再試行する。
  TLS 通信とフォント描画のメモリ使用が重ならないよう、通信中の再描画は完了後に回す。
  その間もメッセージ API は受け付ける。
  カレンダーのキャッシュは再起動で消える。保存済みメッセージは引き続き復元する。
- 日付が変わると表示期間を切り替え、再取得する。

`GET /api/calendar` は取得した予定と状態を JSON で返す。
`configured`、`clock_ready`、`ready`、`stale`、`limited`、`fetched_at`（Unix 秒）、
`error`、`time_zone`、`events` が含まれる。画面で省略した予定も取得範囲内なら含む。
`ready=false` の間は「予定なし」と断定しない。
`auth_http_400` はキーや時刻、`calendar_http_403_accessNotConfigured` は API 未有効化、
その他の `calendar_http_403` は閲覧権限やスコープ、
`calendar_http_404` は ID と共有先を確認する。負数は接続・受信エラー。
ログや API に秘密鍵・アクセストークンは出力しない。

```sh
curl http://paper.local/api/calendar
```

認証仕様: [Google サービスアカウント](https://developers.google.com/identity/protocols/oauth2/service-account)、
取得仕様: [Events: list](https://developers.google.com/workspace/calendar/api/v3/reference/events/list)。

## LLM 利用枠

Paper が Codex / Claude の利用状況を HTTPS で直接取得する。既定 5 分ごと。
グラフは「使用した割合」で、残量ではない。タイトル横に日本時間のリセット日時を表示する。
上から Codex / Claude のセッション、週間、右下に Claude の Fable 週間を配置する。
Fable の独立した枠がレスポンスに存在する場合だけ追加する。左下は空き。

### 初回の専用ログイン

普段の CLI と同じ refresh token を複製すると、更新が競合する可能性があるため、
本体専用のログインを作る。初回のみ PC とブラウザーが必要。その後は Paper 単体で動作する。

Codex は通常の CLI の認証情報を変更しない専用セットアップを使う。

```sh
python3 scripts/login_codex.py
```

表示された OpenAI のデバイス認証 URL を開き、ワンタイムコードを入力する。
`credentials/codex-paper.json` に保存される。既存ファイルがある場合は上書きしない。
再ログインが必要な場合は、このファイルを退避してから実行する。

Claude は専用ディレクトリを指定して、別ターミナルでログインする。

```sh
CLAUDE_CONFIG_DIR="$PWD/credentials/claude-paper" claude auth login --claudeai
```

認証コードの入力が求められた場合はそのターミナルに貼り付ける。
`credentials/claude-paper/.credentials.json` が作成される。
Paper へ渡した後、この専用ディレクトリで Claude を実行しない（認証更新の所有者は Paper）。

`config.local.json` に次を追加してビルド・書き込む。

```json
"usage": {
  "codex_credentials_file": "credentials/codex-paper.json",
  "claude_credentials_file": "credentials/claude-paper/.credentials.json",
  "refresh_seconds": 300
}
```

相対パスは設定 JSON のディレクトリが基準。片方だけ設定することもできる。
更新間隔は 60〜3600 秒。認証情報は `credentials/` と生成ヘッダー・ファームウェア内にあり、
Git 管理対象外。API やログにはトークンを出さない。

### 継続運用と状態表示

- 初回は refresh token からアクセストークンを取得し、期限の 5 分前から自動更新する。
- 更新後のトークン一式は NVS の `paper-usage` にまとめて保存し、再起動後も利用する。
  元の認証ファイルが同じなら、再ビルド・再書き込みしても更新済みトークンを優先する。
  新しい専用ログインのファイルを指定すると、そのログインへ切り替える。
- Codex の primary が週間の場合もあるため、枠の期間を見てセッション／週間を割り当てる。
  返ってこない枠は「対象枠なし」と `--`。未開始などリセット日時が null の枠は「リセット未定」。
- リセット日時を過ぎた古い数値はバーを消して「更新待ち」。勝手に 0% にしない。
- エラー時は前回値を保持。取得から 15 分を超えた場合も「前回」と灰色のバーで示す。
- 429 / 503 の `Retry-After`（秒）を考慮し、通常の失敗は 60 秒後に再試行する。
- カレンダー通信・利用状況通信・フォント描画を直列にして内部 RAM の不足を避ける。
  API のリクエスト処理は継続する。

`GET /api/usage` は `codex` と `claude` の各 `configured` / `fetched_at` / `stale` / `error`、
`session` / `weekly` / `models` を返す。枠は `used_percent` と `resets_at`（Unix 秒または null）。
モデル別の追加枠は `models` に入り、描画対象以外も確認できる。
認証更新が 400 / 401 で失敗し続ける場合は、専用ログインを作り直して再ビルドする。
本体内の更新済みトークンを消す NVS 初期化後も、新しい専用ログインが必要になる場合がある。

利用率・リセット時刻というデータ項目は
[OpenAI Docs の App Server](https://learn.chatgpt.com/docs/app-server#6-rate-limits-chatgpt) と
[Claude Code status line](https://code.claude.com/docs/en/statusline) にも定義されている。
ただし本実装が Paper から直接呼ぶ `/backend-api/wham/usage` と `/api/oauth/usage` は
一般向けの安定した公開 API として保証されたものではなく、CLI の実装・実際の応答に合わせている。
プロバイダー側の変更時には修正が必要になり得る。

## API

Wi-Fi 接続後、シリアル出力に本体の IP アドレスを表示する。
同じ LAN 内から HTTP / port 80 でアクセスする。
mDNS 対応端末からは `http://paper.local` でもアクセスできる。
HTTP サービスを `_http._tcp` として広告し、Wi-Fi 再接続時に mDNS を再登録する。

```sh
curl http://paper.local/api/health
curl http://<M5PaperのIP>/api/health
```

```json
{"status":"ok","device":"m5paper","uptime_ms":12345,"free_heap_bytes":180000,"hostname":"paper","sd_ready":true,"storage_ready":true,"message_bytes":0,"message_pending":false,"message_truncated":false,"body_font":"MPLUS1p-Medium.ttf"}
```

値は実行時の状態によって変わる。未定義のパスは JSON の 404 を返す。
`message_pending` は新しいメッセージの画面反映待ち、`message_truncated` は
最後に描画したメッセージを画面上で省略したかどうかを表す。
`calendar_configured` / `calendar_ready` / `clock_ready` はカレンダーと時刻同期の状態。
スリープは行わず、投稿・カレンダー更新・日付変更で再描画する。

### 文字列投稿

最新の 1 件を UTF-8 の `text/plain` で送る。JSON ではなく本文そのものを渡す。

```sh
curl --fail-with-body http://paper.local/api/message \
  -H 'Content-Type: text/plain; charset=utf-8' \
  --data-binary '文字列投稿が使えるようになりました。'

# 改行を含むファイルから投稿
curl --fail-with-body http://paper.local/api/message \
  -H 'Content-Type: text/plain; charset=utf-8' \
  --data-binary @message.txt

# 保存された全文を取得
curl http://paper.local/api/message

# 空文字列でクリア
curl --fail-with-body http://paper.local/api/message \
  -H 'Content-Type: text/plain; charset=utf-8' --data-binary ''
```

最大 4096 バイト。`Content-Length` が必要（上記の curl では自動設定される）。
改行とタブ以外の制御文字、不正な UTF-8 は受け付けない。
本文は本体の NVS に保存してから `202 {"status":"accepted","bytes":...}` を返し、
続いて画面を更新する。同じ本文なら `200 {"status":"unchanged","bytes":...}` を返し、
再保存・再描画を行わない。連続投稿では画面更新を最短 2 秒間隔にまとめる。
再起動時には最後に保存された全文から画面を復元する。

`GET /api/message` は保存した全文を `text/plain; charset=utf-8` で返す。
画面では実際の字幅で折り返し、収まらない場合は末尾に「…」を付ける。
CRLF / CR は表示時に改行、タブは空白 4 個として扱う。絵文字など BMP 外の文字は
描画ライブラリの制約により画面上で「□」に置き換えるが、保存・取得する原文は保持する。
空文字列のときは「メッセージを待っています」を表示する。

主なエラーは `400`（不正な文字や本文）、`401`（設定したトークンが不一致）、
`405`（未対応メソッド）、`411`（Content-Length なし）、`413`（サイズ超過）、
`415`（Content-Type 不一致）、`500`（保存失敗）、`503`（保存領域未初期化）。
エラー時は直前のメッセージを維持する。

## 日本語フォント

横向き表示は `setRotation(3)` を使用する（初期版の向きから 180 度回転）。
見出しは置かず、上部に横 7 列のカレンダー、中段にメッセージ本文を最大 3 行、
最下段に利用率とリセット日時のグラフを配置する。
カレンダーは高さ 216 px。予定用の 5 行に収まらない分は件数で示し、
カレンダー・メッセージ・グラフ各段の間に余白を取る。
左列 Codex・右列 Claude、上段セッション・中段週間、右下に Fable 週間。
グラフの行間は詰め、上中下の領域間には余白を取る。Wi-Fi 状態と URL の常設表示はしない。
`REGULAR` は予定（20 px）、メッセージ（26 px）、使用率（18 px）、リセット日時（16 px）、
`BOLD` は曜日・日付（22 px）とグラフタイトル（20 px）に使用する。
今日の日付は黒丸で強調する。
TTF の描画には OpenFontRender / FreeType を使用する。

フォント本体はリポジトリに同梱しない。別途用意した TTF を SD のルート、または
`/fonts` などのフォルダーに置く。`config.local.json` の `fonts` で役割ごとの
ファイル名を指定し、ビルド・書き込みする。既定値は以下のとおり。

```json
"fonts": {
  "REGULAR": "MPLUS1p-Medium.ttf",
  "BOLD": "MPLUS1p-ExtraBold.ttf"
}
```

ファイル名だけを指定する（ディレクトリ名は含めない）。MPLUS1p 以外の TTF も指定できる。
空文字にすると、その役割には内蔵日本語フォントを使う。
ルートから 4 階層下まで探索し、大文字小文字・ハイフン・アンダースコア・空白の違いは無視する。
同じ名前のファイルが複数ある場合は最初に見つかったものを使用する。
フォントファイルは読み込みのみ行い、SD への書き込みは行わない。
差し替え後は再起動する。SD や TTF が読めない場合は内蔵日本語フォントに切り替える。

初代 Paper は SD と EPD が SPI を共有するため、設定した 2 つの TTF を
起動時に PSRAM に読み込む（各 3 MB 以下。既定の組み合わせは合計約 3.4 MB）。描画中に SD へアクセスせず、
RAM 上の 16 階調キャンバスへ描画して最後に画面へ転送する。
フォント用 PSRAM の確保や読み込みに失敗した場合も内蔵フォントへ切り替える。
シリアルログと `/api/health` の `sd_ready` / `body_font` で
SD のマウント状態と実際に使った本文フォントを確認できる。

## ライセンス

本プロジェクトのコードは [0BSD](LICENSE)。依存ライブラリと各自で用意するフォントには、
それぞれのライセンスが適用される。

## 参考

- [M5Paper 公式 Arduino ガイド](https://docs.m5stack.com/en/arduino/m5paper/program)
- [M5Unified](https://github.com/m5stack/M5Unified)
- [M5GFX](https://github.com/m5stack/M5GFX)
- [OpenFontRender](https://github.com/takkaO/OpenFontRender)
