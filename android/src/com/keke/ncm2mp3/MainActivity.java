package com.keke.ncm2mp3;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.database.Cursor;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

import javax.crypto.Cipher;
import javax.crypto.spec.SecretKeySpec;

/**
 * NCM 转 MP3 —— 手机 / 平板版
 */
public class MainActivity extends Activity {

    private static final int REQ_SRC = 1001;
    private static final int REQ_OUT = 1002;

    private static final int BG        = 0xFFF4F5F7;
    private static final int CARD      = 0xFFFFFFFF;
    private static final int TEXT      = 0xFF1F2329;
    private static final int MUTED     = 0xFF8B93A1;
    private static final int ACCENT    = 0xFFE8474B;
    private static final int OK        = 0xFF22A06B;
    private static final int ERR       = 0xFFD93026;
    private static final int LINE      = 0xFFE9EBEE;

    private static final byte[] NCM_KEY =
            "hzHRAmso5kInbaxW".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] PREFIX =
            "neteasecloudmusic".getBytes(StandardCharsets.US_ASCII);

    private final Handler ui = new Handler(Looper.getMainLooper());

    private Uri srcTree;
    private Uri outTree;
    private boolean pendingScan = false;

    private boolean tablet;
    private int padH, gap;

    private TextView tvPath;
    private TextView tvStatus;
    private LinearLayout listBox;
    private Button btnScan;
    private Button btnPickSrc;
    private Button btnPickOut;
    private Button btnStart;
    private RadioGroup rgMode;
    private RadioButton rbInPlace;
    private RadioButton rbCustom;
    private CheckBox cbDelete;
    private ProgressBar progress;

    private final List<Entry> entries = new ArrayList<Entry>();
    private final List<TextView> rowStatus = new ArrayList<TextView>();
    private volatile boolean running = false;

    static class Entry {
        Uri uri;
        File file;
        String name;
        String where;
        Entry(Uri u, File f, String n, String w) {
            uri = u; file = f; name = n; where = w;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        computeMetrics();
        setContentView(buildUi());
        if (entries.isEmpty()) showEmptyHint();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (pendingScan && hasAllFiles()) {
            pendingScan = false;
            scanDevice();
        }
    }

    // ---------------------------------------------------------------- 尺寸
    private void computeMetrics() {
        int swDp = getResources().getConfiguration().smallestScreenWidthDp;
        tablet = swDp >= 600;
        int screenDp = getResources().getConfiguration().screenWidthDp;

        gap = dp(tablet ? 14 : 12);
        if (tablet) {
            int maxContent = 760;
            padH = dp(Math.max(24, (screenDp - maxContent) / 2));
        } else {
            padH = dp(16);
        }
    }

    private int dp(int v) {
        return Math.round(getResources().getDisplayMetrics().density * v);
    }

    // ---------------------------------------------------------------- 样式
    private GradientDrawable cardBg() {
        GradientDrawable d = new GradientDrawable();
        d.setColor(CARD);
        d.setCornerRadius(dp(tablet ? 16 : 14));
        return d;
    }

    private GradientDrawable accentBg(int color, int radiusDp) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(radiusDp));
        return d;
    }

    private GradientDrawable outlineBg() {
        GradientDrawable d = new GradientDrawable();
        d.setColor(CARD);
        d.setCornerRadius(dp(tablet ? 16 : 12));
        d.setStroke(dp(1), LINE);
        return d;
    }

    private TextView text(String s, float sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(t.getTypeface(), android.graphics.Typeface.BOLD);
        return t;
    }

    private LinearLayout card() {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setBackground(cardBg());
        int p = dp(tablet ? 18 : 14);
        c.setPadding(p, p, p, p);
        return c;
    }

    private Button flatButton(String label, boolean primary) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(tablet ? 16 : 15);
        if (primary) {
            b.setBackground(accentBg(ACCENT, 12));
            b.setTextColor(Color.WHITE);
            b.setTypeface(b.getTypeface(), android.graphics.Typeface.BOLD);
            b.setPadding(0, dp(14), 0, dp(14));
        } else {
            b.setBackground(outlineBg());
            b.setTextColor(TEXT);
            b.setPadding(0, dp(12), 0, dp(12));
        }
        return b;
    }

    // ---------------------------------------------------------------- 界面
    private View buildUi() {
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(BG);
        scroll.setFillViewport(true);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(padH, dp(16), padH, dp(28));
        scroll.addView(root, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        // ---- 标题 ----
        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.VERTICAL);
        head.setPadding(dp(4), dp(2), dp(4), gap);
        head.addView(text("NCM 转 MP3", tablet ? 28 : 24, TEXT, true));
        TextView sub = text("网易云音乐加密格式解密 · 全程本地处理", tablet ? 15 : 13, MUTED, false);
        sub.setPadding(0, dp(4), 0, 0);
        head.addView(sub);
        root.addView(head);

        // ---- 操作卡片 ----
        LinearLayout actions = card();
        btnScan = flatButton("扫描手机里的 NCM", true);
        btnScan.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { requestScan(); }
        });
        actions.addView(btnScan, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        btnPickSrc = flatButton("或选择指定文件夹", false);
        btnPickSrc.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { pickTree(REQ_SRC); }
        });
        LinearLayout.LayoutParams p2 = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        p2.topMargin = dp(10);
        actions.addView(btnPickSrc, p2);

        tvPath = text("未选择", tablet ? 13 : 12, MUTED, false);
        tvPath.setPadding(dp(2), dp(10), dp(2), 0);
        actions.addView(tvPath);
        root.addView(actions, lp(gap));

        // ---- 文件列表 ----
        LinearLayout listCard = card();
        LinearLayout listHead = new LinearLayout(this);
        listHead.setOrientation(LinearLayout.HORIZONTAL);
        listHead.addView(text("待转换文件", tablet ? 16 : 14, TEXT, true),
                new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        tvStatus = text("等待扫描", tablet ? 13 : 12, MUTED, false);
        tvStatus.setGravity(Gravity.END);
        listHead.addView(tvStatus);
        listCard.addView(listHead);

        listBox = new LinearLayout(this);
        listBox.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams lbp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lbp.topMargin = dp(6);
        listCard.addView(listBox, lbp);
        root.addView(listCard, lp(gap));

        // ---- 选项 ----
        LinearLayout opts = card();
        opts.addView(text("输出位置", tablet ? 15 : 13, MUTED, false));

        rgMode = new RadioGroup(this);
        rgMode.setOrientation(RadioGroup.HORIZONTAL);
        rbInPlace = new RadioButton(this);
        rbInPlace.setText("原地生成");
        rbInPlace.setId(View.generateViewId());
        rbInPlace.setChecked(true);
        rbInPlace.setTextSize(tablet ? 16 : 14);
        rbCustom = new RadioButton(this);
        rbCustom.setText("导出到其他文件夹");
        rbCustom.setId(View.generateViewId());
        rbCustom.setTextSize(tablet ? 16 : 14);
        rgMode.addView(rbInPlace);
        rgMode.addView(rbCustom);
        opts.addView(rgMode);

        btnPickOut = flatButton("选择输出文件夹", false);
        btnPickOut.setEnabled(false);
        btnPickOut.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { pickTree(REQ_OUT); }
        });
        opts.addView(btnPickOut);

        rgMode.setOnCheckedChangeListener(new RadioGroup.OnCheckedChangeListener() {
            public void onCheckedChanged(RadioGroup g, int id) {
                btnPickOut.setEnabled(rbCustom.isChecked());
                btnPickOut.setAlpha(rbCustom.isChecked() ? 1f : 0.45f);
            }
        });
        btnPickOut.setAlpha(0.45f);

        cbDelete = new CheckBox(this);
        cbDelete.setText("转换成功后删除原 .ncm 文件");
        cbDelete.setTextSize(tablet ? 15 : 13);
        cbDelete.setTextColor(TEXT);
        LinearLayout.LayoutParams cbp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        cbp.topMargin = dp(8);
        opts.addView(cbDelete, cbp);
        root.addView(opts, lp(gap));

        // ---- 进度 + 开始 ----
        LinearLayout run = card();
        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(100);
        progress.setProgress(0);
        run.addView(progress, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(10)));

        btnStart = flatButton("开始转换", true);
        btnStart.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { startConvert(); }
        });
        LinearLayout.LayoutParams sp2 = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        sp2.topMargin = dp(12);
        run.addView(btnStart, sp2);
        root.addView(run, lp(gap));

        return scroll;
    }

    private LinearLayout.LayoutParams lp(int topMargin) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        p.topMargin = topMargin;
        return p;
    }

    private void showEmptyHint() {
        listBox.removeAllViews();
        TextView t = text("还没有文件\n点上面的按钮扫描或选择文件夹", tablet ? 15 : 13, MUTED, false);
        t.setGravity(Gravity.CENTER);
        t.setPadding(0, dp(22), 0, dp(22));
        listBox.addView(t, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
    }

    // ---------------------------------------------------------------- 权限
    private boolean hasAllFiles() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return true;
        return Environment.isExternalStorageManager();
    }

    private void requestScan() {
        if (hasAllFiles()) { scanDevice(); return; }
        pendingScan = true;
        try {
            Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
            i.setData(Uri.parse("package:" + getPackageName()));
            startActivity(i);
        } catch (Exception e) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            } catch (Exception e2) {
                toast("请到系统设置手动授予「所有文件访问」权限");
                pendingScan = false;
            }
        }
    }

    // ---------------------------------------------------------------- 扫描
    private void scanDevice() {
        entries.clear();
        rowStatus.clear();
        listBox.removeAllViews();
        tvStatus.setText("扫描中…");
        btnScan.setEnabled(false);

        new Thread(new Runnable() {
            public void run() {
                final List<File> found = new ArrayList<File>();
                List<File> roots = new ArrayList<File>();
                File primary = Environment.getExternalStorageDirectory();
                if (primary != null) roots.add(primary);
                File[] vols = new File("/storage").listFiles();
                if (vols != null) {
                    for (File v : vols) {
                        if (v == null || !v.isDirectory()) continue;
                        String n = v.getName();
                        if (n.equals("emulated") || n.equals("self")) continue;
                        if (!roots.contains(v)) roots.add(v);
                    }
                }
                for (File r : roots) {
                    walk(r, found, 0);
                    if (found.size() > 800) break;
                }
                Collections.sort(found, new Comparator<File>() {
                    public int compare(File a, File b) {
                        return Long.compare(b.lastModified(), a.lastModified());
                    }
                });

                ui.post(new Runnable() {
                    public void run() {
                        btnScan.setEnabled(true);
                        for (File f : found) {
                            entries.add(new Entry(null, f, f.getName(), shortPath(f)));
                        }
                        renderList();
                        tvPath.setText("已扫描手机存储");
                        tvStatus.setText(found.isEmpty()
                                ? "没有找到 .ncm" : "共 " + found.size() + " 个");
                    }
                });
            }
        }).start();
    }

    private static void walk(File dir, List<File> out, int depth) {
        if (dir == null || depth > 12 || out.size() > 800) return;
        String lower = dir.getName().toLowerCase();
        if (lower.equals("android") || lower.equals("thumbnails") || lower.startsWith(".")) return;
        File[] list = dir.listFiles();
        if (list == null) return;
        for (File f : list) {
            if (f == null) continue;
            if (f.isDirectory()) walk(f, out, depth + 1);
            else if (f.getName().toLowerCase().endsWith(".ncm")) out.add(f);
            if (out.size() > 800) return;
        }
    }

    private static String shortPath(File f) {
        String p = f.getParent();
        if (p == null) return "";
        String s = p.replace("/storage/emulated/0", "内部存储");
        if (s.length() > 52) s = "…" + s.substring(s.length() - 51);
        return s;
    }

    // ---------------------------------------------------------------- 选目录
    private void pickTree(int req) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        try {
            startActivityForResult(i, req);
        } catch (Exception e) {
            toast("无法打开文件选择器：" + e.getMessage());
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        try {
            getContentResolver().takePersistableUriPermission(uri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        } catch (Exception ignored) { }

        if (requestCode == REQ_SRC) {
            srcTree = uri;
            scanFolder();
        } else if (requestCode == REQ_OUT) {
            outTree = uri;
            tvStatus.setText("输出目录已选");
        }
    }

    private void scanFolder() {
        entries.clear();
        rowStatus.clear();
        String rootId = DocumentsContract.getTreeDocumentId(srcTree);
        tvPath.setText("来源：" + rootId);
        try {
            Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(srcTree, rootId);
            Cursor c = getContentResolver().query(children, new String[]{
                    DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                    DocumentsContract.Document.COLUMN_DISPLAY_NAME
            }, null, null, null);
            if (c != null) {
                while (c.moveToNext()) {
                    String id = c.getString(0);
                    String name = c.getString(1);
                    if (name != null && name.toLowerCase().endsWith(".ncm")) {
                        entries.add(new Entry(
                                DocumentsContract.buildDocumentUriUsingTree(srcTree, id),
                                null, name, rootId));
                    }
                }
                c.close();
            }
        } catch (Exception e) {
            toast("读取文件夹失败：" + e.getMessage());
        }
        renderList();
    }

    // ---------------------------------------------------------------- 渲染
    private void renderList() {
        listBox.removeAllViews();
        rowStatus.clear();

        if (entries.isEmpty()) {
            showEmptyHint();
            return;
        }

        int max = Math.min(entries.size(), 200);
        for (int i = 0; i < max; i++) {
            Entry e = entries.get(i);

            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setPadding(0, dp(10), 0, dp(10));

            LinearLayout col = new LinearLayout(this);
            col.setOrientation(LinearLayout.VERTICAL);
            col.addView(text(e.name, tablet ? 15 : 14, TEXT, false));
            TextView w = text(e.where, tablet ? 12 : 11, MUTED, false);
            w.setPadding(0, dp(2), 0, 0);
            col.addView(w);

            row.addView(col, new LinearLayout.LayoutParams(
                    0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

            TextView st = text("", tablet ? 15 : 14, MUTED, true);
            st.setGravity(Gravity.CENTER_VERTICAL);
            st.setPadding(dp(8), 0, 0, 0);
            row.addView(st);
            rowStatus.add(st);

            listBox.addView(row, new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

            if (i < max - 1) {
                View line = new View(this);
                line.setBackgroundColor(LINE);
                LinearLayout.LayoutParams llp = new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, dp(1));
                listBox.addView(line, llp);
            }
        }

        if (entries.size() > max) {
            TextView more = text("还有 " + (entries.size() - max) + " 个文件未显示", 12, MUTED, false);
            more.setPadding(0, dp(10), 0, dp(4));
            listBox.addView(more);
        }

        tvStatus.setText("共 " + entries.size() + " 个");
    }

    // ---------------------------------------------------------------- 转换
    private void startConvert() {
        if (running) return;
        if (entries.isEmpty()) {
            toast("先扫描或选择一个包含 .ncm 的文件夹");
            return;
        }
        if (rbCustom.isChecked() && outTree == null) {
            toast("请先选择输出文件夹");
            return;
        }

        running = true;
        btnStart.setEnabled(false);
        btnScan.setEnabled(false);
        btnPickSrc.setEnabled(false);
        progress.setProgress(0);
        for (TextView t : rowStatus) t.setText("");
        tvStatus.setText("转换中…");

        final List<Entry> list = new ArrayList<Entry>(entries);
        final boolean custom = rbCustom.isChecked();
        final boolean del = cbDelete.isChecked();

        new Thread(new Runnable() {
            public void run() {
                int ok = 0, fail = 0;
                for (int i = 0; i < list.size(); i++) {
                    boolean flag;
                    try {
                        convertOne(list.get(i), custom, del);
                        ok++;
                        flag = true;
                    } catch (Exception ex) {
                        fail++;
                        flag = false;
                    }
                    final boolean good = flag;
                    final int idx = i;
                    final int pct = (int) ((i + 1) * 100.0 / list.size());
                    ui.post(new Runnable() {
                        public void run() {
                            if (idx < rowStatus.size()) {
                                rowStatus.get(idx).setText(good ? "✓" : "✗");
                                rowStatus.get(idx).setTextColor(good ? OK : ERR);
                            }
                            progress.setProgress(pct);
                        }
                    });
                }
                final int fok = ok, ffail = fail;
                ui.post(new Runnable() {
                    public void run() {
                        running = false;
                        btnStart.setEnabled(true);
                        btnScan.setEnabled(true);
                        btnPickSrc.setEnabled(true);
                        tvStatus.setText("成功 " + fok + " · 失败 " + ffail);
                        toast("转换完成：成功 " + fok + " 个" + (ffail > 0 ? "，失败 " + ffail : ""));
                    }
                });
            }
        }).start();
    }

    private byte[] readAll(Entry e) throws IOException {
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        InputStream in = (e.file != null)
                ? new FileInputStream(e.file)
                : getContentResolver().openInputStream(e.uri);
        if (in == null) throw new IOException("打不开文件");
        try {
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) bos.write(buf, 0, n);
        } finally {
            in.close();
        }
        return bos.toByteArray();
    }

    private void convertOne(Entry e, boolean custom, boolean del) throws Exception {
        Decrypted d = decrypt(readAll(e));

        String base = e.name;
        int dot = base.lastIndexOf('.');
        if (dot > 0) base = base.substring(0, dot);
        String outName = base + d.ext;

        if (e.file != null && !custom) {
            File out = new File(e.file.getParentFile(), outName);
            int n = 2;
            while (out.exists()) {
                out = new File(e.file.getParentFile(), base + " (" + (n++) + ")" + d.ext);
            }
            FileOutputStream fos = new FileOutputStream(out);
            try { fos.write(d.audio); } finally { fos.close(); }
        } else {
            Uri parent = custom ? treeRoot(outTree) : treeRoot(srcTree);
            String mime = d.ext.equals(".flac") ? "audio/flac" : "audio/mpeg";
            Uri newDoc = DocumentsContract.createDocument(
                    getContentResolver(), parent, mime, outName);
            if (newDoc == null) throw new IOException("无法创建输出文件");
            OutputStream os = getContentResolver().openOutputStream(newDoc);
            if (os == null) throw new IOException("无法写入输出文件");
            try {
                os.write(d.audio);
                os.flush();
            } finally {
                os.close();
            }
        }

        if (del) {
            try {
                if (e.file != null) e.file.delete();
                else DocumentsContract.deleteDocument(getContentResolver(), e.uri);
            } catch (Exception ignored) { }
        }
    }

    private Uri treeRoot(Uri tree) {
        return DocumentsContract.buildDocumentUriUsingTree(
                tree, DocumentsContract.getTreeDocumentId(tree));
    }

    private void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_SHORT).show();
    }

    // ---------------------------------------------------------------- 解密
    static class Decrypted {
        byte[] audio;
        String ext;
    }

    static Decrypted decrypt(byte[] raw) throws Exception {
        if (raw.length < 32) throw new IOException("文件太小");
        if (!"CTENFDAM".equals(new String(raw, 0, 8, StandardCharsets.US_ASCII)))
            throw new IOException("不是 ncm 文件");

        int pos = 10;
        int keyLen = rdU32(raw, pos); pos += 4;
        if (keyLen <= 0 || keyLen > 0x10000 || pos + keyLen > raw.length)
            throw new IOException("密钥长度异常");

        byte[] kdata = new byte[keyLen];
        for (int i = 0; i < keyLen; i++) kdata[i] = (byte) (raw[pos + i] ^ 0x64);
        pos += keyLen;

        int decLen = keyLen - (keyLen % 16);
        Cipher cipher = Cipher.getInstance("AES/ECB/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, new SecretKeySpec(NCM_KEY, "AES"));
        byte[] plain = cipher.doFinal(kdata, 0, decLen);

        int plen = plain.length;
        int pad = plain[plen - 1] & 0xFF;
        if (pad > 0 && pad <= 16) plen -= pad;

        if (plen <= PREFIX.length) throw new IOException("密钥格式不符");
        for (int i = 0; i < PREFIX.length; i++) {
            if (plain[i] != PREFIX[i]) throw new IOException("密钥格式不符");
        }
        byte[] audioKey = new byte[plen - PREFIX.length];
        System.arraycopy(plain, PREFIX.length, audioKey, 0, audioKey.length);

        int metaLen = rdU32(raw, pos); pos += 4 + metaLen;
        pos += 4 + 5;
        if (pos + 4 > raw.length) throw new IOException("文件不完整");
        int imgLen = rdU32(raw, pos); pos += 4 + imgLen;
        if (pos >= raw.length) throw new IOException("文件不完整");

        int audioLen = raw.length - pos;
        byte[] audio = new byte[audioLen];
        System.arraycopy(raw, pos, audio, 0, audioLen);
        decryptAudio(audio, audioKey);

        Decrypted d = new Decrypted();
        d.audio = audio;
        boolean flac = audioLen >= 4 && audio[0] == 'f' && audio[1] == 'L'
                && audio[2] == 'a' && audio[3] == 'C';
        d.ext = flac ? ".flac" : ".mp3";
        return d;
    }

    static void decryptAudio(byte[] data, byte[] key) {
        byte[] box = new byte[256];
        for (int i = 0; i < 256; i++) box[i] = (byte) i;
        int last = 0;
        for (int i = 0; i < 256; i++) {
            byte c = box[i];
            last = ((c & 0xFF) + last + (key[i % key.length] & 0xFF)) & 0xFF;
            box[i] = box[last];
            box[last] = c;
        }
        for (int i = 0; i < data.length; i++) {
            int j = (i + 1) & 0xFF;
            int idx = ((box[j] & 0xFF) + (box[((box[j] & 0xFF) + j) & 0xFF] & 0xFF)) & 0xFF;
            data[i] = (byte) (data[i] ^ box[idx]);
        }
    }

    static int rdU32(byte[] b, int p) {
        return (b[p] & 0xFF) | ((b[p + 1] & 0xFF) << 8)
                | ((b[p + 2] & 0xFF) << 16) | ((b[p + 3] & 0xFF) << 24);
    }
}
