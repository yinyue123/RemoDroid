<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"><meta http-equiv="X-UA-Compatible" content="IE=edge">
<meta http-equiv="Cache-Control" content="no-store">
<title>RemoDroid - 梅林软件中心</title>
<link rel="icon" href="/images/favicon.png">
<link rel="stylesheet" href="/index_style.css">
<link rel="stylesheet" href="/form_style.css">
<link rel="stylesheet" href="/res/softcenter.css">
<link rel="stylesheet" href="/res/remodroid.css">
<script src="/js/jquery.js"></script>
<script src="/state.js"></script>
<script src="/general.js"></script>
<script src="/popup.js"></script>
<script src="/help.js"></script>
<script src="/validator.js"></script>
<script src="/client_function.js"></script>
<script src="/res/softcenter.js"></script>
<script>
function menu_hook() {
    tabtitle[tabtitle.length - 1] = ['', 'RemoDroid'];
    tablink[tablink.length - 1] = ['', 'Module_remodroid.asp'];
}
</script>
<script defer src="/res/remodroid.js"></script>
</head>
<body onload="show_menu();">
<div id="TopBanner"></div>
<div id="Loading" class="popup_bg"></div>
<table class="content" align="center" cellpadding="0" cellspacing="0"><tr>
<td width="17">&nbsp;</td>
<td valign="top" width="202"><div id="mainMenu"></div><div id="subMenu"></div></td>
<td valign="top"><div id="tabMenu" class="submenuBlock"></div>
<table width="98%" border="0" align="left" cellpadding="0" cellspacing="0"><tr><td valign="top">
<table width="760" border="0" cellpadding="5" cellspacing="0" class="FormTitle" id="FormTitle"><tr><td bgcolor="#4D595D" valign="top">
<main id="remodroid">
<div class="rd-title"><h1 class="formfonttitle">RemoDroid</h1><a href="/Module_Softcenter.asp" title="返回软件中心"><img src="/images/backprev.png" width="40" height="40" alt="返回软件中心"></a></div>
<div class="splitLine"></div>
<p class="rd-description">Android 远程连接代理。复制面板提供的配对命令，即可将本路由器添加到面板。</p>
<table class="FormTable" width="100%" cellpadding="4" cellspacing="0"><thead><tr><td colspan="2">状态</td></tr></thead><tbody>
<tr><th>运行状态</th><td><div id="state" role="status"><strong>正在读取状态…</strong></div><p id="process">正在连接路由器</p><span id="version"></span></td></tr>
<tr><th>代理控制</th><td class="rd-actions"><button id="start" class="button_gen" disabled>启动</button> <button id="restart" class="button_gen" disabled>重启</button> <button id="stop" class="button_gen" disabled>停止</button></td></tr>
<tr><th>开机启动</th><td><label><input id="autostart" type="checkbox" disabled> 启用开机自动启动</label></td></tr>
</tbody></table>
<div id="message" role="status" aria-live="polite" hidden></div>
<table class="FormTable" width="100%" cellpadding="4" cellspacing="0"><thead><tr><td colspan="2">绑定面板</td></tr></thead><tbody>
<tr><th><label for="enroll-command">配对命令</label></th><td><textarea id="enroll-command" rows="2" spellcheck="false" autocomplete="off" placeholder="remodroid enroll --panel http://你的面板:5580 --code XXXX-XXXX"></textarea><p>粘贴面板生成的整行命令，配对成功后自动启动。更换面板前请先停止代理。</p><button id="enroll" class="button_gen" disabled>绑定并启动</button></td></tr>
</tbody></table>
<details id="advanced"><summary>高级配置 · agent.ini</summary>
<p>可粘贴已有配置；保存时自动调整本机路径。含设备密钥，请勿分享截图。</p>
<textarea id="config" spellcheck="false" autocomplete="off" autocapitalize="off" aria-label="agent.ini" placeholder="正在读取配置…"></textarea>
<div class="rd-toolbar"><span>保存后重启代理生效。</span><button id="reload" class="button_gen">重新读取</button><button id="save" class="button_gen" disabled>保存配置</button></div>
<p>从 NAS 迁移时，请先停止 NAS 上使用同一身份的 agent。</p>
</details>
<table class="FormTable" width="100%" cellpadding="4" cellspacing="0"><thead><tr><td>运行日志</td></tr></thead><tbody><tr><td>
<div class="rd-toolbar"><span id="updated">每 5 秒更新</span><button id="refresh" class="button_gen">刷新</button><button id="clearlog" class="button_gen">清空</button></div>
<pre id="logs" tabindex="0" aria-label="最近运行日志">正在读取日志…</pre>
<p>仅保存在 /tmp 内存中，不写入 JFFS。显示最近 150 行，重启路由器后清空。</p>
</td></tr></tbody></table>
</main>
</td></tr></table></td></tr></table></td><td width="10"></td>
</tr></table>
<div id="footer"></div>
</body></html>
