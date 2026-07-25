# エイリアス置換ルール（日本語）

> **これは翻訳です。** 正式版は英語の [`rules.md`](./rules.md) です。差異がある場合は英語版が優先されます。
> **AI エージェントはこのファイルを読み込まないでください（翻訳のみ）。**

syncwingetlink は、ポータブルパッケージの実ファイル名から
`Links` に作成すべきエイリアス名（`<alias>.exe`）を導きます。
その際、プラットフォーム接尾辞やバージョンを取り除くために
**正規表現の置換ルール**を使用します。

例:

```
codex-x86_64-pc-windows-msvc.exe   →   codex.exe
restic_0.15.2_windows_amd64.exe    →   restic.exe
```

## エイリアス決定の優先順位

エイリアス名は次の優先順位で決定されます。

1. **COM メタデータ**（winget COM API が `PortableCommandAlias` 相当を返す場合）
2. **本ドキュメントの正規表現置換ルール**
3. **実ファイル名そのまま**（どのルールにも一致しない場合）

## ルールファイルの場所と優先順位

読み込み優先順位（上が優先）:

1. `--rules <path>` で明示指定した JSON
2. `%LOCALAPPDATA%\syncwingetlink\rules.json`（ユーザー設定）
3. バイナリ埋め込みの既定ルール

## JSON スキーマ

```json
{
  "version": 1,
  "rules": [
    {
      "name": "strip-rust-target-triple",
      "pattern": "^(.+?)[-_](x86_64|aarch64|i686)-pc-windows-(msvc|gnu)(\\.exe)$",
      "replacement": "$1.exe",
      "flags": ["ignorecase"]
    }
  ]
}
```

| フィールド | 必須 | 説明 |
|---|---|---|
| `version` | ✔ | スキーマバージョン。現在は `1`。 |
| `rules` | ✔ | ルールの配列。**先頭から順に評価**し、最初に一致したものを適用。 |
| `rules[].name` | ✔ | ルールの識別名（`test-rule` の出力やログで使用）。 |
| `rules[].pattern` | ✔ | 実ファイル名に対する正規表現（ECMAScript 文法）。 |
| `rules[].replacement` | ✔ | 置換後の文字列。キャプチャは `$1`, `$2` … で参照。 |
| `rules[].flags` | ✖ | オプション。現在は `ignorecase` をサポート。 |

> **注意**: 初版では `std::regex`（ECMAScript）を前提とし、
> **番号付きキャプチャ (`$1`)** を既定とします。
> `std::regex` は名前付きキャプチャに非対応のため、`(?<name>...)` や `${name}` は
> 将来対応予定です（RE2 採用を検討中）。

## 評価ルール

- ルールは配列の**先頭から順に評価**され、**最初に一致したルールのみ**適用されます。
- どのルールにも一致しない場合、実ファイル名がそのままエイリアス名になります。
- 複数の exe が**同じエイリアス**に解決される場合は衝突として警告され、
  自動作成は行われません（ユーザーが選択）。

## 動作確認: `test-rule`

あるファイル名にどのルールが適用され、どのエイリアスになるかを確認できます。

```powershell
syncwingetlink test-rule "codex-x86_64-pc-windows-msvc.exe"
```

出力例:

```
input:        codex-x86_64-pc-windows-msvc.exe
matched rule: strip-rust-target-triple
alias:        codex.exe
```

## サンプル集

### Rust ターゲットトリプルの除去

```json
{
  "name": "strip-rust-target-triple",
  "pattern": "^(.+?)[-_](x86_64|aarch64|i686)-pc-windows-(msvc|gnu)(\\.exe)$",
  "replacement": "$1.exe",
  "flags": ["ignorecase"]
}
```

### バージョン + アーキテクチャの除去

```json
{
  "name": "strip-version-and-arch",
  "pattern": "^(.+?)[-_]v?\\d+\\.\\d+[^\\\\/]*?(windows|win)?[-_]?(amd64|x64|arm64)?\\.exe$",
  "replacement": "$1.exe",
  "flags": ["ignorecase"]
}
```

### 特定ツールの固定マッピング

```json
{
  "name": "map-kubelogin",
  "pattern": "^kubelogin.*\\.exe$",
  "replacement": "kubectl-oidc_login.exe",
  "flags": ["ignorecase"]
}
```

> 固定マッピングのように前方一致で十分な場合でも、`pattern` は正規表現として
> 評価される点に注意してください（`.` などはエスケープが必要）。
