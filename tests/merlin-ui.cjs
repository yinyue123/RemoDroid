// Plugin interaction tests with a firmware API fixture; native menu rendering requires a router login.
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '../packaging/merlin/deploy');
(async () => {
  const browser = await chromium.launch({executablePath:process.env.BROWSER_BIN, args:['--no-sandbox']});
  try {
    const page = await browser.newPage();
    const errors = [], calls = [];
    page.on('pageerror', e => errors.push(e.message));
    await page.route('**/*', async route => {
      const url = new URL(route.request().url());
      // This firmware returns 404 for CSS assets with query strings.
      if (url.pathname.endsWith('.css') && url.search) return route.fulfill({status:404,body:''});
      if (url.pathname === '/_api/') {
        const req = route.request().postDataJSON(); calls.push(req);
        const action = req.params[0];
        const data = {ok:true, running:false, autostart:false, pid:'', logs:'', version:'0.3.4', config: action==='config'?'[server]\n[agent]\n':'', message:''};
        if (action === 'enroll') {data.ok=false; data.message='配对码已过期';}
        return route.fulfill({contentType:'application/json', body:JSON.stringify({result:Buffer.from(JSON.stringify(data)).toString('base64')})});
      }
      let file = url.pathname === '/Module_remodroid.asp' ? 'webs/Module_remodroid.asp' : url.pathname.slice(1);
      if (file.startsWith('webs/') || file.startsWith('res/remodroid.')) {
        return route.fulfill({body:fs.readFileSync(path.join(root,file)), contentType:file.endsWith('.asp')?'text/html':file.endsWith('.js')?'application/javascript':'text/css'});
      }
      if (file==='state.js') return route.fulfill({contentType:'application/javascript', body:"var tabtitle=[''],tablink=[''];function show_menu(){menu_hook();window.menuInitialized=true;}"});
      return route.fulfill({body:'',contentType:file.endsWith('.js')?'application/javascript':'text/css'});
    });
    await page.goto('http://router.test/Module_remodroid.asp');
    await page.waitForFunction(()=>!document.getElementById('enroll').disabled);
    assert.equal(await page.evaluate(()=>window.menuInitialized),true);
    assert.equal(await page.evaluate(()=>tablink[0][1]),'Module_remodroid.asp');
    for (const selector of ['#TopBanner','#mainMenu','#subMenu','#tabMenu','#footer']) assert.equal(await page.locator(selector).count(),1);
    assert.equal(await page.locator('#advanced').getAttribute('open'),null);
    assert.equal(await page.locator('#enroll-command').evaluate(e=>getComputedStyle(e).backgroundColor),'rgb(78, 99, 104)');
    assert.equal(await page.locator('#enroll-command').evaluate(e=>getComputedStyle(e).resize),'none');
    const initialHeight=await page.locator('#enroll-command').evaluate(e=>e.clientHeight);
    await page.locator('#enroll-command').fill('long command\n'.repeat(8));
    assert.ok(await page.locator('#enroll-command').evaluate(e=>e.clientHeight)>initialHeight);
    await page.locator('#advanced summary').click();
    await page.locator('#config').fill('[server]\n'+ '; configuration\n'.repeat(80));
    assert.ok(await page.locator('#config').evaluate(e=>e.clientHeight)>=630);
    page.on('dialog', dialog=>dialog.accept());
    await page.locator('#reload').click();
    await page.waitForFunction(()=>!document.getElementById('save').disabled);
    for (const command of ['echo bad','remodroid enroll --panel http://host --code ABCD; reboot','remodroid enroll --panel http://host --code $(reboot)']) {
      await page.locator('#enroll-command').fill(command);
      await page.locator('#enroll').click();
      assert.match(await page.locator('#message').innerText(),/请粘贴完整命令/);
    }
    assert.equal(calls.filter(r=>r.params[0]==='enroll').length,0);
    await page.locator('#enroll-command').fill('$ remodroid enroll --panel http://192.0.2.1:5580 --code ABCD-EFGH');
    await page.locator('#enroll').click();
    await page.waitForFunction(()=>document.getElementById('message').textContent==='配对码已过期');
    const request=calls.find(r=>r.params[0]==='enroll');
    assert.deepEqual(JSON.parse(Buffer.from(request.fields.remodroid_enrollment,'base64').toString()),{panel:'http://192.0.2.1:5580',code:'ABCD-EFGH'});
    assert.deepEqual(errors,[]);
    console.log('PASS: native menu hook/containers, command parsing, injection rejection, enrollment RPC and error display');
  } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
