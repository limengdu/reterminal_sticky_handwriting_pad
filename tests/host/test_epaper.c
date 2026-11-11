/* Integration tests compile the production C files unchanged. Only ESP-IDF I/O
 * is replaced. SPI bytes below are checked independently, pixel by pixel. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "epaper_panel_priv.h"
#include "esp_heap_caps.h"

static int levels[64], edge, last_edge, read_error, fail_command=-1;
static unsigned reads, commands[256], live_allocs, live_sems;
static int alloc_until_failure=-1;
static uint8_t id=7, command, temperature=25;
static uint8_t data[256][100000];
static size_t lengths[256];
static int64_t now;
static void trace_reset(void) { memset(commands,0,sizeof commands); memset(lengths,0,sizeof lengths); reads=0; }
int gpio_config(const gpio_config_t *c) {(void)c;return 0;}
int gpio_set_level(int p,int v) {levels[p]=v;return 0;}
int gpio_get_level(int p) {return levels[p];}
int gpio_install_isr_service(int f) {(void)f;return 0;}
int gpio_isr_handler_add(int p,void (*f)(void *),void *a) {(void)p;(void)f;(void)a;return 0;}
int gpio_isr_handler_remove(int p) {(void)p;return 0;}
int gpio_set_intr_type(int p,int e) {(void)p;edge=e;if(e)last_edge=e;return 0;}
SemaphoreHandle_t xSemaphoreCreateBinary(void) {++live_sems;return malloc(1);}
void vSemaphoreDelete(SemaphoreHandle_t s) {--live_sems;free(s);}
int xSemaphoreTake(SemaphoreHandle_t s,TickType_t t) {(void)s;now+=(int64_t)t*1000;return pdFALSE;}
int xSemaphoreGiveFromISR(SemaphoreHandle_t s,BaseType_t *b) {(void)s;(void)b;return pdTRUE;}
void vTaskDelay(TickType_t t) {now+=(int64_t)t*1000;}
int64_t esp_timer_get_time(void) {return now++;}
void *heap_caps_malloc(size_t n,int caps) {(void)caps;if(alloc_until_failure==0)return NULL;if(alloc_until_failure>0)--alloc_until_failure;void *p=malloc(n);if(p)++live_allocs;return p;}
void *heap_caps_malloc_prefer(size_t n,int count,...) {(void)count;return heap_caps_malloc(n,0);}
void heap_caps_free(void *p) {if(p){assert(live_allocs);--live_allocs;free(p);}}
int spi_device_transmit(spi_device_handle_t h,spi_transaction_t *t) {
    (void)h;
    if(t->rxlength){++reads;if(read_error)return ESP_FAIL;t->rx_data[0]=command==0x70?id:temperature;return 0;}
    const uint8_t *p=t->tx_buffer;size_t n=t->length/8;
    if(!levels[1]){assert(n==1);command=p[0];if(command==fail_command)return ESP_FAIL;++commands[command];}
    else {assert(lengths[command]+n<=sizeof data[0]);memcpy(data[command]+lengths[command],p,n);lengths[command]+=n;}
    return 0;
}
static seeed_epaper_panel_config_t config(bool mirror) {
    return (seeed_epaper_panel_config_t){.spi_handle=(void *)1,.pin_dc=1,.pin_rst=2,.pin_busy=3,.pin_enable=-1,.busy_timeout_ms=2,.busy_level=1,.mirror_x=mirror};
}
static seeed_epaper_panel_handle_t create(int model,bool mirror) {
    seeed_epaper_panel_handle_t p=NULL;seeed_epaper_panel_config_t c=config(mirror);
    levels[3]=(model==SEEED_EPAPER_PANEL_SSD2677 || (model==SEEED_EPAPER_PANEL_STICKY_AUTO&&id==7));
    assert(seeed_epaper_new_panel(model,&c,&p)==ESP_OK);return p;
}
static void destroy(seeed_epaper_panel_handle_t p) {assert(seeed_epaper_panel_del(p)==ESP_OK);assert(!live_allocs&&!live_sems);}
static unsigned bit(const uint8_t *p,unsigned x) {return (p[x/8]>>(7-x%8))&1;}
static unsigned pair(const uint8_t *p,unsigned x) {return (p[x/4]>>(6-2*(x%4)))&3;}
static void test_detection(void) {
    const uint8_t ids[]={7,0,0xff,1};
    for(unsigned i=0;i<sizeof ids;i++) {id=ids[i];trace_reset();seeed_epaper_panel_handle_t p=create(SEEED_EPAPER_PANEL_STICKY_AUTO,false);assert(reads==1&&commands[0x70]==1);assert(p->model==(id==7?SEEED_EPAPER_PANEL_SSD2677:SEEED_EPAPER_PANEL_SSD1677));assert(p->config.busy_level==(id==7?0:1));destroy(p);}
    id=7;read_error=1;seeed_epaper_panel_handle_t p=NULL;seeed_epaper_panel_config_t c=config(false);assert(seeed_epaper_new_panel(SEEED_EPAPER_PANEL_STICKY_AUTO,&c,&p)==ESP_FAIL);assert(!p&&!live_allocs&&!live_sems);read_error=0;
    for(int m=0;m<=SEEED_EPAPER_PANEL_SSD2677;m++){trace_reset();p=create(m,false);assert(!reads&&!commands[0x70]);assert(p->model==m);destroy(p);}
    puts("PASS detection IDs, read failure, explicit models, cleanup");
}
static void test_busy(void) {
    for(int m=SEEED_EPAPER_PANEL_SSD1677;m<=SEEED_EPAPER_PANEL_SSD2677;m++){
        seeed_epaper_panel_handle_t p=create(m,false);assert(seeed_epaper_panel_wait_ready(p)==ESP_OK);
        levels[3]=p->config.busy_level;assert(seeed_epaper_panel_wait_ready(p)==ESP_ERR_TIMEOUT);assert(last_edge==(m==SEEED_EPAPER_PANEL_SSD2677?GPIO_INTR_POSEDGE:GPIO_INTR_NEGEDGE));assert(!edge&&!seeed_epaper_panel_busy_wait_active());destroy(p);
    }puts("PASS BUSY polarity, interrupt edge, timeout cleanup");
}
static void test_frames(bool mirror) {
    seeed_epaper_panel_handle_t p=create(SEEED_EPAPER_PANEL_SSD2677,mirror);
    uint8_t *mono=malloc(48000),*gray=malloc(96000);
    for(unsigned i=0;i<48000;i++)mono[i]=(uint8_t)(i*37+13);
    for(unsigned i=0;i<96000;i++)gray[i]=(uint8_t)(i*19+27);
    seeed_epaper_area_t full={0,0,800,480};
    trace_reset();assert(seeed_epaper_panel_refresh_area(p,&full,mono,100,SEEED_EPAPER_PIXEL_FORMAT_MONO1_MSB,SEEED_EPAPER_REFRESH_FULL)==0);
    assert(lengths[0x10]==96000&&commands[0x04]==1&&commands[0x12]==1&&commands[0x02]==1);assert(!p->analog_on);
    for(unsigned y=0;y<480;y++)for(unsigned x=0;x<800;x++)assert(pair(data[0x10]+y*200,x)==3*bit(mono+y*100,mirror?799-x:x));
    trace_reset();assert(seeed_epaper_panel_write_bitmap_gray4(p,&full,gray,200)==0);assert(seeed_epaper_panel_commit(p,&full,SEEED_EPAPER_REFRESH_GRAY4)==0);assert(lengths[0x10]==96000);
    for(unsigned y=0;y<480;y++)for(unsigned x=0;x<800;x++)assert(pair(data[0x10]+y*200,x)==pair(gray+y*200,mirror?799-x:x));
    trace_reset();assert(seeed_epaper_panel_set_partial_baseline(p,&full,mono,100,SEEED_EPAPER_PIXEL_FORMAT_MONO1_MSB)==0);assert(!commands[0x10]);
    const seeed_epaper_area_t area={16,7,8,1};uint8_t current=0x5a;
    assert(seeed_epaper_panel_write_bitmap(p,&area,&current,1,SEEED_EPAPER_PIXEL_FORMAT_MONO1_MSB,SEEED_EPAPER_REFRESH_PARTIAL)==0);
    assert(seeed_epaper_panel_commit(p,&area,SEEED_EPAPER_REFRESH_PARTIAL)==0);assert(lengths[0x10]==96000&&p->analog_on);
    for(unsigned y=0;y<480;y++)for(unsigned x=0;x<800;x++){
        unsigned lx=mirror?799-x:x;unsigned prev=bit(mono+y*100,lx);unsigned curr=y==7&&lx>=16&&lx<24?bit(&current,lx-16):prev;
        assert(pair(data[0x10]+y*200,x)==(prev*2+curr));
    }
    assert(!memcmp(p->previous_framebuffer,p->framebuffer,48000));
    trace_reset();uint8_t supplied_previous=0x0f,next=0xf0;
    assert(seeed_epaper_panel_write_bitmap_diff(p,&area,&supplied_previous,&next,1,SEEED_EPAPER_PIXEL_FORMAT_MONO1_MSB,SEEED_EPAPER_REFRESH_PARTIAL)==0);
    assert(seeed_epaper_panel_commit(p,&area,SEEED_EPAPER_REFRESH_PARTIAL)==0);
    for(unsigned x=16;x<24;x++) assert(pair(data[0x10]+7*200,mirror?799-x:x)==2*bit(&supplied_previous,x-16)+bit(&next,x-16));
    assert(!commands[4]); /* second partial reuses analog power */
    trace_reset();assert(seeed_epaper_panel_power_off(p)==0);assert(commands[2]==1&&!p->analog_on);assert(seeed_epaper_panel_power_off(p)==0&&commands[2]==1);
    trace_reset();assert(seeed_epaper_panel_sleep(p)==0);assert(commands[7]==1&&lengths[7]==1&&data[7][0]==0xa5);assert(p->sleeping&&!p->partial_ready);assert(seeed_epaper_panel_wakeup(p,SEEED_EPAPER_REFRESH_FULL)==0&&!p->sleeping);
    free(mono);free(gray);destroy(p);
    printf("PASS SSD2677 full mono, gray4, baseline-to-partial, power, sleep/wake (mirror=%d)\n",mirror);
}
static void test_legacy(void) {
    seeed_epaper_panel_handle_t p=create(SEEED_EPAPER_PANEL_SSD1677,false);uint8_t *mono=malloc(48000);memset(mono,0xa5,48000);seeed_epaper_area_t full={0,0,800,480};
    trace_reset();assert(seeed_epaper_panel_refresh_area(p,&full,mono,100,SEEED_EPAPER_PIXEL_FORMAT_MONO1_MSB,SEEED_EPAPER_REFRESH_FULL)==0);assert(lengths[0x24]==48000&&lengths[0x26]==48000);assert(!memcmp(data[0x24],mono,48000)&&!memcmp(data[0x26],mono,48000));
    trace_reset();seeed_epaper_area_t a={8,10,8,1};uint8_t prev=0xa5,curr=0x5a;assert(seeed_epaper_panel_write_bitmap_diff(p,&a,&prev,&curr,1,SEEED_EPAPER_PIXEL_FORMAT_MONO1_MSB,SEEED_EPAPER_REFRESH_PARTIAL)==0);assert(lengths[0x26]==1&&data[0x26][0]==prev);assert(lengths[0x24]==1&&data[0x24][0]==curr);assert(seeed_epaper_panel_commit(p,&a,SEEED_EPAPER_REFRESH_PARTIAL)==0);
    uint8_t *gray=malloc(96000);memset(gray,0x1b,96000);trace_reset();
    assert(seeed_epaper_panel_write_bitmap_gray4(p,&full,gray,200)==0);
    assert(lengths[0x24]==48000&&lengths[0x26]==48000);
    for(unsigned i=0;i<48000;i++){assert(data[0x24][i]==0xcc);assert(data[0x26][i]==0xaa);}
    assert(seeed_epaper_panel_commit(p,&full,SEEED_EPAPER_REFRESH_GRAY4)==0);
    trace_reset();assert(seeed_epaper_panel_set_partial_baseline(p,&full,mono,100,SEEED_EPAPER_PIXEL_FORMAT_MONO1_MSB)==0);
    assert(!commands[0x24]&&!commands[0x26]);
    assert(seeed_epaper_panel_write_bitmap(p,&a,&curr,1,SEEED_EPAPER_PIXEL_FORMAT_MONO1_MSB,SEEED_EPAPER_REFRESH_PARTIAL)==0);
    assert(lengths[0x26]==1&&data[0x26][0]==0xa5);
    free(gray);free(mono);destroy(p);puts("PASS SSD1677 full planes and partial previous/current routing");
}
static void test_failures(void) {
    for(int i=0;i<3;i++){alloc_until_failure=i;seeed_epaper_panel_handle_t p=NULL;seeed_epaper_panel_config_t c=config(false);int e=seeed_epaper_new_panel(SEEED_EPAPER_PANEL_STICKY_AUTO,&c,&p);assert(e==ESP_ERR_NO_MEM&&!p);assert(!live_allocs&&!live_sems);}alloc_until_failure=-1;
    seeed_epaper_panel_handle_t p=create(SEEED_EPAPER_PANEL_SSD2677,false);fail_command=0x00;assert(seeed_epaper_panel_prepare(p,SEEED_EPAPER_REFRESH_FULL)==ESP_FAIL);assert(!p->prepared);fail_command=-1;assert(seeed_epaper_panel_prepare(p,SEEED_EPAPER_REFRESH_FULL)==0);
    uint8_t byte=0x00;seeed_epaper_area_t a={0,0,8,1};
    assert(seeed_epaper_panel_write_bitmap(p,&a,&byte,1,SEEED_EPAPER_PIXEL_FORMAT_MONO1_MSB,SEEED_EPAPER_REFRESH_FULL)==0);
    uint8_t previous=p->previous_framebuffer[0];fail_command=0x10;
    assert(seeed_epaper_panel_commit(p,&a,SEEED_EPAPER_REFRESH_FULL)==ESP_FAIL);
    assert(!p->prepared&&p->previous_framebuffer[0]==previous);
    fail_command=-1;
    alloc_until_failure=0;uint8_t gray[2]={0x1b,0x1b};
    assert(seeed_epaper_panel_write_bitmap_gray4(p,&a,gray,2)==ESP_ERR_NO_MEM);
    alloc_until_failure=-1;destroy(p);puts("PASS allocation rollback and SPI prepare/commit failures");
}
/* Boundary expectations are literal reference values, not the production
 * bucket calculation duplicated in the test. */
static void test_temperature_waveforms(void) {
    static const struct {uint8_t raw,full,partial;} cases[]={
        {10,0xe8,0x08},{11,0xeb,0x0f},{20,0xeb,0x0f},{21,0xee,0x19},
        {30,0xee,0x19},{31,0xf1,0x23},{127,0xf1,0x23},{128,0xe8,0x08}
    };
    static const struct {uint8_t raw,waveform;} grays[]={
        {15,0xf1},{16,0xfa},{127,0xfa},{128,0xf1}
    };
    seeed_epaper_panel_handle_t p=create(SEEED_EPAPER_PANEL_SSD2677,false);
    seeed_epaper_area_t a={0,0,8,1};uint8_t mono=0xa5,gray[2]={0x1b,0xe4};
    for(unsigned i=0;i<sizeof cases/sizeof cases[0];i++){
        temperature=cases[i].raw;
        trace_reset();
        assert(seeed_epaper_panel_refresh_area(p,&a,&mono,1,SEEED_EPAPER_PIXEL_FORMAT_MONO1_MSB,SEEED_EPAPER_REFRESH_FULL)==0);
        assert(commands[0x40]==1&&reads==1&&lengths[0xe6]==1&&data[0xe6][0]==cases[i].full);
        assert(p->last_refresh_timing.temperature_valid&&!p->last_refresh_timing.temperature_external&&p->last_refresh_timing.temperature_c==temperature);
        trace_reset();
        assert(seeed_epaper_panel_refresh_area(p,&a,&mono,1,SEEED_EPAPER_PIXEL_FORMAT_MONO1_MSB,SEEED_EPAPER_REFRESH_PARTIAL)==0);
        assert(commands[0x40]==1&&reads==1&&lengths[0xe6]==1&&data[0xe6][0]==cases[i].partial);
    }
    for(unsigned i=0;i<sizeof grays/sizeof grays[0];i++){
        temperature=grays[i].raw;trace_reset();
        assert(seeed_epaper_panel_write_bitmap_gray4(p,&a,gray,2)==0);
        assert(seeed_epaper_panel_commit(p,&a,SEEED_EPAPER_REFRESH_GRAY4)==0);
        assert(commands[0x40]==1&&reads==1&&lengths[0xe6]==1&&data[0xe6][0]==grays[i].waveform);
    }
    /* Inject failure specifically into the post-staging temperature read. */
    temperature=25;mono=0xff;
    assert(seeed_epaper_panel_set_partial_baseline(p,&a,&mono,1,SEEED_EPAPER_PIXEL_FORMAT_MONO1_MSB)==0);
    mono=0;
    assert(seeed_epaper_panel_write_bitmap(p,&a,&mono,1,SEEED_EPAPER_PIXEL_FORMAT_MONO1_MSB,SEEED_EPAPER_REFRESH_PARTIAL)==0);
    uint8_t *before=malloc(48000);memcpy(before,p->previous_framebuffer,48000);
    trace_reset();read_error=1;
    assert(seeed_epaper_panel_commit(p,&a,SEEED_EPAPER_REFRESH_PARTIAL)==ESP_FAIL);
    assert(commands[0x40]==1&&reads==1&&!commands[0x10]&&!commands[0x12]);
    assert(!p->prepared&&!p->partial_ready&&!p->last_refresh_timing.temperature_valid);
    assert(!memcmp(before,p->previous_framebuffer,48000));
    read_error=0;trace_reset();
    assert(seeed_epaper_panel_prepare(p,SEEED_EPAPER_REFRESH_FULL)==0);
    assert(commands[0x00]==1&&p->prepared);
    assert(seeed_epaper_panel_refresh_area(p,&a,&mono,1,SEEED_EPAPER_PIXEL_FORMAT_MONO1_MSB,SEEED_EPAPER_REFRESH_FULL)==0);
    assert(lengths[0x10]==96000&&p->partial_ready&&!memcmp(p->previous_framebuffer,p->framebuffer,48000));
    assert(pair(data[0x10],0)==0);
    free(before);destroy(p);
    puts("PASS temperature waveform boundaries, read failure baseline preservation, recovery");
}
int main(void) {test_detection();test_busy();test_frames(false);test_frames(true);test_legacy();test_failures();test_temperature_waveforms();puts("All host epaper integration tests passed.");return 0;}
