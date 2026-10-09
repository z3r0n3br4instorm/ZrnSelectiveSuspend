#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""zss-report-gui: the tester report in a window.

The same report as zss-report: collected without changing anything, written to
a folder the tester can read, and sent with the tester's own mail program.
Needs PySide6 or PyQt6; without either, use zss-report in a terminal.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import zss_report as zr  # noqa: E402

try:
    from PySide6.QtCore import QObject, Qt, QThread, Signal
    from PySide6.QtGui import QColor, QFont, QFontDatabase, QGuiApplication, QPalette
    from PySide6.QtWidgets import (QApplication, QFrame, QHBoxLayout, QHeaderView, QLabel, QMainWindow, QMessageBox,
                                   QPlainTextEdit, QProgressBar, QPushButton, QSplitter, QStyleFactory, QTreeWidget,
                                   QTreeWidgetItem, QVBoxLayout, QWidget)
except ImportError:
    try:
        from PyQt6.QtCore import QObject, Qt, QThread
        from PyQt6.QtCore import pyqtSignal as Signal
        from PyQt6.QtGui import QColor, QFont, QFontDatabase, QGuiApplication, QPalette
        from PyQt6.QtWidgets import (QApplication, QFrame, QHBoxLayout, QHeaderView, QLabel, QMainWindow, QMessageBox,
                                     QPlainTextEdit, QProgressBar, QPushButton, QSplitter, QStyleFactory, QTreeWidget,
                                     QTreeWidgetItem, QVBoxLayout, QWidget)
    except ImportError:
        sys.stderr.write("zss-report-gui needs PySide6 or PyQt6 (e.g. pacman -S pyside6, apt install python3-pyside6).\n"
                         "Without them, run zss-report in a terminal: it does the same.\n")
        sys.exit(2)

PRIVACY = ("Nothing on this computer is changed, and nothing is sent by itself. The moving tests open a few test "
           "windows for a moment and switch no graphics card off. The report is saved as a file you can "
           "read first; you send it with your own mail program. Computer and user names, disk identifiers, serial "
           "numbers and network addresses are removed before anything is saved.")

# Result names and colours: (label, light-theme colour, dark-theme colour).
RESULT = {
    "ok": ("Passed", "#2e7d32", "#81c784"),
    "warn": ("Note", "#9a6700", "#e3b341"),
    "fail": ("Problem", "#c62828", "#ef9a9a"),
    "info": ("Recorded", "#5f6368", "#9aa0a6"),
}


def dark(widget):
    return widget.palette().color(QPalette.ColorRole.Window).lightness() < 128


def describe(value, indent=0):
    """A check's details as plain indented lines."""
    pad = "  " * indent
    lines = []
    if isinstance(value, dict):
        for k, v in value.items():
            if isinstance(v, (dict, list)) and v:
                lines.append(f"{pad}{k.replace('_', ' ')}:")
                lines += describe(v, indent + 1)
            else:
                lines.append(f"{pad}{k.replace('_', ' ')}: {word(v)}")
    elif isinstance(value, list):
        for v in value:
            if isinstance(v, dict):
                lines += describe(v, indent)
                lines.append("")
            else:
                lines.append(f"{pad}{v}")
    else:
        lines.append(f"{pad}{word(value)}")
    return lines


def word(v):
    return "yes" if v is True else "no" if v is False else "-" if v in ("", None) else v


class Collector(QObject):
    step = Signal(str, str, str)
    finished = Signal(object, object)

    def run(self):
        results, raw = zr.collect(lambda n, s, t: self.step.emit(n, s, t))
        self.finished.emit(results, raw)


class FullTest(QObject):
    line = Signal(str)
    finished = Signal(object)

    def __init__(self, results):
        super().__init__()
        self.results = results

    def run(self):
        self.finished.emit(zr.run_full_test(self.results, log=lambda t: self.line.emit(t)))


class Window(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("ZrnSelectiveSuspend Hardware Report")
        self.resize(980, 640)
        self.setMinimumSize(760, 480)
        self.results = self.raw = None
        self.details = {}
        self.extra = {}
        self.folder = self.archive = None
        self.thread = None

        # ---- header
        title = QLabel("Hardware Report")
        f = title.font()
        f.setPointSizeF(f.pointSizeF() * 1.45)
        f.setWeight(QFont.Weight.DemiBold)
        title.setFont(f)
        subtitle = QLabel("ZrnSelectiveSuspend tester kit: what this computer offers for switching graphics cards "
                          "off, moving programs between them and lending them to virtual machines.")
        subtitle.setWordWrap(True)
        privacy = QLabel(PRIVACY)
        privacy.setWordWrap(True)
        privacy.setObjectName("muted")
        header = QVBoxLayout()
        header.setSpacing(4)
        header.addWidget(title)
        header.addWidget(subtitle)
        header.addSpacing(6)
        header.addWidget(privacy)
        header_box = QWidget()
        header_box.setObjectName("header")
        header_box.setLayout(header)
        header.setContentsMargins(24, 20, 24, 16)

        # ---- checks and details
        self.tree = QTreeWidget()
        self.tree.setHeaderLabels(["Check", "Result"])
        self.tree.setRootIsDecorated(False)
        self.tree.setUniformRowHeights(True)
        self.tree.setAlternatingRowColors(True)
        self.tree.header().setStretchLastSection(False)
        self.tree.header().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        self.tree.header().setSectionResizeMode(1, QHeaderView.ResizeMode.ResizeToContents)
        for name, _ in zr.CHECKS:
            QTreeWidgetItem(self.tree, [name, "Not run"])
        self.tree.currentItemChanged.connect(self.show_detail)

        self.detail_title = QLabel("Select a check")
        f2 = self.detail_title.font()
        f2.setWeight(QFont.Weight.DemiBold)
        self.detail_title.setFont(f2)
        self.detail_summary = QLabel("Press Collect to examine this computer. With the moving tests it takes a few minutes.")
        self.detail_summary.setWordWrap(True)
        self.detail_summary.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.detail = QPlainTextEdit()
        self.detail.setReadOnly(True)
        self.detail.setFrameShape(QFrame.Shape.NoFrame)
        self.detail.setFont(QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont))
        right = QVBoxLayout()
        right.setContentsMargins(16, 12, 16, 12)
        right.setSpacing(6)
        right.addWidget(self.detail_title)
        right.addWidget(self.detail_summary)
        right.addWidget(self.detail, 1)
        right_box = QWidget()
        right_box.setObjectName("panel")
        right_box.setLayout(right)

        split = QSplitter()
        split.addWidget(self.tree)
        split.addWidget(right_box)
        split.setStretchFactor(0, 0)
        split.setStretchFactor(1, 1)
        split.setSizes([300, 680])
        split.setChildrenCollapsible(False)

        # ---- footer
        self.progress = QProgressBar()
        self.progress.setRange(0, len(zr.CHECKS))
        self.progress.setTextVisible(False)
        self.progress.setFixedHeight(4)
        self.progress.hide()
        self.status = QLabel("")
        self.status.setObjectName("muted")
        self.status.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)

        self.full_btn = QPushButton("Run full test…")
        self.folder_btn = QPushButton("Show files")
        self.copy_btn = QPushButton("Copy file path")
        self.collect_btn = QPushButton("Collect")
        self.collect_btn.setObjectName("primary")
        self.collect_btn.setDefault(True)
        for b in (self.full_btn, self.folder_btn, self.copy_btn):
            b.setEnabled(False)
        self.folder_btn.hide()
        self.copy_btn.hide()
        self.full_btn.setToolTip("Builds and installs ZSS from its source tree, runs its tests and switches the "
                                 "discrete graphics card off and on once. Asks for your password.")
        self.collect_btn.clicked.connect(self.primary)
        self.full_btn.clicked.connect(self.full_test)
        self.folder_btn.clicked.connect(self.open_folder)
        self.copy_btn.clicked.connect(lambda: QGuiApplication.clipboard().setText(self.archive or ""))

        buttons = QHBoxLayout()
        buttons.setContentsMargins(24, 12, 24, 16)
        buttons.setSpacing(8)
        buttons.addWidget(self.status, 1)
        buttons.addWidget(self.full_btn)
        buttons.addWidget(self.folder_btn)
        buttons.addWidget(self.copy_btn)
        buttons.addWidget(self.collect_btn)

        rule = QFrame()
        rule.setFrameShape(QFrame.Shape.HLine)
        rule.setObjectName("rule")
        rule2 = QFrame()
        rule2.setFrameShape(QFrame.Shape.HLine)
        rule2.setObjectName("rule")

        box = QVBoxLayout()
        box.setContentsMargins(0, 0, 0, 0)
        box.setSpacing(0)
        box.addWidget(header_box)
        box.addWidget(rule)
        box.addWidget(split, 1)
        box.addWidget(self.progress)
        box.addWidget(rule2)
        box.addLayout(buttons)
        w = QWidget()
        w.setLayout(box)
        self.setCentralWidget(w)
        self.style_sheet()

    def style_sheet(self):
        pal = self.palette()
        accent = pal.color(QPalette.ColorRole.Highlight).name()
        on_accent = pal.color(QPalette.ColorRole.HighlightedText).name()
        base = pal.color(QPalette.ColorRole.Base).name()
        window = pal.color(QPalette.ColorRole.Window)
        line = window.darker(120).name() if not dark(self) else window.lighter(160).name()
        muted = "#9aa0a6" if dark(self) else "#5f6368"
        self.setStyleSheet(f"""
            QWidget#header {{ background: {base}; }}
            QWidget#panel {{ background: {base}; }}
            QFrame#rule {{ color: {line}; max-height: 1px; }}
            QLabel#muted {{ color: {muted}; }}
            QTreeWidget {{ border: none; border-right: 1px solid {line}; }}
            QTreeWidget::item {{ padding: 4px 8px; }}
            QHeaderView::section {{ background: {base}; border: none; border-bottom: 1px solid {line};
                                    padding: 6px 8px; font-weight: 600; }}
            QPlainTextEdit {{ background: {base}; }}
            QPushButton {{ padding: 5px 14px; border: 1px solid {line}; border-radius: 2px; background: {base}; }}
            QPushButton:disabled {{ color: {muted}; }}
            QPushButton#primary {{ background: {accent}; color: {on_accent}; border: 1px solid {accent}; font-weight: 600; }}
            QPushButton#primary:disabled {{ background: {line}; border-color: {line}; color: {muted}; }}
            QProgressBar {{ border: none; background: transparent; }}
            QProgressBar::chunk {{ background: {accent}; }}
        """)

    def item(self, name):
        for i in range(self.tree.topLevelItemCount()):
            it = self.tree.topLevelItem(i)
            if it.text(0) == name:
                return it
        return None

    def set_result(self, it, status):
        label, light, darkc = RESULT.get(status, (status, "#5f6368", "#9aa0a6"))
        it.setText(1, label)
        it.setForeground(1, QColor(darkc if dark(self) else light))

    # ---- the primary button: Collect, then Send

    def primary(self):
        if self.archive and self.collect_btn.text().startswith("Send"):
            self.send()
        else:
            self.collect()

    def collect(self):
        self.collect_btn.setEnabled(False)
        self.full_btn.setEnabled(False)
        self.progress.setValue(0)
        self.progress.show()
        self.status.setText("Examining this computer…")
        for i in range(self.tree.topLevelItemCount()):
            it = self.tree.topLevelItem(i)
            it.setText(1, "Waiting")
            it.setForeground(1, QColor("#9aa0a6" if dark(self) else "#5f6368"))
        self.thread = QThread()
        self.worker = Collector()
        self.worker.moveToThread(self.thread)
        self.thread.started.connect(self.worker.run)
        self.worker.step.connect(self.on_step)
        self.worker.finished.connect(self.on_collected)
        self.worker.finished.connect(self.thread.quit)
        self.thread.start()

    def on_step(self, name, status, summary):
        it = self.item(name)
        if it:
            self.set_result(it, status)
            it.setToolTip(0, summary)
        self.progress.setValue(self.progress.value() + 1)

    def on_collected(self, results, raw):
        self.results, self.raw = results, raw
        self.details = {r["check"]: r for r in results}
        self.progress.hide()
        self.save()
        self.collect_btn.setEnabled(True)
        self.collect_btn.setText("Send report…")
        self.full_btn.setEnabled(zr.full_test_steps() is not None)
        if zr.full_test_steps() is None:
            self.full_btn.setToolTip("The full test needs the ZSS source tree next to this program.")
        self.tree.setCurrentItem(self.tree.topLevelItem(0))
        self.show_detail(self.tree.topLevelItem(0))

    def show_detail(self, it, _prev=None):
        if not it:
            return
        r = self.details.get(it.text(0))
        self.detail_title.setText(it.text(0))
        if not r:
            self.detail_summary.setText("Not collected yet.")
            self.detail.setPlainText("")
            return
        label = RESULT.get(r["status"], (r["status"],))[0]
        self.detail_summary.setText(f"{label}: {r['summary']}")
        self.detail.setPlainText("\n".join(describe(r["detail"])) or "No further details.")

    def save(self):
        self.folder, self.archive = zr.write_report(self.results, self.raw, None, self.extra)
        self.status.setText(f"Saved to {self.archive}")
        self.status.setToolTip(self.archive)
        for b in (self.folder_btn, self.copy_btn):
            b.show()
            b.setEnabled(True)

    def full_test(self):
        steps = zr.full_test_steps()
        text = "\n".join(f"{name}: {' '.join(cmd)}" for name, cmd in steps)
        box = QMessageBox(self)
        box.setIcon(QMessageBox.Icon.Warning)
        box.setWindowTitle("Run the full test")
        box.setText("The full test changes this computer.")
        box.setInformativeText(
            "It builds and installs ZSS with its kernel module (your graphics driver is not patched), runs the test "
            "suite, and switches the discrete graphics card off and on once. Programs using that card are moved or "
            "paused meanwhile. You will be asked for your password.\n\nSave your work before continuing.")
        box.setDetailedText(text)
        box.setStandardButtons(QMessageBox.StandardButton.Cancel | QMessageBox.StandardButton.Ok)
        box.button(QMessageBox.StandardButton.Ok).setText("Run full test")
        box.setDefaultButton(QMessageBox.StandardButton.Cancel)
        if box.exec() != QMessageBox.StandardButton.Ok:
            return
        self.full_btn.setEnabled(False)
        self.collect_btn.setEnabled(False)
        self.detail_title.setText("Full test")
        self.detail_summary.setText("Running. The output appears below.")
        self.detail.setPlainText("")
        self.status.setText("Full test running…")
        self.thread = QThread()
        self.worker = FullTest(self.results)
        self.worker.moveToThread(self.thread)
        self.thread.started.connect(self.worker.run)
        self.worker.line.connect(self.detail.appendPlainText)
        self.worker.finished.connect(self.on_full_test)
        self.worker.finished.connect(self.thread.quit)
        self.thread.start()

    def on_full_test(self, outcome):
        self.extra["full_test"] = outcome
        self.save()
        self.detail_summary.setText("Finished: " + ", ".join(f"{k} {v[0]}" for k, v in outcome.items()))
        self.collect_btn.setEnabled(True)

    def open_folder(self):
        subprocess.Popen(["xdg-open", self.folder], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def send(self):
        QGuiApplication.clipboard().setText(self.archive)
        opened = zr.open_mail(self.results, self.archive)
        box = QMessageBox(self)
        box.setWindowTitle("Send the report")
        box.setIcon(QMessageBox.Icon.Information)
        box.setText("Attach the report file to the email." if opened else "No mail program could be opened.")
        box.setInformativeText(
            ("Your mail program opens with the address and a summary filled in. " if opened else
             f"Please write to {zr.REPORT_TO} yourself. ") +
            f"The file's path is copied, ready to paste into the attach dialog:\n\n{self.archive}")
        box.exec()


def main():
    app = QApplication(sys.argv)
    app.setApplicationName("ZrnSelectiveSuspend Hardware Report")
    if "Fusion" in QStyleFactory.keys():
        app.setStyle("Fusion")  # the same look on every desktop; colours still follow the system palette
    w = Window()
    w.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
