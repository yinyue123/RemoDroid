// RemoDroid panel on Cloudflare Workers.
//
// Static files (web/: the client, wizard.html, agent.html) are served as assets.
// Everything with state (login, setup, routers, WebSockets, signaling) goes to one
// Durable Object, the Hub (hub.js), which speaks the same protocol as remodroid-server,
// so the web client and remodroid-agent work unchanged.
import { Hub } from './hub.js';

export { Hub };

export default {
  async fetch(request, env) {
    const url = new URL(request.url), p = url.pathname;
    const dynamic = p === '/' || p === '/index.html' || p === '/login' || p.startsWith('/api/') || p.startsWith('/ws/');
    if (!dynamic)
      return env.ASSETS.fetch(request);
    const hub = env.HUB.get(env.HUB.idFromName('hub'));
    const res = await hub.fetch(request);
    // the Hub approved a page (logged in, set up): serve it, keeping its Set-Cookie
    if (res.headers.get('x-rd-asset')) {
      const page = await env.ASSETS.fetch(new Request(new URL('/', url), request)); // "/index.html" would redirect to "/"
      const out = new Response(page.body, page);
      const cookie = res.headers.get('set-cookie');
      if (cookie) out.headers.append('set-cookie', cookie);
      out.headers.set('cache-control', 'no-cache');
      return out;
    }
    return res;
  },
};
