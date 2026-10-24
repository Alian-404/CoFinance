import { html, mount } from './core/dom.js';
import { api, setUnauthorizedHandler } from './core/api.js';
import { getToken, clearToken } from './core/session.js';
import { state, resetProtectedState } from './core/store.js';
import { navigate, startRouter } from './core/router.js';
import { toast } from './core/ui.js';
import { renderAuth } from './views/auth.js';
import { renderGroups } from './views/groups.js';
import { renderWorkspace } from './views/workspace.js';

const app = document.getElementById('app');
let mounted = null;
let routeSequence = 0;

function cleanupMounted() {
  if (mounted?.view?.cleanup) {
    mounted.view.cleanup();
  }
  mounted = null;
}

function signedOutState() {
  resetProtectedState();
  clearToken();
}

function signedOut() {
  signedOutState();
}

setUnauthorizedHandler(() => {
  signedOutState();
  cleanupMounted();
  toast('登录状态已失效，请重新登录', 'warning');
  navigate('/login', { replace: true });
});

async function ensureSession() {
  if (state.user) {
    return true;
  }
  if (!getToken()) {
    return false;
  }
  try {
    state.user = await api.me();
    return true;
  } catch {
    return false;
  }
}

async function handleRoute(route) {
  const sequence = ++routeSequence;
  document.title = 'CoFinance · 一起把账算清楚';

  if (route.name === 'auth') {
    if (getToken()) {
      const valid = await ensureSession();
      if (sequence !== routeSequence) {
        return;
      }
      if (valid) {
        navigate('/groups', { replace: true });
        return;
      }
    }
    if (mounted?.key === `auth:${route.mode}`) {
      return;
    }
    cleanupMounted();
    const view = renderAuth({
      mode: route.mode,
      onAuthenticated(user) {
        state.user = user;
      },
    });
    mounted = { key: `auth:${route.mode}`, view };
    document.title = `${route.mode === 'register' ? '注册' : '登录'} · CoFinance`;
    return;
  }

  if (route.name === 'groups') {
    const authenticated = await ensureSession();
    if (sequence !== routeSequence) {
      return;
    }
    if (!authenticated) {
      navigate('/login', { replace: true });
      return;
    }
    if (mounted?.key === 'groups') {
      return;
    }
    cleanupMounted();
    const view = renderGroups({ onSignedOut: signedOut });
    mounted = { key: 'groups', view };
    document.title = '共同账本 · CoFinance';
    return;
  }

  if (route.name === 'workspace') {
    const authenticated = await ensureSession();
    if (sequence !== routeSequence) {
      return;
    }
    if (!authenticated) {
      navigate('/login', { replace: true });
      return;
    }
    if (mounted?.key === `workspace:${route.gid}`) {
      mounted.view.setTab(route.tab);
      document.title = `${state.currentGroup?.name || '账本'} · CoFinance`;
      return;
    }
    cleanupMounted();
    const view = renderWorkspace({ gid: route.gid, tab: route.tab, onSignedOut: signedOut });
    mounted = { key: `workspace:${route.gid}`, view };
    document.title = '共同账本 · CoFinance';
    return;
  }

  cleanupMounted();
  mount(app, html`
    <div class="not-found">
      <span class="brand-mark" aria-hidden="true">C</span>
      <h1>这一页不在账本里</h1>
      <p>地址可能已经改变，或者页面不存在。</p>
      <a class="button button-primary" href="#${getToken() ? '/groups' : '/login'}">回到${getToken() ? '组列表' : '登录页'}</a>
    </div>
  `);
}

window.addEventListener('unhandledrejection', (event) => {
  if (event.reason?.name === 'ApiError') {
    toast(event.reason.message || '操作没有完成', 'error', 4800);
  }
});

startRouter(handleRoute);
