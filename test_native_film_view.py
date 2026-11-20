from pathlib import Path
import ctypes
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent


class UiToastV1(ctypes.Structure):
    _fields_ = [
        ('magic', ctypes.c_uint32),
        ('seq', ctypes.c_uint32),
        ('kind', ctypes.c_uint32),
        ('duration_ms', ctypes.c_uint32),
        ('text', ctypes.c_char * 64),
    ]


class UiStatusV1(ctypes.Structure):
    _fields_ = [
        ('magic', ctypes.c_uint32),
        ('seq', ctypes.c_uint32),
        ('applied', ctypes.c_uint32),
        ('scanned', ctypes.c_uint32),
        ('max_files', ctypes.c_uint32),
        ('custom_charo', ctypes.c_uint32),
        ('active_jobs', ctypes.c_uint32),
    ]


class NativeFilmViewTests(unittest.TestCase):
    def test_native_enum_input_render_handoff_and_settings(self):
        self.assertTrue((ROOT/'camera/film_native_view.c').exists(),
                        'Native film view is not implemented')
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory)/'view-tests'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-g', '-O1', '-fsanitize=address,undefined',
                            str(ROOT/'camera/test_film_native_view.c'),
                            str(ROOT/'camera/film_native_view.c'),
                            str(ROOT/'camera/film_menu.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.assertIn('NATIVE_FILM_VIEW_OK', result.stdout)

    def test_v31_corner_toast_hud_and_1120_abi(self):
        headers = {
            'dlfcn.h': '#define RTLD_DEFAULT ((void *)0)\n#define RTLD_NOW 2\n#define RTLD_NOLOAD 4\nvoid *dlopen(const char *,int);\nvoid *dlsym(void *,const char *);\nint dlclose(void *);\n',
            'stdio.h': '#include <stddef.h>\nint snprintf(char *,size_t,const char *,...);\n',
            'stdlib.h': '#include <stddef.h>\nvoid *calloc(size_t,size_t);\n',
            'string.h': '#include <stddef.h>\nvoid *memcpy(void *,const void *,size_t);\nvoid *memset(void *,int,size_t);\nchar *strcpy(char *,const char *);\n',
            'sys/syscall.h': '#define SYS_gettid 224\n#define SYS_clock_gettime 263\n',
            'time.h': '#define CLOCK_MONOTONIC 1\nstruct timespec {long tv_sec,tv_nsec;};\n',
            'unistd.h': 'long model_syscall(long,...);\n#define syscall model_syscall\n',
        }
        model_c = r'''
#include <stdarg.h>
#include "native_film_ui_trial3.c"
unsigned model_now_ms=1000;
int model_layer_is_active=0;
char model_toast_label[64];
char model_texts[8][64];
static unsigned char menu_marker[64],ticker_marker[64];
static int model_key(void *p,int key,void *data) {(void)p;(void)key;(void)data;return 0;}
static void model_position(const void *p,unsigned char n,short *x,short *y) {(void)p;(void)n;*x=32767;*y=32767;}
static int model_state(const void *p,unsigned char a,unsigned char b) {(void)p;(void)a;(void)b;return 6;}
static void model_layer(void *p,int active) {(void)p;model_layer_is_active=active;}
static void model_animation(void *p,int active) {(void)p;(void)active;}
static void model_unary(void *p) {(void)p;}
static void model_place(void *p,const float *xy) {(void)p;(void)xy;}
static void model_size(void *p,float w,float h) __attribute__((pcs("aapcs")));
static void model_size(void *p,float w,float h) {(void)p;(void)w;(void)h;}
static void model_color(void *p,const unsigned char *c) {(void)p;(void)c;}
static void model_text(void *p,const char *s) {
    if(p==ui_toast.label && s)snprintf(model_toast_label,sizeof(model_toast_label),"%s",s);
    for(unsigned i=0;i<8;i++) {
        if(p==ui.labels[i] && s)snprintf(model_texts[i],sizeof(model_texts[i]),"%s",s);
    }
}
const char *model_header_text(void) { return model_texts[0]; }
const char *model_footer_text(void) { return model_texts[7]; }
const char *model_row_text(unsigned row) { return row<6?model_texts[row+1]:""; }
void model_open_home(void) { ui.view.menu.visible=1; ui.view.menu.page=FILM_MENU_HOME; draw(0,&ui.view.menu); }
void model_open_details(void) { ui.view.menu.visible=1; ui.view.menu.page=FILM_MENU_DETAILS; draw(0,&ui.view.menu); }
long model_syscall(long n, ...) {
    if(n==SYS_gettid)return 380;
    if(n==SYS_clock_gettime) {
        va_list ap;
        va_start(ap, n);
        int clk = va_arg(ap, int);
        struct timespec *t = va_arg(ap, struct timespec *);
        va_end(ap);
        if(clk==CLOCK_MONOTONIC && t) {
            t->tv_sec=(long)(model_now_ms/1000u);
            t->tv_nsec=(long)((model_now_ms%1000u)*1000000u);
            return 0;
        }
    }
    return 0;
}
static void *model_get_key_inst(void) { return ui.keys; }
void model_init(void) {
    memset(&ui,0,sizeof(ui));
    memset(&ui_theme,0,sizeof(ui_theme));
    memset(&ui_toast,0,sizeof(ui_toast));
    struct film_settings initial={0,0,2,1};
    const struct film_native_view_ops ops={visibility,draw,settings};
    film_native_view_init(&ui.view,&ops,0,&initial);
    ui.system=ui_sys_stub;ui.keys=(void *)0x59150;get_key_instance_fn=model_get_key_inst;
    ui.tid=380;ui.running=ui.attached=1;
    ui.layer=menu_marker;ui.ticker=ui.ticker_layer=ticker_marker;
    ui.key=model_key;ui.mouse_position=model_position;ui.mouse_state=model_state;
    ui.layer_active=model_layer;ui.set_animate=model_animation;ui.move_last=model_unary;
    ui.set_position=model_place;ui.panel_size=ui.label_size=model_size;
    ui.set_color=model_color;ui.set_text=model_text;
    for(unsigned i=0;i<8;i++)ui.labels[i]=(void *)(uintptr_t)(0x3000u+i);
    ui_toast.bg=(void *)0x2001;
    ui_toast.accent=(void *)0x2002;
    ui_toast.label=(void *)0x2003;
    model_now_ms=1000;
    model_layer_is_active=0;
    model_toast_label[0]=0;
    for(unsigned i=0;i<8;i++)model_texts[i][0]=0;
}
void model_tick(unsigned dt) {
    model_now_ms += dt;
    animate(ui.ticker);
}
int model_layer_active(void) { return model_layer_is_active; }
int model_toast_visible(void) { return ui_toast.visible; }
const char *model_toast_text(void) { return model_toast_label; }
'''
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            inc = d / 'inc'
            (inc / 'sys').mkdir(parents=True)
            for name, content in headers.items():
                (inc / name).write_text(content, encoding='utf-8')
            for name in ('film_menu.h', 'film_native_view.h', 'ui_trial_trace.h'):
                (inc / name).write_text((ROOT / 'camera' / name).read_text(encoding='utf-8'), encoding='utf-8')
            src = (ROOT / 'camera/native_film_ui_trial3.c').read_text(encoding='utf-8')
            body = model_c.replace('#include "native_film_ui_trial3.c"', src)
            body = body.replace('__attribute__((pcs("aapcs")))', '')
            preamble = (
                'static unsigned char ui_sys_stub[4096];\n'
                '#define _Static_assert(cond, msg)\n'
            )
            (d / 'model.c').write_text(preamble + body, encoding='utf-8')
            so_file = d / 'toast_model.so'
            subprocess.run([
                'cc', '-std=c11', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function',
                '-DA6400_UI_TRIAL_MS=0', '-DA6400_UI_DISPLAY_MS=20000',
                '-I', str(inc), '-I', str(d),
                str(d / 'model.c'),
                str(ROOT / 'camera/film_native_view.c'),
                str(ROOT / 'camera/film_menu.c'),
                '-o', str(so_file),
            ], check=True)
            lib = ctypes.CDLL(str(so_file))
            lib.model_init()
            lib.model_toast_text.restype = ctypes.c_char_p
            lib.model_header_text.restype = ctypes.c_char_p
            lib.model_footer_text.restype = ctypes.c_char_p
            lib.model_row_text.restype = ctypes.c_char_p

            toast = UiToastV1.in_dll(lib, 'a6400_ui_toast_v1')
            status = UiStatusV1.in_dll(lib, 'a6400_ui_status_v1')
            self.assertEqual(toast.magic, 0x54534F54)
            self.assertEqual(status.magic, 0x53544156)
            self.assertEqual(lib.model_toast_visible(), 0)

            toast.kind = 1
            toast.duration_ms = 3500
            toast.text = '處理中: portra_400...'.encode('utf-8')
            toast.seq = 1
            lib.model_tick(20)
            self.assertEqual(lib.model_toast_visible(), 1)
            self.assertEqual(lib.model_layer_active(), 1)
            self.assertEqual(lib.model_toast_text().decode('utf-8'), '處理中: portra_400...')

            lib.model_tick(3600)
            self.assertEqual(lib.model_toast_visible(), 0)
            self.assertEqual(lib.model_layer_active(), 0)

            status.applied = 7
            status.scanned = 42
            status.max_files = 1024
            status.active_jobs = 0
            status.seq = 1
            lib.model_open_home()
            self.assertIn('V3.9 SSLF', lib.model_header_text().decode('utf-8'))
            self.assertIn('已轉:7 (42/1024)', lib.model_header_text().decode('utf-8'))
            lib.model_open_details()
            self.assertEqual(lib.model_footer_text().decode('utf-8'), '佇列:42/1024 | 1.7MP')

            status.active_jobs = 2
            status.seq = 2
            lib.model_tick(20)
            self.assertIn('轉檔:2 已轉:7', lib.model_header_text().decode('utf-8'))

    def test_v39_multi_charo_sound_cycling_and_shutter_ids(self):
        headers = {
            'dlfcn.h': '#define RTLD_DEFAULT ((void *)0)\n#define RTLD_NOW 2\n#define RTLD_NOLOAD 4\nvoid *dlopen(const char *,int);\nvoid *dlsym(void *,const char *);\nint dlclose(void *);\n',
            'stdio.h': '#include <stddef.h>\nint snprintf(char *,size_t,const char *,...);\n',
            'stdlib.h': '#include <stddef.h>\nvoid *calloc(size_t,size_t);\n',
            'string.h': '#include <stddef.h>\nvoid *memcpy(void *,const void *,size_t);\nvoid *memset(void *,int,size_t);\nchar *strcpy(char *,const char *);\n',
            'sys/syscall.h': '#define SYS_gettid 224\n#define SYS_clock_gettime 263\n',
            'time.h': '#define CLOCK_MONOTONIC 1\nstruct timespec {long tv_sec,tv_nsec;};\n',
            'unistd.h': 'long model_syscall(long,...);\n#define syscall model_syscall\n',
        }
        model_c = r'''
#include <stdarg.h>
#include "native_film_ui_trial3.c"
unsigned model_now_ms=1000;
int model_touch=6,model_x=32767,model_y=32767,model_shutter=0;
unsigned model_beep_calls=0;
int model_last_beep_id=-1;
char model_row3_buf[64];
static unsigned char menu_marker[64],ticker_marker[64];
static int model_key(void *p,int key,void *data) {
    (void)p;(void)data;
    return key==0xa7?model_shutter:0;
}
static void model_position(const void *p,unsigned char n,short *x,short *y) {
    (void)p;(void)n;*x=(short)model_x;*y=(short)model_y;
}
static int model_state(const void *p,unsigned char a,unsigned char b) {
    (void)p;(void)a;(void)b;return model_touch;
}
static void model_layer(void *p,int active) {(void)p;(void)active;}
static void model_animation(void *p,int active) {(void)p;(void)active;}
static void model_unary(void *p) {(void)p;}
static void model_place(void *p,const float *xy) {(void)p;(void)xy;}
static void model_size(void *p,float w,float h) __attribute__((pcs("aapcs")));
static void model_size(void *p,float w,float h) {(void)p;(void)w;(void)h;}
static void model_color(void *p,const unsigned char *c) {(void)p;(void)c;}
static void model_text(void *p,const char *s) {
    if(p==ui.labels[4] && s)snprintf(model_row3_buf,sizeof(model_row3_buf),"%s",s);
}
static void model_beep(void *p,int sound_id) {
    (void)p;++model_beep_calls;model_last_beep_id=sound_id;
}
long model_syscall(long n, ...) {
    if(n==SYS_gettid)return 380;
    if(n==SYS_clock_gettime) {
        va_list ap;va_start(ap,n);
        int clk=va_arg(ap,int);
        struct timespec *t=va_arg(ap,struct timespec *);
        va_end(ap);
        if(clk==CLOCK_MONOTONIC && t) {
            t->tv_sec=(long)(model_now_ms/1000u);
            t->tv_nsec=(long)((model_now_ms%1000u)*1000000u);
            return 0;
        }
    }
    return 0;
}
static void *model_get_key_inst(void) { return ui.keys; }
void model_init(void) {
    memset(&ui,0,sizeof(ui));
    memset(&ui_theme,0,sizeof(ui_theme));
    memset(&ui_toast,0,sizeof(ui_toast));
    struct film_settings initial={0,0,2,1};
    const struct film_native_view_ops ops={visibility,draw,settings};
    film_native_view_init(&ui.view,&ops,0,&initial);
    ui.system=ui_sys_stub;ui.keys=(void *)0x59150;get_key_instance_fn=model_get_key_inst;
    ui.tid=380;ui.running=ui.attached=1;
    ui.layer=menu_marker;ui.ticker=ui.ticker_layer=ticker_marker;
    ui.key=model_key;ui.mouse_position=model_position;ui.mouse_state=model_state;
    ui.layer_active=model_layer;ui.set_animate=model_animation;ui.move_last=model_unary;
    ui.set_position=model_place;ui.panel_size=ui.label_size=model_size;
    ui.set_color=model_color;ui.set_text=model_text;
    for(unsigned i=0;i<8;i++)ui.labels[i]=(void *)(uintptr_t)(0x3000u+i);
    ui_theme.beep_play=model_beep;
    ui_theme.shutter_sound_enabled=1;
    model_now_ms=1000;model_beep_calls=0;model_last_beep_id=-1;model_row3_buf[0]=0;
    ui.view.menu.visible=1;ui.view.menu.page=FILM_MENU_HOME;draw(0,&ui.view.menu);
}
void model_tick(unsigned dt,int touch,int x,int y,int shutter) {
    model_now_ms+=dt;model_touch=touch;model_x=x;model_y=y;model_shutter=shutter;
    animate(ui.ticker);
}
const char *model_row3(void) { return model_row3_buf; }
unsigned model_beeps(void) { return model_beep_calls; }
int model_beep_id(void) { return model_last_beep_id; }
void model_redraw(void) { draw(0,&ui.view.menu); }
void model_open_home(void) { ui.view.menu.visible=1; ui.view.menu.page=FILM_MENU_HOME; draw(0,&ui.view.menu); }
'''
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            inc = d / 'inc'
            (inc / 'sys').mkdir(parents=True)
            for name, content in headers.items():
                (inc / name).write_text(content, encoding='utf-8')
            for name in ('film_menu.h', 'film_native_view.h', 'ui_trial_trace.h'):
                (inc / name).write_text((ROOT / 'camera' / name).read_text(encoding='utf-8'), encoding='utf-8')
            src = (ROOT / 'camera/native_film_ui_trial3.c').read_text(encoding='utf-8')
            body = model_c.replace('#include "native_film_ui_trial3.c"', src).replace('__attribute__((pcs("aapcs")))', '')
            preamble = 'static unsigned char ui_sys_stub[4096];\n#define _Static_assert(cond, msg)\n'
            (d / 'model.c').write_text(preamble + body, encoding='utf-8')
            so_file = d / 'multi_charo.so'
            subprocess.run([
                'cc', '-std=c11', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                '-DA6400_UI_TRIAL_MS=0', '-DA6400_UI_DISPLAY_MS=20000',
                '-DA6400_CHARO_SHUTTER=1', '-DA6400_CHARO_MULTI=1',
                '-I', str(inc), '-I', str(d),
                str(d / 'model.c'), str(ROOT / 'camera/film_native_view.c'), str(ROOT / 'camera/film_menu.c'),
                '-o', str(so_file),
            ], check=True)
            lib = ctypes.CDLL(str(so_file))
            lib.model_row3.restype = ctypes.c_char_p
            lib.model_init()
            status = UiStatusV1.in_dll(lib, 'a6400_ui_status_v1')

            def tap_row3():
                lib.model_tick(500, 5, 200, 338, 0)
                lib.model_tick(20, 6, 32767, 32767, 0)

            def fire_shutter():
                lib.model_tick(500, 6, 32767, 32767, 1)
                lib.model_tick(20, 6, 32767, 32767, 0)
                lib.model_open_home()
                lib.model_tick(20, 6, 32767, 32767, 0)

            self.assertEqual(lib.model_row3().decode('utf-8'), '快門聲: 日文 Charo')
            lib.model_tick(20, 6, 32767, 32767, 0)  # establish touch_ready=1 without pressing
            fire_shutter()
            self.assertEqual((lib.model_beeps(), lib.model_beep_id()), (1, 0x33))

            tap_row3()
            self.assertEqual(lib.model_row3().decode('utf-8'), '快門聲: Bling Bling')
            self.assertEqual((lib.model_beeps(), lib.model_beep_id()), (2, 0x34))
            status.custom_charo = 1
            lib.model_redraw()
            self.assertEqual(lib.model_row3().decode('utf-8'), '快門聲: SD自訂')
            status.custom_charo = 0
            lib.model_redraw()
            self.assertEqual(lib.model_row3().decode('utf-8'), '快門聲: Bling Bling')
            fire_shutter()
            self.assertEqual((lib.model_beeps(), lib.model_beep_id()), (3, 0x34))

            tap_row3()
            self.assertEqual(lib.model_row3().decode('utf-8'), '快門聲: iPhone 快門')
            self.assertEqual((lib.model_beeps(), lib.model_beep_id()), (4, 0x35))
            fire_shutter()
            self.assertEqual((lib.model_beeps(), lib.model_beep_id()), (5, 0x35))

            tap_row3()
            self.assertEqual(lib.model_row3().decode('utf-8'), '快門聲: 關閉')
            self.assertEqual(lib.model_beeps(), 5)
            fire_shutter()
            self.assertEqual(lib.model_beeps(), 5)

            tap_row3()
            self.assertEqual(lib.model_row3().decode('utf-8'), '快門聲: 日文 Charo')
            self.assertEqual((lib.model_beeps(), lib.model_beep_id()), (6, 0x33))

    def test_v39_simplified_chinese_ui_labels(self):
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            inc = d / 'inc'
            inc.mkdir()
            for name in ('film_menu.h', 'film_native_view.h'):
                (inc / name).write_text((ROOT / 'camera' / name).read_text(encoding='utf-8'), encoding='utf-8')
            so_file = d / 'view_sc.so'
            subprocess.run([
                'cc', '-std=c11', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                '-DA6400_UI_LANG_SC=1', '-DA6400_CHARO_SHUTTER=1',
                '-I', str(inc),
                str(ROOT / 'camera/film_native_view.c'),
                str(ROOT / 'camera/film_menu.c'),
                '-o', str(so_file),
            ], check=True)
            lib = ctypes.CDLL(str(so_file))

            class FilmSettings(ctypes.Structure):
                _fields_ = [('enabled', ctypes.c_int), ('stock', ctypes.c_int), ('grain', ctypes.c_int), ('halation', ctypes.c_int)]

            class FilmMenu(ctypes.Structure):
                _fields_ = [
                    ('settings', FilmSettings),
                    ('visible', ctypes.c_int), ('page', ctypes.c_int), ('selected', ctypes.c_int),
                    ('last_sample', ctypes.c_uint), ('hold_started', ctypes.c_uint),
                    ('have_sample', ctypes.c_int), ('released', ctypes.c_int), ('pending', ctypes.c_int), ('fired', ctypes.c_int),
                    ('touch_ready', ctypes.c_int), ('touching', ctypes.c_int), ('touch_row', ctypes.c_int), ('touch_cancelled', ctypes.c_int),
                ]

            menu = FilmMenu()
            init_settings = FilmSettings(0, 0, 2, 1)
            lib.film_menu_init(ctypes.byref(menu), ctypes.byref(init_settings))
            menu.visible = 1
            buf = ctypes.create_string_buffer(64)
            x, y, w, h = ctypes.c_int(), ctypes.c_int(), ctypes.c_int(), ctypes.c_int()

            def row_text(page, row):
                menu.page = page
                rc = lib.film_native_view_row(ctypes.byref(menu), row, buf, 64,
                                              ctypes.byref(x), ctypes.byref(y), ctypes.byref(w), ctypes.byref(h))
                self.assertEqual(rc, 1)
                return buf.value.decode('utf-8')

            self.assertEqual(row_text(0, 0), 'LUT 滤镜: 关闭')
            self.assertEqual(row_text(0, 1), '胶片风格: Portra 400')
            self.assertEqual(row_text(0, 4), '胶片细节: 颗粒中 / 光晕开')
            self.assertEqual(row_text(0, 2), '关闭')
            self.assertEqual(row_text(3, 0), '颗粒强度: 标准')
            self.assertEqual(row_text(3, 1), '胶片光晕: 开启')
            self.assertEqual(row_text(3, 2), '恢复细节默认 (标准/开)')


if __name__ == '__main__':
    unittest.main(verbosity=2)
