/* Use the authenticated Koolshare RPC endpoint; never interpolate configuration into HTML. */
'use strict';
(() => {
  const el = id => document.getElementById(id);
  let busy = false, dirty = false, loaded = false, running = false, auto = false;
  const encode = text => btoa(Array.from(new TextEncoder().encode(text), b => String.fromCharCode(b)).join(''));
  const decode = text => new TextDecoder().decode(Uint8Array.from(atob(text), c => c.charCodeAt(0)));
  function fitEditor(id) {
    const field = el(id);
    if (!field.clientWidth) return;
    field.style.height = 'auto';
    field.style.height = `${Math.min(field.scrollHeight + 2, id === 'config' ? 640 : 180)}px`;
  }
  function message(text, error = false) {
    el('message').hidden = !text;
    el('message').textContent = text;
    el('message').className = error ? 'error' : '';
  }
  function controls() {
    for (const id of ['start','restart','stop','save','reload','refresh','clearlog','autostart','enroll']) el(id).disabled = busy;
    el('enroll').disabled = busy || !loaded || running;
    el('start').disabled = busy || !loaded || running;
    el('stop').disabled = busy || !running;
    el('restart').disabled = busy || !loaded || !running;
    el('save').disabled = busy || !loaded;
    el('config').disabled = !loaded;
  }
  function render(data, action) {
    running = data.running; auto = data.autostart;
    el('state').className = running ? 'running' : '';
    el('state').querySelector('strong').textContent = running ? 'Agent running' : 'Agent stopped';
    el('process').textContent = running ? `Process PID ${data.pid} · see logs for panel connection status` : 'Save the connection config, then click Start';
    el('autostart').checked = auto;
    el('version').textContent = `Agent ${data.version}`;
    el('logs').textContent = data.logs || 'No logs yet. Connection details appear here once configured and started.';
    el('updated').textContent = `Updated ${new Date().toLocaleTimeString()}`;
    if (data.config && (action === 'config' || action === 'save' || action === 'enroll')) {
      el('config').value = data.config;
      fitEditor('config');
      loaded = true; dirty = false;
    }
    if (data.ok && action === 'enroll') { el('enroll-command').value = ''; fitEditor('enroll-command'); }
    if (data.message) message(data.message, !data.ok);
  }
  async function request(action, fields = {}, silent = false) {
    if (busy) return;
    busy = true; controls();
    if (action === 'config' || action === 'save' || action === 'enroll') el('config').disabled = true;
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 25000);
    try {
      const response = await fetch('/_api/', {
        method:'POST', credentials:'same-origin', cache:'no-store', signal:controller.signal,
        headers:{'Content-Type':'application/json'},
        body:JSON.stringify({id:Math.floor(Math.random()*90000000)+10000000,
          method:'remodroid_control.sh', params:[action], fields})
      });
      if (!response.ok) throw new Error(`Router returned HTTP ${response.status}`);
      const envelope = await response.json();
      if (typeof envelope.result !== 'string') throw new Error('The management API returned no valid result');
      const data = JSON.parse(decode(envelope.result.trim()));
      render(data, action);
    } catch (error) {
      // A login redirect returns HTML rather than JSON; don't silently display stale status.
      el('state').className = '';
      el('state').querySelector('strong').textContent = "Can't read status";
      el('process').textContent = "Check the connection and that you're logged in, then click Refresh";
      if (!silent || !el('message').textContent) message(`Action didn't complete: ${error.message}. If your login expired, go back to the software center and sign in again.`, true);
    } finally { clearTimeout(timeout); busy = false; controls(); }
  }
  el('config').addEventListener('input', () => { dirty = true; fitEditor('config'); });
  el('enroll-command').addEventListener('input', () => fitEditor('enroll-command'));
  el('advanced').addEventListener('toggle', () => fitEditor('config'));
  window.addEventListener('resize', () => { fitEditor('config'); fitEditor('enroll-command'); });
  fitEditor('enroll-command');
  el('enroll').onclick = () => {
    if (dirty) return message('Save the advanced config first, or reload to discard changes.', true);
    const command = el('enroll-command').value.trim().replace(/^\$\s*/, '');
    const match = /^(?:remodroid|remodroid-agent)\s+enroll\s+--panel\s+(https?:\/\/[A-Za-z0-9.\[\]:_-]+\/?)[ \t]+--code\s+([A-Za-z0-9-]{1,64})\s*$/.exec(command);
    if (!match) return message('Paste the full command: remodroid enroll --panel http://panel-host:port --code XXXX-XXXX', true);
    message('Pairing with the panel…');
    request('enroll', {remodroid_enrollment:encode(JSON.stringify({panel:match[1], code:match[2]}))});
  };
  el('save').onclick = () => {
    if (new TextEncoder().encode(el('config').value).length > 16384) return message("Config can't exceed 16KB", true);
    request('save', {remodroid_payload:encode(el('config').value)});
  };
  el('reload').onclick = () => { if (!dirty || confirm('Discard unsaved changes and reload the config?')) request('config'); };
  for (const action of ['start','stop','restart','clearlog']) el(action).onclick = () => {
    if ((action === 'start' || action === 'restart') && dirty) return message('You have unsaved config; click Save config first.', true);
    request(action);
  };
  el('refresh').onclick = () => request(loaded ? 'status' : 'config');
  el('autostart').onchange = () => { const wanted = el('autostart').checked; el('autostart').checked = auto; request(wanted ? 'auto_on' : 'auto_off'); };
  window.addEventListener('beforeunload', event => { if (dirty) { event.preventDefault(); event.returnValue = ''; } });
  request('config');
  setInterval(() => { if (!document.hidden && !busy) request(loaded ? 'status' : 'config', {}, true); }, 5000);
})();
