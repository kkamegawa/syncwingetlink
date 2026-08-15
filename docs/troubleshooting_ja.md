<!-- SPDX-License-Identifier: MIT -->

# トラブルシューティング

このページでは、`syncwingetlink` が報告する代表的な失敗パターン、特に
winget COM API を使ったパッケージ列挙に関するものをまとめます。

## `--source com` で `APPMODEL_ERROR_NO_PACKAGE` が出て失敗する（終了コード 4）

**症状**

```text
The winget PackageManager COM server rejected typed activation from this unpackaged process
(APPMODEL_ERROR_NO_PACKAGE, HRESULT 0x80073d54)
hint: This host rejected typed WinRT activation from an unpackaged process; re-run with --source fs, or use --source auto to fall back automatically. See https://github.com/kkamegawa/syncwingetlink/blob/main/docs/troubleshooting.md
```

**原因**

一部の環境では winget COM サーバー自体はインストール済みで `winget` も動作します
が、このアンパッケージドなデスクトッププロセスから `PackageManager` の型付き
WinRT アクティベーションが拒否されます。

**対処**

`--source fs` を付けて再実行するか、既定の `--source auto` を使って自動フォール
バックに任せてください。

## `--verbose` で `used: filesystem (degraded: ...)` と表示される

**症状**

```text
verbose: package source - requested: auto, used: filesystem (degraded: <reason>)
```

**原因**

`--source auto` は最初に COM を試し、`PackageSourceError` で失敗した場合に
ファイルシステム走査へ降格します。COM アクティベーションが使えない環境では想定
内の動作です。

**対処**

コマンド自体が成功しているなら追加対応は不要です。その環境で最初から COM を避
けたい場合は `--source fs` を付けてください。

## 終了コード 2: シンボリックリンク作成権限が不足している

**症状**

`fix` が権限不足エラーを報告して終了コード `2` で終了します。

**原因**

昇格せずにシンボリックリンクを作成するには、Developer Mode（または同等の権限）
が必要です。

**対処**

Developer Mode を有効にするか、昇格したシェルから再実行してください。

## 終了コード 3: `rules.json` が不正

**症状**

ルールの解析/検証エラーが出て、終了コード `3` で終了します。

**原因**

置換ルール JSON の形式が壊れているか、想定スキーマに一致していません。

**対処**

[`rules_ja.md`](./rules_ja.md) の形式に従ってファイルを修正し、再実行してくだ
さい。

## パッケージが 1 件も見つからない

**症状**

`scan` や `fix` は完了するが、インストール済み portable package が 0 件と報告さ
れます。

**原因**

実際にユーザー スコープの portable package が存在しないか、走査対象ディレクトリ
が期待と異なっている可能性があります。

**対処**

既定以外の場所に packages がある場合は `--packages-dir <path>` を指定してくだ
さい。machine-scope 対応は初回リリースの対象外です。
