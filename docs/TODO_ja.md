# syncwingetlink — TODO（日本語）

> **これは翻訳です。** 正式版は英語の [`TODO.md`](./TODO.md) です。差異がある場合は英語版が優先されます。
> **AI エージェントはこのファイルを読み込まないでください（翻訳のみ）。**


各タスクは上から順に着手可能。`[core]` はテスト必須。1 タスク = 小さめの PR を想定。

## M0. プロジェクト立ち上げ

ビルドシステムに関する意思決定は [`adr.md`](./adr.md)（ADR-0001〜ADR-0004）に記録済みです。

- [x] `.editorconfig` / clang-format / clang-tidy 設定
- [x] ビルドシステムの決定を `docs/adr.md` に記録
- [ ] MSBuild ソリューション `syncwingetlink.sln` 作成
      （VS2026 / プラットフォームツールセット v145, C++20, `Debug|Release` × `x64|ARM64`）
- [ ] ADR-0002 に従い 3 プロジェクトを追加：`syncwingetlink.core`（静的ライブラリ）、
      `syncwingetlink`（実行可能ファイル）、`syncwingetlink.tests`（MSTest DLL）
- [ ] `Directory.Build.props` + `props/syncwingetlink.common.props` を作成し、
      全プロジェクトからインポート（ADR-0001〜ADR-0003 から再導出。初期案は破棄済み、
      `task.md` 参照）
- [ ] app manifest 追加：`longPathAware=true`, `requestedExecutionLevel=asInvoker`
- [ ] MSTest（Microsoft Unit Testing Framework for C++）を設定し、
      `vstest.console.exe` で通るスモークテストを 1 件用意
- [ ] **ADR-0003 の解決**：CppUnitTest のライブラリが `/MT` のテスト DLL とリンクできるか
      検証し、`StaticRuntime` の既定値を確定して ADR を更新
- [ ] C++/WinRT は **Windows SDK 同梱**のヘッダーを使う。パッケージ不要（ADR-0007）。
      `Microsoft.Windows.CppWinRT` は追加しない
- [ ] `vcpkg.json` は、ネイティブ依存が実際に必要になった場合**のみ**追加する
      （ADR-0007）。`builtin-baseline` を固定し、triplet を `StaticRuntime` に一致させる
- [ ] **調査**：`Microsoft.Management.Deployment` の winmd の入手元を確定し、
      SDK 同梱の `cppwinrt.exe` でプリビルド時に projection を生成する
      （`adr.md` 未解決事項 1）。確定するまで `WingetComSource` は実装しない
- [ ] CI（GitHub Actions）：`msbuild` + `vstest.console.exe` を x64 / ARM64 で実行。
      `windows-11-arm` ランナーが使えるか確認し、使えない場合 ARM64 はビルドのみとし
      その旨を明記（`adr.md` 未解決事項 3）
- [ ] vcpkg を理解する脆弱性スキャナを評価し CI に組み込む（OSV-Scanner が候補だが
      vcpkg 対応は未確認）。Dependabot は vcpkg に非対応（`adr.md` 未解決事項 6）。
      それまでゲートは手動であり、自動化されているかのように書かない

## M1. パス / モデル基盤
- [ ] `core/Model.h`：`InstalledPackage`, `PackageExe`, `LinkStatus{Ok,Missing,Broken,Mismatch}`, `RepairItem`, `AppOptions` 定義
- [ ] `core/Paths`：`SHGetKnownFolderPath(FOLDERID_LocalAppData)` で
      Links パス（および FS フォールバック用 Packages パス）を解決（`--links-dir`/`--packages-dir` 上書き対応）
- [ ] `\\?\` ロングパス正規化ヘルパ
- [ ] `core/IPackageSource.h`：インストール済みパッケージ列挙の抽象インターフェース定義

## M2. パッケージ列挙（COM 優先 + FS フォールバック）
- [ ] `[core] core/WingetComSource`：C++/WinRT で `winrt::init_apartment()` →
      `PackageManager` 生成
- [ ] `GetLocalPackageCatalog(LocalPackageCatalog.InstalledPackages)` → `Connect()`
- [ ] `FindPackages()` で `CatalogPackage` を列挙、`InstalledVersion` から
      Id/Name/バージョン/インストール場所/（取得可なら）エイリアスを取得
- [ ] installer type = portable のパッケージに絞り込み
- [ ] COM 活性化失敗（App Installer 未導入/ポリシー無効/権限不足）を検出し例外処理
- [ ] `[core] core/FsScanSource`：Packages 配下を再帰探索し `*.exe` を収集（フォールバック）
- [ ] reparse point（symlink/junction）はデフォルトで辿らない（ループ防止）
- [ ] `--source com|fs|auto` の切替実装（auto は COM→FS 縮退）
- [ ] `--include`/`--exclude` の glob フィルタ
- [ ] 単体テスト：FsScanSource の exe 列挙、および COM/FS 切替ロジック

## M3. エイリアス解決 + 正規表現ルール（最重要）
- [ ] `[core] rules/RuleSet`：ルール JSON の読み込み・検証（不正時は終了コード 3）
- [ ] 既定ルールをバイナリ埋め込み（rust target triple 除去等）
- [ ] `[core] core/AliasResolver`：優先順位でエイリアス名決定
      （(1) COM メタデータ `PortableCommandAlias` → (2) 正規表現ルール → (3) 実ファイル名）
- [ ] `test-rule` サブコマンド：ファイル名 → 適用ルール名 → エイリアスを表示
- [ ] 単体テスト：`codex-x86_64-pc-windows-msvc.exe → codex.exe` を含むケース網羅
- [ ] ルール優先順位（`--rules` > ユーザー設定 > 埋め込み）の実装とテスト
- [ ] `docs/rules.md`：書式・キャプチャ・置換記法・サンプルを記載

## M4. リンク状態判定
- [ ] `[core] core/LinkInspector`：`Links\<alias>.exe` の状態を判定
- [ ] `GetFileAttributesW` + `FSCTL_GET_REPARSE_POINT` で symlink ターゲット解決
- [ ] コピー配置（通常ファイル）や別ターゲットを `Mismatch` 判定
- [ ] エイリアス衝突（複数 exe→同一 alias）の検出と警告
- [ ] 単体テスト：Ok/Missing/Broken/Mismatch の分類

## M5. symlink 作成サービス
- [ ] `core/SymlinkService`：`CreateSymbolicLinkW`
      + `SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE`
- [ ] 既存の壊れたリンクは削除→再作成
- [ ] 権限/開発者モード判定と切り分け（失敗時 終了コード 2 とガイダンス）
- [ ] `--dry-run` で副作用なしの計画出力

## M6. CLI
- [ ] `cli/ArgParser`：`scan`/`fix`/`test-rule` と各オプション
- [ ] `cli/Console`：UTF-8/UTF-16 出力、色付け、確認プロンプト（`--yes` 対応）
- [ ] `--json` 出力（スクリプト連携用）
- [ ] `main.cpp`：モード分岐と終了コードマッピング
- [ ] `--help`/`--version`

## M7. TUI（`--tui`）
- [ ] Console Virtual Terminal Sequences 有効化
      (`ENABLE_VIRTUAL_TERMINAL_PROCESSING`)
- [ ] 修復候補のチェックリスト UI（スペースで選択、Enter で実行）
- [ ] 実行進捗・結果サマリ表示

## M8. 品質 / 仕上げ
- [ ] 統合テスト：ダミーの Packages/Links ツリーで scan→fix→再scan が Ok になる
- [ ] 日本語パスでの表示・作成の動作確認
- [ ] エラーメッセージの日本語/英語対応方針を決定
- [ ] README（インストール、使い方、権限要件、例）
- [ ] リリース：署名なし single exe（静的リンク）を GitHub Releases に添付
      （`-p:StaticRuntime=true` でビルド。ADR-0003 の解決が前提）

## M9. ドキュメント（COM API）
- [ ] `docs/com-api.md`：COM 活性化手順、必要ケイパビリティ、
      out-of-proc/in-proc の違い、失敗時フォールバックの挙動

## 将来拡張（別マイルストーン）
- [ ] winget `PortableIndex`(sqlite) を read-only 参照（COM/FS が不十分な場合の最終手段）
- [ ] machine スコープ対応（要管理者）
- [ ] `PATH` に Links が含まれるかの検証・登録支援
- [ ] タスクスケジューラ登録で定期自動修復
