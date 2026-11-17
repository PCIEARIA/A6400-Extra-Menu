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
    if(!v->drawn || v->drawn_page!=v->menu.page ||
       v->drawn_selected!=v->menu.selected ||
       v->drawn_enabled!=v->menu.settings.enabled || v->drawn_stock!=v->menu.settings.stock) {
        v->ops.draw(v->context,&v->menu);
        v->drawn=1;v->drawn_page=v->menu.page;v->drawn_selected=v->menu.selected;
        v->drawn_enabled=v->menu.settings.enabled;v->drawn_stock=v->menu.settings.stock;
    }
    if(change & FILM_MENU_OPENED)v->ops.visibility(v->context,1);
    return change;
}
int film_native_view_row(const struct film_menu *m,unsigned row,char *out,unsigned capacity,
                         int *x,int *y,int *width,int *height) {
    if(!m || row>=6 || !out || !capacity)return -1;
#ifdef A6400_BETA_BLING_SHUTTER
    unsigned count=m->page==FILM_MENU_HOME?4:6;
#else
    unsigned count=m->page==FILM_MENU_HOME?3:6;
#endif
    if(row>=count){out[0]=0;return 0;}
    if(!x||!y||!width||!height)return -1;
    int n;
    if(m->page==FILM_MENU_STOCKS) {
        if(row<5) {
            *x=76;*width=488;*y=98+(int)row*54;*height=46;
            n=snprintf(out,capacity,"%s %s",(int)row==m->settings.stock?"*":" ",
                       film_stock_display_name((int)row));
        } else {
            *x=404;*width=160;*y=386;*height=42;
            n=snprintf(out,capacity,"返回");
        }
    } else if(row<2) {
        *x=76;*width=488;*y=row==0?106:198;*height=76;
        if(row==0)n=snprintf(out,capacity,"LUT 濾鏡: %s",m->settings.enabled?"開啟":"關閉");
        else n=snprintf(out,capacity,"膠片風格: %s",film_stock_display_name(m->settings.stock));
#ifdef A6400_BETA_BLING_SHUTTER
    } else if(row==3) {
        *x=76;*width=488;*y=290;*height=76;
        n=snprintf(out,capacity,"快門聲: Bling");
#endif
    } else {
        *x=404;*width=160;*y=386;*height=42;
        n=snprintf(out,capacity,"關閉");
    }
    return n>=0 && (unsigned)n<capacity?1:-1;
}
