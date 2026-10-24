class SafeHtml {
  constructor(value) {
    this.value = String(value);
  }

  toString() {
    return this.value;
  }
}

export function escapeHtml(value) {
  return String(value ?? '').replace(/[&<>"']/g, (char) => ({
    '&': '&amp;',
    '<': '&lt;',
    '>': '&gt;',
    '"': '&quot;',
    "'": '&#39;',
  })[char]);
}

export function raw(value) {
  return value instanceof SafeHtml ? value : new SafeHtml(value);
}

function interpolate(value) {
  if (value === null || value === undefined || value === false) {
    return '';
  }
  if (value instanceof SafeHtml) {
    return value.value;
  }
  if (Array.isArray(value)) {
    return value.map(interpolate).join('');
  }
  return escapeHtml(value);
}

export function html(strings, ...values) {
  let output = strings[0];
  for (let index = 0; index < values.length; index += 1) {
    output += interpolate(values[index]) + strings[index + 1];
  }
  return new SafeHtml(output);
}

export function mount(target, content) {
  if (!target) {
    throw new Error('mount target is required');
  }
  target.innerHTML = content instanceof SafeHtml ? content.value : escapeHtml(content);
  return target;
}

export function qs(selector, root = document) {
  return root.querySelector(selector);
}

export function qsa(selector, root = document) {
  return [...root.querySelectorAll(selector)];
}

export function on(root, event, selectorOrHandler, maybeHandler) {
  const selector = typeof selectorOrHandler === 'string' ? selectorOrHandler : null;
  const handler = typeof selectorOrHandler === 'function' ? selectorOrHandler : maybeHandler;
  const listener = (event) => {
    if (!selector) {
      handler(event);
      return;
    }
    const target = event.target.closest(selector);
    if (target && root.contains(target)) {
      handler(event, target);
    }
  };
  root.addEventListener(event, listener);
  return () => root.removeEventListener(event, listener);
}

export function setHtml(element, content) {
  if (element) {
    element.innerHTML = content instanceof SafeHtml ? content.value : escapeHtml(content);
  }
}

export function formValues(form) {
  const values = {};
  const data = new FormData(form);
  for (const [key, value] of data.entries()) {
    if (key.endsWith('[]')) {
      const normalized = key.slice(0, -2);
      values[normalized] ??= [];
      values[normalized].push(String(value));
    } else {
      values[key] = typeof value === 'string' ? value : value.name;
    }
  }
  return values;
}

export function setFieldError(form, name, message) {
  const field = form.elements.namedItem(name);
  const wrapper = field?.closest?.('.field');
  if (!wrapper) {
    return;
  }
  wrapper.classList.toggle('has-error', Boolean(message));
  let error = wrapper.querySelector('.field-error');
  if (!error) {
    error = document.createElement('p');
    error.className = 'field-error';
    wrapper.append(error);
  }
  error.textContent = message || '';
}

export function clearFieldErrors(form) {
  qsa('.field.has-error', form).forEach((field) => field.classList.remove('has-error'));
  qsa('.field-error', form).forEach((error) => {
    error.textContent = '';
  });
}
