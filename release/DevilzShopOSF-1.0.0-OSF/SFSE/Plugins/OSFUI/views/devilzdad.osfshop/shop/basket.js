/* Basket: rendering, line edits and transfer. Loaded before app.js. */

function renderBasket() {
  el.basketCount.textContent = state.basket.length;
  if (!state.basket.length) {
    el.basketItems.innerHTML = '<p class="empty" style="color:#c79aa1">Basket is empty.</p>';
    renderWallet();
    return;
  }
  let html = '';
  state.basket.forEach((line, index) => {
    const price = state.free ? '' : ' &middot; ' + (line.unitPrice * line.count) + 'c';
    const lineWeight = (line.weight || 0) * line.count;
    const mass = lineWeight > 0 ? ' &middot; ' + lineWeight.toFixed(2) + ' wt' : '';
    html += '<div class="bline">' +
      '<span class="blabel" title="' + esc(nameOf(line)) + '">' + esc(nameOf(line)) + (line.omods ? ' <span title="Configured parts">*</span>' : '') + price + mass + '</span>' +
      '<button data-act="dec" data-i="' + index + '" title="One less">&#8722;</button>' +
      '<span class="bqty">' + line.count + '</span>' +
      '<button data-act="inc" data-i="' + index + '" title="One more">+</button>' +
      '<button class="rm" data-act="rm" data-i="' + index + '" title="Remove">&#10005;</button></div>';
  });
  el.basketItems.innerHTML = html;
  renderWallet();
}

async function basketOp(op, extra) {
  try {
    applyBasket(await call(EP.basket, Object.assign({ op: op, free: state.free }, extra || {})));
  } catch (err) {
    say('Basket update failed: ' + errText(err), 'err');
  }
}

function applyBasket(reply) {
  if (!reply) return;
  if (Array.isArray(reply.basket)) state.basket = reply.basket;
  if (typeof reply.cost === 'number') state.cost = reply.cost;
  if (typeof reply.credits === 'number') state.credits = reply.credits;
  if (reply.error) say(reply.error, 'err');
  renderBasket();
}

async function acceptBasket() {
  if (!state.basket.length) { say('Basket is empty.', 'err'); return; }
  el.acceptBtn.disabled = true;
  try {
    const reply = await call(EP.accept, { free: state.free });
    state.basket = (reply && reply.basket) || [];
    if (typeof reply.credits === 'number') state.credits = reply.credits;
    if (typeof reply.cost === 'number') state.cost = reply.cost;
    renderBasket();
    const lines = (reply && reply.lines) || [];
    say(lines.length ? lines.slice(0, 3).join('  |  ') : ((reply && reply.ok) ? 'Transfer complete.' : 'Transfer failed.'),
      (reply && reply.ok) ? 'ok' : 'err');
  } catch (err) {
    say('Accept failed: ' + errText(err), 'err');
  } finally {
    el.acceptBtn.disabled = false;
  }
}

