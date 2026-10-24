import { html, raw, mount, qs, qsa, escapeHtml } from './dom.js';
import { icon } from './icons.js';
import { initials, displayName } from './format.js';

let modalSequence = 0;
let activeModal = null;

export function toast(message, tone = 'info', duration = 3600) {
  const root = document.getElementById('toast-root');
  if (!root) {
    return;
  }
  const id = `toast-${Date.now()}-${Math.random().toString(16).slice(2)}`;
  const container = document.createElement('div');
  container.id = id;
  container.className = `toast toast-${tone}`;
  mount(container, html`
    <span class="toast-icon">${icon(tone === 'success' ? 'check' : tone === 'error' ? 'alert' : 'info')}</span>
    <span class="toast-message">${message}</span>
    <button class="icon-button toast-close" type="button" aria-label="关闭提示">${icon('close')}</button>
  `);
  root.append(container);
  const remove = () => {
    container.classList.add('is-leaving');
    window.setTimeout(() => container.remove(), 180);
  };
  container.querySelector('.toast-close').addEventListener('click', remove);
  window.setTimeout(remove, duration);
}

export function avatar(person, { size = 'md', className = '' } = {}) {
  const name = displayName(person);
  const url = person?.avatar_url || person?.sender_avatar_url || '';
  if (url) {
    return html`<span class="avatar avatar-${size} ${className}"><img src="${url}" alt="" loading="lazy"></span>`;
  }
  return html`<span class="avatar avatar-${size} avatar-fallback ${className}" aria-hidden="true">${initials(name)}</span>`;
}

export function spinner(label = '加载中') {
  return html`<span class="spinner-wrap" role="status"><span class="spinner" aria-hidden="true"></span><span>${label}</span></span>`;
}

export function loadingBlock(label = '正在加载…') {
  return html`<div class="loading-block">${spinner(label)}</div>`;
}

export function emptyState(title, body = '', action = null, iconName = 'wallet') {
  return html`
    <div class="empty-state">
      <span class="empty-icon">${icon(iconName)}</span>
      <h3>${title}</h3>
      ${body ? html`<p>${body}</p>` : ''}
      ${action ? html`<div class="empty-action">${action}</div>` : ''}
    </div>
  `;
}

export function errorBlock(error, retryLabel = '重试') {
  return html`
    <div class="error-block" role="alert">
      <span class="error-icon">${icon('alert')}</span>
      <div>
        <strong>${errorTitle(error)}</strong>
        <p>${friendlyError(error)}</p>
      </div>
      ${retryLabel ? html`<button class="button button-secondary" type="button" data-action="retry">${retryLabel}</button>` : ''}
    </div>
  `;
}

export function errorTitle(error) {
  if (error?.aborted) {
    return '请求已取消';
  }
  if (error?.status === 403) {
    return '没有访问权限';
  }
  if (error?.status === 404) {
    return '内容不存在';
  }
  if (error?.status === 429) {
    return '操作太频繁';
  }
  if (error?.status >= 500) {
    return '服务暂时不可用';
  }
  return '操作没有完成';
}

export function friendlyError(error) {
  if (!error) {
    return '未知错误';
  }
  if (error.aborted) {
    return '页面已切换或操作被取消。';
  }
  const message = error.message || '请稍后重试。';
  const hints = {
    1001: '',
    1002: '登录状态已失效，请重新登录。',
    1003: '登录状态已过期，请重新登录。',
    1004: '你不在这个组中，或已经被移除。',
    1014: '该分类下仍有交易，暂时不能删除。',
    1015: '还有未结清余额，请先完成结算。',
    1017: '你仍是创建者，请先转让组或解散。',
    1018: '这个请求与另一笔操作冲突，请刷新后重试。',
    2002: 'AI 服务暂时不可用，记账和聊天不受影响。',
    3001: 'AI 提问次数已达上限，请一分钟后再试。',
    3002: '等待中的实时连接过多，请稍后再试。',
  };
  const hint = hints[error.code];
  return hint && hint !== message ? `${message} ${hint}` : message;
}

export function setBusy(button, busy, text = '处理中…') {
  if (!button) {
    return;
  }
  if (busy) {
    button.dataset.originalText = button.textContent;
    button.disabled = true;
    button.classList.add('is-busy');
    button.textContent = text;
  } else {
    button.disabled = false;
    button.classList.remove('is-busy');
    if (button.dataset.originalText) {
      button.textContent = button.dataset.originalText;
      delete button.dataset.originalText;
    }
  }
}

export function openModal({ title, body, actions = [], size = 'md', onClose = null, className = '' }) {
  closeModal();
  const root = document.getElementById('modal-root');
  const id = `modal-${++modalSequence}`;
  const overlay = document.createElement('div');
  overlay.className = `modal-backdrop ${className}`;
  overlay.innerHTML = `
    <section class="modal modal-${size}" role="dialog" aria-modal="true" aria-labelledby="${id}-title">
      <header class="modal-header">
        <div>
          <p class="eyebrow">CoFinance</p>
          <h2 id="${id}-title"></h2>
        </div>
        <button class="icon-button" type="button" data-modal-close aria-label="关闭">${icon('close')}</button>
      </header>
      <div class="modal-body"></div>
      <footer class="modal-actions"></footer>
    </section>
  `;
  overlay.querySelector(`#${id}-title`).textContent = title;
  overlay.querySelector('.modal-body').appendChild(toNode(body));
  const footer = overlay.querySelector('.modal-actions');
  if (actions.length === 0) {
    footer.remove();
  } else {
    for (const action of actions) {
      const button = document.createElement('button');
      button.type = 'button';
      button.className = `button ${action.className || 'button-secondary'}`;
      button.textContent = action.label;
      if (action.disabled) {
        button.disabled = true;
      }
      button.addEventListener('click', () => action.onClick?.({ close: closeModal, button }));
      footer.append(button);
    }
  }
  root.append(overlay);
  document.body.classList.add('modal-open');
  const close = () => closeModal();
  overlay.querySelector('[data-modal-close]').addEventListener('click', close);
  overlay.addEventListener('mousedown', (event) => {
    if (event.target === overlay) {
      close();
    }
  });
  const keyHandler = (event) => {
    if (event.key === 'Escape') {
      close();
    }
  };
  document.addEventListener('keydown', keyHandler);
  activeModal = {
    overlay,
    keyHandler,
    onClose,
    previousFocus: document.activeElement,
    close,
  };
  window.requestAnimationFrame(() => {
    const focusable = overlay.querySelector('input, textarea, select, button');
    focusable?.focus();
  });
  return activeModal;
}

function toNode(content) {
  if (content instanceof Node) {
    return content;
  }
  const container = document.createElement('div');
  container.innerHTML = String(content ?? '');
  return container;
}

export function closeModal() {
  if (!activeModal) {
    return;
  }
  const current = activeModal;
  activeModal = null;
  document.removeEventListener('keydown', current.keyHandler);
  current.overlay.remove();
  document.body.classList.remove('modal-open');
  current.previousFocus?.focus?.();
  current.onClose?.();
}

export function confirmDialog({ title, message, confirmText = '确认', tone = 'danger', details = '' }) {
  return new Promise((resolve) => {
    let settled = false;
    const finish = (value) => {
      if (!settled) {
        settled = true;
        resolve(value);
      }
      closeModal();
    };
    openModal({
      title,
      size: 'sm',
      body: toNode(html`
        <div class="confirm-copy">
          <p>${message}</p>
          ${details ? raw(`<div class="confirm-details">${details}</div>`) : ''}
        </div>
      `),
      actions: [
        { label: '取消', className: 'button-ghost', onClick: () => finish(false) },
        {
          label: confirmText,
          className: tone === 'danger' ? 'button-danger' : 'button-primary',
          onClick: () => finish(true),
        },
      ],
      onClose: () => {
        if (!settled) {
          settled = true;
          resolve(false);
        }
      },
    });
  });
}

export function promptDialog({ title, label, placeholder = '', value = '', type = 'text', confirmText = '确认' }) {
  return new Promise((resolve) => {
    let settled = false;
    const modal = openModal({
      title,
      size: 'sm',
      body: toNode(html`
        <form class="dialog-form" data-prompt-form>
          <label class="field">
            <span>${label}</span>
            <input class="input" name="value" type="${type}" value="${value}" placeholder="${placeholder}" autocomplete="off" maxlength="255">
          </label>
        </form>
      `),
      actions: [
        { label: '取消', className: 'button-ghost', onClick: () => finish(null) },
        {
          label: confirmText,
          className: 'button-primary',
          onClick: () => {
            const input = modal.overlay.querySelector('[name="value"]');
            finish(input.value);
          },
        },
      ],
      onClose: () => {
        if (!settled) {
          settled = true;
          resolve(null);
        }
      },
    });
    const form = modal.overlay.querySelector('[data-prompt-form]');
    form.addEventListener('submit', (event) => {
      event.preventDefault();
      finish(form.elements.value.value);
    });
    function finish(value) {
      if (!settled) {
        settled = true;
        resolve(value);
      }
      closeModal();
    }
  });
}

export function bindRetry(root, callback) {
  qsa('[data-action="retry"]', root).forEach((button) => button.addEventListener('click', callback));
}

export function errorMessageFor(error) {
  return friendlyError(error);
}

export function safeExternalLabel(value) {
  return escapeHtml(value);
}
