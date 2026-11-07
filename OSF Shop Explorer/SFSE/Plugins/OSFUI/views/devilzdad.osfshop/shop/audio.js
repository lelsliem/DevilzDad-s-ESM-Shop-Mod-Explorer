/* Audio panel: volume, mute and track controls. Loaded before app.js. */

/* ---------------------------------------------------------------- audio -- */

const audioEl = {
  music: document.getElementById('musicVol'),
  musicOut: document.getElementById('musicVolOut'),
  musicMute: document.getElementById('musicMute'),
  sfx: document.getElementById('sfxVol'),
  sfxOut: document.getElementById('sfxVolOut'),
  sfxMute: document.getElementById('sfxMute'),
  track: document.getElementById('trackSel'),
  prev: document.getElementById('trackPrev'),
  next: document.getElementById('trackNext'),
};

let audioPersistTimer = 0;

function renderAudio() {
  audioEl.music.value = audio.musicVolume;
  audioEl.musicOut.textContent = audio.musicVolume + '%';
  audioEl.musicMute.checked = audio.musicMuted;
  audioEl.sfx.value = audio.sfxVolume;
  audioEl.sfxOut.textContent = audio.sfxVolume + '%';
  audioEl.sfxMute.checked = audio.sfxMuted;
  let html = '<option value="">(theme track)</option>';
  audio.tracks.forEach((track) => {
    html += '<option value="' + esc(track) + '"' + (audio.track === track ? ' selected' : '') + '>' +
      esc(track) + '</option>';
  });
  audioEl.track.innerHTML = html;
  audioEl.track.value = audio.track || '';
  // A soundtrack folder is the only selection with a next track. A single
  // file loops, so the buttons stay disabled rather than restarting it.
  const canStep = !!audio.playlist;
  audioEl.prev.disabled = !canStep;
  audioEl.next.disabled = !canStep;
  audioEl.prev.title = canStep
    ? 'Previous track in the soundtrack'
    : 'Pick a soundtrack folder to step through its tracks';
  audioEl.next.title = canStep
    ? 'Next track in the soundtrack'
    : 'Pick a soundtrack folder to step through its tracks';
}

/**
 * The plugin owns the queue, so the page only says which way to go. The reply
 * is the whole audio state, which also carries the playlist flag: a folder that
 * failed to yield a single decodable file reports false and the buttons lock
 * again.
 */
function stepTrack(delta) {
  call(EP.audio, { op: 'skip', delta: delta }).then((reply) => {
    if (reply && reply.ok) {
      audio.playlist = !!reply.playlist;
      renderAudio();
    } else if (reply && reply.ok === false) {
      say('Track skip refused.', 'err');
    }
  }).catch((err) => say('Track skip failed: ' + errText(err), 'err'));
}

function pushAudio(persist) {
  clearTimeout(audioPersistTimer);
  // A slider drag is not 200 settings writes: apply immediately, persist when
  // the drag settles, which is what the overlay's popup does on release.
  if (!persist) audioPersistTimer = setTimeout(() => pushAudio(true), 500);
  call(EP.audio, {
    op: 'set',
    musicVolume: audio.musicVolume | 0,
    sfxVolume: audio.sfxVolume | 0,
    musicMuted: audio.musicMuted,
    sfxMuted: audio.sfxMuted,
    track: audio.track,
    persist: !!persist,
  }).then((reply) => {
    if (reply && reply.ok === false) say('Audio change refused.', 'err');
  }).catch((err) => say('Audio change failed: ' + errText(err), 'err'));
}

async function loadAudio() {
  try {
    const reply = await call(EP.audio, {});
    if (!reply || !reply.ok) return;
    audio.musicVolume = Number(reply.musicVolume) || 0;
    audio.sfxVolume = Number(reply.sfxVolume) || 0;
    audio.musicMuted = !!reply.musicMuted;
    audio.sfxMuted = !!reply.sfxMuted;
    audio.track = reply.track || '';
    audio.tracks = reply.tracks || [];
    audio.playlist = !!reply.playlist;
    renderAudio();
  } catch (err) {
    say('Audio settings unavailable: ' + errText(err), 'err');
  }
}

audioEl.music.addEventListener('input', () => {
  audio.musicVolume = Number(audioEl.music.value);
  audioEl.musicOut.textContent = audio.musicVolume + '%';
  pushAudio(false);
});
audioEl.sfx.addEventListener('input', () => {
  audio.sfxVolume = Number(audioEl.sfx.value);
  audioEl.sfxOut.textContent = audio.sfxVolume + '%';
  pushAudio(false);
});
audioEl.musicMute.addEventListener('change', () => {
  audio.musicMuted = audioEl.musicMute.checked;
  pushAudio(true);
});
audioEl.sfxMute.addEventListener('change', () => {
  audio.sfxMuted = audioEl.sfxMute.checked;
  pushAudio(true);
});
audioEl.track.addEventListener('change', () => {
  audio.track = audioEl.track.value;
  pushAudio(true);
});
audioEl.prev.addEventListener('click', () => stepTrack(-1));
audioEl.next.addEventListener('click', () => stepTrack(1));

