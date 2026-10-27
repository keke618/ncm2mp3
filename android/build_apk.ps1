# Build NCM2MP3.apk without Gradle, using SDK build-tools + JDK.
# All messages ASCII to avoid codepage issues.
$ErrorActionPreference = 'Stop'

$PROJ = 'C:\Users\AMD-XXX-01\Documents\deepseek-harness\default-workspace\Ncm2Mp3Android'
$SDK  = "$env:LOCALAPPDATA\Android\Sdk"
$PLAT = Join-Path $SDK 'platforms\android-37.0\android.jar'
$BT   = Join-Path $SDK 'build-tools\36.0.0'
$JDK  = 'D:\student\downloads\Android studio\jbr'

$AAPT2     = Join-Path $BT 'aapt2.exe'
$D8        = Join-Path $BT 'd8.bat'
$ZIPALIGN  = Join-Path $BT 'zipalign.exe'
$APKSIGNER = Join-Path $BT 'apksigner.bat'
$JAVAC     = Join-Path $JDK 'bin\javac.exe'
$JAR       = Join-Path $JDK 'bin\jar.exe'
$KEYTOOL   = Join-Path $JDK 'bin\keytool.exe'

$env:JAVA_HOME = $JDK

$BUILD = Join-Path $PROJ 'build'
if (Test-Path $BUILD) { Remove-Item $BUILD -Recurse -Force }
New-Item -ItemType Directory -Force -Path $BUILD, "$BUILD\gen", "$BUILD\classes", "$BUILD\dex" | Out-Null

Write-Host '[1/7] aapt2 compile'
& $AAPT2 compile --dir "$PROJ\res" -o "$BUILD\res.zip"
if ($LASTEXITCODE -ne 0) { throw 'aapt2 compile failed' }

Write-Host '[2/7] aapt2 link'
& $AAPT2 link -o "$BUILD\base.apk" -I $PLAT --manifest "$PROJ\AndroidManifest.xml" `
    -R "$BUILD\res.zip" --java "$BUILD\gen" --auto-add-overlay `
    --min-sdk-version 24 --target-sdk-version 36 `
    --version-code 1 --version-name 1.0
if ($LASTEXITCODE -ne 0) { throw 'aapt2 link failed' }

Write-Host '[3/7] javac'
$sources = @()
$sources += Get-ChildItem "$BUILD\gen" -Recurse -Filter '*.java' | ForEach-Object { $_.FullName }
$sources += Get-ChildItem "$PROJ\src" -Recurse -Filter '*.java' | ForEach-Object { $_.FullName }
& $JAVAC -nowarn -encoding UTF-8 -source 8 -target 8 -bootclasspath $PLAT `
    -d "$BUILD\classes" $sources
if ($LASTEXITCODE -ne 0) { throw 'javac failed' }

Write-Host '[4/7] d8'
$classes = Get-ChildItem "$BUILD\classes" -Recurse -Filter '*.class' | ForEach-Object { $_.FullName }
& $D8 --lib $PLAT --min-api 24 --output "$BUILD\dex" $classes
if ($LASTEXITCODE -ne 0) { throw 'd8 failed' }

Write-Host '[5/7] package'
Copy-Item "$BUILD\base.apk" "$BUILD\unsigned.apk" -Force
Push-Location "$BUILD\dex"
& $JAR uf "$BUILD\unsigned.apk" classes.dex
Pop-Location
if ($LASTEXITCODE -ne 0) { throw 'jar failed' }

& $ZIPALIGN -f 4 "$BUILD\unsigned.apk" "$BUILD\aligned.apk"
if ($LASTEXITCODE -ne 0) { throw 'zipalign failed' }

Write-Host '[6/7] keystore'
$ks = Join-Path $PROJ 'debug.keystore'
if (-not (Test-Path $ks)) {
    $old_ea = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $KEYTOOL -genkeypair -keystore $ks -storepass android -keypass android `
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 `
        -dname "CN=Android Debug,O=Android,C=US" *> $null
    $ErrorActionPreference = $old_ea
    Write-Host ("  keystore created: " + (Test-Path $ks))
}

Write-Host '[7/7] sign'
& $APKSIGNER sign --ks $ks --ks-pass pass:android --key-pass pass:android `
    --out "$PROJ\NCM2MP3.apk" "$BUILD\aligned.apk"
if ($LASTEXITCODE -ne 0) { throw 'apksigner failed' }

& $APKSIGNER verify --print-certs "$PROJ\NCM2MP3.apk"

$apk = Get-Item "$PROJ\NCM2MP3.apk"
Write-Host ("DONE  {0}  {1:N2} MB" -f $apk.Name, ($apk.Length / 1MB))
