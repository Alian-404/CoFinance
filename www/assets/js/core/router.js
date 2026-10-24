import { getToken } from './session.js';

export function parseRoute(hash = window.location.hash) {
  const raw = String(hash || '').replace(/^#/, '') || '/';
  const clean = raw.split('?')[0];
  const parts = clean.split('/').filter(Boolean);
  if (parts.length === 0) {
    return { name: getToken() ? 'groups' : 'auth', mode: 'login', path: '/' };
  }
  if (parts[0] === 'login' || parts[0] === 'register') {
    return { name: 'auth', mode: parts[0], path: `/${parts[0]}` };
  }
  if (parts[0] === 'groups' && parts.length === 1) {
    return { name: 'groups', path: '/groups' };
  }
  if (parts[0] === 'groups' && /^\d+$/.test(parts[1] || '')) {
    const tab = ['overview', 'ledger', 'chat', 'settings'].includes(parts[2])
      ? parts[2]
      : 'overview';
    return { name: 'workspace', gid: parts[1], tab, path: `/groups/${parts[1]}/${tab}` };
  }
  return { name: 'not-found', path: clean };
}

export function navigate(path, { replace = false } = {}) {
  const target = `#${path.startsWith('/') ? path : `/${path}`}`;
  if (window.location.hash === target) {
    window.dispatchEvent(new HashChangeEvent('hashchange'));
    return;
  }
  if (replace) {
    window.history.replaceState(null, '', target);
    window.dispatchEvent(new HashChangeEvent('hashchange'));
  } else {
    window.location.hash = target;
  }
}

export function groupPath(gid, tab = 'overview') {
  return `/groups/${gid}/${tab}`;
}

export function startRouter(handler) {
  const run = () => handler(parseRoute());
  window.addEventListener('hashchange', run);
  run();
  return () => window.removeEventListener('hashchange', run);
}
