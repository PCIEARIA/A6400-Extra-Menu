/* Exact-version RAM trial. Invoked on the natural Appframework thread.
 * All drawing uses owned Sony widgets/layer. No framebuffer writes, input
 * consumption, firmware/configuration patch or independent SD mount.
 */
#define _GNU_SOURCE
#include "film_native_view.h"
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "ui_trial_trace.h"
#ifdef A6400_INPUT_DIAGNOSTIC
#include "ui_input_journal.h"
static volatile struct ui_input_journal a6400_ui_input_journal_v1;
#endif

#if defined(A6400_UI_TOUCH_CLOSE_DIAGNOSTIC) && !defined(A6400_UI_RENDER_DIAGNOSTIC)
#error "touch-close diagnostic requires the bounded render diagnostic"
#endif

#if defined(A6400_UI_COMBO_DIAGNOSTIC) && defined(A6400_UI_RENDER_DIAGNOSTIC)
#error "combo and automatic-render diagnostics are mutually exclusive"
#endif

#if defined(A6400_UI_SETTINGS_DIAGNOSTIC) && \
    (A6400_UI_SETTINGS_DIAGNOSTIC != 1 || !defined(A6400_UI_COMBO_DIAGNOSTIC))
#error "settings diagnostic requires the combo diagnostic"
#endif

typedef void *(*key_inst_fn)(void);
static key_inst_fn get_key_instance_fn;
#ifndef A6400_UI_TRIAL_MS
# ifdef A6400_UI_COMBO_DIAGNOSTIC
#  define A6400_UI_TRIAL_MS 180000u
# else
#  define A6400_UI_TRIAL_MS 30000u
# endif
#endif
#if A6400_UI_TRIAL_MS != 0 && (A6400_UI_TRIAL_MS < 30000 || A6400_UI_TRIAL_MS > 600000)
#error "A6400_UI_TRIAL_MS must be 0 (persistent) or between 30000 and 600000 milliseconds"
#endif
#define TRIAL_MS A6400_UI_TRIAL_MS
#ifndef A6400_UI_INSTANCE_SUFFIX
#define A6400_UI_INSTANCE_SUFFIX ""
#endif
#ifndef A6400_UI_DISPLAY_MS
#define A6400_UI_DISPLAY_MS 5000u
#endif
#if A6400_UI_DISPLAY_MS < 1 || A6400_UI_DISPLAY_MS > 20000 || (TRIAL_MS != 0 && A6400_UI_DISPLAY_MS >= TRIAL_MS)
#error "A6400_UI_DISPLAY_MS must be between 1 and 20000 milliseconds"
#endif
static volatile struct ui_trial_trace a6400_ui_trial_trace;
#define TRACE (&a6400_ui_trial_trace)
typedef void (*ctor_fn)(void *);
typedef void (*unary_fn)(void *);
typedef void (*bool_fn)(void *,int);
typedef void (*layer_id_fn)(void *,const uint32_t *);
/* The firmware is base AAPCS soft-float. Compiler/system headers are hard-float:
 * annotate the only native calls with float arguments explicitly. */
typedef void (*size_fn)(void *,float,float) __attribute__((pcs("aapcs")));
typedef void (*position_fn)(void *,const float *);
typedef void (*text_fn)(void *,const char *);
typedef void (*color_fn)(void *,const unsigned char *);
typedef uint32_t (*id_fn)(const char *);
typedef void *(*create_layer_fn)(void *,const uint32_t *);
typedef void (*mouse_position_fn)(const void *,unsigned char,short *,short *);
typedef int (*mouse_state_fn)(const void *,unsigned char,unsigned char);
typedef int (*key_fn)(void *,int,void *);
typedef void (*font_fn)(void *,uint32_t);
#ifdef A6400_CHARO_SHUTTER
typedef void (*beep_play_fn)(void *,int);
#define A6400_CHARO_SHUTTER_KEY 0xa7
#define A6400_CHARO_SOUND_ID 0x34
#define A6400_CHARO_CONNECT_SOUND_ID 0x33
#define A6400_CHARO_PUSH_SOUND_ID 0x35
#ifndef A6400_CHARO_DELAY_MS
#define A6400_CHARO_DELAY_MS 0u
#endif
#ifndef A6400_CHARO_COOLDOWN_MS
#define A6400_CHARO_COOLDOWN_MS 450u
#endif
static int charo_sound_id(int mode) {
#if defined(A6400_CHARO_MULTI) && A6400_CHARO_MULTI
    if(mode==1)return A6400_CHARO_CONNECT_SOUND_ID;
    if(mode==2)return A6400_CHARO_SOUND_ID;
    if(mode==3)return A6400_CHARO_PUSH_SOUND_ID;
    return 0;
#else
    return mode?A6400_CHARO_SOUND_ID:0;
#endif
}
static const char *charo_mode_label(int mode,int custom) {
#if defined(A6400_UI_LANG_SC)
#if defined(A6400_CHARO_MULTI) && A6400_CHARO_MULTI
    if(mode==1)return "快门声: 日文 Charo";
    if(mode==2)return custom?"快门声: SD自定义":"快门声: Bling Bling";
    if(mode==3)return "快门声: iPhone 快门";
    return "快门声: 关闭";
#else
    if(!mode)return "快门声: 关闭";
    return custom?"快门声: Charo (SD自定义)":"快门声: Charo (开启)";
#endif
#else
#if defined(A6400_CHARO_MULTI) && A6400_CHARO_MULTI
    if(mode==1)return "快門聲: 日文 Charo";
    if(mode==2)return custom?"快門聲: SD自訂":"快門聲: Bling Bling";
    if(mode==3)return "快門聲: iPhone 快門";
    return "快門聲: 關閉";
#else
    if(!mode)return "快門聲: 關閉";
    return custom?"快門聲: Charo (SD自訂)":"快門聲: Charo (開啟)";
#endif
#endif
}
#endif

static struct a6400_ui_state {
    struct film_native_view view;
    void *system,*layer,*ticker,*background,*panels[6],*labels[8],*keys;
    void *ticker_layer;
    uint32_t ticker_layer_id;
    uint32_t layer_id,tid,started,last_tick,ticks;
    uint32_t opened_ms;
    int ever_opened;
#ifdef A6400_UI_TOUCH_CLOSE_DIAGNOSTIC
    int last_touch;
#endif
#ifdef A6400_UI_COMBO_DIAGNOSTIC
    int last_menu,last_zoom,last_other;
#elif defined(A6400_UI_TOUCH_CLOSE_DIAGNOSTIC)
    uint32_t snapshot_abi_padding[2];
#else
    uint32_t snapshot_abi_padding[3];
#endif
    uintptr_t vtable[42];
    int attached,running;
    ctor_fn widget_ctor,panel_ctor,label_ctor;
    layer_id_fn set_layer;
    position_fn set_position;
    size_fn panel_size,label_size;
    text_fn set_text;
    color_fn set_color;
    bool_fn layer_active,layer_clear,set_animate;
    unary_fn move_last;
    id_fn make_id;
    create_layer_fn create_layer;
    mouse_position_fn mouse_position;
    mouse_state_fn mouse_state;
    key_fn key;
    font_fn set_font;
    char texts[8][80];
} ui;
_Static_assert(sizeof(ui)==1120,"UI snapshot ABI must remain 1120 bytes");
_Static_assert(offsetof(struct a6400_ui_state,system)==0x68,"ui.system ABI mismatch");
_Static_assert(offsetof(struct a6400_ui_state,tid)==0xc0,"ui.tid ABI mismatch");
_Static_assert(offsetof(struct a6400_ui_state,started)==0xc4,"ui.started ABI mismatch");
_Static_assert(offsetof(struct a6400_ui_state,last_tick)==0xc8,"ui.last_tick ABI mismatch");
_Static_assert(offsetof(struct a6400_ui_state,attached)==0x18c,"ui.attached ABI mismatch");
_Static_assert(offsetof(struct a6400_ui_state,running)==0x190,"ui.running ABI mismatch");

/* Owned control word, separate from the pinned 1120-byte UI snapshot ABI.
 * A matching external manager may write 1; only the natural UI callback
 * stops owned layers and publishes 2. Retained widgets keep this SO loaded. */
volatile unsigned a6400_ui_stop_requested_v1;
__attribute__((visibility("default")))
volatile uint32_t a6400_ui_hires_v1 = 0u;

/* Owned corner-toast ABI, separate from the 1120-byte UI snapshot ABI. */
struct a6400_ui_toast_v1 {
    uint32_t magic;
    uint32_t seq;
    uint32_t kind;
    uint32_t duration_ms;
    char text[64];
};
__attribute__((visibility("default")))
volatile struct a6400_ui_toast_v1 a6400_ui_toast_v1 = { 0x54534f54u, 0u, 0u, 0u, "" };

struct a6400_ui_status_v1 {
    uint32_t magic;       /* 0x53544156u 'STAV' */
    uint32_t seq;
    uint32_t applied;
    uint32_t scanned;
    uint32_t max_files;
    uint32_t custom_charo;
    uint32_t active_jobs;
};
__attribute__((visibility("default")))
volatile struct a6400_ui_status_v1 a6400_ui_status_v1 = { 0x53544156u, 0u, 0u, 0u, 1024u, 0u, 0u };

static struct {
    void *bg, *accent, *label;
    uint32_t seq, until_ms;
    int visible;
    char text[64];
} ui_toast;

/* Decorative theme panels kept outside `ui` so the 1120-byte snapshot ABI stays exact. */
static struct {
    void *card_border, *card_panel, *header_accent, *header_divider, *page_btn;
    void *row_accents[6];
    uint32_t status_seq;
#ifdef A6400_CHARO_SHUTTER
    beep_play_fn beep_play;
    int shutter_sound_enabled;
    int last_shutter_key;
    int shutter_pending;
    uint32_t shutter_arm_ms;
    uint32_t last_shutter_beep_ms;
#endif
} ui_theme;

static uint32_t now_ms(void) {
    struct timespec t;
    if(syscall(SYS_clock_gettime,CLOCK_MONOTONIC,&t))return 0;
    return (uint32_t)((uint64_t)t.tv_sec*1000u+(unsigned)t.tv_nsec/1000000u);
}
static void input_snapshot(unsigned stage) {
#ifdef A6400_INPUT_DIAGNOSTIC
    void *owned[UI_INPUT_OWNED]={0};
    owned[0]=ui.background;
    for(unsigned i=0;i<6;i++)owned[i+1]=ui.panels[i];
    for(unsigned i=0;i<8;i++)owned[i+7]=ui.labels[i];
    owned[15]=ui.ticker;
    if(ui.layer)owned[16]=*(void **)((unsigned char *)ui.layer+0x38);
    if(ui.ticker_layer)owned[17]=*(void **)((unsigned char *)ui.ticker_layer+0x38);
    ui_input_capture(&a6400_ui_input_journal_v1,stage,ui.system,owned,
        UI_INPUT_OWNED,now_ms(),(uint32_t)syscall(SYS_gettid));
#else
    (void)stage;
#endif
}
static void record(const char *event,int a,int b) {
    ui_trial_record(TRACE,event,now_ms(),(uint32_t)syscall(SYS_gettid),
                    (uint32_t)a,(uint32_t)b,ui.ticks,
                    (unsigned)ui.running | ((unsigned)ui.view.menu.visible<<1));
}
static void *api(const char *name,uint32_t offset) {
    (void)offset;
    void *p=dlsym(RTLD_DEFAULT,name);
    if(!p){record("API_MISMATCH",(int)offset,0);return 0;}
    return p;
}
static __attribute__((noinline)) int resolve(void) {
#define API(member,type,name,offset) do {ui.member=(type)api(name,offset);if(!ui.member)return -1;}while(0)
    API(widget_ctor,ctor_fn,"_ZN2ux6wgtsys6WidgetC1Ev",0x5ed5ad);
    API(panel_ctor,ctor_fn,"_ZN9GEN_PanelC1Ev",0x4149ad);
    API(label_ctor,ctor_fn,"_ZN9GEN_LabelC1Ev",0x3c8d41);
    API(set_layer,layer_id_fn,"_ZN2ux6wgtsys6Widget10setLayerIDERKNS_4core2IDE",0x5ed50d);
    API(set_position,position_fn,"_ZN2ux6wgtsys6Widget11setLocalPosERKNS_4core7Vector2E",0x5ed45d);
    API(panel_size,size_fn,"_ZN9GEN_Panel7setSizeEff",0x414def);
    API(label_size,size_fn,"_ZN9GEN_Label7setSizeEff",0x3c9381);
    API(set_text,text_fn,"_ZN9GEN_Label13setTextDirectEPKc",0x3c88cf);
    API(set_font,font_fn,"_ZN9GEN_Label7setFontEj",0x3c89af);
    API(set_color,color_fn,"_ZN9GEN_Panel14setColorDirectERKN6stella6CColorE",0x4148e5);
    API(layer_active,bool_fn,"_ZN2ux6wgtsys5Layer9setActiveEb",0x5ef12f);
    API(layer_clear,bool_fn,"_ZN2ux6wgtsys5Layer8setClearEb",0x5ef159);
    API(set_animate,bool_fn,"_ZN2ux6wgtsys10WidgetBase10setAnimateEb",0x606951);
    API(move_last,unary_fn,"_ZN2ux6wgtsys9LayerBase8moveLastEv",0x601331);
    API(make_id,id_fn,"_ZN2ux4core2ID5genIDEPKc",0x63dcd9);
    API(create_layer,create_layer_fn,"_ZN2ux6wgtsys12WidgetSystem11createLayerERKNS_4core2IDE",0x5f0f49);
    API(mouse_position,mouse_position_fn,"_ZNK2ux6wgtsys12WidgetSystem24getMousePosInFramebufferEhRsS2_",0x5f1019);
    API(mouse_state,mouse_state_fn,"_ZNK2ux6wgtsys12WidgetSystem13getMouseStateEhh",0x5f1045);
    API(key,key_fn,"_ZN10InfraInput20GetPhyKeyStatusCacheEiP21_InfraInputPhyKeyData",0x860fa9);
    get_key_instance_fn=(key_inst_fn)api("_ZN10InfraInput11GetInstanceEv",0x860385);
    if(!get_key_instance_fn)return -1;
#ifdef A6400_CHARO_SHUTTER
    ui_theme.beep_play=(beep_play_fn)api("_ZN13AppWidgetBase8beepPlayEi",0);
    if(!ui_theme.beep_play)return -1;
#endif
#undef API
    return 0;
}
static void place(void *widget,int label,int x,int y,int w,int h) {
    const float xy[2]={(float)x,(float)y};
    ui.set_position(widget,xy);
    (label?ui.label_size:ui.panel_size)(widget,(float)w,(float)h);
}
static void show_menu_chrome(void) {
    static const unsigned char background[4]={8,10,14,148};
    static const unsigned char card_border[4]={84,92,106,224};
    static const unsigned char card_panel[4]={18,22,29,230};
    static const unsigned char header_accent[4]={234,106,28,255};
    static const unsigned char header_divider[4]={58,66,78,200};
    place(ui.background,0,0,0,640,480);ui.set_color(ui.background,background);
    place(ui_theme.card_border,0,54,34,532,412);ui.set_color(ui_theme.card_border,card_border);
    place(ui_theme.card_panel,0,56,36,528,408);ui.set_color(ui_theme.card_panel,card_panel);
    place(ui_theme.header_accent,0,56,36,528,4);ui.set_color(ui_theme.header_accent,header_accent);
    place(ui_theme.header_divider,0,76,86,488,1);ui.set_color(ui_theme.header_divider,header_divider);
}
static void hide_menu_widgets(void) {
    static const unsigned char clear_c[4]={0,0,0,0};
    place(ui.background,0,0,0,0,0);ui.set_color(ui.background,clear_c);
    place(ui_theme.card_border,0,0,0,0,0);ui.set_color(ui_theme.card_border,clear_c);
    place(ui_theme.card_panel,0,0,0,0,0);ui.set_color(ui_theme.card_panel,clear_c);
    place(ui_theme.header_accent,0,0,0,0,0);ui.set_color(ui_theme.header_accent,clear_c);
    place(ui_theme.header_divider,0,0,0,0,0);ui.set_color(ui_theme.header_divider,clear_c);
    place(ui_theme.page_btn,0,0,0,0,0);ui.set_color(ui_theme.page_btn,clear_c);
    for(unsigned i=0;i<6;i++){
        place(ui.panels[i],0,0,0,0,0);ui.set_color(ui.panels[i],clear_c);
        place(ui_theme.row_accents[i],0,0,0,0,0);ui.set_color(ui_theme.row_accents[i],clear_c);
    }
    for(unsigned i=0;i<8;i++){
        ui.texts[i][0]=0;
        place(ui.labels[i],1,0,0,0,0);
        ui.set_text(ui.labels[i],ui.texts[i]);
    }
}
static void hide_toast_widgets(void) {
    static const unsigned char clear_c[4]={0,0,0,0};
    if(!ui_toast.bg)return;
    place(ui_toast.bg,0,0,0,0,0);ui.set_color(ui_toast.bg,clear_c);
    place(ui_toast.accent,0,0,0,0,0);ui.set_color(ui_toast.accent,clear_c);
    ui_toast.text[0]=0;
    place(ui_toast.label,1,0,0,0,0);ui.set_text(ui_toast.label,ui_toast.text);
}
static void visibility(void *unused,int active) {
    (void)unused;
    input_snapshot(active?3:5);
    record(active?"SHOW_BEFORE_MOVE":"HIDE_BEFORE_ACTIVE",active,0);
    if(active){ui.move_last(ui.layer);record("SHOW_AFTER_MOVE",0,0);}
    ui.layer_active(ui.layer,active);
    input_snapshot(active?4:6);
    if(active){ui.opened_ms=now_ms();ui.ever_opened=1;}
#if defined(A6400_UI_COMBO_DIAGNOSTIC) || (TRIAL_MS == 0 && !defined(A6400_UI_RENDER_DIAGNOSTIC))
    else ui.opened_ms=0;
#endif
    record(active?"FILM_OPEN":"FILM_CLOSE",ui.view.menu.settings.enabled,ui.view.menu.settings.stock);
}
static void draw(void *unused,const struct film_menu *menu) {
    (void)unused;
    record("DRAW_BEGIN",menu->page,menu->selected);
    hide_toast_widgets();
    ui_toast.visible=0;
    show_menu_chrome();
    place(ui.labels[0],1,76,48,488,34);
    const unsigned char row_plain[4]={30,36,46,226};
    const unsigned char row_active[4]={58,44,24,236};
    const unsigned char row_pressed[4]={234,114,36,245};
    const unsigned char pill_plain[4]={42,49,62,232};
    const unsigned char pill_pressed[4]={92,104,122,248};
    const unsigned char accent_orange[4]={234,140,42,255};
    const unsigned char accent_gold[4]={212,162,58,220};
    const unsigned char accent_muted[4]={68,76,90,200};
#ifdef A6400_UI_LANG_SC
    if(a6400_ui_status_v1.magic==0x53544156u && a6400_ui_status_v1.seq>0u) {
        ui_theme.status_seq=a6400_ui_status_v1.seq;
        unsigned qmax=a6400_ui_status_v1.max_files?(unsigned)a6400_ui_status_v1.max_files:1024u;
        if(a6400_ui_status_v1.active_jobs>0u) {
            snprintf(ui.texts[0],sizeof(ui.texts[0]),"V3.9 SSLF | %s | 转档:%u 已转:%u",
                     menu->settings.enabled?"开启":"关闭",
                     (unsigned)a6400_ui_status_v1.active_jobs,(unsigned)a6400_ui_status_v1.applied);
        } else {
            snprintf(ui.texts[0],sizeof(ui.texts[0]),"V3.9 SSLF | %s | 已转:%u (%u/%u)",
                     menu->settings.enabled?"开启":"关闭",
                     (unsigned)a6400_ui_status_v1.applied,(unsigned)a6400_ui_status_v1.scanned,qmax);
        }
    } else {
        snprintf(ui.texts[0],sizeof(ui.texts[0]),"A6400 V3.9 SSLF | %s",
                 menu->settings.enabled?"开启":"关闭");
    }
#else
    if(a6400_ui_status_v1.magic==0x53544156u && a6400_ui_status_v1.seq>0u) {
        ui_theme.status_seq=a6400_ui_status_v1.seq;
        unsigned qmax=a6400_ui_status_v1.max_files?(unsigned)a6400_ui_status_v1.max_files:1024u;
        if(a6400_ui_status_v1.active_jobs>0u) {
            snprintf(ui.texts[0],sizeof(ui.texts[0]),"V3.9 SSLF | %s | 轉檔:%u 已轉:%u",
                     menu->settings.enabled?"開啟":"關閉",
                     (unsigned)a6400_ui_status_v1.active_jobs,(unsigned)a6400_ui_status_v1.applied);
        } else {
            snprintf(ui.texts[0],sizeof(ui.texts[0]),"V3.9 SSLF | %s | 已轉:%u (%u/%u)",
                     menu->settings.enabled?"開啟":"關閉",
                     (unsigned)a6400_ui_status_v1.applied,(unsigned)a6400_ui_status_v1.scanned,qmax);
        }
    } else {
        snprintf(ui.texts[0],sizeof(ui.texts[0]),"A6400 V3.9 SSLF | %s",
                 menu->settings.enabled?"開啟":"關閉");
    }
#endif
    ui.set_text(ui.labels[0],ui.texts[0]);
    for(unsigned row=0;row<6;row++) {
        int x=0,y=0,w=0,h=0;
        int used=film_native_view_row(menu,row,ui.texts[row+1],sizeof(ui.texts[0]),&x,&y,&w,&h);
        if(used<=0){ui.texts[row+1][0]=0;w=h=0;}
#ifdef A6400_CHARO_SHUTTER
        else if(menu->page==FILM_MENU_HOME && row==3) {
            int custom=(a6400_ui_status_v1.magic==0x53544156u && a6400_ui_status_v1.custom_charo);
            snprintf(ui.texts[row+1],sizeof(ui.texts[0]),"%s",
                     charo_mode_label(ui_theme.shutter_sound_enabled,custom));
        }
#endif
        record("ROW_PANEL_GEOMETRY",row,used);
        place(ui.panels[row],0,x,y,w,h);
        int stocks_page=(menu->page==FILM_MENU_STOCKS || menu->page==FILM_MENU_STOCKS_P2);
        int details_page=(menu->page==FILM_MENU_DETAILS);
        int stock_idx=(menu->page==FILM_MENU_STOCKS_P2?5:0)+(int)row;
        int is_dismiss=(menu->page==FILM_MENU_HOME && row==2) || (stocks_page && row==5) || (details_page && row==5);
        int is_active=(menu->page==FILM_MENU_HOME && row==0 && menu->settings.enabled) ||
                      (menu->page==FILM_MENU_HOME && row==4 && (menu->settings.grain!=2 || !menu->settings.halation)) ||
#ifdef A6400_CHARO_SHUTTER
                      (menu->page==FILM_MENU_HOME && row==3 && ui_theme.shutter_sound_enabled) ||
#endif
                      (details_page && ((row==0 && menu->settings.grain>0) || (row==1 && menu->settings.halation))) ||
                      (stocks_page && row<5 && stock_idx==menu->settings.stock);
        const unsigned char *panel_color=row_plain;
        if(menu->selected==(int)row)panel_color=is_dismiss?pill_pressed:row_pressed;
        else if(is_dismiss)panel_color=pill_plain;
        else if(is_active)panel_color=row_active;
        record("ROW_PANEL_COLOR",row,0);
        ui.set_color(ui.panels[row],panel_color);
        if(used>0 && !is_dismiss) {
            place(ui_theme.row_accents[row],0,x,y,5,h);
            const unsigned char *stripe=is_active?accent_orange:
                (menu->page==FILM_MENU_HOME && (row==1 || row==4)?accent_gold:accent_muted);
            ui.set_color(ui_theme.row_accents[row],stripe);
        } else {
            place(ui_theme.row_accents[row],0,0,0,0,0);
        }
        record("ROW_LABEL_GEOMETRY",row,0);
        if(is_dismiss)place(ui.labels[row+1],1,x+20,y+6,w>40?w-40:0,h>12?h-12:0);
        else place(ui.labels[row+1],1,x+22,y+4,w>38?w-38:0,h>8?h-8:0);
        record("ROW_TEXT_BEFORE",row,0);
        ui.set_text(ui.labels[row+1],ui.texts[row+1]);
        record("ROW_TEXT_AFTER",row,0);
    }
    int px=0,py=0,pw=0,ph=0;
    if(film_native_view_row(menu,6,ui.texts[7],sizeof(ui.texts[0]),&px,&py,&pw,&ph)>0) {
        place(ui_theme.page_btn,0,px,py,pw,ph);
        ui.set_color(ui_theme.page_btn,menu->selected==6?pill_pressed:pill_plain);
        place(ui.labels[7],1,px+16,py+6,pw>32?pw-32:0,ph>12?ph-12:0);
    } else if(menu->page==FILM_MENU_HOME) {
        place(ui_theme.page_btn,0,76,386,264,42);
        ui.set_color(ui_theme.page_btn,
                     menu->selected==7?pill_pressed:(a6400_ui_hires_v1?row_active:pill_plain));
#ifdef A6400_UI_LANG_SC
        snprintf(ui.texts[7],sizeof(ui.texts[0]),"%s",
                 a6400_ui_hires_v1?"画质: 6MP 高清":"画质: 1.7MP 极速");
#else
        snprintf(ui.texts[7],sizeof(ui.texts[0]),"%s",
                 a6400_ui_hires_v1?"畫質: 6MP 高清":"畫質: 1.7MP 極速");
#endif
        place(ui.labels[7],1,92,392,232,30);
    } else if(menu->page==FILM_MENU_DETAILS) {
        place(ui_theme.page_btn,0,76,386,264,42);
        ui.set_color(ui_theme.page_btn,pill_plain);
        unsigned qmax=(a6400_ui_status_v1.magic==0x53544156u && a6400_ui_status_v1.max_files)
                      ?(unsigned)a6400_ui_status_v1.max_files:1024u;
        unsigned qcur=a6400_ui_status_v1.magic==0x53544156u?(unsigned)a6400_ui_status_v1.scanned:0u;
#ifdef A6400_UI_LANG_SC
        snprintf(ui.texts[7],sizeof(ui.texts[0]),"队列:%u/%u | %s",
                 qcur,qmax,a6400_ui_hires_v1?"6MP":"1.7MP");
#else
        snprintf(ui.texts[7],sizeof(ui.texts[0]),"佇列:%u/%u | %s",
                 qcur,qmax,a6400_ui_hires_v1?"6MP":"1.7MP");
#endif
        place(ui.labels[7],1,92,392,232,30);
    } else {
        place(ui_theme.page_btn,0,0,0,0,0);
        ui.texts[7][0]=0;
        place(ui.labels[7],1,0,0,0,0);
    }
    ui.set_text(ui.labels[7],ui.texts[7]);
    record("FILM_DRAW",menu->page,menu->selected);
}
static void settings(void *unused,const struct film_settings *s) {
    (void)unused;
    /* The persistent reader samples ui.view.menu.settings directly. Keep the
     * callback native and allocation-free; photo work stays in the daemon. */
    record("FILM_SETTINGS_SNAPSHOT",s->enabled,s->stock);
}
static void stop_trial(const char *why) {
    record("STOP_BEGIN",0,0);
    film_native_view_hide(&ui.view);
    record("STOP_MENU_HIDDEN",0,0);
    if(ui.layer)ui.layer_active(ui.layer,0);
    ui_toast.visible=0;
#ifdef A6400_CHARO_SHUTTER
    ui_theme.shutter_pending=0;
#endif
    ui.running=0;
    input_snapshot(7);
    /* setAnimate only changes the flag/counter; retain allocated native nodes. */
    record("STOP_TICKER_BEFORE",0,0);
    if(ui.ticker)ui.set_animate(ui.ticker,0);
    record("STOP_LAYER_BEFORE",0,0);
    if(ui.ticker_layer)ui.layer_active(ui.ticker_layer,0);
    input_snapshot(8);
    record(why,(int)ui.ticks,0);
}
int a6400_ui_stop_v2(void) {
    if(!ui.attached || (uint32_t)syscall(SYS_gettid)!=ui.tid)return -1;
    stop_trial("FILM_EXPLICIT_STOP");return 0;
}
static int stop_old_v1(void) {
    void *old=dlopen("/tmp_bt/a6400-film-ui-v1.so",RTLD_NOW|RTLD_NOLOAD);
    if(!old)return 0;
    uintptr_t entry=(uintptr_t)dlsym(old,"a6400_ui_attach_v1");
    if(!entry){dlclose(old);return -1;}
    uintptr_t base=entry-0x12e0u;
    if(base!=0x10a73000u){dlclose(old);return -2;}
    /* This cleanup is solely for the hash-pinned v1 already observed in RAM.
     * Validate its owned state/vtable before calling native setters on it. */
    uintptr_t *old_ui=(uintptr_t *)(base+0x14088u);
    void *ticker=(void *)old_ui[0x70/4],*layer=(void *)old_ui[0x6c/4];
    if(old_ui[0x68/4]!=(uintptr_t)ui.system ||
       old_ui[0xb8/4]!=ui.tid || old_ui[0x170/4]!=1 || !ticker || !layer ||
       *(uintptr_t *)ticker!=(uintptr_t)old_ui+0xd0u ||
       old_ui[(0xd0+32*4)/4]!=base+0xaa4u) {
        dlclose(old);return -2;
    }
    old_ui[0x174/4]=0;
    ui.set_animate(ticker,0);ui.layer_active(layer,0);
    record("FILM_OLD_V1_STOPPED",(int)(uintptr_t)ticker,0);
    dlclose(old);return 0;
}
static void animate(void *self) {
    if(!ui.running)return;
    if(self!=ui.ticker || (uint32_t)syscall(SYS_gettid)!=ui.tid) {
        ui.running=0;record("FILM_BAD_TICK_CONTEXT",0,0);return;
    }
    if(a6400_ui_stop_requested_v1==1u) {
        stop_trial("FILM_STOP_REQUESTED");
        __sync_synchronize();a6400_ui_stop_requested_v1=2u;return;
    }
    uint32_t now=now_ms();
    if(!now){stop_trial("FILM_CLOCK_ERROR");return;}
    if(!ui.started) {ui.started=now;record("FILM_TICK_READY",(int)(uintptr_t)self,0);}
    ++ui.ticks;
#if TRIAL_MS != 0
    if(now-ui.started>=TRIAL_MS) {
        stop_trial("FILM_TRIAL_STOPPED");return;
    }
#endif
    if(ui.opened_ms && now-ui.opened_ms>=A6400_UI_DISPLAY_MS) {
#if defined(A6400_UI_COMBO_DIAGNOSTIC) || (TRIAL_MS == 0 && !defined(A6400_UI_RENDER_DIAGNOSTIC))
        /* Hide only: the separate active ticker still watches the chord. */
        film_native_view_hide(&ui.view);
        record("COMBO_AUTO_HIDE",0,0);return;
#else
        stop_trial("TRIAL_AUTO_HIDE");return;
#endif
    }
#if !defined(A6400_UI_COMBO_DIAGNOSTIC) && !(TRIAL_MS == 0 && !defined(A6400_UI_RENDER_DIAGNOSTIC))
    if(ui.ever_opened && !ui.view.menu.visible) {
        stop_trial("TRIAL_CLOSE_DONE");return;
    }
#endif
    if(now-ui.last_tick<15u)return;
    ui.last_tick=now;
    if(ui_toast.bg && ui.layer) {
        if(ui.view.menu.visible) {
            ui_toast.visible=0;
        } else {
            uint32_t seq=a6400_ui_toast_v1.seq;
            if(seq!=ui_toast.seq && a6400_ui_toast_v1.magic==0x54534f54u) {
                for(unsigned i=0;i<63;i++)ui_toast.text[i]=a6400_ui_toast_v1.text[i];
                ui_toast.text[63]=0;
                uint32_t kind=a6400_ui_toast_v1.kind;
                uint32_t dur=a6400_ui_toast_v1.duration_ms;
                ui_toast.seq=seq;
                if(ui_toast.text[0]) {
                    static const unsigned char bg_c[4]={18,22,29,235};
                    static const unsigned char k1_c[4]={234,158,28,255};
                    static const unsigned char k2_c[4]={46,196,96,255};
                    static const unsigned char k3_c[4]={220,62,62,255};
                    hide_menu_widgets();
                    place(ui_toast.bg,0,24,390,480,36);
                    place(ui_toast.accent,0,24,390,8,36);
                    place(ui_toast.label,1,40,394,456,28);
                    ui.set_color(ui_toast.bg,bg_c);
                    ui.set_color(ui_toast.accent,kind==2?k2_c:kind==3?k3_c:k1_c);
                    ui.set_text(ui_toast.label,ui_toast.text);
                    ui.move_last(ui.layer);
                    ui.layer_active(ui.layer,1);
                    ui_toast.visible=1;
                    ui_toast.until_ms=now+(dur?dur:3000u);
                } else if(ui_toast.visible) {
                    ui.layer_active(ui.layer,0);
                    ui_toast.visible=0;
                }
            }
            if(ui_toast.visible && (int32_t)(now-ui_toast.until_ms)>=0) {
                ui.layer_active(ui.layer,0);
                ui_toast.visible=0;
            }
        }
    }
#ifdef A6400_UI_RENDER_DIAGNOSTIC
    /* Isolate rendering from the entire input state machine. A simulated
     * chord can lose its release latch after a startup dispatch gap and
     * therefore never open. No synthetic events reach Sony. */
    if(!ui.ever_opened && now-ui.started>=1000u) {
        record("RENDER_DIAGNOSTIC_BEGIN",(int)(now-ui.started),0);
        ui.view.menu.visible=1;
        draw(0,&ui.view.menu);
        visibility(0,1);
    }
#ifdef A6400_UI_TOUCH_CLOSE_DIAGNOSTIC
    /* Read-only native getters: no capture, focus, mode or key setters.
     * The enum is 5=pressed, 6=released. Other rows remain inert in this
     * diagnostic; only a complete Close gesture can stop our owned UI. */
    if(ui.view.menu.visible) {
        short x=32767,y=32767;
        ui.mouse_position(ui.system,0,&x,&y);
        int state=ui.mouse_state(ui.system,0,0);
        if(state!=ui.last_touch) {
            record(state==5?"CLOSE_TOUCH_DOWN":state==6?"CLOSE_TOUCH_UP":"CLOSE_TOUCH_INVALID",x,y);
            ui.last_touch=state;
        }
        if(state==5 && film_menu_hit(ui.view.menu.page,x,y)!=2)x=y=32767;
        unsigned change=film_native_view_step(&ui.view,now,0,0,0,state,x,y);
        if(change&FILM_MENU_CLOSED) {
            stop_trial("TRIAL_TOUCH_CLOSE");return;
        }
    }
#endif
    return;
#endif
    if(!get_key_instance_fn || get_key_instance_fn()!=ui.keys){stop_trial("FILM_KEY_INSTANCE_CHANGED");return;}
    int menu=ui.key(ui.keys,0x1ab,0),zoom=ui.key(ui.keys,0x70dc,0),other=0;
    static const int dismiss_keys[]={0x3,0x15,0x30,0x66,0xa7,0x1ac,0x1af,
        0x1f0,0x1f4,0x1f7,0x1f8,0x1f9,0x7095,0x70a9,0x70aa,0x70ab,
        0x70ad,0x70c4,0x10000000,0x10000001,0x10000003,0x10000004};
    for(unsigned i=0;i<sizeof(dismiss_keys)/sizeof(*dismiss_keys);i++)
        if(ui.key(ui.keys,dismiss_keys[i],0)==1)other=1;
    short x=32767,y=32767;
    ui.mouse_position(ui.system,0,&x,&y);
    int touch=ui.mouse_state(ui.system,0,0);
#ifdef A6400_UI_COMBO_DIAGNOSTIC
    /* Like the validated Close-only round, this does not consume native keys
     * or touch. Observed physical actions dismiss our layer; native redraw
     * ordering is not guaranteed because Sony still receives those events.
     * Settings are enabled only in the separate RAM-settings diagnostic. */
    if(ui.view.menu.have_sample && now-ui.view.menu.last_sample>250u)
        record("COMBO_GAP",(int)(now-ui.view.menu.last_sample),
            ui.view.menu.released | (ui.view.menu.pending<<1) |
            (ui.view.menu.fired<<2) | ((menu==1)<<3) |
            ((zoom==1)<<4) | ((other!=0)<<5));
    if(menu!=ui.last_menu || zoom!=ui.last_zoom || other!=ui.last_other) {
        record("COMBO_KEYS",menu,zoom);
        if(other)record("COMBO_OTHER",other,0);
        ui.last_menu=menu;ui.last_zoom=zoom;ui.last_other=other;
    }
#ifndef A6400_UI_SETTINGS_DIAGNOSTIC
    if(touch==5 && film_menu_hit(ui.view.menu.page,x,y)!=2)x=y=32767;
#endif
#endif
    unsigned change=film_native_view_step(&ui.view,now,menu,zoom,other,touch,x,y);
    if(change&FILM_MENU_SETTINGS_CHANGED)record("FILM_TAP",x,y);
    if(change&FILM_MENU_HIRES_TOGGLED) {
        a6400_ui_hires_v1^=1u;
        draw(0,&ui.view.menu);
        record("FILM_HIRES_TOGGLE",(int)a6400_ui_hires_v1,0);
    }
#ifdef A6400_CHARO_SHUTTER
    if(change&FILM_MENU_SOUND_TOGGLED) {
#if defined(A6400_CHARO_MULTI) && A6400_CHARO_MULTI
        ui_theme.shutter_sound_enabled=(ui_theme.shutter_sound_enabled+1)&3;
#else
        ui_theme.shutter_sound_enabled^=1;
#endif
        if(!ui_theme.shutter_sound_enabled)ui_theme.shutter_pending=0;
        draw(0,&ui.view.menu);
        record("CHARO_SOUND_TOGGLE",ui_theme.shutter_sound_enabled,0);
        int preview_id=charo_sound_id(ui_theme.shutter_sound_enabled);
        if(preview_id && ui_theme.beep_play) {
            ui_theme.last_shutter_beep_ms=now;
            ui_theme.beep_play(ui.background,preview_id);
            record("CHARO_MENU_BEEP_PREVIEW",preview_id,(int)now);
        }
    }
    int shutter_down=ui.key(ui.keys,A6400_CHARO_SHUTTER_KEY,0);
    if(shutter_down==1 && ui_theme.last_shutter_key==0 && ui_theme.shutter_sound_enabled) {
        ui_theme.shutter_pending=1;
        ui_theme.shutter_arm_ms=now;
        record("CHARO_SHUTTER_ARMED",charo_sound_id(ui_theme.shutter_sound_enabled),(int)now);
    }
    ui_theme.last_shutter_key=(shutter_down==1)?1:0;
    if(ui_theme.shutter_pending &&
       ui_theme.shutter_sound_enabled &&
       ui_theme.beep_play
#if A6400_CHARO_DELAY_MS > 0
       && (uint32_t)(now-ui_theme.shutter_arm_ms)>=A6400_CHARO_DELAY_MS
#endif
       ) {
        ui_theme.shutter_pending=0;
        int active_id=charo_sound_id(ui_theme.shutter_sound_enabled);
        if(active_id &&
           (!ui_theme.last_shutter_beep_ms ||
            (uint32_t)(now-ui_theme.last_shutter_beep_ms)>=A6400_CHARO_COOLDOWN_MS)) {
            ui_theme.last_shutter_beep_ms=now;
            ui_theme.beep_play(ui.background,active_id);
            record("CHARO_SHUTTER_BEEP",active_id,(int)now);
        }
    }
#endif
    if(ui.view.menu.visible && a6400_ui_status_v1.magic==0x53544156u &&
       a6400_ui_status_v1.seq!=ui_theme.status_seq) {
        draw(0,&ui.view.menu);
    }
    if(ui.ticks%300u==0)record("FILM_ALIVE",(int)ui.ticks,ui.view.menu.visible);
}
static void *make(int label) {
    void *p=calloc(1,label?0x400u:0x200u);
    if(p){
        record(label?"LABEL_CTOR_BEFORE":"PANEL_CTOR_BEFORE",0,0);
        (label?ui.label_ctor:ui.panel_ctor)(p);
        if(label){
            /* The first five IDs are static firmware resources. The native
             * Label's sixth entry was 0x0100061d, a runtime override from a
             * previous boot: it is absent now and the original font resolver
             * rejects it. Retain the constructor's unset sixth entry. */
            static const uint32_t attributes[6]={0x02076000,0x0207a00f,
                0x0207a000,0x02066027,0x02076002,0xffffffff};
            memcpy((unsigned char *)p+0x2b4,attributes,sizeof(attributes));
            record("LABEL_FONT_BEFORE",(int)(uintptr_t)p,0);
            ui.set_font(p,attributes[0]);
            record("LABEL_FONT_AFTER",*(int *)((unsigned char *)p+0x1f4),0);
            if(!*(void **)((unsigned char *)p+0x1f4))return 0;
        }
        record("WIDGET_LAYER_BEFORE",label,0);
        ui.set_layer(p,&ui.layer_id);
        record("WIDGET_LAYER_AFTER",label,0);
    }
    return p;
}
int a6400_ui_attach_v1(void *system,unsigned tid) {
    record("FILM_ATTACH_BEGIN",(int)tid,(int)(uintptr_t)system);
    if(ui.attached || !system || (uintptr_t)system<0x10000u || tid!=(uint32_t)syscall(SYS_gettid))return -1;
    if(resolve())return -2;
    ui.system=system;ui.tid=tid;ui.keys=get_key_instance_fn();
    if(!ui.keys || (uintptr_t)ui.keys<0x10000u || ui.key(ui.keys,0x1ab,0)<0 || ui.key(ui.keys,0x70dc,0)<0)return -3;
    if(stop_old_v1())return -7;
    input_snapshot(0);
    struct film_settings initial={0,0,2,1};
    const struct film_native_view_ops ops={visibility,draw,settings};
    film_native_view_init(&ui.view,&ops,0,&initial);
#if defined(A6400_UI_COMBO_DIAGNOSTIC)
    ui.layer_id=ui.make_id("A6400_FILM_COMBO_V3" A6400_UI_INSTANCE_SUFFIX);
#elif defined(A6400_UI_TOUCH_CLOSE_DIAGNOSTIC)
    ui.layer_id=ui.make_id("A6400_FILM_CLOSE_TOUCH_V3");
#elif defined(A6400_INPUT_DIAGNOSTIC) && A6400_INPUT_DIAGNOSTIC == 2
    ui.layer_id=ui.make_id("A6400_FILM_INPUT2_V3");
#elif defined(A6400_INPUT_DIAGNOSTIC)
    ui.layer_id=ui.make_id("A6400_FILM_INPUT1_V3");
#elif defined(A6400_UI_RENDER_DIAGNOSTIC)
    ui.layer_id=ui.make_id("A6400_FILM_NATIVE_FONTFIX3_V3");
#else
    ui.layer_id=ui.make_id("A6400_FILM_NATIVE_TRIAL_V3");
#endif
    record("CREATE_LAYER_BEFORE",0,0);
    ui.layer=ui.create_layer(system,&ui.layer_id);
    if(!ui.layer)return -4;
    ui.layer_active(ui.layer,0);ui.layer_clear(ui.layer,1);
    record("FILM_LAYER_CREATED",(int)(uintptr_t)ui.layer,0);
    ui.background=make(0);
    if(!ui.background)return -5;
    place(ui.background,0,0,0,640,480);
    const unsigned char background[4]={8,10,14,148};
    ui.set_color(ui.background,background);
    ui_theme.card_border=make(0);
    ui_theme.card_panel=make(0);
    ui_theme.header_accent=make(0);
    ui_theme.header_divider=make(0);
    if(!ui_theme.card_border || !ui_theme.card_panel ||
       !ui_theme.header_accent || !ui_theme.header_divider)return -5;
    place(ui_theme.card_border,0,54,34,532,412);
    const unsigned char card_border[4]={84,92,106,224};
    ui.set_color(ui_theme.card_border,card_border);
    place(ui_theme.card_panel,0,56,36,528,408);
    const unsigned char card_panel[4]={18,22,29,230};
    ui.set_color(ui_theme.card_panel,card_panel);
    place(ui_theme.header_accent,0,56,36,528,4);
    const unsigned char header_accent[4]={234,106,28,255};
    ui.set_color(ui_theme.header_accent,header_accent);
    place(ui_theme.header_divider,0,76,86,488,1);
    const unsigned char header_divider[4]={58,66,78,200};
    ui.set_color(ui_theme.header_divider,header_divider);
    for(unsigned i=0;i<6;i++){
        ui.panels[i]=make(0);
        ui_theme.row_accents[i]=make(0);
        if(!ui.panels[i] || !ui_theme.row_accents[i])return -5;
    }
    ui_theme.page_btn=make(0);
    if(!ui_theme.page_btn)return -5;
    for(unsigned i=0;i<8;i++){ui.labels[i]=make(1);if(!ui.labels[i])return -5;}
    ui_toast.bg=make(0);
    ui_toast.accent=make(0);
    ui_toast.label=make(1);
    if(ui_toast.bg && ui_toast.accent && ui_toast.label) {
        hide_toast_widgets();
    } else {
        ui_toast.bg=ui_toast.accent=ui_toast.label=0;
    }
#ifdef A6400_CHARO_SHUTTER
    ui_theme.shutter_sound_enabled=1;
    ui_theme.last_shutter_key=0;
    ui_theme.shutter_pending=0;
    ui_theme.shutter_arm_ms=0;
    ui_theme.last_shutter_beep_ms=0;
#ifdef A6400_UI_LANG_SC
    strcpy(ui.texts[0],"A6400 V3.9 SSLF | 关闭");
#else
    strcpy(ui.texts[0],"A6400 V3.9 SSLF | 關閉");
#endif
#else
#ifdef A6400_UI_LANG_SC
    strcpy(ui.texts[0],"A6400 V3.9 SSLF | 关闭");
#else
    strcpy(ui.texts[0],"A6400 V3.9 SSLF | 關閉");
#endif
#endif
#ifdef A6400_UI_SETTINGS_DIAGNOSTIC
    strcpy(ui.texts[7],"MENU + 放大鏡 | RAM");
#elif defined(A6400_UI_COMBO_DIAGNOSTIC)
    strcpy(ui.texts[7],"MENU + 放大鏡");
#elif defined(A6400_UI_TOUCH_CLOSE_DIAGNOSTIC)
    strcpy(ui.texts[7],"20秒自動隱藏");
#elif defined(A6400_UI_RENDER_DIAGNOSTIC) && A6400_UI_DISPLAY_MS % 1000 == 0
    snprintf(ui.texts[7],sizeof(ui.texts[7]),"%u second display test",A6400_UI_DISPLAY_MS/1000u);
#elif defined(A6400_UI_RENDER_DIAGNOSTIC)
    snprintf(ui.texts[7],sizeof(ui.texts[7]),"%u ms display test",(unsigned)A6400_UI_DISPLAY_MS);
#else
    strcpy(ui.texts[7],"MENU + 放大鏡");
#endif
    record("TITLE_TEXT_BEFORE",0,0);
    place(ui.labels[0],1,76,48,488,34);ui.set_text(ui.labels[0],ui.texts[0]);
    place(ui.labels[7],1,76,394,312,28);ui.set_text(ui.labels[7],ui.texts[7]);
    record("FILM_WIDGETS_CREATED",7,8);
    input_snapshot(1);
    ui.ticker=calloc(1,0x200u);
    if(!ui.ticker)return -5;
    ui.widget_ctor(ui.ticker);
    uintptr_t *original=*(uintptr_t **)ui.ticker;
    uintptr_t expected_animate=(uintptr_t)dlsym(RTLD_DEFAULT,"_ZN2ux6wgtsys10WidgetBase9onAnimateEv");
    if(!original || !expected_animate || original[32]!=expected_animate)return -6;
    memcpy(ui.vtable,original-2,sizeof(ui.vtable));
    ui.vtable[34]=(uintptr_t)animate;
    *(uintptr_t **)ui.ticker=ui.vtable+2; /* Clone only our new Widget's table. */
#if defined(A6400_UI_COMBO_DIAGNOSTIC)
    ui.ticker_layer_id=ui.make_id("A6400_FILM_COMBO_TICKER_V3" A6400_UI_INSTANCE_SUFFIX);
#elif defined(A6400_UI_TOUCH_CLOSE_DIAGNOSTIC)
    ui.ticker_layer_id=ui.make_id("A6400_FILM_CLOSE_TOUCH_TICKER_V3");
#elif defined(A6400_INPUT_DIAGNOSTIC) && A6400_INPUT_DIAGNOSTIC == 2
    ui.ticker_layer_id=ui.make_id("A6400_FILM_INPUT2_TICKER_V3");
#elif defined(A6400_INPUT_DIAGNOSTIC)
    ui.ticker_layer_id=ui.make_id("A6400_FILM_INPUT1_TICKER_V3");
#elif defined(A6400_UI_RENDER_DIAGNOSTIC)
    ui.ticker_layer_id=ui.make_id("A6400_FILM_NATIVE_FONTFIX3_TICKER_V3");
#else
    ui.ticker_layer_id=ui.make_id("A6400_FILM_NATIVE_TICKER_V3");
#endif
    ui.ticker_layer=ui.create_layer(system,&ui.ticker_layer_id);
    if(!ui.ticker_layer)return -4;
    ui.layer_clear(ui.ticker_layer,0);
    ui.set_layer(ui.ticker,&ui.ticker_layer_id);
    ui.layer_active(ui.ticker_layer,1);
    ui.attached=ui.running=1;
    ui.set_animate(ui.ticker,1);
    input_snapshot(2);
    record("FILM_ATTACHED_NATIVE",(int)ui.layer_id,(int)(uintptr_t)ui.ticker);
    return 0;
}
