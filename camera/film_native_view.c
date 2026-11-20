#include "film_native_view.h"
#include <stdio.h>

void film_native_view_init(struct film_native_view *v,const struct film_native_view_ops *ops,
                          void *context,const struct film_settings *settings) {
    *v=(struct film_native_view){0};
    film_menu_init(&v->menu,settings);
    v->ops=*ops;v->context=context;
}
void film_native_view_hide(struct film_native_view *v) {
    if(film_menu_close(&v->menu))v->ops.visibility(v->context,0);
    v->drawn=0;
}
unsigned film_native_view_step(struct film_native_view *v,unsigned now,
                               int menu,int zoom,int other,int touch_state,int x,int y) {
    unsigned change=film_menu_input(&v->menu,now,menu,zoom,other);
    /* Sony's enum is 5=pressed, 6=released, NOT a boolean. Invalid state
     * cancels the gesture by hiding, rather than fabricating a release. */
    if(touch_state!=5 && touch_state!=6) {
        change|=film_menu_close(&v->menu);
    } else change|=film_menu_touch(&v->menu,now,touch_state==5,x,y);
    if(change & FILM_MENU_SETTINGS_CHANGED)v->ops.settings(v->context,&v->menu.settings);
    if(!v->menu.visible) {
        if(change & FILM_MENU_CLOSED)v->ops.visibility(v->context,0);
        v->drawn=0;
        return change;
    }
    if(!v->drawn || (change & FILM_MENU_SETTINGS_CHANGED) ||
       v->drawn_page!=v->menu.page || v->drawn_selected!=v->menu.selected ||
       v->drawn_enabled!=v->menu.settings.enabled || v->drawn_stock!=v->menu.settings.stock) {
        v->ops.draw(v->context,&v->menu);
        v->drawn=1;v->drawn_page=v->menu.page;v->drawn_selected=v->menu.selected;
        v->drawn_enabled=v->menu.settings.enabled;v->drawn_stock=v->menu.settings.stock;
    }
    if(change & FILM_MENU_OPENED)v->ops.visibility(v->context,1);
    return change;
}
#ifdef A6400_UI_LANG_SC
#define L_ON          "开启"
#define L_OFF         "关闭"
#define L_OPEN_S      "开"
#define L_CLOSE_S     "关"
#define L_NEXT_PAGE   "下一页 >"
#define L_PREV_PAGE   "< 上一页"
#define L_GRAIN_FMT   "颗粒强度: %s"
#define L_HALATION_FMT "胶片光晕: %s"
#define L_HALATION_OFF "关闭 (加速)"
#define L_RESET_DET   "恢复细节默认 (标准/开)"
#define L_LUT_FMT     "LUT 滤镜: %s"
#define L_STYLE_FMT   "胶片风格: %s"
#define L_DETAIL_FMT  "胶片细节: 颗粒%s / 光晕%s"
#define L_CHARO_DEF   "快门声: Charo"
#define L_GRAIN_SHORT {"无","轻","中","强"}
#define L_GRAIN_DET   {"无 (加速)","轻微","标准","浓厚"}
#define L_GRAIN_MID   "中"
#define L_GRAIN_STD   "标准"
#else
#define L_ON          "開啟"
#define L_OFF         "關閉"
#define L_OPEN_S      "開"
#define L_CLOSE_S     "關"
#define L_NEXT_PAGE   "下一頁 >"
#define L_PREV_PAGE   "< 上一頁"
#define L_GRAIN_FMT   "顆粒強度: %s"
#define L_HALATION_FMT "底片光暈: %s"
#define L_HALATION_OFF "關閉 (加速)"
#define L_RESET_DET   "恢復細節預設 (標準/開)"
#define L_LUT_FMT     "LUT 濾鏡: %s"
#define L_STYLE_FMT   "膠片風格: %s"
#define L_DETAIL_FMT  "膠片細節: 顆粒%s / 光暈%s"
#define L_CHARO_DEF   "快門聲: Charo"
#define L_GRAIN_SHORT {"無","輕","中","強"}
#define L_GRAIN_DET   {"無 (加速)","輕微","標準","濃厚"}
#define L_GRAIN_MID   "中"
#define L_GRAIN_STD   "標準"
#endif

static const char *grain_short(int grain) {
    static const char *const names[4]=L_GRAIN_SHORT;
    return (grain>=0 && grain<4)?names[grain]:L_GRAIN_MID;
}
static const char *grain_detail(int grain) {
    static const char *const names[4]=L_GRAIN_DET;
    return (grain>=0 && grain<4)?names[grain]:L_GRAIN_STD;
}
int film_native_view_row(const struct film_menu *m,unsigned row,char *out,unsigned capacity,
                         int *x,int *y,int *width,int *height) {
    if(!m || row>=7 || !out || !capacity)return -1;
    unsigned count=m->page==FILM_MENU_HOME?5:m->page==FILM_MENU_DETAILS?6:7;
    if(row>=count){out[0]=0;return 0;}
#ifndef A6400_CHARO_SHUTTER
    if(m->page==FILM_MENU_HOME && row==3){out[0]=0;return 0;}
#endif
    if(m->page==FILM_MENU_DETAILS && (row==3 || row==4)){out[0]=0;return 0;}
    if(!x||!y||!width||!height)return -1;
    int n;
    if(m->page==FILM_MENU_STOCKS || m->page==FILM_MENU_STOCKS_P2) {
        if(row<5) {
            int stock=(m->page==FILM_MENU_STOCKS_P2?5:0)+(int)row;
            *x=76;*width=488;*y=98+(int)row*54;*height=46;
            n=snprintf(out,capacity,"%s %s",stock==m->settings.stock?"*":" ",
                       film_stock_display_name(stock));
        } else if(row==5) {
            *x=404;*width=160;*y=386;*height=42;
            n=snprintf(out,capacity,"返回");
        } else {
            *x=76;*width=160;*y=386;*height=42;
            n=snprintf(out,capacity,"%s",m->page==FILM_MENU_STOCKS?L_NEXT_PAGE:L_PREV_PAGE);
        }
    } else if(m->page==FILM_MENU_DETAILS) {
        if(row==0) {
            *x=76;*width=488;*y=106;*height=76;
            n=snprintf(out,capacity,L_GRAIN_FMT,grain_detail(m->settings.grain));
        } else if(row==1) {
            *x=76;*width=488;*y=198;*height=76;
            n=snprintf(out,capacity,L_HALATION_FMT,m->settings.halation?L_ON:L_HALATION_OFF);
        } else if(row==2) {
            *x=76;*width=488;*y=290;*height=76;
            n=snprintf(out,capacity,L_RESET_DET);
        } else {
            *x=404;*width=160;*y=386;*height=42;
            n=snprintf(out,capacity,"返回");
        }
    } else if(row<2) {
        *x=76;*width=488;*y=row==0?98:168;*height=60;
        if(row==0)n=snprintf(out,capacity,L_LUT_FMT,m->settings.enabled?L_ON:L_OFF);
        else n=snprintf(out,capacity,L_STYLE_FMT,film_stock_display_name(m->settings.stock));
    } else if(row==4) {
        *x=76;*width=488;*y=238;*height=60;
        n=snprintf(out,capacity,L_DETAIL_FMT,
                   grain_short(m->settings.grain),m->settings.halation?L_OPEN_S:L_CLOSE_S);
#ifdef A6400_CHARO_SHUTTER
    } else if(row==3) {
        *x=76;*width=488;*y=308;*height=60;
        n=snprintf(out,capacity,L_CHARO_DEF);
#endif
    } else {
        *x=404;*width=160;*y=386;*height=42;
        n=snprintf(out,capacity,L_OFF);
    }
    return n>=0 && (unsigned)n<capacity?1:-1;
}
