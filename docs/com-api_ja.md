# winget COM API の利用（日本語）

> **これは翻訳です。** 正式版は英語の [`com-api.md`](./com-api.md) です。差異がある場合は英語版が優先されます。
> **AI エージェントはこのファイルを読み込まないでください（翻訳のみ）。**

syncwingetlink はインストール済みポータブルパッケージの列挙に、
winget の **COM API (`Microsoft.Management.Deployment` 名前空間)** を第一手段として使用します。
利用できない場合はファイルシステム走査 (`FsScanSource`) に自動フォールバックします。

## なぜ COM API か

- CLI とは別に提供される、**安定・バージョン管理された公開インターフェース**である。
- C++/WinRT から Win32 デスクトップアプリとして利用でき、out-of-proc / in-proc 両対応。
- sqlite (`PortableIndex`) の内部スキーマは非公開で将来変更のリスクがあるため、
  直接読み取りは避け、最終フォールバックに留める。

## 取得フロー

```
PackageManager
   └─ GetLocalPackageCatalog(LocalPackageCatalog.InstalledPackages)
        └─ PackageCatalogReference.Connect()  →  PackageCatalog
             └─ FindPackages(FindPackagesOptions)  →  [CatalogPackage]
                  └─ CatalogPackage.InstalledVersion (PackageVersionInfo)
                        →  Id / Name / Version / インストール場所 / (可能なら) エイリアス
```

1. `PackageManager` を生成（ファクトリ経由の COM 活性化）。
2. `GetLocalPackageCatalog(LocalPackageCatalog.InstalledPackages)` で
   インストール済みカタログ参照を取得。
   必要に応じ `CreateCompositePackageCatalog` + `CompositeSearchBehavior.LocalCatalogs`。
3. `PackageCatalogReference.Connect()` で `PackageCatalog` を取得。
4. `FindPackages()` で `CatalogPackage` を列挙。
5. `CatalogPackage.InstalledVersion` から識別子・バージョン・
   **インストール場所などのメタデータ**を取得。
6. installer type が **portable** のパッケージに絞り込む。

## C++/WinRT スケルトン（参考）

> 実際の型名・メソッド名は winget のバージョンに依存します。ビルド時に
> `Microsoft.Management.Deployment` の projection を参照してください。

```cpp
#include <winrt/Microsoft.Management.Deployment.h>

using namespace winrt;
using namespace winrt::Microsoft::Management::Deployment;

void EnumerateInstalledPortables()
{
    winrt::init_apartment();

    // ファクトリ経由で PackageManager を活性化
    PackageManager manager{};

    // インストール済みローカルカタログ
    auto catalogRef = manager.GetLocalPackageCatalog(LocalPackageCatalog::InstalledPackages);
    auto connectResult = catalogRef.Connect();
    if (connectResult.Status() != ConnectResultStatus::Ok)
    {
        // フォールバック（FsScanSource）へ
        return;
    }
    auto catalog = connectResult.PackageCatalog();

    // すべてのインストール済みパッケージを検索
    FindPackagesOptions options{};
    auto result = catalog.FindPackages(options);

    for (auto const& match : result.Matches())
    {
        auto pkg = match.CatalogPackage();
        auto installed = pkg.InstalledVersion();
        if (!installed) continue;

        // Id / Name / Version / インストール場所などを取得し、
        // installer type == portable のものを対象にする。
        // ...
    }
}
```

## ケイパビリティ / 権限

- COM 呼び出しには `packageQuery` ケイパビリティ、または Medium 以上の
  整合性レベルが必要（環境により異なる）。
- App Installer 未導入、winget ポリシー無効などの場合は COM 活性化に失敗する。
  その際は `--source auto` により `FsScanSource` に自動縮退する。

## フォールバックの挙動（`--source`）

| 値 | 挙動 |
|---|---|
| `com` | COM API のみ使用。失敗時はエラー終了。 |
| `fs` | ファイルシステム走査のみ使用（COM を試さない）。 |
| `auto`（既定） | COM を試し、失敗したら FS 走査へ縮退。 |

## 既知の注意点

- COM API は識別子・バージョン・インストール場所は取得できるが、
  **ファイル単位の symlink エイリアス対応まで完全に返すとは限らない**。
  その場合はエイリアスを `docs/rules.md` の正規表現ルールで補う。
- ローカルカタログの列挙挙動にはバージョン依存のクセが報告されている。
  そのため `auto` による FS フォールバックは実運用上ほぼ必須。
- C++/WinRT では `winrt::init_apartment()` の呼び出しと、
  活性化失敗時の例外処理を確実に行うこと。
