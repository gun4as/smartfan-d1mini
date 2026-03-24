var lockUntil = {};
var lastAutoMode = null;

// ── Fanu slīdņu apstrāde ──────────────────────────────────
function fanSlide(id) {
  var s = document.getElementById('s' + id);
  var p = document.getElementById('p' + id);
  lockUntil[id] = Date.now() + 3000;
  p.textContent = s.value + '%';
}

function fanSet(id) {
  var s = document.getElementById('s' + id);
  lockUntil[id] = Date.now() + 3000;
  fetch('/api/set?id=' + id + '&speed=' + s.value);
}

// ── Releja pārslēgšana ─────────────────────────────────────
function toggleRelay(id) {
  var el = document.getElementById('rl' + id);
  var isOn = el.textContent === 'ON';
  fetch('/api/relay?id=' + id + '&state=' + (isOn ? '0' : '1')).then(function() {
    update();
  });
}

// ── Dinamiski veidot fanu blokus ───────────────────────────
var modeNames = ['', 'Grafiks', 'Temperatura', 'Serveris'];

function buildFans(fans, autoMode) {
  var box = document.getElementById('fans');
  if (!box) return;
  var html = '';
  var names = ['Fan 1', 'Fan 2'];
  for (var i = 0; i < fans.length; i++) {
    if (!fans[i].active) continue;
    var am = (autoMode && autoMode[i]) || 0;
    html += '<div class="fan-block">';
    html += '<div class="fan-header">';
    html += '<h3>' + names[i] + '</h3>';
    if (am === 0) {
      html += '<button class="btn-relay" id="rb' + i + '" onclick="toggleRelay(' + i + ')">';
      html += '<span id="rl' + i + '">---</span></button>';
    } else {
      html += '<span class="fan-pct" style="font-size:12px;' + (am === 3 ? 'color:#e8a735' : '') + '">' + modeNames[am] + '</span>';
    }
    html += '</div>';
    html += '<div class="fan-row">';
    html += '<span class="rpm" id="r' + i + '">---</span><small>RPM</small>';
    html += '<span class="fan-pct" id="p' + i + '">50%</span>';
    html += '</div>';
    if (am === 0) {
      html += '<input type="range" id="s' + i + '" min="0" max="100" value="50"';
      html += ' oninput="fanSlide(' + i + ')" onchange="fanSet(' + i + ')">';
    } else if (am === 3) {
      html += '<div style="font-size:11px;color:#e8a735;padding:4px 0;text-align:center">Vada serveris — lai mainitu, nopauzejiet noteikumu</div>';
    }
    html += '</div>';
  }
  box.innerHTML = html;
}

// ── Dinamiski veidot sensoru blokus ────────────────────────
function buildSensors(d) {
  var box = document.getElementById('sensors');
  if (!box) return;
  var html = '';

  if (d.ds18b20 && d.ds18b20.length > 0) {
    html += '<div class="stat"><label>DS18B20</label>';
    for (var i = 0; i < d.ds18b20.length; i++) {
      var s = d.ds18b20[i];
      html += '<div class="sensor-row"><span class="lbl">' + s.name + '</span>';
      html += '<span class="val" id="ds-t' + i + '">' + s.temp + ' &deg;C</span></div>';
    }
    html += '</div>';
  }
  if (d.dht22) {
    var dn = d.dht22.name || 'DHT22';
    html += '<div class="stat"><label class="sensor-name" onclick="editSensorName(\'dht22\',this)">' + dn + '</label>';
    html += '<div class="sensor-row"><span class="lbl">Temperatura</span>';
    html += '<span class="val" id="dht-t">' + d.dht22.temp + ' &deg;C</span></div>';
    html += '<div class="sensor-row"><span class="lbl">Mitrums</span>';
    html += '<span class="val" id="dht-h">' + d.dht22.hum + '%</span></div></div>';
  }
  box.innerHTML = html;
}

// ── Sensora nosaukuma redigesana ───────────────────────────
function editSensorName(type, el) {
  var cur = el.textContent;
  var inp = document.createElement('input');
  inp.type = 'text';
  inp.value = cur;
  inp.maxLength = 15;
  inp.className = 'sensor-name-input';
  inp.onblur = function() { saveSensorName(type, inp, el); };
  inp.onkeydown = function(e) {
    if (e.key === 'Enter') inp.blur();
    if (e.key === 'Escape') { el.style.display = ''; inp.remove(); }
  };
  el.style.display = 'none';
  el.parentNode.insertBefore(inp, el);
  inp.focus();
  inp.select();
}

function saveSensorName(type, inp, el) {
  var name = inp.value.trim();
  if (name.length === 0) name = type.toUpperCase();
  fetch('/api/sensor-set-name?type=' + type + '&name=' + encodeURIComponent(name));
  el.textContent = name;
  el.style.display = '';
  inp.remove();
  sensorsBuilt = false; // rebuild next update
}

// ── Galvenā update funkcija ────────────────────────────────
var sensorsBuilt = false;

function update() {
  fetch('/api/status')
    .then(function(r) { return r.json(); })
    .then(function(d) {
      // Fani
      if (d.fans) {
        var am = d.autoMode || [];
        var amStr = JSON.stringify(am);
        if (amStr !== JSON.stringify(lastAutoMode)) {
          lastAutoMode = am;
          buildFans(d.fans, am);
        }
        for (var i = 0; i < d.fans.length; i++) {
          if (!d.fans[i].active) continue;
          var re = document.getElementById('r' + i);
          if (re) re.textContent = d.fans[i].rpm;
          if (!lockUntil[i] || Date.now() > lockUntil[i]) {
            var se = document.getElementById('s' + i);
            var pe = document.getElementById('p' + i);
            if (se) se.value = d.fans[i].speed;
            if (pe) pe.textContent = d.fans[i].speed + '%';
          }
          // Releja stāvoklis pie fana (tikai manuālajā režīmā)
          var rl = document.getElementById('rl' + i);
          var rb = document.getElementById('rb' + i);
          if (rl) rl.textContent = d.fans[i].relay ? 'ON' : 'OFF';
          if (rb) rb.className = 'btn-relay' + (d.fans[i].relay ? ' on' : '');
        }
      }

      // Sensori
      if (!sensorsBuilt) {
        buildSensors(d);
        sensorsBuilt = true;
      } else {
        var el;
        if (d.ds18b20) {
          for (var i = 0; i < d.ds18b20.length; i++) {
            el = document.getElementById('ds-t' + i);
            if (el) el.textContent = d.ds18b20[i].temp + ' C';
          }
        }
        if (d.dht22) {
          el = document.getElementById('dht-t'); if(el) el.textContent = d.dht22.temp + ' C';
          el = document.getElementById('dht-h'); if(el) el.textContent = d.dht22.hum + '%';
        }
      }

      // Ierīces nosaukums
      var nameEl = document.getElementById('dev-name');
      if (nameEl && d.name && nameEl.textContent !== d.name && !nameEl.classList.contains('editing')) {
        nameEl.textContent = d.name;
      }

      // Laiks
      document.getElementById('time').textContent = d.time;
      document.getElementById('date').textContent = d.date;
    })
    .catch(function() {});
}

// ── Ierīces nosaukuma redigesana ───────────────────────────
function editDevName() {
  var el = document.getElementById('dev-name');
  var cur = el.textContent;
  var inp = document.createElement('input');
  inp.type = 'text';
  inp.value = cur;
  inp.maxLength = 23;
  inp.className = 'dev-name-input';
  el.classList.add('editing');
  inp.onblur = function() { saveDevName(inp, el); };
  inp.onkeydown = function(e) {
    if (e.key === 'Enter') inp.blur();
    if (e.key === 'Escape') { el.style.display = ''; el.classList.remove('editing'); inp.remove(); }
  };
  el.style.display = 'none';
  el.parentNode.insertBefore(inp, el);
  inp.focus();
  inp.select();
}

function saveDevName(inp, el) {
  var name = inp.value.trim();
  if (name.length === 0) name = 'Smart Fan';
  fetch('/api/device-set-name?name=' + encodeURIComponent(name));
  el.textContent = name;
  el.style.display = '';
  el.classList.remove('editing');
  inp.remove();
}

setInterval(update, 1000);
update();
