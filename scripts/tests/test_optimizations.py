"""Host regression tests for the actual firmware queue and upload helpers.

Run with .venv Python; set HOST_CC to a host clang executable if needed.
The FreeRTOS stubs model ownership and inject a FLUSH during PCM copying;
they do not replace real dual-core/hardware timing validation.
"""
from pathlib import Path
import os
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / '.pio' / 'optimization-tests'
OUT.mkdir(parents=True, exist_ok=True)


def read(path):
    return (ROOT / path).read_text(encoding='utf-8')


def function(source, name):
    start = source.index(name + '(')
    start = source.rfind('\n', 0, start) + 1
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_INVALID_SIZE 3
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define heap_caps_malloc(n,c) malloc(n)
#define heap_caps_free(p) free(p)
'''


def run_c(name, source):
    compiler = os.environ.get('HOST_CC') or shutil.which('clang')
    if not compiler:
        raise RuntimeError('Set HOST_CC to a host clang executable')
    path = OUT / (name + '.c')
    exe = OUT / (name + '.exe')
    path.write_text(source, encoding='utf-8')
    subprocess.run([compiler, '-std=c11', '-Werror', '-Wno-unused-function',
                    '-D_CRT_SECURE_NO_WARNINGS', str(path), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], cwd=OUT, check=True)


def audio_test():
    header = re.sub(r'^#include.*$|^#pragma once$', '', read('main/audio/audio_buffer.h'), flags=re.M)
    source = re.sub(r'^#include.*$', '', read('main/audio/audio_buffer.c'), flags=re.M)
    stubs = r'''
typedef unsigned TickType_t;
typedef int TimeOut_t;
typedef int portMUX_TYPE;
typedef int *SemaphoreHandle_t;
typedef struct { int buffer_underruns, packets_decoded; } audio_stats_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
static int lock_depth;
#define portENTER_CRITICAL(p) do { assert(!lock_depth); lock_depth++; } while(0)
#define portEXIT_CRITICAL(p) do { assert(lock_depth == 1); lock_depth--; } while(0)
static int *xSemaphoreCreateBinary(void) { return calloc(1,sizeof(int)); }
static void vSemaphoreDelete(int *p) { free(p); }
static int xSemaphoreGive(int *p) { *p=1; return 1; }
static void (*on_wait)(void);
static int xSemaphoreTake(int *p, unsigned ticks) {
  if (on_wait && ticks) { void (*f)(void)=on_wait; on_wait=NULL; f(); }
  if (*p) { *p=0; return 1; } return 0;
}
static void vTaskSetTimeOutState(int *p) { *p=0; }
static int xTaskCheckForTimeOut(int *p, unsigned *ticks) { return 0; }
'''
    hook = r'''
static audio_buffer_t *copy_flush;
static void *test_memcpy(void *dst, const void *src, size_t n) {
  assert(lock_depth == 0);
  if(copy_flush) { audio_buffer_t *b=copy_flush; copy_flush=NULL; audio_buffer_flush(b); }
  return memcpy(dst,src,n);
}
'''
    test = r'''
static audio_buffer_t buffer;
static int16_t pcm[352*2];
static void enqueue_on_wait(void) {
  assert(audio_buffer_queue_decoded(&buffer,NULL,77,pcm,352,2));
}
static uint32_t take(void) {
  void *item; size_t n;
  assert(audio_buffer_take(&buffer,&item,&n,0));
  audio_frame_header_t *h=item;
  assert(n == sizeof(*h)+352*4);
  assert(memcmp((char *)item+sizeof(*h),pcm,352*4)==0);
  uint32_t ts=h->rtp_timestamp;
  audio_buffer_return(&buffer,item);
  return ts;
}
int main(void) {
  for(int i=0;i<704;i++) pcm[i]=(int16_t)i;
  assert(audio_buffer_init(&buffer)==ESP_OK);
  buffer.capacity=8; buffer.free_top=8;
  for(int i=0;i<8;i++)buffer.free_stack[i]=(uint16_t)i;
  uint32_t model[8]; int count=0;
  srand(42);
  for(int step=0;step<20000;step++) {
    if(count && rand()%3==0) {
      assert(take()==model[0]); memmove(model,model+1,(--count)*sizeof(*model));
    } else {
      uint32_t ts=UINT32_MAX-2000+(uint32_t)(rand()%4000);
      if(count==8) { memmove(model,model+1,7*sizeof(*model)); count--; }
      int pos=0; while(pos<count && (int32_t)(model[pos]-ts)<0)pos++;
      memmove(model+pos+1,model+pos,(count-pos)*sizeof(*model));model[pos]=ts;count++;
      assert(audio_buffer_queue_decoded(&buffer,NULL,ts,pcm,352,2));
    }
    assert(audio_buffer_get_frame_count(&buffer)==count);
    assert(buffer.free_top+count==8);
    if(count) { uint32_t ts; assert(audio_buffer_oldest_timestamp(&buffer,&ts)); assert(ts==model[0]);
      assert(audio_buffer_peek_newest_rtp(&buffer,&ts)); assert(ts==model[count-1]); }
  }
  audio_buffer_flush(&buffer);
  assert(buffer.free_top==8);
  copy_flush=&buffer;
  assert(!audio_buffer_queue_decoded(&buffer,NULL,42,pcm,352,2));
  assert(buffer.count==0 && buffer.free_top==8);
  assert(!audio_buffer_queue_decoded(&buffer,NULL,42,pcm,352,3));
  on_wait=enqueue_on_wait; void *item; size_t size;
  assert(audio_buffer_take(&buffer,&item,&size,10));
  assert(((audio_frame_header_t *)item)->rtp_timestamp==77);
  audio_buffer_flush(&buffer); // consumer still owns the slot
  assert(buffer.free_top==7);
  for(int i=0;i<10;i++)assert(audio_buffer_queue_decoded(&buffer,NULL,i,pcm,352,2));
  assert(((audio_frame_header_t *)item)->rtp_timestamp==77);
  audio_buffer_return(&buffer,item);
  audio_buffer_flush(&buffer);
  assert(buffer.free_top==8);
  for(int i=0;i<4;i++)assert(audio_buffer_queue_decoded(&buffer,NULL,UINT32_MAX-351+i*352,pcm,352,2));
  uint32_t start,span;
  assert(audio_buffer_bulk_start_rtp(&buffer,&start,&span));
  assert(start==UINT32_MAX-351 && span==1408);
  audio_buffer_deinit(&buffer);
  puts("audio: 20,000 randomized operations, wraparound, ownership, FLUSH and wakeup passed");
}
'''
    run_c('audio', PRELUDE+stubs+header+hook+source.replace('memcpy(', 'test_memcpy(')+test)


def weather_test():
    source = read('components/weather_epd/weather_epd.c')
    start = source.index('#define WEATHER_HTTP_MAX_BYTES')
    end = source.index('static void weather_fetch_air_quality', start)
    stubs = r'''
#define HTTP_EVENT_ON_DATA 1
typedef struct { void *user_data; int event_id; void *data; int data_len; } esp_http_client_event_t;
static int allocations, fail_alloc;
static void *heap_caps_realloc(void *p,size_t n,int caps) {
  allocations++; return fail_alloc ? NULL : realloc(p,n);
}
'''
    tests = r'''
int main(void) {
  weather_http_buf_t b={0}; char data[1024]={0};
  esp_http_client_event_t e={&b,HTTP_EVENT_ON_DATA,data,1024};
  for(int i=0;i<64;i++)assert(weather_http_event(&e)==ESP_OK);
  assert(b.len==65536 && b.cap==65537 && b.buf[b.len]==0);
  int previous=allocations;
  assert(weather_http_event(&e)==ESP_ERR_INVALID_SIZE);
  assert(weather_http_event(&e)==ESP_ERR_INVALID_SIZE);
  assert(allocations==previous && b.len==65536);
  free(b.buf); b=(weather_http_buf_t){0};fail_alloc=1;
  assert(weather_http_event(&e)==ESP_ERR_NO_MEM);
  fail_alloc=0;assert(weather_http_event(&e)==ESP_ERR_NO_MEM);
  assert(b.len==0 && b.buf==NULL);
  puts("weather: exact size limit, overflow rejection and sticky allocation failure passed");
}
'''
    run_c('weather', PRELUDE+stubs+source[start:end]+tests)


def storage_test():
    source = read('components/spiffs_storage/spiffs_storage.c')
    stubs = r'''
static int renames, fail_rename, fail_rollback;
static int test_rename(const char *from,const char *to) {
  renames++;
  if(renames==fail_rename || (fail_rollback && renames==3)) { errno=EIO; return -1; }
  return rename(from,to);
}
'''
    implementation = function(source,'recover_file')+'\n'+function(source,'spiffs_storage_replace_file')
    tests = r'''
static void put(const char *path,const char *text) { FILE *f=fopen(path,"wb");assert(f);assert(fputs(text,f)>=0);assert(!fclose(f)); }
static void check(const char *path,const char *text) { char b[32]={0};FILE*f=fopen(path,"rb");assert(f);fread(b,1,31,f);fclose(f);assert(!strcmp(b,text)); }
int main(void) {
  remove("asset");remove("asset.upbak");remove("asset.uptmp");
  put("asset","old");put("asset.uptmp","new");
  assert(spiffs_storage_replace_file("asset.uptmp","asset")==ESP_OK);check("asset","new");
  put("asset.uptmp","next");renames=0;fail_rename=2;
  assert(spiffs_storage_replace_file("asset.uptmp","asset")!=ESP_OK);check("asset","new");
  renames=0;fail_rollback=1;
  assert(spiffs_storage_replace_file("asset.uptmp","asset")!=ESP_OK);check("asset.upbak","new");
  fail_rename=fail_rollback=0;
  assert(recover_file("asset","asset.upbak")==ESP_OK);check("asset","new");
  put("asset.upbak","old");assert(recover_file("asset","asset.upbak")==ESP_OK);check("asset","new");
  assert(fopen("asset.upbak","rb")==NULL);
  puts("storage: verified replacement, rename failure, rollback failure and recovery passed");
}
'''
    run_c('storage', PRELUDE+stubs+implementation.replace('rename(', 'test_rename(')+tests)


def refresh_test():
    source = read('components/weather_epd/weather_epd.c')
    stubs = r'''
#define ESP_ERR_INVALID_STATE 4
#define portMAX_DELAY 0xffffffff
static int s_task=1, s_cfg_mutex=1, notifications;
typedef struct { bool pending,running; esp_err_t last_result; uint32_t completed; } weather_refresh_status_t;
static weather_refresh_status_t s_refresh;
static struct { bool enabled; char api_key[8]; } s_remote_cfg={true,"key"};
static void xSemaphoreTake(int s,unsigned ticks) {}
static void xSemaphoreGive(int s) {}
static void xTaskNotifyGive(int s) { notifications++; }
'''
    tests = r'''
int main(void) {
  for(int i=0;i<20;i++)assert(weather_epd_refresh_remote_now()==ESP_OK);
  assert(s_refresh.pending && notifications==1);
  s_refresh.pending=false;s_refresh.running=true;
  for(int i=0;i<20;i++)assert(weather_epd_refresh_remote_now()==ESP_OK);
  assert(!s_refresh.pending && notifications==1);
  s_refresh.running=false;s_refresh.completed=1;s_refresh.last_result=ESP_OK;
  weather_refresh_status_t status;
  assert(weather_epd_get_refresh_status(&status)==ESP_OK);
  assert(status.completed==1 && status.last_result==ESP_OK);
  assert(weather_epd_refresh_remote_now()==ESP_OK && notifications==2);
  s_remote_cfg.enabled=false;
  assert(weather_epd_refresh_remote_now()==ESP_ERR_INVALID_STATE);
  s_task=0;assert(weather_epd_refresh_remote_now()==ESP_ERR_INVALID_STATE);
  puts("refresh: pending/running request coalescing, status and disabled service passed");
}
'''
    run_c('refresh', PRELUDE+stubs+function(source,'weather_epd_refresh_remote_now')+
          '\n'+function(source,'weather_epd_get_refresh_status')+tests)


def upload_test():
    web = read('main/network/web_server.c')
    storage = read('components/spiffs_storage/spiffs_storage.c')
    stubs = r'''
#define SPIFFS_CHUNK_SIZE 4
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define HTTPD_RESP_USE_STRLEN -1
typedef struct { size_t content_len,offset; int fail_recv, status; } httpd_req_t;
static char request_crc[16];
static int fail_write,fail_close;
static size_t test_fwrite(const void *p,size_t s,size_t n,FILE *f) {
  if(fail_write) { fail_write=0;return 0; } return fwrite(p,s,n,f);
}
static int test_fclose(FILE *f) {
  int rc=fclose(f);if(fail_close){fail_close=0;return -1;}return rc;
}
static int httpd_req_get_url_query_str(httpd_req_t*r,char *q,size_t n){q[0]=0;return ESP_OK;}
static int httpd_query_key_value(const char*q,const char*key,char*out,size_t n){
  if(!strcmp(key,"path")){snprintf(out,n,"upload-asset");return ESP_OK;}
  if(request_crc[0]){snprintf(out,n,"%s",request_crc);return ESP_OK;}return ESP_FAIL;
}
static bool is_path_allowed(const char *p){return true;}
static int httpd_req_recv(httpd_req_t*r,char*out,size_t n){
  if(r->fail_recv && r->offset>=4)return -1;
  memcpy(out,&"new data"[r->offset],n);r->offset+=n;return (int)n;
}
static int httpd_resp_send_err(httpd_req_t*r,int status,const char*b){r->status=status;return ESP_OK;}
static int httpd_resp_send_500(httpd_req_t*r){r->status=500;return ESP_OK;}
static int httpd_resp_set_type(httpd_req_t*r,const char*s){return ESP_OK;}
static int httpd_resp_send(httpd_req_t*r,const char*s,int n){r->status=200;return ESP_OK;}
typedef int cJSON;
static cJSON* cJSON_CreateObject(void){return NULL;}
static void cJSON_AddBoolToObject(cJSON*p,const char*k,bool v){}
static void cJSON_AddNumberToObject(cJSON*p,const char*k,double v){}
static void cJSON_AddStringToObject(cJSON*p,const char*k,const char*v){}
static char* cJSON_Print(cJSON*p){return calloc(1,1);}
static void cJSON_Delete(cJSON*p){}
'''
    tests = r'''
static void old_file(void){FILE*f=fopen("upload-asset","wb");assert(f);fputs("old",f);fclose(f);}
static void verify(const char *expected){char b[32]={0};FILE*f=fopen("upload-asset","rb");assert(f);fread(b,1,31,f);fclose(f);assert(!strcmp(b,expected));}
int main(void){
  remove("upload-asset");remove("upload-asset.upbak");remove("upload-asset.uptmp");
  for(int scenario=0;scenario<5;scenario++){
    old_file();httpd_req_t req={.content_len=8};request_crc[0]=0;
    if(scenario==0)req.fail_recv=1;
    if(scenario==1)fail_write=1;
    if(scenario==2)fail_close=1;
    if(scenario==3)strcpy(request_crc,"00000000");
    if(scenario==4)strcpy(request_crc,"invalid!");
    assert(fs_upload_handler(&req)!=ESP_OK);assert(req.status>=400);verify("old");
  }
  snprintf(request_crc,sizeof(request_crc),"%08x",~upload_crc32(UINT32_MAX,"new data",8));
  httpd_req_t req={.content_len=8};assert(fs_upload_handler(&req)==ESP_OK);
  assert(req.status==200);verify("new data");
  puts("upload: receive/short-write/close/CRC failures preserve old file; verified commit passed");
}
'''
    impl = (function(storage,'recover_file')+'\n'+function(storage,'spiffs_storage_replace_file')+
            '\n'+function(web,'upload_crc32')+'\n'+function(web,'fs_upload_handler'))
    impl = impl.replace('fwrite(', 'test_fwrite(').replace('fclose(', 'test_fclose(')
    run_c('upload', PRELUDE+stubs+impl+tests)


if __name__ == '__main__':
    audio_test()
    weather_test()
    storage_test()
    refresh_test()
    upload_test()
