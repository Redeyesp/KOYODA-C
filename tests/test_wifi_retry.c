#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "koyoda_wifi_retry.h"
#include "koyoda_animation.h"

int main(void)
{
    koyoda_wifi_retry_t r=wifi_retry_init(100);
    assert(wifi_retry_poll(&r,30099)==WIFI_RETRY_NONE);
    assert(wifi_retry_poll(&r,30100)==WIFI_RETRY_START_TIMEOUT);
    wifi_retry_begin(&r,30100);
    assert(wifi_retry_poll(&r,60099)==WIFI_RETRY_NONE);
    assert(wifi_retry_poll(&r,60100)==WIFI_RETRY_TIMEOUT);
    assert(wifi_retry_schedule(&r,60100)==5000);
    assert(wifi_retry_schedule(&r,60200)==0);
    assert(!wifi_retry_connected(&r)); /* stale DHCP during retry */
    assert(wifi_retry_poll(&r,65099)==WIFI_RETRY_NONE);
    assert(wifi_retry_poll(&r,65100)==WIFI_RETRY_CONNECT);
    assert(wifi_retry_poll(&r,65100)==WIFI_RETRY_NONE); /* one attempt */
    assert(wifi_retry_connected(&r));
    assert(wifi_retry_schedule(&r,66000)==5000); /* reset on success */
    r=wifi_retry_init(UINT32_MAX-1000);
    wifi_retry_begin(&r,UINT32_MAX-1000);
    assert(wifi_retry_schedule(&r,UINT32_MAX-1000)==5000);
    assert(wifi_retry_poll(&r,3998)==WIFI_RETRY_NONE);
    assert(wifi_retry_poll(&r,3999)==WIFI_RETRY_CONNECT);
    puts("PASS policy: deadline, duplicate event, stale IP, reset, clock wrap");

    /* Regression check on unchanged tap-wake animation. These assertions
     * do not simulate an LCD or the ESP-IDF scheduler. */
    koyoda_animation_t a; bool pending=false;
    anim_reset(&a,0);
    anim_tick(&a,299999,true,false,false,&pending); assert(a.mode==ANIM_IDLE);
    anim_tick(&a,300000,true,false,false,&pending); assert(a.mode==ANIM_DROWSY);
    anim_tick(&a,300600,true,false,false,&pending);
    anim_tick(&a,301400,true,false,false,&pending);
    anim_tick(&a,301900,true,false,false,&pending);
    assert(anim_tick(&a,302400,true,false,false,&pending)==3 && a.mode==ANIM_SLEEP);
    assert(anim_tick(&a,303200,true,false,false,&pending)==4);
    assert(anim_tick(&a,304000,true,false,false,&pending)==5);
    assert(anim_touch(&a,304100) && a.mode==ANIM_WAKE);
    pending=true; /* charging must wait for wake + happy to finish */
    assert(anim_tick(&a,304100,true,false,false,&pending)==2 && pending);
    assert(anim_tick(&a,304550,true,false,false,&pending)==1);
    assert(anim_tick(&a,305050,true,false,false,&pending)==0);
    assert(anim_tick(&a,305700,true,false,false,&pending)==12);
    assert(anim_tick(&a,306800,true,false,false,&pending)==0 && a.mode==ANIM_IDLE);
    assert(anim_tick(&a,306820,true,false,false,&pending)==6 && !pending);
    uint32_t now=306820;
    for (unsigned i=0;i<8;i++) {
        assert(anim_tick(&a,now+anim_charge_ms[i]-1,true,false,false,&pending)==anim_charge_frames[i]);
        now+=anim_charge_ms[i]; anim_tick(&a,now,true,false,false,&pending);
    }
    assert(a.mode==ANIM_IDLE && anim_charge_ms[5]==450);
    pending=true; anim_tick(&a,now+20,true,false,false,&pending);
    anim_tick(&a,now+40,false,false,false,&pending); assert(pending && a.mode==ANIM_IDLE);
    anim_tick(&a,now+60,true,true,false,&pending); assert(pending);
    assert(anim_tick(&a,now+80,true,false,false,&pending)==6 && !pending);
    puts("PASS animation: 5 min sleep, wake happy, charging hold, page/dialog defer");
}
