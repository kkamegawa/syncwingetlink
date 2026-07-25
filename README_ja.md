# syncwingetlink（日本語）

> winget のポータブルパッケージで作られるべきシンボリックリンクの欠落・破損を検出し、再作成するネイティブ CLI ツール（Windows 11 24H2+）。

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](./LICENSE)

📖 The canonical English version is [`README.md`](./README.md).

## これは何？

winget でポータブルパッケージをインストールすると、本来はコマンドエイリアス用の
シンボリックリンクが `%LOCALAPPDATA%\Microsoft\WinGet\Links\<alias>.exe` に作成され、
このフォルダが `PATH` に含まれることで CLI から呼び出せます。

しかし環境によっては、この symlink が作成されない／壊れる不具合が報告されています。
その結果、`codex_0.x_x86_64-pc-windows-msvc.exe` のような長い実ファイル名でしか
起動できず、`codex` のような短いエイリアスが使えなくなります。

**syncwingetlink** は、インストール済みのポータブルパッケージを列挙し、
`Links` フォルダにあるべきリンクと突き合わせて、欠落・破損しているものを検出し、
ユーザー確認のうえで再作成します。

## 主な特徴

- 🔎 **検出**: インストール済み portable パッケージを列挙し、リンク状態を
  `Ok / Missing / Broken / Mismatch` に分類
- 🔗 **修復**: 欠落・破損した symlink をユーザー確認のうえで作成（`--dry-run` 対応）
- 🧩 **エイリアス正規表現ルール**: `codex-x86_64-pc-windows-msvc.exe → codex.exe` のような
  プラットフォーム接尾辞の除去ルールをカスタマイズ可能
- 🖥️ **CLI / TUI**: 通常は CLI、`--tui` で対話的なチェックリスト操作
- ⚡ **winget COM API を優先**: `Microsoft.Management.Deployment` から権威的に列挙し、
  利用不可時はファイルシステム走査へ自動フォールバック

## 動作環境

- Windows 11 24H2 (build 26100) 以降
- x64 / arm64
- symlink 作成には**開発者モード**有効（推奨）または管理者権限が必要

## インストール

> 準備中。リリースが公開されたら GitHub Releases から単一 exe を取得できます。

```powershell
# （公開後の例）
winget install <publisher>.syncwingetlink
```

## 使い方

```powershell
# 検出のみ（読み取り専用）
syncwingetlink scan

# 欠落・破損リンクを修復（確認プロンプトあり）
syncwingetlink fix

# 何が行われるかだけ確認（副作用なし）
syncwingetlink fix --dry-run

# 対話 TUI で選択して一括作成
syncwingetlink fix --tui

# あるファイル名にどの置換ルールが適用されるか確認
syncwingetlink test-rule "codex-x86_64-pc-windows-msvc.exe"
```

### 主なオプション

| オプション | 説明 |
|---|---|
| `--source com\|fs\|auto` | パッケージ列挙のソース（既定 `auto`：COM 優先→FS 縮退） |
| `--dry-run` | 実行せず計画のみ表示（`fix`） |
| `--yes`, `-y` | 確認をスキップして実行 |
| `--rules <path>` | 置換ルール JSON のパス |
| `--tui` | 対話 TUI モード |
| `--json` | 結果を JSON 出力（スクリプト連携） |
| `--help` / `--version` | ヘルプ / バージョン |

### 終了コード

| コード | 意味 |
|---|---|
| 0 | 正常（修復不要 or 成功） |
| 1 | 修復が必要だが未実行 |
| 2 | 権限不足（開発者モード無効 & 非管理者） |
| 3 | 引数 / 設定エラー |
| 10 | 一部の修復に失敗 |

## エイリアス置換ルール

実ファイル名からエイリアス名を導く正規表現ルールを JSON で定義できます。
詳しくは [`docs/rules.md`](./docs/rules.md) を参照してください。

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

## ビルド（開発者向け）

**Visual Studio 2026**（プラットフォームツールセット v145）と Windows SDK 10.0.26100.0 が
必要です。Developer PowerShell for VS 2026 から実行してください。

```powershell
msbuild syncwingetlink.sln -p:Configuration=Release -p:Platform=x64 -m
vstest.console.exe build\x64\Release\syncwingetlink.tests.dll /Platform:x64
```

単体テストには MSTest（Microsoft Unit Testing Framework for C++）を使用し、Visual Studio の
テストエクスプローラーからも実行できます。

詳細な設計は [`docs/PLAN.md`](./docs/PLAN.md)、作業単位は [`docs/TODO.md`](./docs/TODO.md)、
アーキテクチャ上の意思決定は [`docs/adr.md`](./docs/adr.md) を参照してください。

## リポジトリのセットアップ（フォーク後）

識別可能な情報をコミットしないよう、URL とメールアドレスはプレースホルダのままにして
あります。次のいずれかのスクリプトで一度だけ置換してください（内容は同等です）。

```powershell
./tools/Set-RepositoryPlaceholders.ps1 -Owner <owner> -SecurityContact <email>
```

```bash
tools/set-repository-placeholders.sh --owner <owner> --security-contact <email>
```

どちらも `-WhatIf` / `--dry-run` で事前確認できます。

## 貢献

歓迎します！[`CONTRIBUTING.md`](./CONTRIBUTING.md) をご覧ください。
AI コーディングエージェントを使う場合は [`AGENTS.md`](./AGENTS.md) を先に読んでください。

セキュリティ上の問題は公開 Issue ではなく [`SECURITY.md`](./SECURITY.md) の手順で
非公開に報告してください。

## ライセンス

[MIT License](./LICENSE)

## 免責

本ツールはシンボリックリンクの作成・削除を行います。`scan` は読み取り専用ですが、
`fix` を使う際は `--dry-run` で内容を確認することを推奨します。
winget 本体のデータベースは変更しません。
