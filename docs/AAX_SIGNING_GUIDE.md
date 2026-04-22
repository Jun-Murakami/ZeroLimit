# AAXプラグイン署名ガイド

## 概要
通常のPro Toolsで動作させるには、PACE Eden Toolsを使用してAAXプラグインに署名する必要があります。

## 必要なもの

1. **Avid Developer Account**（無料）
2. **PACE Developer Account**（Avid経由で自動的に無料アップグレード）
3. **iLok USB**（物理デバイス、約$50）
4. **PACE Eden Tools**（Windows/macOS）
5. **AAX SDK**（既に取得済み）

## セットアップ手順

### 1. Avid Developer Accountの作成

1. https://developer.avid.com にアクセス
2. "Join Developer Program"から登録（無料）
3. アカウント承認を待つ（通常1-2営業日）

### 2. PACE Centralのセットアップ

1. https://www.ilok.com からPACE Centralをダウンロード
2. iLok.comアカウントを作成
3. PACE Centralをインストール
4. iLok USBを接続

### 3. PACE Developer Portalへのアクセス

1. Avidから承認メールが届いたら、PACEからも開発者アクセスの招待メールが届く
2. https://developer.pace.com でログイン
3. Developer Subscription（通常$500/年）が無料でアクティベートされていることを確認

### 4. Eden Toolsのダウンロードとセットアップ

```bash
# PACE Developer Portalから最新版をダウンロード
# Windows: eden_tools_windows_x.x.x.zip
# macOS: eden_tools_macos_x.x.x.dmg
```

#### Windows
```powershell
# Eden Toolsを解凍（例: C:\PACE\EdenTools）
cd C:\PACE\EdenTools

# 環境変数を設定
[Environment]::SetEnvironmentVariable("PACE_EDEN_TOOLS", "C:\PACE\EdenTools", "User")
$env:PATH += ";C:\PACE\EdenTools\bin"
```

#### macOS
```bash
# Eden Toolsをインストール（通常 /usr/local/pace/eden）
export PACE_EDEN_TOOLS="/usr/local/pace/eden"
export PATH="$PATH:$PACE_EDEN_TOOLS/bin"

# ~/.zshrc or ~/.bash_profileに追加
echo 'export PACE_EDEN_TOOLS="/usr/local/pace/eden"' >> ~/.zshrc
echo 'export PATH="$PATH:$PACE_EDEN_TOOLS/bin"' >> ~/.zshrc
```

### 5. 署名用証明書の作成

```bash
# iLok USBが接続されていることを確認
# PACE Developer Portalで証明書を生成

# 1. ログイン
pace_eden login --account YOUR_ILOK_ACCOUNT

# 2. 証明書の生成（初回のみ）
pace_eden certificate create --type developer
```

## 署名プロセス

### Windows用スクリプト (sign_aax_windows.ps1)

```powershell
# AAX署名スクリプト - Windows
param(
    [Parameter(Mandatory=$true)]
    [string]$AAXPath,
    
    [string]$OutputPath = "",
    
    [string]$WrapGuid = "YOUR_WRAP_GUID_HERE"  # PACE Portalで生成
)

# Eden Toolsのパスを確認
$EdenPath = $env:PACE_EDEN_TOOLS
if (-not $EdenPath) {
    Write-Error "PACE_EDEN_TOOLS environment variable not set"
    exit 1
}

$WrapTool = "$EdenPath\bin\wraptool.exe"
if (-not (Test-Path $WrapTool)) {
    Write-Error "wraptool.exe not found at: $WrapTool"
    exit 1
}

# 出力パスの設定
if ([string]::IsNullOrEmpty($OutputPath)) {
    $OutputPath = [System.IO.Path]::GetDirectoryName($AAXPath)
    $OutputPath = Join-Path $OutputPath ([System.IO.Path]::GetFileNameWithoutExtension($AAXPath) + "_signed.aaxplugin")
}

Write-Host "Signing AAX Plugin..." -ForegroundColor Green
Write-Host "Input: $AAXPath"
Write-Host "Output: $OutputPath"

# 署名実行
& $WrapTool sign `
    --verbose `
    --account $env:ILOK_ACCOUNT `
    --wcguid $WrapGuid `
    --signid "Developer" `
    --in "$AAXPath" `
    --out "$OutputPath"

if ($LASTEXITCODE -eq 0) {
    Write-Host "Successfully signed AAX plugin!" -ForegroundColor Green
    Write-Host "Signed plugin: $OutputPath"
} else {
    Write-Error "Failed to sign AAX plugin. Error code: $LASTEXITCODE"
    exit 1
}
```

### macOS用スクリプト (sign_aax_macos.sh)

```bash
#!/bin/bash

# AAX署名スクリプト - macOS

# 色設定
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# 引数チェック
if [ $# -lt 1 ]; then
    echo -e "${RED}Usage: $0 <path_to_aax_plugin> [output_path]${NC}"
    exit 1
fi

AAX_PATH="$1"
OUTPUT_PATH="${2:-}"
WRAP_GUID="YOUR_WRAP_GUID_HERE"  # PACE Portalで生成

# Eden Toolsパス確認
if [ -z "$PACE_EDEN_TOOLS" ]; then
    echo -e "${RED}Error: PACE_EDEN_TOOLS environment variable not set${NC}"
    exit 1
fi

WRAP_TOOL="$PACE_EDEN_TOOLS/bin/wraptool"
if [ ! -f "$WRAP_TOOL" ]; then
    echo -e "${RED}Error: wraptool not found at: $WRAP_TOOL${NC}"
    exit 1
fi

# 出力パス設定
if [ -z "$OUTPUT_PATH" ]; then
    DIR=$(dirname "$AAX_PATH")
    BASE=$(basename "$AAX_PATH" .aaxplugin)
    OUTPUT_PATH="$DIR/${BASE}_signed.aaxplugin"
fi

echo -e "${GREEN}Signing AAX Plugin...${NC}"
echo "Input: $AAX_PATH"
echo "Output: $OUTPUT_PATH"

# 署名実行
$WRAP_TOOL sign \
    --verbose \
    --account "$ILOK_ACCOUNT" \
    --wcguid "$WRAP_GUID" \
    --signid "Developer" \
    --in "$AAX_PATH" \
    --out "$OUTPUT_PATH"

if [ $? -eq 0 ]; then
    echo -e "${GREEN}Successfully signed AAX plugin!${NC}"
    echo "Signed plugin: $OUTPUT_PATH"
else
    echo -e "${RED}Failed to sign AAX plugin${NC}"
    exit 1
fi
```

## ビルドスクリプトへの統合

### build_windows_release.ps1への追加

```powershell
# AAX署名（オプション）
if ($BuildAAX -and $env:PACE_EDEN_TOOLS) {
    Write-Step "Signing AAX plugin..."
    $SignScript = "$RootDir\scripts\sign_aax_windows.ps1"
    if (Test-Path $SignScript) {
        & $SignScript -AAXPath "$SourceAAX" -OutputPath "$DestAAX"
        if ($LASTEXITCODE -eq 0) {
            Write-Success "AAX plugin signed successfully"
        } else {
            Write-Warning "AAX signing failed - distributing unsigned version"
        }
    }
}
```

## WRAP GUIDの取得

1. PACE Developer Portalにログイン
2. "Products" → "Create New Product"
3. 製品情報を入力
4. 生成されたWRAP GUIDをメモ

## 署名の確認

```bash
# 署名情報の確認
wraptool verify --in "ZeroLimit_signed.aaxplugin"

# 出力例:
# Plugin is signed
# Signer: Your Company Name
# WRAP GUID: xxxx-xxxx-xxxx-xxxx
```

## トラブルシューティング

### よくあるエラーと対処法

1. **"No valid signing certificate found"**
   - iLok USBが接続されているか確認
   - PACE Centralで証明書が有効か確認
   - `pace_eden certificate list`で証明書を確認

2. **"Invalid WRAP GUID"**
   - PACE Developer PortalでWRAP GUIDを再確認
   - 製品が正しく登録されているか確認

3. **"Failed to sign: -2147024891"**
   - iLokライセンスマネージャーが実行中か確認
   - iLok USBを抜き差しして再試行

4. **Pro Toolsで読み込めない**
   - 署名が正しく完了しているか`wraptool verify`で確認
   - Pro Toolsのプラグインフォルダが正しいか確認
   - Pro Toolsを再起動してプラグインを再スキャン

## 自動化のベストプラクティス

1. **環境変数の設定**
   ```bash
   # .env.local（gitignore済み）
   ILOK_ACCOUNT=your_account
   WRAP_GUID=xxxx-xxxx-xxxx-xxxx
   PACE_EDEN_TOOLS=C:\PACE\EdenTools
   ```

2. **CI/CDパイプライン**
   - GitHub ActionsやAzure DevOpsで署名を自動化
   - iLok USBの代わりにクラウドベースの署名サービスを検討

3. **バッチ署名**
   - 複数のフォーマット（VST3、AAX）を一度に署名
   - バージョン管理と署名履歴の記録

## 重要な注意事項

- **署名には時間がかかる**: 初回は特に時間がかかることがある（5-10分）
- **インターネット接続必須**: 署名プロセス中はオンラインである必要がある
- **iLok USBは安全に保管**: 紛失すると証明書の再発行が必要
- **テスト署名と本番署名を分離**: 開発用と配布用で異なるWRAP GUIDを使用することを推奨

## 参考リンク

- [PACE Developer Portal](https://developer.pace.com)
- [Avid AAX Developer](https://developer.avid.com/aax/)
- [iLok License Manager](https://www.ilok.com)
- [AAX SDK Documentation](http://developer.avid.com/aax/documentation)