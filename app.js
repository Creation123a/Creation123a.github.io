/* ============================================================================
   AURORA COFFEE CO. — app.js
   Frontend controller. Talks to the C++ backend in main.cpp.
   ============================================================================ */

'use strict';

/* ---------------------------------------------------------------------------
   1. CONFIG & SMALL HELPERS
   --------------------------------------------------------------------------- */
const API = '';   // same origin — main.cpp serves everything

const SID = (() => {
  let s = localStorage.getItem('aurora.sid');
  if (!s) {
    s = 's' + Math.random().toString(36).slice(2, 10) + Date.now().toString(36);
    localStorage.setItem('aurora.sid', s);
  }
  return s;
})();

const $  = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

/** Core fetch wrapper. Always sends the session header. */
async function api(path, opts = {}) {
  const res = await fetch(API + path, {
    ...opts,
    headers: { 'X-Session': SID, ...(opts.headers || {}) },
  });
  if (!res.ok) {
    let msg = `Request failed (${res.status})`;
    try {
      const j = await res.json();
      if (j && j.error) msg = j.error;
    } catch (_) { /* not JSON */ }
    throw new Error(msg);
  }
  const ct = res.headers.get('content-type') || '';
  return ct.includes('application/json') ? res.json() : res.text();
}

/** POST form-encoded or plain text. */
function post(path, body) {
  const isForm = typeof body === 'string' || body instanceof URLSearchParams;
  return api(path, {
    method: 'POST',
    headers: isForm
      ? { 'Content-Type': 'application/x-www-form-urlencoded' }
      : { 'Content-Type': 'text/plain; charset=utf-8' },
    body: isForm ? body.toString() : body,
  });
}

const fmtINR = n => '₹' + Math.round(Number(n) || 0).toLocaleString('en-IN');

const debounce = (fn, ms = 220) => {
  let t;
  return (...a) => { clearTimeout(t); t = setTimeout(() => fn(...a), ms); };
};

const escapeHtml = s => String(s ?? '').replace(/[&<>"']/g, c => ({
  '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
}[c]));

const starSvg = () =>
  `<svg viewBox="0 0 24 24" width="11" height="11"><path d="M12 3.5l2.6 5.3 5.9.9-4.2 4.1 1 5.8L12 16.9 6.7 19.6l1-5.8L3.5 9.7l5.9-.9z" fill="currentColor"/></svg>`;


/* ---------------------------------------------------------------------------
   2. STATE
   --------------------------------------------------------------------------- */
const state = {
  menu: [],
  cart: null,
  query: '',
  category: '',
  sort: '',
  activeView: 'menu',

  currentItem: null,
  modalSelections: { size: '', milk: '', sugar: '', extras: new Set() },

  promo: '',
  promoPreview: { code: '', discount: 0, label: '' },

  trackId: null,
  trackTimer: null,
  healthTimer: null,
};

const PROMO_RULES = {
  BREW20:   { type: 'percent', value: 20, label: '20% off' },
  FIRSTCUP: { type: 'flat',    value: 50, label: 'Flat ₹50 off' },
  AURORA10: { type: 'percent', value: 10, label: '10% off' },
};
const PROMO_CAP = 500;   // matches the C++ percent-discount cap


/* ---------------------------------------------------------------------------
   3. TOASTS
   --------------------------------------------------------------------------- */
const toastWrap = $('#toastWrap');

function toast(message, variant = 'info', ms = 2600) {
  if (!toastWrap) return;
  const el = document.createElement('div');
  el.className = `toast ${variant}`;
  el.textContent = message;
  toastWrap.appendChild(el);
  setTimeout(() => {
    el.classList.add('is-leaving');
    setTimeout(() => el.remove(), 300);
  }, ms);
}


/* ---------------------------------------------------------------------------
   4. PRELOADER — 4-second beam, skippable
   --------------------------------------------------------------------------- */
(function bootPreloader() {
  const pre = $('#preloader');
  if (!pre) return;
  let done = false;
  const finish = () => {
    if (done) return;
    done = true;
    pre.classList.add('is-hidden');
    setTimeout(() => pre.remove(), 700);
  };
  setTimeout(finish, 4000);
  pre.addEventListener('click', finish);   // evaluator can skip
})();


/* ---------------------------------------------------------------------------
   5. THEME
   --------------------------------------------------------------------------- */
(function bootTheme() {
  const saved = localStorage.getItem('aurora.theme');
  const prefersLight = window.matchMedia
    && window.matchMedia('(prefers-color-scheme: light)').matches;
  const initial = saved || (prefersLight ? 'light' : 'dark');
  document.documentElement.dataset.theme = initial;
  localStorage.setItem('aurora.theme', initial);

  const btn = $('#themeToggle');
  if (btn) btn.addEventListener('click', () => {
    const next = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
    document.documentElement.dataset.theme = next;
    localStorage.setItem('aurora.theme', next);
  });
})();


/* ---------------------------------------------------------------------------
   6. VIEW ROUTER
   --------------------------------------------------------------------------- */
function showView(name) {
  if (!name) return;
  state.activeView = name;
  $$('.view').forEach(v => v.classList.toggle('is-active', v.id === 'view-' + name));
  $$('.nav-link').forEach(a => a.classList.toggle('is-active', a.dataset.view === name));
  window.scrollTo({ top: 0, behavior: 'smooth' });

  if (name === 'cart')     loadCart();
  if (name === 'orders')   loadOrders();
  if (name === 'checkout') loadCart().then(renderCheckout);
}

document.addEventListener('click', e => {
  const trigger = e.target.closest('[data-view]');
  if (!trigger) return;
  e.preventDefault();
  showView(trigger.dataset.view);
});


/* ---------------------------------------------------------------------------
   7. MENU — skeleton, fetch, render, filter, sort, search
   --------------------------------------------------------------------------- */
const grid      = $('#grid');
const gridEmpty = $('#gridEmpty');

function skeletonCards(n = 8) {
  return Array.from({ length: n }, () => `
    <div class="card skeleton" aria-hidden="true">
      <div class="card-top">
        <div class="sk-emoji"></div>
        <div class="sk-line" style="width:44px"></div>
      </div>
      <div class="sk-line w-80"></div>
      <div class="sk-line w-100"></div>
      <div class="sk-line w-60"></div>
      <div class="card-foot">
        <div class="sk-line" style="width:60px"></div>
        <div class="sk-line" style="width:72px"></div>
      </div>
    </div>
  `).join('');
}

function renderMenu() {
  if (!grid) return;
  if (!state.menu.length) {
    grid.innerHTML = '';
    if (gridEmpty) gridEmpty.hidden = false;
    return;
  }
  if (gridEmpty) gridEmpty.hidden = true;
  grid.setAttribute('aria-busy', 'false');
  grid.innerHTML = state.menu.map((p, i) => `
    <article class="card" style="animation-delay:${Math.min(i * 22, 260)}ms" data-id="${p.id}">
      <div class="card-top">
        <div class="card-emoji" aria-hidden="true">${escapeHtml(p.emoji || '☕')}</div>
        <span class="card-rating">${starSvg()}${Number(p.rating || 0).toFixed(1)}</span>
      </div>
      <h3>${escapeHtml(p.name)}</h3>
      <p class="card-desc">${escapeHtml(p.description || '')}</p>
      ${(p.tags && p.tags.length)
        ? `<div class="card-tags">${p.tags.slice(0, 3).map(t => `<span class="tag">${escapeHtml(t)}</span>`).join('')}</div>`
        : ''}
      <div class="card-foot">
        <span class="card-price">${fmtINR(p.price)}</span>
        <button class="card-add" type="button" data-add="${p.id}">
          <svg viewBox="0 0 24 24" width="14" height="14" aria-hidden="true">
            <path d="M12 5v14M5 12h14" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>
          </svg>
          Add
        </button>
      </div>
    </article>
  `).join('');
}

async function fetchMenu() {
  if (!grid) return;
  grid.setAttribute('aria-busy', 'true');
  grid.innerHTML = skeletonCards(8);
  try {
    const params = new URLSearchParams();
    if (state.query)    params.set('q',   state.query);
    if (state.category) params.set('cat', state.category);
    if (state.sort)     params.set('sort', state.sort);
    const qs = params.toString();
    const data = await api('/api/menu' + (qs ? '?' + qs : ''));
    state.menu = Array.isArray(data) ? data : [];
    renderMenu();
  } catch (err) {
    console.error(err);
    grid.innerHTML = '';
    toast('Could not load menu · is the C++ server running?', 'error', 4200);
  }
}

/* Card "Add" → open item modal */
document.addEventListener('click', e => {
  const addBtn = e.target.closest('[data-add]');
  if (!addBtn) return;
  const id = Number(addBtn.dataset.add);
  const product = state.menu.find(p => p.id === id);
  if (product) openItemModal(product);
});

/* Category tabs */
const catTabs = $('#catTabs');
if (catTabs) {
  catTabs.addEventListener('click', e => {
    const tab = e.target.closest('.tab');
    if (!tab) return;
    $$('.tab', catTabs).forEach(t => t.classList.toggle('is-active', t === tab));
    state.category = tab.dataset.cat || '';
    fetchMenu();
  });
}

/* Sort */
const sortSelect = $('#sortSelect');
if (sortSelect) {
  sortSelect.addEventListener('change', () => {
    state.sort = sortSelect.value;
    fetchMenu();
  });
}

/* Search — synced across desktop + mobile */
const searchInline = $('#searchInline');
const searchMobile = $('#searchMobile');

const applySearch = debounce(value => {
  state.query = String(value || '').trim();
  if (searchInline && searchInline.value !== value) searchInline.value = value;
  if (searchMobile && searchMobile.value !== value) searchMobile.value = value;
  fetchMenu();
}, 220);

if (searchInline) searchInline.addEventListener('input', e => applySearch(e.target.value));
if (searchMobile) searchMobile.addEventListener('input', e => applySearch(e.target.value));

/* Clear-filters */
const clearFilters = $('#clearFilters');
if (clearFilters) {
  clearFilters.addEventListener('click', () => {
    state.query = ''; state.category = ''; state.sort = '';
    if (searchInline) searchInline.value = '';
    if (searchMobile) searchMobile.value = '';
    if (sortSelect)   sortSelect.value = '';
    if (catTabs) $$('.tab', catTabs).forEach(t => t.classList.toggle('is-active', !t.dataset.cat));
    fetchMenu();
  });
}


/* ---------------------------------------------------------------------------
   8. PROMOS STRIP + CHIPS
   --------------------------------------------------------------------------- */
async function loadPromos() {
  const strip = $('#promoStrip');
  if (!strip) return;
  try {
    const promos = await api('/api/promos');
    strip.innerHTML = promos.map(p => `
      <div class="promo-card">
        <span class="promo-code">${escapeHtml(p.code)}</span>
        <span class="promo-text">${escapeHtml(p.description)}</span>
      </div>
    `).join('');
    const chips = $('#promoChips');
    if (chips) {
      chips.innerHTML = promos.map(p =>
        `<button class="chip" type="button" data-promo="${escapeHtml(p.code)}">${escapeHtml(p.code)}</button>`
      ).join('');
    }
  } catch (err) {
    strip.innerHTML = '';
  }
}

document.addEventListener('click', e => {
  const chip = e.target.closest('[data-promo]');
  if (!chip) return;
  const input = $('#coPromo');
  if (!input) return;
  input.value = chip.dataset.promo;
  applyPromo();
});


/* ---------------------------------------------------------------------------
   9. CART
   --------------------------------------------------------------------------- */
function updateCartBadge(cart) {
  const badge = $('#cartCount');
  const pill  = $('.cart-pill');
  if (!badge) return;
  const next = (cart && cart.count) || 0;
  if (badge.textContent !== String(next)) {
    badge.textContent = String(next);
    if (pill) {
      pill.classList.remove('is-bumping');
      void pill.offsetWidth;   // force reflow to restart the animation
      pill.classList.add('is-bumping');
    }
  }
}

async function loadCart() {
  try {
    const cart = await api('/api/cart');
    state.cart = cart;
    renderCart(cart);
    updateCartBadge(cart);
    return cart;
  } catch (err) {
    console.error(err);
    return null;
  }
}

function renderCart(cart) {
  const list     = $('#cartList');
  const empty    = $('#emptyCart');
  const totals   = $('#cartTotals');
  const checkout = $('#toCheckout');
  if (!list) return;

  const items   = (cart && cart.items) || [];
  const isEmpty = items.length === 0;

  if (empty) empty.hidden = !isEmpty;
  list.hidden = isEmpty;

  if (isEmpty) {
    list.innerHTML = '';
    if (totals) totals.innerHTML = '';
    if (checkout) checkout.disabled = true;
    return;
  }

  list.innerHTML = items.map(it => `
    <div class="cart-item" data-item-id="${it.id}">
      <div class="cart-emoji" aria-hidden="true">${escapeHtml(it.emoji || '☕')}</div>
      <div class="cart-info">
        <h4>${escapeHtml(it.name)}</h4>
        ${it.notes ? `<div class="notes">${escapeHtml(it.notes)}</div>` : ''}
        <div class="unit">${fmtINR(it.price)} each</div>
      </div>
      <div class="cart-qty" role="group" aria-label="Quantity for ${escapeHtml(it.name)}">
        <button type="button" data-qty-minus="${it.id}" aria-label="Decrease">−</button>
        <span>${it.qty}</span>
        <button type="button" data-qty-plus="${it.id}" aria-label="Increase">+</button>
      </div>
      <div class="cart-line">
        <span class="line-total">${fmtINR(it.price * it.qty)}</span>
        <button class="cart-remove" type="button" data-remove="${it.id}">Remove</button>
      </div>
    </div>
  `).join('');

  if (totals) {
    totals.innerHTML = `
      <div class="row"><span>Subtotal</span><b>${fmtINR(cart.subtotal)}</b></div>
      <div class="row"><span>Tax (5%)</span><b>${fmtINR(cart.tax)}</b></div>
      <div class="row"><span>Delivery</span><b>${fmtINR(cart.delivery)}</b></div>
      <div class="divider"></div>
      <div class="row grand"><span>Total</span><b>${fmtINR(cart.total)}</b></div>
    `;
  }
  if (checkout) checkout.disabled = false;
}

document.addEventListener('click', async e => {
  const plus  = e.target.closest('[data-qty-plus]');
  const minus = e.target.closest('[data-qty-minus]');
  const rem   = e.target.closest('[data-remove]');
  if (!plus && !minus && !rem) return;

  const items = (state.cart && state.cart.items) || [];

  try {
    if (plus) {
      const id = Number(plus.dataset.qtyPlus);
      const it = items.find(i => i.id === id);
      if (!it) return;
      const updated = await post('/api/cart/update',
        new URLSearchParams({ id: String(id), qty: String(it.qty + 1) }));
      state.cart = updated; renderCart(updated); updateCartBadge(updated);
    } else if (minus) {
      const id = Number(minus.dataset.qtyMinus);
      const it = items.find(i => i.id === id);
      if (!it) return;
      const updated = await post('/api/cart/update',
        new URLSearchParams({ id: String(id), qty: String(it.qty - 1) }));
      state.cart = updated; renderCart(updated); updateCartBadge(updated);
    } else if (rem) {
      const id = Number(rem.dataset.remove);
      const updated = await post('/api/cart/remove',
        new URLSearchParams({ id: String(id) }));
      state.cart = updated; renderCart(updated); updateCartBadge(updated);
      toast('Removed from cart', 'info', 1600);
    }
  } catch (err) {
    toast(err.message || 'Could not update cart', 'error');
  }
});

const clearCartBtn = $('#clearCart');
if (clearCartBtn) {
  clearCartBtn.addEventListener('click', async () => {
    try {
      const updated = await post('/api/cart/clear', '');
      state.cart = updated; renderCart(updated); updateCartBadge(updated);
      toast('Cart cleared', 'info', 1600);
    } catch (err) {
      toast('Could not clear cart', 'error');
    }
  });
}

const toCheckout = $('#toCheckout');
if (toCheckout) toCheckout.addEventListener('click', () => showView('checkout'));

const backToCart = $('#backToCart');
if (backToCart) backToCart.addEventListener('click', () => showView('cart'));


/* ---------------------------------------------------------------------------
   10. ITEM MODAL
   --------------------------------------------------------------------------- */
const modal = $('#itemModal');

function setGroupDefault(groupName, value) {
  const group = document.querySelector(`.modal-group[data-group="${groupName}"]`);
  if (!group) return;
  $$('.chip', group).forEach(c => c.classList.toggle('is-active', c.dataset.val === value));
  state.modalSelections[groupName] = value;
}

function openItemModal(product) {
  if (!modal) return;
  state.currentItem = product;
  state.modalSelections = { size: '', milk: '', sugar: '', extras: new Set() };

  $('#imEmoji').textContent   = product.emoji || '☕';
  $('#imName').textContent    = product.name;
  $('#imDesc').textContent    = product.description || '';
  $('#imPrice').textContent   = fmtINR(product.price);
  $('#imUnit').textContent    = 'per item';
  $('#imNotes').value         = '';
  $('#imQty').value           = '1';

  // Reset chips, then set defaults
  $$('.modal-group').forEach(group =>
    $$('.chip', group).forEach(c => c.classList.remove('is-active')));
  setGroupDefault('size',  '');
  setGroupDefault('milk',  '');
  setGroupDefault('sugar', '');

  updateModalPrice();
  modal.classList.add('is-open');
  modal.setAttribute('aria-hidden', 'false');
}

function closeItemModal() {
  if (!modal) return;
  modal.classList.remove('is-open');
  modal.setAttribute('aria-hidden', 'true');
  state.currentItem = null;
}

document.addEventListener('click', e => {
  if (e.target.closest('[data-close-modal]')) closeItemModal();
});
document.addEventListener('keydown', e => {
  if (e.key === 'Escape' && modal && modal.classList.contains('is-open')) closeItemModal();
});

/* Chip toggles inside the modal */
document.addEventListener('click', e => {
  const chip = e.target.closest('.modal-group .chip');
  if (!chip) return;
  const group = chip.closest('.modal-group');
  if (!group) return;
  const key = group.dataset.group;

  if (key === 'extras') {
    chip.classList.toggle('is-active');
    if (chip.classList.contains('is-active')) state.modalSelections.extras.add(chip.dataset.val);
    else                                       state.modalSelections.extras.delete(chip.dataset.val);
  } else {
    $$('.chip', group).forEach(c => c.classList.toggle('is-active', c === chip));
    state.modalSelections[key] = chip.dataset.val || '';
  }
  updateModalPrice();
});

/* Price preview in the modal.
   NOTE: the C++ backend charges only product base price × qty.
   Size/milk/sugar/extras are carried in the "notes" field for the barista. */
function computeModalTotal() {
  if (!state.currentItem) return { unit: 0, total: 0, qty: 1 };
  const qty  = Math.max(1, Math.min(99, parseInt($('#imQty').value, 10) || 1));
  const unit = Number(state.currentItem.price) || 0;
  return { unit, total: unit * qty, qty };
}

function updateModalPrice() {
  const { unit, total } = computeModalTotal();
  $('#imPrice').textContent = fmtINR(unit);
  $('#imTotal').textContent = fmtINR(total);
}

['#imQtyMinus', '#imQtyPlus'].forEach(sel => {
  const btn = $(sel);
  if (!btn) return;
  btn.addEventListener('click', () => {
    const input = $('#imQty');
    let v = parseInt(input.value, 10) || 1;
    v += sel === '#imQtyPlus' ? 1 : -1;
    v = Math.max(1, Math.min(99, v));
    input.value = String(v);
    updateModalPrice();
  });
});

const imQty = $('#imQty');
if (imQty) imQty.addEventListener('input', () => {
  imQty.value = imQty.value.replace(/[^\d]/g, '').slice(0, 2);
  if (!imQty.value) imQty.value = '1';
  updateModalPrice();
});

const imAdd = $('#imAdd');
if (imAdd) {
  imAdd.addEventListener('click', async () => {
    if (!state.currentItem) return;
    const { qty } = computeModalTotal();

    // Build the notes string from selections + custom notes
    const parts = [];
    if (state.modalSelections.size)  parts.push(state.modalSelections.size);
    if (state.modalSelections.milk)  parts.push(state.modalSelections.milk);
    if (state.modalSelections.sugar) parts.push(state.modalSelections.sugar);
    state.modalSelections.extras.forEach(v => parts.push(v));
    const custom = ($('#imNotes').value || '').trim();
    if (custom) parts.push(custom);
    const notes = parts.join(' · ').slice(0, 200);

    try {
      imAdd.disabled = true;
      const cart = await post('/api/cart/add', new URLSearchParams({
        id: String(state.currentItem.id),
        qty: String(qty),
        notes,
      }));
      state.cart = cart;
      updateCartBadge(cart);
      if (state.activeView === 'cart') renderCart(cart);
      toast(`Added ${qty} × ${state.currentItem.name}`, 'success');
      closeItemModal();
    } catch (err) {
      toast(err.message || 'Could not add to cart', 'error');
    } finally {
      imAdd.disabled = false;
    }
  });
}


/* ---------------------------------------------------------------------------
   11. CHECKOUT
   --------------------------------------------------------------------------- */
function cartSubtotal() {
  return ((state.cart && state.cart.items) || [])
    .reduce((s, it) => s + it.price * it.qty, 0);
}

function computePromoPreview() {
  const rule = PROMO_RULES[state.promo];
  if (!rule) return { code: '', discount: 0, label: '' };
  const sub = cartSubtotal();
  if (!sub) return { code: '', discount: 0, label: '' };
  let disc = rule.type === 'percent' ? sub * rule.value / 100 : rule.value;
  disc = Math.min(disc, PROMO_CAP, sub);
  return { code: state.promo, discount: disc, label: rule.label };
}

function renderCheckout() {
  const items    = (state.cart && state.cart.items) || [];
  const sub      = cartSubtotal();
  state.promoPreview = computePromoPreview();
  const disc     = state.promoPreview.discount;
  const taxable  = Math.max(0, sub - disc);
  const tax      = taxable * 0.05;
  const del      = items.length ? 40 : 0;
  const total    = taxable + tax + del;

  const summary = $('#coSummary');
  if (summary) {
    if (!items.length) {
      summary.innerHTML = `<p class="muted" style="font-size:13px">Your cart is empty.</p>`;
    } else {
      summary.innerHTML = `
        <div class="row"><span>Subtotal</span><b>${fmtINR(sub)}</b></div>
        ${disc > 0
          ? `<div class="row discount"><span>${escapeHtml(state.promoPreview.label || 'Discount')} (${escapeHtml(state.promo)})</span><b>−${fmtINR(disc)}</b></div>`
          : ''}
        <div class="row"><span>Tax (5%)</span><b>${fmtINR(tax)}</b></div>
        <div class="row"><span>Delivery</span><b>${fmtINR(del)}</b></div>
        <div class="divider"></div>
        <div class="row grand"><span>Total</span><b>${fmtINR(total)}</b></div>
      `;
    }
  }

  const place = $('#placeOrder');
  if (place) place.disabled = items.length === 0;
}

async function applyPromo() {
  const input = $('#coPromo');
  const hint  = $('#promoHint');
  if (!input || !hint) return;

  const code = (input.value || '').trim().toUpperCase();
  input.value = code;

  if (!code) {
    state.promo = '';
    hint.textContent = ''; hint.className = 'hint';
    renderCheckout();
    return;
  }

  try {
    const list  = await api('/api/promos');
    const found = (list || []).find(p => String(p.code).toUpperCase() === code);
    if (!found) {
      state.promo = '';
      hint.textContent = "That code isn't valid.";
      hint.className = 'hint error';
    } else {
      state.promo = code;
      hint.textContent = `Applied: ${found.description}`;
      hint.className = 'hint success';
      toast(`Promo ${code} applied`, 'success', 2000);
    }
  } catch (err) {
    // Server offline — accept the code locally, backend will validate on checkout
    state.promo = code;
    hint.textContent = `Applied: ${code}`;
    hint.className = 'hint success';
  }
  renderCheckout();
}

const applyPromoBtn = $('#applyPromo');
if (applyPromoBtn) applyPromoBtn.addEventListener('click', applyPromo);

const coPromo = $('#coPromo');
if (coPromo) coPromo.addEventListener('keydown', e => {
  if (e.key === 'Enter') { e.preventDefault(); applyPromo(); }
});

function setFieldError(id, message) {
  const field = document.getElementById(id);
  if (!field) return;
  const wrap = field.closest('.field');
  if (wrap) wrap.classList.toggle('has-error', !!message);
}

const placeOrderBtn = $('#placeOrder');
if (placeOrderBtn) {
  placeOrderBtn.addEventListener('click', async () => {
    const name    = ($('#coName').value    || '').trim();
    const phone   = ($('#coPhone').value   || '').trim();
    const email   = ($('#coEmail').value   || '').trim();
    const address = ($('#coAddress').value || '').trim();
    const payment = (document.querySelector('input[name="payment"]:checked') || {}).value || 'online';

    ['coName', 'coPhone', 'coAddress'].forEach(id => setFieldError(id, ''));

    if (name.length < 2) {
      setFieldError('coName', 'Please enter your full name.');
      toast('Please enter your full name.', 'error'); return;
    }
    if (phone.length < 8) {
      setFieldError('coPhone', 'Please enter a valid phone number.');
      toast('Please enter a valid phone number.', 'error'); return;
    }
    if (address.length < 8) {
      setFieldError('coAddress', 'Please enter a complete delivery address.');
      toast('Please enter a complete delivery address.', 'error'); return;
    }
    if (!state.cart || !state.cart.items || !state.cart.items.length) {
      toast('Your cart is empty.', 'error'); return;
    }

    const body = new URLSearchParams({
      name, phone, email, address, payment,
      promo: state.promo || '',
    });

    try {
      placeOrderBtn.disabled = true;
      placeOrderBtn.textContent = 'Placing…';

      const res = await post('/api/checkout', body);
      toast(`Order ${res.orderId} confirmed`, 'success', 3400);

      // Reset cart + promo state
      state.cart = null;
      state.promo = '';
      state.promoPreview = { code: '', discount: 0, label: '' };
      const promoInput = $('#coPromo');
      if (promoInput) promoInput.value = '';
      const promoHint = $('#promoHint');
      if (promoHint) { promoHint.textContent = ''; promoHint.className = 'hint'; }
      updateCartBadge({ count: 0 });

      // Go straight to tracking
      showView('track');
      const tId = $('#trackId');
      if (tId) tId.value = res.orderId;
      startTracking(res.orderId);
    } catch (err) {
      toast(err.message || 'Checkout failed', 'error', 3800);
    } finally {
      placeOrderBtn.disabled = false;
      placeOrderBtn.textContent = 'Place order';
    }
  });
}


/* ---------------------------------------------------------------------------
   12. TRACKING
   --------------------------------------------------------------------------- */
function renderTrack(order) {
  const result = $('#trackResult');
  const idle   = $('#trackIdle');
  if (!result) return;
  if (idle) idle.hidden = true;

  const steps      = Array.isArray(order.steps) ? order.steps : [];
  const isCancelled = order.status === 'Cancelled';
  const isDelivered = order.status === 'Delivered';

  const stepsHtml = steps.map(s => `
    <div class="step ${s.done ? 'done' : ''} ${s.active ? 'active' : ''}">
      <div class="step-dot"></div>
      <div class="step-name">${escapeHtml(s.name)}</div>
    </div>
  `).join('');

  const items     = Array.isArray(order.items) ? order.items : [];
  const itemsHtml = items.map(it => `
    <div class="track-item">
      <span class="emoji">${escapeHtml(it.emoji || '☕')}</span>
      <span>${escapeHtml(it.name)}</span>
      <span class="qty">×${it.qty}</span>
    </div>
  `).join('');

  result.innerHTML = `
    <div class="track-head">
      <div>
        <div class="track-id">${escapeHtml(order.id)}</div>
        <div class="track-meta">
          ${escapeHtml(order.customer || 'Guest')} · ${escapeHtml(order.address || '')}
        </div>
      </div>
      <div class="track-status ${isCancelled ? 'cancelled' : ''}">
        ${escapeHtml(order.status)}
      </div>
    </div>

    ${!isCancelled ? `
      <div class="eta">
        ${isDelivered ? 'Delivered in' : 'Arriving in'}
        <b class="${isDelivered ? '' : 'tick'}">${isDelivered ? '—' : order.eta + 's'}</b>
      </div>
      <div class="stepper">${stepsHtml}</div>
    ` : `<p class="muted" style="margin-bottom:20px">This order was cancelled.</p>`}

    <div class="track-items">
      <div style="display:flex;justify-content:space-between;font-size:12px;color:var(--fg-3);letter-spacing:.2em;text-transform:uppercase;margin-bottom:6px">
        <span>Items</span>
        <span>Total ${fmtINR(order.total)} · ${escapeHtml(order.payment)}</span>
      </div>
      ${itemsHtml || '<p class="muted" style="font-size:13px">No items.</p>'}
    </div>
  `;
}

async function fetchTrack(id) {
  if (!id) return null;
  try {
    return await api('/api/order/' + encodeURIComponent(id));
  } catch (_) {
    return null;
  }
}

async function startTracking(id) {
  if (!id) return;
  stopTracking();

  const idle = $('#trackIdle');
  if (idle) idle.hidden = true;
  const result = $('#trackResult');
  if (result) result.innerHTML = `<p class="muted">Locating order ${escapeHtml(id)}…</p>`;

  const tick = async () => {
    const order = await fetchTrack(id);
    if (!order) {
      if (result) result.innerHTML = `<p class="muted">Order not found. Check the ID and try again.</p>`;
      stopTracking();
      return;
    }
    renderTrack(order);
    if (order.status === 'Delivered' || order.status === 'Cancelled') stopTracking();
  };

  await tick();
  state.trackTimer = setInterval(tick, 2000);
}

function stopTracking() {
  if (state.trackTimer) { clearInterval(state.trackTimer); state.trackTimer = null; }
}

const trackBtn = $('#trackBtn');
if (trackBtn) {
  trackBtn.addEventListener('click', () => {
    const input = $('#trackId');
    const id = (input && input.value || '').trim().toUpperCase();
    if (!id) { toast('Enter an order ID like CFE-10001', 'info'); return; }
    state.trackId = id;
    startTracking(id);
  });
}

const trackIdInput = $('#trackId');
if (trackIdInput) {
  trackIdInput.addEventListener('keydown', e => {
    if (e.key === 'Enter') { e.preventDefault(); trackBtn && trackBtn.click(); }
  });
}


/* ---------------------------------------------------------------------------
   13. ORDERS HISTORY
   --------------------------------------------------------------------------- */
async function loadOrders() {
  const list  = $('#ordersList');
  const empty = $('#emptyOrders');
  if (!list) return;

  try {
    const orders = await api('/api/orders');
    const arr = Array.isArray(orders) ? orders : [];

    if (!arr.length) {
      list.innerHTML = '';
      if (empty) empty.hidden = false;
      return;
    }
    if (empty) empty.hidden = true;

    list.innerHTML = arr.map(o => {
      const cls = o.status === 'Delivered' ? 'delivered'
                : o.status === 'Cancelled' ? 'cancelled' : '';
      const n   = (o.items && o.items.length) || 0;
      return `
        <div class="order-card" data-order-id="${escapeHtml(o.id)}">
          <div class="order-card-head">
            <span class="order-card-id">${escapeHtml(o.id)}</span>
            <span class="order-card-status ${cls}">${escapeHtml(o.status)}</span>
          </div>
          <div class="order-card-body">
            ${escapeHtml(o.customer || 'Guest')} · ${n} item${n === 1 ? '' : 's'}<br>
            ${escapeHtml(o.payment || '')}
          </div>
          <div class="order-card-foot">
            <span class="order-card-total">${fmtINR(o.total)}</span>
            <span class="muted">Tap to track →</span>
          </div>
        </div>
      `;
    }).join('');
  } catch (err) {
    toast('Could not load orders', 'error');
  }
}

document.addEventListener('click', e => {
  const card = e.target.closest('[data-order-id]');
  if (!card) return;
  const id = card.dataset.orderId;
  showView('track');
  const input = $('#trackId');
  if (input) input.value = id;
  startTracking(id);
});

const refreshOrders = $('#refreshOrders');
if (refreshOrders) refreshOrders.addEventListener('click', loadOrders);


/* ---------------------------------------------------------------------------
   14. BOT PANEL (rule-based AI avatar)
   --------------------------------------------------------------------------- */
const botPanel = $('#botPanel');
const botLog   = $('#botLog');
const botForm  = $('#botForm');

function botOpen(open) {
  if (!botPanel) return;
  botPanel.classList.toggle('is-open', open);
  botPanel.setAttribute('aria-hidden', String(!open));
  if (open && botLog && !botLog.dataset.greeted) {
    botLog.dataset.greeted = '1';
    appendBotMsg(
      "Hi, I'm Aria ☕ Ask me for a recommendation, or about delivery, promos, tracking. Try 'vegan', 'decaf', 'hours' or 'cancel'."
    );
  }
}

function appendBotMsg(text, cls = 'bot') {
  if (!botLog) return null;
  const el = document.createElement('div');
  el.className = `msg ${cls}`;
  el.textContent = text;
  botLog.appendChild(el);
  botLog.scrollTop = botLog.scrollHeight;
  return el;
}

function appendTyping() {
  if (!botLog) return null;
  const el = document.createElement('div');
  el.className = 'msg bot typing';
  el.innerHTML = '<i></i><i></i><i></i>';
  botLog.appendChild(el);
  botLog.scrollTop = botLog.scrollHeight;
  return el;
}

const botToggle = $('#botToggle');
if (botToggle) botToggle.addEventListener('click', () => {
  const isOpen = !!(botPanel && botPanel.classList.contains('is-open'));
  botOpen(!isOpen);
});

const botClose = $('#botClose');
if (botClose) botClose.addEventListener('click', () => botOpen(false));

document.addEventListener('click', e => {
  const chip = e.target.closest('#botChips .chip');
  if (!chip) return;
  sendBot(chip.dataset.msg || chip.textContent);
});

if (botForm) {
  botForm.addEventListener('submit', e => {
    e.preventDefault();
    const input = $('#botMsg');
    const msg = (input && input.value || '').trim();
    if (!msg) return;
    input.value = '';
    sendBot(msg);
  });
}

async function sendBot(text) {
  if (!text) return;
  appendBotMsg(text, 'me');
  const typing = appendTyping();
  try {
    const res = await fetch('/api/chat', {
      method: 'POST',
      headers: { 'X-Session': SID, 'Content-Type': 'text/plain; charset=utf-8' },
      body: text,
    });
    const reply = await res.text();
    if (typing) typing.remove();
    appendBotMsg(reply || "I'm not sure — try asking about the menu, delivery, promos, or tracking.");
  } catch (err) {
    if (typing) typing.remove();
    appendBotMsg('Aria is offline right now. Please try again.');
  }
}


/* ---------------------------------------------------------------------------
   15. HEALTH CHECK
   --------------------------------------------------------------------------- */
async function pingHealth() {
  const label = $('#healthLabel');
  const dot   = document.querySelector('.foot-right .live-dot');
  if (!label) return;
  try {
    const data = await api('/api/health');
    label.textContent = `ok · ${data.menu_size} items`;
    if (dot) dot.classList.remove('offline');
    const stat = $('#statMenu');
    if (stat) stat.textContent = String(data.menu_size);
  } catch (_) {
    label.textContent = 'offline';
    if (dot) dot.classList.add('offline');
  }
}

function startHealthPolling() {
  pingHealth();
  if (state.healthTimer) clearInterval(state.healthTimer);
  state.healthTimer = setInterval(pingHealth, 15000);
}


/* ---------------------------------------------------------------------------
   16. BOOT
   --------------------------------------------------------------------------- */
async function boot() {
  const year = $('#year');
  if (year) year.textContent = String(new Date().getFullYear());

  startHealthPolling();
  await loadPromos();
  await fetchMenu();
  await loadCart();

  // Deep-link support (#cart, #checkout, #track, #orders)
  const hash = (location.hash || '').replace('#', '');
  if (['menu', 'cart', 'checkout', 'track', 'orders'].includes(hash)) showView(hash);
}

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', boot);
} else {
  boot();
}
