import { html, mount, qs, clearFieldErrors, setFieldError } from '../core/dom.js';
import { api } from '../core/api.js';
import { navigate } from '../core/router.js';
import { icon } from '../core/icons.js';
import { friendlyError, setBusy, toast } from '../core/ui.js';

function brand() {
  return html`
    <span class="brand-mark" aria-hidden="true">C</span>
    <span class="brand-copy">
      <span class="brand-name">CoFinance</span>
      <span class="brand-tagline">Shared money, clearly</span>
    </span>
  `;
}

export function renderAuth({ mode = 'login', onAuthenticated }) {
  const app = document.getElementById('app');
  const isRegister = mode === 'register';
  mount(app, html`
    <main class="auth-page">
      <section class="auth-story" aria-label="CoFinance 介绍">
        <a class="auth-brand" href="#/login" aria-label="CoFinance 首页">
          <span class="brand-mark" aria-hidden="true">C</span>
          <span class="auth-brand-name">CoFinance</span>
        </a>
        <div class="auth-story-content">
          <span class="auth-kicker">为共同生活而设计的账本</span>
          <h1 class="auth-title">一起花钱，<br>也要<em>一起明白</em>。</h1>
          <p class="auth-lede">室友、伴侣、家人共用一本账。记下每一笔共同开销，实时聊清楚，再用最少几笔转账把账结平。</p>
          <div class="auth-features" aria-label="功能亮点">
            <span class="auth-feature">${icon('wallet')}多人共同记账</span>
            <span class="auth-feature">${icon('chat')}实时消息</span>
            <span class="auth-feature">${icon('sparkles')}AI 财务建议</span>
          </div>
        </div>
        <p class="auth-footnote">账目留在自己的服务里。AI 只读取聚合统计，不读取备注与聊天。</p>
      </section>

      <section class="auth-panel">
        <div class="auth-card">
          <a class="brand auth-mobile-brand" href="#/login" aria-label="CoFinance">${brand()}</a>
          <p class="eyebrow">${isRegister ? 'Create your ledger' : 'Welcome back'}</p>
          <h1 class="auth-heading">${isRegister ? '创建你的账号' : '回到你们的账本'}</h1>
          <p class="auth-subheading">${isRegister ? '注册后即可创建第一个共同财务组。' : '登录后继续记录、聊天和结算。'}</p>

          <div class="auth-tabs" role="tablist" aria-label="登录或注册">
            <button class="auth-tab ${!isRegister ? 'is-active' : ''}" type="button" data-auth-mode="login" role="tab" aria-selected="${!isRegister}">登录</button>
            <button class="auth-tab ${isRegister ? 'is-active' : ''}" type="button" data-auth-mode="register" role="tab" aria-selected="${isRegister}">注册</button>
          </div>

          <form class="auth-form" data-auth-form novalidate>
            <div data-form-message></div>
            <label class="field">
              <span>用户名</span>
              <input class="input" name="username" autocomplete="username" placeholder="4–20 位字母、数字或下划线" maxlength="20" required>
              <small class="field-hint">登录时使用，创建后不能修改。</small>
            </label>
            ${isRegister ? html`
              <label class="field">
                <span>昵称</span>
                <input class="input" name="nickname" autocomplete="nickname" placeholder="别人会这样称呼你" maxlength="50">
              </label>
            ` : ''}
            <label class="field">
              <span>密码</span>
              <input class="input" name="password" type="password" autocomplete="${isRegister ? 'new-password' : 'current-password'}" placeholder="至少 8 位" minlength="8" maxlength="128" required>
            </label>
            ${isRegister ? html`
              <label class="field">
                <span>确认密码</span>
                <input class="input" name="confirm" type="password" autocomplete="new-password" placeholder="再输入一次密码" required>
              </label>
            ` : ''}
            <button class="button button-primary button-wide auth-submit" type="submit">
              ${isRegister ? '创建账号并登录' : '进入 CoFinance'}
            </button>
          </form>

          <p class="auth-switch">
            ${isRegister ? '已经有账号？' : '第一次使用 CoFinance？'}
            <a href="#/${isRegister ? 'login' : 'register'}">${isRegister ? '直接登录' : '创建一个账号'}</a>
          </p>
          <div class="auth-note">
            ${icon('lock')}
            <span>密码使用 PBKDF2 加盐存储，登录凭证只保存在当前浏览器中。公共电脑使用后请退出。</span>
          </div>
        </div>
      </section>
    </main>
  `);

  const form = qs('[data-auth-form]', app);
  const message = qs('[data-form-message]', app);
  const submit = qs('button[type="submit"]', form);
  const usernameInput = form.elements.username;
  const passwordInput = form.elements.password;

  requestAnimationFrame(() => (usernameInput || passwordInput)?.focus());

  app.querySelectorAll('[data-auth-mode]').forEach((button) => {
    button.addEventListener('click', () => {
      navigate(`/${button.dataset.authMode}`);
    });
  });

  function validate() {
    clearFieldErrors(form);
    let valid = true;
    const username = usernameInput.value.trim();
    const password = passwordInput.value;
    if (!/^[A-Za-z0-9_]{4,20}$/.test(username)) {
      setFieldError(form, 'username', '用户名需为 4–20 位字母、数字或下划线');
      valid = false;
    }
    if (password.length < 8 || password.length > 128) {
      setFieldError(form, 'password', '密码长度需为 8–128 位');
      valid = false;
    }
    if (isRegister) {
      if (password !== form.elements.confirm.value) {
        setFieldError(form, 'confirm', '两次输入的密码不一致');
        valid = false;
      }
      const nickname = form.elements.nickname.value.trim();
      if ([...nickname].length > 50) {
        setFieldError(form, 'nickname', '昵称不能超过 50 个字符');
        valid = false;
      }
    }
    return valid;
  }

  form.addEventListener('submit', async (event) => {
    event.preventDefault();
    message.innerHTML = '';
    if (!validate()) {
      return;
    }
    setBusy(submit, true, isRegister ? '正在创建…' : '正在登录…');
    try {
      const username = usernameInput.value.trim();
      const password = passwordInput.value;
      if (isRegister) {
        const nickname = form.elements.nickname.value.trim();
        await api.register({ username, password, nickname });
      }
      const login = await api.login({ username, password });
      const user = login.user || (await api.me());
      onAuthenticated?.(user);
      toast(isRegister ? '账号已创建，欢迎加入 CoFinance' : `欢迎回来，${user.nickname || user.username}`, 'success');
      navigate('/groups');
    } catch (error) {
      mount(message, html`<div class="notice notice-danger">${icon('alert')}<span>${friendlyError(error)}</span></div>`);
      toast(friendlyError(error), 'error', 4800);
      if (error?.code === 1101 || /用户名/.test(error?.message || '')) {
        setFieldError(form, 'username', friendlyError(error));
      }
    } finally {
      setBusy(submit, false);
    }
  });

  return { cleanup() {} };
}
