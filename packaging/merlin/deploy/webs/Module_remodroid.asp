<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Cache-Control" content="no-store">
<title>RemoDroid · 梅林</title>
<link rel="icon" href="/res/icon-remodroid.png">
<link rel="stylesheet" href="/res/remodroid.css">
<script defer src="/res/remodroid.js"></script>
</head>
<body>
<main>
<header><a class="back" href="/Module_Softcenter.asp">‹ 返回软件中心</a><span class="firmware">ASUS / 梅林</span></header>
<section class="intro"><img src="/res/icon-remodroid.png" width="56" height="56" alt=""><div><h1>RemoDroid</h1><p>连接你的 Android 手机，让远程面板随时可用。</p></div><span id="version">代理管理</span></section>
<section class="status-panel" aria-label="运行状态"><div><div id="state" role="status"><i></i><strong>正在读取状态…</strong></div><p id="process">正在连接路由器</p></div><div class="actions"><button id="start" class="primary" disabled>启动</button><button id="restart" disabled>重启</button><button id="stop" disabled>停止</button></div></section>
<div id="message" role="status" aria-live="polite" hidden></div>
<section class="settings"><div class="section-head"><h2>启动设置</h2><label class="check"><input id="autostart" type="checkbox" disabled>开机启动</label></div><p>先保存服务器地址、设备 ID 和密钥，再启动代理。进程正在运行不代表已连接面板，请结合下方日志确认。</p></section>
<section class="settings"><div class="section-head"><h2>连接配置</h2><button id="reload" class="quiet">重新读取</button></div>
<p>粘贴你的 <code>agent.ini</code>，或直接编辑下方模板。保存时会自动调整程序文件、ADB 密钥和通信文件的路径。</p>
<label for="config" class="editor-label">agent.ini <span>含设备密钥，请勿分享截图</span></label>
<textarea id="config" spellcheck="false" autocomplete="off" autocapitalize="off" aria-describedby="config-note" placeholder="正在读取配置…"></textarea>
<div class="editor-foot"><p id="config-note">配置保存在路由器中，重启后仍保留。保存后点击“重启”应用修改。</p><button id="save" class="primary" disabled>保存配置</button></div>
<p class="migration">从 NAS 迁移时，请先停止 NAS 上使用同一设备身份的 agent，再启动本机。</p>
</section>
<section class="settings"><div class="section-head"><h2>运行日志</h2><div class="actions"><span id="updated">每 5 秒更新</span><button id="refresh" class="quiet">刷新</button><button id="clearlog" class="quiet">清空</button></div></div><pre id="logs" tabindex="0" aria-label="最近运行日志">还没有日志。配置完成并启动后，连接信息会显示在这里。</pre><p>显示最近 150 行，日志存放在内存中，重启路由器后清空。</p></section>
<footer>RemoDroid · Koolshare / KoolCenter 插件 <span>管理页沿用路由器的登录验证</span></footer>
</main>
</body></html>
