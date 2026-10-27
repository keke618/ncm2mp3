# -*- coding: utf-8 -*-
"""NCM 转 MP3 —— 图形界面

功能：
  * 拖拽 .ncm 文件或文件夹到窗口
  * 批量添加、批量转换
  * 原地生成，或导出到指定文件夹
  * 可选：转换成功后删除原文件
"""
from __future__ import annotations

import os
import queue
import sys
import threading
import traceback
from pathlib import Path

# 便携版 Python 用 ._pth 接管了 sys.path，脚本目录不会自动加入，这里补上
_HERE = str(Path(__file__).resolve().parent)
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import tkinter as tk
from tkinter import filedialog, messagebox

from ncm_core import collect_ncm, convert_file, NcmError

try:
    from tkinterdnd2 import DND_FILES, TkinterDnD
    HAS_DND = True
except Exception:
    HAS_DND = False

__version__ = "1.0.0"

# ---------------------------------------------------------------- 外观
BG = "#f4f5f7"
CARD = "#ffffff"
TEXT = "#1f2329"
MUTED = "#8b93a1"
ACCENT = "#e8474b"
ACCENT_DK = "#cf3639"
BORDER = "#e3e5e9"
FIELD = "#fafbfc"
OK = "#22a06b"
ERR = "#d93026"

UI = "Microsoft YaHei UI"


def human(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024.0
    return f"{n:.1f} GB"


class App:
    def __init__(self, root):
        self.root = root
        self.files = []            # list[Path]
        self.running = False
        self.queue = queue.Queue()

        root.title("NCM 转 MP3")
        root.configure(bg=BG)
        root.geometry("640x760")
        root.minsize(580, 700)
        root.resizable(True, True)

        self._build()
        self._setup_dnd()
        self.root.after(100, self._poll)

    # ------------------------------------------------------------ 界面
    def _build(self):
        root = self.root

        head = tk.Frame(root, bg=BG)
        head.pack(fill="x", padx=24, pady=(18, 8))
        tk.Label(head, text="NCM 转 MP3", bg=BG, fg=TEXT,
                 font=(UI, 17, "bold")).pack(anchor="w")
        tk.Label(head, text="网易云音乐加密格式解密 · 拖进来就能用" +
                            ("" if HAS_DND else "（当前环境不支持拖拽，请用按钮添加）"),
                 bg=BG, fg=MUTED, font=(UI, 9)).pack(anchor="w", pady=(2, 0))

        # ---- 拖放区 ----
        self.drop = tk.Frame(root, bg=CARD, highlightbackground=BORDER,
                             highlightthickness=1)
        self.drop.pack(fill="x", padx=24, pady=(6, 10))
        self.drop_label = tk.Label(
            self.drop,
            text="把 .ncm 文件或文件夹拖到这里\n（也可以点下面的按钮选择）",
            bg=CARD, fg=MUTED, font=(UI, 11), justify="center", pady=22)
        self.drop_label.pack(fill="both")

        # ---- 按钮行 ----
        bar = tk.Frame(root, bg=BG)
        bar.pack(fill="x", padx=24)
        self._btn(bar, "添加文件", self.pick_files, primary=False).pack(side="left")
        self._btn(bar, "添加文件夹", self.pick_folder, primary=False).pack(side="left", padx=(8, 0))
        self._btn(bar, "清空列表", self.clear_list, primary=False).pack(side="left", padx=(8, 0))
        self.count_label = tk.Label(bar, text="0 个文件", bg=BG, fg=MUTED,
                                    font=(UI, 9))
        self.count_label.pack(side="right")

        # ---- 文件列表 ----
        list_wrap = tk.Frame(root, bg=CARD, highlightbackground=BORDER,
                             highlightthickness=1)
        list_wrap.pack(fill="both", expand=True, padx=24, pady=(10, 10))

        self.listbox = tk.Listbox(
            list_wrap, bg=CARD, fg=TEXT, font=(UI, 9), relief="flat",
            activestyle="none", selectmode="extended", bd=0,
            highlightthickness=0, selectbackground="#fdecec",
            selectforeground=TEXT)
        scroll = tk.Scrollbar(list_wrap, command=self.listbox.yview)
        self.listbox.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        self.listbox.pack(side="left", fill="both", expand=True, padx=10, pady=8)

        # ---- 输出选项 ----
        opt = tk.Frame(root, bg=CARD, highlightbackground=BORDER, highlightthickness=1)
        opt.pack(fill="x", padx=24)

        inner = tk.Frame(opt, bg=CARD)
        inner.pack(fill="x", padx=14, pady=12)

        tk.Label(inner, text="输出位置", bg=CARD, fg=MUTED,
                 font=(UI, 9)).pack(anchor="w")

        self.mode = tk.StringVar(value="inplace")
        row1 = tk.Frame(inner, bg=CARD)
        row1.pack(fill="x", pady=(6, 0))
        tk.Radiobutton(row1, text="原地生成（和原文件放一起）", variable=self.mode,
                       value="inplace", bg=CARD, fg=TEXT, font=(UI, 9),
                       activebackground=CARD, selectcolor=CARD,
                       highlightthickness=0, bd=0,
                       command=self._sync_mode).pack(anchor="w")

        row2 = tk.Frame(inner, bg=CARD)
        row2.pack(fill="x", pady=(4, 0))
        tk.Radiobutton(row2, text="导出到", variable=self.mode, value="custom",
                       bg=CARD, fg=TEXT, font=(UI, 9), activebackground=CARD,
                       selectcolor=CARD, highlightthickness=0, bd=0,
                       command=self._sync_mode).pack(side="left")
        self.out_entry = tk.Entry(row2, font=(UI, 9), bg=FIELD, fg=TEXT,
                                  relief="flat", highlightthickness=1,
                                  highlightbackground=BORDER, highlightcolor=ACCENT)
        self.out_entry.pack(side="left", fill="x", expand=True, padx=(6, 6), ipady=4)
        self.out_btn = self._btn(row2, "浏览", self.pick_outdir, primary=False)
        self.out_btn.pack(side="left")

        self.del_var = tk.BooleanVar(value=False)
        tk.Checkbutton(inner, text="转换成功后删除原 .ncm 文件", variable=self.del_var,
                       bg=CARD, fg=TEXT, font=(UI, 9), activebackground=CARD,
                       selectcolor=CARD, highlightthickness=0, bd=0
                       ).pack(anchor="w", pady=(10, 0))

        # ---- 底部：进度 + 开始 ----
        bottom = tk.Frame(root, bg=BG)
        bottom.pack(fill="x", padx=24, pady=(10, 16))

        self.progress = tk.Canvas(bottom, height=6, bg="#e9ebee",
                                  highlightthickness=0, bd=0)
        self.progress.pack(fill="x", pady=(0, 8))
        self.bar = self.progress.create_rectangle(0, 0, 0, 6, fill=ACCENT, width=0)

        row = tk.Frame(bottom, bg=BG)
        row.pack(fill="x")
        self.start_btn = self._btn(row, "开始转换", self.start, primary=True)
        self.start_btn.pack(side="left")
        self.status = tk.Label(row, text="等待添加文件", bg=BG, fg=MUTED,
                               font=(UI, 9), anchor="w")
        self.status.pack(side="left", padx=(12, 0), fill="x", expand=True)

    def _btn(self, parent, text, cmd, primary=False):
        bg = ACCENT if primary else "#ffffff"
        fg = "#ffffff" if primary else TEXT
        hover = ACCENT_DK if primary else "#f2f3f5"
        b = tk.Button(parent, text=text, command=cmd, bg=bg, fg=fg,
                      activebackground=hover, activeforeground=fg,
                      relief="flat", bd=0, cursor="hand2",
                      font=(UI, 9, "bold" if primary else "normal"),
                      padx=14, pady=6,
                      highlightthickness=1,
                      highlightbackground=ACCENT if primary else BORDER)
        b.bind("<Enter>", lambda e: b.configure(bg=hover))
        b.bind("<Leave>", lambda e: b.configure(bg=bg))
        return b

    def _sync_mode(self):
        state = "normal" if self.mode.get() == "custom" else "disabled"
        self.out_entry.configure(state=state)
        self.out_btn.configure(state=state)
        if state == "disabled":
            self.out_entry.configure(bg="#f0f1f3")
        else:
            self.out_entry.configure(bg=FIELD)

    # ------------------------------------------------------------ 拖拽
    def _setup_dnd(self):
        if not HAS_DND:
            return
        for w in (self.root, self.drop, self.drop_label, self.listbox):
            try:
                w.drop_target_register(DND_FILES)
                w.dnd_bind("<<Drop>>", self.on_drop)
            except Exception:
                pass
        self.drop_label.configure(
            text="把 .ncm 文件或文件夹拖到这里\n（也可以点下面的按钮选择）")

    def on_drop(self, event):
        try:
            paths = self.root.tk.splitlist(event.data)
        except Exception:
            paths = [event.data]
        self.add_paths(paths)
        return event.action

    # ------------------------------------------------------------ 列表
    def pick_files(self):
        paths = filedialog.askopenfilenames(
            title="选择 ncm 文件",
            filetypes=[("网易云音乐", "*.ncm"), ("所有文件", "*.*")])
        if paths:
            self.add_paths(list(paths))

    def pick_folder(self):
        d = filedialog.askdirectory(title="选择包含 ncm 的文件夹")
        if d:
            self.add_paths([d])

    def pick_outdir(self):
        d = filedialog.askdirectory(title="选择导出目录")
        if d:
            self.out_entry.delete(0, "end")
            self.out_entry.insert(0, d)

    def add_paths(self, paths):
        new = collect_ncm(paths)
        existing = {f for f in self.files}
        added = 0
        for f in new:
            if f not in existing:
                self.files.append(f)
                existing.add(f)
                added += 1
        self.refresh()
        if added:
            self.status.configure(text=f"已添加 {added} 个文件", fg=MUTED)
        elif new:
            self.status.configure(text="这些文件已经在列表里了", fg=MUTED)
        else:
            self.status.configure(text="没有找到 .ncm 文件", fg=ERR)

    def refresh(self):
        self.listbox.delete(0, "end")
        for f in self.files:
            try:
                size = human(f.stat().st_size)
            except OSError:
                size = "?"
            self.listbox.insert("end", f"  {f.name}    ({size})")
        self.count_label.configure(text=f"{len(self.files)} 个文件")

    def clear_list(self):
        if self.running:
            return
        self.files.clear()
        self.refresh()
        self._set_progress(0, 1)
        self.status.configure(text="等待添加文件", fg=MUTED)

    # ------------------------------------------------------------ 转换
    def start(self):
        if self.running:
            return
        if not self.files:
            messagebox.showinfo("提示", "先添加几个 .ncm 文件吧")
            return

        out_dir = None
        if self.mode.get() == "custom":
            raw = self.out_entry.get().strip()
            if not raw:
                messagebox.showwarning("提示", "请选择导出目录")
                return
            out_dir = Path(raw)
            try:
                out_dir.mkdir(parents=True, exist_ok=True)
            except OSError as e:
                messagebox.showerror("错误", f"无法创建目录：\n{e}")
                return

        self.running = True
        self.start_btn.configure(state="disabled", text="转换中…")
        self._set_progress(0, len(self.files))
        threading.Thread(target=self._worker,
                         args=(list(self.files), out_dir, self.del_var.get()),
                         daemon=True).start()

    def _worker(self, files, out_dir, delete_src):
        ok = fail = 0
        for i, f in enumerate(files, 1):
            try:
                out = convert_file(f, out_dir, delete_src)
                self.queue.put(("item", i, len(files), f, True, out.name, ""))
                ok += 1
            except Exception as e:
                self.queue.put(("item", i, len(files), f, False, "", str(e)))
                fail += 1
        self.queue.put(("done", ok, fail))

    def _poll(self):
        try:
            while True:
                msg = self.queue.get_nowait()
                if msg[0] == "item":
                    _, i, total, f, ok, name, err = msg
                    self._mark(i, ok, err)
                    self._set_progress(i, total)
                    self.status.configure(
                        text=f"{i}/{total}  {'✓ ' + name if ok else '✗ ' + f.name}",
                        fg=OK if ok else ERR)
                elif msg[0] == "done":
                    ok, fail = msg[1], msg[2]
                    self._finish(ok, fail)
        except queue.Empty:
            pass
        self.root.after(100, self._poll)

    def _mark(self, index, ok, err):
        idx = index - 1
        if idx < self.listbox.size():
            text = self.listbox.get(idx)
            self.listbox.delete(idx)
            self.listbox.insert(idx, text + ("   ✓" if ok else "   ✗"))
            self.listbox.itemconfigure(idx, foreground=OK if ok else ERR)

    def _set_progress(self, done, total):
        w = self.progress.winfo_width() or 560
        frac = 0 if total == 0 else done / total
        self.progress.coords(self.bar, 0, 0, int(w * frac), 6)

    def _finish(self, ok, fail):
        self.running = False
        self.start_btn.configure(state="normal", text="开始转换")
        color = OK if fail == 0 else ERR
        self.status.configure(text=f"完成：成功 {ok} 个，失败 {fail} 个", fg=color)
        if fail == 0:
            messagebox.showinfo("完成", f"全部转换成功：{ok} 个")
        else:
            messagebox.showwarning("完成", f"成功 {ok} 个，失败 {fail} 个\n"
                                           "失败的通常是文件损坏或不是 ncm")


def main():
    # 命令行自测模式：ncm2mp3.exe --selftest <文件> [输出目录]
    if "--selftest" in sys.argv:
        i = sys.argv.index("--selftest")
        args = sys.argv[i + 1:]
        if not args:
            print("need file")
            return 1
        src = Path(args[0])
        out = Path(args[1]) if len(args) > 1 else src.parent
        try:
            p = convert_file(src, out, False)
            print("OK", p.name, p.stat().st_size)
            return 0
        except Exception as e:
            print("FAIL", e)
            return 1

    try:
        from ctypes import windll
        windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass

    root = TkinterDnD.Tk() if HAS_DND else tk.Tk()
    root.option_add("*Font", (UI, 9))
    app = App(root)

    # 支持把文件拖到 exe 图标上启动
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        app.add_paths(sys.argv[1:])

    # 调试：记录启动参数与识别到的文件数
    try:
        import tempfile
        dbg = Path(tempfile.gettempdir()) / "ncm_gui_debug.txt"
        dbg.write_text(
            "argv=" + repr(sys.argv) + "\nfiles=" + str(len(app.files)) + "\n",
            encoding="utf-8")
    except Exception:
        pass

    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
