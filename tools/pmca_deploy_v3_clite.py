#!/usr/bin/env python3
"""Deploy, inspect (--diag), or restore (--rollback) V3.9 SSLF on Sony A6400 via Sony-PMCA-RE."""
import argparse
import base64
import hashlib
import io
import json
import posixpath
import re
import sys
import tarfile
import time
from datetime import datetime
from pathlib import Path

MAX_SHELL_CMD_LEN = 180
HOOK_CAMERA_PATH = "/etc/a6400-autoexec.so"
LEGACY_HOOK_PATH = "/tmp/a6400-autoexec.so"


def run_cmd(cam, cmd, timeout=15.0, fast=False, retry_on_none=True):
    dev = cam.dev
    attempts = 2 if retry_on_none else 1
    out_bytes = b""
    rc = None
    pat = re.compile(rb"__PMCA_RC:(\d+)__")
    for attempt in range(attempts):
        if attempt > 0:
            # Recover from a potentially truncated line or open quote before retrying once.
            try:
                dev.writeTerminal(b"\x03\n")
                time.sleep(0.15)
                dev.readTerminal()
            except Exception:
                pass
        dev.writeTerminal(f"{cmd}; printf '\\n__PMCA_RC:%s__\\n' $?\n".encode())
        end = time.monotonic() + timeout
        out = bytearray()
        rc = None
        while time.monotonic() < end:
            try:
                chunk = dev.readTerminal()
            except Exception:
                break
            if chunk:
                out.extend(chunk)
                m = pat.search(out)
                if m:
                    rc = int(m.group(1))
                    if not fast:
                        time.sleep(0.03)
                        try:
                            more = dev.readTerminal()
                            if more:
                                out.extend(more)
                        except Exception:
                            pass
                    break
            else:
                time.sleep(0.005)
        out_bytes = bytes(out)
        if rc is not None:
            break
    return out_bytes.decode("latin1", errors="replace"), rc


def must_cmd(cam, cmd, label, timeout=15.0):
    out, rc = run_cmd(cam, cmd, timeout=timeout, retry_on_none=True)
    if rc is None:
        raise RuntimeError(
            f"{label} failed rc=None (終端命令可能被截斷或超時，已重試一次仍無回應): {out[-400:]}"
        )
    if rc != 0:
        raise RuntimeError(f"{label} failed rc={rc}: {out[-400:]}")
    return out


def atomic_copy_steps(src, dst, mode):
    parent = posixpath.dirname(dst)
    name = posixpath.basename(dst)
    tmp_dst = f"{parent}/{name}.new"
    return [
        f'/bin/busybox cp "{src}" "{tmp_dst}"',
        f'/bin/busybox chmod {mode} "{tmp_dst}" && /bin/busybox sync',
        f'/bin/busybox mv "{tmp_dst}" "{dst}" && /bin/busybox sync',
    ]


def build_backup_steps():
    targets = (
        "/etc/ld.so.preload",
        "/tmp/autoexec.sh",
        "/tmp/a6400-film-boot-v15.tar",
        HOOK_CAMERA_PATH,
    )
    steps = []
    for t in targets:
        steps.append(f'P="{t}"')
        steps.append('test ! -f "$P" || test -f "$P.pre-v3c-backup" || /bin/busybox cp -p "$P" "$P.pre-v3c-backup"')
    steps.append("/bin/busybox sync")
    return steps


def build_install_steps(has_hook, extra_entries):
    d = "/tmp_bt/v3c_up"
    steps = []
    if has_hook:
        steps.extend(atomic_copy_steps(f"{d}/new-autoexec.so", HOOK_CAMERA_PATH, "755"))
    for item in extra_entries:
        name, mode = item if isinstance(item, tuple) else (item, "755" if str(item).endswith(".elf") else "644")
        steps.extend(atomic_copy_steps(f"{d}/{name}", f"/tmp/{name}", mode))
    steps.extend(atomic_copy_steps(f"{d}/new-boot.tar", "/tmp/a6400-film-boot-v15.tar", "644"))
    steps.extend(atomic_copy_steps(f"{d}/new-autoexec.sh", "/tmp/autoexec.sh", "755"))
    if has_hook:
        steps.extend([
            f'printf "{HOOK_CAMERA_PATH}\\n" > /etc/ld.so.preload.new',
            '/bin/busybox chmod 644 /etc/ld.so.preload.new && /bin/busybox sync',
            '/bin/busybox mv /etc/ld.so.preload.new /etc/ld.so.preload && /bin/busybox sync',
        ])
    steps.append('/bin/busybox rm -f /tmp/disable_hook && /bin/busybox sync')
    return steps


def build_rollback_steps():
    steps = []
    for path in ("/etc/ld.so.preload", "/tmp/autoexec.sh", "/tmp/a6400-film-boot-v15.tar", HOOK_CAMERA_PATH):
        steps.append(f'P="{path}"')
        steps.append('test -f "$P.pre-v3c-backup" && /bin/busybox mv "$P.pre-v3c-backup" "$P" || /bin/busybox rm -f "$P"')
    for extra in (
        "/tmp/a6400-charo-bind.elf",
        "/tmp/a6400-charo-shutter.pcm",
        "/tmp/a6400-charo-connect.pcm",
        "/tmp/a6400-charo-push.pcm",
        "/tmp/a6400-hook-err.log",
    ):
        steps.append(f'/bin/busybox rm -f "{extra}"')
    steps.append("/bin/busybox sync")
    return steps


def build_diag_commands():
    return [
        ("VERSION", "/bin/busybox uname -a"),
        ("MOUNTS", "/bin/busybox cat /proc/mounts"),
        ("PRELOAD", "/bin/busybox cat /etc/ld.so.preload 2>&1 || true"),
        ("FILES", f'/bin/busybox ls -la {HOOK_CAMERA_PATH} /tmp/autoexec.sh /tmp/a6400-* 2>&1'),
        ("SHA256", f'/bin/busybox sha256sum {HOOK_CAMERA_PATH} /tmp/autoexec.sh /tmp/a6400-film-boot-v15.tar 2>&1'),
        ("HOOK_ERR", "/bin/busybox cat /tmp/a6400-hook-err.log 2>&1 || true"),
        ("BOOT_LOG", "/bin/busybox cat /tmp_bt/a6400-film-boot.log 2>&1 || true"),
        ("RUNTIME_LOG", "/bin/busybox tail -n 80 /tmp_bt/a6400-film-capture.runtime.log 2>&1 || true"),
        ("MANIFEST", "/bin/busybox cat /tmp_bt/a6400-film-capture.manifest 2>&1 || true"),
        ("JOB_LOG", "/bin/busybox tail -n 60 /tmp_bt/a6400-film-capture.job.log 2>&1 || true"),
    ]


def build_upload_chunks(b64_text, step=120):
    return [f'printf %s {b64_text[i:i + step]} >> /tmp_bt/v3c_up/up.b64' for i in range(0, len(b64_text), step)]


def detect_windows_usb_driver_hint():
    if sys.platform != "win32":
        return ""
    try:
        import usb.core
        devs = list(usb.core.find(find_all=True, idVendor=0x054C) or [])
        if devs:
            pids = ", ".join(f"054C:{d.idProduct:04X}" for d in devs)
            return (
                f"偵測到 Sony USB 裝置 ({pids})，但 PMCA 無法以 libusb-win32 開啟；"
                "請確認不是 WinUSB 驅動（PMCA **不支援 WinUSB**，請改用 libusb-win32）。"
            )
    except Exception:
        pass
    return ""


def format_usb_help(exc=None, platform=None, driver_hint=""):
    plat = platform or sys.platform
    lines = []
    if plat == "win32":
        lines.append("\n[!] 尚未完成連線：相機未進入 Mass Storage / Service 模式，或 Windows 驅動不是 libusb-win32。")
        if driver_hint:
            lines.append(f"診斷：{driver_hint}")
        lines.extend([
            "請依以下步驟確認後重跑：",
            "  1. 相機切為 Mass Storage（大容量儲存）模式並以 USB 線接妥（勿選 MTP/PC遙控）",
            "  2. 以系統管理員執行 Zadig -> Options -> List All Devices",
            "  3. 注意勿選到筆電內建視訊攝影機（WebCam / HD Camera）！請選 ILCE-6400",
            "  4. 右側驅動**必須選 libusb-win32**（PMCA 不支援 WinUSB，選 WinUSB 會顯示 No devices found）-> Replace Driver",
            "  5. 切換服務模式（054C:0336）後，Zadig 不會自動刷新清單：**請完全關閉並重開 Zadig** 再確認 054C:0336 也為 libusb-win32",
        ])
    else:
        lines.append("\n[!] 尚未完成連線：請確認相機已切為 Mass Storage 模式並接上 USB（Linux 若無權限請加 sudo）。")
    if exc is not None:
        exc_str = str(exc)
        if "WinUSB" in exc_str or "GenericUsbDriver" in exc_str:
            lines.append("提示：偵測到 WinUSB / 非 libusb 驅動錯誤 — PMCA 僅支援 libusb-win32。")
        lines.append(f"原始錯誤：{type(exc).__name__}: {exc}")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package_dir", nargs="?", default="", help="Path to local-boot-package directory")
    parser.add_argument("--pmca-dir", default="", help="Path to Sony-PMCA-RE checkout")
    parser.add_argument(
        "--install-hook",
        default="",
        help=f"Optional path to a6400-autoexec.so (installed to {HOOK_CAMERA_PATH})",
    )
    parser.add_argument("--rollback", action="store_true", help="Restore .pre-v3c-backup files or remove hook cleanly")
    parser.add_argument("--diag", action="store_true", help="Collect camera diagnostics without modifying flash")
    parser.add_argument("--diag-out", default="", help="Optional output path for --diag report")
    args = parser.parse_args()

    if args.rollback and args.diag:
        parser.error("Cannot combine --rollback and --diag")
    if not args.rollback and not args.diag and not args.package_dir:
        parser.error("package_dir is required unless --rollback or --diag is specified")

    if args.pmca_dir:
        sys.path.insert(0, str(Path(args.pmca_dir).resolve()))
    from pmca.commands.usb import senserShellCommand

    if args.diag:
        diag_ok = False

        def collect_diag(cam):
            nonlocal diag_ok
            cam.setTerminalEnable(True)
            time.sleep(0.6)
            try:
                cam.dev.readTerminal()
            except Exception:
                pass
            sections = []
            try:
                for label, cmd in build_diag_commands():
                    out, rc = run_cmd(cam, cmd, timeout=15.0)
                    sections.append(f"=== {label} (rc={rc}) ===\n{out.strip()}\n")
                report = "\n".join(sections)
                print(report, flush=True)
                out_file = Path(args.diag_out) if args.diag_out else Path(
                    f"a6400-diag-{datetime.now().strftime('%Y%m%d-%H%M%S')}.txt"
                )
                out_file.write_text(report, encoding="utf-8")
                print(f"V3_CLITE_DIAG_OK saved={out_file}", flush=True)
                diag_ok = True
            finally:
                try:
                    cam.setTerminalEnable(False)
                except Exception:
                    pass

        try:
            senserShellCommand(driverName="libusb", complete=collect_diag)
        except Exception as exc:
            print(format_usb_help(exc, driver_hint=detect_windows_usb_driver_hint()), file=sys.stderr)
            sys.exit(1)
        if not diag_ok:
            print(format_usb_help(driver_hint=detect_windows_usb_driver_hint()), file=sys.stderr)
            sys.exit(1)
        return

    if args.rollback:
        rb_ok = False

        def rollback(cam):
            nonlocal rb_ok
            cam.setTerminalEnable(True)
            time.sleep(0.6)
            try:
                cam.dev.readTerminal()
            except Exception:
                pass
            try:
                for idx, step in enumerate(build_rollback_steps()):
                    must_cmd(cam, step, f"ROLLBACK_{idx}", timeout=20.0)
                print("V3_CLITE_ROLLBACK_OK", flush=True)
                rb_ok = True
            finally:
                try:
                    cam.setTerminalEnable(False)
                except Exception:
                    pass

        try:
            senserShellCommand(driverName="libusb", complete=rollback)
        except Exception as exc:
            print(format_usb_help(exc, driver_hint=detect_windows_usb_driver_hint()), file=sys.stderr)
            sys.exit(1)
        if not rb_ok:
            print(format_usb_help(driver_hint=detect_windows_usb_driver_hint()), file=sys.stderr)
            sys.exit(1)
        return

    pkg = Path(args.package_dir).resolve()
    manifest = json.loads((pkg / "manifest.json").read_text())
    boot_sh = pkg / "bootstrap-local.sh"
    boot_tar = pkg / "persistent/a6400-film-boot-v15.tar"
    boot_sh_sha = hashlib.sha256(boot_sh.read_bytes()).hexdigest()
    boot_tar_sha = hashlib.sha256(boot_tar.read_bytes()).hexdigest()
    hook_path = Path(args.install_hook).resolve() if args.install_hook else None
    hook_sha = hashlib.sha256(hook_path.read_bytes()).hexdigest() if hook_path else ""

    extra_items = []
    for row in manifest.get("extra_persistent_files", []):
        ep = pkg / row["local_path"]
        data = ep.read_bytes()
        sha = hashlib.sha256(data).hexdigest()
        if sha != row["sha256"]:
            raise RuntimeError(f"local extra file SHA mismatch: {ep}")
        extra_items.append((ep, posixpath.basename(row["camera_path"]), sha, row.get("mode", "644")))

    entry_shas = []
    with tarfile.open(boot_tar, "r") as tf:
        for m in tf.getmembers():
            if m.isfile():
                data = tf.extractfile(m).read()
                entry_shas.append((m.name, hashlib.sha256(data).hexdigest()))

    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tgz:
        tgz.add(boot_sh, arcname="new-autoexec.sh")
        tgz.add(boot_tar, arcname="new-boot.tar")
        if hook_path:
            tgz.add(hook_path, arcname="new-autoexec.so")
        for ep, ename, _, _ in extra_items:
            tgz.add(ep, arcname=ename)
    tgz_bytes = buf.getvalue()
    tgz_sha = hashlib.sha256(tgz_bytes).hexdigest()

    deploy_ok = False

    def deploy(cam):
        nonlocal deploy_ok
        cam.setTerminalEnable(True)
        time.sleep(0.6)
        try:
            cam.dev.readTerminal()
        except Exception:
            pass
        try:
            if not hook_path:
                pre = must_cmd(cam, "/bin/busybox cat /etc/ld.so.preload", "CHECK_PRELOAD")
                if HOOK_CAMERA_PATH in pre:
                    must_cmd(cam, f'/bin/busybox ls -la "{HOOK_CAMERA_PATH}"', "CHECK_HOOK_FILE")
                elif LEGACY_HOOK_PATH in pre:
                    must_cmd(cam, f'/bin/busybox ls -la "{LEGACY_HOOK_PATH}"', "CHECK_LEGACY_HOOK")
                else:
                    raise RuntimeError(
                        f"Existing /etc/ld.so.preload + {HOOK_CAMERA_PATH} hook required (or pass --install-hook)"
                    )

            for idx, step in enumerate(build_backup_steps()):
                must_cmd(cam, step, f"BACKUP_{idx}")

            b64 = base64.b64encode(tgz_bytes).decode("ascii")
            must_cmd(cam, "/bin/busybox umount /tmp_bt/v3c_up 2>/dev/null||true; rm -rf /tmp_bt/v3c_up && mkdir -p /tmp_bt/v3c_up", "PREP_UP_DIR")
            must_cmd(cam, "/bin/busybox mount -t tmpfs -o size=2048k tmpfs /tmp_bt/v3c_up", "MOUNT_UP_TMPFS")
            must_cmd(cam, ": > /tmp_bt/v3c_up/up.b64", "PREP_UP_FILE")
            chunks = build_upload_chunks(b64, step=120)
            total = len(chunks)
            print(f"Uploading {len(tgz_bytes)} B ({total} chunks)...", flush=True)
            for idx, chunk_cmd in enumerate(chunks):
                _, rc = run_cmd(cam, chunk_cmd, timeout=5.0, fast=True, retry_on_none=True)
                if rc != 0:
                    raise RuntimeError(f"upload chunk {idx}/{total} failed rc={rc}")

            must_cmd(
                cam,
                '/bin/busybox base64 -d /tmp_bt/v3c_up/up.b64 > /tmp_bt/v3c_up/up.tgz && rm -f /tmp_bt/v3c_up/up.b64',
                "B64_DECODE",
                timeout=20.0,
            )
            tgz_out = must_cmd(cam, '/bin/busybox sha256sum /tmp_bt/v3c_up/up.tgz', "VERIFY_TGZ")
            if tgz_sha not in tgz_out:
                raise RuntimeError("uploaded tgz SHA mismatch")
            must_cmd(
                cam,
                '/bin/busybox tar -xzf /tmp_bt/v3c_up/up.tgz -C /tmp_bt/v3c_up',
                "UNTAR_UP",
                timeout=20.0,
            )
            must_cmd(cam, 'rm -f /tmp_bt/v3c_up/up.tgz', "CLEAN_UP_TGZ")
            must_cmd(cam, '/bin/sh -n /tmp_bt/v3c_up/new-autoexec.sh', "SYNTAX_AUTOEXEC")

            verify_targets = [
                ("/tmp_bt/v3c_up/new-autoexec.sh", boot_sh_sha),
                ("/tmp_bt/v3c_up/new-boot.tar", boot_tar_sha),
            ]
            if hook_path:
                verify_targets.append(("/tmp_bt/v3c_up/new-autoexec.so", hook_sha))
            for _, ename, esha, _ in extra_items:
                verify_targets.append((f"/tmp_bt/v3c_up/{ename}", esha))
            for vpath, vsha in verify_targets:
                vout = must_cmd(cam, f'/bin/busybox sha256sum "{vpath}"', f"VERIFY_{posixpath.basename(vpath)}")
                if vsha not in vout:
                    raise RuntimeError(f"uploaded SHA mismatch for {vpath}")

            for name, exp_sha in entry_shas:
                must_cmd(cam, 'mkdir -p /tmp_bt/v3c_up/ex', f"MKEX_{name}")
                must_cmd(
                    cam,
                    f'/bin/busybox tar -xf /tmp_bt/v3c_up/new-boot.tar -C /tmp_bt/v3c_up/ex "{name}"',
                    f"EX_{name}",
                )
                r = must_cmd(cam, f'/bin/busybox sha256sum "/tmp_bt/v3c_up/ex/{name}"', f"VERIFY_{name}")
                must_cmd(cam, 'rm -rf /tmp_bt/v3c_up/ex', f"RMEX_{name}")
                if exp_sha not in r:
                    raise RuntimeError(f"entry {name} SHA mismatch")

            extra_entries = [(ename, emode) for _, ename, _, emode in extra_items]
            for idx, step in enumerate(build_install_steps(bool(hook_path), extra_entries)):
                must_cmd(cam, step, f"INSTALL_{idx}", timeout=20.0)

            final_targets = [
                ("/tmp/autoexec.sh", boot_sh_sha),
                ("/tmp/a6400-film-boot-v15.tar", boot_tar_sha),
            ]
            if hook_path:
                final_targets.append((HOOK_CAMERA_PATH, hook_sha))
            for _, ename, esha, _ in extra_items:
                final_targets.append((f"/tmp/{ename}", esha))
            for fpath, fsha in final_targets:
                fout = must_cmd(cam, f'/bin/busybox sha256sum "{fpath}"', f"FINAL_{posixpath.basename(fpath)}")
                if fsha not in fout:
                    raise RuntimeError(f"final SHA mismatch for {fpath}")

            if hook_path:
                preload_out = must_cmd(cam, '/bin/busybox cat /etc/ld.so.preload', "FINAL_PRELOAD")
                if HOOK_CAMERA_PATH not in preload_out:
                    raise RuntimeError(f"/etc/ld.so.preload missing {HOOK_CAMERA_PATH}")

            must_cmd(cam, '/bin/busybox umount /tmp_bt/v3c_up 2>/dev/null||true; rm -rf /tmp_bt/v3c_up && /bin/busybox sync', "CLEAN_UP_STAGE")
            print(f"V3_CLITE_DEPLOY_OK package={manifest['package_id']} version=V3.9-SSLF", flush=True)
            deploy_ok = True
        finally:
            try:
                cam.setTerminalEnable(False)
            except Exception:
                pass

    try:
        senserShellCommand(driverName="libusb", complete=deploy)
    except Exception as exc:
        print(format_usb_help(exc, driver_hint=detect_windows_usb_driver_hint()), file=sys.stderr)
        sys.exit(1)

    if not deploy_ok:
        print(format_usb_help(driver_hint=detect_windows_usb_driver_hint()), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
