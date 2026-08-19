#!/usr/bin/env python3
"""Chuneng ARC331 live bench test via pywinauto.

Launches dist\\uds_tool_qt.exe, selects ChuNeng ARC331 left-rear radar,
runs online probe, then starts flashing, and collects evidence
(screenshots, execution log, ASC trace, HTML report).
Usage:
  set PYWIN32_DLL_DIR=<python site-packages\\win32>
  python chuneng331_live_test.py --exe <dist\\uds_tool_qt.exe> --evidence <dir>
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
import win32con

os.add_dll_directory(os.environ["PYWIN32_DLL_DIR"])
from pywinauto.application import Application
from pywinauto import Desktop


def find_by_suffix(window, suffix, control_type=None):
    deadline = time.time() + 20
    while time.time() < deadline:
        for control in window.descendants():
            info = control.element_info
            if info.automation_id.endswith(suffix):
                if control_type is None or info.control_type == control_type:
                    return control
        time.sleep(0.25)
    raise RuntimeError(f"UI control not found: {suffix}")


def select_combo(window, suffix, text):
    combo = find_by_suffix(window, suffix, "ComboBox")
    combo.expand()
    time.sleep(0.2)
    items = combo.descendants(control_type="ListItem")
    names = [i.window_text() for i in items]
    if text not in names:
        raise RuntimeError(f"combo {suffix} item not found: {text}; items={names}")
    items[names.index(text)].click_input()
    time.sleep(0.8)
    combo.expand()
    time.sleep(0.2)
    selected = [i.window_text() for i in combo.descendants(control_type="ListItem")
                if i.is_selected()]
    combo.collapse()
    combo.type_keys("{ESC}")
    time.sleep(0.2)
    if selected != [text]:
        raise RuntimeError(f"combo {suffix} selection mismatch: {selected}")
    return combo


def newest_after(directory, pattern, after):
    matches = [p for p in Path(directory).glob(pattern)
               if p.is_file() and p.stat().st_mtime >= after - 1]
    return max(matches, key=lambda p: p.stat().st_mtime) if matches else None


def set_path_edit(window, app, suffix, path):
    browse_map = {
        "driverPathLineEdit": "driverBrowseButton",
        "driverVerifyPathLineEdit": "driverVerifyBrowseButton",
        "appPathLineEdit": "appBrowseButton",
        "appVerifyPathLineEdit": "appVerifyBrowseButton",
        "calPathLineEdit": "calBrowseButton",
        "seedKeyDllPathLineEdit": "seedKeyDllBrowseButton",
    }
    browse_suffix = browse_map.get(suffix, suffix.replace("LineEdit", "BrowseButton"))
    browse = find_by_suffix(window, browse_suffix, "Button")
    if not browse.is_enabled():
        raise RuntimeError(f"browse button disabled: {browse_suffix}")
    # Activate even when the file row is outside the current scroll viewport;
    # this avoids depending on an interactive mouse desktop.
    browse.invoke()
    time.sleep(2)
    dlg = None
    # Native QFileDialog is exposed as a top-level Win32 dialog and is not
    # necessarily returned by the UIA Application window enumeration.
    for win in Desktop(backend="win32").windows(class_name="#32770"):
        if win.is_visible():
            dlg = win
            break
    if dlg is None:
        raise RuntimeError(f"file dialog not opened for {suffix}")
    # Standard Windows Open dialog: edt1/control-id 1148 is the filename
    # field. Set it through Win32 messages and submit IDOK so this also works
    # in a background desktop session without synthetic mouse/keyboard input.
    filename_edits = [control for control in dlg.descendants(class_name="Edit")
                      if control.control_id() == 1148]
    if not filename_edits:
        raise RuntimeError(f"file name edit not found for {suffix}")
    filename_edits[0].set_edit_text(path)
    dlg.send_message(win32con.WM_COMMAND, win32con.IDOK)
    time.sleep(1.5)


def wait_log_contains(log_view, needle, timeout, also_status=None):
    deadline = time.time() + timeout
    while time.time() < deadline:
        text = log_view.get_value()
        if needle in text:
            return text
        if also_status is not None:
            st = also_status.window_text()
            if "失败" in st or "FAIL" in st:
                raise RuntimeError(f"status shows failure: {st}")
        time.sleep(1)
    raise TimeoutError(f"log did not contain {needle!r} within {timeout}s")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--evidence", required=True)
    parser.add_argument("--do-flash", action="store_true",
                        help="run the flash after probe (default: probe only)")
    parser.add_argument("--entry", default="APP",
                        help="entry mode: APP or FT (default APP)")
    parser.add_argument("--driver-s19")
    parser.add_argument("--driver-verify-asc")
    parser.add_argument("--app-s19")
    parser.add_argument("--app-verify-asc")
    parser.add_argument("--project-index", type=int)
    parser.add_argument("--device-index", type=int)
    parser.add_argument("--radar-index", type=int)
    parser.add_argument("--entry-index", type=int)
    args = parser.parse_args()

    exe = Path(args.exe).resolve()
    evidence = Path(args.evidence).resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    logs_dir = exe.parent / "logs"
    logs_dir.mkdir(exist_ok=True)
    started = time.time()

    process = subprocess.Popen([str(exe)], cwd=str(exe.parent))
    print(f"launched pid={process.pid}", flush=True)
    app = Application(backend="uia").connect(process=process.pid, timeout=20)
    window = app.window(title="UDS 通用刷写工具")
    window.wait("visible", timeout=20)
    window.set_focus()
    window.maximize()
    time.sleep(1.5)

    try:
        # QSettings persists the previous selection (ChuNeng/ARC331/
        # left-rear/APP/Channel 2); Chinese combo text arrives as mojibake
        # under this locale, so rely on the persisted selection and only
        # verify the endpoint IDs below.

        if args.driver_s19:
            set_path_edit(window, app, "driverPathLineEdit", args.driver_s19)
        if args.driver_verify_asc:
            set_path_edit(window, app, "driverVerifyPathLineEdit", args.driver_verify_asc)
        if args.app_s19:
            set_path_edit(window, app, "appPathLineEdit", args.app_s19)
        if args.app_verify_asc:
            set_path_edit(window, app, "appVerifyPathLineEdit", args.app_verify_asc)

        tx = find_by_suffix(window, "txIdLineEdit", "Edit").get_value()
        rx = find_by_suffix(window, "rxIdLineEdit", "Edit").get_value()
        app_file = find_by_suffix(window, "appPathLineEdit", "Edit").get_value()
        driver_file = find_by_suffix(window, "driverPathLineEdit", "Edit").get_value()
        seed = find_by_suffix(window, "seedKeyDllPathLineEdit", "Edit").get_value()
        print(f"SELECTED TX={tx} RX={rx}", flush=True)
        print(f"DRIVER={driver_file}", flush=True)
        print(f"APP={app_file}", flush=True)
        print(f"SEED={seed}", flush=True)
        if tx.upper() not in ("0X72E", "0X72C") or rx.upper() not in ("0X72F", "0X72D"):
            raise RuntimeError(f"unexpected endpoint {tx}/{rx}")

        # --- step 1: online probe ---
        probe = find_by_suffix(window, "probeButton", "Button")
        if not probe.is_enabled():
            raise RuntimeError("probe button disabled")
        probe.invoke()
        time.sleep(1)
        status = find_by_suffix(window, "progressStatusLabel", "Text")
        log_view = find_by_suffix(window, "logPlainTextEdit", "Edit")
        log_text = wait_log_contains(log_view, "● 在线", 30, also_status=status)
        print("PROBE ONLINE", flush=True)
        window.capture_as_image().save(evidence / "probe_online.png")

        if not args.do_flash:
            print("PROBE-ONLY OK", flush=True)
            return 0

        # --- step 2: flash ---
        start_button = find_by_suffix(window, "startFlashButton", "Button")
        if not start_button.is_enabled():
            raise RuntimeError("start flash button disabled")
        start_button.invoke()
        time.sleep(2)
        deadline = time.time() + 360
        final = ""
        while time.time() < deadline:
            if not app.is_process_running():
                raise RuntimeError("UI process exited during flashing")
            st = status.window_text()
            text = log_view.get_value()
            final = st
            print(f"STATUS {st}", flush=True)
            if "刷写成功" in st or "刷写成功" in text[-2000:]:
                break
            if "刷写失败" in st or "刷写失败" in text[-2000:]:
                raise RuntimeError(f"flash failed: {st}")
            time.sleep(2)
        else:
            raise TimeoutError(f"flash timeout: {final}")

        time.sleep(2)
        window.capture_as_image().save(evidence / "flash_pass.png")
        report = newest_after(logs_dir, "report_*.html", started)
        if report:
            shutil.copy2(report, evidence / "flash_report.html")
        trace = newest_after(logs_dir, "trace_*_app.asc", started)
        if trace:
            shutil.copy2(trace, evidence / "flash_trace.asc")
        exec_log = newest_after(logs_dir, "execution_*.log", 0)
        if exec_log:
            shutil.copy2(exec_log, evidence / "execution.log")
        print(f"FLASH PASS: {final}", flush=True)
        return 0
    except Exception as error:
        try:
            window.capture_as_image().save(evidence / "fail.png")
        except Exception:
            pass
        print(f"FAIL: {error}", file=sys.stderr)
        return 2
    finally:
        try:
            window.close()
            time.sleep(2)
            if app.is_process_running():
                app.kill()
        except Exception:
            pass
        exec_log = newest_after(logs_dir, "execution_*.log", 0)
        if exec_log:
            shutil.copy2(exec_log, evidence / "execution.log")
        trace = newest_after(logs_dir, "trace_*_app.asc", started)
        if trace:
            shutil.copy2(trace, evidence / "flash_trace.asc")


if __name__ == "__main__":
    raise SystemExit(main())
