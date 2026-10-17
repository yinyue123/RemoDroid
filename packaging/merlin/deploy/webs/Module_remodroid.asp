<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"><meta http-equiv="X-UA-Compatible" content="IE=edge">
<meta http-equiv="Cache-Control" content="no-store">
<title>RemoDroid - Merlin Software Center</title>
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
<div class="rd-title"><h1 class="formfonttitle">RemoDroid</h1><a href="/Module_Softcenter.asp" title="Back to Software Center"><img src="/images/backprev.png" width="40" height="40" alt="Back to Software Center"></a></div>
<div class="splitLine"></div>
<p class="rd-description">Android remote-control agent. Paste the pairing command from your panel to add this router to it.</p>
<table class="FormTable" width="100%" cellpadding="4" cellspacing="0"><thead><tr><td colspan="2">Status</td></tr></thead><tbody>
<tr><th>State</th><td><div id="state" role="status"><strong>Reading status…</strong></div><p id="process">Connecting to the router…</p><span id="version"></span></td></tr>
<tr><th>Agent control</th><td class="rd-actions"><button id="start" class="button_gen" disabled>Start</button> <button id="restart" class="button_gen" disabled>Restart</button> <button id="stop" class="button_gen" disabled>Stop</button></td></tr>
<tr><th>Start on boot</th><td><label><input id="autostart" type="checkbox" disabled> Start automatically on boot</label></td></tr>
</tbody></table>
<div id="message" role="status" aria-live="polite" hidden></div>
<table class="FormTable" width="100%" cellpadding="4" cellspacing="0"><thead><tr><td colspan="2">Pair with a panel</td></tr></thead><tbody>
<tr><th><label for="enroll-command">Pairing command</label></th><td><textarea id="enroll-command" rows="2" spellcheck="false" autocomplete="off" placeholder="remodroid enroll --panel http://your-panel:5580 --code XXXX-XXXX"></textarea><p>Paste the whole command from the panel; it starts automatically once paired. Stop the agent before switching panels.</p><button id="enroll" class="button_gen" disabled>Pair and start</button></td></tr>
</tbody></table>
<details id="advanced"><summary>Advanced · agent.ini</summary>
<p>Paste an existing config; local paths are adjusted on save. It contains the device secret — don't share screenshots.</p>
<textarea id="config" spellcheck="false" autocomplete="off" autocapitalize="off" aria-label="agent.ini" placeholder="Reading config…"></textarea>
<div class="rd-toolbar"><span>Restart the agent for changes to take effect.</span><button id="reload" class="button_gen">Reload</button><button id="save" class="button_gen" disabled>Save config</button></div>
<p>When migrating from a NAS, first stop any agent there that uses the same identity.</p>
</details>
<table class="FormTable" width="100%" cellpadding="4" cellspacing="0"><thead><tr><td>Logs</td></tr></thead><tbody><tr><td>
<div class="rd-toolbar"><span id="updated">Updates every 5s</span><button id="refresh" class="button_gen">Refresh</button><button id="clearlog" class="button_gen">Clear</button></div>
<pre id="logs" tabindex="0" aria-label="Recent logs">Reading logs…</pre>
<p>Kept in /tmp (RAM) only, never written to JFFS. Shows the last 150 lines; cleared when the router reboots.</p>
</td></tr></tbody></table>
</main>
</td></tr></table></td></tr></table></td><td width="10"></td>
</tr></table>
<div id="footer"></div>
</body></html>
