# syncwingetlink へのコントリビュート

syncwingetlink に関心を持っていただきありがとうございます。このドキュメントでは開発の
進め方とルールをまとめます。

📖 English version: [`CONTRIBUTING.md`](./CONTRIBUTING.md)（こちらが正典です）

> このファイルは翻訳です。内容が英語版と食い違う場合は英語版が優先されます。

## 始める前に

- 設計: [`docs/PLAN.md`](./docs/PLAN.md)
- 作業分解（TODO）: [`docs/TODO.md`](./docs/TODO.md)
- アーキテクチャ上の意思決定: [`docs/adr.md`](./docs/adr.md)
- AI コーディングエージェント向けガイド: [`AGENTS.md`](./AGENTS.md)

作業を始める前に上記に目を通してください。

## コントリビュートの種類

- 🐛 バグ報告（Issue）
- 💡 機能提案（Issue / Discussions）
- 🔧 コードのコントリビュート（Pull Request）
- 📝 ドキュメントの改善
- 🌐 翻訳 / エイリアス置換ルールの追加

## Issue の起票

バグを報告する際は次の情報を含めてください。

- 再現手順
- 期待する動作と実際の動作
- 環境（`winget --info` の出力、Windows のバージョン、アーキテクチャ）
- 関連するログ / エラーメッセージ

## 開発環境

### 必要なもの

- Windows 11 24H2（ビルド 26100）以降
- **Visual Studio 2026**、「C++ によるデスクトップ開発」ワークロード
  （C++/WinRT および **C++ のテスト関連コンポーネント**を含む）
  - プラットフォームツールセット **v145** が必須です。Visual Studio 2022 が提供するのは
    v143 のため、このソリューションはビルドできません。
  - ARM64 構成をビルドする場合は ARM64 ビルドツールも導入してください。
- Windows SDK 10.0.26100.0
- 開発者モードの有効化（シンボリックリンク作成のテストに必要）

CMake は使用しません。理由は [`docs/adr.md`](./docs/adr.md) の ADR-0001 を参照してください。

### ビルドとテスト

`msbuild` と `vstest.console` に PATH が通るよう、**Developer PowerShell for VS 2026**
から実行してください。

```powershell
# ビルド（x64 Debug）
msbuild syncwingetlink.sln -p:Configuration=Debug -p:Platform=x64 -m

# テスト
vstest.console.exe build\x64\Debug\syncwingetlink.tests.dll /Platform:x64

# 副作用なしで実行
.\build\x64\Debug\syncwingetlink.exe scan
```

単体テストには **MSTest**（Microsoft Unit Testing Framework for C++、`CppUnitTest.h`）を
使用します。Visual Studio のテストエクスプローラーにも表示されるため、IDE から実行・
デバッグできます。

## コーディング規約

- **標準**: C++20。可能な限り STL / `std::filesystem` を優先します。
- **エンコーディング**: 内部は UTF-16（`std::wstring`）、長いパスは `\\?\` で対応します。
- **命名**: 型は `PascalCase`、関数・変数は `camelCase`、定数は `kPascalCase`、
  メンバー変数は `m_` プレフィックスを付けます。
- **フォーマット**: リポジトリの `.clang-format` に従います。`.clang-tidy` の警告は
  原則として解消してください（命名規則はここで強制されます）。
- **プロジェクトファイル**: ソースファイルを追加・削除したら `.vcxproj` と
  `.vcxproj.filters` の両方を更新してください。新しいロジックは実行可能プロジェクト
  ではなく `syncwingetlink.core` 静的ライブラリに置きます（単体テスト可能にするため）。
- **依存関係**: 追加は最小限にとどめ、PR で理由を説明し MIT 互換であることを確認します。
- **副作用**: `scan` は読み取り専用を維持します。破壊的な操作は `--dry-run` を
  尊重しなければなりません。

詳細な設計上の制約は [`AGENTS.md`](./AGENTS.md) の「Key design constraints」を参照して
ください。

## Pull Request の流れ

1. リポジトリをフォークしてブランチを作成します
   - `feature/<topic>` / `fix/<topic>` / `docs/<topic>`
2. 変更を実装し、対応するテストを追加します
   - `[core]` のロジック（`AliasResolver` / `RuleSet` / `LinkInspector` /
     パッケージ列挙の切り替え）には単体テストが必須です
3. ローカルでビルドとテストを行います
4. コミットメッセージは [Conventional Commits](https://www.conventionalcommits.org/)
   に従います
   - 例: `feat(core): add WingetComSource for installed package enumeration`
5. PR を作成し、次の内容を含めます
   - `docs/TODO.md` の該当項目
   - 変更内容と設計判断の要約
   - テストした内容と結果（未検証の点は明記）
   - 破壊的変更の有無

### PR のサイズ

レビューしやすいよう、PR は小さく保ってください。大きな機能は複数の PR に分割します。

## AI コーディングエージェントの利用

GitHub Copilot / Codex / Claude を使う場合は、必ず [`AGENTS.md`](./AGENTS.md) を
エージェントに読み込ませ、C++ に触れる前に **`cpp-msbuild` スキル**を読み込ませて
ください。

| ツール | パス |
|---|---|
| GitHub Copilot | `.github/skills/cpp-msbuild/SKILL.md`（**正典**） |
| Claude | `.claude/skills/cpp-msbuild/SKILL.md` |
| Codex | `.codex/skills/cpp-msbuild/SKILL.md` |

git で追跡されるのは `.github/skills/` のみです。後者 2 つは**ローカルで生成され
gitignore される**ミラーなので、クローン後に一度スクリプトを実行してください。

```powershell
./tools/Sync-Skills.ps1      # または: tools/sync-skills.sh
```

**編集は正典のみ**に行い、再度スクリプトを実行します。`--check` / `-Check` を付けると
書き込まずにミラーの同期状態だけを検証できます。

特に次の点に注意します。

- スコープを勝手に広げない（`docs/PLAN.md` の non-goals を参照）
- 他リポジトリからのコピーではなくオリジナルのコードを書く（ライセンス混入の回避）
- 生成されたコードが MIT 互換であることを確認する
- `*_ja.md` はエージェントの入力に使わない（翻訳にすぎないため）

## ライセンスと著作権

- 本プロジェクトは [MIT License](./LICENSE) で公開されています。
- コントリビュートされたコードは MIT License で提供されたものとみなされます。
- コピーレフト（GPL など）のコードを取り込まないでください。
- シークレット（トークン、個人のパス、内部情報）をコミットしないでください。

## 行動規範

誰もが協力できるよう、建設的かつ敬意をもってコミュニケーションしてください。

---

質問があれば気軽に Issue や Discussion を立ててください。コントリビュートありがとう
ございます！🙌
