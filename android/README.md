# Android 版

手机 / 平板上的 NCM 转 MP3，界面和 Windows 版一致，解密逻辑用 Java 重写，输出结果与 Python / C 版逐字节相同。

## 环境要求

| 项目 | 版本 |
|---|---|
| Android 系统 | 7.0 及以上（API 24） |
| 编译目标 | API 36 |
| JDK | 17 以上（Android Studio 自带 JBR 即可） |
| SDK | build-tools 36+、platform android-36+ |

## 编译

不需要 Gradle，直接调用 SDK 里的工具链：

```powershell
powershell -File build_apk.ps1
```

脚本里改一下这三个路径即可：

```powershell
$SDK  = "$env:LOCALAPPDATA\Android\Sdk"
$PLAT = "$SDK\platforms\android-37.0\android.jar"
$BT   = "$SDK\build-tools\36.0.0"
$JDK  = "D:\student\downloads\Android studio\jbr"
```

编译流程：

```
aapt2 compile   资源 -> res.zip
aapt2 link      资源 + 清单 -> base.apk，同时生成 R.java
javac           源码 + R.java -> .class
d8              .class -> classes.dex
jar             把 dex 塞进 apk
zipalign        4 字节对齐
apksigner       用调试证书签名
```

## 在 Android Studio 里开发

把 `android/` 当作工程根目录打开会缺少 Gradle 配置。想要 IDE 里跑，可以：

1. Android Studio → New Project → Empty Views Activity
2. 把 `src/com/keke/ncm2mp3/MainActivity.java` 复制进新工程
3. 把 `AndroidManifest.xml` 里的权限和 Activity 配置合并过去
4. 把 `res/mipmap-*/ic_launcher.png` 替换成自己的图标

## 权限说明

| 权限 | 用途 |
|---|---|
| `MANAGE_EXTERNAL_STORAGE` | 全盘扫描 `.ncm`（Android 11+ 需在设置里手动开启） |
| `READ_EXTERNAL_STORAGE` | Android 12 及以下的读取 |
| `WRITE_EXTERNAL_STORAGE` | Android 10 及以下的写入 |

不给「所有文件访问」也能用：点「或选择指定文件夹」，用系统文件选择器挑目录即可。

## 说明

- 解密全部在本地完成，不联网
- 扫描时跳过 `Android/`、`.thumbnails/` 和隐藏目录，最多收录 800 个文件
- 列表最多渲染 200 行，超出的显示「还有 N 个文件未显示」
- 原地生成时直接写回源文件所在目录；导出时通过 SAF 写入目标目录
