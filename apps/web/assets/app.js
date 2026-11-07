/*
 * tix web UI enhancements (M7, docs/architecture.md section 13.4).
 *
 * Progressive enhancement only: every page works without this file, and every request it
 * makes is one an existing form or link can also make (GET /api/tickets is the search form
 * as JSON). No framework, no build step, no third-party code. Data from the server is only
 * ever placed with textContent or attributes, never parsed as markup, except whole pages
 * from this server re-read with DOMParser when refreshing.
 */
'use strict';
(() => {
	const $ = (sel, root = document) => root.querySelector(sel);
	const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
	const body = document.body;
	const readOnly = body.hasAttribute('data-read-only');
	const STATUSES = ['open', 'in-progress', 'blocked', 'done', 'cancelled'];

	function el(tag, attrs = {}, text) {
		const e = document.createElement(tag);
		for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
		if (text !== undefined) e.textContent = text;
		return e;
	}

	function csrf() {
		const i = $('input[name=csrf]');
		return i ? i.value : '';
	}

	/* ---- messages ------------------------------------------------------------------ */

	let toastTimer = 0;
	function toast(msg, isError) {
		let t = $('#tix-toast');
		if (!t) {
			t = el('div', {id: 'tix-toast', role: 'status', 'aria-live': 'polite'});
			body.append(t);
		}
		t.textContent = msg;
		t.className = isError ? 'toast error' : 'toast';
		t.hidden = false;
		clearTimeout(toastTimer);
		toastTimer = setTimeout(() => { t.hidden = true; }, isError ? 8000 : 2500);
	}

	/* ---- posting the ordinary forms --------------------------------------------- */

	/* POSTs fields (plus the session's csrf token) like a form would. RETURNS {ok, msg}.
	 * Success is the server's 303 (an opaque redirect to fetch). */
	async function post(url, fields) {
		const data = new URLSearchParams();
		data.set('csrf', csrf());
		for (const [k, v] of Object.entries(fields)) data.set(k, v);
		let r;
		try {
			r = await fetch(url, {method: 'POST', body: data, credentials: 'same-origin', redirect: 'manual'});
		} catch (e) {
			return {ok: false, msg: 'The server did not answer.'};
		}
		if (r.type === 'opaqueredirect' || (r.status >= 200 && r.status < 300)) return {ok: true};
		let msg = 'Not saved (' + r.status + ').';
		try {
			const doc = new DOMParser().parseFromString(await r.text(), 'text/html');
			const errs = $$('.error, .conflict', doc).map((e) => e.textContent.trim()).filter(Boolean);
			if (errs.length) msg = errs.join(' ');
			else if ($('main p', doc)) msg = $('main p', doc).textContent.trim();
		} catch (e) { /* keep the status line */ }
		return {ok: false, msg};
	}

	/* ---- refreshing the page in place ------------------------------------------- */

	let dirty = false; /* the user typed into a form on this page */
	document.addEventListener('input', (e) => {
		if (e.target.closest('form') && !e.target.closest('form.search, form.filters')) dirty = true;
	});

	function busyEditing() {
		const a = document.activeElement;
		return dirty || (a && a.matches('input:not([type=search]), textarea, select') && !a.closest('form.filters'));
	}

	/* Re-reads this page and swaps <main>, keeping focus on the same ticket if possible. */
	async function refresh() {
		const focusId = document.activeElement && document.activeElement.dataset ? document.activeElement.dataset.id : null;
		let r;
		try {
			r = await fetch(location.href, {credentials: 'same-origin'});
		} catch (e) {
			return false;
		}
		if (!r.ok) return false;
		const doc = new DOMParser().parseFromString(await r.text(), 'text/html');
		const fresh = $('main', doc), old = $('main');
		if (!fresh || !old) return false;
		old.replaceWith(document.adoptNode(fresh));
		if (doc.body.dataset.stamp) body.dataset.stamp = doc.body.dataset.stamp;
		dirty = false;
		hideBanner();
		body.dataset.refreshes = String(+(body.dataset.refreshes || 0) + 1); /* for tests and debugging */
		enhance($('main'));
		if (focusId) {
			const f = $(`main [data-id="${CSS.escape(focusId)}"]`);
			if (f && f.matches('a, [tabindex]')) f.focus();
		}
		return true;
	}

	function banner() {
		if ($('#tix-banner')) return;
		const b = el('div', {id: 'tix-banner', class: 'banner', role: 'status'});
		b.append(document.createTextNode('This page changed elsewhere. '));
		const btn = el('button', {type: 'button'}, 'Reload');
		btn.addEventListener('click', () => location.reload());
		b.append(btn);
		$('main').before(b);
	}

	function hideBanner() {
		const b = $('#tix-banner');
		if (b) b.remove();
	}

	/* ---- live refresh (server-sent events) --------------------------------------- */

	/* Desktop notifications when a ticket starts waiting for a person (opt-in, §15.6). */
	const notified = new Set();
	function notify(changed) {
		if (!window.Notification || Notification.permission !== 'granted') return;
		for (const c of changed) {
			if (!c.human || notified.has(c.id)) continue;
			notified.add(c.id);
			const n = new Notification('tix: waiting for you', {body: c.title, tag: 'tix-' + c.id});
			n.onclick = () => { window.focus(); location.href = '/t/' + c.id; };
		}
		for (const c of changed) if (!c.human) notified.delete(c.id);
	}

	function notifyButton(root) {
		const b = $('#tix-notify', root);
		if (!b || !window.Notification || Notification.permission === 'granted') return;
		b.hidden = false;
		b.addEventListener('click', async () => {
			const p = await Notification.requestPermission();
			if (p === 'granted') { b.hidden = true; toast('Notifications on'); }
		});
	}

	function live() {
		if (!body.dataset.stamp || !window.EventSource) return;
		const es = new EventSource('/events');
		let skip = false; /* the next stamp only changed tickets this page doesn't show */
		es.addEventListener('tickets', (e) => {
			let d;
			try { d = JSON.parse(e.data); } catch (_) { return; }
			const changed = Array.isArray(d.changed) ? d.changed : [];
			notify(changed);
			const t = body.dataset.ticket;
			skip = !!t && changed.length < 50 &&
				!changed.some((c) => c.id === t || $(`main a[href="/t/${CSS.escape(c.id)}"]`));
		});
		es.addEventListener('stamp', (e) => {
			if (e.data === body.dataset.stamp) return;
			if (skip) { /* a ticket page refreshes for its own ticket and the ones it links */
				skip = false;
				body.dataset.stamp = e.data;
				return;
			}
			if (busyEditing()) banner();
			else refresh();
		});
		/* a refused stream (too many tabs, logged out) is not retried by the browser */
	}

	/* ---- ticket search and pickers ------------------------------------------------ */

	let pickSeq = 0;

	/* Turns a text input into a combobox over /api/tickets. onPick(ticket) on choice. */
	function picker(input, onPick, opts = {}) {
		if (input.dataset.picker) return;
		input.dataset.picker = '1';
		const id = 'tix-picks-' + (++pickSeq);
		const wrap = el('span', {class: 'pickwrap'});
		input.before(wrap);
		wrap.append(input);
		const list = el('ul', {id, class: 'picks', role: 'listbox'});
		list.hidden = true;
		wrap.append(list);
		input.setAttribute('role', 'combobox');
		input.setAttribute('aria-autocomplete', 'list');
		input.setAttribute('aria-controls', id);
		input.setAttribute('aria-expanded', 'false');
		input.setAttribute('autocomplete', 'off');
		let items = [], active = -1, timer = 0, ctl = null;

		function close() {
			list.hidden = true;
			input.setAttribute('aria-expanded', 'false');
			input.removeAttribute('aria-activedescendant');
			active = -1;
		}

		function show(tickets, more) {
			list.textContent = '';
			items = tickets;
			active = -1;
			tickets.forEach((t, i) => {
				const li = el('li', {id: `${id}-${i}`, role: 'option', 'data-id': t.id});
				li.append(el('code', {}, t.short), document.createTextNode(' ' + t.title + ' '),
					el('span', {class: 'status s-' + t.status}, t.status));
				li.addEventListener('mousedown', (e) => { e.preventDefault(); choose(i); });
				list.append(li);
			});
			if (more) list.append(el('li', {class: 'note', 'aria-disabled': 'true'}, 'more… (Enter searches)'));
			if (!tickets.length) list.append(el('li', {class: 'note', 'aria-disabled': 'true'}, 'no matches'));
			list.hidden = false;
			input.setAttribute('aria-expanded', 'true');
		}

		function mark(i) {
			$$('li', list).forEach((li) => li.classList.remove('active'));
			active = i;
			if (i >= 0 && list.children[i]) {
				list.children[i].classList.add('active');
				input.setAttribute('aria-activedescendant', `${id}-${i}`);
				list.children[i].scrollIntoView({block: 'nearest'});
			}
		}

		function choose(i) {
			const t = items[i];
			close();
			if (t) onPick(t);
		}

		async function search() {
			const q = input.value.trim();
			if (!q) return close();
			if (ctl) ctl.abort();
			ctl = new AbortController();
			const params = new URLSearchParams({q, limit: '10'});
			try {
				const r = await fetch('/api/tickets?' + params, {credentials: 'same-origin', signal: ctl.signal});
				if (!r.ok) return close();
				const j = await r.json();
				if (input.value.trim() === q && document.activeElement === input)
					show(j.tickets.filter((t) => t.id !== opts.exclude), j.more);
			} catch (e) { /* aborted or offline: the form still works */ }
		}

		input.addEventListener('input', () => {
			clearTimeout(timer);
			timer = setTimeout(search, 120);
		});
		input.addEventListener('keydown', (e) => {
			if (list.hidden) return;
			if (e.key === 'ArrowDown') { e.preventDefault(); mark(Math.min(active + 1, items.length - 1)); }
			else if (e.key === 'ArrowUp') { e.preventDefault(); mark(Math.max(active - 1, 0)); }
			else if (e.key === 'Enter' && active >= 0) { e.preventDefault(); choose(active); }
			else if (e.key === 'Escape') { e.preventDefault(); close(); }
		});
		input.addEventListener('blur', () => setTimeout(close, 100));
	}

	function pickers(root) {
		const search = $('form.search input[name=q]');
		if (search) picker(search, (t) => { location.href = '/t/' + t.id; });
		const self = (location.pathname.match(/^\/t\/([0-9a-f]+)/) || [])[1];
		for (const name of ['parent', 'pred', 'under']) {
			$$(`input[name=${name}]:not([type=hidden])`, root).forEach((input) => {
				picker(input, (t) => {
					input.value = name === 'parent' ? t.id : t.short;
					input.dispatchEvent(new Event('input', {bubbles: true}));
				}, {exclude: self});
			});
		}
	}

	/* ---- inline status ---------------------------------------------------------------- */

	function statusSelect(current) {
		const s = el('select', {class: 'inline-status', 'aria-label': 'Status'});
		if (current && !STATUSES.includes(current)) /* phase/stage (§15.1): keep it selectable */
			s.append(el('option', {value: current, selected: ''}, current));
		for (const st of STATUSES) {
			const o = el('option', {value: st}, st);
			if (st === current) o.selected = true;
			s.append(o);
		}
		return s;
	}

	async function setStatus(id, head, from, to) {
		const r = await post('/t/' + id, {head, status: to, 'o.status': from});
		if (r.ok) {
			toast('Status: ' + to);
			await refresh();
		} else {
			toast(r.msg, true);
			await refresh();
		}
	}

	function inlineStatus(root) {
		if (readOnly) return;
		$$('table.tickets tr[data-id]', root).forEach((tr) => {
			const td = $('td.st', tr);
			if (!td || $('select', td)) return;
			const s = statusSelect(tr.dataset.status);
			s.addEventListener('change', () => setStatus(tr.dataset.id, tr.dataset.head, tr.dataset.status, s.value));
			td.textContent = '';
			td.append(s);
		});
		/* the ticket page's status form: apply on change */
		$$('form select[name=status]', root).forEach((s) => {
			const f = s.form;
			const o = f && f.querySelector('input[name="o.status"]'), head = f && f.querySelector('input[name=head]');
			if (!o || !head || s.dataset.inline) return;
			s.dataset.inline = '1';
			const btn = f.querySelector('button');
			if (btn) btn.hidden = true;
			s.addEventListener('change', (e) => {
				e.preventDefault();
				const id = (f.getAttribute('action').match(/^\/t\/([0-9a-f]+)$/) || [])[1];
				if (id) setStatus(id, head.value, o.value, s.value);
			});
		});
	}

	/* ---- Gantt: drag to move, drag the end to resize, drag the handle to link ---------- */

	const DAY = 86400000;
	const parseDay = (s) => (s ? Math.round(Date.parse(s + 'T00:00:00Z') / DAY) : null);
	const isoDay = (d) => new Date(d * DAY).toISOString().slice(0, 10);
	const weekday = (d) => (new Date(d * DAY).getUTCDay() + 6) % 7; /* 0 = Monday */

	/* Mon-Fri days in (from, to] (negative when to < from): what the chart shades as working.
	 * The server's calendar decides the real schedule; the chart is re-drawn from it. */
	function workdays(from, to) {
		let n = 0;
		const step = to >= from ? 1 : -1;
		for (let d = from + step; step > 0 ? d <= to : d >= to; d += step)
			if (weekday(d) < 5) n += step;
		return n;
	}

	function gantt(svg) {
		if (!svg.hasAttribute('data-edit') || svg.dataset.enhanced) return;
		svg.dataset.enhanced = '1';
		const g = {
			step: +svg.dataset.step, colw: +svg.dataset.colw, x0: +svg.dataset.x0,
		};
		const NS = 'http://www.w3.org/2000/svg';

		function svgX(evt) {
			const pt = svg.createSVGPoint();
			pt.x = evt.clientX;
			pt.y = evt.clientY;
			return pt.matrixTransform(svg.getScreenCTM().inverse());
		}

		/* The fields a shift of `days` (move) or of the finish (resize) posts. */
		function fieldsFor(a, kind, days) {
			const d = a.dataset, manual = d.mode === 'manual';
			const s = parseDay(d.s), f = parseDay(d.f);
			if (kind === 'move') {
				const out = {head: d.head, start: isoDay(s + days), 'o.start': d.start};
				if (manual) Object.assign(out, {finish: isoDay(f + days), 'o.finish': d.finish});
				return out;
			}
			if (manual) return {head: d.head, finish: isoDay(Math.max(s, f + days)), 'o.finish': d.finish};
			const delta = g.step === 1 ? workdays(f, f + days) : Math.round(days / 7) * 5;
			const dur = Math.max(1, (+d.days || 1) + delta);
			return {head: d.head, duration: String(dur), 'o.duration': d.dur};
		}

		async function commit(a, kind, days) {
			if (!days) return;
			const r = await post('/t/' + a.dataset.id, fieldsFor(a, kind, days));
			if (!r.ok) toast(r.msg, true);
			await refresh();
		}

		function shape(a) {
			return a.querySelector('rect.bar, path.ms');
		}

		let drag = null;

		$$('a.task', svg).forEach((a) => {
			const kind = a.dataset.kind;
			const bar = shape(a);
			if (!bar || (kind !== 'task' && kind !== 'ms')) return;
			a.setAttribute('aria-roledescription', 'draggable task bar');
			let handle = null;
			if (kind === 'task') { /* the link handle at the bar's end */
				const bb = bar.getBBox();
				handle = document.createElementNS(NS, 'circle');
				handle.setAttribute('class', 'link-h');
				handle.setAttribute('cx', bb.x + bb.width + 5);
				handle.setAttribute('cy', bb.y + bb.height / 2);
				handle.setAttribute('r', 4);
				handle.append(document.createElementNS(NS, 'title'));
				handle.firstChild.textContent = 'drag to another task: it then waits for this one';
				a.append(handle);
			}

			a.addEventListener('pointerdown', (e) => {
				if (e.button !== 0) return;
				const p = svgX(e);
				const bb = bar.getBBox();
				let mode = 'move';
				if (e.target === handle) mode = 'link';
				else if (kind === 'task' && p.x > bb.x + bb.width - 6) mode = 'resize';
				else if (p.x < bb.x - 2 || p.x > bb.x + bb.width + 2) return; /* the label: a plain link */
				e.preventDefault();
				a.setPointerCapture(e.pointerId);
				drag = {a, mode, x: p.x, moved: false, bar, w: bb.width, line: null};
				svg.classList.add('dragging');
			});
			a.addEventListener('pointermove', (e) => {
				if (!drag || drag.a !== a) return;
				const p = svgX(e), dx = p.x - drag.x;
				if (Math.abs(dx) > 3) drag.moved = true;
				const cols = Math.round(dx / g.colw);
				if (drag.mode === 'move') {
					a.querySelectorAll('rect, path.ms, circle').forEach((n) => n.setAttribute('transform', `translate(${cols * g.colw},0)`));
				} else if (drag.mode === 'resize') {
					drag.bar.setAttribute('width', Math.max(3, drag.w + cols * g.colw));
				} else if (drag.mode === 'link') {
					if (!drag.line) {
						drag.line = document.createElementNS(NS, 'line');
						drag.line.setAttribute('class', 'link-tmp');
						drag.line.setAttribute('x1', handle.getAttribute('cx'));
						drag.line.setAttribute('y1', handle.getAttribute('cy'));
						svg.append(drag.line);
					}
					drag.line.setAttribute('x2', p.x);
					drag.line.setAttribute('y2', p.y);
				}
				drag.cols = cols;
			});
			a.addEventListener('pointerup', async (e) => {
				if (!drag || drag.a !== a) return;
				const d = drag;
				drag = null;
				svg.classList.remove('dragging');
				if (d.line) d.line.remove();
				if (!d.moved) return; /* a click: follow the link */
				a.dataset.dragged = '1';
				if (d.mode === 'link') {
					const under = document.elementFromPoint(e.clientX, e.clientY);
					const target = under && under.closest('a.task');
					if (!target || target === a) return;
					const r = await post('/t/' + target.dataset.id + '/dep', {op: 'add', pred: a.dataset.id, kind: 'FS'});
					if (!r.ok) toast(r.msg, true);
					else toast('Linked: it now waits for ' + a.dataset.id.slice(0, 7));
					await refresh();
					return;
				}
				await commit(a, d.mode, (d.cols || 0) * g.step);
			});
			a.addEventListener('click', (e) => {
				if (a.dataset.dragged) {
					e.preventDefault();
					delete a.dataset.dragged;
				}
			});

			/* keyboard: arrows move by a column, Shift+arrows resize; saved after a pause */
			let pending = 0, pendingKind = 'move', timer = 0;
			a.addEventListener('keydown', (e) => {
				if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight') {
					if (e.key === 'Escape' && pending) {
						pending = 0;
						clearTimeout(timer);
						a.querySelectorAll('rect, path.ms, circle').forEach((n) => n.removeAttribute('transform'));
						bar.setAttribute('width', bar.dataset.w0);
					}
					return;
				}
				e.preventDefault();
				const kindNow = e.shiftKey && kind === 'task' ? 'resize' : 'move';
				if (pending && kindNow !== pendingKind) return;
				pendingKind = kindNow;
				pending += e.key === 'ArrowRight' ? 1 : -1;
				if (kindNow === 'move')
					a.querySelectorAll('rect, path.ms, circle').forEach((n) => n.setAttribute('transform', `translate(${pending * g.colw},0)`));
				else
					bar.setAttribute('width', Math.max(3, +bar.dataset.w0 + pending * g.colw));
				clearTimeout(timer);
				timer = setTimeout(() => {
					const days = pending * g.step;
					pending = 0;
					commit(a, pendingKind, days);
				}, 700);
			});
			bar.dataset.w0 = bar.getAttribute('width') || '0';
		});
	}

	/* ---- keyboard shortcuts ----------------------------------------------------------- */

	const KEYS = [
		['/', 'find a ticket'], ['g t', 'tickets'], ['g w', 'WBS'], ['g p', 'plan'], ['g l', 'load'],
		['g r', 'resources'], ['g c', 'calendars'], ['n', 'new ticket'], ['e', 'edit this ticket'],
		['j / k', 'next / previous ticket in a list'], ['Enter', 'open the selected ticket'],
		['← →', 'move a focused Gantt bar (Shift: change its length)'], ['?', 'this help'],
	];
	const GO = {t: '/tickets', w: '/wbs', p: '/plan', l: '/load', r: '/res', c: '/cal'};

	function help() {
		let d = $('#tix-help');
		if (!d) {
			d = el('dialog', {id: 'tix-help', class: 'help', 'aria-label': 'Keyboard shortcuts'});
			d.append(el('h2', {}, 'Keyboard shortcuts'));
			const t = el('table');
			for (const [k, what] of KEYS) {
				const tr = el('tr');
				tr.append(el('th', {}, k), el('td', {}, what));
				t.append(tr);
			}
			const close = el('button', {type: 'button'}, 'Close');
			close.addEventListener('click', () => d.close());
			d.append(t, close);
			d.addEventListener('close', () => {
				if (d.returnTo && document.contains(d.returnTo)) d.returnTo.focus();
				else document.activeElement.blur();
			});
			body.append(d);
		}
		if (d.open) {
			d.close();
		} else {
			d.returnTo = document.activeElement !== body ? document.activeElement : null;
			d.showModal();
		}
	}

	function rows() {
		return $$('table.tickets tr[data-id]');
	}

	function select(i) {
		const r = rows();
		if (!r.length) return;
		const cur = r.findIndex((tr) => tr.classList.contains('sel'));
		r.forEach((tr) => tr.classList.remove('sel'));
		const n = Math.max(0, Math.min(r.length - 1, (cur < 0 ? (i > 0 ? -1 : r.length) : cur) + i));
		r[n].classList.add('sel');
		r[n].scrollIntoView({block: 'nearest'});
	}

	let prefix = '';
	document.addEventListener('keydown', (e) => {
		if (e.ctrlKey || e.metaKey || e.altKey || e.defaultPrevented) return;
		const t = e.target;
		/* a closed dialog can keep focus for a moment after closing: only an open one counts */
		if (t.closest && t.closest('input, textarea, select, [contenteditable], dialog[open]')) return;
		const k = e.key;
		if (prefix === 'g') {
			prefix = '';
			if (GO[k]) { e.preventDefault(); location.href = GO[k]; }
			return;
		}
		if (k === 'g') { prefix = 'g'; setTimeout(() => { prefix = ''; }, 1500); return; }
		if (k === '/') {
			const s = $('form.search input[name=q]');
			if (s) { e.preventDefault(); s.focus(); s.select(); }
		} else if (k === '?') {
			e.preventDefault();
			help();
		} else if (k === 'n' && !readOnly) {
			location.href = '/new';
		} else if (k === 'e' && !readOnly) {
			const a = $('main a[href$="/edit"]');
			if (a) location.href = a.getAttribute('href');
		} else if (k === 'j' || k === 'k') {
			if (rows().length) { e.preventDefault(); select(k === 'j' ? 1 : -1); }
		} else if (k === 'Enter' || k === 'o') {
			const s = $('table.tickets tr.sel[data-id]');
			if (s) { e.preventDefault(); location.href = '/t/' + s.dataset.id; }
		}
	});

	/* ---- wiring ---------------------------------------------------------------------- */

	function enhance(root) {
		pickers(root);
		inlineStatus(root);
		notifyButton(root);
		$$('svg.gantt', root).forEach(gantt);
	}

	body.classList.add('js');
	enhance(document);
	live();
})();
