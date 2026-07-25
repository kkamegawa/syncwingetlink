# syncwingetlink — 実装プラン（日本語）

> **これは翻訳です。** 正式版は英語の [`PLAN.md`](./PLAN.md) です。差異がある場合は英語版が優先されます。
> **AI エージェントはこのファイルを読み込まないでください（翻訳のみ）。**


> winget のポータブルパッケージで `%LOCALAPPDATA%\Microsoft\WinGet\Links` にシンボリックリンクが作られない／壊れた場合に、`packages` 配下の実体 exe を再帰探索し、欠落しているリンクを検出してユーザー確認のうえ再作成するネイティブ CLI ツール。

---

## 1. 背景 / 問題

- winget でポータブルパッケージをインストールすると、実体は
  `%LOCALAPPDATA%\Microsoft\WinGet\Packages\<Package>\...` 配下に展開される。
- 本来はコマンドエイリアス用の symlink が
  `%LOCALAPPDATA%\Microsoft\WinGet\Links\<alias>.exe` に作成され、
  この `Links` ディレクトリが `PATH` に含まれることで CLI から呼び出せる。
- しかし環境によっては symlink が作成されない／壊れる不具合が報告されている
  （例: `PortableCommandAlias` があるのに `Links` に反映されない、
  ポータブルインストール全般で symlink が生成されない など）。
- winget 内部は `PortableInstaller` が `CreateSymlink / VerifySymlink` で
  symlink を管理し、reparse point 非対応時はコピーにフォールバックする設計。
- 結果として、ユーザーは `codex_0.x_x86_64-pc-windows-msvc.exe` のような
  長い実ファイル名でしか起動できず、`codex` エイリアスが使えない。

## 2. 目的 / ゴール

1. `Packages` 配下を再帰探索し、実体 exe を列挙する。
2. `Links` 配下の既存 symlink（またはコピー）と突き合わせ、
   「あるべきなのに無い／壊れている」リンクを検出する。
3. 欠落・破損リンクをユーザーに提示し、確認のうえ `mklink`（Win32 API）で作成／修復する。
4. `codex-x86_64-pc-windows-msvc.exe → codex.exe` のように、
   実ファイル名からエイリアス名を導く **正規表現の置換ルール** をサポートする。

## 3. データソース方針：winget COM API を優先（重要）

インストール済みポータブルパッケージの情報取得は、**sqlite (`PortableIndex`) の直接読み取りではなく、
winget の COM API (`Microsoft.Management.Deployment` 名前空間) を第一手段とする。**

### 理由
- COM API は CLI とは別の**安定・バージョン管理された公開インターフェース**で、
  Win32/デスクトップアプリから C++/WinRT 経由で利用できる（out-of-proc / in-proc 両対応）。
- sqlite の内部スキーマは非公開・将来変更のリスクがあり、直接依存は壊れやすい。
- winget サービス経由のため、権限・整合性チェック等も適切に扱われる。

### COM API での取得フロー
1. `PackageManager` を生成（`WindowsPackageManagerStandardFactory` 等のファクトリ経由）。
2. `GetLocalPackageCatalog(LocalPackageCatalog.InstalledPackages)` でローカル（インストール済み）カタログ参照を取得。
   - 必要に応じて `CreateCompositePackageCatalog` + `CompositeSearchBehavior.LocalCatalogs` で合成。
3. `PackageCatalogReference.Connect()` → `PackageCatalog` を取得。
4. `FindPackages(FindPackagesOptions)` で `CatalogPackage` 群を列挙。
5. 各 `CatalogPackage.InstalledVersion` (`PackageVersionInfo`) から
   識別子 (`Id`/`Name`)、バージョン、**インストール場所などのメタデータ**を取得。
6. インストーラー種別が **portable** のパッケージに絞り込む。

### 重要な限界と対処（正直な注意）
- COM API は「識別子・バージョン・インストール場所」等のメタデータは取得できるが、
  PortableIndex が保持する**ファイル単位の symlink エイリアス対応（実 exe → `<alias>.exe`）**
  まで完全に公開しているとは限らない。
- そのため **エイリアス名の決定は次の優先順位**とする：
  1. COM API が提供するメタデータ（`PortableCommandAlias` 相当が取得できる場合）
  2. §7 の正規表現置換ルール
  3. 実ファイル名そのまま
- COM 経由でインストール場所を確定し、その配下のファイル走査（§6）と突き合わせる、
  という「COM で権威的に列挙 → FS で実体確認」の二段構えにする。

### 実装上の選択肢
- **C++/WinRT** で `Microsoft.Management.Deployment` を projection して利用（推奨）。
- COM サーバーは out-of-proc（`WindowsPackageManagerServer.exe`）を既定とし、
  `CoCreateInstance` 相当のファクトリ活性化を用いる。
- COM 呼び出しには `packageQuery` ケイパビリティ（または Medium 以上の整合性レベル）が必要な点に留意。
- COM が利用不可（App Installer 未導入・ポリシー無効等）の場合は、
  §9 の **FS 走査フォールバック**に切り替える（後述の `--source` オプションで制御）。

## 3b. 非目標 (Out of Scope) — 初版では扱わない

- winget 本体の sqlite DB (`PortableIndex`) の**書き込み・整合性管理**。
  （読み取りは COM API を優先し、sqlite 直読みは最終フォールバックのみ。§9 参照）
- machine スコープ (`C:\Program Files\WinGet\Links`) の管理（初版は user スコープのみ）。
- winget のインストール／アンインストール処理そのものの代替。
- PATH 環境変数の登録（初版では検証・警告のみ）。

## 4. 対象環境 / 制約

- **OS**: Windows 11 24H2 (build 26100) 以降。
- **言語 / API**: C++ (C++20) + Win32 API。CRT/STL は利用可。
- **アーキテクチャ**: x64 primary、arm64 もビルド対象に含める。
- **ビルドシステム**: **MSBuild**（`.sln` + `.vcxproj`）。Visual Studio 2026 で駆動する。
  プラットフォームツールセット **v145**、Windows SDK 10.0.26100.0。
  CMake は使用しない — [`adr.md`](./adr.md) の ADR-0001 を参照。
- **単体テスト**: **MSTest** — Microsoft Unit Testing Framework for C++
  （`CppUnitTest.h`）。`vstest.console.exe` で実行する。ADR-0002 を参照。
- **依存関係**: 標準ライブラリと Windows API を優先する（ADR-0005）。ネイティブ依存が
  必要になった場合は **vcpkg**（マニフェストモード）で管理し、NuGet は使用しない
  （ADR-0007）。C++/WinRT は Windows SDK 同梱のものを使うため、依存は原則ゼロ。
- **文字コード**: 内部は UTF-16 (`std::wstring`) を基本、
  パスは `\\?\` プレフィックスでロングパス対応。
- **権限**:
  - Windows 11 では開発者モード有効時、非管理者でも symlink 作成が可能
    (`SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE`)。
  - 開発者モード無効の場合は管理者昇格が必要 → 検出して明示メッセージを出す。

## 5. アーキテクチャ / モジュール構成

ソリューションは **3 プロジェクト**に分割する。コアを静的ライブラリにするのは、
実行可能ファイルと MSTest DLL の両方からリンクできるようにするためである。C++ の MSTest
プロジェクトは DLL でなければならず、実行可能ファイルのオブジェクトはリンクできない。
[`adr.md`](./adr.md) の ADR-0002 / ADR-0004 を参照。

| プロジェクト | 種別 | 出力 |
|---|---|---|
| `src/syncwingetlink.core.vcxproj` | StaticLibrary | `syncwingetlink.core.lib` |
| `src/syncwingetlink.vcxproj` | Application | `syncwingetlink.exe` |
| `tests/syncwingetlink.tests.vcxproj` | DynamicLibrary | `syncwingetlink.tests.dll` |

```
syncwingetlink/
├─ syncwingetlink.sln          # VS2026 ソリューション：Debug|Release × x64|ARM64
├─ Directory.Build.props       # 共通プロパティ（toolset v145, SDK, 出力パス）
├─ props/
│  └─ syncwingetlink.common.props # 共通コンパイラ/リンカ設定（C++20, /W4 /WX, CRT）
├─ src/
│  ├─ syncwingetlink.core.vcxproj # 静的ライブラリ：cli/ core/ rules/ tui/
│  ├─ syncwingetlink.vcxproj   # 実行可能ファイル：main.cpp のみ
│  ├─ app.manifest             # longPathAware=true, requestedExecutionLevel=asInvoker
│  ├─ main.cpp                 # エントリポイント、引数パース、モード分岐
│  ├─ cli/
│  │  ├─ ArgParser.{h,cpp}     # フラグ解析（--help/--dry-run/--tui/--rules/--source 等）
│  │  └─ Console.{h,cpp}       # UTF-8/UTF-16 出力、色付け、確認プロンプト
│  ├─ core/
│  │  ├─ Paths.{h,cpp}         # 既知フォルダ解決 (SHGetKnownFolderPath) / Links パス
│  │  ├─ IPackageSource.h      # インストール済みパッケージ列挙の抽象IF（COM/FS 切替）
│  │  ├─ WingetComSource.{h,cpp} # ★COM API 実装（Microsoft.Management.Deployment, C++/WinRT）
│  │  ├─ FsScanSource.{h,cpp}  # フォールバック：Packages 再帰探索・exe 列挙
│  │  ├─ LinkInspector.{h,cpp} # Links 内 symlink 状態判定（欠落/破損/OK）
│  │  ├─ AliasResolver.{h,cpp} # メタデータ＞正規表現＞実名 の優先でエイリアス決定
│  │  ├─ SymlinkService.{h,cpp}# symlink 作成/削除/検証 (Win32)
│  │  └─ Model.h               # PackageExe / InstalledPackage / LinkStatus / Plan などの型
│  ├─ rules/
│  │  ├─ RuleSet.{h,cpp}       # 置換ルールの読み込み・適用
│  │  └─ default_rules.*       # 既定ルール（埋め込み or JSON）
│  └─ tui/
│     └─ TuiApp.{h,cpp}        # --tui 時の対話 UI（Console Virtual Terminal）
├─ tests/
│  └─ syncwingetlink.tests.vcxproj # MSTest（Microsoft Unit Testing Framework for C++）
└─ docs/
   ├─ rules.md                 # 置換ルールの書式・サンプル
   ├─ com-api.md               # COM API 利用手順・活性化・ケイパビリティ注意
   ├─ task.md                  # 作業記録
   └─ adr.md                   # アーキテクチャ上の意思決定記録
```

### データソース抽象化（COM とフォールバックの切替）
- `IPackageSource` を導入し、`WingetComSource`（既定）と `FsScanSource`（フォールバック）を差し替え可能にする。
- `--source com|fs|auto`（既定 `auto`）で選択。`auto` は COM を試し、失敗時に FS へ縮退。
- `WingetComSource` は「識別子・バージョン・インストール場所・可能ならエイリアス」を返し、
  `LinkInspector`/`AliasResolver` はソースに依存しない共通ロジックとして再利用する。

### レイヤ分割方針
- **core は Win32 依存を隔離**し、`AliasResolver` / `RuleSet` は純粋ロジックとして
  テスト可能にする（ファイルシステムはインターフェース経由でモック可能に）。
- CLI と TUI は `core` の同じ API を叩く薄い presentation 層にする。

## 6. 処理フロー

1. **パス解決**: `SHGetKnownFolderPath(FOLDERID_LocalAppData)` から
   `...\Microsoft\WinGet\Links` を組み立てる（`--links-dir` で上書き可）。
2. **インストール済みパッケージ列挙（COM 優先）**:
   `WingetComSource` で `PackageManager` → `GetLocalPackageCatalog(InstalledPackages)`
   → `Connect()` → `FindPackages()` を実行し、portable パッケージと
   その**インストール場所**・（取得できれば）エイリアス・バージョンを得る。
   - COM 不可時は `FsScanSource` にフォールバックし、
     `Packages` を再帰走査して `*.exe` を収集（`recursive_directory_iterator`）。
     reparse point はループ防止のためデフォルトで辿らない。
3. **エイリアス決定**: 優先順位で `<alias>.exe` を決める：
   (1) COM メタデータ（`PortableCommandAlias` 相当）→ (2) `AliasResolver` の正規表現ルール
   → (3) 実ファイル名そのまま。
4. **突き合わせ**: `Links\<alias>.exe` の状態を判定。
   - `Missing`: リンクが存在しない
   - `Broken`: symlink はあるがターゲットが存在しない／別の実体を指す
   - `Mismatch`: 実体は別の場所を指している（更新漏れ）
   - `Ok`: 正しくリンクされている
5. **プラン生成**: `Missing/Broken/Mismatch` を修復候補として一覧化。
6. **確認**: `--yes` 未指定なら 1 件ずつ or 一括でユーザーに確認（TUI ならチェックリスト）。
7. **実行**: `CreateSymbolicLinkW` で作成。既存の壊れたリンクは削除してから再作成。
   `--dry-run` の場合は実行せず計画のみ表示。
8. **結果レポート**: 作成/スキップ/失敗を集計して終了コードに反映。

## 7. エイリアス正規表現ルール（重要要件）

- 目的: `codex-x86_64-pc-windows-msvc.exe` → `codex.exe` のような
  プラットフォーム接尾辞を除去し、正規のエイリアス名を決める。
- ルールは **順序付きリスト**。最初にマッチしたルールを適用（または「全適用」モードも検討）。
- 各ルールの構造（JSON 例）:

```json
{
  "version": 1,
  "rules": [
    {
      "name": "strip-rust-target-triple",
      "pattern": "^(?<alias>.+?)[-_](x86_64|aarch64|i686)-pc-windows-(msvc|gnu)(\\.exe)$",
      "replacement": "${alias}.exe",
      "flags": ["ignorecase"]
    },
    {
      "name": "strip-version-and-arch",
      "pattern": "^(?<name>.+?)[-_]v?\\d+\\.\\d+.*?(windows|win)?[-_]?(amd64|x64|arm64)?\\.exe$",
      "replacement": "${name}.exe"
    }
  ]
}
```

- 実装: C++ 標準 `std::regex`（ECMAScript 文法）または RE2 の採用を検討。
  - **名前付きキャプチャ**が必要なら `std::regex` は名前付きグループ非対応のため、
    番号付きキャプチャに変換するか、RE2 / `std::regex` + 独自プレースホルダで対応する。
    → 初版は **番号付きキャプチャ (`$1`)** を既定書式にし、名前付きは将来拡張とする。
- ルール読み込み優先順位:
  1. `--rules <path>` で指定された JSON
  2. `%LOCALAPPDATA%\syncwingetlink\rules.json`（ユーザー設定）
  3. バイナリ埋め込みの既定ルール
- `--test-rule "<filename>"` で、ある実ファイル名にどのルールが適用され
  どのエイリアスになるかをドライで確認できるサブコマンドを用意する。

## 8. CLI 仕様（案）

```
syncwingetlink [command] [options]

Commands:
  scan            検出のみ（読み取り専用、既定コマンド）
  fix             欠落/破損リンクを作成・修復
  test-rule NAME  実ファイル名に対する置換結果を表示

Options:
  --source com|fs|auto  パッケージ列挙のソース（既定 auto：COM 優先→FS 縮退）
  --tui                 対話 TUI モードで実行
  --dry-run             実行せず計画のみ表示（fix 用）
  --yes, -y             すべての確認をスキップして実行
  --rules <path>        置換ルール JSON のパス
  --packages-dir <p>    Packages ディレクトリ上書き
  --links-dir <p>       Links ディレクトリ上書き
  --include <glob>      対象パッケージ/exe を絞り込み
  --exclude <glob>      除外
  --json                結果を JSON で出力（スクリプト連携）
  --verbose / --quiet   ログレベル
  --version / --help
```

### 終了コード
- `0`: 正常（修復不要 or 修復成功）
- `1`: 修復が必要だが未実行（scan で欠落検出時など、`--fail-on-missing` 指定時）
- `2`: 権限不足（開発者モード無効かつ非管理者で symlink 作成不可）
- `3`: 引数/設定エラー（不正なルール JSON 等）
- `10`: 一部の修復に失敗

## 9. フォールバック / 将来拡張

- **FS 走査フォールバック（初版に含む）**: COM API が利用不可の場合、
  `Packages` 配下を再帰走査して exe を列挙する `FsScanSource` に切り替える。
- **sqlite 直読み（最終フォールバック・任意）**: COM も FS も不十分な場合に限り、
  各パッケージの `PortableIndex`(sqlite) を**読み取り専用**で参照して
  `PortableCommandAlias` を取得する経路を検討（スキーマ変更リスクありのため優先度は最低）。
- machine スコープ対応（要管理者）。
- `PATH` に `Links` が含まれているかの検証と、未登録時の登録支援。
- winget イベント連携（インストール後フックでの自動修復）。
- 定期実行用のタスクスケジューラ登録サブコマンド。

## 10. 技術的リスク / 注意点

- **COM API の可用性 / ケイパビリティ**: COM 呼び出しには `packageQuery`
  ケイパビリティ（または Medium 以上の整合性レベル）が必要。App Installer 未導入や
  ポリシー無効時は活性化に失敗するため、`--source auto` で FS へ縮退する設計にする。
- **COM とエイリアス情報のギャップ**: COM API がファイル単位のエイリアス対応を
  返さない場合は正規表現ルールで補う（§3・§7）。COM だけに依存しない。
- **C++/WinRT の初期化**: `winrt::init_apartment()` の適切な呼び出しと、
  out-of-proc サーバー活性化失敗時の例外処理を確実に行う。
- **開発者モード / 権限**: symlink 作成は権限依存。
  `CreateSymbolicLinkW` に `SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE` を付与し、
  失敗時は原因（権限 or 開発者モード無効）を切り分けて案内する。
- **ロングパス**: `Packages` 配下は深くなりがち → `\\?\` プレフィックスと
  マニフェスト `longPathAware` を設定。
- **symlink 判定**: `GetFileAttributesW` の `FILE_ATTRIBUTE_REPARSE_POINT` と
  `DeviceIoControl(FSCTL_GET_REPARSE_POINT)` でターゲット解決。
  winget がコピー（=通常ファイル）で配置しているケースも `Mismatch` として扱う。
- **重複エイリアス**: 複数 exe が同一エイリアスに解決される場合は衝突として警告し、
  ユーザーに選択させる（自動作成しない）。
- **文字コード / 出力**: コンソールは `SetConsoleOutputCP(CP_UTF8)` or ワイド API で
  日本語パスを正しく表示。
- **既存リンクの尊重**: winget が正しく作った `Ok` リンクは変更しない。

## 11. 受け入れ基準 (Definition of Done)

- [ ] Windows 11 24H2 (x64/arm64) でビルド・実行できる。
- [ ] COM API 経由でインストール済み portable パッケージを列挙できる。
- [ ] COM 不可時に FS 走査へ自動フォールバックする（`--source auto`）。
- [ ] `scan` で欠落/破損/正常が正しく分類され一覧表示される。
- [ ] `fix` が確認プロンプト経由で欠落 symlink を作成できる。
- [ ] `--dry-run` が副作用なしで計画を出力する。
- [ ] 正規表現ルールで `codex-x86_64-pc-windows-msvc.exe → codex.exe` が導出される。
- [ ] `--tui` で対話的にチェックして一括作成できる。
- [ ] 開発者モード無効時に権限エラーを明示し、終了コード 2 を返す。
- [ ] `AliasResolver` / `RuleSet` / `LinkInspector` に MSTest の単体テストがある。
- [ ] **すべての単体テストがグリーンである** — `vstest.console.exe` が成功を報告すること。
      ビルドが通っただけでは証拠として不十分。
- [ ] **依存パッケージに既知の脆弱性がない**。すべての依存が MIT 互換であり、
      導入した PR で必要性が説明されている。
