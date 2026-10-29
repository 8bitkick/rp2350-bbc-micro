const {chromium}=require('playwright');
const fs=require('fs'),path=require('path'),cp=require('child_process'),zlib=require('zlib'),os=require('os');
(async()=>{
const root=path.resolve(__dirname,'../..'),temp=fs.mkdtempSync(path.join(os.tmpdir(),'webdiag-'));
const b=await chromium.launch({executablePath:process.env.CHROME_PATH,headless:true});
try{for(const enabled of [false,true]){
const out=path.join(temp,'web.c');
cp.execFileSync('python3',[path.join(root,'tools/embed_web.py'),path.join(root,'src/common/api/web/index.html'),out,'--webdiag='+(enabled?'ON':'OFF')]);
const html=zlib.gunzipSync(Buffer.from([...fs.readFileSync(out,'utf8').matchAll(/0x([0-9a-f]{2})/g)].map(m=>parseInt(m[1],16)))).toString();
const page=await b.newPage();let errors=[];page.on('pageerror',e=>errors.push(e.message));
await page.route('**/*',route=>{
const u=new URL(route.request().url());
if(u.pathname==='/')return route.fulfill({contentType:'text/html',body:html});
return route.fulfill({contentType:'application/json',body:JSON.stringify({machine:'c64',sound:{on:true,vol:100},ambient:{on:true,bright:40},c64:{build:'r11',recovery:true,phase:6,pc:'0',cfsr:'0'}})});
});
await page.goto('http://presto.test/');await page.waitForFunction(()=>document.querySelector('#mname').textContent==='Commodore 64');
for(const id of ['tracepanel','soundtest','c64notice'])if(await page.locator('#'+id).isVisible()!==enabled)throw Error(id+' wrong WEBDIAG state');
if(!await page.locator('#lights').isVisible()||!await page.locator('#sound').isVisible())throw Error('Regular controls hidden');
if(!enabled && await page.evaluate(()=>sessionStorage.getItem('presto-trace'))!==null)throw Error('Disabled mode records trace');
if(errors.length)throw Error(errors.join('\n'));await page.close();
}}finally{await b.close();fs.rmSync(temp,{recursive:true,force:true});}
console.log('Generated WEBDIAG ON/OFF pages, controls and disabled trace storage passed');
})().catch(e=>{console.error(e);process.exit(1)});
