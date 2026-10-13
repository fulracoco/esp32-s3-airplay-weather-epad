// Exercise the actual page functions without requiring a connected device.
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const html=fs.readFileSync('data/www/index.html','utf8');
const script=html.match(/<script>([\s\S]*?)<\/script>/)[1];
new vm.Script(script); // Check the entire page script, not just test excerpts.
const ota=script.slice(script.indexOf('async function fetchWithTimeout('),script.indexOf('async function restart('));
const weather=script.slice(script.indexOf('var weatherRefreshing='),script.indexOf('async function toggleDisplayPage('));

function setup(fetch){
  let now=0,id=0;
  const messages=[],timers=new Map(),button={disabled:false};
  const context={AbortController,Date:{now:()=>now},console,
    document:{getElementById:id=>id==='fw-file'?{files:[{size:10}]}:button},
    msg:(id,text,type)=>messages.push({id,text,type}),fetch,
    setTimeout(fn,ms){const key=++id;timers.set(key,{fn,ms});
      if(ms<=3000)queueMicrotask(()=>{if(timers.delete(key)){now+=ms;fn();}});
      return key;},clearTimeout:key=>timers.delete(key)};
  vm.createContext(context);vm.runInContext(ota+weather,context);
  return {context,messages,button,timers,advance(ms){now+=ms;}};
}
function response(body,status=200){return {ok:status>=200&&status<300,status,text:async()=>JSON.stringify(body),json:async()=>body};}
const installed={success:true,firmware_sha256:'abc',firmware_version:'1.0',previous_boot_id:1};
const watchdog=setTimeout(()=>{console.error('Test did not settle');process.exit(1);},10000);

(async()=>{
  for(const mode of ['http','network','json','rejected']){
    const t=setup(async()=>{
      if(mode==='network')throw new Error('offline');
      if(mode==='json')return {ok:true,text:async()=>'not json'};
      return response({success:false},mode==='http'?500:200);
    });
    await t.context.startOTA();
    assert.equal(t.messages.at(-1).type,'err',mode);
    assert.equal(t.button.disabled,false);
    assert.equal(t.context.otaUploading,false);
  }
  let calls=0;
  const t=setup(async url=>{
    calls++;
    if(url==='/api/ota/update')return response(installed);
    if(calls===2)return response({firmware_sha256:'abc',boot_id:1}); // old boot must not pass
    if(calls===3)throw new Error('rebooting');
    return response({firmware_sha256:'abc',firmware_version:'1.0',boot_id:2});
  });
  await t.context.startOTA();assert.equal(calls,4);assert.equal(t.messages.at(-1).type,'ok');
  let resolveUpload;
  const duplicate=setup(()=>new Promise(resolve=>{resolveUpload=resolve;}));
  const first=duplicate.context.startOTA();await duplicate.context.startOTA();
  assert.equal(duplicate.messages.length,1);
  resolveUpload(response({},500));await first;
  const mismatch=setup(async url=>url==='/api/ota/update'?response(installed):response({firmware_sha256:'wrong',boot_id:2}));
  await mismatch.context.startOTA();assert.equal(mismatch.messages.at(-1).type,'err');
  // Header receipt does not cancel the body timeout.
  const timeout=setup(async(url,options)=>({ok:true,status:200,text:()=>new Promise((resolve,reject)=>{
    options.signal.addEventListener('abort',()=>reject(new Error('aborted')));
  })}));
  const waiting=timeout.context.startOTA();
  for(let i=0;i<10;i++)await Promise.resolve();
  for(const timer of timeout.timers.values())if(timer.ms===180000)timer.fn();
  await waiting;assert.equal(timeout.messages.at(-1).type,'err');assert.equal(timeout.button.disabled,false);
  let polls=0;
  const w=setup(async(url,options)=>{
    if(options.method==='POST')return response({success:true},202);
    polls++;return response({pending:false,running:polls===1,success:true});
  });
  await w.context.refreshWeather();assert.equal(polls,2);assert.equal(w.messages.at(-1).type,'ok');
  const wf=setup(async(url,options)=>response(options.method==='POST'?{success:true}:{pending:false,running:false,success:false,error:'HTTP failure'}));
  await wf.context.refreshWeather();assert.equal(wf.messages.at(-1).type,'err');
  console.log('web: syntax, OTA failures/retry/timeout/reboot identity, duplicate submit and weather polling passed');
})().catch(e=>{console.error(e);process.exitCode=1;}).finally(()=>clearTimeout(watchdog));
