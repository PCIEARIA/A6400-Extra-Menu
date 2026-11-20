"""Regression tests for V3.9 SSLF tooling, hook builder, deployer, bootstrap, and JPEG worker."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent / 'tools'))
sys.path.insert(0, str(Path(__file__).resolve().parent / 'host-tests'))
import build_autoexec_hook as hook_builder
import pmca_deploy_v3_clite as deployer


class FakeDev:
    def __init__(self, script):
        self.script = list(script)
        self.current = []
        self.writes = []

    def writeTerminal(self, data):
        self.writes.append(data)
        if data != b'\x03\n' and self.script:
            self.current = list(self.script.pop(0))

    def readTerminal(self):
        return self.current.pop(0) if self.current else b''


class FakeCam:
    def __init__(self, script):
        self.dev = FakeDev(script)


class V39ToolsTests(unittest.TestCase):
    def test_autoexec_hook_stub_contains_tmpfs_fallback_and_err_log(self):
        asm = hook_builder.ASM_TEMPLATE
        self.assertIn('str_tmpfs', asm)
        self.assertIn('str_tmp_bt', asm)
        self.assertIn('size=2048k', asm)
        self.assertIn('/tmp/a6400-hook-err.log', asm)
        self.assertIn('mov r7, #21', asm)
        self.assertIn('cmn r0, #17', asm)
        self.assertIn('mov r1, #512', asm)
        self.assertIn('add r1, r1, #65', asm)
        self.assertNotIn('mov r1, #577', asm)

    def test_patch_libak8975_rejects_rodata_overrun_and_injects_dt_init(self):
        rodata_off = 0xB88
        dyn_off = 0xC00
        shstr_off = 0xC80
        shoff = 0xD00
        shstr = b'\x00.text\x00.rodata\x00.dynamic\x00.shstrtab\x00'
        buf = bytearray(shoff + 5 * 40)
        buf[0:4] = b'\x7fELF'
        struct.pack_into('<I', buf, 0x20, shoff)
        struct.pack_into('<HHH', buf, 0x2E, 40, 5, 4)
        struct.pack_into('<IIIIII', buf, shoff + 1 * 40, 1, 1, 6, 0x988, 0x988, 0x200)
        struct.pack_into('<IIIIII', buf, shoff + 2 * 40, 7, 1, 2, rodata_off, rodata_off, 0x20)
        struct.pack_into('<IIIIII', buf, shoff + 3 * 40, 15, 6, 3, dyn_off, dyn_off, 0x40)
        struct.pack_into('<IIIIII', buf, shoff + 4 * 40, 24, 3, 0, 0, shstr_off, len(shstr))
        buf[shstr_off:shstr_off + len(shstr)] = shstr

        payload = b'\x00\xf0\x20\xe3' * 16
        patched = hook_builder.patch_libak8975(bytes(buf), payload)
        self.assertEqual(patched[0x988:0x988 + len(payload)], payload)
        dt_tag, dt_val = struct.unpack_from('<II', patched, dyn_off)
        self.assertEqual((dt_tag, dt_val), (12, 0x988))

        with self.assertRaises(ValueError):
            hook_builder.patch_libak8975(bytes(buf), b'A' * 0x200)

    def test_deployer_commands_stay_under_180_chars_and_retry_rc_none(self):
        cmds = []
        cmds.extend(deployer.build_backup_steps())
        cmds.extend(deployer.build_install_steps(True, [
            ('a6400-charo-bind.elf', '755'),
            ('a6400-charo-shutter.pcm', '644'),
            ('a6400-charo-connect.pcm', '644'),
            ('a6400-charo-push.pcm', '644'),
        ]))
        cmds.extend(deployer.build_rollback_steps())
        cmds.extend(cmd for _, cmd in deployer.build_diag_commands())
        cmds.extend(deployer.build_upload_chunks('A' * 400))
        self.assertTrue(cmds)
        for c in cmds:
            self.assertLessEqual(len(c), deployer.MAX_SHELL_CMD_LEN, f'Command too long ({len(c)}): {c}')

        install = '\n'.join(deployer.build_install_steps(True, []))
        self.assertIn('/etc/a6400-autoexec.so', install)
        self.assertNotIn('/tmp/a6400-autoexec.so', install)
        self.assertNotIn('\\etc', install)
        self.assertNotIn('\\tmp', install)

        cam = FakeCam([[b'> '], [b'__PMCA_RC:0__\n']])
        out, rc = deployer.run_cmd(cam, 'echo ok', timeout=0.05, fast=True, retry_on_none=True)
        self.assertEqual(rc, 0, out)
        self.assertGreaterEqual(len(cam.dev.writes), 3)
        self.assertIn(b'\x03\n', cam.dev.writes)

        msg = deployer.format_usb_help(RuntimeError('No devices found'), platform='win32')
        self.assertIn('不支援 WinUSB', msg)
        self.assertIn('完全關閉並重開 Zadig', msg)
        self.assertNotIn('若失敗改 WinUSB', msg)
        rb = '\n'.join(deployer.build_rollback_steps())
        self.assertIn('/tmp/a6400-charo-connect.pcm', rb)
        self.assertIn('/tmp/a6400-charo-push.pcm', rb)
        self.assertNotIn('/tmp/a6400-autoexec.so', rb)
        src_deploy = (Path(__file__).resolve().parent / 'tools/pmca_deploy_v3_clite.py').read_text()
        self.assertNotIn('CLEAN_LEGACY_HOOK', src_deploy)

    def test_multi_charo_local_boot_package_optional_slots(self):
        root = Path(__file__).resolve().parent
        sys.path.insert(0, str(root / 'host-tests'))
        import build_film_local_boot_package as pkg_builder
        import build_film_runtime_package as rt_pkg
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            artifacts = []
            data_map = {}
            for role, name in (
                ('ui', 'a6400-film-ui-v3-persist.so'),
                ('menu_job', 'a6400-film-menu-job.elf'),
                ('worker', 'a6400-film-job.elf'),
                ('native', 'a6400-film-native-command.elf'),
                ('ram_job', 'a6400-film-ram-job.sh'),
                ('guard', 'a6400-ui-load-guard.elf'),
                ('check', 'a6400-check-persist.req'),
                ('install', 'a6400-install-persist.req'),
                ('remove', 'a6400-remove-persist.req'),
                ('ui_stop', 'a6400-film-ui-stop.elf'),
                ('capture_daemon', 'a6400-film-capture-daemon.elf'),
            ):
                p = d / name
                data = b'#!/bin/sh\necho ok\n' if role == 'ram_job' else f'dummy-{role}'.encode()
                p.write_bytes(data)
                data_map[role] = data
                artifacts.append({'role': role, 'file': str(p), 'dest': name, 'sha256': hashlib.sha256(data).hexdigest()})
            menu_pins = d / 'menu_pins.json'
            menu_pins.write_text(json.dumps({
                'path': '/tmp_bt/a6400-film-ui-v3-persist.so',
                'sha256': hashlib.sha256(data_map['ui']).hexdigest(),
                'bytes': len(data_map['ui']),
                'fnv': rt_pkg.fnv(data_map['ui']),
                'lifetime_ms': 0,
                'build_defines': {'FILM_SETTINGS_PERSISTENT': 1},
                'ui_bytes': 1120,
                'ui_rva': '0x15544',
            }))
            cap_pins = d / 'cap_pins.json'
            cap_pins.write_text(json.dumps({
                'persistent': True,
                'runtime': '/tmp_bt/a6400-film-capture-daemon.elf',
                'arguments': ['watch'],
                'daemon_sha256': hashlib.sha256(data_map['capture_daemon']).hexdigest(),
                'daemon_bytes': len(data_map['capture_daemon']),
                'script': {
                    'runtime': '/tmp_bt/a6400-film-ram-job.sh',
                    'sha256': hashlib.sha256(data_map['ram_job']).hexdigest(),
                    'bytes': len(data_map['ram_job']),
                    'fnv': rt_pkg.fnv(data_map['ram_job']),
                },
                'settings': {
                    'runtime': '/tmp_bt/a6400-film-menu-job.elf',
                    'sha256': hashlib.sha256(data_map['menu_job']).hexdigest(),
                    'bytes': len(data_map['menu_job']),
                    'fnv': rt_pkg.fnv(data_map['menu_job']),
                },
            }))
            spec = d / 'artifacts.json'
            spec.write_text(json.dumps({
                'schema': 1, 'default_enabled': False, 'capture_persistent': True,
                'menu_pins_file': str(menu_pins), 'capture_pins_file': str(cap_pins),
                'artifacts': artifacts,
            }))
            cdir = d / 'charo'
            cdir.mkdir()
            (cdir / 'a6400-charo-bind.elf').write_bytes(b'\x7fELFbind')
            (cdir / 'a6400-charo-shutter.pcm').write_bytes(b'\x01' * 29760)
            (cdir / 'a6400-charo-connect.pcm').write_bytes(b'\x02' * 24768)
            (cdir / 'a6400-charo-push.pcm').write_bytes(b'\x03' * 18560)
            out = d / 'pkg'
            old = os.environ.get('A6400_CHARO_DIR')
            os.environ['A6400_CHARO_DIR'] = str(cdir)
            try:
                pkg_builder.build(spec, out)
            finally:
                if old is None:
                    os.environ.pop('A6400_CHARO_DIR', None)
                else:
                    os.environ['A6400_CHARO_DIR'] = old
            mf = json.loads((out / 'manifest.json').read_text())
            cam_paths = [e['camera_path'] for e in mf['extra_persistent_files']]
            self.assertEqual(cam_paths, [
                '/tmp/a6400-charo-bind.elf',
                '/tmp/a6400-charo-shutter.pcm',
                '/tmp/a6400-charo-connect.pcm',
                '/tmp/a6400-charo-push.pcm',
            ])
            boot_sh = (out / 'bootstrap-local.sh').read_text()
            self.assertIn('/tmp/a6400-charo-connect.pcm', boot_sh)
            self.assertIn('/tmp/a6400-charo-push.pcm', boot_sh)

    def test_bootstrap_local_template_supports_self_mounted_tmpfs_on_ro_dir(self):
        root = Path(__file__).resolve().parent
        tpl = (root / 'host-tests/film-runtime-v2-templates/bootstrap-local.sh').read_text()
        self.assertIn('mount -t tmpfs -o size=2048k tmpfs "$RAM"', tpl)
        self.assertIn('version=V3.9-SSLF', tpl)

        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            ram = d / 'ram'
            ram.mkdir()
            ram.chmod(0o555)  # Read-only /tmp_bt until fake busybox mount makes it writable
            self.addCleanup(lambda: ram.chmod(0o755) if ram.exists() else None)
            proc = d / 'proc'
            proc.mkdir()
            (proc / 'mounts').write_text('')
            bb = d / 'bb'
            bb.write_text(
                '#!/usr/bin/env python3\n'
                'import os, sys\n'
                'from pathlib import Path\n'
                'cmd = sys.argv[1]; args = sys.argv[2:]\n'
                'if cmd == "mount":\n'
                '    target = Path(args[-1])\n'
                '    target.chmod(0o755)\n'
                '    (target / "mounted_tmpfs").write_text(" ".join(args))\n'
                '    sys.exit(0)\n'
                'os.execvp(cmd, [cmd] + args)\n'
            )
            bb.chmod(0o755)
            tar_dummy = d / 'boot.tar'
            tar_dummy.write_bytes(b'wrong')
            script = (
                tpl.replace('/bin/busybox', str(bb))
                .replace('/tmp_bt', str(ram))
                .replace('/proc', str(proc))
                .replace('/tmp/a6400-film-boot-v15.tar', str(tar_dummy))
                .replace('@@BUNDLE_BYTES@@', '5')
                .replace('@@PACKAGE_ID@@', 'v39testpkg')
                .replace('@@BUNDLE_SHA@@', '0' * 64)
            )
            sh_path = d / 'boot.sh'
            sh_path.write_text(script)
            # Without ae.lock it must mount tmpfs, log begin, and stop cleanly at missing_constructor_lock
            res = subprocess.run(['sh', str(sh_path)], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(res.returncode, 0)
            self.assertTrue((ram / 'mounted_tmpfs').is_file())
            log = (ram / 'a6400-film-boot.log').read_text()
            self.assertIn('FILM_LOCAL_BOOT_BEGIN package=v39testpkg', log)
            self.assertIn('FILM_LOCAL_BOOT_FAIL stage=missing_constructor_lock', log)

    def test_picojpeg_host_job_downsample2_and_bad_input_rejection(self):
        root = Path(__file__).resolve().parent
        cam = root / 'camera'
        vendor = root / 'vendor'
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            objs = []
            for name, src, lang, extra in (
                ('film_jpeg.o', cam / 'film_jpeg.cpp', 'c++', ['-O2', '-std=c++11', '-fno-exceptions', '-fno-rtti']),
                ('author_film.o', cam / 'author_film.c', 'cc', ['-O3', '-std=c11']),
                ('picojpeg.o', vendor / 'picojpeg.c', 'cc', ['-O2', '-std=c11', '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-but-set-variable']),
                ('jpge.o', vendor / 'jpge.cpp', 'c++', ['-O3', '-std=c++11', '-fno-exceptions', '-fno-rtti']),
            ):
                obj = d / name
                subprocess.run([lang, '-DA6400_FILM_HOST', '-DA6400_FILM_JOB', '-Wall', '-Wextra', '-Werror',
                                '-I', str(cam), '-I', str(vendor), *extra, '-c', str(src), '-o', str(obj)],
                               check=True, capture_output=True, text=True)
                objs.append(str(obj))
            binary = d / 'film_job'
            subprocess.run(['c++', *objs, '-o', str(binary)], check=True, capture_output=True, text=True)
            # Compile tiny helper using vendor/jpge.cpp to emit a synthetic 2404x24 JPEG (triggers downsample2)
            gen_cpp = d / 'gen.cpp'
            gen_cpp.write_text(
                '#include <vector>\n'
                '#include "jpge.h"\n'
                'int main(int argc, char **argv) {\n'
                '  int w = 2404, h = 24;\n'
                '  std::vector<unsigned char> rgb(w * h * 3);\n'
                '  for (int y = 0; y < h; ++y)\n'
                '    for (int x = 0; x < w; ++x) {\n'
                '      int i = (y * w + x) * 3;\n'
                '      rgb[i] = (unsigned char)((x * 3 + y * 5) & 255);\n'
                '      rgb[i+1] = (unsigned char)((x + y * 11) & 255);\n'
                '      rgb[i+2] = (unsigned char)((128 + x - y) & 255);\n'
                '    }\n'
                '  jpge::params p; p.m_quality = 90;\n'
                '  return jpge::compress_image_to_jpeg_file(argv[1], w, h, 3, rgb.data(), p) ? 0 : 1;\n'
                '}\n'
            )
            gen_bin = d / 'gen'
            subprocess.run(['c++', '-O2', '-std=c++11', '-I', str(root / 'vendor'),
                            str(gen_cpp), str(root / 'vendor/jpge.cpp'), '-o', str(gen_bin)],
                           check=True, capture_output=True, text=True)
            src = d / 'wide.jpg'
            subprocess.run([str(gen_bin), str(src)], check=True)
            out = d / 'rendered.jpg'
            res = subprocess.run([str(binary), 'kodak_gold', str(src), str(out), '2', '0', '--hires'],
                                 capture_output=True, text=True, timeout=30)
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            self.assertIn('FILM_SOURCE kind=downsample2 source_width=2404 source_height=24 width=1202 height=12', res.stdout)
            self.assertIn('FILM_DONE width=1202 height=12 ', res.stdout)
            raw = out.read_bytes()
            self.assertTrue(raw.startswith(b'\xff\xd8') and raw.endswith(b'\xff\xd9'))

            bad = d / 'bad.jpg'
            bad.write_bytes(b'\xff\xd8\xff\xd9')
            r_bad = subprocess.run([str(binary), 'kodak_gold', str(bad), str(d / 'bad_out.jpg'), '2', '0', '--hires'],
                                   capture_output=True, text=True)
            self.assertNotEqual(r_bad.returncode, 0)
            self.assertFalse((d / 'bad_out.jpg').exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
