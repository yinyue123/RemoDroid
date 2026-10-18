'use strict';
'require view';
'require fs';
'require ui';
'require rpc';
'require poll';

/* Services → RemoDroid: pair the agent with a panel, see whether the panel and the
 * phone are connected, start/stop it and read its log. Ships in the agent's ipk. */

var AGENT = '/usr/bin/remodroid-agent', INIT = '/etc/init.d/remodroid-agent', CONF = '/etc/remodroid/agent.ini';

var callServiceList = rpc.declare({
	object: 'service',
	method: 'list',
	params: [ 'name' ],
	expect: { '': {} }
});

function iniValue(ini, key) {
	var m = (ini || '').match(new RegExp('^[ \\t]*' + key + '[ \\t]*=[ \\t]*([^ \\t;\\r\\n]*)', 'm'));
	return m ? m[1] : '';
}

function load() {
	return Promise.all([
		L.resolveDefault(callServiceList('remodroid-agent'), {}),
		L.resolveDefault(fs.exec(INIT, [ 'enabled' ]), { code: 1 }),
		L.resolveDefault(fs.read(CONF), ''),
		L.resolveDefault(fs.exec(AGENT, [ 'status' ]), {}),
		L.resolveDefault(fs.exec('/sbin/logread', [ '-e', 'remodroid' ]), {})
	]).then(function(r) {
		var inst = ((r[0]['remodroid-agent'] || {}).instances) || {}, running = false, status = null;
		for (var k in inst)
			running = running || !!inst[k].running;
		try { status = r[3].code === 0 ? JSON.parse(r[3].stdout) : null; } catch (e) {}
		return {
			running: running,
			enabled: r[1].code === 0,
			url: iniValue(r[2], 'url'),
			paired: iniValue(r[2], 'device_id') !== '',
			status: status,
			log: (r[4].stdout || '').trim().split('\n').slice(-100).join('\n')
		};
	});
}

function panelText(s) {
	if (!s.paired)
		return _('Not paired yet. Paste the pairing command below.');
	if (s.status && s.status.server_connected)
		return _('Connected to %s').format(s.status.server_url);
	return s.running ? _('Connecting to %s…').format(s.url) : _('Paired with %s').format(s.url);
}

function phoneText(s) {
	var p = s.status && s.status.phone;
	if (!s.running)
		return '—';
	if (!p || !p.attached)
		return _('No phone. Plug it in over USB with USB debugging turned on.');
	if (p.adb_state === 'unauthorized')
		return _('Tap “Allow USB debugging” on the phone.');
	return '%s · Android %s'.format(p.model || p.serial, p.android || '?');
}

return view.extend({
	load: load,

	run: function(argv) {
		return fs.exec(argv[0], argv.slice(1)).then(function(res) {
			if (res.code !== 0)
				throw new Error((res.stderr || res.stdout || '').trim() || _('Command failed: %s').format(argv.join(' ')));
			return res;
		});
	},

	action: function(fn) {
		var self = this;
		return Promise.resolve().then(fn).catch(function(e) {
			ui.addNotification(null, E('p', e.message), 'danger');
		}).then(function() {
			return load().then(L.bind(self.update, self));
		});
	},

	handleService: function(verb) {
		return this.action(L.bind(function() { return this.run([ INIT, verb ]); }, this));
	},

	handlePair: function() {
		var cmd = this.pairInput.value.trim(),
		    panel = (cmd.match(/--panel\s+(https?:\/\/\S+)/) || [])[1],
		    code = (cmd.match(/--code\s+([A-Za-z0-9-]+)/) || [])[1],
		    self = this;

		if (!panel || !code) {
			ui.addNotification(null, E('p', _('Paste the whole command from the panel: remodroid enroll --panel https://… --code XXXX-XXXX')), 'warning');
			return Promise.resolve();
		}
		/* Enroll with the agent stopped, so it doesn't wait for the reconnect, then start it. */
		return this.action(function() {
			return self.run([ INIT, 'stop' ])
				.then(function() { return self.run([ AGENT, 'enroll', '--panel', panel, '--code', code ]); })
				.then(function() { return self.run([ INIT, 'enable' ]); })
				.then(function() { return self.run([ INIT, 'start' ]); })
				.then(function() {
					self.pairInput.value = '';
					ui.addNotification(null, E('p', _('Paired. The agent is starting and will connect to the panel.')), 'info');
				}, function(e) {
					return self.run([ INIT, 'start' ]).catch(function() {}).then(function() { throw e; });
				});
		});
	},

	update: function(s) {
		this.rows.service.textContent = s.running
			? (s.status ? _('Running · version %s').format(s.status.version) : _('Running'))
			: _('Stopped');
		this.rows.panel.textContent = panelText(s);
		this.rows.phone.textContent = phoneText(s);
		this.boot.checked = s.enabled;
		this.buttons.start.disabled = s.running;
		this.buttons.restart.disabled = this.buttons.stop.disabled = !s.running;
		this.log.value = s.log || _('No log entries yet.');
	},

	render: function(s) {
		var self = this;
		var row = function(label, content) {
			var td = E('td', { 'class': 'td left' }, content);
			return [ E('tr', { 'class': 'tr' }, [ E('td', { 'class': 'td left', 'width': '33%' }, label), td ]), td ];
		};
		var service = row(_('Service')), panel = row(_('Panel')), phone = row(_('Phone'));
		var button = function(cls, label, verb) {
			return E('button', { 'class': 'cbi-button ' + cls, 'click': ui.createHandlerFn(self, 'handleService', verb) }, label);
		};

		this.rows = { service: service[1], panel: panel[1], phone: phone[1] };
		this.buttons = {
			start: button('cbi-button-apply', _('Start'), 'start'),
			restart: button('cbi-button-action', _('Restart'), 'restart'),
			stop: button('cbi-button-negative', _('Stop'), 'stop')
		};
		this.boot = E('input', { 'type': 'checkbox', 'change': function(ev) {
			self.handleService(ev.target.checked ? 'enable' : 'disable');
		} });
		this.pairInput = E('input', {
			'type': 'text', 'class': 'cbi-input-text', 'style': 'width:100%', 'spellcheck': 'false',
			'placeholder': 'remodroid enroll --panel https://your.panel --code XXXX-XXXX'
		});
		var boot = row(_('Start on boot'), this.boot),
		    control = row(_('Control'), [ this.buttons.start, ' ', this.buttons.restart, ' ', this.buttons.stop ]);
		this.log = E('textarea', { 'readonly': 'readonly', 'wrap': 'off', 'rows': 15, 'style': 'width:100%;font-family:monospace;font-size:12px' });

		var node = E([], [
			E('h2', _('RemoDroid')),
			E('div', { 'class': 'cbi-map-descr' }, _('Control the Android phone on this router\'s USB port from Safari on your iPhone.')),

			E('div', { 'class': 'cbi-section' }, [
				E('h3', _('Status')),
				E('table', { 'class': 'table' }, [ service[0], panel[0], phone[0], boot[0], control[0] ])
			]),

			E('div', { 'class': 'cbi-section' }, [
				E('h3', _('Pair with a panel')),
				E('div', { 'class': 'cbi-value' }, [
					E('label', { 'class': 'cbi-value-title' }, _('Pairing command')),
					E('div', { 'class': 'cbi-value-field' }, [
						this.pairInput,
						E('div', { 'class': 'cbi-value-description' },
							_('On your panel, open Agents → Add and paste the command here. The code works once, for 10 minutes.'))
					])
				]),
				E('div', { 'class': 'cbi-page-actions' },
					E('button', { 'class': 'cbi-button cbi-button-save', 'click': ui.createHandlerFn(this, 'handlePair') }, _('Pair')))
			]),

			E('div', { 'class': 'cbi-section' }, [
				E('h3', _('Log')),
				this.log
			])
		]);

		this.update(s);
		poll.add(function() { return load().then(L.bind(self.update, self)); }, 5);
		return node;
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
