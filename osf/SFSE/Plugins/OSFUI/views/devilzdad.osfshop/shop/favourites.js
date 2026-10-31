/* Favourites: star toggling and the starred list. Loaded before app.js. */

function isFavourite(row) {
  // The list, not the row flag: a star click updates this array and the row on
  // screen without a round trip, and either front end may change the list.
  if (Array.isArray(state.favourites) && state.favourites.length) {
    return state.favourites.indexOf(row.formId) >= 0;
  }
  return !!row.favourite;
}

function renderFavourites() {
  if (el.favCount) el.favCount.textContent = String(state.favourites.length || 0);
  if (el.favouritesOnly) el.favouritesOnly.checked = !!state.favouritesOnly;
}

/* ------------------------------------------------------------ favourites -- */

async function toggleFavourite(formId) {
  try {
    const reply = await call(EP.favourite, { formId: formId, free: state.free });
    if (!reply || reply.ok === false) {
      say('Could not star that record: ' + ((reply && reply.error) || 'unknown error'), 'err');
      return;
    }
    if (Array.isArray(reply.favourites)) state.favourites = reply.favourites;
    renderFavourites();
    // Said before the starred-view reload returns: that path used to skip the
    // footnote, which then kept the previous click's wording.
    say(reply.favourite ? 'Starred ' + formId + '.' : 'Unstarred ' + formId + '.');
    if (state.favouritesOnly) {
      // Unstarred rows no longer belong to the list; let the plugin rebuild it
      // so the page count and the pager stay true.
      loadRows(true);
      return;
    }
    // Redraw just the buttons: a full row reload would scroll the list back.
    el.rows.querySelectorAll('button[data-fav]').forEach((button) => {
      const starred = state.favourites.indexOf(button.dataset.fav) >= 0;
      button.classList.toggle('on', starred);
      button.title = starred ? 'Remove from favourites' : 'Add to favourites';
    });
  } catch (err) {
    say('Could not star that record: ' + errText(err), 'err');
  }
}


