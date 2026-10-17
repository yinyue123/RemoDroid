/* Use the authenticated Koolshare RPC endpoint; never interpolate configuration into HTML. */
'use strict';
(() => {
  const el = id => document.getElementById(id);
  let busy = false, dirty = false, loaded = false, running = false, auto = false;
  const encode = text => btoa(Array.from(new TextEncoder().encode(text), b => String.fromCharCode(b)).join(''));
  const decode = text => new TextDecoder().decode(Uint8Array.from(atob(text), c => c.charCodeAt(0)));
  function message(text, error = false) {
    el('message').hidden = !text;
    el('message').textContent = text;
    el('message').className = error ? 'error' : '';
  }
  function controls() {
    for (const id of ['start','restart','stop','save','reload','refresh','clearlog','autostart']) el(id).disabled = busy;
    el('start').disabled = busy || !loaded || running;
    el('stop').disabled = busy || !running;
    el('restart').disabled = busy || !loaded || !running;
    el('save').disabled = busy || !loaded;
  }
  function render(data, action) {
    running = data.running; auto = data.autostart;
    el('state').className = running ? 'running' : '';
    el('state').querySelector('strong').textContent = running ? '代理正在运行' : '代理未启动';
    el('process').textContent = running ? `进程 PID ${data.pid} · 面板连接状态请查看日志` : '保存连接配置后，点击“启动”';
    el('autostart').checked = auto;
    el('version').textContent = `Agent ${data.version}`;
    el('logs').textContent = data.logs || '还没有日志。配置完成并启动后，连接信息会显示在这里。';
    el('updated').textContent = `更新于 ${new Date().toLocaleTimeString()}`;
    if (data.ok && (action === 'config' || action === 'save')) {
      el('config').value = data.config;
      loaded = true; dirty = false;
    }
    if (data.message) message(data.message, !data.ok);
  }
  async function request(action, fields = {}, silent = false) {
    if (busy) return;
    busy = true; controls();
    try {
      const response = await fetch('/_api/', {
        method:'POST', credentials:'same-origin', cache:'no-store',
        headers:{'Content-Type':'application/json'},
        body:JSON.stringify({id:Math.floor(Math.random()*90000000)+10000000,
          method:'remodroid_control.sh', params:[action], fields})
      });
      if (!response.ok) throw new Error(`路由器返回 HTTP ${response.status}`);
      const envelope = await response.json();
      if (typeof envelope.result !== 'string') throw new Error('管理接口没有返回有效结果');
      const data = JSON.parse(decode(envelope.result.trim()));
      render(data, action);
    } catch (error) {
      // A login redirect returns HTML rather than JSON; don't silently display stale status.
      el('state').className = '';
      el('state').querySelector('strong').textContent = '无法读取状态';
      el('process').textContent = '检查连接和路由器登录状态后，点击刷新';
      if (!silent || !el('message').textContent) message(`操作未完成：${error.message}。如果登录已过期，请返回软件中心重新登录。`, true);
    } finally { busy = false; controls(); }
  }
  el('config').addEventListener('input', () => { dirty = true; });
  el('save').onclick = () => {
    if (new TextEncoder().encode(el('config').value).length > 16384) return message('配置不能超过 16KB', true);
    request('save', {remodroid_payload:encode(el('config').value)});
  };
  el('reload').onclick = () => { if (!dirty || confirm('放弃未保存的修改，重新读取配置？')) request('config'); };
  for (const action of ['start','stop','restart','clearlog']) el(action).onclick = () => {
    if ((action === 'start' || action === 'restart') && dirty) return message('有未保存的配置，请先点击“保存配置”。', true);
    request(action);
  };
  el('refresh').onclick = () => request(loaded ? 'status' : 'config');
  el('autostart').onchange = () => { const wanted = el('autostart').checked; el('autostart').checked = auto; request(wanted ? 'auto_on' : 'auto_off'); };
  window.addEventListener('beforeunload', event => { if (dirty) { event.preventDefault(); event.returnValue = ''; } });
  request('config');
  setInterval(() => { if (!document.hidden && !busy) request('status', {}, true); }, 5000);
})();
